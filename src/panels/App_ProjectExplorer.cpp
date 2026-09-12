// Project-explorer translation unit: the expandable source list ("media bin")
// and sequence/shot tree, plus the source selection / add / remove logic that
// backs it. App members, split out of App.cpp purely to keep that file
// manageable. The left icon strip that toggles this panel lives in
// App_NavPanel.cpp.

#include "App.h"
#include "AppInternal.h"
#include "Layout.h"
#include "RevealFile.h"

#include <SDL3/SDL_dialog.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <system_error>
#include <unordered_map>
#include <unordered_set>

namespace fs = std::filesystem;
using namespace jplay;

namespace {
// Keep the audio extension set in sync with isAudioPath() in AppInternal.h.
// Keep the image extension set in sync with ImageSeq::isSequenceExt.
const SDL_DialogFileFilter kMediaFilters[] = {
    { "Media (video / images / audio)",
      "mov;mp4;m4v;mkv;avi;mxf;webm;mpg;mpeg;m2v;m2ts;mts;ts;wmv;flv;y4m;ogv;3gp;"
      "exr;png;jpg;jpeg;tif;tiff;"
      "wav;aif;aiff;flac;mp3;m4a;aac;ogg;opus;wma" },
    { "Images", "exr;png;jpg;jpeg;tif;tiff" },
    { "Audio", "wav;aif;aiff;flac;mp3;m4a;aac;ogg;opus;wma" },
    { "All files", "*" },
};

// ---- project-explorer palette
// Static colors used by the side strip, section buttons and source/sequence
// list. The row backgrounds (sequence / shot / source rows) are computed per
// active/hover/idle state and stay inline at their draw sites.

// Icon strip + explorer panel share the window-chrome fill (jplay::kPanelBg).

constexpr SDL_Color kActive { 150, 190, 255, 255 };     // active toggle / focused-seq dot
constexpr SDL_Color kTitle  { 160, 170, 200, 255 };     // panel title ("PROJECT") — matches other panels
constexpr SDL_Color kWarn   { 235, 180, 90, 255 };      // tech-mode live tint / cut-differ flag

constexpr SDL_Color kHeader   { 150, 154, 164, 255 };   // SEQUENCES / SOURCES headers
constexpr SDL_Color kBtnBg    { 44, 46, 52, 255 };      // +/- section button fill
constexpr SDL_Color kBtnBorder{ 90, 94, 104, 255 };     // +/- section button border
constexpr SDL_Color kBtnBgHov { 64, 68, 80, 255 };      // ditto, hovered
constexpr SDL_Color kBtnBrdHov{ 145, 152, 168, 255 };   // ditto, hovered
constexpr SDL_Color kBtnPlus  { 150, 200, 150, 255 };   // add (green)
constexpr SDL_Color kBtnMinus { 200, 150, 150, 255 };   // remove (red)

constexpr SDL_Color kCaret    { 180, 184, 194, 255 };   // sequence collapse caret
constexpr SDL_Color kSeqRange { 140, 150, 170, 255 };   // sequence frame-range text
constexpr SDL_Color kRowText  { 234, 238, 246, 255 };   // sequence name / source in timeline
constexpr SDL_Color kShotName { 214, 218, 226, 255 };   // shot name
constexpr SDL_Color kShotCell { 150, 170, 200, 255 };   // shot S/E/I/O cells
constexpr SDL_Color kDragLine { 120, 170, 255, 255 };   // reorder insertion line
constexpr SDL_Color kSourceDim{ 158, 162, 172, 255 };   // source not used in timeline
constexpr SDL_Color kPlaying  { 240, 244, 252, 255 };   // bin section holding the playhead source
// Marker at the right of a row / top-right of a grid cell. Fill and border are
// left to say what the mouse did (selection, hover); the marker is the only thing
// that speaks about the timeline, so the two never fight over a cell. Shape
// carries the state as much as colour: a hollow cream square for a clip selected
// on the timeline, a solid red down-triangle - a playhead in miniature - for the
// source the player is showing. Cream matches the dotted border the timeline
// draws around a selected clip (kSelectCream there), 20% darker so it sits back
// against the bin's darker rows; red is the playhead's own colour (kPlayhead).
constexpr SDL_Color kTlSelDot { 196, 190, 171, 255 };  // backs a clip selected on the timeline
constexpr SDL_Color kPlayMark { 235,  70,  70, 255 };  // source the player is showing
constexpr float     kMarkSize = 7.0f;                  // marker box side, px
constexpr SDL_Color kAudioIcon{ 150, 170, 200, 255 };   // speaker glyph for audio sources (no thumbnail)

inline void setColor(SDL_Renderer* r, SDL_Color c) {
    SDL_SetRenderDrawColor(r, c.r, c.g, c.b, c.a);
}

// Size slider range (device px): from small 16px tiles up to 128px. At/below
// kThumbRowMax the grid collapses to a vertical list (thumbnail + name per row).
constexpr float kThumbMin = 16.0f;
constexpr float kThumbMax = 128.0f;
constexpr float kThumbRowMax = 32.0f;
} // namespace

// Small +/- button used for the SEQUENCES and SOURCES section headers.
// `hover` lifts the fill and border so the button reads as clickable under the
// cursor. The cross strokes are filled rects rather than 1px lines: at a high
// dpiScale a hairline in a large button looks broken.
void App::drawPeButton(const SDL_FRect& b, bool plus, bool hover) {
    setColor(renderer_, hover ? kBtnBgHov : kBtnBg);
    jplay::fillRect(renderer_, &b);
    setColor(renderer_, hover ? kBtnBrdHov : kBtnBorder);
    jplay::drawRect(renderer_, &b);
    const float p = b.h * 0.25f;                         // stroke inset, proportional to the button
    const float t = std::max(1.0f, std::round(dpiScale)); // stroke thickness
    setColor(renderer_, plus ? kBtnPlus : kBtnMinus);
    if (plus) {
        SDL_FRect rect = { std::round(b.x + (b.w - t) * 0.5f), b.y + p, t, b.h - 2.0f * p };
        jplay::fillRect(renderer_, &rect);
    }
    SDL_FRect h = { b.x + p, std::round(b.y + (b.h - t) * 0.5f), b.w - 2.0f * p, t };
    jplay::fillRect(renderer_, &h);
}

void App::renderProjectExplorer() {
    explorerRows_.clear();
    peRows_.clear();
    peBinHeaders_.clear();
    peSourceBand_ = {};
    // Refilled below by whichever tab draws a scrollbar; the other tab's stays
    // empty, so a press can be offered to both without testing the tab.
    peSourceSb_.bar = {};
    peSeqSb_.bar = {};
    if (!panelOpen(kPanelProjectExplorer)) {
        peSortMenu_.close();  // the button it anchors to is gone
        peMediaMenu_.close(); // ditto the row it was opened on
        peSeqColorMenu_.close();
        return;
    }

    SDL_FRect panel = beginLeftPanel();
    const float ex = panel.x;   // pane left edge
    const float topH = panel.y; // pane top edge

    float mx = 0.0f, my = 0.0f;
    uiHoverMouse(mx, my);
    const float pad = 8.0f;
    const float lh = textFont_.lineHeight();
    const float rh = 22.0f * dpiScale;           // header / sequence / source row
    const float shotH = 3.0f * lh + 12.0f;       // name + two value rows
    const float bottom = panelsBottom_ - 4.0f;
    const float bs = std::round(lh);              // section button size (= caption text height)
    const float bgap = 6.0f * dpiScale;           // gap between the +/- pair

    // The padded column the panel chrome (title, tabs, section headers) lays out
    // in. The scrolling lists below span the full panel width instead, so they
    // build their own rects off `panel` rather than cutting from this one.
    SDL_FRect body = inset(panel, pad, 0.0f);
    gapTop(body, pad);

    SDL_FRect title = cutTop(body, lh);
    drawText(title.x, title.y, kTitle, "PROJECT EXPLORER");
    gapTop(body, 8.0f);

    // ---- SOURCES / SEQUENCES tab bar ----------------------------------------
    {
        const char* labels[2] = { "SOURCES", "SEQUENCES" };
        SDL_FRect* rects[2] = { &peTabSources_, &peTabSequences_ };
        SDL_FRect tabs = cutTop(body, rh);
        gapTop(body, 8.0f);
        const float tabW = tabs.w * 0.5f;
        for (int i = 0; i < 2; ++i) {
            SDL_FRect tr = cutLeft(tabs, tabW);
            gapRight(tr, 2.0f); // separation between the two tabs
            *rects[i] = tr;
            const bool act = (peActiveTab_ == i);
            const bool hov = inRect(tr, mx, my);
            // Standard toggle-button art (see the timeline's Snap button): the
            // unselected tab sits at the dark idle fill, the selected one at the
            // lit "on" fill, and both lift on hover.
            drawButton(renderer_, &textFont_, tr, labels[i],
                       act ? (hov ? colors().uibtnOnHover : colors().uibtnOnBg)
                           : (hov ? kUiBtnBgHover : kUiBtnBg),
                       act ? colors().uibtnOnBorder
                           : (hov ? kUiBtnBorderHover : kUiBtnBorder),
                       act ? colors().uibtnOnText
                           : (hov ? SDL_Color{ 235, 238, 245, 255 } : kHeader));
        }
    }

    SDL_Rect clip = { (int)ex, (int)topH, (int)openPanelW(), (int)(panelsBottom_ - topH) };
    SDL_SetRenderClipRect(renderer_, &clip);

    auto regs = timeline_.seqRegions();
    const int focusedSeq = filteredSeqIdx(); // -1 in the All view

    // Zero the section buttons; only the active tab's buttons get real rects
    // (below), so clicks never land on the hidden tab's stale positions.
    peSeqAddRect_ = peSeqRemoveRect_ = SDL_FRect{};
    peAddRect_ = peRemoveRect_ = peSortBtnRect_ = peFilterClearRect_ = SDL_FRect{};
    peFilterTlVisibleRect_ = SDL_FRect{};
    peGoToCurrentRect_ = SDL_FRect{};

    // Row buttons under the cursor, if any: the hover labels at the end of the
    // frame need their rects, and the rows themselves are laid out inside a
    // clipped scrolling band.
    SDL_FRect hoveredEye{}, hoveredColor{};

  if (peActiveTab_ == PeTabSequences) {
    // ---- SEQUENCES header + add/remove (fixed; the tree below scrolls) -------
    {
        SDL_FRect hdr = cutTop(body, rh);
        SDL_FRect removeSlot = cutRight(hdr, bs);
        gapRight(hdr, bgap);
        SDL_FRect addSlot = cutRight(hdr, bs);
        peSeqRemoveRect_ = centerV(removeSlot, bs);
        peSeqAddRect_    = centerV(addSlot, bs);
        const SDL_FRect cap = centerV(hdr, lh);
        drawText(cap.x, cap.y, kHeader, "SEQUENCES");
        drawPeButton(peSeqAddRect_, true, inRect(peSeqAddRect_, mx, my));
        drawPeButton(peSeqRemoveRect_, false, inRect(peSeqRemoveRect_, mx, my));
    }

    // Scrollable band below the fixed header. Content taller than the band
    // scrolls with peSeqScroll_ (mouse wheel); rows are drawn offset by it and
    // clipped to the band, with off-band rows skipping their (expensive) draws
    // but still registering into peRows_ so drag/reorder math stays scroll-aware.
    const float listTop = body.y;
    const float listBottom = bottom;
    const float viewH = std::max(listBottom - listTop, 0.0f);
    const SDL_FRect band = { ex, listTop, openPanelW(), listBottom - listTop };
    peSeqListTop_ = listTop;
    peSeqListBottom_ = listBottom;

    float contentH = 0.0f;
    for (const Sequence& s : timeline_.sequences) {
        if (s.temporary) continue; // the source view is not part of the project
        contentH += rh;
        if (peCollapsedSeqs_.count(s.id)) continue;
        for (int sid : s.shotIds)
            if (timeline_.findShotById(sid)) contentH += shotH;
    }
    const float maxScroll = std::max(0.0f, contentH - viewH);
    peSeqScroll_ = std::clamp(peSeqScroll_, 0.0f, maxScroll);

    SDL_Rect bandClip = { (int)ex, (int)listTop, (int)openPanelW(), (int)viewH };
    SDL_SetRenderClipRect(renderer_, &bandClip);
    // Rows are cut off a rect that is unbounded (see Layout.h) and offset by the
    // scroll: the band clip above bounds what is visible, not what is laid out.
    SDL_FRect content = inset(SDL_FRect{ ex, listTop - peSeqScroll_, openPanelW(), kUnbounded },
                              2.0f, 0.0f);

    for (int si = 0; si < (int)timeline_.sequences.size(); ++si) {
        const Sequence& seq = timeline_.sequences[si];
        if (seq.temporary) continue; // matches the contentH pass above
        const bool collapsed = peCollapsedSeqs_.count(seq.id) > 0;
        const bool active = (si == focusedSeq);
        SDL_FRect row = cutTop(content, rh);
        const bool onBand = visibleIn(row, band);
        bool hover = onBand && inRect(row, mx, my);
        if (onBand) {
            SDL_SetRenderDrawColor(renderer_, active ? 54 : (hover ? 42 : 34),
                                   active ? 64 : (hover ? 44 : 36),
                                   active ? 86 : (hover ? 52 : 42), 255);
            jplay::fillRect(renderer_, &row);
        }

        PeRow pr;
        pr.kind = PeRow::Kind::Seq;
        pr.id = seq.id;
        pr.rect = row;

        // Row internals, cut left-to-right and right-to-left off what is left; the
        // name takes the middle.
        SDL_FRect inner = row;
        gapLeft(inner, 5.0f);
        SDL_FRect caretSlot = cutLeft(inner, 10.0f);
        gapLeft(inner, 6.0f);
        SDL_FRect caret = centerV(caretSlot, 10.0f);
        pr.caret = caret;
        // ICON_MDI_CHEVRON_RIGHT (U+F0142) collapsed / ICON_MDI_CHEVRON_DOWN (U+F0140) open.
        if (onBand)
            icons_.drawGlyph(renderer_, collapsed ? 0xF0142 : 0xF0140, caret, kCaret);

        if (active) { // active-sequence dot, ahead of the name
            SDL_FRect dotSlot = cutLeft(inner, 5.0f);
            gapLeft(inner, 5.0f);
            if (onBand) {
                SDL_FRect dot = centerV(dotSlot, 5.0f);
                setColor(renderer_, kActive);
                jplay::fillRect(renderer_, &dot);
            }
        }

        // Eye button (ICON_MDI_EYE, U+F0208) on the right focuses this sequence in the timeline.
        const float eyeSz = 16.0f;
        gapRight(inner, 6.0f);
        SDL_FRect eyeSlot = cutRight(inner, eyeSz);
        SDL_FRect fEye = centerV(eyeSlot, eyeSz);
        pr.fEye = fEye;
        bool eyeHover = onBand && inRect(fEye, mx, my);
        if (eyeHover) hoveredEye = fEye;
        if (onBand)
            icons_.drawGlyph(renderer_, 0xF0208, fEye, active ? kActive : (eyeHover ? kRowText : kCaret));

        // Background-color button, left of the eye: a swatch of the sequence's own
        // color, so it doubles as the current-value readout. Opens the palette.
        const float swSz = 11.0f * dpiScale;
        gapRight(inner, 6.0f);
        SDL_FRect colorSlot = cutRight(inner, swSz);
        SDL_FRect fColor = centerV(colorSlot, swSz);
        pr.fColor = fColor;
        bool colorHover = onBand && inRect(fColor, mx, my);
        if (colorHover) hoveredColor = fColor;
        if (onBand) {
            setColor(renderer_, seqBgSdlColor(seq.bgColor, kSeqBgLift));
            jplay::fillRect(renderer_, &fColor);
            setColor(renderer_, colorHover ? kRowText : kCaret);
            jplay::drawRect(renderer_, &fColor);
        }

        int64_t a = 0, b = 0;
        std::string range = timeline_.sequenceSpan(seq, a, b) ? ("0-" + std::to_string(b - a)) : "empty";
        gapRight(inner, 8.0f);
        SDL_FRect rangeSlot = cutRight(inner, textFont_.measure(renderer_, range.c_str()));
        if (onBand) {
            const SDL_FRect rt = centerV(rangeSlot, lh);
            drawText(rt.x, rt.y, kSeqRange, range);
        }
        gapRight(inner, 8.0f);

        SDL_FRect nameSlot = inner;
        nameSlot.w = std::max(nameSlot.w, 20.0f); // never collapse below a clickable width
        SDL_FRect fName = centerV(nameSlot, lh + 4.0f);
        pr.fName = fName;
        if (onBand && !(peEdit_ == PeEdit::SeqName && peEditId_ == seq.id)) {
            const SDL_FRect nt = centerV(nameSlot, lh);
            drawText(nt.x + 2.0f, nt.y, kRowText, fitText(seq.name, fName.w - 4.0f));
        }

        peRows_.push_back(pr);

        if (collapsed)
            continue;

        const int64_t off = regs[si].start;
        for (int shotId : seq.shotIds) {
            Shot* sh = timeline_.findShotById(shotId);
            if (!sh) continue;

            SDL_FRect srow = cutTop(content, shotH);
            const bool inView = visibleIn(srow, band);
            bool live = timeline_.playhead >= sh->timelineStart && timeline_.playhead < sh->end();
            bool shover = inView && inRect(srow, mx, my);
            if (inView) {
                SDL_SetRenderDrawColor(renderer_, live ? 44 : (shover ? 38 : 30),
                                       live ? 48 : (shover ? 40 : 32),
                                       live ? 60 : (shover ? 46 : 38), 255);
                jplay::fillRect(renderer_, &srow);
            }

            PeRow sr;
            sr.kind = PeRow::Kind::Shot;
            sr.id = sh->id;
            sr.seqId = seq.id;
            sr.rect = srow;

            SDL_FRect sinner = srow;
            gapLeft(sinner, 20.0f); // shot indent (ex + 22 from the panel edge)
            gapTop(sinner, 2.0f);
            SDL_FRect nameSlot = cutTop(sinner, lh + 2.0f);
            gapTop(sinner, 1.0f);
            SDL_FRect fName2 = nameSlot;
            gapRight(fName2, 20.0f); // room for the warning triangle at the row's right edge
            fName2.w = std::max(fName2.w, 20.0f);
            sr.fName = fName2;
            if (inView && !(peEdit_ == PeEdit::ShotName && peEditId_ == sh->id)) {
                const SDL_FRect nt = centerV(nameSlot, lh);
                drawText(nt.x, nt.y, kShotName, fitText(sh->name, fName2.w - 4.0f));
            }
            if (inView && sh->cutDiffer) { // orange warning triangle: cut range edited from original
                float wx = srow.x + srow.w - 13.0f, wy = srow.y + 3.0f;
                fillTriangle(wx, wy + 9.0f, wx + 9.0f, wy + 9.0f, wx + 4.5f, wy, kWarn);
            }

            const int64_t locStart = sh->timelineStart - off;
            const int64_t locEnd = sh->end() - off;
            // Values start at a fixed column so the first number lines up
            // vertically across both rows regardless of label width.
            const char* lab1 = "Start - End:";
            const char* lab2 = "Cut In - Cut Out:";
            const float labelColW = std::max(textFont_.measure(renderer_, lab1),
                                             textFont_.measure(renderer_, lab2)) + 8.0f;
            // A labeled row: "<label>   <v1> - <v2>", where each value is an
            // individually editable cell cut off the left of what remains. Fills
            // r1/r2 for click/edit hit-testing (always), but only draws when the
            // row is on the visible band.
            const float spaceW = textFont_.measure(renderer_, " ");
            auto valueRow = [&](SDL_FRect rowRect, const char* label,
                                int64_t v1, SDL_FRect& r1, PeEdit k1,
                                int64_t v2, SDL_FRect& r2, PeEdit k2) {
                // Unbounded width: a narrow panel should clip the numbers off the
                // right edge, not collapse them into each other (cuts clamp).
                SDL_FRect cells = rowRect;
                cells.w = kUnbounded;
                SDL_FRect labelSlot = cutLeft(cells, labelColW);
                if (inView) drawText(labelSlot.x, labelSlot.y, kSeqRange, label);
                auto valCell = [&](int64_t v, SDL_FRect& r, PeEdit k) {
                    std::string seq = std::to_string(v);
                    // one space after the value
                    r = cutLeft(cells, textFont_.measure(renderer_, seq.c_str()) + spaceW);
                    if (inView && !(peEdit_ == k && peEditId_ == sh->id))
                        drawText(r.x, r.y, kShotCell, seq);
                };
                valCell(v1, r1, k1);
                SDL_FRect dash = cutLeft(cells, textFont_.measure(renderer_, "-") + spaceW);
                if (inView) drawText(dash.x, dash.y, kSeqRange, "-");
                valCell(v2, r2, k2);
            };
            SDL_FRect valRow1 = cutTop(sinner, lh + 2.0f);
            SDL_FRect valRow2 = cutTop(sinner, lh + 2.0f);
            valueRow(valRow1, lab1, locStart, sr.fA, PeEdit::ShotStart,
                                    locEnd,   sr.fB, PeEdit::ShotEnd);
            valueRow(valRow2, lab2, sh->cutIn,  sr.fCutIn,  PeEdit::ShotCutIn,
                                    sh->cutOut, sr.fCutOut, PeEdit::ShotCutOut);

            peRows_.push_back(sr);
        }
    }

    // Drag-to-reorder insertion line: snapped to the boundary the drop will land
    // on rather than tracking the cursor. Sequences span the full width; shots are
    // indented and confined to their owning sequence's shot block.
    if (peDragActive_ && peDragSeqId_ >= 0) {
        std::vector<const PeRow*> seqRows;
        for (const PeRow& r : peRows_)
            if (r.kind == PeRow::Kind::Seq) seqRows.push_back(&r);
        int insert = 0;
        for (const PeRow* r : seqRows)
            if (r->rect.y + r->rect.h * 0.5f < peDragY_) ++insert;
        float lineY = (insert < (int)seqRows.size())
            ? seqRows[insert]->rect.y
            : (peRows_.empty() ? peDragY_ : peRows_.back().rect.y + peRows_.back().rect.h);
        lineY = std::clamp(lineY, listTop, listBottom);
        setColor(renderer_, kDragLine);
        jplay::drawLine(renderer_, ex + 2.0f, lineY, ex + openPanelW() - 2.0f, lineY);
    } else if (peDragActive_ && peDragShotId_ >= 0) {
        std::vector<const PeRow*> shotRows;
        for (const PeRow& r : peRows_)
            if (r.kind == PeRow::Kind::Shot && r.seqId == peDragShotSeqId_) shotRows.push_back(&r);
        int insert = 0;
        for (const PeRow* r : shotRows)
            if (r->rect.y + r->rect.h * 0.5f < peDragY_) ++insert;
        if (!shotRows.empty()) {
            float lineY = (insert < (int)shotRows.size())
                ? shotRows[insert]->rect.y
                : shotRows.back()->rect.y + shotRows.back()->rect.h;
            lineY = std::clamp(lineY, listTop, listBottom);
            setColor(renderer_, kDragLine);
            jplay::drawLine(renderer_, ex + 20.0f, lineY, ex + openPanelW() - 2.0f, lineY);
        }
    }
    SDL_SetRenderClipRect(renderer_, &clip); // restore full-panel clip

    // Scroll indicator on the right edge of the band, as the SOURCES tab has —
    // grabbable too, so the draw also records what it maps onto for a press on it.
    drawScrollbar(renderer_, band, contentH, peSeqScroll_, dpiScale, true, peSeqSb_);
  } else {
    // ---- SOURCES header + the media bin -------------------------------------
    if (body.y <= bottom) {
        {
            SDL_FRect hdr = cutTop(body, rh);
            SDL_FRect removeSlot = cutRight(hdr, bs);
            gapRight(hdr, bgap);
            SDL_FRect addSlot = cutRight(hdr, bs);
            peRemoveRect_ = centerV(removeSlot, bs);
            peAddRect_    = centerV(addSlot, bs);
            const SDL_FRect cap = centerV(hdr, lh);
            drawText(cap.x, cap.y, kHeader, "SOURCES");
            drawPeButton(peAddRect_, true, inRect(peAddRect_, mx, my));
            drawPeButton(peRemoveRect_, false, inRect(peRemoveRect_, mx, my));
        }

        // Filter row: a name filter over the bin below. The X inside the field's
        // right edge clears it back to showing every source. The toggle pinned to
        // the row's right edge adds the timeline-visible narrowing on top.
        {
            SDL_FRect filterRow = cutTop(body, rh);
            SDL_FRect labelSlot = cutLeft(filterRow, textFont_.measure(renderer_, "Filter:"));
            const SDL_FRect lt = centerV(labelSlot, lh);
            drawText(lt.x, lt.y, kHeader, "Filter:");
            gapLeft(filterRow, 8.0f);
            // Filter on Timeline Visible (icon only), pinned to the right edge; the
            // field takes whatever is left. ICON_MDI_TIMELINE, U+F0BD1.
            const float tvSz = 16.0f * dpiScale;
            SDL_FRect tvSlot = cutRight(filterRow, tvSz);
            peFilterTlVisibleRect_ = centerV(tvSlot, tvSz);
            gapRight(filterRow, 6.0f * dpiScale);
            const bool tvHover = inRect(peFilterTlVisibleRect_, mx, my);
            icons_.drawGlyph(renderer_, 0xF0BD1, peFilterTlVisibleRect_,
                             peFilterTlVisible_ ? kActive : (tvHover ? kRowText : kCaret));
            SDL_FRect fld = centerV(filterRow, std::min(filterRow.h, lh + 6.0f));
            fld.w = std::max(fld.w, 20.0f);
            peFilterFld_.setRect(fld);
            peFilterFld_.render(renderer_, &textFont_);
            // Clear button (ICON_MDI_CLOSE, U+F0156), inside the field's right edge.
            const float xSz = 12.0f * dpiScale;
            SDL_FRect xInner = fld;
            gapRight(xInner, 3.0f * dpiScale);
            SDL_FRect xSlot = cutRight(xInner, xSz);
            peFilterClearRect_ = centerV(xSlot, xSz);
            const bool xHover = inRect(peFilterClearRect_, mx, my);
            icons_.drawGlyph(renderer_, 0xF0156, peFilterClearRect_,
                             peFilterFld_.text().empty() ? kBtnBorder
                                                         : (xHover ? kRowText : kCaret));
        }

        // "Go To Current" target: the source behind the first clip selected on the
        // timeline, or failing that the one under the playhead (what the player is
        // showing). Empty when neither exists, which greys the button out.
        std::string goToPath;
        {
            const Clip* clip = nullptr;
            if (!selectedClipIds_.empty())
                clip = timeline_.findClipById(selectedClipIds_.front());
            if (!clip)
                clip = playheadClip();
            if (clip && !clip->mediaId.empty())
                if (auto gm = timeline_.findMediaById(clip->mediaId))
                    goToPath = gm->path();
        }

        // Size row: a single slider drives the media-bin presentation — a plain
        // name list at the minimum, thumbnail rows just above it, an Overview-style
        // grid larger still.
        {
            SDL_FRect sizeRow = cutTop(body, rh);
            // Sort button (icon only) pinned to the right edge; the slider takes
            // whatever is left. ICON_MDI_SORT_VARIANT, U+F04BF.
            const float sortSz = 16.0f * dpiScale;
            SDL_FRect sortSlot = cutRight(sizeRow, sortSz);
            peSortBtnRect_ = centerV(sortSlot, sortSz);
            // Go To Current, left of the sort button. ICON_MDI_TARGET, U+F04FE.
            gapRight(sizeRow, 6.0f * dpiScale);
            SDL_FRect gotoSlot = cutRight(sizeRow, sortSz);
            peGoToCurrentRect_ = centerV(gotoSlot, sortSz);
            SDL_FRect labelSlot = cutLeft(sizeRow, textFont_.measure(renderer_, "Size:"));
            const SDL_FRect lt = centerV(labelSlot, lh);
            drawText(lt.x, lt.y, kHeader, "Size:");
            bool sortHover = inRect(peSortBtnRect_, mx, my);
            icons_.drawGlyph(renderer_, 0xF04BF, peSortBtnRect_,
                             peSortMenu_.isOpen() ? kActive : (sortHover ? kRowText : kCaret));
            const bool gotoHover = inRect(peGoToCurrentRect_, mx, my);
            icons_.drawGlyph(renderer_, 0xF04FE, peGoToCurrentRect_,
                             goToPath.empty() ? kBtnBorder : (gotoHover ? kRowText : kCaret));
            gapLeft(sizeRow, 8.0f);
            gapRight(sizeRow, 8.0f);
            const float trackH = 4.0f * dpiScale;
            peSizeSliderRect_ = centerV(sizeRow, trackH);
            peSizeSliderRect_.w = std::max(peSizeSliderRect_.w, 20.0f);
            setColor(renderer_, SDL_Color{ 52, 54, 62, 255 });
            jplay::fillRect(renderer_, &peSizeSliderRect_);
            setColor(renderer_, kBtnBorder);
            jplay::drawRect(renderer_, &peSizeSliderRect_);
            float t = std::clamp((peThumbSize_ - kThumbMin) / (kThumbMax - kThumbMin), 0.0f, 1.0f);
            float hx = peSizeSliderRect_.x + t * peSizeSliderRect_.w;
            // Handle grows past the thin track on each side; the click hit-test
            // in handleProjectExplorerEvent() grows by the same amount.
            const float grow = 4.0f * dpiScale;
            SDL_FRect handle = { hx - 3.0f * dpiScale, peSizeSliderRect_.y - grow,
                                 6.0f * dpiScale, trackH + 2.0f * grow };
            setColor(renderer_, SDL_Color{ 220, 224, 232, 255 });
            jplay::fillRect(renderer_, &handle);
        }

        // With a source selected, reserve a fixed-height info sub-panel at the
        // bottom; the list / thumbnail grid above it scrolls (peSourceScroll_).
        const bool showInfo = !inspectMediaPath_.empty();
        const float infoCap = 220.0f * dpiScale;
        const float infoH = showInfo ? std::min((bottom - body.y) * 0.5f, infoCap) : 0.0f;
        const float listTop = body.y;
        const float listBottom = bottom - infoH;
        const float viewH = std::max(listBottom - listTop, 0.0f);
        const SDL_FRect band = { ex, listTop, openPanelW(), listBottom - listTop };
        peSourceBand_ = band;

        std::vector<BinGroup> groups = binGroups();

        // Go To Current with the target's section folded away: open it here, before
        // the layout below measures the list, so the scroll still lands this frame.
        if (peGoToCurrentPending_ && !goToPath.empty()) {
            for (const BinGroup& g : groups) {
                if (g.header.empty() || !peCollapsedBinGroups_.count(g.header))
                    continue;
                for (const Media* media : g.sources)
                    if (media->path() == goToPath) { peCollapsedBinGroups_.erase(g.header); break; }
            }
        }

        // Source under the playhead (the one the player is showing). Recomputed
        // every frame, so as playback advances onto a clip backed by a different
        // source the highlight follows to that row.
        std::string playingPath;
        if (const Clip* pc = playheadClip(); pc && !pc->mediaId.empty())
            if (auto pm = timeline_.findMediaById(pc->mediaId))
                playingPath = pm->path();

        // Sources backing the clips currently selected on the timeline. Also
        // recomputed every frame; the selection is small, so a flat vector of
        // paths is cheaper than a set.
        std::vector<std::string> tlSelPaths;
        for (int id : selectedClipIds_)
            if (const Clip* sc = timeline_.findClipById(id); sc && !sc->mediaId.empty())
                if (auto sm = timeline_.findMediaById(sc->mediaId))
                    if (std::find(tlSelPaths.begin(), tlSelPaths.end(), sm->path()) == tlSelPaths.end())
                        tlSelPaths.push_back(sm->path());
        auto timelineSelected = [&](const Media* m) {
            return std::find(tlSelPaths.begin(), tlSelPaths.end(), m->path()) != tlSelPaths.end();
        };

        SDL_Rect listClip = { (int)ex, (int)listTop, (int)openPanelW(), (int)viewH };
        SDL_SetRenderClipRect(renderer_, &listClip);

        // Filled in by whichever layout runs below; drives the scroll indicator
        // drawn after the band clip is restored.
        float sbContentH = 0.0f;

        // Section header inside the scrolling list ("Sequence" order only —
        // the other orders produce a single headerless group). Scrolls with the
        // rows; the list clip rect trims it at the band edges. Each header carries
        // the same collapse caret a sequence row does; clicking anywhere on the
        // header folds its sources away (see projectTreeHandleEvent).
        const float hdrH = lh + 6.0f;   // header band height
        const float hdrGap = 6.0f;      // breathing room above every header but the first
        auto groupCollapsed = [&](const std::string& header) {
            return !header.empty() && peCollapsedBinGroups_.count(header) > 0;
        };
        // Whether a section holds the source under the playhead. Only meaningful
        // with several sections stacked, so with a single one the header stays
        // plain — the highlighted row is the only one there is.
        auto groupPlaying = [&](const BinGroup& g) {
            if (groups.size() < 2 || playingPath.empty())
                return false;
            for (const Media* media : g.sources)
                if (media->path() == playingPath)
                    return true;
            return false;
        };
        auto drawBinHeader = [&](const SDL_FRect& r, const std::string& text, bool playing) {
            if (!visibleIn(r, band))
                return;
            SDL_FRect fill = r;
            setColor(renderer_, SDL_Color{ 30, 32, 38, 255 });
            jplay::fillRect(renderer_, &fill);
            SDL_FRect inner = r;
            gapLeft(inner, 4.0f);
            gapRight(inner, 4.0f);
            SDL_FRect caretSlot = cutLeft(inner, 10.0f);
            gapLeft(inner, 6.0f);
            SDL_FRect caret = centerV(caretSlot, 10.0f);
            icons_.drawGlyph(renderer_, groupCollapsed(text) ? 0xF0142 : 0xF0140, caret, kCaret);
            const SDL_FRect slot = centerV(inner, lh);
            drawText(slot.x, slot.y, playing ? kPlaying : kHeader, fitText(text, inner.w));
            peBinHeaders_.push_back({ text, clipV(r, band) });
        };

        // Honour a pending arrow-key reveal: scroll the just-selected row into the
        // band, and no further than that — stepping down a list shouldn't re-centre
        // it under the cursor. y < 0 means the row isn't in the list (a filter hides
        // it, or its section is collapsed), which still clears the request.
        auto scrollRowIntoView = [&](float y, float rowH, float viewH, float maxScroll) {
            if (peRevealPath_.empty())
                return;
            peRevealPath_.clear();
            if (y < 0.0f)
                return;
            if (peSourceScroll_ > y)
                peSourceScroll_ = y;
            else if (peSourceScroll_ < y + rowH - viewH)
                peSourceScroll_ = y + rowH - viewH;
            peSourceScroll_ = std::clamp(peSourceScroll_, 0.0f, maxScroll);
        };

        // The sources whose cells this render puts on screen, in draw order. Handed
        // to syncSourceThumbs() below, which is the only thing that decides what
        // gets decoded — so a bin of hundreds doesn't stall the workers on cells
        // nobody can see. Empty at the name-list minimum (no thumbnails drawn).
        std::vector<Media*> thumbCells;

        if (peThumbSize_ <= kThumbRowMax) {
            // Row layout: the basename per row, with a small thumbnail on the left
            // once above the minimum. At the minimum no thumbnails are generated,
            // so it is a plain name list. Cells register into explorerRows_ so
            // select / drag-onto-timeline is reused unchanged.
            const bool showThumb = peThumbSize_ > kThumbMin;
            const float thumbSz = std::clamp(peThumbSize_, kThumbMin, kThumbRowMax);
            const float rowGap = 2.0f;
            const float rowH = std::max(showThumb ? thumbSz : 0.0f, lh) + rowGap;
            // Thumbnails are generated for the rows in the band plus one either
            // side, so a scroll lands on rows already decoded.
            const SDL_FRect thumbBand = { band.x, band.y - rowH, band.w, band.h + 2.0f * rowH };
            float contentH = 0.0f;
            float goToY = -1.0f;  // content-space top of the Go To Current row
            float revealY = -1.0f; // and of the row the arrow keys just selected
            for (size_t gi = 0; gi < groups.size(); ++gi) {
                if (!groups[gi].header.empty())
                    contentH += (gi ? hdrGap : 0.0f) + hdrH;
                if (groupCollapsed(groups[gi].header))
                    continue;
                for (const Media* media : groups[gi].sources) {
                    if (media->path() == goToPath) goToY = contentH;
                    if (media->path() == peRevealPath_) revealY = contentH;
                    contentH += rowH;
                }
            }
            const float maxScroll = std::max(0.0f, contentH - viewH);
            peSourceScroll_ = std::clamp(peSourceScroll_, 0.0f, maxScroll);
            sbContentH = contentH;
            // Width the scroll indicator takes off the right edge, known once the
            // content height is: the marker slot and the name stop short of it
            // rather than running under the bar.
            const float sbW = maxScroll > 0.0f ? scrollbarStripW(dpiScale, true) : 0.0f;
            if (peGoToCurrentPending_) {
                // Centre the target row in the band and select it, as a click on the
                // row would. Nothing to do if the filter hides it (goToY < 0).
                if (goToY >= 0.0f) {
                    peSourceScroll_ = std::clamp(goToY - (viewH - rowH) * 0.5f, 0.0f, maxScroll);
                    selectedSourcePaths_ = { goToPath };
                    selectionAnchorPath_ = goToPath;
                }
                peGoToCurrentPending_ = false;
            }
            scrollRowIntoView(revealY, rowH, viewH, maxScroll);
            SDL_FRect content = inset(SDL_FRect{ ex, listTop - peSourceScroll_, openPanelW(), kUnbounded },
                                      4.0f, 0.0f);
            for (size_t gi = 0; gi < groups.size(); ++gi) {
              if (!groups[gi].header.empty()) {
                  if (gi) gapTop(content, hdrGap);
                  SDL_FRect hdr = cutTop(content, hdrH);
                  drawBinHeader(hdr, groups[gi].header, groupPlaying(groups[gi]));
              }
              if (groupCollapsed(groups[gi].header))
                  continue;
              for (Media* media : groups[gi].sources) {
                // Hit target spans the whole row pitch, gap included, so a click
                // landing between two rows still picks the row above it.
                SDL_FRect slot = cutTop(content, rowH);
                SDL_FRect fill = slot;
                fill.h -= rowGap;
                if (showThumb && media->type() != ClipType::Audio && visibleIn(fill, thumbBand))
                    thumbCells.push_back(media); // audio draws a glyph, not a thumbnail
                SDL_FRect vis = clipV(fill, band);
                SDL_FRect hit = clipV(slot, band);
                if (vis.h > 1.0f) {
                    bool selected = sourceSelected(media);
                    bool playing = !playingPath.empty() && media->path() == playingPath;
                    bool tlSel = timelineSelected(media);
                    bool hover = inRect(hit, mx, my);
                    SDL_SetRenderDrawColor(renderer_, selected ? 58 : (hover ? 44 : 36),
                                           selected ? 70 : (hover ? 46 : 38),
                                           selected ? 92 : (hover ? 54 : 44), 255);
                    jplay::fillRect(renderer_, &fill);
                    SDL_FRect inner = fill;
                    // Marker slot at the right edge, reserved whether or not a
                    // square is drawn so the name does not reflow as the playhead
                    // moves between sources. Cream wins when a source is both.
                    gapRight(inner, 4.0f + sbW);
                    SDL_FRect markSlot = cutRight(inner, kMarkSize);
                    gapRight(inner, 4.0f);
                    if (tlSel || playing) {
                        SDL_FRect mk = centerV(markSlot, kMarkSize);
                        if (tlSel) {
                            setColor(renderer_, kTlSelDot);
                            jplay::drawRect(renderer_, &mk);
                        } else {
                            fillTriangle(mk.x, mk.y, mk.x + mk.w, mk.y,
                                         mk.x + mk.w * 0.5f, mk.y + mk.h, kPlayMark);
                        }
                    }
                    if (showThumb) {
                        gapLeft(inner, 2.0f);
                        SDL_FRect thumbSlot = cutLeft(inner, thumbSz);
                        SDL_FRect thumb = centerV(thumbSlot, thumbSz);
                        SDL_SetRenderDrawColor(renderer_, 52, 54, 60, 255);
                        jplay::fillRect(renderer_, &thumb);
                        if (media->type() == ClipType::Audio) // no frames: speaker glyph (ICON_MDI_VOLUME_HIGH, U+F057E)
                            icons_.drawGlyph(renderer_, 0xF057E, thumb, kAudioIcon);
                        else if (SDL_Texture* tex = sourceThumb(sourceThumbKey(media)))
                            SDL_RenderTexture(renderer_, tex, nullptr, &thumb);
                    }
                    gapLeft(inner, 6.0f);
                    fs::path mp(media->path());
                    std::string name = hashSeqStem(mp.stem().string(),
                                                   media->type() == ClipType::ImageSequence)
                                     + mp.extension().string();
                    std::string base = fitText(name, inner.w - 4.0f);
                    SDL_Color tc = sourceInTimeline(media) ? kRowText : kSourceDim;
                    const SDL_FRect nameSlot = centerV(inner, lh);
                    drawText(nameSlot.x, nameSlot.y, tc, base);
                    explorerRows_.push_back({ media->path(), hit });
                }
              }
            }
        } else {
            // Overview-style thumbnail grid; background generation mirrors the
            // Overview pane. Cells register into explorerRows_ so the existing
            // select / drag-onto-timeline machinery is reused unchanged.
            const float gap = 8.0f, labelH = 14.0f, gpad = 4.0f;
            const float gridX = ex + gpad;
            const float gridW = openPanelW() - 2.0f * gpad;
            float cellW = std::clamp(peThumbSize_, kThumbMin, gridW);
            int cols = std::max(1, (int)std::floor((gridW + gap) / (cellW + gap)));
            cellW = std::floor((gridW - (cols - 1) * gap) / cols);
            if (cellW < 1.0f) cellW = gridW;
            const float cellH = cellW + labelH;
            // Thumbnails are generated for the grid rows in the band plus one
            // either side, so a scroll lands on cells already decoded.
            const SDL_FRect thumbBand = { band.x, band.y - (cellH + gap), band.w,
                                          band.h + 2.0f * (cellH + gap) };
            // Each section starts its own block of grid rows, under a full-width
            // header. Measured first so the scroll range is known before drawing.
            auto gridRows = [&](const BinGroup& g) { return ((int)g.sources.size() + cols - 1) / cols; };
            float contentH = 0.0f;
            float goToY = -1.0f;  // content-space top of the Go To Current cell's row
            float revealY = -1.0f; // and of the cell the arrow keys just selected
            for (size_t gi = 0; gi < groups.size(); ++gi) {
                if (!groups[gi].header.empty())
                    contentH += (gi ? hdrGap : 0.0f) + hdrH;
                if (groupCollapsed(groups[gi].header))
                    continue;
                for (int i = 0; i < (int)groups[gi].sources.size(); ++i) {
                    const std::string& sp = groups[gi].sources[i]->path();
                    if (sp == goToPath)
                        goToY = contentH + (i / cols) * (cellH + gap);
                    if (sp == peRevealPath_)
                        revealY = contentH + (i / cols) * (cellH + gap);
                }
                int r = gridRows(groups[gi]);
                if (r > 0) contentH += r * cellH + (r - 1) * gap;
            }
            float maxScroll = std::max(0.0f, contentH - viewH);
            peSourceScroll_ = std::clamp(peSourceScroll_, 0.0f, maxScroll);
            sbContentH = contentH;
            // Width the scroll indicator takes off the right edge. The cells keep
            // their layout - only the last column's marker slides in, so the bar
            // can't cover it.
            const float sbW = maxScroll > 0.0f ? scrollbarStripW(dpiScale, true) : 0.0f;
            if (peGoToCurrentPending_) {
                // Centre the target's grid row in the band and select the cell, as a
                // click on it would. Nothing to do if the filter hides it (goToY < 0).
                if (goToY >= 0.0f) {
                    peSourceScroll_ = std::clamp(goToY - (viewH - cellH) * 0.5f, 0.0f, maxScroll);
                    selectedSourcePaths_ = { goToPath };
                    selectionAnchorPath_ = goToPath;
                }
                peGoToCurrentPending_ = false;
            }
            scrollRowIntoView(revealY, cellH, viewH, maxScroll);

            float top = listTop - peSourceScroll_; // running top of the current section
            for (size_t gi = 0; gi < groups.size(); ++gi) {
              const BinGroup& g = groups[gi];
              if (!g.header.empty()) {
                  if (gi) top += hdrGap;
                  drawBinHeader({ gridX, top, gridW, hdrH }, g.header, groupPlaying(g));
                  top += hdrH;
              }
              if (groupCollapsed(g.header))
                  continue;
              for (int i = 0; i < (int)g.sources.size(); ++i) {
                Media* media = g.sources[i];
                int col = i % cols, row = i / cols;
                float cx = gridX + col * (cellW + gap);
                float cy = top + row * (cellH + gap);
                const SDL_FRect cell = { cx, cy, cellW, cellH };
                if (media->type() != ClipType::Audio && visibleIn(cell, thumbBand))
                    thumbCells.push_back(media); // audio draws a glyph, not a thumbnail
                if (!visibleIn(cell, band))
                    continue; // scrolled out of view
                // Hit target covers the label strip and the row gap under the
                // thumbnail, so a click between two grid rows still picks a cell.
                const SDL_FRect hit = clipV({ cx, cy, cellW, cellH + gap }, band);
                SDL_FRect thumb = { cx, cy, cellW - 2.0f, cellW - 2.0f };
                bool selected = sourceSelected(media);
                bool playing = !playingPath.empty() && media->path() == playingPath;
                bool tlSel = timelineSelected(media);
                bool hover = inRect(hit, mx, my);
                SDL_SetRenderDrawColor(renderer_, selected ? 58 : 52,
                                       selected ? 70 : 54, selected ? 92 : 60, 255);
                jplay::fillRect(renderer_, &thumb);
                if (media->type() == ClipType::Audio) // no frames: speaker glyph (ICON_MDI_VOLUME_HIGH, U+F057E)
                    icons_.drawGlyph(renderer_, 0xF057E, thumb, kAudioIcon);
                else if (SDL_Texture* tex = sourceThumb(sourceThumbKey(media)))
                    SDL_RenderTexture(renderer_, tex, nullptr, &thumb);
                if (selected || hover) {
                    setColor(renderer_, selected ? kActive : SDL_Color{ 200, 210, 235, 220 });
                    jplay::drawRect(renderer_, &thumb);
                }
                if (tlSel || playing) {
                    // Marker right-aligned in the thumbnail's top corner on a dark
                    // backing so it still reads over a bright frame. Cream wins
                    // when a source is both selected and playing.
                    const float mkX = std::min(thumb.x + thumb.w - kMarkSize - 3.0f,
                                               ex + openPanelW() - sbW - kMarkSize - 1.0f);
                    const SDL_FRect mk = { mkX, thumb.y + 3.0f, kMarkSize, kMarkSize };
                    const SDL_FRect back = { mk.x - 1.0f, mk.y - 1.0f,
                                             mk.w + 2.0f, mk.h + 2.0f };
                    setColor(renderer_, SDL_Color{ 20, 22, 26, 255 });
                    jplay::fillRect(renderer_, &back);
                    if (tlSel) {
                        setColor(renderer_, kTlSelDot);
                        jplay::drawRect(renderer_, &mk);
                    } else {
                        fillTriangle(mk.x, mk.y, mk.x + mk.w, mk.y,
                                     mk.x + mk.w * 0.5f, mk.y + mk.h, kPlayMark);
                    }
                }
                fs::path mp(media->path());
                std::string name = hashSeqStem(mp.stem().string(),
                                               media->type() == ClipType::ImageSequence)
                                 + mp.extension().string();
                std::string base = fitText(name, cellW - 4.0f);
                float tw = textFont_.measure(renderer_, base.c_str());
                SDL_Color tc = sourceInTimeline(media) ? kRowText : kSourceDim;
                drawText(cx + (cellW - tw) * 0.5f, cy + cellW + 2.0f, tc, base);
                if (hit.h > 1.0f)
                    explorerRows_.push_back({ media->path(), hit });
              }
              int r = gridRows(g);
              if (r > 0) top += r * cellH + (r - 1) * gap;
            }
        }
        // Both layouts have laid their cells out: point the workers at them.
        syncSourceThumbs(thumbCells);

        SDL_SetRenderClipRect(renderer_, &clip); // restore full-panel clip

        // Scroll indicator on the right edge of the band, only when it overflows.
        // Grabbable, so the draw also records what it maps onto for a press on it.
        const SDL_FRect sbView{ ex, listTop, openPanelW(), viewH };
        drawScrollbar(renderer_, sbView, sbContentH, peSourceScroll_, dpiScale, true,
                      peSourceSb_);

        if (showInfo) {
            sourceInfoRect_ = { ex, listBottom, openPanelW(), infoH };
            renderSourceInfoPanel(sourceInfoRect_);
        } else {
            sourceInfoRect_ = {};
            sourceInfoCloseRect_ = {};
        }
    }
  }

    SDL_SetRenderClipRect(renderer_, nullptr);

    // Hover labels for the buttons of whichever tab is active, dropped below the
    // hovered button. Drawn unclipped so they overlay the list beneath.
    {
        auto tip = [&](const SDL_FRect& btn, const char* text) {
            if (btn.w <= 0.0f || !inRect(btn, mx, my))
                return; // other tab: the rect is zeroed
            const float tp = 5.0f * dpiScale;
            float bw = textFont_.measure(renderer_, text) + tp * 2.0f;
            float bh = lh + tp * 2.0f;
            float bx = std::clamp(btn.x + (btn.w - bw) * 0.5f, 4.0f, winW_ - bw - 4.0f);
            float by = btn.y + btn.h + 4.0f * dpiScale;
            SDL_FRect box = { bx, by, bw, bh };
            SDL_SetRenderDrawColor(renderer_, 20, 21, 24, 240);
            jplay::fillRect(renderer_, &box);
            setColor(renderer_, SDL_Color{ 80, 82, 90, 255 });
            jplay::drawRect(renderer_, &box);
            drawText(bx + tp, by + tp, SDL_Color{ 235, 238, 245, 255 }, text);
        };
        tip(peSeqAddRect_,      "Add sequence");
        tip(peSeqRemoveRect_,   "Remove active sequence");
        tip(hoveredEye,         "Show only this sequence");
        tip(hoveredColor,       "Sequence color");
        tip(peAddRect_,         "Add source media");
        tip(peRemoveRect_,      "Remove selected sources");
        tip(peFilterClearRect_, "Clear filter");
        tip(peFilterTlVisibleRect_, "Filter on Timeline Visible");
        tip(peSortBtnRect_,     "Sort sources");
        tip(peGoToCurrentRect_, "Go To Current");
        tip(sourceInfoCloseRect_, "Close media info");
    }

    // Inline edit field draws on top of whatever it covers.
    if (peEdit_ != PeEdit::None)
        peEditField_.render(renderer_, &textFont_);
}

std::vector<Media*> App::sortedSources() const {
    std::vector<Media*> v;
    for (const auto& kv : timeline_.media)
        if (kv.second)
            v.push_back(kv.second.get());
    auto lowerBase = [](Media* m) {
        std::string s = fs::path(m->path()).filename().string();
        std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char)std::tolower(c); });
        return s;
    };
    std::sort(v.begin(), v.end(), [&](Media* a, Media* b) { return lowerBase(a) < lowerBase(b); });
    return v;
}

// The SOURCES bin's contents: sortedSources() narrowed by the "Filter:" entry and
// by the timeline-visible toggle beside it.
// Alphabetical as it comes, which is PeSort::Name's order; PeSort::Sequence
// re-ranks it section by section in binGroups().
std::vector<Media*> App::binSources() const {
    std::vector<Media*> v = sortedSources();

    auto lower = [](std::string s) {
        std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char)std::tolower(c); });
        return s;
    };

    // "Filter:" entry under the SOURCES header: drop the sources whose displayed
    // name — the row label, so an image sequence matches on its #### form — does
    // not contain the text, compared case-insensitively.
    if (!peFilterFld_.text().empty()) {
        const std::string needle = lower(peFilterFld_.text());
        v.erase(std::remove_if(v.begin(), v.end(), [&](Media* m) {
                    fs::path mp(m->path());
                    const std::string name = hashSeqStem(mp.stem().string(),
                                                         m->type() == ClipType::ImageSequence)
                                           + mp.extension().string();
                    return lower(name).find(needle) == std::string::npos;
                }),
                v.end());
    }

    // "Filter on Timeline Visible" toggle right of the field: keep only the sources
    // a clip currently on screen in the timeline references.
    if (peFilterTlVisible_) {
        const std::unordered_set<std::string> vis = timelineVisibleMediaIds();
        v.erase(std::remove_if(v.begin(), v.end(),
                               [&](Media* m) { return vis.find(m->id()) == vis.end(); }),
                v.end());
    }
    return v;
}

// The bin split into the sections the renderer draws. PeSort::Name is one
// headerless section holding binSources() as it comes.
//
// PeSort::Sequence groups by project first and sequence second: projects appear
// in the order they first show up in the sequence list and their sequences in
// that same list order, so a project's sections stay together and read the way
// the SEQUENCES tab does. A source lands in the first section whose clips use it
// and is listed once, ranked by that first clip — so within a section the bin
// reads top to bottom the way the sequence plays. Sources no clip uses trail in a
// "No Sequence" section. Sections left empty (by the filter, or a sequence with
// no clips) are dropped rather than drawn as bare headers.
std::vector<App::BinGroup> App::binGroups() const {
    std::vector<Media*> v = binSources();
    if (peSort_ != PeSort::Sequence)
        return { BinGroup{ std::string(), std::move(v) } };

    std::vector<const Sequence*> seqs;
    {
        std::vector<std::string> projects; // first-appearance order, "" included
        for (const auto& s : timeline_.sequences) {
            const std::string pn = timeline_.projectNameOfSeq(s);
            if (std::find(projects.begin(), projects.end(), pn) == projects.end())
                projects.push_back(pn);
        }
        for (const std::string& p : projects)
            for (const auto& s : timeline_.sequences)
                if (timeline_.projectNameOfSeq(s) == p)
                    seqs.push_back(&s);
    }

    // Name the project in the header only when more than one is in play; with a
    // single project the prefix would repeat on every section for nothing.
    size_t namedProjects = 0;
    {
        std::vector<std::string> seen;
        for (const Sequence* seq : seqs) {
            const std::string pn = timeline_.projectNameOfSeq(*seq);
            if (!pn.empty() && std::find(seen.begin(), seen.end(), pn) == seen.end())
                seen.push_back(pn);
        }
        namedProjects = seen.size();
    }
    const bool showProject = namedProjects > 1;

    std::vector<BinGroup> groups;
    std::vector<std::vector<std::pair<size_t, Media*>>> ranked; // per group: (rank, source)
    std::unordered_map<std::string, std::pair<size_t, size_t>> place; // mediaId -> (group, rank)
    for (const Sequence* s : seqs) {
        std::vector<const Clip*> clips;
        for (const auto& c : s->clips)
            clips.push_back(&c);
        std::sort(clips.begin(), clips.end(), [](const Clip* a, const Clip* b) {
            return a->timelineStart != b->timelineStart ? a->timelineStart < b->timelineStart
                                                        : a->track < b->track;
        });
        BinGroup g;
        const std::string pn = timeline_.projectNameOfSeq(*s);
        g.header = (showProject && !pn.empty()) ? pn + " / " + s->name
                                                            : s->name;
        groups.push_back(std::move(g));
        ranked.emplace_back();
        size_t next = 0;
        for (const Clip* clip : clips)
            if (!clip->mediaId.empty())
                place.emplace(clip->mediaId, std::make_pair(groups.size() - 1, next++));
    }
    groups.push_back(BinGroup{ "No Sequence", {} }); // unplaced sources, last
    ranked.emplace_back();

    for (Media* media : v) {
        auto it = place.find(media->id());
        const size_t gi = it == place.end() ? groups.size() - 1 : it->second.first;
        const size_t rank = it == place.end() ? 0 : it->second.second;
        ranked[gi].push_back({ rank, media });
    }

    std::vector<BinGroup> out;
    for (size_t i = 0; i < groups.size(); ++i) {
        if (ranked[i].empty())
            continue;
        // v is alphabetical, so the stable sort leaves the unranked trailing
        // section — every entry rank 0 — in name order.
        std::stable_sort(ranked[i].begin(), ranked[i].end(),
                         [](const auto& a, const auto& b) { return a.first < b.first; });
        for (const auto& pr : ranked[i])
            groups[i].sources.push_back(pr.second);
        out.push_back(std::move(groups[i]));
    }
    return out;
}

void App::openPeSortMenu() {
    std::vector<ContextMenu::Item> items;
    auto add = [&](const char* label, PeSort mode) {
        ContextMenu::Item it;
        it.label = label;
        it.checked = (peSort_ == mode);
        it.action = [this, mode] {
            peSort_ = mode;
            writePrefs();
        };
        items.push_back(std::move(it));
    };
    add("Name", PeSort::Name);
    add("Sequence", PeSort::Sequence);
    // ContextMenu anchors its bottom edge at the given y and grows upward; offset
    // by the list's height so it lands just below the button instead.
    const float listH = (float)items.size() * 20.0f; // ContextMenu::kRowH
    peSortMenu_.open(peSortBtnRect_.x, peSortBtnRect_.y + peSortBtnRect_.h + listH,
                     winW_, winH_, std::move(items));
}

bool App::sourceInTimeline(const Media* m) const {
    bool found = false;
    timeline_.forEachClip([&](const Clip& c) {
        if (!found && c.mediaId == m->id())
            found = true;
    });
    return found;
}

// What the timeline is showing right now: the in-view sequences (the All view, a
// scoped project or a single filtered sequence) narrowed to the clips overlapping
// the visible frame range, so a pan or zoom changes the answer. Hidden clips count
// — the timeline still draws them, transparently.
std::unordered_set<std::string> App::timelineVisibleMediaIds() const {
    // Before the first layout contentW_ is 0; fall back to the window width so the
    // range is never empty.
    const double w = contentW_ > 0.0f ? (double)contentW_
                                      : std::max((double)winW_ - (double)headerX_, 1.0);
    const int64_t lo = (int64_t)std::floor(viewStart_);
    const int64_t hi = (int64_t)std::ceil(viewStart_ + w * framesPerPx_);
    std::unordered_set<std::string> ids;
    forEachViewClip([&](const Clip& c) {
        if (!c.mediaId.empty() && c.timelineStart < hi && c.end() > lo)
            ids.insert(c.mediaId);
    });
    return ids;
}

bool App::sourceSelected(const Media* m) const {
    return std::find(selectedSourcePaths_.begin(), selectedSourcePaths_.end(), m->path())
           != selectedSourcePaths_.end();
}

// ---------------------------------------------------------------- source thumbnails

std::string App::sourceThumbKey(const Media* m) const {
    // Per-source key (distinct from the Overview pane's per-clip keys, which fold
    // in sequence + sourceOffset), so the two never collide in the shared dir. The
    // leading tag is the cache format (ThumbnailCache::kFormatTag).
    return fnv1aHex(ThumbnailCache::kFormatTag + std::string("src\x1f") + m->path());
}

// (Re)start background generation for the cells the bin just drew (`cells`, in
// draw order, padded by a row either side of the visible band — see the render).
// Only those are decoded: a frame per source in the whole media pool stalled the
// workers on cells nobody can see, and left the ones on screen queued behind them.
//
// Restarting is debounced the way the Overview pane's syncGridThumbnails() does it:
// the drawn set has to hold still briefly, so scrolling through the bin re-fans the
// workers once at the end rather than on every notch. Cheap to call every frame
// from the render path.
void App::syncSourceThumbs(const std::vector<Media*>& cells) {
    std::vector<std::string> paths;
    paths.reserve(cells.size());
    for (const Media* m : cells)
        paths.push_back(m->path());

    if (paths == sourceThumbGen_) {
        sourceThumbPending_.clear();
        sourceThumbPendingMs_ = 0;
        return;
    }
    // An empty started-set means no pass covers anything yet (the Size slider just
    // came off the name-list minimum): start immediately rather than leaving the
    // cells grey for the settle delay.
    const Uint64 now = SDL_GetTicks();
    if (!sourceThumbGen_.empty()) {
        if (paths != sourceThumbPending_) {
            sourceThumbPending_ = paths;
            sourceThumbPendingMs_ = now;
            return;
        }
        if (now - sourceThumbPendingMs_ < 150)
            return;
    }
    sourceThumbPending_.clear();
    sourceThumbPendingMs_ = 0;
    sourceThumbGen_ = std::move(paths);

    sourceThumbs_.setDir(ThumbnailCache::resolveDir(projectHasPath_ ? projectPath_ : std::string()));
    std::vector<ThumbItem> items;
    items.reserve(cells.size());
    for (const Media* m : cells) {
        std::shared_ptr<Media> pm = timeline_.findMediaById(m->id());
        if (!pm)
            continue; // dropped from the pool since the cell was drawn
        int64_t fc = pm->info().frameCount;
        items.push_back({ sourceThumbKey(pm.get()), pm, fc > 0 ? fc / 2 : 0 });
    }
    sourceThumbs_.start(std::move(items));
}

void App::freeSourceThumbs() {
    sourceThumbs_.stop();
    for (auto& kv : sourceThumbTex_)
        if (kv.second)
            SDL_DestroyTexture(kv.second);
    sourceThumbTex_.clear();
    sourceThumbGen_.clear();
    sourceThumbPending_.clear();
    sourceThumbPendingMs_ = 0;
}

// SOURCES bin: load a per-source thumbnail. Returns null (caller draws a
// placeholder) until the background worker has produced it.
SDL_Texture* App::sourceThumb(const std::string& key) {
    return loadThumbTexture(sourceThumbs_, sourceThumbTex_, key);
}

// Selection on a ProjectExplorer row: plain click selects just it; Ctrl toggles
// it in/out of the selection; Shift selects the contiguous range (in displayed
// order) from the anchor to this row (Ctrl+Shift extends instead of replacing).
void App::clickExplorerRow(int index, SDL_Keymod mods) {
    if (index < 0 || index >= (int)explorerRows_.size())
        return;
    binSelectionActive_ = true; // touching the bin hands Delete to it
    const std::string path = explorerRows_[index].path;
    bool ctrl = (mods & SDL_KMOD_CTRL) != 0;
    bool shift = (mods & SDL_KMOD_SHIFT) != 0;

    auto add = [&](const std::string& p) {
        if (std::find(selectedSourcePaths_.begin(), selectedSourcePaths_.end(), p)
            == selectedSourcePaths_.end())
            selectedSourcePaths_.push_back(p);
    };

    // Resolve the anchor (stored by path) to its current displayed index.
    int anchor = -1;
    for (int i = 0; i < (int)explorerRows_.size(); ++i)
        if (explorerRows_[i].path == selectionAnchorPath_) { anchor = i; break; }

    if (shift && anchor >= 0) {
        if (!ctrl)
            selectedSourcePaths_.clear();
        int a = std::min(anchor, index), b = std::max(anchor, index);
        for (int i = a; i <= b; ++i)
            add(explorerRows_[i].path);
        // anchor stays put so the range can be re-dragged
    } else if (ctrl) {
        auto it = std::find(selectedSourcePaths_.begin(), selectedSourcePaths_.end(), path);
        if (it != selectedSourcePaths_.end())
            selectedSourcePaths_.erase(it);
        else
            selectedSourcePaths_.push_back(path);
        selectionAnchorPath_ = path;
    } else {
        selectedSourcePaths_ = { path };
        selectionAnchorPath_ = path;
    }
}

// Arrow-key move through the bin. Walks binGroups() rather than explorerRows_:
// the latter only holds the rows this render actually drew, so a selection at the
// edge of the band would have nowhere to step to. Collapsed sections are skipped,
// as they are when drawing. One item per press in either layout, grid included.
void App::stepBinSelection(int dir) {
    std::vector<std::string> paths;
    for (const BinGroup& g : binGroups()) {
        if (!g.header.empty() && peCollapsedBinGroups_.count(g.header))
            continue;
        for (const Media* media : g.sources)
            paths.push_back(media->path());
    }
    if (paths.empty())
        return;

    // The anchor is where the last click left the selection, so stepping continues
    // from there rather than from wherever a multi-selection happens to start.
    // With no anchor in the list (it was filtered out, or the selection came from
    // somewhere that doesn't set one) the first selected row stands in.
    auto indexOf = [&](const std::string& path) {
        for (int i = 0; i < (int)paths.size(); ++i)
            if (paths[i] == path)
                return i;
        return -1;
    };
    int cur = selectionAnchorPath_.empty() ? -1 : indexOf(selectionAnchorPath_);
    for (size_t i = 0; cur < 0 && i < selectedSourcePaths_.size(); ++i)
        cur = indexOf(selectedSourcePaths_[i]);

    // Nothing selected yet: the first press lands on the near end of the list.
    int next = cur < 0 ? (dir > 0 ? 0 : (int)paths.size() - 1) : cur + dir;
    if (next < 0 || next >= (int)paths.size())
        return; // already at the end; stay put rather than wrapping

    binSelectionActive_ = true; // as a click would: Delete now acts on the bin
    selectedSourcePaths_ = { paths[next] };
    selectionAnchorPath_ = paths[next];
    peRevealPath_ = paths[next];
}

// Mouse-down on a source row: select it (so a plain click still works) and arm a
// possible drag onto the timeline. Pressing an already-selected row with no
// modifier keeps the multi-selection intact in case this becomes a drag; if it
// turns out to be a plain click (no drag), the release collapses to this row.
// The MEDIA info sub-panel is not driven from here — only "Properties" on the
// right-click menu opens it (see openBinContextMenu).
void App::pressExplorerRow(int index, SDL_Keymod mods) {
    if (index < 0 || index >= (int)explorerRows_.size())
        return;
    bool plain = (mods & (SDL_KMOD_CTRL | SDL_KMOD_SHIFT)) == 0;
    bool onSelected = std::find(selectedSourcePaths_.begin(), selectedSourcePaths_.end(),
                                explorerRows_[index].path) != selectedSourcePaths_.end();
    if (plain && onSelected) {
        binDragReselect_ = true; // defer collapsing the selection until release
    } else {
        clickExplorerRow(index, mods);
        binDragReselect_ = false;
    }
    binDragRow_ = index;
    binDragging_ = false;
    // Logical units, so the drag threshold compares against converted event coords.
    uiMouse(binDragPressX_, binDragPressY_);
}

// Right-click on a source row. Selects the row first when it isn't already part
// of the selection (so the menu visibly belongs to what was clicked), then opens
// the popup at the cursor. The actions apply to the whole selection, so only the
// ones that mean something on more than one source are offered when several rows
// are selected.
void App::openBinContextMenu(int index, float mx, float my) {
    if (index < 0 || index >= (int)explorerRows_.size())
        return;
    const std::string path = explorerRows_[index].path;
    if (std::find(selectedSourcePaths_.begin(), selectedSourcePaths_.end(), path)
        == selectedSourcePaths_.end())
        clickExplorerRow(index, SDL_KMOD_NONE);

    std::vector<ContextMenu::Item> items;
    auto add = [&](const char* label, ContextMenu::Action action) {
        ContextMenu::Item it;
        it.label = label;
        it.action = std::move(action);
        items.push_back(std::move(it));
    };
    // A rule between groups, collapsed when the group before it is empty (the
    // single-source rows are conditional).
    auto sep = [&] {
        if (!items.empty() && !items.back().separator) {
            ContextMenu::Item it;
            it.separator = true;
            items.push_back(std::move(it));
        }
    };
    // Copying is the one action that spans a selection: every selected path,
    // space-separated. Reveal and Properties address a single source, so they
    // drop off the menu once more than one row is selected.
    const std::vector<std::string> paths = selectedSourcePaths_;
    add("Copy Source Path", [this, paths] {
        std::string joined;
        for (const auto& p : paths) {
            if (!joined.empty())
                joined += '\n';
            joined += p;
        }
        SDL_SetClipboardText(joined.c_str());
        setStatus(paths.size() > 1 ? std::to_string(paths.size()) + " PATHS COPIED"
                                   : std::string("PATH COPIED"), 2000);
    });
    if (paths.size() <= 1) {
        add("Show in File Browser", [this, path] {
            if (!revealInFileManager(path))
                setStatus("COULD NOT OPEN FILE BROWSER", 3000);
        });
    }
    // The same three the drop-action chooser over the video frame offers, on the
    // bin selection instead of on a drag (see applyPlayerDrop for what each does).
    // A scratch view up at the time is dropped first, so "Create" always stands a
    // new one up rather than adding to the one on screen.
    auto create = [this, paths](int action) {
        dropScratchView();
        for (size_t i = 0; i < paths.size(); ++i)
            applyPlayerDrop(action, paths[i], /*first=*/i == 0);
    };
    sep(); // the rows above only report on the source; the ones below use it
    add("Create Sequence", [create] { create(3); });
    add("Create Layout", [create] { create(4); });
    add("Create Stack", [create] { create(5); });
    sep(); // and the ones below change the source itself
    if (paths.size() <= 1) {
        add("Replace Source", [this, path] {
            // Asynchronous: browseReplaceSource only pins this source and raises the
            // chooser (see replaceSourceMedia for the swap itself).
            browseReplaceSource(path);
        });
    }
    // Applies to the whole selection, like Copy Source Path, and always confirms.
    add(paths.size() > 1 ? "Remove Sources" : "Remove Source", [this] {
        removeSelectedSource(/*alwaysConfirm=*/true);
    });
    if (paths.size() <= 1) {
        sep(); // keeps Properties clear of the destructive row above it
        add("Properties", [this, path] {
            // Opens the MEDIA info sub-panel at the bottom of the SOURCES tab on this
            // source, scrolled back to the top (renderSourceInfoPanel).
            inspectMediaPath_ = path;
            sourceInfoScroll_ = 0.0f;
        });
    }
    // ContextMenu anchors its bottom edge at the given y and grows upward; offset
    // by the list's height so the menu's top sits at the cursor instead.
    float listH = 0.0f; // ContextMenu::kRowH / kSepH
    for (const auto& it : items)
        listH += it.separator ? 5.0f : 20.0f;
    peMediaMenu_.open(mx, my + listH, winW_, winH_, std::move(items));
}

// Drop sources into the timeline the short way: scopes the view to "Default
// Sequence" (created on demand if the project has none) and puts the source in it.
// With replaceContents the sequence is emptied first, so the gesture yields just
// this source rather than accumulating past ones; callers adding several sources in
// one gesture pass false for all but the first, which then append. When appending,
// a source already placed there is not added twice — the playhead just jumps to its
// first clip.
//
// This lands real clips in a real sequence. A double-click on a bin row instead
// opens the source in a throwaway source view (App::openSourceView), which leaves
// the cut untouched.
void App::openSourceInDefaultSequence(const std::string& path, bool replaceContents) {
    const char* kDefaultSeqName = "Default Sequence";

    int idx = -1;
    for (int i = 0; i < (int)timeline_.sequences.size(); ++i)
        if (timeline_.sequences[i].name == kDefaultSeqName) { idx = i; break; }
    if (idx < 0) {
        Sequence seq;
        seq.id = nextSeqId_++;
        seq.name = kDefaultSeqName;
        timeline_.sequences.push_back(std::move(seq));
        timeline_.repackSequences();
        idx = (int)timeline_.sequences.size() - 1;
    }

    Sequence& seq = timeline_.sequences[idx];
    if (replaceContents && !(seq.clips.empty() && seq.shotIds.empty())) {
        // Drop the sequence's own shots from the global pool, then its clips —
        // same order removeActiveSequence uses.
        for (int sid : seq.shotIds)
            timeline_.shots.erase(std::remove_if(timeline_.shots.begin(), timeline_.shots.end(),
                                                 [sid](const Shot& sh) { return sh.id == sid; }),
                                  timeline_.shots.end());
        seq.shotIds.clear();
        seq.clips.clear();
        clearClipSelection();  // the selected ids just went away
        focusedShotId_ = -1;
        undoStack_.clear();    // clearing the sequence is not tracked on the undo stack
        timeline_.repackSequences();
        timeline_.clampPlayhead();
        infoClipId_ = -2;      // force the info overlay to rebuild
        hostSnapshotDirty_ = true; // re-push the project to spectators if hosting
    }

    activeSequenceIdx_ = idx;
    scopeToSequence(idx); // switches the view; addMediaFileAt then routes clips here

    if (auto pm = timeline_.findMediaByPath(mediaTypeForPath(path), path)) {
        const Clip* first = nullptr;
        for (const Clip& clip : timeline_.sequences[idx].clips)
            if (clip.mediaId == pm->id() && (!first || clip.timelineStart < first->timelineStart))
                first = &clip;
        if (first) {
            setPlayhead(first->timelineStart);
            setStatus("ALREADY IN " + std::string(kDefaultSeqName));
            return;
        }
    }

    // Append at the end of the sequence's own content on the target track (the
    // sequence's packed start when it holds nothing there yet).
    const int track = firstTrackForKind(isAudioPath(path));
    int64_t at = timeline_.seqRegions()[idx].start;
    for (const Clip& c : timeline_.sequences[idx].clips)
        if (c.track == track) at = std::max(at, c.end());

    const int newClipId = nextClipId_; // the clip addMediaFileAt is about to create
    if (addMediaFileAt(path, track, at) < 0)
        return; // rejected (wrong track kind / open failed); it set the status
    if (const Clip* c = clipById(newClipId))
        setPlayhead(c->timelineStart);
}

// Live preview while dragging source rows onto the tracks: reuse the file-drop
// "DROP HERE" box at the cursor's track/frame. Over the video frame the
// drop-action chooser takes over instead (see renderPlayerDropBoxes).
void App::updateBinDrag(float x, float y) {
    playerDropActive_ = inPlayerView(x, y) && playerDropChooserApplies();
    playerDropHover_ = playerDropActive_ ? playerDropBoxAt(x, y) : -1;
    if (overTrackArea(x, y)) {
        if (const Clip* above = pickerAlignDropClip(x, y)) {
            // Preview the slot itself rather than a cursor-width guess, so the box
            // reads as "exactly under that clip" (see pickerAlignDropClip).
            fileHoverTrack_ = above->track + 1;
            fileHoverStart_ = above->timelineStart;
            fileHoverSpan_ = above->duration;
        } else {
            dropTargetAt(x, y, fileHoverTrack_, fileHoverStart_);
            fileHoverSpan_ = 0;
        }
        fileHoverActive_ = true;
    } else {
        fileHoverActive_ = false;
    }
}

// Translucent card trailing the cursor while source rows are being dragged, so the
// drag reads as carrying the item. Over the tracks the "DROP HERE" placeholder
// already marks where it would land, so the card stays out of the way there; over
// the video frame it keeps naming what is being carried into the chooser.
void App::renderBinDragGhost() {
    if (!binDragging_ || binDragPaths_.empty() || fileHoverActive_)
        return;
    float mx = 0.0f, my = 0.0f;
    uiMouse(mx, my);

    // A Clip Source drag names the item that was grabbed (e.g. the version);
    // a bin drag names the source file.
    std::string label = binDragLabel_;
    if (label.empty()) {
        const std::string& path = binDragPaths_.front();
        fs::path p(path);
        label = hashSeqStem(p.stem().string(),
                            mediaTypeForPath(path) == ClipType::ImageSequence)
              + p.extension().string();
    }
    if (binDragPaths_.size() > 1)
        label += " +" + std::to_string(binDragPaths_.size() - 1);

    const float lh = textFont_.lineHeight();
    const float pad = 6.0f;
    SDL_FRect box = { mx + 12.0f, my + 12.0f,
                      textFont_.measure(renderer_, label.c_str()) + pad * 2.0f, lh + pad };
    box.x = std::min(box.x, winW_ - box.w - 2.0f); // keep it on screen near the edges
    box.y = std::min(box.y, winH_ - box.h - 2.0f);
    SDL_SetRenderDrawColor(renderer_, 58, 70, 92, 170);
    jplay::fillRect(renderer_, &box);
    SDL_SetRenderDrawColor(renderer_, kActive.r, kActive.g, kActive.b, 200);
    jplay::drawRect(renderer_, &box);
    drawText(box.x + pad, box.y + (box.h - lh) * 0.5f, SDL_Color{ 230, 236, 248, 220 }, label);
}

// Drop the dragged sources onto the timeline, laid end-to-end from the cursor.
// Dropped on the video frame instead, the chooser box under the cursor decides
// what happens (releasing between the boxes opens the source view, like "View").
// With no chooser — an empty timeline, where the launcher owns the frame — the
// frame keeps its old meaning: the sources go to the Default Sequence as real
// clips (unlike a double-click on the row, which only opens a throwaway source
// view).
void App::commitBinDrag(float x, float y) {
    fileHoverActive_ = false;
    playerDropActive_ = false;
    playerDropHover_ = -1;
    if (binDragPaths_.empty())
        return;
    if (inPlayerView(x, y)) {
        if (playerDropChooserApplies()) {
            int box = playerDropBoxAt(x, y);
            if (box < 0)
                box = 0; // released between the boxes: fall back to the source view
            // Creating a Layout/Stack from a Clip Source drag: bring the clip's
            // current source along too, ahead of the dragged version, so the
            // stage opens comparing the two instead of showing the new one alone.
            // Skipped when it's already what's being dragged (stepping to the
            // clip's own version is a no-op drag, not a comparison).
            if ((box == 4 || box == 5) && binDragPickerClipId_ >= 0) {
                const Clip* ref = timeline_.findClipById(binDragPickerClipId_);
                auto refMedia = ref ? timeline_.findMediaById(ref->mediaId) : nullptr;
                const std::string cur = refMedia ? refMedia->resolvedPath() : std::string();
                if (!cur.empty() &&
                    std::find(binDragPaths_.begin(), binDragPaths_.end(), cur) == binDragPaths_.end())
                    binDragPaths_.insert(binDragPaths_.begin(), cur);
            }
            for (size_t i = 0; i < binDragPaths_.size(); ++i)
                applyPlayerDrop(box, binDragPaths_[i], /*first=*/i == 0);
            return;
        }
        // Only the first replaces the sequence's contents; the rest append after it.
        for (size_t i = 0; i < binDragPaths_.size(); ++i)
            openSourceInDefaultSequence(binDragPaths_[i], /*replaceContents=*/i == 0);
        return;
    }
    if (!overTrackArea(x, y))
        return; // released off the tracks: no-op
    // Aligned Clip Source drop: same slot, same source frames as the clip above, so
    // the two versions sit frame-for-frame on top of each other. The range is
    // matched on absolute frame numbers (see alignedSourceRange) — the two versions
    // need not be numbered alike. Copy the fields first: adding the clip
    // invalidates the pointer.
    if (const Clip* above = pickerAlignDropClip(x, y)) {
        const std::string& path = binDragPaths_.front();
        const int dstTrack = above->track + 1;
        int64_t at = above->timelineStart, srcIn = -1, srcDur = -1;
        // alignedSourceRange reads the dropped source's extent, so open it first;
        // addMediaFileAt reuses this pool entry rather than opening it again.
        auto media = ensureMedia(path, mediaTypeForPath(path));
        if (!media)
            return; // ensureMedia set the status
        int64_t shift = 0;
        if (alignedSourceRange(*above, *media, srcIn, srcDur, shift))
            at += shift;
        else
            srcIn = srcDur = -1; // no overlap: place the whole source
        ContentSnapshot snap = captureContent();
        if (addMediaFileAt(path, dstTrack, at, /*queryAudio=*/true,
                           srcIn, srcDur, /*keepView=*/true) >= 0)
            pushContentUndo("ADD CLIP", snap);
        return;
    }
    int track = 0;
    int64_t frame = 0;
    dropTargetAt(x, y, track, frame);
    // Dropping into an empty sequence: scopeToSequence anchored the view on that
    // sequence's zero-width region, so there is no zoom worth keeping (the drop
    // asks addMediaFileAt to keep it). Fit once all the clips are in instead.
    const int fsi = filteredSeqIdx();
    const bool intoEmptySeq = fsi >= 0 && timeline_.sequences[fsi].clips.empty();
    // One undo step for the whole gesture, however many items it carried.
    ContentSnapshot before = captureContent();
    int added = 0;
    for (const auto& path : binDragPaths_) {
        int64_t end = addMediaFileAt(path, track, frame, /*queryAudio=*/true,
                                     /*srcIn=*/-1, /*srcDur=*/-1, /*keepView=*/true);
        if (end >= 0) { frame = end; ++added; }
    }
    if (added > 0 && intoEmptySeq)
        fitScope();
    if (added > 0)
        pushContentUndo(added == 1 ? "ADD CLIP"
                                   : "ADD " + std::to_string(added) + " CLIPS", before);
    if (added > 1)
        setStatus("ADDED " + std::to_string(added) + " CLIPS");
}

void App::addMediaViaBrowser() {
    // allow_many: like a multi-file drop. EXR sequences are picked by selecting
    // any one frame (the source re-expands the whole run, as a drop does).
    SDL_ShowOpenFileDialog(&App::onMediaChosen, this, window_, kMediaFilters,
                           SDL_arraysize(kMediaFilters), nullptr, true);
}

void App::removeSelectedSource(bool alwaysConfirm) {
    if (selectedSourcePaths_.empty()) {
        setStatus("NO CLIP SELECTED", 3000);
        return;
    }
    // Resolve the selection to pool entries (skipping any that no longer exist).
    std::vector<std::shared_ptr<Media>> targets;
    for (const auto& path : selectedSourcePaths_)
        for (const auto& kv : timeline_.media)
            if (kv.second && kv.second->path() == path) { targets.push_back(kv.second); break; }
    if (targets.empty()) {
        selectedSourcePaths_.clear();
        selectionAnchorPath_.clear();
        return;
    }

    // The removal itself, deferred behind the confirmation below when the sources
    // are in use. `targets` is captured by value: shared_ptr keeps them alive.
    auto doRemove = [this, targets] {
        if (draggingClip_) { draggingClip_ = false; dragClipId_ = -1; }
        for (const auto& t : targets) {
            const std::string& mid = t->id();
            for (auto& s : timeline_.sequences) {
                s.clips.erase(std::remove_if(s.clips.begin(), s.clips.end(),
                                             [&mid](const Clip& c) { return c.mediaId == mid; }),
                              s.clips.end());
            }
            timeline_.media.erase(mid);
        }
        size_t removed = targets.size();
        selectedSourcePaths_.clear();
        selectionAnchorPath_.clear();
        inspectMediaPath_.clear(); // drop the MEDIA view if it was on a removed source
        timeline_.repackSequences();
        timeline_.clampPlayhead();
        pruneTransitions(); // the removed sources took their clips, and their dissolves
        infoClipId_ = -2; // force the info overlay to rebuild for whatever's now under the playhead
        hostSnapshotDirty_ = true; // re-push the project to spectators if hosting
        setStatus("REMOVED " + std::to_string(removed) + " CLIP(S)");
    };

    // Confirm once if any selected source is used in the timeline.
    int inTl = 0;
    for (const auto& t : targets)
        if (sourceInTimeline(t.get()))
            ++inTl;
    if (inTl == 0) {
        if (!alwaysConfirm) {
            doRemove();
            return;
        }
        // Nothing to warn about — no clip goes away with these — so the prompt only
        // asks whether the removal was meant at all.
        std::string msg = (targets.size() == 1)
            ? "Remove \"" + fileLabel(targets[0]->path()) + "\" from the project?"
            : "Remove " + std::to_string(targets.size()) + " sources from the project?";
        showDialog("Remove Source", msg,
                   { { "Abort", nullptr }, { "Remove", doRemove } },
                   /*escIdx*/ 0, /*enterIdx*/ 0);
        return;
    }

    std::string msg = (targets.size() == 1)
        ? "This clip is used in the timeline. Remove it and all its instances?"
        : std::to_string(inTl) + " of " + std::to_string(targets.size()) +
              " selected clips are used in the timeline. Remove them all?";
    showDialog("Remove Clip", msg,
               { { "Abort", nullptr }, { "Remove", doRemove } },
               /*escIdx*/ 0, /*enterIdx*/ 0);
}

// Context-menu "Replace Source": pin the source the menu was opened on (by id, so
// a pool change while the chooser is up can't retarget the pick) and browse for
// the file that should stand in for it. The swap runs when the pick comes back,
// in processPendingDialogs -> replaceSourceMedia.
void App::browseReplaceSource(const std::string& path) {
    replaceSourceMediaId_.clear();
    for (const auto& kv : timeline_.media)
        if (kv.second && kv.second->path() == path) { replaceSourceMediaId_ = kv.first; break; }
    if (replaceSourceMediaId_.empty()) {
        setStatus("SOURCE NOT IN THE PROJECT", 3000);
        return;
    }
    // allow_many false: exactly one file stands in for the source.
    SDL_ShowOpenFileDialog(&App::onReplaceSourceChosen, this, window_, kMediaFilters,
                           SDL_arraysize(kMediaFilters), nullptr, false);
}

// Swap the file behind a bin source. Every clip on it keeps its source in point
// and duration, so a replacement of the same length or longer only changes the
// pixels; a shorter one shortens the clips that no longer fit, in place. Nothing
// ripples - the frames a shortened clip gives up are left as empty space.
//
// The clips are repointed at the replacement's own pool entry (an id is derived
// from type+path, so it brings its own frame-cache and thumbnail keys and no
// stale frame can survive the swap) and the old entry is erased, which is what
// takes the source out of the bin. Undo snapshots hold clips, not media, so
// restoring one would resurrect clips referencing an entry that is gone: the
// stack is cleared instead, as the other structural edits do.
void App::replaceSourceMedia(const std::string& oldMediaId, const std::string& newPath) {
    auto old = timeline_.findMediaById(oldMediaId);
    if (!old) {
        setStatus("SOURCE NO LONGER IN THE PROJECT", 3000);
        return;
    }
    const std::string oldPath = old->path();
    const ClipType newType = mediaTypeForPath(newPath);
    // Audio and picture are not interchangeable: a clip's kind decides which
    // tracks it may sit on (see addMediaFileAt), so swapping it would strand the
    // clip on a track that no longer accepts it.
    if ((newType == ClipType::Audio) != (old->type() == ClipType::Audio)) {
        setStatus(newType == ClipType::Audio ? "CANNOT REPLACE A PICTURE SOURCE WITH AUDIO"
                                            : "CANNOT REPLACE AN AUDIO SOURCE WITH PICTURE", 5000);
        return;
    }
    if (newType == old->type() && newPath == oldPath) {
        setStatus("SAME SOURCE - NOTHING REPLACED", 3000);
        return;
    }

    // Reuses the entry when the file is already in the bin, which merges the two
    // sources into one rather than leaving a duplicate behind.
    auto fresh = ensureMedia(newPath, newType);
    if (!fresh)
        return; // ensureMedia set the status
    if (fresh->id() == oldMediaId)
        return; // paths differ, so this can't happen; never erase what the clips now use

    const int64_t frames = std::max<int64_t>(fresh->info().frameCount, 1);
    int placed = 0, shortened = 0;
    for (auto& s : timeline_.sequences) {
        for (auto& c : s.clips) {
            if (c.mediaId != oldMediaId)
                continue;
            c.mediaId = fresh->id();
            ++placed;
            const int64_t off = std::clamp<int64_t>(c.sourceOffset, 0, frames - 1);
            const int64_t dur = std::clamp<int64_t>(c.duration, 1, frames - off);
            if (off != c.sourceOffset || dur != c.duration)
                ++shortened;
            c.sourceOffset = off;
            c.duration = dur;
            // Fades clamp to the duration on read (Timeline::clipFades) and the
            // annotations are keyed by source frame, so both follow this trim.
        }
    }
    timeline_.media.erase(oldMediaId);

    for (auto& p : selectedSourcePaths_)
        if (p == oldPath)
            p = newPath;
    if (selectionAnchorPath_ == oldPath)
        selectionAnchorPath_ = newPath;
    if (inspectMediaPath_ == oldPath)
        inspectMediaPath_ = newPath; // keep the MEDIA sub-panel on the row it was opened for

    timeline_.repackSequences();  // a shortened last clip changes its sequence's extent
    timeline_.clampPlayhead();
    pruneTransitions();           // a shortened clip may no longer meet its dissolve partner
    infoClipId_ = -2;             // force the info overlay to rebuild
    hostSnapshotDirty_ = true;    // re-push the project to spectators if hosting
    undoStack_.clear();           // its snapshots reference the media that just left the pool
    // What is on screen may be a frame the replacement doesn't have, so the
    // displayed key is dropped to force a re-render. The texture itself is kept:
    // the player holds the last frame until the replacement's decodes (as it does
    // for any undecoded frame) instead of flashing black across the swap. A
    // playhead left over a gap draws the backdrop regardless - the program image
    // is only drawn while a clip with a live source is under it.
    clearFramePreview();
    displayedKey_ = CacheKey{};

    std::string msg = "REPLACED WITH " + fileLabel(newPath);
    if (placed == 0)
        msg += " (NO CLIPS)";
    else if (shortened > 0)
        msg += " - " + std::to_string(shortened) + " CLIP(S) SHORTENED";
    setStatus(msg, 4000);
}

// ---------------------------------------------------------------- sequences tree

int64_t App::seqOffsetForShot(int shotId) const {
    auto regs = timeline_.seqRegions();
    for (size_t i = 0; i < timeline_.sequences.size(); ++i)
        for (int sid : timeline_.sequences[i].shotIds)
            if (sid == shotId)
                return regs[i].start;
    return 0;
}

void App::scopeToSequence(int seqIdx) {
    if (seqIdx < 0 || seqIdx >= (int)timeline_.sequences.size())
        return;
    // A source view is always the last sequence, so dropping it here leaves seqIdx
    // valid; scoping to the view itself (openSourceView) must not close it.
    dropScratchViewUnless(timeline_.sequences[seqIdx].id);
    pushViewHistory({timeline_.sequences[seqIdx].id, -1}); // Backspace comes back here
    activeSequenceIdx_ = seqIdx;
    focusedShotId_ = -1; // changing the sequence scope drops any shot focus
    clearProjectView();   // a single sequence and a project scope are exclusive
    viewSeqIdx_ = seqIdx; // reflected by the "Sequence" menu checkmark
    int64_t a = 0, b = 0;
    if (timeline_.sequenceSpan(timeline_.sequences[seqIdx], a, b)) {
        fitRange(a, b);
        setPlayhead(a);
    } else {
        // Empty sequence: no clips to span, so anchor the view at the sequence's
        // packed location (where dropped clips will land) instead of leaving it
        // on the previously scoped sequence.
        auto regs = timeline_.seqRegions();
        int64_t at = regs[seqIdx].start;
        fitRange(at, at);
        setPlayhead(at);
    }
}

void App::scopeToShot(int shotIdx) {
    if (shotIdx < 0 || shotIdx >= (int)timeline_.shots.size())
        return;
    const Shot& shot = timeline_.shots[shotIdx];
    focusedShotId_ = shot.id;
    fitRange(shot.timelineStart, shot.end());
    setPlayhead(shot.timelineStart);
}

void App::unscopeShot() {
    focusedShotId_ = -1;
    fitToFilteredSequence(); // back to the whole focused sequence; leaves the playhead put
}

void App::scopeToAll() {
    dropScratchViewUnless(-1);
    pushViewHistory({-1, -1});
    focusedShotId_ = -1;
    clearProjectView();
    viewSeqIdx_ = -1; // "All" — reflected by the "Sequence" menu checkmark
    fitView();
}

void App::addSequence() {
    Sequence seq;
    seq.id = nextSeqId_++;
    seq.name = "Sequence " + std::to_string(timeline_.sequences.size() + 1);
    timeline_.sequences.push_back(std::move(seq));
    timeline_.repackSequences();
    scopeToSequence((int)timeline_.sequences.size() - 1);
    hostSnapshotDirty_ = true; // re-push the project to spectators if hosting
    setStatus("ADDED SEQUENCE");
}

// Swatch-grid popup for the sequence row's color button: the whole 8 x 8
// background palette as unlabelled swatches, the current one outlined. Cells are
// painted lifted (kSeqBgLift) to match the swatch and the timeline bar.
void App::openSeqColorMenu(int seqId, const SDL_FRect& btn) {
    std::vector<ContextMenu::Item> items;
    const Sequence* seq = timeline_.findSequenceById(seqId);
    for (uint32_t c : kSeqBgColors) {
        ContextMenu::Item it;
        const SDL_Color rc = seqBgSdlColor(c, kSeqBgLift);
        it.rowColor = ((uint32_t)rc.r << 16) | ((uint32_t)rc.g << 8) | rc.b;
        it.checked = seq && seq->bgColor == c;
        it.action = [this, seqId, c] {
            if (Sequence* seq = timeline_.findSequenceById(seqId))
                seq->bgColor = c;
            hostSnapshotDirty_ = true; // re-push the project to spectators if hosting
        };
        items.push_back(std::move(it));
    }
    // ContextMenu anchors its bottom edge at the given y and grows upward; offset
    // by the grid's height so it lands just below the button instead.
    const float gridH = ContextMenu::gridHeight((int)items.size(), kSeqBgCols);
    peSeqColorMenu_.openGrid(btn.x, btn.y + btn.h + gridH, winW_, winH_, std::move(items),
                             kSeqBgCols);
}

void App::removeActiveSequence() {
    if (timeline_.sequences.empty())
        return;
    int idx = std::clamp(activeSequenceIdx_, 0, (int)timeline_.sequences.size() - 1);
    Sequence& seq = timeline_.sequences[idx];
    if (!seq.clips.empty()) {
        std::string msg = "Sequence \"" + seq.name + "\" has " + std::to_string(seq.clips.size()) +
                          " clip(s). Delete the sequence and its clips?";
        showDialog("Remove Sequence", msg,
                   { { "Abort", nullptr },
                     { "Delete", [this, idx] { removeSequenceAt(idx); } } },
                   /*escIdx*/ 0, /*enterIdx*/ 0);
        return;
    }
    removeSequenceAt(idx);
}

// The delete itself. The modal froze the UI while the confirmation was up, so
// `idx` still names the same sequence; it is re-validated all the same because
// the empty-sequence path reaches here directly.
void App::removeSequenceAt(int idx) {
    if (idx < 0 || idx >= (int)timeline_.sequences.size())
        return;
    Sequence& seq = timeline_.sequences[idx];
    // Drop the sequence's own shots from the global pool, then the sequence.
    for (int sid : seq.shotIds)
        timeline_.shots.erase(std::remove_if(timeline_.shots.begin(), timeline_.shots.end(),
                                             [sid](const Shot& sh) { return sh.id == sid; }),
                              timeline_.shots.end());
    timeline_.sequences.erase(timeline_.sequences.begin() + idx);
    if (timeline_.sequences.empty()) { // a project must keep at least one sequence
        Sequence seq;
        seq.id = nextSeqId_++;
        seq.name = "Default Sequence";
        timeline_.sequences.push_back(std::move(seq));
    }
    timeline_.repackSequences();
    // Removing the leading sequence would otherwise leave its frames as a hole in
    // front of the survivors, which they'd then carry as a permanent start offset.
    timeline_.normalizeLead();
    timeline_.clampPlayhead();
    undoStack_.clear(); // sequence delete is not tracked on the undo stack
    scopeToSequence(std::clamp(idx, 0, (int)timeline_.sequences.size() - 1));
    setStatus("REMOVED SEQUENCE");
}

// Sort each sequence's shot list by its shots' timeline position so the tree
// reflects the current layout. Sequences themselves are always packed in
// timeline order by repackSequences(), so only the shot lists can drift (after
// clip drags/trims move shots without touching shotIds order).
void App::refreshExplorerOrder() {
    for (auto& s : timeline_.sequences) {
        std::sort(s.shotIds.begin(), s.shotIds.end(), [&](int a, int b) {
            const Shot* sa = timeline_.findShotById(a);
            const Shot* sb = timeline_.findShotById(b);
            return (sa ? sa->timelineStart : 0) < (sb ? sb->timelineStart : 0);
        });
    }
}

bool App::reorderShotInSequence(int seqId, int shotId, int insertIdx) {
    int seqIdx = -1;
    for (int i = 0; i < (int)timeline_.sequences.size(); ++i)
        if (timeline_.sequences[i].id == seqId) { seqIdx = i; break; }
    if (seqIdx < 0) return false;
    Sequence& seq = timeline_.sequences[seqIdx];
    std::vector<int>& ids = seq.shotIds;
    int from = -1;
    for (int i = 0; i < (int)ids.size(); ++i)
        if (ids[i] == shotId) { from = i; break; }
    if (from < 0) return false;

    insertIdx = std::clamp(insertIdx, 0, (int)ids.size());
    int adj = insertIdx > from ? insertIdx - 1 : insertIdx;
    if (adj == from) return false; // dropped back into the same slot

    int moved = ids[from];
    ids.erase(ids.begin() + from);
    ids.insert(ids.begin() + adj, moved);

    // Re-lay the sequence's shots (and their clips) contiguously in the new order,
    // starting at the sequence's packed region start. Each shot keeps its
    // duration; its clips shift by the same delta so their positions within the
    // shot are preserved. shotIds now matches timeline order, so the tree stays
    // consistent without a refreshExplorerOrder() re-sort.
    auto regs = timeline_.seqRegions();
    int64_t cursor = regs[seqIdx].start;
    for (int sid : ids) {
        Shot* sh = timeline_.findShotById(sid);
        if (!sh) continue;
        int64_t delta = cursor - sh->timelineStart;
        if (delta != 0) {
            sh->timelineStart += delta;
            for (auto& c : seq.clips)
                if (c.shotId == sid)
                    c.timelineStart += delta;
        }
        cursor += sh->duration;
    }
    timeline_.repackSequences();
    timeline_.clampPlayhead();
    return true;
}

void App::beginPeEdit(PeEdit kind, int id, const SDL_FRect& rect, const std::string& initial) {
    peEdit_ = kind;
    peEditId_ = id;
    peEditRect_ = rect;
    peEditField_.setRect(rect);
    peEditField_.setText(initial);
    peEditField_.setFocus(true);
    SDL_StartTextInput(window_);
}

void App::commitPeEdit() {
    const std::string txt = peEditField_.text();
    auto parse = [&](int64_t& v) -> bool {
        try { v = std::stoll(txt); return true; } catch (...) { return false; }
    };
    // The clip that owns the shot being edited (shots mirror their clip, and the
    // player reads clips) — null for a bare shot with no media.
    auto ownerClip = [&]() -> Clip* {
        Clip* found = nullptr;
        timeline_.forEachClipMut([&](Clip& c) { if (c.shotId == peEditId_) found = &c; });
        return found;
    };
    switch (peEdit_) {
    case PeEdit::SeqName:
        if (Sequence* seq = timeline_.findSequenceById(peEditId_); seq && !txt.empty()) seq->name = txt;
        break;
    case PeEdit::ShotName:
        if (Shot* sh = timeline_.findShotById(peEditId_); sh && !txt.empty()) sh->name = txt;
        break;
    case PeEdit::ShotStart: {
        int64_t v;
        if (Shot* sh = timeline_.findShotById(peEditId_); sh && parse(v)) {
            int64_t target = std::max<int64_t>(v + seqOffsetForShot(peEditId_), 0);
            int64_t end = sh->end();
            if (Clip* clip = ownerClip()) {
                // Left-edge trim (end fixed): start + sourceOffset move together,
                // clamped to source frame 0 and one frame short of the end. Then
                // mirror the clip's new span back onto the shot bar.
                int64_t fixedEnd = clip->end();
                int64_t minStart = std::max<int64_t>(clip->timelineStart - clip->sourceOffset, 0);
                int64_t start = std::clamp<int64_t>(target, minStart, fixedEnd - 1);
                clip->sourceOffset += start - clip->timelineStart;
                clip->timelineStart = start;
                clip->duration = fixedEnd - start;
                sh->timelineStart = start;
                sh->duration = clip->duration;
                sh->setCut(clip->sourceOffset, clip->sourceOffset + clip->duration);
            } else {
                int64_t newStart = std::min(target, end - 1);
                sh->timelineStart = newStart;
                sh->duration = std::max<int64_t>(end - newStart, 1);
            }
            timeline_.repackSequences();
            timeline_.clampPlayhead();
        }
        break;
    }
    case PeEdit::ShotEnd: {
        int64_t v;
        if (Shot* sh = timeline_.findShotById(peEditId_); sh && parse(v)) {
            int64_t newEnd = v + seqOffsetForShot(peEditId_);
            if (Clip* clip = ownerClip()) {
                // Right-edge trim (start + sourceOffset fixed): duration follows,
                // capped at 1 frame and the frames remaining in the source.
                int64_t srcFrames = clip->sourceOffset + clip->duration;
                if (auto pm = timeline_.findMediaById(clip->mediaId)) {
                    int64_t fn = pm->info().frameCount;
                    if (fn > 0) srcFrames = fn;
                }
                int64_t maxDur = std::max<int64_t>(srcFrames - clip->sourceOffset, 1);
                int64_t dur = std::clamp<int64_t>(newEnd - clip->timelineStart, 1, maxDur);
                clip->duration = dur;
                sh->duration = dur;
                sh->setCut(clip->sourceOffset, clip->sourceOffset + dur);
            } else {
                sh->duration = std::max<int64_t>(newEnd - sh->timelineStart, 1);
            }
            timeline_.repackSequences();
            timeline_.clampPlayhead();
        }
        break;
    }
    case PeEdit::ShotCutIn: {
        int64_t v;
        if (Shot* sh = timeline_.findShotById(peEditId_); sh && parse(v)) sh->setCut(v, sh->cutOut);
        break;
    }
    case PeEdit::ShotCutOut: {
        int64_t v;
        if (Shot* sh = timeline_.findShotById(peEditId_); sh && parse(v)) sh->setCut(sh->cutIn, v);
        break;
    }
    case PeEdit::None:
        break;
    }
    peEdit_ = PeEdit::None;
    peEditId_ = -1;
    SDL_StopTextInput(window_);
}

bool App::projectTreeHandleEvent(const SDL_Event& e) {
    // An active inline edit captures typing plus Enter (commit) / Esc (cancel).
    if (peEdit_ != PeEdit::None) {
        if (e.type == SDL_EVENT_KEY_DOWN) {
            if (e.key.key == SDLK_RETURN || e.key.key == SDLK_KP_ENTER) { commitPeEdit(); return true; }
            if (e.key.key == SDLK_ESCAPE) { cancelPeEdit(); return true; }
            peEditField_.handleEvent(e); // backspace / arrows / home / end
            return true;
        }
        if (e.type == SDL_EVENT_TEXT_INPUT) { peEditField_.handleEvent(e); return true; }
        if (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN) {
            if (inRect(peEditRect_, e.button.x, e.button.y)) { peEditField_.handleEvent(e); return true; }
            commitPeEdit(); // click elsewhere commits, then the click is processed below
        }
    }

    // The SOURCES filter field keeps focus between events, so while it holds focus
    // typing goes to it rather than to the global keyboard shortcuts. Enter and Esc
    // give the keys back (Esc also clears the filter).
    if (peActiveTab_ == PeTabSources) {
        if (peFilterFld_.focused() &&
            (e.type == SDL_EVENT_KEY_DOWN || e.type == SDL_EVENT_TEXT_INPUT)) {
            if (e.type == SDL_EVENT_KEY_DOWN &&
                (e.key.key == SDLK_RETURN || e.key.key == SDLK_KP_ENTER || e.key.key == SDLK_ESCAPE)) {
                if (e.key.key == SDLK_ESCAPE)
                    peFilterFld_.setText("");
                peFilterFld_.setFocus(false);
                SDL_StopTextInput(window_);
                return true;
            }
            peFilterFld_.handleEvent(e);
            return true;
        }
        if (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN && e.button.button == SDL_BUTTON_LEFT) {
            if (peFilterClearRect_.w > 0.0f && inRect(peFilterClearRect_, e.button.x, e.button.y)) {
                peFilterFld_.setText(""); // the X inside the field's right edge
                return true;
            }
            if (peFilterTlVisibleRect_.w > 0.0f &&
                inRect(peFilterTlVisibleRect_, e.button.x, e.button.y)) {
                peFilterTlVisible_ = !peFilterTlVisible_;
                return true;
            }
            const bool wasFocused = peFilterFld_.focused();
            peFilterFld_.handleEvent(e); // focuses on a click inside, blurs on one outside
            if (peFilterFld_.focused()) {
                SDL_StartTextInput(window_);
                return true;
            }
            if (wasFocused)
                SDL_StopTextInput(window_);
        } else if (peFilterFld_.focused() &&
                   (e.type == SDL_EVENT_MOUSE_MOTION || e.type == SDL_EVENT_MOUSE_BUTTON_UP)) {
            if (peFilterFld_.handleEvent(e)) // drag-selection inside the field
                return true;
        }
    }

    if (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN && e.button.button == SDL_BUTTON_LEFT) {
        const float mx = e.button.x, my = e.button.y;
        const bool dbl = e.button.clicks >= 2;
        if (inRect(peTabSources_, mx, my))   { peActiveTab_ = PeTabSources; return true; }
        if (inRect(peTabSequences_, mx, my)) {
            peActiveTab_ = PeTabSequences;
            // The filter field is not drawn on this tab; drop its focus so the
            // tree's own inline edits are the only text consumer.
            if (peFilterFld_.focused()) {
                peFilterFld_.setFocus(false);
                SDL_StopTextInput(window_);
            }
            return true;
        }
        if (peSortBtnRect_.w > 0.0f && inRect(peSortBtnRect_, mx, my)) {
            openPeSortMenu();
            return true;
        }
        // Go To Current: the next render scrolls the bin to the source behind the
        // selected / playing clip and selects it (see renderProjectExplorer).
        if (peGoToCurrentRect_.w > 0.0f && inRect(peGoToCurrentRect_, mx, my)) {
            peGoToCurrentPending_ = true;
            return true;
        }
        // Grabbing either tab's scrollbar. Tested ahead of the rows and bin
        // headers below, which are laid out full-width and so reach under the
        // bar. Only the tab on show has a bar to hit, so neither needs a tab test.
        // The bar runs right up against the panel's resize edge, whose hit zone
        // reaches back over it: there the cursor already reads as a resize, so
        // that press belongs to the resize and not to the bar.
        if (panelResizeEdgeAt(mx, my) < 0) {
            if (peSourceSb_.press(mx, my, peSourceScroll_, dpiScale)) return true;
            if (peSeqSb_.press(mx, my, peSeqScroll_, dpiScale)) return true;
        }
        // Bin section header (Sequence order only — the list is empty otherwise):
        // clicking it collapses / expands that sequence's sources.
        for (const BinHeaderRow& h : peBinHeaders_) {
            if (!inRect(h.rect, mx, my)) continue;
            if (peCollapsedBinGroups_.count(h.header)) peCollapsedBinGroups_.erase(h.header);
            else peCollapsedBinGroups_.insert(h.header);
            return true;
        }
        // Expand the slider hit area to the handle's height (the same grow the
        // handle uses when drawn) so the bare track is as easy to click as the
        // handle itself.
        const float sliderGrow = 4.0f * dpiScale;
        SDL_FRect sliderHit = { peSizeSliderRect_.x, peSizeSliderRect_.y - sliderGrow,
                                peSizeSliderRect_.w, peSizeSliderRect_.h + 2.0f * sliderGrow };
        if (peActiveTab_ == PeTabSources && inRect(sliderHit, mx, my)) {
            // Clicking anywhere on the track snaps the handle to that exact
            // position and begins a drag from there.
            float t = std::clamp((mx - peSizeSliderRect_.x) / peSizeSliderRect_.w, 0.0f, 1.0f);
            peThumbSize_ = kThumbMin + t * (kThumbMax - kThumbMin);
            peSizeDragging_ = true;
            if (peThumbSize_ <= kThumbMin)
                freeSourceThumbs();
            return true;
        }
        if (inRect(peSeqAddRect_, mx, my)) { addSequence(); return true; }
        if (inRect(peSeqRemoveRect_, mx, my)) { removeActiveSequence(); return true; }
        // Rows scroll under a clipped band; ignore clicks that land outside it so
        // a row scrolled up behind the fixed header can't be hit by its off-band rect.
        const bool inSeqBand = (my >= peSeqListTop_ && my <= peSeqListBottom_);
        for (const PeRow& r : peRows_) {
            if (!inSeqBand || !inRect(r.rect, mx, my)) continue;
            auto toggleCollapse = [&](int id) {
                if (peCollapsedSeqs_.count(id)) peCollapsedSeqs_.erase(id);
                else peCollapsedSeqs_.insert(id);
            };
            if (r.kind == PeRow::Kind::Seq) {
                if (inRect(r.caret, mx, my)) { toggleCollapse(r.id); return true; }
                int idx = -1;
                for (int i = 0; i < (int)timeline_.sequences.size(); ++i)
                    if (timeline_.sequences[i].id == r.id) { idx = i; break; }
                // The eye button focuses the sequence in the timeline; clicking it
                // while already focused toggles back to the All view.
                if (inRect(r.fEye, mx, my)) {
                    if (idx >= 0)
                        (filteredSeqIdx() == idx) ? scopeToAll() : scopeToSequence(idx);
                    return true;
                }
                if (inRect(r.fColor, mx, my)) {
                    openSeqColorMenu(r.id, r.fColor);
                    return true;
                }
                if (inRect(r.fName, mx, my) && dbl) {
                    // Reverse the collapse toggle this double-click's first click
                    // already applied on release, then open the rename field.
                    toggleCollapse(r.id);
                    if (Sequence* seq = timeline_.findSequenceById(r.id))
                        beginPeEdit(PeEdit::SeqName, r.id, r.fName, seq->name);
                    return true;
                }
                // Arm a reorder drag. A plain click on the name (no drag) toggles
                // collapse on release, like the caret.
                peDragSeqId_ = r.id;
                peDragOnName_ = inRect(r.fName, mx, my);
                peDragPressY_ = peDragY_ = my;
                peDragActive_ = false;
                return true;
            }
            Shot* sh = timeline_.findShotById(r.id);
            if (!sh) return true;
            const int64_t off = seqOffsetForShot(r.id);
            // Value cells are sized to the digits they show; the edit field needs
            // room for the text field's own padding and the caret past the last
            // digit, plus slack for typing a longer number.
            auto numField = [](SDL_FRect c) {
                c.x -= 6.0f;
                c.w = std::max(c.w + 18.0f, 56.0f);
                return c;
            };
            if (inRect(r.fName, mx, my)) {
                if (dbl) { beginPeEdit(PeEdit::ShotName, r.id, r.fName, sh->name); return true; }
            } else if (inRect(r.fA, mx, my)) {
                beginPeEdit(PeEdit::ShotStart, r.id, numField(r.fA), std::to_string(sh->timelineStart - off)); return true;
            } else if (inRect(r.fB, mx, my)) {
                beginPeEdit(PeEdit::ShotEnd, r.id, numField(r.fB), std::to_string(sh->end() - off)); return true;
            } else if (inRect(r.fCutIn, mx, my)) {
                beginPeEdit(PeEdit::ShotCutIn, r.id, numField(r.fCutIn), std::to_string(sh->cutIn)); return true;
            } else if (inRect(r.fCutOut, mx, my)) {
                beginPeEdit(PeEdit::ShotCutOut, r.id, numField(r.fCutOut), std::to_string(sh->cutOut)); return true;
            }
            // Elsewhere on the shot row: arm a drag-to-reorder (confined to this
            // sequence). A plain click (no drag) scopes its sequence and jumps the
            // playhead on release.
            peDragShotId_ = r.id;
            peDragShotSeqId_ = r.seqId;
            peDragPressY_ = peDragY_ = my;
            peDragActive_ = false;
            return true;
        }
        return false; // not on the tree (source rows handled by the caller)
    }

    if (e.type == SDL_EVENT_MOUSE_MOTION && peSizeDragging_) {
        float t = std::clamp((e.motion.x - peSizeSliderRect_.x) / peSizeSliderRect_.w, 0.0f, 1.0f);
        peThumbSize_ = kThumbMin + t * (kThumbMax - kThumbMin);
        return true;
    }
    if (e.type == SDL_EVENT_MOUSE_BUTTON_UP && peSizeDragging_) {
        peSizeDragging_ = false;
        if (peThumbSize_ <= kThumbMin)
            freeSourceThumbs(); // at the minimum no thumbnails are shown; release them
        writePrefs(); // persist the final size
        return true;
    }

    if (e.type == SDL_EVENT_MOUSE_MOTION && (peDragSeqId_ >= 0 || peDragShotId_ >= 0)) {
        if (std::fabs(e.motion.y - peDragPressY_) > 4.0f) peDragActive_ = true;
        peDragY_ = e.motion.y;
        return peDragActive_;
    }

    if (e.type == SDL_EVENT_MOUSE_BUTTON_UP && peDragSeqId_ >= 0) {
        const bool was = peDragActive_;
        if (peDragActive_) {
            int insert = 0;
            for (const PeRow& r : peRows_)
                if (r.kind == PeRow::Kind::Seq && (r.rect.y + r.rect.h * 0.5f) < peDragY_) ++insert;
            int from = -1;
            for (int i = 0; i < (int)timeline_.sequences.size(); ++i)
                if (timeline_.sequences[i].id == peDragSeqId_) { from = i; break; }
            if (from >= 0) {
                // Pin the timeline's leading edge (first non-empty sequence's
                // start) so repack keeps the reordered sequences anchored where
                // the timeline began, rather than jumping to the new first
                // sequence's own position.
                int64_t lead = 0;
                for (const auto& s : timeline_.sequences) {
                    int64_t a = 0, b = 0;
                    if (timeline_.sequenceSpan(s, a, b)) { lead = a; break; }
                }
                // Remember which view is active (by id) so the reorder does not
                // yank the user out of "All" or off the sequence they're viewing.
                int viewedIdx = filteredSeqIdx();
                int viewedId = viewedIdx >= 0 ? timeline_.sequences[viewedIdx].id : -1;

                Sequence moved = std::move(timeline_.sequences[from]);
                timeline_.sequences.erase(timeline_.sequences.begin() + from);
                if (insert > from) --insert;
                insert = std::clamp(insert, 0, (int)timeline_.sequences.size());
                timeline_.sequences.insert(timeline_.sequences.begin() + insert, std::move(moved));
                timeline_.repackSequences(&lead);
                timeline_.clampPlayhead();

                if (viewedId < 0) {
                    scopeToAll();
                } else {
                    int ni = 0;
                    for (int i = 0; i < (int)timeline_.sequences.size(); ++i)
                        if (timeline_.sequences[i].id == viewedId) { ni = i; break; }
                    scopeToSequence(ni);
                }
                setStatus("REORDERED SEQUENCES");
            }
        } else if (peDragOnName_) {
            // Plain click on the sequence name toggles collapse, like the caret.
            if (peCollapsedSeqs_.count(peDragSeqId_)) peCollapsedSeqs_.erase(peDragSeqId_);
            else peCollapsedSeqs_.insert(peDragSeqId_);
        }
        peDragSeqId_ = -1;
        peDragOnName_ = false;
        peDragActive_ = false;
        (void)was;
        return true;
    }

    if (e.type == SDL_EVENT_MOUSE_BUTTON_UP && peDragShotId_ >= 0) {
        if (peDragActive_) {
            std::vector<const PeRow*> shotRows;
            for (const PeRow& r : peRows_)
                if (r.kind == PeRow::Kind::Shot && r.seqId == peDragShotSeqId_) shotRows.push_back(&r);
            int insert = 0;
            for (const PeRow* r : shotRows)
                if (r->rect.y + r->rect.h * 0.5f < peDragY_) ++insert;
            if (reorderShotInSequence(peDragShotSeqId_, peDragShotId_, insert))
                setStatus("REORDERED SHOTS");
        } else {
            // Plain click: jump the playhead to the shot. Only switch scope when
            // already viewing a *different* specific sequence; leave the "All" view
            // and the already-scoped sequence untouched.
            Shot* sh = timeline_.findShotById(peDragShotId_);
            int shotSeqIdx = -1;
            for (int i = 0; i < (int)timeline_.sequences.size(); ++i)
                if (timeline_.sequences[i].id == peDragShotSeqId_) { shotSeqIdx = i; break; }
            int viewed = filteredSeqIdx(); // -1 in the All view
            if (viewed >= 0 && shotSeqIdx >= 0 && viewed != shotSeqIdx)
                scopeToSequence(shotSeqIdx);
            if (sh) setPlayhead(sh->timelineStart);
        }
        peDragShotId_ = -1;
        peDragShotSeqId_ = -1;
        peDragActive_ = false;
        return true;
    }

    return false;
}

// ─── Presses inside the pane ─────────────────────────────────────────────────
// Reached from the pane's registry entry (App_NavPanel.cpp) once a press has
// landed inside the open pane and the app's own chrome — the strip toggles, the
// resize edge — has passed on it. The order below is the order the rects
// overlap in: the sub-panel close box and the +/- buttons sit over the bin, so
// they are tested before the rows underneath them.
bool App::projectExplorerHandleEvent(const SDL_Event& e) {
    if (e.type != SDL_EVENT_MOUSE_BUTTON_DOWN)
        return false;
    const float mx = e.button.x, my = e.button.y;

    if (e.button.button == SDL_BUTTON_RIGHT) {
        // Right-click a source row: copy path / filename, reveal in the system
        // file browser.
        for (int i = 0; i < (int)explorerRows_.size(); ++i)
            if (inRect(explorerRows_[i].rect, mx, my)) {
                openBinContextMenu(i, mx, my);
                return true;
            }
        return false;
    }
    if (e.button.button != SDL_BUTTON_LEFT)
        return false;

    if (inRect(sourceInfoCloseRect_, mx, my)) {
        // X on the MEDIA info sub-panel: close it.
        inspectMediaPath_.clear();
        sourceInfoScroll_ = 0.0f;
        sourceInfoRect_ = {};
        sourceInfoCloseRect_ = {};
        return true;
    }
    if (inRect(peAddRect_, mx, my)) {
        addMediaViaBrowser();
        return true;
    }
    if (inRect(peRemoveRect_, mx, my)) {
        removeSelectedSource();
        return true;
    }
    for (int i = 0; i < (int)explorerRows_.size(); ++i) {
        if (!inRect(explorerRows_[i].rect, mx, my))
            continue;
        // Double-click shows the source on its own in a throwaway source view,
        // leaving the cut alone; the first click already selected the row.
        if (e.button.clicks >= 2)
            openSourceView(explorerRows_[i].path);
        else
            pressExplorerRow(i, SDL_GetModState());
        return true;
    }
    // Empty space in the bin's scrolling band: drop the selection. Everything
    // else in the panel (tabs, headers, scrollbar, the buttons above) was
    // consumed before this point, so a press that reaches here inside the band
    // really is on nothing.
    if (inRect(peSourceBand_, mx, my) && !selectedSourcePaths_.empty()) {
        selectedSourcePaths_.clear();
        selectionAnchorPath_.clear();
    }
    return true; // clicks in the panel never reach the player / timeline
}

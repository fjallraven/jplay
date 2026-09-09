// Clip Source panel: a left-expand pane holding the same cascading
// naming-config pickers as the clip right-click menu (Department / Asset /
// Version …), stacked vertically instead of laid out as table columns. The
// cascade rules, the async Python queries and the media swap are shared with the
// menu — see buildPickerColumns / startPickerNavigate / startPickerCommit in
// App_TimelineClip.cpp. This unit is the view plus its refresh policy.
//
// Driven by the selected clip — or, with nothing selected, the clip under the
// frame indicator — rather than by a click, so the describe_pickers query runs
// only when the panel opens, when a different clip/source becomes the target, or
// after a commit. While the playhead is what drives it, playback suppresses the
// refresh: crossing clip boundaries would fire one Python query per boundary.
//
// A timeline selection of several clips is the target as a whole, and the panel then
// behaves like a right-click on that selection: the sections stop at the config's
// multi_select_picker, that one lists the values the selection itself carries, and a
// click applies the pick to every clip — with no drag and no marking, since the
// selection has no single source to carry out of the panel.
//
// A target change is diffed against the previous one before re-describing, since
// each option list depends only on the picker values above it: sections upstream of
// the first changed value keep what they show, and a change confined to the commit
// picker (a different version of the same asset) is answered from the result already
// in hand with no query at all. See updateClipSourceData.
//
// Panel icon (named here so the font subsetter includes its glyph):
//   ICON_MDI_LAYERS_TRIPLE

#include "App.h"
#include "AppInternal.h"
#include "Layout.h"
#include "PythonBridge.h"
#include "PythonStartup.h"

#include <algorithm>
#include <filesystem>
#include <memory>
#include <utility>

namespace fs = std::filesystem;

using namespace jplay;

namespace {

constexpr SDL_Color kHeader   = { 160, 170, 200, 255 }; // panel + section captions
constexpr SDL_Color kDim      = { 130, 134, 142, 255 }; // hints, cleared sections
constexpr SDL_Color kNeutral  = { 205, 209, 217, 255 }; // option text
constexpr SDL_Color kSelected = { 150, 190, 255, 255 }; // the current value
constexpr SDL_Color kCapBg    = {  34,  36,  41, 255 }; // band behind a section caption

inline void setCol(SDL_Renderer* r, SDL_Color c) {
    SDL_SetRenderDrawColor(r, c.r, c.g, c.b, c.a);
}

inline bool holds(const std::vector<std::string>& v, const std::string& s) {
    return std::find(v.begin(), v.end(), s) != v.end();
}

} // namespace

void App::closeClipSource() {
    // Closing is the cancel: bump the query id so an in-flight completion drops its
    // result instead of applying it to whatever the panel shows when reopened.
    ++panelCascade_.queryId;
    panelCascade_.loading = false;
    panelCascade_.clipIds.clear();
    panelCascade_.repPath.clear();
    panelCascade_.chain.clear();
    clipSourceStates_.clear();
    clipSourceClipId_ = -1;
    clipSourceMediaId_.clear();
    clipSourceRows_.clear();
    clipSourceNoPickers_ = false;
    clipSourceDragArmed_ = false;
    clipSourceMarked_.clear();
    clipSourceMarkAnchor_.clear();
    clipSourceDirty_ = true;
}

// A selected clip pins the panel to it, so the user can inspect and swap a clip's
// components without moving the playhead onto it. Dropping the selection hands the
// panel back to whatever is under the frame indicator.
const Clip* App::clipSourceTarget() const {
    if (selectedClipId_ >= 0)
        if (const Clip* sel = timeline_.findClipById(selectedClipId_))
            return sel;
    return playheadClip();
}

// More than one clip selected turns the panel into the multi-clip picker: the same
// set the right-click menu would open on, in selection order. buildPickerColumns
// reads the count off the cascade and does the rest — stopping at the config's
// multi_select_picker and listing the values the selection itself carries.
std::vector<int> App::clipSourceTargetIds() const {
    std::vector<int> ids;
    auto push = [&](const Clip* c) {
        if (c && !c->mediaId.empty() && timeline_.findMediaById(c->mediaId))
            ids.push_back(c->id);
    };
    if (selectedClipIds_.size() > 1) {
        for (int id : selectedClipIds_)
            push(timeline_.findClipById(id));
        return ids;
    }
    push(clipSourceTarget());
    return ids;
}

// Which level of the cascade a target change first touches. The naming-convention
// values of a media are cached on it (path-derived, serialized), so this is a map
// lookup per key rather than a Python query. A key set on one media and not the other
// counts as a difference: the two paths don't share that level.
int App::pickerDivergenceIndex(const Media& a, const Media& b) const {
    // The values that aren't pickers — the sequence and shot the path sits under, its
    // extension — place the media rather than select within it. Any of them differing
    // means the two clips aren't siblings, and then no section survives: an option list
    // gathered from one shot's neighbours says nothing about another's, even where the
    // department and asset happen to be named the same. Compared both ways so a key
    // only one of them carries counts too.
    auto placedApart = [this](const Media& x, const Media& y) {
        for (const auto& kv : x.meta())
            if (pickerDisplayIndex(kv.first) < 0 && y.metaValue(kv.first) != kv.second)
                return true;
        return false;
    };
    if (placedApart(a, b) || placedApart(b, a))
        return -1;

    for (int i = 0; i < (int)metaPickers_.size(); ++i) {
        const std::string& key = metaPickers_[i].key;
        if (a.metaValue(key) != b.metaValue(key))
            return i;
    }
    return (int)metaPickers_.size(); // every configured picker value matches
}

// Re-seed the cascade and re-describe the pickers for the target clip. Called every
// frame from render(); does real work only when a refresh is due.
void App::updateClipSourceData() {
    if (!panelOpen(kPanelClipSource))
        return;
    // A query is already resolving — its completion drives the next state.
    if (panelCascade_.loading)
        return;
    // Suppressed during playback, but only while the playhead is what picks the
    // target: the panel keeps showing the last resolved clip and catches up when
    // playback stops. A selection pins the target, so selecting a different clip
    // mid-playback still refreshes (one query, on a deliberate user action).
    if (playing_ && selectedClipId_ < 0)
        return;

    // The whole set the pickers will apply to; the first is the one described.
    const std::vector<int> ids = clipSourceTargetIds();
    const Clip* c = ids.empty() ? nullptr : timeline_.findClipById(ids.front());
    auto media = (c && !c->mediaId.empty()) ? timeline_.findMediaById(c->mediaId) : nullptr;
    const std::string mediaId = media ? media->id() : std::string();

    // No target clip (nothing selected, nothing under the playhead): clear once,
    // then stay idle.
    if (!media) {
        if (clipSourceDirty_ || clipSourceClipId_ != -1) {
            clipSourceStates_.clear();
            clipSourceRows_.clear();
            clipSourceClipId_ = -1;
            clipSourceMediaId_.clear();
            clipSourceSelIds_.clear();
            panelCascade_.clipIds.clear();
            panelCascade_.repPath.clear();
            panelCascade_.chain.clear();
            clipSourceNoPickers_ = false;
            clipSourceMarked_.clear();
            clipSourceMarkAnchor_.clear();
            clipSourceDirty_ = false;
        }
        return;
    }
    if (!clipSourceDirty_ && ids == clipSourceSelIds_
        && mediaId == clipSourceMediaId_)
        return; // same target clips and source; nothing to re-query

    buildMetaPickers(); // load the config picker list on first use (needs Python)
    if (metaPickers_.empty()) {
        // Python isn't up yet (or the config declares no pickers). Stay dirty so the
        // next refresh retries, but only once the interpreter is actually ready —
        // otherwise this would call into Python every frame.
        clipSourceNoPickers_ = jplayPythonReady();
        clipSourceDirty_ = !clipSourceNoPickers_;
        return;
    }

    // How deep the new target differs from the one the current describe result
    // belongs to. Only comparable while nothing has been navigated: after an upstream
    // pick the states describe a resolved path rather than any clip's own. -1 means
    // there is nothing to compare against, so none of the result survives — which is
    // also the answer whenever either side is a multi-clip target: crossing into (or
    // out of) that mode moves where the cascade commits and swaps the commit
    // section's options for the selection's own values, so nothing carries over.
    int diverge = -1;
    if (panelCascade_.chain.empty() && !clipSourceStates_.empty()
        && ids.size() == 1 && clipSourceSelIds_.size() == 1)
        if (auto prev = timeline_.findMediaById(clipSourceMediaId_))
            diverge = pickerDivergenceIndex(*prev, *media);

    // Fresh cascade for this target: the panel commits to the clips it describes —
    // one, or the whole timeline selection. The chain always drops — it records
    // navigation away from the previous target's path.
    panelCascade_.clipIds = ids;
    panelCascade_.repPath = media->resolvedPath();
    panelCascade_.chain.clear();
    clipSourceNoPickers_ = false;
    clipSourceClipId_ = c->id;
    clipSourceMediaId_ = mediaId;
    clipSourceSelIds_ = ids;
    clipSourceDirty_ = false;
    clipSourceMarked_.clear(); // the marks were made against the previous target
    clipSourceMarkAnchor_.clear();

    // Whether any picker below `from` offers options for the path the states in hand
    // describe. Which pickers apply is a property of the naming convention: a
    // versioned tree fills VERSION and leaves TAKE empty, a take-named tree the other
    // way round.
    auto hasOptionsBelow = [&](int from) {
        for (int i = from + 1; i < (int)metaPickers_.size(); ++i)
            for (const PickerState& s : clipSourceStates_)
                if (s.key == metaPickers_[i].key && !s.options.empty())
                    return true;
        return false;
    };

    // Only the deepest picker that applies here moved — the common case of stepping
    // between versions (or takes) of the same asset. Every option list the panel shows
    // is a function of the upstream values alone, so the whole describe result still
    // holds and the one thing that did change, which option reads as current, is
    // already in the list in hand. Answer it locally: no Python query, nothing
    // blanked, scroll kept.
    // Testing the last *configured* picker instead loses this as soon as the config
    // declares one the path doesn't use: with `take` listed after `version`, a version
    // step diverges at 2 of 4 and would re-describe on every step.
    if (diverge >= 0 && diverge < (int)metaPickers_.size() && !hasOptionsBelow(diverge)) {
        const std::string& key = metaPickers_[diverge].key;
        const std::string& value = media->metaValue(key);
        for (PickerState& s : clipSourceStates_) {
            if (s.key != key)
                continue;
            auto it = std::find_if(s.options.begin(), s.options.end(),
                                   [&](const PickerOption& o) { return o.value == value; });
            if (it != s.options.end()) {
                s.currentIndex = (int)(it - s.options.begin());
                return;
            }
            break; // the new value isn't in the list we hold; re-describe below
        }
    }

    // Sections upstream of the first changed value keep what they show: their option
    // lists depend only on values that didn't change, so the query hands back the same
    // ones. From the change down the states are dropped — leaving them would show the
    // previous clip's current value under those headers until the query lands, while
    // the source name above the pickers is already the new clip's. Those sections stay
    // listed but cleared meanwhile (see buildPickerColumns). With nothing comparable,
    // the whole result goes and the scroll returns to the top.
    if (diverge < 0) {
        clipSourceStates_.clear();
        clipSourceScroll_ = 0.0f;
    } else {
        clipSourceStates_.erase(
            std::remove_if(clipSourceStates_.begin(), clipSourceStates_.end(),
                           [&](const PickerState& s) {
                               const int i = pickerDisplayIndex(s.key);
                               return i < 0 || i >= diverge;
                           }),
            clipSourceStates_.end());
    }

    const uint64_t id = ++panelCascade_.queryId;
    panelCascade_.loading = true;
    struct DescribeResult {
        bool ok = false;
        std::vector<PickerState> states;
        std::map<std::string, std::string> values; // naming-convention values of `path`
    };
    auto res = std::make_shared<DescribeResult>();
    const std::string path = panelCascade_.repPath;
    pickerWork_.submit(
        [path, res](const std::atomic<bool>&) {
            res->ok = jplayDescribePickers(path, res->states);
            // Re-read the path's values while we're on the worker with the GIL: media
            // cached before the convention reported its non-picker groups (or added
            // with Python not yet up) carries too little for pickerDivergenceIndex to
            // trust, and refreshing here means every media the panel has described
            // compares properly next time — at no main-thread cost.
            jplayGetPathValues(path, res->values);
        },
        [this, id, mediaId, res] {
            if (id != panelCascade_.queryId) // panel closed or superseded
                return;
            panelCascade_.loading = false;
            clipSourceStates_ = res->ok ? std::move(res->states) : std::vector<PickerState>{};
            clipSourceNoPickers_ = !res->ok || clipSourceStates_.empty();
            // Applied on the main thread: Media's metadata map is written there only.
            if (auto m = timeline_.findMediaById(mediaId))
                for (auto& kv : res->values)
                    m->setMetaValue(kv.first, std::move(kv.second));
            // Labels and colors follow on their own query, so the option lists are
            // never held back by a site's database lookup. The panel redraws from
            // clipSourceStates_ every frame, so taking each instalment into it
            // is the whole refresh — the scroll position is untouched.
            panelCascade_.decorateApply = [this](const std::vector<PickerState>& st) {
                clipSourceStates_ = st;
            };
            startPickerDecorate(panelCascade_, clipSourceStates_);
        });
}

// ─── Panel ───────────────────────────────────────────────────────────────────
void App::renderClipSourcePanel() {
    if (!panelOpen(kPanelClipSource))
        return;
    SDL_FRect panel = beginLeftPanel();

    const float pad = 10.0f * dpiScale;
    const float lineH = textFont_.lineHeight();
    SDL_FRect body = inset(panel, pad, 0.0f);
    gapTop(body, 8.0f);

    SDL_FRect header = cutTop(body, lineH);
    drawText(header.x, header.y, kHeader, "CLIP SOURCE");
    gapTop(body, 8.0f);
    setCol(renderer_, SDL_Color{ 50, 53, 60, 255 });
    jplay::drawLine(renderer_, body.x, body.y, body.x + body.w, body.y);
    gapTop(body, 8.0f);

    // One line of the panel's full width.
    auto line = [&](SDL_Color col, const std::string& s) {
        SDL_FRect r = cutTop(body, lineH);
        drawText(r.x, r.y, col, s);
    };

    clipSourceRows_.clear();

    // Nothing to pick: say why rather than showing an empty pane. With several clips
    // in the cascade the representative it describes stands in for the target: the
    // primary selection may itself be one of the clips that carries no media.
    const bool multi = panelCascade_.clipIds.size() > 1;
    const Clip* clip = multi ? timeline_.findClipById(panelCascade_.clipIds.front())
                             : clipSourceTarget();
    if (!clip || clip->mediaId.empty()) {
        line(kDim, (clip && clip->id == selectedClipId_)
                       ? "Selected clip has no source."
                       : "No clip under the frame indicator.");
        return;
    }

    // The source the pickers describe, above the first picker header. Tagged when the
    // panel is pinned to the timeline selection rather than following the playhead.
    if (auto m = timeline_.findMediaById(clip->mediaId)) {
        line(kNeutral, fitText(fs::u8path(m->path()).filename().u8string(), body.w));
        gapTop(body, 3.0f);
    }
    // What the pick will land on: the one clip, or the whole selection with the
    // described source standing in for it above.
    if (multi) {
        line(kSelected, fitText("[" + std::to_string(panelCascade_.clipIds.size())
                                    + " CLIPS SELECTED - PICK APPLIES TO ALL]", body.w));
        gapTop(body, 3.0f);
    } else if (clip->id == selectedClipId_) {
        line(kSelected, "[SELECTED IN TIMELINE]");
        gapTop(body, 3.0f);
    }
    gapTop(body, 7.0f);

    if (clipSourceNoPickers_) {
        line(kDim, "No pickers for this source.");
        return;
    }

    // With no describe result yet this yields every configured picker cleared, so a
    // new target clip shows its section headers with the values blanked out instead
    // of the previous clip's.
    std::vector<PickerColumn> picks = buildPickerColumns(clipSourceStates_, panelCascade_);
    if (picks.empty()) {
        line(kDim, "Resolving\xe2\x80\xa6"); // config picker list not loaded yet
        return;
    }

    // Content area, clipped + scrollable (the option lists easily outrun the panel).
    // The clip rect spans the full panel width; only the layout is inset. `content`
    // is deliberately unbounded — see Layout.h — with its y advance read back below
    // as the height to clamp the scroll against.
    SDL_FRect view = body;
    gapBottom(view, 6.0f);
    SDL_Rect clipRect = { (int)panel.x, (int)view.y, (int)panel.w, (int)view.h };
    SDL_SetRenderClipRect(renderer_, &clipRect);

    const float rowH = 18.0f * dpiScale;
    const float contentTop = view.y - clipSourceScroll_;
    SDL_FRect content = { view.x, contentTop, view.w, kUnbounded };
    float mouseX = 0.0f, mouseY = 0.0f;
    uiMouse(mouseX, mouseY);

    // One chip width for the whole panel, the widest status in it: the badges then
    // line up in a column of their own down the right edge instead of each ending
    // where its own word does. Across all the pickers, not per section, so the
    // column doesn't step in and out as the eye goes down.
    float badgeChipW = 0.0f;
    for (const PickerColumn& p : picks)
        for (const PickerOption& o : p.options)
            badgeChipW = std::max(badgeChipW,
                                  statusBadgeWidth(renderer_, &textFont_, o.badge, dpiScale));

    // Content-space top of the commit section's current row, filled in below and
    // read back after the loop to honour a pending keyboard reveal.
    float revealY = -1.0f;

    for (int ci = 0; ci < (int)picks.size(); ++ci) {
        const PickerColumn& p = picks[ci];
        // The commit picker is the only interactive section — a click swaps the clip's
        // media, a drag carries it out onto the timeline — and nothing about the rows
        // says so, hence the caption hint. Inline when the panel is wide enough for it,
        // otherwise on a second caption line rather than clipped off the right edge.
        // With rows marked, the caption says so instead: the click-to-replace line
        // no longer describes what the section is holding.
        // Across several clips a click is the only thing a row does — the pick spans
        // the selection, so there is no one source to drag out of it and no marking
        // either (see clipSourceHandleEvent) — and the hint says so.
        std::string hint;
        if (p.commit && !p.options.empty())
            hint = multi ? "[click to apply to " + std::to_string(panelCascade_.clipIds.size())
                               + " clips]"
                 : clipSourceMarked_.empty()
                       ? "[click to replace or drag drop]"
                       : "[" + std::to_string(clipSourceMarked_.size()) + " marked - drag out]";
        const float labelW = textFont_.measure(renderer_, p.label.c_str());
        const float hintGap = 5.0f * dpiScale;
        const bool hintWraps = !hint.empty() &&
            labelW + hintGap + textFont_.measure(renderer_, hint.c_str()) > content.w;
        const float capExtra = hintWraps ? lineH + 2.0f : 0.0f;
        SDL_FRect capRow = cutTop(content, lineH + capExtra);
        gapTop(content, 4.0f);
        // Section caption on a darker band, so it reads as a header rather than a row.
        SDL_FRect cap = { capRow.x - 4.0f * dpiScale, capRow.y - 2.0f * dpiScale,
                          capRow.w + 8.0f * dpiScale, capRow.h + 4.0f * dpiScale };
        setCol(renderer_, kCapBg);
        jplay::fillRect(renderer_, &cap);
        SDL_FRect capText = capRow;
        SDL_FRect labelRow = cutTop(capText, lineH);
        drawText(labelRow.x, labelRow.y, p.options.empty() ? kDim : kHeader, p.label);
        if (!hint.empty()) {
            if (hintWraps) {
                gapTop(capText, 2.0f);
                SDL_FRect hintRow = cutTop(capText, lineH);
                drawText(hintRow.x, hintRow.y, kDim, fitText(hint, hintRow.w));
            } else {
                SDL_FRect hintSlot = labelRow;
                gapLeft(hintSlot, labelW + hintGap);
                drawText(hintSlot.x, hintSlot.y, kDim, hint);
            }
        }
        if (p.options.empty()) {
            // Cleared: listed to keep its place, filled in by an upstream pick.
            SDL_FRect none = cutTop(content, rowH);
            gapTop(content, 10.0f);
            gapLeft(none, 8.0f * dpiScale);
            drawText(none.x, none.y, kDim, "\xe2\x80\x94");
            continue;
        }
        for (int oi = 0; oi < (int)p.options.size(); ++oi) {
            SDL_FRect row = cutTop(content, rowH);
            const bool sel = (oi == p.checkedIdx);
            if (sel && p.commit)
                revealY = row.y - contentTop;
            // Hit-test only what the viewport actually shows, so a click can't land
            // on a row scrolled out under the header or past the bottom edge.
            if (visibleIn(row, view))
                clipSourceRows_.push_back({ row, ci, oi });
            // Ctrl/Shift-marked rows carry the bin's selection blue, brightened
            // under the cursor so hover still reads on them.
            const bool marked = p.commit && holds(clipSourceMarked_, p.options[oi].value);
            const bool hover = !panelCascade_.loading && inRect(row, mouseX, mouseY);
            if (marked || hover) {
                setCol(renderer_, marked ? (hover ? SDL_Color{ 72, 86, 112, 255 }
                                                  : SDL_Color{ 58, 70, 92, 255 })
                                         : SDL_Color{ 40, 43, 52, 255 });
                jplay::fillRect(renderer_, &row);
            }
            if (sel) { // accent stripe + tinted label on the current value
                SDL_FRect bar = { row.x, row.y, 2.0f * dpiScale, row.h };
                setCol(renderer_, kSelected);
                jplay::fillRect(renderer_, &bar);
            }
            SDL_FRect text = row;
            gapLeft(text, 8.0f * dpiScale);
            const SDL_FRect lbl = centerV(text, lineH);
            // A color from Python is a status (approved, published, on hold) and
            // outranks the current-value tint: the accent stripe drawn above
            // already says which row is current, so nothing is lost by letting the
            // status through on it.
            const uint32_t rgb = p.options[oi].color;
            const SDL_Color col = rgb ? SDL_Color{ (Uint8)((rgb >> 16) & 0xFF),
                                                   (Uint8)((rgb >> 8) & 0xFF),
                                                   (Uint8)(rgb & 0xFF), 255 }
                                      : (sel ? kSelected : kNeutral);
            // A badge says the same kind of thing on its own background instead,
            // right-aligned, leaving the label's text and color untouched. What it
            // takes on the right is what the label may no longer use.
            const float badgeW = drawStatusBadge(renderer_, &textFont_, row,
                                                 p.options[oi].badge,
                                                 p.options[oi].badgeColor, dpiScale,
                                                 badgeChipW);
            drawText(lbl.x, lbl.y, col,
                     fitText(p.options[oi].label, row.w - 10.0f * dpiScale - badgeW));
        }
        gapTop(content, 10.0f);
    }

    SDL_SetRenderClipRect(renderer_, nullptr);

    const float used = content.y - contentTop; // what the columns cut off
    const float maxScroll = std::max(0.0f, used - view.h);
    clipSourceScroll_ = std::clamp(clipSourceScroll_, 0.0f, maxScroll);
    // An arrow key moved the version: bring its row into the pane, no further than
    // needed. The layout is only known here, so the scroll lands a frame later. The
    // request survives the swap's in-flight query — until that resolves, the row
    // reading as current is still the version stepped away from.
    if (clipSourceRevealVersion_) {
        clipSourceRevealVersion_ = panelCascade_.loading;
        if (revealY >= 0.0f) {
            if (clipSourceScroll_ > revealY)
                clipSourceScroll_ = revealY;
            else if (clipSourceScroll_ < revealY + rowH - view.h)
                clipSourceScroll_ = revealY + rowH - view.h;
            clipSourceScroll_ = std::clamp(clipSourceScroll_, 0.0f, maxScroll);
        }
    }
    // Against the panel edge, not the padded body: inset by the 10px body pad it
    // floated in the middle of the right margin (see the clip rect above, which
    // already spans the full panel width for the same reason).
    drawScrollbar(renderer_, { panel.x, view.y, panel.w, view.h }, used,
                  clipSourceScroll_, dpiScale, true);
}

// ─── Event handling ──────────────────────────────────────────────────────────
bool App::clipSourceHandleEvent(const SDL_Event& e) {
    if (e.type != SDL_EVENT_MOUSE_BUTTON_DOWN || e.button.button != SDL_BUTTON_LEFT)
        return false;
    if (panelCascade_.loading)
        return true; // a query is resolving; swallow the click rather than queue another

    std::vector<PickerColumn> picks = buildPickerColumns(clipSourceStates_, panelCascade_);
    for (const ClipSourceRow& r : clipSourceRows_) {
        if (!inRect(r.rect, e.button.x, e.button.y))
            continue;
        if (r.col >= (int)picks.size() || r.opt >= (int)picks[r.col].options.size())
            break; // stale row (the panel refreshed since the last render)
        const PickerColumn& p = picks[r.col];
        const std::string key = p.key, value = p.options[r.opt].value;
        if (p.commit) {
            // Several clips: the row is a pick for the whole selection, so it acts on
            // the press and neither arms a drag (there is no single source to carry
            // out — the selection sits on several) nor takes a mark.
            if (panelCascade_.clipIds.size() > 1) {
                clipSourceDragKey_ = key;
                clipSourceDragValue_ = value;
                commitClipSourcePick();
                break;
            }
            const SDL_Keymod mods = SDL_GetModState();
            const bool ctrl = (mods & SDL_KMOD_CTRL) != 0;
            const bool shift = (mods & SDL_KMOD_SHIFT) != 0;
            if (ctrl || shift) {
                markClipSourceOption(p, r.opt, ctrl, shift);
                break; // a mark, not a pick: nothing is armed and nothing is replaced
            }
            // A plain press inside the marks keeps them, so the drag can carry the
            // group; on any other row they go, as a plain click does in the bin.
            if (!holds(clipSourceMarked_, value)) {
                clipSourceMarked_.clear();
                clipSourceMarkAnchor_.clear();
            }
            // Don't act on the press: this row can also be dragged out onto the
            // timeline, so the swap waits for a release in place (see App.cpp's
            // motion/release handling and beginClipSourceDrag).
            clipSourceDragArmed_ = true;
            clipSourceDragKey_ = key;
            clipSourceDragValue_ = value;
            clipSourceDragLabel_ = p.options[r.opt].label;
            clipSourceDragPressX_ = e.button.x;
            clipSourceDragPressY_ = e.button.y;
        } else {
            clipSourceMarked_.clear(); // the commit list is about to be rebuilt
            clipSourceMarkAnchor_.clear();
            startPickerNavigate(panelCascade_, key, value, pickerDisplayIndex(key),
                [this](const std::vector<PickerState>& st, bool ok) {
                    clipSourceStates_ = ok ? st : std::vector<PickerState>{};
                    clipSourceNoPickers_ = !ok;
                });
        }
        break;
    }
    return true; // swallow any other click inside the panel
}

// Ctrl / Shift on a commit row: mark it rather than replace with it. Ctrl toggles
// the row in and out of the marks; Shift marks the run from the anchor to it (in
// list order), replacing the marks unless Ctrl extends them instead. Mirrors
// clickExplorerRow, minus the click-selects-one case: a plain click on these rows
// already means "replace with this version".
void App::markClipSourceOption(const PickerColumn& p, int opt, bool ctrl, bool shift) {
    auto add = [&](const std::string& v) {
        if (!holds(clipSourceMarked_, v))
            clipSourceMarked_.push_back(v);
    };
    const std::string& value = p.options[opt].value;

    // The anchor is held as a value; a re-describe may have dropped its row.
    int anchor = -1;
    for (int i = 0; i < (int)p.options.size(); ++i)
        if (p.options[i].value == clipSourceMarkAnchor_) { anchor = i; break; }

    if (shift && anchor >= 0) {
        if (!ctrl)
            clipSourceMarked_.clear();
        for (int i = std::min(anchor, opt); i <= std::max(anchor, opt); ++i)
            add(p.options[i].value);
        return; // anchor stays put, so the range can be re-dragged from it
    }
    if (ctrl) {
        auto it = std::find(clipSourceMarked_.begin(), clipSourceMarked_.end(), value);
        if (it != clipSourceMarked_.end())
            clipSourceMarked_.erase(it);
        else
            clipSourceMarked_.push_back(value);
    } else {
        add(value); // Shift with no anchor: the first mark, and the anchor for the next
    }
    clipSourceMarkAnchor_ = value;
}

// The armed commit row moved past the threshold: hand it to the media-bin drag,
// which already owns the ghost card, the frame's three-box drop chooser and the
// drop onto the tracks. The pick is resolved to a real media path here, so from
// this point on the drag is indistinguishable from dragging a SOURCES row.
bool App::beginClipSourceDrag() {
    clipSourceDragArmed_ = false; // one shot: it becomes a drag or nothing
    // Pressing inside the marks drags the whole set, in the order the rows are
    // drawn so the drop lands them predictably; any other row drags alone.
    std::vector<std::string> values;
    if (holds(clipSourceMarked_, clipSourceDragValue_))
        for (const PickerColumn& p : buildPickerColumns(clipSourceStates_, panelCascade_))
            if (p.commit)
                for (const PickerOption& o : p.options)
                    if (holds(clipSourceMarked_, o.value))
                        values.push_back(o.value);
    if (values.empty())
        values = { clipSourceDragValue_ };

    std::vector<std::string> paths;
    std::string missing;
    for (const std::string& value : values) {
        std::string path;
        bool missed = false;
        // `missed` means the row's own value didn't resolve and the resolve handed back
        // the path it had reached instead. A commit lives with that (the asset's latest
        // stands in for a version some clip in the selection lacks), but a drag would
        // carry a source the card doesn't name, so drop it and say why.
        if (resolvePickerPick(panelCascade_, clipSourceDragKey_, value, path, &missed)
            && !missed) {
            paths.push_back(path);
            continue;
        }
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "Picker drag: %s=%s has no media (from %s)",
                    clipSourceDragKey_.c_str(), value.c_str(),
                    panelCascade_.repPath.c_str());
        missing += (missing.empty() ? "" : ", ") + value;
    }
    if (!missing.empty())
        setStatusWarn("NO MEDIA FOR " + missing);
    if (paths.empty())
        return false;
    binDragRow_ = -1; // not a bin row: nothing to re-select on release
    binDragPaths_ = std::move(paths);
    binDragLabel_ = clipSourceDragLabel_; // the card names the item as drawn, not the file
    const Clip* target = clipSourceTarget();
    binDragPickerClipId_ = target ? target->id : -1; // enables the aligned drop below it
    binDragging_ = true;
    return true;
}

// Dragging a picker item into the free video row directly under its own clip means
// "put this version alongside that one", so the drop lands in the clip's exact
// slot instead of at the cursor. Anywhere else on the tracks — a different row, or
// past the clip's ends where other shots live — the ordinary drop applies.
const Clip* App::pickerAlignDropClip(float x, float y) const {
    if (binDragPickerClipId_ < 0 || !overTrackArea(x, y))
        return nullptr;
    if (binDragPaths_.size() != 1)
        return nullptr; // a marked group has no single slot to align into
    const Clip* above = timeline_.findClipById(binDragPickerClipId_);
    if (!above || above->audio)
        return nullptr;
    const int below = above->track + 1;
    if (below >= std::max(trackCount(), 1) || trackFromY(y) != below)
        return nullptr;
    if (timeline_.trackKind(below) == Timeline::TrackKind::Audio)
        return nullptr;
    const double f = xToFrame((double)x);
    if (f < (double)above->timelineStart || f >= (double)above->end())
        return nullptr;
    // The whole slot has to be free, else the drop would ripple that row apart.
    for (const Sequence& s : timeline_.sequences)
        for (const Clip& c : s.clips)
            if (c.track == below && c.timelineStart < above->end() && c.end() > above->timelineStart)
                return nullptr;
    return above;
}

// Line a dropped version up with `ref` frame-for-frame. Two versions of the same
// shot need not be numbered alike — a comp may be rendered over 1010-1020 where
// its plate runs 1001-1030 — so the match is made on absolute (editorial) frame
// numbers, not on the raw sourceOffset: taking the reference's offset as-is would
// land the plate on 1001-1011.
//
// Fills srcIn/srcDur with the range in `dst`'s own 0-based indexing, and
// startShift with the frames to push the clip's timeline start by when `dst`
// begins after the reference's in point and so cannot supply the head. Returns
// false when the two don't overlap at all, leaving the caller to place the whole
// source as before.
bool App::alignedSourceRange(const Clip& ref, Media& dst,
                             int64_t& srcIn, int64_t& srcDur, int64_t& startShift) {
    // The first frame number (e.g. 1001) is a property of an image sequence's file
    // names, which only its open decoder knows; video and audio are 0-based.
    auto firstFrameOf = [](Media& m) -> int64_t {
        if (m.type() != ClipType::ImageSequence)
            return 0;
        std::string err;
        if (auto src = m.ensureOpen(err))
            return src->firstFrameNumber();
        return 0;
    };
    auto refMedia = timeline_.findMediaById(ref.mediaId);
    if (!refMedia)
        return false;

    const int64_t count = std::max<int64_t>(dst.info().frameCount, 1);
    int64_t in  = firstFrameOf(*refMedia) + ref.sourceOffset - firstFrameOf(dst);
    int64_t dur = ref.duration;
    startShift = 0;
    if (in < 0) {          // dst starts later: it has no frames for the head
        startShift = -in;
        dur += in;
        in = 0;
    }
    if (in >= count || dur <= 0)
        return false;      // the ranges miss each other entirely
    srcIn  = in;
    srcDur = std::min(dur, count - in); // a shorter version yields a shorter clip
    return true;
}

// Released without moving: the press was a plain click after all, so do what
// clicking the commit row has always done — swap the clip's media, which lands a
// different source under the playhead, so re-describe from scratch afterwards.
void App::commitClipSourcePick() {
    clipSourceDragArmed_ = false;
    clipSourceMarked_.clear();
    clipSourceMarkAnchor_.clear();
    startPickerCommit(panelCascade_, clipSourceDragKey_, clipSourceDragValue_,
                      [this] { clipSourceDirty_ = true; });
}

// Up / Down with the panel open: step the commit (version) section and swap, which
// is the click path with the row picked by the key instead of the cursor. Only that
// section — the pickers above it navigate the cascade rather than replace anything,
// and there is no focus in this panel to say which one an arrow would mean.
void App::stepClipSourceVersion(int dir) {
    if (panelCascade_.loading)
        return; // a query is resolving; its result decides what the rows are
    std::vector<PickerColumn> picks = buildPickerColumns(clipSourceStates_, panelCascade_);
    const PickerColumn* commit = nullptr;
    for (const PickerColumn& p : picks)
        if (p.commit)
            commit = &p;
    if (!commit || commit->options.empty())
        return;
    // Nothing checked (the clip's own version isn't among the options) puts the
    // first press on the near end of the list.
    const int cur = commit->checkedIdx;
    const int next = cur < 0 ? (dir > 0 ? 0 : (int)commit->options.size() - 1) : cur + dir;
    if (next < 0 || next >= (int)commit->options.size())
        return; // at the end of the list; stay on the current version
    clipSourceDragKey_ = commit->key;
    clipSourceDragValue_ = commit->options[next].value;
    clipSourceRevealVersion_ = true; // the row may be scrolled out of the pane
    commitClipSourcePick();
}

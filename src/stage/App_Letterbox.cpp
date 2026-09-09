// Letterbox matte: masks the program image to a target aspect ratio with black
// bars (top/bottom when the target is wider than the image, left/right when
// narrower). This translation unit owns the "Letterbox" toolbar popup (aspect
// presets + custom entry + Fit View + opacity slider) and the bar geometry/rendering shared
// by the player, the review monitor, and the external (NDI/SDI) output. The chosen
// ratio/opacity live on the Timeline, so they persist per-project (see Project.cpp).
// The toolbar button that opens the popup is drawn in App_TopBar.cpp.
// All members of App; split out of App.cpp for the same reasons as the other App_*.

#include "App.h"
#include "AppInternal.h"
#include "Layout.h"
#include "TopBarStyle.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

using namespace jplay;

namespace {

// Aspect-ratio presets offered by the popup (label + target W/H). "None" (0)
// disables the matte. Order = top-to-bottom in the popup.
struct Preset { const char* label; double ratio; };
const Preset kPresets[] = {
    { "None",   0.0 },
    { "2.39:1", 2.39 },
    { "2.35:1", 2.35 },
    { "1.85:1", 1.85 },
    { "16:9",   16.0 / 9.0 },
    { "1.66:1", 1.66 },
    { "4:3",    4.0 / 3.0 },
    { "1:1",    1.0 },
    { "9:16",   9.0 / 16.0 },
};
constexpr int kNumPresets = (int)(sizeof(kPresets) / sizeof(kPresets[0]));

// A preset row is "current" when its ratio matches the active one (None matches
// when the matte is off).
bool presetIsCurrent(const Preset& p, double active) {
    if (p.ratio <= 0.0)
        return active <= 0.0;
    return active > 0.0 && std::fabs(p.ratio - active) < 1e-3;
}

// Parse a custom ratio: "W:H" / "W/H" or a bare decimal ("2.39"). 0 = invalid.
double parseRatio(const std::string& s) {
    size_t a = s.find_first_not_of(" \t");
    if (a == std::string::npos)
        return 0.0;
    size_t b = s.find_last_not_of(" \t");
    std::string t = s.substr(a, b - a + 1);
    size_t sep = t.find(':');
    if (sep == std::string::npos)
        sep = t.find('/');
    if (sep != std::string::npos) {
        double num = SDL_atof(t.substr(0, sep).c_str());
        double den = SDL_atof(t.substr(sep + 1).c_str());
        return (num > 0.0 && den > 0.0) ? num / den : 0.0;
    }
    double v = SDL_atof(t.c_str());
    return v > 0.0 ? v : 0.0;
}

// Editable decimal for the custom field (empty when the matte is off).
std::string formatRatio(double r) {
    if (r <= 0.0)
        return "";
    char buf[24];
    SDL_snprintf(buf, sizeof(buf), "%.4g", r);
    return buf;
}

} // namespace

// ─── Geometry (shared by all three sinks) ────────────────────────────────────

int App::letterboxBars(const SDL_FRect& c, SDL_FRect out[2]) const {
    const double R = timeline_.letterboxRatio;
    if (R <= 0.0 || c.w <= 0.0f || c.h <= 0.0f)
        return 0;
    const double contentAR = (double)c.w / (double)c.h;
    const double eps = 1e-3;
    if (R > contentAR + eps) {
        // Target wider than the image → horizontal bars, top and bottom.
        float barH = (float)(((double)c.h - (double)c.w / R) * 0.5);
        if (barH <= 0.5f)
            return 0;
        out[0] = { c.x, c.y, c.w, barH };
        out[1] = { c.x, c.y + c.h - barH, c.w, barH };
        return 2;
    }
    if (R < contentAR - eps) {
        // Target narrower → vertical bars, left and right (pillarbox).
        float barW = (float)(((double)c.w - (double)c.h * R) * 0.5);
        if (barW <= 0.5f)
            return 0;
        out[0] = { c.x, c.y, barW, c.h };
        out[1] = { c.x + c.w - barW, c.y, barW, c.h };
        return 2;
    }
    return 0;
}

void App::drawLetterbox(SDL_Renderer* r, const SDL_FRect& content) const {
    SDL_FRect bars[2];
    int n = letterboxBars(content, bars);
    if (n == 0)
        return;
    Uint8 a = (Uint8)std::lround(std::clamp(timeline_.letterboxOpacity, 0.0f, 1.0f) * 255.0f);
    SDL_SetRenderDrawColor(r, 0, 0, 0, a);
    for (int i = 0; i < n; ++i)
        jplay::fillRect(r, &bars[i]);
}

const uint8_t* App::letterboxForOutput(const uint8_t* px, int w, int h) {
    if (!px || !letterboxActive() || w <= 0 || h <= 0)
        return px;
    SDL_FRect bars[2];
    int n = letterboxBars({ 0.0f, 0.0f, (float)w, (float)h }, bars);
    if (n == 0)
        return px;
    letterboxOutBuf_.assign(px, px + (size_t)w * h * 4);
    uint8_t* buf = letterboxOutBuf_.data();
    const float op = std::clamp(timeline_.letterboxOpacity, 0.0f, 1.0f);
    const float keep = 1.0f - op; // src scaled toward black; alpha left untouched
    for (int b = 0; b < n; ++b) {
        int x0 = std::clamp((int)std::floor(bars[b].x), 0, w);
        int y0 = std::clamp((int)std::floor(bars[b].y), 0, h);
        int x1 = std::clamp((int)std::ceil(bars[b].x + bars[b].w), 0, w);
        int y1 = std::clamp((int)std::ceil(bars[b].y + bars[b].h), 0, h);
        for (int y = y0; y < y1; ++y) {
            uint8_t* row = buf + ((size_t)y * w + x0) * 4;
            for (int x = x0; x < x1; ++x) {
                row[0] = (uint8_t)(row[0] * keep);
                row[1] = (uint8_t)(row[1] * keep);
                row[2] = (uint8_t)(row[2] * keep);
                row += 4;
            }
        }
    }
    return buf;
}

// Same matte, applied to the scRGB half program image. The bars scale toward black
// exactly as above; in linear light that is the physically correct dimming, so the
// same `keep` factor applies without any encode/decode.
const Imath::half* App::letterboxForOutput(const Imath::half* px, int w, int h) {
    if (!px || !letterboxActive() || w <= 0 || h <= 0)
        return px;
    SDL_FRect bars[2];
    int n = letterboxBars({ 0.0f, 0.0f, (float)w, (float)h }, bars);
    if (n == 0)
        return px;
    letterboxOutBufHdr_.assign(px, px + (size_t)w * h * 4);
    Imath::half* buf = letterboxOutBufHdr_.data();
    const float op = std::clamp(timeline_.letterboxOpacity, 0.0f, 1.0f);
    const float keep = 1.0f - op;
    for (int b = 0; b < n; ++b) {
        int x0 = std::clamp((int)std::floor(bars[b].x), 0, w);
        int y0 = std::clamp((int)std::floor(bars[b].y), 0, h);
        int x1 = std::clamp((int)std::ceil(bars[b].x + bars[b].w), 0, w);
        int y1 = std::clamp((int)std::ceil(bars[b].y + bars[b].h), 0, h);
        for (int y = y0; y < y1; ++y) {
            Imath::half* row = buf + ((size_t)y * w + x0) * 4;
            for (int x = x0; x < x1; ++x) {
                row[0] = (float)row[0] * keep;
                row[1] = (float)row[1] * keep;
                row[2] = (float)row[2] * keep;
                row += 4;
            }
        }
    }
    return buf;
}

// ─── State ───────────────────────────────────────────────────────────────────

void App::openLetterboxMenu() {
    letterboxMenuOpen_ = !letterboxMenuOpen_; // the button toggles the popup
    letterboxHoverRow_ = -1;
    letterboxFitHover_ = false;
    letterboxOpacityDrag_ = false;
    if (!letterboxMenuOpen_ && letterboxFld_.focused()) {
        letterboxFld_.setFocus(false);
        SDL_StopTextInput(window_);
    }
}

// The active matte, named: the matching preset's label, or the bare decimal for a
// custom ratio. Feeds the toolbar tooltip, which is where the ratio is stated now
// that the button that opens this popup is icon-only.
std::string App::letterboxLabel() const {
    if (!letterboxActive())
        return "off";
    for (const Preset& p : kPresets)
        if (presetIsCurrent(p, timeline_.letterboxRatio))
            return p.label; // "None" never matches here: it is the inactive case above
    return formatRatio(timeline_.letterboxRatio);
}

void App::setLetterboxRatio(double ratio) {
    timeline_.letterboxRatio = ratio > 0.0 ? ratio : 0.0;
    // Re-push the program frame so the external output re-bakes its bars even while
    // parked on a static frame (the GUI + review draw their bars live each frame).
    hasTexture_ = false;
}

// ─── Popup ─────────────────────────────────────────────────────────────────

void App::renderLetterboxMenu() {
    if (!letterboxMenuOpen_)
        return;

    const float pad = 10.0f * dpiScale;
    const float lineH = textFont_.lineHeight();
    const float rowH = lineH + 6.0f * dpiScale;
    const float fieldH = 20.0f * dpiScale;
    const float trackH = 6.0f * dpiScale;
    const float gap = 8.0f * dpiScale;
    const float checkW = 16.0f * dpiScale; // reserved gutter left of every label
    const float mark = 6.0f * dpiScale;    // filled dot marking the active preset
    const float chk = 12.0f * dpiScale;    // "Fit View" checkbox box, in the same gutter

    // Panel width: widest of the header, preset rows, custom row, Fit View, opacity.
    float custLabelW = textFont_.measure(renderer_, "Custom");
    float contentW = textFont_.measure(renderer_, "LETTERBOX");
    for (const auto& p : kPresets)
        contentW = std::max(contentW, checkW + textFont_.measure(renderer_, p.label));
    contentW = std::max(contentW, custLabelW + gap + 80.0f * dpiScale);
    contentW = std::max(contentW, checkW + textFont_.measure(renderer_, "Fit View"));
    contentW = std::max(contentW, textFont_.measure(renderer_, "Opacity  100%"));
    float panelW = std::max(contentW + pad * 2.0f, 170.0f * dpiScale);

    // Heights of the stacked blocks.
    float presetsH = kNumPresets * rowH;
    float opacityH = lineH + 6.0f * dpiScale + trackH + 6.0f * dpiScale;
    float totalH = pad + lineH + gap + presetsH + gap + fieldH + gap + rowH + gap +
                   opacityH + pad;

    // Anchor the panel's top at the button's bottom edge; grow downward (the button
    // now lives in the top toolbar). Clamp within the window on the sides/bottom.
    float panelX = letterboxBtnRect_.x;
    float panelY = letterboxBtnRect_.y + letterboxBtnRect_.h;
    if (panelX + panelW > winW_ - 4.0f)
        panelX = winW_ - panelW - 4.0f;
    panelX = std::max(panelX, 4.0f);
    if (panelY + totalH > winH_ - 4.0f)
        panelY = winH_ - totalH - 4.0f;
    panelY = std::max(panelY, letterboxBtnRect_.y + letterboxBtnRect_.h);
    letterboxMenuRect_ = { panelX, panelY, panelW, totalH };

    jplay::drawPopupPanel(renderer_, letterboxMenuRect_);

    // Walk the panel top-down. The body keeps the panel's full width, since the
    // preset highlight runs edge to edge; the pad is taken per block instead.
    SDL_FRect body = letterboxMenuRect_;
    gapTop(body, pad);
    SDL_FRect hdr = cutTop(body, lineH);
    drawText(hdr.x + pad, hdr.y, kHeader, "LETTERBOX");
    gapTop(body, gap);

    // Preset rows.
    letterboxRowRects_.resize(kNumPresets);
    for (int i = 0; i < kNumPresets; ++i) {
        SDL_FRect row = cutTop(body, rowH);
        row = inset(row, 2.0f, 0.0f);
        letterboxRowRects_[i] = row;
        bool current = presetIsCurrent(kPresets[i], timeline_.letterboxRatio);
        if (i == letterboxHoverRow_) {
            jplay::drawRowHover(renderer_, row);
        } else if (current) {
            setColor(renderer_, kCurrent);
            jplay::fillRect(renderer_, &row);
        }
        // Marker gutter, then the label in what is left of the row.
        SDL_FRect inner = { panelX + pad, row.y, panelW - pad * 2.0f, row.h };
        SDL_FRect gutter = cutLeft(inner, checkW);
        if (current) {
            gapLeft(gutter, 2.0f);
            SDL_FRect mkCol = cutLeft(gutter, mark);
            SDL_FRect mk = centerV(mkCol, mark);
            setColor(renderer_, kCheck);
            jplay::fillRect(renderer_, &mk);
        }
        SDL_FRect lbl = centerV(inner, lineH);
        drawText(lbl.x, lbl.y, kLabel, kPresets[i].label);
    }
    gapTop(body, gap);

    // Custom-ratio row: label + editable field. Keep the field synced to the
    // current ratio unless the user is typing in it.
    SDL_FRect crow = cutTop(body, fieldH);
    SDL_FRect cinner = inset(crow, pad, 0.0f);
    SDL_FRect clabel = cutLeft(cinner, custLabelW);
    SDL_FRect ctext = centerV(clabel, lineH);
    drawText(ctext.x, ctext.y, kLabel, "Custom");
    gapLeft(cinner, gap);
    letterboxFldBox_ = cinner;
    letterboxFld_.setRect(letterboxFldBox_);
    if (!letterboxFld_.focused())
        letterboxFld_.setText(formatRatio(timeline_.letterboxRatio));
    letterboxFld_.render(renderer_, &textFont_);
    gapTop(body, gap);

    // "Fit View" checkbox: zooms the view so the bars fall outside it. The box sits
    // in the same gutter the preset markers use, so the label lines up with theirs.
    SDL_FRect frow = cutTop(body, rowH);
    frow = inset(frow, 2.0f, 0.0f);
    letterboxFitRowRect_ = frow;
    if (letterboxFitHover_)
        jplay::drawRowHover(renderer_, frow);
    SDL_FRect finner = { panelX + pad, frow.y, panelW - pad * 2.0f, frow.h };
    SDL_FRect fgutter = cutLeft(finner, checkW);
    SDL_FRect fbox = centerV(cutLeft(fgutter, chk), chk);
    if (letterboxFitView_) {
        setColor(renderer_, kCheck);
        jplay::fillRect(renderer_, &fbox);
    }
    setColor(renderer_, kPanelBorder);
    jplay::drawRect(renderer_, &fbox);
    SDL_FRect flbl = centerV(finner, lineH);
    drawText(flbl.x, flbl.y, kLabel, "Fit View");
    gapTop(body, gap);

    // Opacity row: label + percentage, slider below.
    char ob[16];
    SDL_snprintf(ob, sizeof(ob), "%.0f%%",
                 std::clamp(timeline_.letterboxOpacity, 0.0f, 1.0f) * 100.0f);
    SDL_FRect orow = cutTop(body, lineH);
    SDL_FRect oinner = inset(orow, pad, 0.0f);
    SDL_FRect ovalue = cutRight(oinner, textFont_.measure(renderer_, ob));
    drawText(oinner.x, oinner.y, kLabel, "Opacity");
    drawText(ovalue.x, ovalue.y, kValue, ob);
    gapTop(body, 6.0f * dpiScale);
    SDL_FRect trow = cutTop(body, trackH);
    SDL_FRect track = inset(trow, pad, 0.0f);
    // Store a slightly taller hit region than the drawn track for easier grabbing.
    letterboxOpacityRect_ = { track.x, track.y - 4.0f * dpiScale, track.w,
                              trackH + 8.0f * dpiScale };
    setColor(renderer_, SDL_Color{ 42, 45, 52, 255 });
    jplay::fillRect(renderer_, &track);
    float t = std::clamp(timeline_.letterboxOpacity, 0.0f, 1.0f);
    float hx = track.x + t * track.w;
    SDL_FRect fill = { track.x, track.y, hx - track.x, track.h };
    setColor(renderer_, SDL_Color{ 90, 120, 180, 255 });
    jplay::fillRect(renderer_, &fill);
    setColor(renderer_, SDL_Color{ 70, 74, 84, 255 });
    jplay::drawRect(renderer_, &track);
    SDL_FRect knob = { hx - 3.0f, track.y - 3.0f, 6.0f, track.h + 6.0f };
    setColor(renderer_, SDL_Color{ 210, 214, 222, 255 });
    jplay::fillRect(renderer_, &knob);
}

bool App::letterboxMenuHandleEvent(const SDL_Event& e) {
    auto closeMenu = [&]() {
        letterboxMenuOpen_ = false;
        letterboxOpacityDrag_ = false;
        letterboxHoverRow_ = -1;
        letterboxFitHover_ = false;
        if (letterboxFld_.focused()) {
            letterboxFld_.setFocus(false);
            SDL_StopTextInput(window_);
        }
    };
    auto commitField = [&]() {
        double r = parseRatio(letterboxFld_.text());
        if (r > 0.0)
            setLetterboxRatio(r);
        letterboxFld_.setFocus(false);
        SDL_StopTextInput(window_);
    };
    auto applyOpacity = [&](float mx) {
        float t = std::clamp((mx - letterboxOpacityRect_.x) / letterboxOpacityRect_.w, 0.0f, 1.0f);
        timeline_.letterboxOpacity = t;
        hasTexture_ = false; // re-bake bars for the external output
    };

    switch (e.type) {
    case SDL_EVENT_MOUSE_BUTTON_DOWN: {
        if (e.button.button != SDL_BUTTON_LEFT) {
            closeMenu();
            return true;
        }
        float mx = e.button.x, my = e.button.y;
        // Clicking the Letterbox button itself: let it fall through so the button
        // handler toggles the popup closed.
        if (inRect(letterboxBtnRect_, mx, my))
            return false;
        if (!inRect(letterboxMenuRect_, mx, my)) {
            closeMenu();
            return true; // outside click just dismisses; don't also scrub
        }
        // In a sync session the matte is host-driven, exactly like the zoom/pan it
        // feeds (Fit View decides which rectangle the framing is relative to). A
        // spectator can read the popup but not touch it — one guard on the whole
        // panel, which also keeps the custom field from ever taking focus.
        if (transportLocked()) {
            spectatorLocked("CHANGE THE MATTE");
            return true;
        }
        // Preset rows.
        for (int i = 0; i < (int)letterboxRowRects_.size(); ++i) {
            if (inRect(letterboxRowRects_[i], mx, my)) {
                setLetterboxRatio(kPresets[i].ratio);
                if (letterboxFld_.focused()) {
                    letterboxFld_.setFocus(false);
                    SDL_StopTextInput(window_);
                }
                return true;
            }
        }
        // Custom field.
        if (inRect(letterboxFldBox_, mx, my)) {
            if (!letterboxFld_.focused()) {
                letterboxFld_.setText(formatRatio(timeline_.letterboxRatio));
                letterboxFld_.setFocus(true);
                SDL_StartTextInput(window_);
            }
            letterboxFld_.handleEvent(e); // place the caret
            return true;
        }
        // Fit View checkbox. Only the fit scale changes, so the current zoom/pan
        // carry over and the external output keeps its baked bars.
        if (inRect(letterboxFitRowRect_, mx, my)) {
            letterboxFitView_ = !letterboxFitView_;
            if (letterboxFld_.focused())
                commitField();
            return true;
        }
        // Opacity slider.
        if (inRect(letterboxOpacityRect_, mx, my)) {
            letterboxOpacityDrag_ = true;
            applyOpacity(mx);
            return true;
        }
        // Empty area inside the panel: commit/blur the field if focused.
        if (letterboxFld_.focused())
            commitField();
        return true;
    }
    case SDL_EVENT_MOUSE_MOTION: {
        float mx = e.motion.x, my = e.motion.y;
        if (letterboxOpacityDrag_) {
            applyOpacity(mx);
            return true;
        }
        letterboxHoverRow_ = -1;
        for (int i = 0; i < (int)letterboxRowRects_.size(); ++i)
            if (inRect(letterboxRowRects_[i], mx, my)) {
                letterboxHoverRow_ = i;
                break;
            }
        letterboxFitHover_ = inRect(letterboxFitRowRect_, mx, my);
        return inRect(letterboxMenuRect_, mx, my);
    }
    case SDL_EVENT_MOUSE_BUTTON_UP:
        if (letterboxOpacityDrag_) {
            letterboxOpacityDrag_ = false;
            return true;
        }
        return false;
    case SDL_EVENT_TEXT_INPUT:
        if (letterboxFld_.focused()) {
            letterboxFld_.handleEvent(e);
            return true;
        }
        return false;
    case SDL_EVENT_KEY_DOWN:
        if (e.key.key == SDLK_ESCAPE) {
            closeMenu();
            return true;
        }
        if (letterboxFld_.focused()) {
            if (e.key.key == SDLK_RETURN || e.key.key == SDLK_KP_ENTER) {
                commitField();
                return true;
            }
            letterboxFld_.handleEvent(e);
            return true;
        }
        return false;
    default:
        return false;
    }
}

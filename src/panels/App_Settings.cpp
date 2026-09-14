// Settings panel: a left-expand pane (mutually exclusive with the other left
// panes) holding two sections — "PROJECT SETTINGS" (frame rate, colour management)
// and "JPLAY SETTINGS", the user preferences, which are themselves grouped into
// sub-sections (General / Audio / Playback / Advanced). App members, split out
// of App.cpp.
//
// Toggle icon (named here so the font subsetter includes its glyph):
//   ICON_MDI_COG

#include "App.h"
#include "AppInternal.h"
#include "Layout.h"
#include "MenuDraw.h"
#include "PythonStartup.h"
#include "SkinColors.h"

#include <cmath>

using namespace jplay;

namespace {

// ---- settings palette
// Static colors used by renderSettingsPanel(). The two-segment toggle fills are
// computed per active/inactive state (ternaries on the channel values) and stay
// inline at their draw sites; the segment text picks between kSegText / kSegTextOff.
const SDL_Color& kHeader = jplay::colors().header;  // section headers
const SDL_Color& kSection= jplay::colors().section; // sub-section headers
const SDL_Color& kLabel  = jplay::colors().label;   // row labels
// Settings panel fills with the shared panel colour (jplay::kPanelBg), unskinned,
// so it reads the same as the explorer/grade panels that share its slot.
const SDL_Color& kDivider     = jplay::colors().divider;      // panel edge line
const SDL_Color& kToggleBorder= jplay::colors().borderStrong; // toggle border + segment separator
const SDL_Color& kSegText     = jplay::colors().text;         // active segment label
const SDL_Color& kSegTextOff  = jplay::colors().labelOff;     // inactive segment label

inline void setColor(SDL_Renderer* r, SDL_Color c) {
    SDL_SetRenderDrawColor(r, c.r, c.g, c.b, c.a);
}

// Standard delivery frame rates offered by the FPS picker. Fractional rates use
// their exact NTSC values so a round-trip through the picker is loss-free.
struct Rate { const char* label; double value; };
const Rate kRates[] = {
    { "23.976",  24000.0 / 1001.0 },
    { "24",      24.0 },
    { "25",      25.0 },
    { "29.97",   30000.0 / 1001.0 },
    { "30",      30.0 },
    { "47.952",  48000.0 / 1001.0 },
    { "48",      48.0 },
    { "50",      50.0 },
    { "59.94",   60000.0 / 1001.0 },
    { "60",      60.0 },
    { "72",      72.0 },
    { "96",      96.0 },
    { "100",     100.0 },
    { "119.88",  120000.0 / 1001.0 },
    { "120",     120.0 },
};
constexpr int kNumRates = (int)(sizeof(kRates) / sizeof(kRates[0]));

constexpr float kFpsRowH     = 20.0f;
constexpr float kFpsLabelGap =  6.0f;
constexpr float kFpsInnerPad =  8.0f;
constexpr float kFpsCaretW   = 14.0f;

// ---- row metrics
// The panel is a vertical stack cut off its content rect (see Layout.h): each
// control takes a row of one of these heights, followed by a named gap.
constexpr float kFieldH   = 20.0f; // editable field (FPS, cache size)
constexpr float kToggleH  = 20.0f; // two-segment toggle
constexpr float kSegPad   =  8.0f; // horizontal padding inside a toggle segment
constexpr float kChkSize  = 14.0f; // checkbox box
constexpr float kChkGap   =  6.0f; // checkbox box -> label
constexpr float kLabelGap =  6.0f; // control label -> the control it names
constexpr float kGroupGap = 16.0f; // between controls
constexpr float kHeaderGap= 12.0f; // section header -> first control
constexpr float kSubGap   =  8.0f; // sub-section header -> first control
constexpr float kSubBreak = 14.0f; // last control -> next sub-section header

// Label for the given rate: a standard rate's canonical string, else a trimmed
// decimal of the raw value (covers non-standard rates from a loaded project).
std::string fpsLabel(double f) {
    for (const auto& r : kRates)
        if (std::abs(f - r.value) < 0.01)
            return r.label;
    char buf[32];
    SDL_snprintf(buf, sizeof(buf), "%.3f", f);
    std::string s = buf;
    while (s.find('.') != std::string::npos && (s.back() == '0' || s.back() == '.'))
        s.pop_back();
    return s;
}

SDL_FRect fpsListRect(const SDL_FRect& box) {
    return { box.x, box.y + box.h, box.w, kFpsRowH * kNumRates };
}

// ---- Decode threads
// Worker counts the picker offers: Auto (0) leads, then the common pool widths.
// A count outside the list can still be typed into the field, as with FPS.
const int kDecodeThreads[] = { 0, 1, 2, 4, 6, 8, 12, 16, 24, 32 };
constexpr int kNumDecodeThreads = (int)(sizeof(kDecodeThreads) / sizeof(kDecodeThreads[0]));

std::string decodeThreadLabel(int n) {
    return n > 0 ? std::to_string(n) : std::string("Auto");
}

// ---- UI Scale
// Device pixels per logical unit the whole interface is drawn at. 0 = Auto: take the
// display's own scale and keep following it when the window moves to another monitor.
// The fixed steps are the common Windows display-scaling values; picking one pins the
// scale, which is also how the high-DPI look can be checked on a 1x monitor.
struct UiScale { const char* label; float value; };
const UiScale kUiScales[] = {
    { "Auto", 0.00f },
    { "100%", 1.00f },
    { "125%", 1.25f },
    { "150%", 1.50f },
    { "175%", 1.75f },
    { "200%", 2.00f },
    { "250%", 2.50f },
    { "300%", 3.00f },
};
constexpr int kNumUiScales = (int)(sizeof(kUiScales) / sizeof(kUiScales[0]));

// Text for the closed picker. Auto also reports what it resolved to, since "Auto"
// alone doesn't say whether this display asked for 100% or 200%. `pref` is the stored
// preference (0 = Auto), `active` the scale actually in force.
std::string uiScaleText(float pref, float active) {
    char buf[32];
    if (pref <= 0.0f) {
        SDL_snprintf(buf, sizeof(buf), "Auto (%d%%)", (int)std::lround(active * 100.0f));
        return buf;
    }
    for (const auto& s : kUiScales)
        if (s.value > 0.0f && std::abs(pref - s.value) < 0.001f)
            return s.label;
    SDL_snprintf(buf, sizeof(buf), "%d%%", (int)std::lround(pref * 100.0f)); // hand-edited value
    return buf;
}

// The picker's option list shares the FPS list's row height and style so the two
// read as one control.
SDL_FRect uiScaleListRect(const SDL_FRect& box) {
    return { box.x, box.y + box.h, box.w, kFpsRowH * kNumUiScales };
}

// ── Controls ─────────────────────────────────────────────────────────────────
// Each takes the row rect the panel cut for it, draws itself left-aligned in
// that row, and returns its hit region for the event handler to store. Sizing
// happens here only, so the handler never re-measures a label.

// Two-segment toggle; `active` is the lit segment. The returned hit rects tile
// the control: the 1px separator column belongs to the right segment.
SegToggle segToggle(SDL_Renderer* r, TextFont* font, const SDL_FRect& row,
                    const char* labelA, const char* labelB, int active) {
    const float w0 = font->measure(r, labelA) + kSegPad * 2.0f;
    const float w1 = font->measure(r, labelB) + kSegPad * 2.0f;
    SDL_FRect track = row;
    const SDL_FRect box = cutLeft(track, w0 + w1 + 1.0f);

    SDL_FRect fills = box;
    const SDL_FRect f0 = cutLeft(fills, w0);
    cutLeft(fills, 1.0f);                  // separator column
    const SDL_FRect f1 = cutLeft(fills, w1);

    // The two segments share one outline plus a separator column between them.
    const float glyphH = font->lineHeight();
    for (int i = 0; i < 2; ++i) {
        const SDL_FRect& rect = (i == 0) ? f0 : f1;
        const bool on = (active == i);
        SDL_SetRenderDrawColor(r, on ? 55 : 38, on ? 78 : 39, on ? 130 : 43, 255);
        jplay::fillRect(r, &rect);
        font->draw(r, rect.x + kSegPad, centerV(rect, glyphH).y,
                   on ? kSegText : kSegTextOff, (i == 0) ? labelA : labelB);
    }
    setColor(r, kToggleBorder);
    jplay::drawRect(r, &box);
    jplay::drawLine(r, f1.x - 1.0f, box.y + 1.0f, f1.x - 1.0f, box.y + box.h - 1.0f);

    SegToggle t;
    t.seg[0] = f0;
    t.seg[1] = { f1.x - 1.0f, f1.y, f1.w + 1.0f, f1.h };
    return t;
}

// Checkbox + label. Returns the clickable rect (box and label together).
SDL_FRect checkbox(SDL_Renderer* r, TextFont* font, const SDL_FRect& row,
                   const char* label, bool checked) {
    SDL_FRect band = row;
    const SDL_FRect box = centerV(cutLeft(band, kChkSize), kChkSize);
    if (checked) {
        setColor(r, jplay::colors().accent);
        jplay::fillRect(r, &box);
    }
    setColor(r, kToggleBorder);
    jplay::drawRect(r, &box);
    if (checked) {
        const SDL_FRect inner = inset(box, 3.0f, 3.0f);
        setColor(r, jplay::colors().text);
        jplay::fillRect(r, &inner);
    }
    font->draw(r, row.x + kChkSize + kChkGap, row.y, checked ? kSegText : kLabel, label);
    return { row.x, box.y, kChkSize + kChkGap + font->measure(r, label), kChkSize };
}

} // namespace

void App::buildSettingsBar() {
    // FPS field requires no setup; font and rect are supplied at render time.
}

// ─── FPS editable combo ───────────────────────────────────────────────────────
bool App::settingsFpsHandleEvent(const SDL_Event& e) {
    switch (e.type) {
    case SDL_EVENT_MOUSE_BUTTON_DOWN: {
        if (e.button.button != SDL_BUTTON_LEFT) {
            if (fpsFldListOpen_) { fpsFldListOpen_ = false; return true; }
            return false;
        }
        if (decodeThreadsListOpen_)
            return false; // its list is drawn over these rows; let it have the click
        float mx = e.button.x, my = e.button.y;
        if (inRect(fpsFldBox_, mx, my)) {
            if (!fpsFld_.focused()) {
                cacheGbFld_.setFocus(false); // only one field focused at a time
                decodeThreadsFld_.setFocus(false);
                fpsFld_.setText(fpsLabel(timeline_.fps));
                fpsFld_.setFocus(true);
                SDL_StartTextInput(window_);
            }
            fpsFldListOpen_ = !fpsFldListOpen_;
            fpsFldListHover_ = -1;
            return true;
        }
        if (fpsFldListOpen_) {
            SDL_FRect lr = fpsListRect(fpsFldBox_);
            if (inRect(lr, mx, my)) {
                int row = (int)((my - lr.y) / kFpsRowH);
                if (row < 0) row = 0;
                if (row >= kNumRates) row = kNumRates - 1;
                timeline_.fps = kRates[row].value;
                fpsFld_.setFocus(false);
                fpsFldListOpen_ = false;
                SDL_StopTextInput(window_);
                return true;
            }
            fpsFldListOpen_ = false;
        }
        if (fpsFld_.focused()) {
            fpsFld_.setFocus(false);
            SDL_StopTextInput(window_);
        }
        return false;
    }
    case SDL_EVENT_MOUSE_MOTION:
        if (fpsFldListOpen_) {
            SDL_FRect lr = fpsListRect(fpsFldBox_);
            fpsFldListHover_ = inRect(lr, e.motion.x, e.motion.y)
                ? (int)((e.motion.y - lr.y) / kFpsRowH) : -1;
            return true;
        }
        return false;
    case SDL_EVENT_TEXT_INPUT:
        if (fpsFld_.focused()) { fpsFld_.handleEvent(e); return true; }
        return false;
    case SDL_EVENT_KEY_DOWN:
        if (fpsFld_.focused()) {
            if (e.key.key == SDLK_RETURN || e.key.key == SDLK_KP_ENTER) {
                double v = SDL_atof(fpsFld_.text().c_str());
                if (v > 0.0) timeline_.fps = v;
                fpsFld_.setFocus(false);
                fpsFldListOpen_ = false;
                SDL_StopTextInput(window_);
                return true;
            }
            if (e.key.key == SDLK_ESCAPE) {
                fpsFld_.setFocus(false);
                fpsFldListOpen_ = false;
                SDL_StopTextInput(window_);
                return true;
            }
            fpsFld_.handleEvent(e);
            return true;
        }
        if (fpsFldListOpen_ && e.key.key == SDLK_ESCAPE) {
            fpsFldListOpen_ = false;
            return true;
        }
        return false;
    default:
        return false;
    }
}

// ─── UI Scale picker ─────────────────────────────────────────────────────────
// Fixed option list, no text entry. A pick applies immediately: setUiScale()
// re-rasterizes the glyph atlases for the new density and computeLayout() re-derives
// the logical size from it on the next frame.
bool App::settingsUiScaleHandleEvent(const SDL_Event& e) {
    switch (e.type) {
    case SDL_EVENT_MOUSE_BUTTON_DOWN: {
        if (e.button.button != SDL_BUTTON_LEFT) {
            if (uiScaleListOpen_) { uiScaleListOpen_ = false; return true; }
            return false;
        }
        if (decodeThreadsListOpen_)
            return false; // as above: the open decode-threads list owns the click
        float mx = e.button.x, my = e.button.y;
        if (inRect(uiScaleBox_, mx, my)) {
            if (fpsFld_.focused() || cacheGbFld_.focused() ||
                decodeThreadsFld_.focused()) { // one control at a time
                fpsFld_.setFocus(false);
                cacheGbFld_.setFocus(false);
                decodeThreadsFld_.setFocus(false);
                SDL_StopTextInput(window_);
            }
            fpsFldListOpen_ = false;
            decodeThreadsListOpen_ = false;
            uiScaleListOpen_ = !uiScaleListOpen_;
            uiScaleListHover_ = -1;
            return true;
        }
        if (uiScaleListOpen_) {
            SDL_FRect lr = uiScaleListRect(uiScaleBox_);
            uiScaleListOpen_ = false;
            if (inRect(lr, mx, my)) {
                int row = std::clamp((int)((my - lr.y) / kFpsRowH), 0, kNumUiScales - 1);
                uiScalePref_ = kUiScales[row].value;
                float target = uiScalePref_;
                if (target <= 0.0f) { // Auto: hand the display's own scale back over
                    target = SDL_GetWindowDisplayScale(window_);
                    if (target <= 0.0f) target = 1.0f;
                }
                setUiScale(target);
                writePrefs();
                return true;
            }
        }
        return false;
    }
    case SDL_EVENT_MOUSE_MOTION:
        if (uiScaleListOpen_) {
            SDL_FRect lr = uiScaleListRect(uiScaleBox_);
            uiScaleListHover_ = inRect(lr, e.motion.x, e.motion.y)
                ? (int)((e.motion.y - lr.y) / kFpsRowH) : -1;
            return true;
        }
        return false;
    case SDL_EVENT_KEY_DOWN:
        if (e.key.key == SDLK_ESCAPE && uiScaleListOpen_) {
            uiScaleListOpen_ = false;
            return true;
        }
        return false;
    default:
        return false;
    }
}

// ─── Numeric editable fields (Playback Cache, Decode Threads) ───────────────
// Click to focus, Enter or a click away commits, Escape abandons the edit. The
// two share that plumbing here and differ only in their commits; Decode Threads
// is additionally a picker, so it also carries the dropdown handling that
// settingsFpsHandleEvent has.
bool App::settingsCacheHandleEvent(const SDL_Event& e) {
    // In the order they are laid out, which is the order they commit in when a
    // click moves focus from one to the other.
    TextInput* const  flds[]  = { &cacheGbFld_, &decodeThreadsFld_ };
    const SDL_FRect* const boxes[] = { &cacheGbFldBox_, &decodeThreadsFldBox_ };
    constexpr int kNumFlds = 2;

    // The pool is spawned once, at startup, so this run keeps the width it was
    // built with however this lands. Say so rather than leave the control reading
    // like it took effect.
    auto setDecodeThreads = [&](int n) {
        decodeThreads_ = n;
        setStatus(n > 0 ? "DECODE THREADS: " + std::to_string(n) + " (RESTART TO APPLY)"
                        : "DECODE THREADS: AUTO (RESTART TO APPLY)",
                  4000);
    };

    auto commit = [&](int i) {
        if (i == 0) {
            double v = SDL_atof(cacheGbFld_.text().c_str());
            cacheGb_ = (float)std::clamp(v, 0.25, 128.0);
            if (cache_)
                cache_->setMaxBytes((size_t)(cacheGb_ * (1024.0 * 1024.0 * 1024.0)));
        } else {
            // Anything that is not a positive count reads as Auto -- an emptied
            // field, and the "Auto (n)" the field itself shows when it is on Auto.
            int v = SDL_atoi(decodeThreadsFld_.text().c_str());
            setDecodeThreads(v > 0 ? std::clamp(v, 1, 64) : 0);
        }
        flds[i]->setFocus(false);
        SDL_StopTextInput(window_);
        writePrefs();
    };
    auto focusedField = [&]() {
        for (int i = 0; i < kNumFlds; ++i)
            if (flds[i]->focused())
                return i;
        return -1;
    };

    switch (e.type) {
    case SDL_EVENT_MOUSE_BUTTON_DOWN: {
        if (e.button.button != SDL_BUTTON_LEFT) {
            if (decodeThreadsListOpen_) { decodeThreadsListOpen_ = false; return true; }
            return false;
        }
        float mx = e.button.x, my = e.button.y;
        for (int i = 0; i < kNumFlds; ++i) {
            if (!inRect(*boxes[i], mx, my))
                continue;
            if (!flds[i]->focused()) {
                fpsFld_.setFocus(false); // only one field focused at a time
                fpsFldListOpen_ = false;
                uiScaleListOpen_ = false;
                const int prev = focusedField();
                if (prev >= 0)
                    commit(prev); // moving between the two commits the one left
                flds[i]->setFocus(true);
                SDL_StartTextInput(window_);
            }
            // Decode Threads is a combo: the click that focuses it also works the
            // picker, as the FPS field's does.
            decodeThreadsListOpen_ = (i == 1) ? !decodeThreadsListOpen_ : false;
            decodeThreadsListHover_ = -1;
            return true;
        }
        if (decodeThreadsListOpen_) {
            decodeThreadsListOpen_ = false;
            if (inRect(decodeThreadsListRect_, mx, my)) {
                const int row = std::clamp(
                    (int)((my - decodeThreadsListRect_.y) / kFpsRowH), 0, kNumDecodeThreads - 1);
                setDecodeThreads(kDecodeThreads[row]);
                decodeThreadsFld_.setFocus(false);
                SDL_StopTextInput(window_);
                writePrefs();
                return true;
            }
        }
        const int f = focusedField();
        if (f >= 0) {
            commit(f);    // clicking away commits the typed value
            return false; // let the click also hit whatever it landed on
        }
        return false;
    }
    case SDL_EVENT_MOUSE_MOTION:
        if (decodeThreadsListOpen_) {
            decodeThreadsListHover_ = inRect(decodeThreadsListRect_, e.motion.x, e.motion.y)
                ? (int)((e.motion.y - decodeThreadsListRect_.y) / kFpsRowH) : -1;
            return true;
        }
        return false;
    case SDL_EVENT_TEXT_INPUT: {
        const int f = focusedField();
        if (f < 0)
            return false;
        flds[f]->handleEvent(e);
        return true;
    }
    case SDL_EVENT_KEY_DOWN: {
        const int f = focusedField();
        if (f < 0) {
            if (decodeThreadsListOpen_ && e.key.key == SDLK_ESCAPE) {
                decodeThreadsListOpen_ = false;
                return true;
            }
            return false;
        }
        if (e.key.key == SDLK_RETURN || e.key.key == SDLK_KP_ENTER) {
            decodeThreadsListOpen_ = false;
            commit(f);
            return true;
        }
        if (e.key.key == SDLK_ESCAPE) {
            decodeThreadsListOpen_ = false;
            flds[f]->setFocus(false);
            SDL_StopTextInput(window_);
            return true;
        }
        flds[f]->handleEvent(e);
        return true;
    }
    default:
        return false;
    }
}

// ─── Panel ───────────────────────────────────────────────────────────────────
void App::renderSettingsPanel() {
    if (!panelOpen(kPanelSettings)) {
        if (fpsFld_.focused()) {
            fpsFld_.setFocus(false);
            fpsFldListOpen_ = false;
            SDL_StopTextInput(window_);
        }
        if (cacheGbFld_.focused()) {
            cacheGbFld_.setFocus(false);
            SDL_StopTextInput(window_);
        }
        if (decodeThreadsFld_.focused()) {
            decodeThreadsFld_.setFocus(false);
            SDL_StopTextInput(window_);
        }
        uiScaleListOpen_ = false;
        decodeThreadsListOpen_ = false;
        return;
    }
    SDL_FRect panel = beginLeftPanel();

    const float pad = 10.0f * dpiScale;
    const float line = textFont_.lineHeight();

    // The panel body is a vertical stack: every control cuts its row off the top
    // of `body`, so no row needs to know its own y. Text rows are `line` tall,
    // controls take their own height, and the gaps between them are named.
    // The stack is taller than the panel on a short window, so it is laid out
    // into an unbounded rect offset by settingsScroll_ and clipped to `view`,
    // with a scrollbar down the right edge (as the grade panel, which shares this
    // slot, does). The strip the bar occupies is kept clear whether or not the bar
    // is showing, so a control's width doesn't change as the window is resized.
    SDL_FRect view = inset(panel, pad, 0.0f);
    gapTop(view, 8.0f);
    const SDL_Rect clip = { (int)panel.x, (int)view.y, (int)panel.w, (int)view.h };
    SDL_SetRenderClipRect(renderer_, &clip);
    const float contentTop = view.y - settingsScroll_;
    SDL_FRect body = { view.x, contentTop, view.w, kUnbounded };
    gapRight(body, scrollbarStripW(dpiScale, false));

    // A one-line text row (section header or a label naming the control below it).
    auto textRow = [&](SDL_Color c, const char* s) {
        SDL_FRect row = cutTop(body, line);
        drawText(row.x, row.y, c, s);
    };

    // A sub-section header inside JPLAY SETTINGS: the group name followed by a
    // hairline out to the panel edge, so the groups read one level below the
    // "JPLAY SETTINGS" header rather than as peers of it.
    auto subHeader = [&](const char* s) {
        SDL_FRect row = cutTop(body, line);
        drawText(row.x, row.y, kSection, s);
        const float x0 = row.x + textFont_.measure(renderer_, s) + 8.0f;
        const float y  = std::floor(row.y + line * 0.5f) + 0.5f;
        if (x0 < row.x + row.w) {
            setColor(renderer_, kDivider);
            jplay::drawLine(renderer_, x0, y, row.x + row.w, y);
        }
        gapTop(body, kSubGap);
    };

    // ── PROJECT ──
    textRow(kHeader, "PROJECT SETTINGS");
    gapTop(body, kHeaderGap);

    // FPS row: lay out the box here; field + dropdown drawn last so the open
    // list overlays the panel content below.
    {
        SDL_FRect row = cutTop(body, kFieldH);
        SDL_FRect label = cutLeft(row, textFont_.measure(renderer_, "FPS:") + kFpsLabelGap);
        fpsFldBox_ = row;
        fpsFld_.setRect(fpsFldBox_);
        if (!fpsFld_.focused())
            fpsFld_.setText(fpsLabel(timeline_.fps));
        drawText(label.x, centerV(label, line).y, kLabel, "FPS:");
    }
    gapTop(body, kGroupGap);

    // Color Management: OCIO display transform / built-in linear→sRGB fallback.
    // Reflects ocio_.isEnabled(); persisted per-project via timeline_.ocioEnabled.
    textRow(kLabel, "Color Management:");
    gapTop(body, kLabelGap);
    colorMgmtToggle_ = segToggle(renderer_, &textFont_, cutTop(body, kToggleH),
                                 "OCIO", "sRGB", ocio_.isEnabled() ? 0 : 1);
    gapTop(body, 24.0f); // break before the JPLAY section

    // ── JPLAY SETTINGS ──
    // The user preferences, grouped into sub-sections. Every control still writes
    // straight to its App member and calls writePrefs(); the grouping is layout only.
    textRow(kHeader, "JPLAY SETTINGS");
    gapTop(body, kHeaderGap);

    auto chkRow = [&](const char* label, bool checked) {
        return checkbox(renderer_, &textFont_, cutTop(body, line), label, checked);
    };

    // ── General ──
    subHeader("General");

    // UI Scale row: the box is laid out here, then drawn (with its open list) further
    // down so the list overlays the rows below rather than being painted under them.
    {
        const char* uiScaleLbl = "UI Scale:";
        SDL_FRect row = cutTop(body, kFieldH);
        SDL_FRect label = cutLeft(row, textFont_.measure(renderer_, uiScaleLbl) + kFpsLabelGap);
        uiScaleBox_ = row;
        drawText(label.x, centerV(label, line).y, kLabel, uiScaleLbl);
    }
    gapTop(body, 12.0f);

    warnUnsavedRect_ = chkRow("Warn on Unsaved Changes", warnUnsaved_);
    gapTop(body, 10.0f);
    // Gates the top-bar Proxy dropdown (see App_ProxyMenu.cpp); off, the button
    // is not laid out and playback always decodes the nominal (Full) file.
    proxyEnabledRect_ = chkRow("Use Proxy Media", proxyEnabled_);
    gapTop(body, kSubBreak);

    // (Time Format, Frame Numbering and the frame-preview thumbnail — the whole
    // Timeline group — live in the top-level "View" menu; see buildMenu.)

    // ── Audio ──
    // (Audio Scrubbing lives in the "Output" menu; see populateOutputMenu.)
    subHeader("Audio");
    attachAudioToSeqRect_ = chkRow("Attach Audio to Image Sequence", attachAudioToSeq_);
    gapTop(body, kSubBreak);

    // ── Playback ──
    subHeader("Playback");

    // Frame Cache budget (GiB). The cache fills with decoded frames up to this
    // size, then evicts the frames farthest from the playhead. Inline field
    // mirroring the FPS row; no dropdown, so it's drawn here rather than last.
    {
        const char* cacheLabel = "Playback Cache (GB):";
        SDL_FRect row = cutTop(body, kFieldH);
        SDL_FRect label = cutLeft(row, textFont_.measure(renderer_, cacheLabel) + kFpsLabelGap);
        cacheGbFldBox_ = row;
        cacheGbFld_.setRect(cacheGbFldBox_);
        if (!cacheGbFld_.focused()) {
            char buf[32];
            SDL_snprintf(buf, sizeof(buf), "%.2f", cacheGb_);
            cacheGbFld_.setText(buf);
        }
        drawText(label.x, centerV(label, line).y, kLabel, cacheLabel);
        cacheGbFld_.render(renderer_, &textFont_);
    }
    gapTop(body, kGroupGap);

    // Decode workers the frame cache runs. How many frames a second can be readied
    // is this wide, which is what caps playback on heavy media (a 4K EXR decode is
    // hundreds of ms of mostly-inflate work). The pool is spawned at startup, so a
    // change is stored and takes effect on the next launch. Auto reports the count
    // it resolved to as well as the word, since "Auto" alone doesn't say what this
    // machine settled on. Editable combo like the FPS row: the box is laid out
    // here and drawn further down so its open list overlays the rows below.
    {
        const char* thrLabel = "Decode Threads:";
        SDL_FRect row = cutTop(body, kFieldH);
        SDL_FRect label = cutLeft(row, textFont_.measure(renderer_, thrLabel) + kFpsLabelGap);
        decodeThreadsFldBox_ = row;
        decodeThreadsFld_.setRect(decodeThreadsFldBox_);
        if (!decodeThreadsFld_.focused()) {
            char buf[32];
            if (decodeThreads_ > 0)
                SDL_snprintf(buf, sizeof(buf), "%d", decodeThreads_);
            else
                SDL_snprintf(buf, sizeof(buf), "Auto (%d)", decodeThreadsActive_);
            decodeThreadsFld_.setText(buf);
        }
        drawText(label.x, centerV(label, line).y, kLabel, thrLabel);
    }
    gapTop(body, kSubBreak);

    // (HDR output enable + reference-white controls live at the top of the
    // "Output" menu — see populateOutputMenu.)

    // ── Advanced ──
    subHeader("Advanced");
    // Opens/closes the loopback port an MCP server talks to. Off on a fresh
    // install so nothing binds a socket (and Windows raises no firewall prompt).
    // Mirror of the sync panel's NETWORK master switch — same syncNetwork_ member,
    // same toggleSyncNetwork(), so the two rows never disagree.
    syncNetworkRect_ = chkRow("Enable Sync Review Socket", syncNetwork_);
    gapTop(body, 10.0f);
    mcpEnabledRect_ = chkRow("MCP Control Channel", mcpEnabled_);
    gapTop(body, 10.0f);
    pythonDebugRect_ = chkRow("Debug Logging", pythonDebug_);

    // (External output — NDI / review monitor — now lives in the top-level
    // "Output" menu; see populateOutputMenu.)

    // The stack is done: measure what it cut off (plus a little room past the last
    // row) and draw the bar. The clamp lands a frame after the layout that needed
    // it, which is what the panel's own render already does elsewhere.
    gapTop(body, 8.0f); // breathing room at the end of the scroll
    settingsContentH_ = body.y - contentTop;
    settingsScroll_ = std::clamp(settingsScroll_, 0.0f,
                                 std::max(0.0f, settingsContentH_ - view.h));
    drawScrollbar(renderer_, view, settingsContentH_, settingsScroll_, dpiScale, true);

    // ── UI Scale picker (box + open list, over the rows laid out above) ──
    // Drawn before the FPS field below, whose own open list drops down across this
    // row: last one drawn wins, and the two are never open at the same time.
    {
        setColor(renderer_, colors().fieldBg); // TextInput's box fill
        jplay::fillRect(renderer_, &uiScaleBox_);
        setColor(renderer_, uiScaleListOpen_ ? colors().focus : colors().border);
        jplay::drawRect(renderer_, &uiScaleBox_);
        const float glyphH = textFont_.lineHeight();
        textFont_.draw(renderer_, uiScaleBox_.x + 6.0f, centerV(uiScaleBox_, glyphH).y,
                       kSegText, uiScaleText(uiScalePref_, uiScale_).c_str());
        const float cx = uiScaleBox_.x + uiScaleBox_.w - kFpsCaretW * 0.5f - 3.0f;
        const float cy = uiScaleBox_.y + uiScaleBox_.h * 0.5f - 1.0f;
        SDL_SetRenderDrawColor(renderer_, 160, 163, 172, 255);
        jplay::drawCaretDown(renderer_, cx, cy);
    }
    if (uiScaleListOpen_) {
        SDL_FRect lr = uiScaleListRect(uiScaleBox_);
        setColor(renderer_, colors().dropBg);
        jplay::fillRect(renderer_, &lr);
        setColor(renderer_, colors().borderStrong);
        jplay::drawRect(renderer_, &lr);
        const float glyphH = textFont_.lineHeight();
        for (int i = 0; i < kNumUiScales; ++i) {
            const float ry = lr.y + i * kFpsRowH;
            const bool hover   = (i == uiScaleListHover_);
            const bool current = std::abs(uiScalePref_ - kUiScales[i].value) < 0.001f;
            const SDL_FRect row{ lr.x + 1.0f, ry, lr.w - 2.0f, kFpsRowH };
            if (hover) {
                jplay::drawRowHover(renderer_, row);
            } else if (current) {
                setColor(renderer_, colors().rowSelected);
                jplay::fillRect(renderer_, &row);
            }
            SDL_Color col = current
                ? SDL_Color{255, 230, 150, 255}
                : SDL_Color{225, 228, 235, 255};
            textFont_.draw(renderer_, lr.x + kFpsInnerPad,
                           ry + (kFpsRowH - glyphH) * 0.5f, col, kUiScales[i].label);
        }
    }

    // ── FPS field (drawn last so its open list overlays the panel body) ──
    fpsFld_.render(renderer_, &textFont_);
    // Chevron: visible when not in text-editing mode to hint the control is also a picker.
    if (!fpsFld_.focused()) {
        float cx = fpsFldBox_.x + fpsFldBox_.w - kFpsCaretW * 0.5f - 3.0f;
        float cy = fpsFldBox_.y + fpsFldBox_.h * 0.5f - 1.0f;
        SDL_SetRenderDrawColor(renderer_, 160, 163, 172, 255);
        jplay::drawCaretDown(renderer_, cx, cy);
    }
    // Dropdown list.
    if (fpsFldListOpen_) {
        SDL_FRect lr = fpsListRect(fpsFldBox_);
        setColor(renderer_, colors().dropBg);
        jplay::fillRect(renderer_, &lr);
        setColor(renderer_, colors().borderStrong);
        jplay::drawRect(renderer_, &lr);
        float glyphH = textFont_.lineHeight();
        for (int i = 0; i < kNumRates; ++i) {
            float ry = lr.y + i * kFpsRowH;
            bool hover   = (i == fpsFldListHover_);
            bool current = std::abs(timeline_.fps - kRates[i].value) < 0.01;
            const SDL_FRect row{ lr.x + 1.0f, ry, lr.w - 2.0f, kFpsRowH };
            if (hover) {
                jplay::drawRowHover(renderer_, row);
            } else if (current) {
                setColor(renderer_, colors().rowSelected);
                jplay::fillRect(renderer_, &row);
            }
            SDL_Color col = current
                ? SDL_Color{255, 230, 150, 255}
                : SDL_Color{225, 228, 235, 255};
            textFont_.draw(renderer_, lr.x + kFpsInnerPad,
                           ry + (kFpsRowH - glyphH) * 0.5f, col, kRates[i].label);
        }
    }

    // ── Decode Threads combo (drawn last, as the FPS field is, so its open list
    // overlays the rows below it) ──
    // The row sits low in the panel, so the list drops up instead when there is
    // no room for it below the box — clipped to `view`, a downward list there
    // would run off the bottom half-drawn and half of it would not be clickable.
    {
        const float listH = kFpsRowH * kNumDecodeThreads;
        const float below  = decodeThreadsFldBox_.y + decodeThreadsFldBox_.h;
        const bool  dropUp = below + listH > view.y + view.h &&
                             decodeThreadsFldBox_.y - listH >= view.y;
        decodeThreadsListRect_ = { decodeThreadsFldBox_.x,
                                   dropUp ? decodeThreadsFldBox_.y - listH : below,
                                   decodeThreadsFldBox_.w, listH };
    }
    decodeThreadsFld_.render(renderer_, &textFont_);
    if (!decodeThreadsFld_.focused()) {
        const float cx = decodeThreadsFldBox_.x + decodeThreadsFldBox_.w - kFpsCaretW * 0.5f - 3.0f;
        const float cy = decodeThreadsFldBox_.y + decodeThreadsFldBox_.h * 0.5f - 1.0f;
        SDL_SetRenderDrawColor(renderer_, 160, 163, 172, 255);
        jplay::drawCaretDown(renderer_, cx, cy);
    }
    if (decodeThreadsListOpen_) {
        const SDL_FRect& lr = decodeThreadsListRect_;
        setColor(renderer_, colors().dropBg);
        jplay::fillRect(renderer_, &lr);
        setColor(renderer_, colors().borderStrong);
        jplay::drawRect(renderer_, &lr);
        const float glyphH = textFont_.lineHeight();
        for (int i = 0; i < kNumDecodeThreads; ++i) {
            const float ry = lr.y + i * kFpsRowH;
            const bool hover   = (i == decodeThreadsListHover_);
            const bool current = (decodeThreads_ == kDecodeThreads[i]);
            const SDL_FRect row{ lr.x + 1.0f, ry, lr.w - 2.0f, kFpsRowH };
            if (hover) {
                jplay::drawRowHover(renderer_, row);
            } else if (current) {
                setColor(renderer_, colors().rowSelected);
                jplay::fillRect(renderer_, &row);
            }
            const SDL_Color col = current
                ? SDL_Color{255, 230, 150, 255}
                : SDL_Color{225, 228, 235, 255};
            textFont_.draw(renderer_, lr.x + kFpsInnerPad,
                           ry + (kFpsRowH - glyphH) * 0.5f, col,
                           decodeThreadLabel(kDecodeThreads[i]).c_str());
        }
    }

    SDL_SetRenderClipRect(renderer_, nullptr);
}

// ─── Event handling ──────────────────────────────────────────────────────────
bool App::settingsHandleEvent(const SDL_Event& e) {
    if (e.type != SDL_EVENT_MOUSE_BUTTON_DOWN || e.button.button != SDL_BUTTON_LEFT)
        return true; // swallow any other click inside the panel
    float mx = e.button.x, my = e.button.y;
    bool changed = false;
    // Segment hit rects come straight from the render pass, so a label change can
    // never desync the split point from what was drawn.
    int seg = -1;
    if ((seg = colorMgmtToggle_.hit(mx, my)) >= 0) {
        bool useOcio = (seg == 0);
        ocio_.setEnabled(useOcio);
        timeline_.ocioEnabled = useOcio; // persisted per-project on save
        // The sRGB default opens no config at all, so the first switch to OCIO in a
        // session is what loads one (from the project's own media, as at startup).
        reinitOcioForFirstSource();
        hasTexture_ = false;             // force a re-render through the new path
        // Thumbnails are unaffected: a tile is the frame's own encoding, not a
        // display rendering, so it stands whichever way this is switched.
        // Color management is a per-project setting, not a user pref; no writePrefs.
    } else if (inRect(pythonDebugRect_, mx, my)) {
        pythonDebug_ = !pythonDebug_;
        jplaySetPythonDebug(pythonDebug_);
        jplaySetDebugLogging(pythonDebug_);
        // The HDR/display diagnostics log each distinct state once. Drop the
        // "already logged" markers so switching this on re-reports the current
        // state instead of staying silent until something else changes.
        hdrDiagSink_.clear();
        hdrDiagReadFmt_ = 0;
        changed = true;
    } else if (inRect(attachAudioToSeqRect_, mx, my)) {
        attachAudioToSeq_ = !attachAudioToSeq_;
        changed = true;
    } else if (inRect(warnUnsavedRect_, mx, my)) {
        warnUnsaved_ = !warnUnsaved_;
        changed = true;
    } else if (inRect(syncNetworkRect_, mx, my)) {
        toggleSyncNetwork(); // persists syncNetwork_ and sets its own status
    } else if (inRect(mcpEnabledRect_, mx, my)) {
        mcpEnabled_ = !mcpEnabled_;
        if (mcpEnabled_) {
            control_.start();
            controlAdvertised_ = false; // re-publish control.json once the bind lands
            setStatus("MCP CONTROL CHANNEL ENABLED");
        } else {
            control_.stop();
            setStatus("MCP CONTROL CHANNEL DISABLED");
        }
        changed = true;
    } else if (inRect(proxyEnabledRect_, mx, my)) {
        proxyEnabled_ = !proxyEnabled_;
        // On, a project that never picked a mode takes the config's default one, so
        // ticking "Use Proxy Media" shows proxy media rather than only offering it.
        adoptDefaultProxyMode();
        applyProxyMode(); // off: forces Full live; on: re-applies timeline_.proxyMode
        if (!proxyEnabled_)
            proxyMenuOpen_ = false; // the button vanishes next layout; close its popup now
        setStatus(proxyEnabled_ ? "PROXY ENABLED" : "PROXY DISABLED");
        changed = true;
    }
    if (changed)
        writePrefs();
    return true;
}

// Persist all UserData preferences from the current App members. Centralised so
// every writer (settings toggles, the SOURCES view/size controls) saves a
// complete, consistent preferences section.
void App::writePrefs() {
    UserData::savePrefs({ timeFormatFrames_, frameNumberingClip_, pythonDebug_,
                          showFramePreview_, snapPlayhead_, audioScrub_, clipWaveform_,
                          attachAudioToSeq_, warnUnsaved_,
                          (int)(peThumbSize_ + 0.5f), (int)peSort_,
                          cacheGb_, decodeThreads_, nitRef_, hdrOutput_,
                          hdrRefWhiteNits_, uiScalePref_,
                          syncNetwork_, syncPort_, mcpEnabled_, proxyEnabled_,
                          (int)(gridThumbH_ + 0.5f), volume_, muted_,
                          frameOverlay_, frameOverlayBottom_, overlaySize_,
                          overlayColor_, compactTimeline_ });
}

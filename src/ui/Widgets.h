#pragma once

#include "TextFont.h"

#include <SDL3/SDL.h>

#include <functional>
#include <string>
#include <vector>

// Reusable immediate-mode UI primitives for SDL3.
// Each widget owns its interaction state and manages its own SDL rect.
// Pattern: host calls handleEvent() then render() each frame.

// ── drawButton ────────────────────────────────────────────────────────────
// Fill + 1px border + centered label: the shape every flat button in the side
// panels shares. The colors are parameters because each panel carries its own
// skin — only the geometry is common. The caller owns the rect and stores it
// for hit-testing; this draws, it does not lay out. A null font draws the box
// alone.
void drawButton(SDL_Renderer* r, TextFont* font, const SDL_FRect& box, const char* label,
                SDL_Color face, SDL_Color border, SDL_Color text);

// ── drawStatusBadge ───────────────────────────────────────────────────────
// A short status word ("Approved", "On hold") as a filled chip pinned to the
// right edge of `row` and vertically centered in it: how a picker option's badge
// from Python draws (see PickerOption). The chip is `color` (packed 0xRRGGBB, 0
// = a neutral grey) and its text is black or white by the chip's own brightness,
// so a site picks one color per status and the label stays readable on it.
// Returns the width it reserved on the right of the row, including the gap to
// the row edge — subtract it from what the row's own label may use. Empty text
// or a null font draws nothing and reserves nothing.
//
// `minChipW` is the chip's floor: pass 0 and each chip is as wide as its own
// word, or pass the widest statusBadgeWidth() over a run of rows and they all
// come out that wide, so the chips line up as a column instead of each ending
// where its status happens to end. The text is centered in the chip either way.
//
// `scale` is the caller's DPI scale (1.0 for the fixed-metric widgets), applied
// to the paddings; the chip's height follows the font.
//
// statusBadgeWidth is the chip alone, without the gap: what one status costs, and
// so the thing to take the max of for `minChipW`. 0 for an empty status.
float statusBadgeWidth(SDL_Renderer* r, TextFont* font, const std::string& text, float scale);
float drawStatusBadge(SDL_Renderer* r, TextFont* font, const SDL_FRect& row,
                      const std::string& text, uint32_t color, float scale,
                      float minChipW = 0.0f);

// The same badge with its word dropped: a small outlined square in the status
// color, right-aligned in `row`, for a view that cannot spend the width a word
// costs (the clip menu's table columns). Reserves the same width whatever the
// status is, so a column no longer widens by the longest status word — hence a
// width that needs neither the text nor the font. Draw it only when there is a
// badge to draw; unlike drawStatusBadge it has no text to test for that.
float statusSwatchWidth(float scale);
float drawStatusSwatch(SDL_Renderer* r, const SDL_FRect& row, uint32_t color, float scale);

// ── drawScrollbar ─────────────────────────────────────────────────────────
// Vertical scroll indicator hugging the right edge of `view`, for `contentH` of
// content scrolled down by `scroll`. Draws nothing when the content fits, which
// is also the "no scrollbar wanted" case, so callers need no guard of their own.
// The width, inset and thumb minimum are fixed here rather than passed: the
// panels used to differ by a pixel or two in each, which read as drift. What
// does differ is the skin — `withTrack` draws the recessed track the docked list
// panels use behind the thumb; the overlay panels (inspector, top-bar popups)
// float the thumb alone over their own background.
void drawScrollbar(SDL_Renderer* r, const SDL_FRect& view, float contentH, float scroll,
                   float dpiScale, bool withTrack);

// The track drawScrollbar puts inside `view`, and the thumb inside that track.
// Split out so a panel whose scrollbar is draggable hit-tests the very geometry
// it drew instead of re-deriving the inset, width and thumb minimum kept above.
// Zero height when the content fits, i.e. when no bar was drawn. A grabbable bar
// is the wider of the two widths — a bar meant to be taken hold of should be easy
// to put the cursor on, where one that only reports the scroll position can stay
// out of the way.
SDL_FRect scrollbarBar(const SDL_FRect& view, float contentH, float dpiScale,
                       bool grabbable = false);
SDL_FRect scrollbarThumb(const SDL_FRect& bar, float contentH, float scroll, float dpiScale);

// How much of `view`'s right edge a scrollbar covers: the bar plus its inset.
// For panels that lay their content out clear of the bar instead of under it.
float scrollbarStripW(float dpiScale, bool grabbable);

// ── ScrollbarDrag ─────────────────────────────────────────────────────────
// Makes one panel's scrollbar grabbable. A panel that wants that keeps an
// instance next to its scroll offset, calls record() with the same arguments it
// passed drawScrollbar (the geometry only exists once the content height is
// known), and routes events to press / drag, clearing `dragging` on mouse-up.
// The offset is held by the panel, not here, so nothing needs to be written back
// and a render is free to clamp or animate it as it already does.
struct ScrollbarDrag {
    SDL_FRect bar{};          // the bar the last render drew; zero height when it drew none
    float contentH = 0.0f;
    bool dragging = false;
    float grabDy = 0.0f;      // where inside the thumb it was taken hold of

    void record(const SDL_FRect& view, float contentH, float dpiScale);
    bool press(float mx, float my, float& scroll, float dpiScale);
    void drag(float my, float& scroll, float dpiScale);
};

// drawScrollbar for a grabbable bar: draws it at the wider width and records what
// it drew in `sb` for next frame's press, so the two can't disagree about where
// the bar is or how wide it was.
void drawScrollbar(SDL_Renderer* r, const SDL_FRect& view, float contentH, float scroll,
                   float dpiScale, bool withTrack, ScrollbarDrag& sb);

// ── TextInput ─────────────────────────────────────────────────────────────
// Single-line editable text field with selection and clipboard support:
// click / click-drag / shift+click to select, Shift+Arrows/Home/End and Ctrl+A
// for keyboard selection, Ctrl+C/X/V copy/cut/paste, and typing / backspace /
// delete over a selection replaces it.
// The host dialog calls SDL_StartTextInput/StopTextInput for the window; the
// widget itself only manages focus, cursor, and selection state. Indices are
// byte offsets (matching the existing UTF-8-agnostic editing).
class TextInput {
public:
    void setRect(const SDL_FRect& r) { rect_ = r; }
    const SDL_FRect& rect() const { return rect_; }

    void setFocus(bool f);
    bool focused() const { return focused_; }

    const std::string& text() const { return text_; }
    void setText(const std::string& t) { text_ = t; cursor_ = (int)t.size(); anchor_ = cursor_; }

    // Returns true when the field content or selection/cursor changed.
    bool handleEvent(const SDL_Event& e);
    void render(SDL_Renderer* r, TextFont* font) const;

private:
    bool hasSel() const { return anchor_ != cursor_; }
    int  selLo()  const { return anchor_ < cursor_ ? anchor_ : cursor_; }
    int  selHi()  const { return anchor_ > cursor_ ? anchor_ : cursor_; }
    void deleteSel();              // erase the selection, collapse cursor to its start
    void insertUtf8(const char* s); // replace any selection, insert s at the cursor
    int  indexAtX(float x) const;   // byte index nearest window-x (uses cached glyphX_)

    SDL_FRect rect_{};
    std::string text_;
    int cursor_ = 0;
    int anchor_ = 0;               // selection anchor; anchor_ == cursor_ means no selection
    bool focused_ = false;
    bool dragging_ = false;        // mouse selection in progress

    // Layout cache for cursor/selection/hit-testing, rebuilt when text_ changes.
    // glyphX_[i] is the x offset (from text start) of byte boundary i; single-char
    // measures keep the font's texture cache bounded to distinct characters.
    mutable std::vector<float> glyphX_;
    mutable std::string lastLayoutText_;
    mutable float textStartX_ = 0.f;
};

// ── SegToggle ─────────────────────────────────────────────────────────────
// Hit rects of a two-segment toggle (the Timecode/Frames, Global/Clip and
// OCIO/sRGB controls in the settings panel). The two segments are sized by
// their labels, so the render pass stores the rects it actually drew and the
// event handler hit-tests those — rather than re-deriving the split from the
// label widths and having the two copies drift apart.
struct SegToggle {
    SDL_FRect seg[2]{};

    // Index of the segment under (x, y), or -1 if the point is outside.
    int hit(float x, float y) const {
        for (int i = 0; i < 2; ++i) {
            const SDL_FRect& r = seg[i];
            if (x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h)
                return i;
        }
        return -1;
    }
};

// ── RadioGroup ────────────────────────────────────────────────────────────
// Mutually exclusive labelled radio buttons.
class RadioGroup {
public:
    void setOptions(std::vector<std::string> labels);
    int selected() const { return selected_; }
    void setSelected(int i) { selected_ = i; }
    int count() const { return (int)labels_.size(); }
    // Returns the bounding rect of option i (valid after layoutHorizontal).
    SDL_FRect optionRect(int i) const {
        return (i >= 0 && i < (int)rects_.size()) ? rects_[i] : SDL_FRect{};
    }

    // Lay options out horizontally from (x, y), row height rowH, gap between items.
    // Returns total width used.
    float layoutHorizontal(float x, float y, float rowH, float gap,
                           TextFont* font, SDL_Renderer* r);

    // Returns true when selection changed.
    bool handleEvent(const SDL_Event& e);
    void render(SDL_Renderer* r, TextFont* font) const;

private:
    std::vector<std::string> labels_;
    std::vector<SDL_FRect>   rects_;   // bounding box for each option (circle + label)
    int selected_ = 0;
};

// ── Button ────────────────────────────────────────────────────────────────
// Labelled push-button. handleEvent returns true when clicked.
class Button {
public:
    void setRect(const SDL_FRect& r) { rect_ = r; }
    const SDL_FRect& rect() const { return rect_; }
    void setLabel(const std::string& l) { label_ = l; }
    void setEnabled(bool e) { enabled_ = e; }

    bool handleEvent(const SDL_Event& e);
    void render(SDL_Renderer* r, TextFont* font) const;

private:
    SDL_FRect rect_{};
    std::string label_;
    bool hovered_ = false;
    bool enabled_ = true;
};

// ── Combobox ──────────────────────────────────────────────────────────────
// Standalone single-selection dropdown.
// Call render() to draw the closed box, then renderDropdown() for the open
// list — separate so the host can draw all boxes first, then all open lists
// on top, avoiding Z-order issues.
class Combobox {
public:
    void setRect(const SDL_FRect& r) { rect_ = r; }
    const SDL_FRect& rect() const { return rect_; }
    void setOptions(std::vector<std::string> options, int selected = 0);
    int selected() const { return selected_; }
    const std::string& value() const;
    bool isOpen() const { return open_; }
    bool empty() const { return options_.empty(); }

    // Returns true when event was consumed or selection changed.
    bool handleEvent(const SDL_Event& e);
    // Draw the closed-box portion only (no list).
    void render(SDL_Renderer* r, TextFont* font) const;
    // Draw the open dropdown list (no-op if closed). Call after all render()s.
    void renderDropdown(SDL_Renderer* r, TextFont* font) const;

private:
    SDL_FRect rect_{};
    std::vector<std::string> options_;
    int selected_ = 0;
    bool open_ = false;
    int hoverRow_ = -1;

    SDL_FRect listRect() const;
};

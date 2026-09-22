#include "Widgets.h"

#include "Layout.h"
#include "MenuDraw.h"
#include "PixelSnap.h"
#include "SkinColors.h"

#include <algorithm>
#include <cstdio>

// ─── Shared colours ──────────────────────────────────────────────────────────
// References into the live skin palette (see jplay::colors()), so a skin change
// retints these without touching any of the use sites below.
static const SDL_Color& kColText    = jplay::colors().text;
static const SDL_Color& kColDim     = jplay::colors().textDim;
static const SDL_Color& kColBorder  = jplay::colors().border;
static const SDL_Color& kColBorderStrong = jplay::colors().borderStrong;
static const SDL_Color& kColFocus   = jplay::colors().focus;
static const SDL_Color& kColBg      = jplay::colors().fieldBg;
static const SDL_Color& kColDropBg  = jplay::colors().dropBg;
static const SDL_Color& kColBtnBg   = jplay::colors().btnBg;
// The radio dot is the same accent as a focused border, and the palette has no
// separate entry for it.
static const SDL_Color& kColRadio   = jplay::colors().focus;
static const SDL_Color& kColSbTrack = jplay::colors().sbTrack; // recessed scrollbar groove
static const SDL_Color& kColSbThumb = jplay::colors().sbThumb; // thumb inside that groove
static const SDL_Color& kColSbBare  = jplay::colors().sbBare;  // thumb with no groove behind it
// Not in the palette: no skin token names the primary-button fill.
static constexpr SDL_Color kColBtnPri  = { 55,  85, 148, 255};

static inline bool inR(const SDL_FRect& r, float x, float y) {
    return x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h;
}
static inline void setColor(SDL_Renderer* r, SDL_Color c) {
    SDL_SetRenderDrawColor(r, c.r, c.g, c.b, c.a);
}

// ─── drawButton ──────────────────────────────────────────────────────────────

// Label centered in `box`. Split out so the flat and skinned button paths share
// one copy of the centering rather than drifting apart by a pixel.
static void drawCenteredLabel(SDL_Renderer* r, TextFont* font, const SDL_FRect& box,
                              const char* label, SDL_Color text) {
    if (!font) return;
    const float tw = font->measure(r, label);
    const float th = font->lineHeight();
    font->draw(r, box.x + (box.w - tw) * 0.5f, box.y + (box.h - th) * 0.5f, text, label);
}

// Deliberately unskinned: the five callers outside this file each pass their own
// palette (grade tabs, project-explorer, sync, clip ranges), so binding it to one
// skin surface would silently retint all of them. Widgets that map onto a specific
// surface — Button below — draw that surface themselves.
void drawButton(SDL_Renderer* r, TextFont* font, const SDL_FRect& box, const char* label,
                SDL_Color face, SDL_Color border, SDL_Color text) {
    setColor(r, face);
    jplay::fillRect(r, &box);
    setColor(r, border);
    jplay::drawRect(r, &box);
    drawCenteredLabel(r, font, box, label, text);
}

// ─── drawStatusBadge ─────────────────────────────────────────────────────────

namespace {
constexpr float kBadgePadX   = 5.0f; // chip's own width either side of the text
constexpr float kBadgePadY   = 1.0f; // chip growth above/below the glyph box
constexpr float kBadgeInset  = 6.0f; // chip's / swatch's right edge to the row's
constexpr float kSwatchSide  = 9.0f; // wordless swatch: square, this per side
constexpr SDL_Color kBadgeBg { 92, 96, 106, 255 }; // chip fill when Python gave none

// The chip's fill, or the neutral grey when Python gave no color.
SDL_Color badgeFill(uint32_t color) {
    if (!color)
        return kBadgeBg;
    return { (Uint8)((color >> 16) & 0xFF), (Uint8)((color >> 8) & 0xFF),
             (Uint8)(color & 0xFF), 255 };
}
} // namespace

float statusBadgeWidth(SDL_Renderer* r, TextFont* font, const std::string& text, float scale) {
    if (text.empty() || !font)
        return 0.0f;
    return font->measure(r, text.c_str()) + 2.0f * kBadgePadX * scale;
}

float drawStatusBadge(SDL_Renderer* r, TextFont* font, const SDL_FRect& row,
                      const std::string& text, uint32_t color, float scale,
                      float minChipW) {
    if (text.empty() || !font)
        return 0.0f;
    const float chipW = std::max(statusBadgeWidth(r, font, text, scale), minChipW);
    const float chipH = font->lineHeight() + 2.0f * kBadgePadY * scale;
    const SDL_FRect chip = { row.x + row.w - kBadgeInset * scale - chipW,
                             row.y + (row.h - chipH) * 0.5f, chipW, chipH };
    const SDL_Color bg = badgeFill(color);
    setColor(r, bg);
    jplay::fillRect(r, &chip);
    // Rec.601 luma: dark chips take white text, bright ones black, so a site can
    // pick its status colors without also having to pick a readable text color.
    const float luma = 0.299f * bg.r + 0.587f * bg.g + 0.114f * bg.b;
    const SDL_Color fg = luma > 140.0f ? SDL_Color{ 0, 0, 0, 255 }
                                       : SDL_Color{ 255, 255, 255, 255 };
    drawCenteredLabel(r, font, chip, text.c_str(), fg);
    return chipW + kBadgeInset * scale;
}

float statusSwatchWidth(float scale) {
    return (kSwatchSide + kBadgeInset) * scale;
}

float drawStatusSwatch(SDL_Renderer* r, const SDL_FRect& row, uint32_t color, float scale) {
    const float side = kSwatchSide * scale;
    const SDL_FRect sw = { row.x + row.w - kBadgeInset * scale - side,
                           row.y + (row.h - side) * 0.5f, side, side };
    setColor(r, badgeFill(color));
    jplay::fillRect(r, &sw);
    // Outlined, unlike the chip: nine pixels of a dark status color would all but
    // vanish against the row it sits on, and the border is what keeps it a swatch.
    setColor(r, kColBorderStrong);
    jplay::drawRect(r, &sw);
    return side + kBadgeInset * scale;
}

// ─── drawScrollbar ───────────────────────────────────────────────────────────

static constexpr float kSbInset = 2.0f; // gap between the bar and the right edge
static constexpr float kSbW      = 4.0f; // a bar that only reports the scroll position
static constexpr float kSbGrabW  = 9.0f; // one that is meant to be grabbed

SDL_FRect scrollbarBar(const SDL_FRect& view, float contentH, float dpiScale,
                       bool grabbable) {
    if (view.h <= 0.0f || contentH <= view.h)
        return { view.x + view.w, view.y, 0.0f, 0.0f }; // content fits: no bar
    SDL_FRect strip = view;
    jplay::gapRight(strip, kSbInset * dpiScale);
    return jplay::cutRight(strip, (grabbable ? kSbGrabW : kSbW) * dpiScale);
}

float scrollbarStripW(float dpiScale, bool grabbable) {
    return (kSbInset + (grabbable ? kSbGrabW : kSbW)) * dpiScale;
}

SDL_FRect scrollbarThumb(const SDL_FRect& bar, float contentH, float scroll, float dpiScale) {
    // Cap the thumb minimum against the bar itself: a short viewport would
    // otherwise be given a thumb taller than the bar it slides in.
    const float minH = std::min(20.0f * dpiScale, bar.h * 0.5f);
    return jplay::scrollThumb(bar, contentH, scroll, minH);
}

void drawScrollbar(SDL_Renderer* r, const SDL_FRect& view, float contentH, float scroll,
                   float dpiScale, bool withTrack) {
    const SDL_FRect bar = scrollbarBar(view, contentH, dpiScale);
    if (bar.h <= 0.0f)
        return; // content fits: no bar
    if (withTrack) {
        setColor(r, kColSbTrack);
        jplay::fillRect(r, &bar);
    }
    const SDL_FRect thumb = scrollbarThumb(bar, contentH, scroll, dpiScale);
    setColor(r, withTrack ? kColSbThumb : kColSbBare);
    jplay::fillRect(r, &thumb);
}

void ScrollbarDrag::record(const SDL_FRect& view, float contentHeight, float dpiScale) {
    bar = scrollbarBar(view, contentHeight, dpiScale, true);
    contentH = contentHeight;
}

void drawScrollbar(SDL_Renderer* r, const SDL_FRect& view, float contentH, float scroll,
                   float dpiScale, bool withTrack, ScrollbarDrag& sb) {
    sb.record(view, contentH, dpiScale);
    if (sb.bar.h <= 0.0f)
        return; // content fits: no bar
    if (withTrack) {
        setColor(r, kColSbTrack);
        jplay::fillRect(r, &sb.bar);
    }
    const SDL_FRect thumb = scrollbarThumb(sb.bar, contentH, scroll, dpiScale);
    setColor(r, withTrack ? kColSbThumb : kColSbBare);
    jplay::fillRect(r, &thumb);
}

// A press on the thumb takes hold of it where it was grabbed; one on the track
// above or below centres the thumb under the cursor and is then held the same
// way, so a click on the track jumps there and can carry on dragging from it.
// The bar is grown a little for the hit test, taking in its inset and a couple of
// pixels of the content beside it. False when the press missed the bar (or there
// is none), leaving the press to whatever wanted it next.
bool ScrollbarDrag::press(float mx, float my, float& scroll, float dpiScale) {
    if (bar.h <= 0.0f)
        return false;
    if (mx < bar.x - 3.0f * dpiScale || mx > bar.x + bar.w + kSbInset * dpiScale ||
        my < bar.y || my > bar.y + bar.h)
        return false;
    const SDL_FRect thumb = scrollbarThumb(bar, contentH, scroll, dpiScale);
    grabDy = (my >= thumb.y && my <= thumb.y + thumb.h) ? my - thumb.y : thumb.h * 0.5f;
    dragging = true;
    drag(my, scroll, dpiScale);
    return true;
}

// Where the thumb's top sits in its travel is where the scroll sits in its own.
// The thumb height doesn't move with the scroll, so recomputing it here against
// the current offset is safe. The panel's own render clamps the result again.
void ScrollbarDrag::drag(float my, float& scroll, float dpiScale) {
    const SDL_FRect thumb = scrollbarThumb(bar, contentH, scroll, dpiScale);
    const float travel = bar.h - thumb.h;
    if (travel <= 0.0f)
        return;
    const float t = std::clamp((my - grabDy - bar.y) / travel, 0.0f, 1.0f);
    scroll = t * (contentH - bar.h);
}

// ─── TextInput ───────────────────────────────────────────────────────────────

static const SDL_Color& kColSel = jplay::colors().selection; // selection highlight

void TextInput::setFocus(bool f) {
    focused_ = f;
    if (!f) { dragging_ = false; anchor_ = cursor_; } // collapse selection on blur; keep text
}

void TextInput::deleteSel() {
    int lo = selLo(), hi = selHi();
    text_.erase((size_t)lo, (size_t)(hi - lo));
    cursor_ = lo;
    anchor_ = lo;
}

void TextInput::insertUtf8(const char* s) {
    if (hasSel()) deleteSel();
    int n = (int)SDL_strlen(s);
    text_.insert((size_t)cursor_, s);
    cursor_ += n;
    anchor_ = cursor_;
}

int TextInput::indexAtX(float x) const {
    if (glyphX_.size() < 2) return 0;
    float rel = x - textStartX_;
    if (rel <= 0.f) return 0;
    for (size_t i = 0; i + 1 < glyphX_.size(); ++i) {
        float mid = (glyphX_[i] + glyphX_[i + 1]) * .5f;
        if (rel < mid) return (int)i;
    }
    return (int)glyphX_.size() - 1;
}

bool TextInput::handleEvent(const SDL_Event& e) {
    // ── Mouse: focus, cursor placement, and drag-selection ──
    if (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN && e.button.button == SDL_BUTTON_LEFT) {
        bool inside = inR(rect_, e.button.x, e.button.y);
        focused_ = inside;
        if (!inside) { dragging_ = false; return false; }
        int idx = std::min(indexAtX(e.button.x), (int)text_.size());
        bool shift = (SDL_GetModState() & SDL_KMOD_SHIFT) != 0;
        cursor_ = idx;
        if (!shift) anchor_ = idx;   // shift+click extends the existing selection
        dragging_ = true;
        return true;
    }
    if (e.type == SDL_EVENT_MOUSE_MOTION && dragging_) {
        cursor_ = std::min(indexAtX(e.motion.x), (int)text_.size());
        return true;
    }
    if (e.type == SDL_EVENT_MOUSE_BUTTON_UP && e.button.button == SDL_BUTTON_LEFT) {
        if (dragging_) { dragging_ = false; return true; }
        return false;
    }

    if (!focused_) return false;

    if (e.type == SDL_EVENT_TEXT_INPUT) {
        insertUtf8(e.text.text);
        return true;
    }

    if (e.type == SDL_EVENT_KEY_DOWN) {
        const bool shift = (e.key.mod & SDL_KMOD_SHIFT) != 0;
        const bool ctrl  = (e.key.mod & SDL_KMOD_CTRL) != 0;

        if (ctrl) {
            switch (e.key.key) {
            case SDLK_A: // select all
                anchor_ = 0; cursor_ = (int)text_.size();
                return true;
            case SDLK_C: // copy
                if (hasSel()) SDL_SetClipboardText(text_.substr((size_t)selLo(), (size_t)(selHi() - selLo())).c_str());
                return true;
            case SDLK_X: // cut
                if (hasSel()) {
                    SDL_SetClipboardText(text_.substr((size_t)selLo(), (size_t)(selHi() - selLo())).c_str());
                    deleteSel();
                }
                return true;
            case SDLK_V: // paste (single-line: truncate at the first newline)
                if (char* clip = SDL_GetClipboardText()) {
                    std::string s(clip);
                    SDL_free(clip);
                    if (size_t nl = s.find_first_of("\r\n"); nl != std::string::npos) s.resize(nl);
                    if (!s.empty()) insertUtf8(s.c_str());
                }
                return true;
            default: break;
            }
        }

        switch (e.key.key) {
        case SDLK_BACKSPACE:
            if (hasSel()) deleteSel();
            else if (cursor_ > 0) { text_.erase((size_t)--cursor_, 1); anchor_ = cursor_; }
            return true;
        case SDLK_DELETE:
            if (hasSel()) deleteSel();
            else if (cursor_ < (int)text_.size()) text_.erase((size_t)cursor_, 1);
            return true;
        case SDLK_LEFT:
            if (!shift && hasSel()) cursor_ = selLo();
            else if (cursor_ > 0) --cursor_;
            if (!shift) anchor_ = cursor_;
            return true;
        case SDLK_RIGHT:
            if (!shift && hasSel()) cursor_ = selHi();
            else if (cursor_ < (int)text_.size()) ++cursor_;
            if (!shift) anchor_ = cursor_;
            return true;
        case SDLK_HOME:
            cursor_ = 0; if (!shift) anchor_ = cursor_;
            return true;
        case SDLK_END:
            cursor_ = (int)text_.size(); if (!shift) anchor_ = cursor_;
            return true;
        default: break;
        }
    }
    return false;
}

void TextInput::render(SDL_Renderer* r, TextFont* font) const {
    // Background
    setColor(r, kColBg);
    jplay::fillRect(r, &rect_);
    // Border
    setColor(r, focused_ ? kColFocus : kColBorder);
    jplay::drawRect(r, &rect_);

    if (!font) return;

    const float glyphH = font->lineHeight();
    const float ty = rect_.y + (rect_.h - glyphH) * .5f;
    const float padX = 6.f;

    // Rebuild per-character x offsets when the text changes. Single-char measures
    // keep the font's (non-evicting) texture cache bounded to distinct characters.
    if (lastLayoutText_ != text_) {
        lastLayoutText_ = text_;
        glyphX_.assign(text_.size() + 1, 0.f);
        float acc = 0.f;
        for (size_t i = 0; i < text_.size(); ++i) {
            char ch[2] = { text_[i], '\0' };
            acc += font->measure(r, ch);
            glyphX_[i + 1] = acc;
        }
    }
    // Clamp accessor: cursor_/anchor_ can briefly outrun a stale layout.
    auto gx = [&](int i) -> float {
        if (i < 0) i = 0;
        if (i >= (int)glyphX_.size()) i = (int)glyphX_.size() - 1;
        return glyphX_.empty() ? 0.f : glyphX_[i];
    };

    // Scroll text wider than the field so the cursor stays in view, and never
    // leave empty space past the end of the text while it could be showing text.
    const float availW = std::max(0.f, rect_.w - 2.f * padX);
    const float textW  = gx((int)glyphX_.size() - 1);
    const float curX   = gx(cursor_);
    if (curX - scrollX_ > availW) scrollX_ = curX - availW;
    if (curX < scrollX_)          scrollX_ = curX;
    scrollX_ = std::clamp(scrollX_, 0.f, std::max(0.f, textW - availW));
    textStartX_ = rect_.x + padX - scrollX_;

    // Clip the contents to the field's interior (intersected with any clip the
    // host panel already set), so long text doesn't spill past the border.
    SDL_Rect prevClip{};
    const bool hadClip = SDL_RenderClipEnabled(r);
    if (hadClip) SDL_GetRenderClipRect(r, &prevClip);
    SDL_Rect clip{ (int)SDL_floorf(rect_.x + 1.f), (int)SDL_floorf(rect_.y + 1.f),
                   std::max(0, (int)SDL_ceilf(rect_.w - 2.f)), std::max(0, (int)SDL_ceilf(rect_.h - 2.f)) };
    if (hadClip && !SDL_GetRectIntersection(&clip, &prevClip, &clip)) clip.w = clip.h = 0;
    SDL_SetRenderClipRect(r, &clip);

    // Selection highlight, behind the text.
    if (focused_ && hasSel()) {
        float x0 = textStartX_ + gx(selLo());
        float x1 = textStartX_ + gx(selHi());
        SDL_FRect sel{ x0, rect_.y + 2.f, x1 - x0, rect_.h - 4.f };
        setColor(r, kColSel);
        jplay::fillRect(r, &sel);
    }

    font->draw(r, textStartX_, ty, kColText, text_.c_str());

    // Blinking caret (hidden while a selection is active).
    if (focused_ && !hasSel() && (SDL_GetTicks() / 530) % 2 == 0) {
        SDL_FRect cur{ textStartX_ + gx(cursor_), ty, 1.5f, glyphH };
        setColor(r, kColText);
        jplay::fillRect(r, &cur);
    }

    SDL_SetRenderClipRect(r, hadClip ? &prevClip : nullptr);
}

// ─── RadioGroup ──────────────────────────────────────────────────────────────

void RadioGroup::setOptions(std::vector<std::string> labels) {
    labels_ = std::move(labels);
    rects_.assign(labels_.size(), SDL_FRect{});
    selected_ = 0;
}

float RadioGroup::layoutHorizontal(float x, float y, float rowH, float gap,
                                   TextFont* font, SDL_Renderer* r) {
    rects_.resize(labels_.size());
    const float circR = rowH * .35f;   // radius of circle
    const float circD = circR * 2.f;
    const float innerPad = 5.f;        // gap between circle and label
    float cx = x;
    for (int i = 0; i < (int)labels_.size(); ++i) {
        float labelW = font ? font->measure(r, labels_[i].c_str()) : (float)labels_[i].size() * 8.f;
        float w = circD + innerPad + labelW;
        rects_[i] = { cx, y, w, rowH };
        cx += w + gap;
    }
    return cx - x - gap;
}

bool RadioGroup::handleEvent(const SDL_Event& e) {
    if (e.type != SDL_EVENT_MOUSE_BUTTON_DOWN || e.button.button != SDL_BUTTON_LEFT)
        return false;
    for (int i = 0; i < (int)rects_.size(); ++i) {
        if (inR(rects_[i], e.button.x, e.button.y)) {
            if (selected_ != i) { selected_ = i; return true; }
            return false;
        }
    }
    return false;
}

void RadioGroup::render(SDL_Renderer* r, TextFont* font) const {
    if (!font) return;
    float glyphH = font->lineHeight();
    for (int i = 0; i < (int)labels_.size(); ++i) {
        const SDL_FRect& rc = rects_[i];
        float circR = rc.h * .35f;
        float circD = circR * 2.f;
        float cx = rc.x + circR;
        float cy = rc.y + rc.h * .5f;

        // Outer circle (SDL has no native circle; approximate with a rect + corners).
        // Use a simple filled/bordered quad approach.
        SDL_FRect outer{ rc.x, rc.y + (rc.h - circD) * .5f, circD, circD };
        setColor(r, kColBorder);
        jplay::drawRect(r, &outer);

        if (i == selected_) {
            float inner = circR * .55f;
            SDL_FRect dot{ cx - inner, cy - inner, inner * 2.f, inner * 2.f };
            setColor(r, kColRadio);
            jplay::fillRect(r, &dot);
        }

        float labelX = rc.x + circD + 5.f;
        float labelY = rc.y + (rc.h - glyphH) * .5f;
        font->draw(r, labelX, labelY, kColText, labels_[i].c_str());
    }
}

// ─── Button ──────────────────────────────────────────────────────────────────

bool Button::handleEvent(const SDL_Event& e) {
    if (e.type == SDL_EVENT_MOUSE_MOTION) {
        hovered_ = enabled_ && inR(rect_, e.motion.x, e.motion.y);
        return false;
    }
    if (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN && e.button.button == SDL_BUTTON_LEFT) {
        if (enabled_ && inR(rect_, e.button.x, e.button.y))
            return true; // clicked
    }
    return false;
}

void Button::render(SDL_Renderer* r, TextFont* font) const {
    const SDL_Color text = enabled_ ? kColText : kColDim;
    SDL_Color bg = hovered_ ? SDL_Color{75, 105, 175, 255} : kColBtnBg;
    if (!enabled_) bg = {38, 39, 43, 255};
    drawButton(r, font, rect_, label_.c_str(), bg, kColBorder, text);
}

// ─── Combobox ────────────────────────────────────────────────────────────────

// One row of the open list, and of the closed box: the boxes are laid out in
// fixed logical px by every pane that uses them, so the rows are too.
static constexpr float kComboRowH = 20.f;

void Combobox::setOptions(std::vector<std::string> options, int selected) {
    options_ = std::move(options);
    // -1 passes through as "nothing selected"; anything else is clamped into the
    // options, and no options at all can only be unselected.
    selected_ = options_.empty() || selected < 0
                    ? -1
                    : std::clamp(selected, 0, (int)options_.size() - 1);
    open_ = false; hoverRow_ = -1;
    // The old options' scroll means nothing against the new ones, and a box
    // refilled from a query keeps its offset otherwise.
    scroll_ = 0.0f;
    sb_ = {};
}

const std::string& Combobox::value() const {
    static const std::string empty;
    return selected_ < 0 || selected_ >= (int)options_.size() ? empty : options_[selected_];
}

float Combobox::contentH() const {
    return kComboRowH * (float)options_.size();
}

SDL_FRect Combobox::listRect() const {
    const int rows = std::min((int)options_.size(), kMaxRows);
    return { rect_.x, rect_.y + rect_.h, rect_.w, kComboRowH * (float)rows };
}

int Combobox::rowAt(float y) const {
    const SDL_FRect lr = listRect();
    if (y < lr.y || y >= lr.y + lr.h)
        return -1;
    const int row = (int)((y - lr.y + scroll_) / kComboRowH);
    return (row >= 0 && row < (int)options_.size()) ? row : -1;
}

// Put the picked row in view, a third of the way down where there is room above
// it: the rows either side of it are the context for the one the box is on.
void Combobox::scrollToSelected() {
    const SDL_FRect lr = listRect();
    const float max = std::max(0.0f, contentH() - lr.h);
    if (selected_ < 0 || max <= 0.0f) {
        scroll_ = 0.0f;
        return;
    }
    scroll_ = std::clamp((float)selected_ * kComboRowH - lr.h / 3.0f, 0.0f, max);
}

bool Combobox::handleEvent(const SDL_Event& e) {
    // The dpiScale the scrollbar helpers take is 1: this widget lays itself out
    // in fixed logical px throughout, so a bar scaled past that would not match
    // the list it is drawn down.
    static constexpr float kNoScale = 1.0f;

    // Only while the list is open, and only over it -- a wheel anywhere else
    // belongs to the pane behind, which is scrolling its own rows.
    if (e.type == SDL_EVENT_MOUSE_WHEEL) {
        if (!open_ || !inR(listRect(), e.wheel.mouse_x, e.wheel.mouse_y))
            return false;
        const float max = std::max(0.0f, contentH() - listRect().h);
        scroll_ = std::clamp(scroll_ - e.wheel.y * kComboRowH * 3.0f, 0.0f, max);
        return true;
    }
    if (e.type == SDL_EVENT_MOUSE_BUTTON_UP && e.button.button == SDL_BUTTON_LEFT) {
        // Not consumed: the release that ends a drag is still the release every
        // other widget is waiting on to clear its own state.
        sb_.dragging = false;
        return false;
    }
    if (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN && e.button.button == SDL_BUTTON_LEFT) {
        if (inR(rect_, e.button.x, e.button.y)) {
            // Nothing to open with no options: opening would drop a zero-height
            // list over the row below and swallow the click that did it.
            open_ = !open_ && !options_.empty();
            hoverRow_ = -1;
            if (open_)
                scrollToSelected();
            return true;
        }
        if (open_) {
            // The bar first: it is drawn inside the list, so a press on it would
            // otherwise read as a press on the row behind it.
            if (sb_.press(e.button.x, e.button.y, scroll_, kNoScale))
                return true;
            const int row = rowAt(e.button.y);
            if (row >= 0 && inR(listRect(), e.button.x, e.button.y)) {
                selected_ = row; open_ = false; hoverRow_ = -1;
                return true;
            }
            open_ = false; hoverRow_ = -1;
            return true;
        }
        return false;
    }
    if (e.type == SDL_EVENT_MOUSE_MOTION) {
        if (open_) {
            if (sb_.dragging) {
                sb_.drag(e.motion.y, scroll_, kNoScale);
                return true;
            }
            hoverRow_ = inR(listRect(), e.motion.x, e.motion.y) ? rowAt(e.motion.y) : -1;
        }
        return false;
    }
    return false;
}

void Combobox::render(SDL_Renderer* r, TextFont* font) const {
    static constexpr float kCaretW = 14.f;

    // Box only (no open list — call renderDropdown() separately).
    setColor(r, kColBg);
    jplay::fillRect(r, &rect_);
    setColor(r, open_ ? kColFocus : kColBorder);
    jplay::drawRect(r, &rect_);

    if (!font)
        return;
    const float glyphH = font->lineHeight();
    const float ty = rect_.y + (rect_.h - glyphH) * .5f;
    // Bounds-checked rather than trusting selected_: a default-constructed box
    // has selected_ == 0 with no options, so "something is selected" is not the
    // same question as "there is something to draw".
    if (selected_ >= 0 && selected_ < (int)options_.size()) {
        font->draw(r, rect_.x + 6.f, ty, kColText, options_[selected_].c_str());
    } else if (!placeholder_.empty()) {
        // Dim, because it names the state rather than reporting a value.
        font->draw(r, rect_.x + 6.f, ty, kColDim, placeholder_.c_str());
    }
    // The caret is the affordance, so it appears exactly when there is a list
    // behind it — an empty box with one would invite a click that does nothing.
    if (!options_.empty())
        font->draw(r, rect_.x + rect_.w - kCaretW + 2.f, ty, kColDim, "v");
}

void Combobox::renderDropdown(SDL_Renderer* r, TextFont* font) {
    if (!open_) return;
    static constexpr float kNoScale = 1.0f; // see handleEvent

    SDL_FRect lr = listRect();
    setColor(r, kColDropBg);
    jplay::fillRect(r, &lr);
    setColor(r, kColBorder);
    jplay::drawRect(r, &lr);

    const float content = contentH();
    scroll_ = std::clamp(scroll_, 0.0f, std::max(0.0f, content - lr.h));

    if (font) {
        // Clipped to the list, so the rows either end of the scroll are cut at
        // its edge rather than drawn over the pane above and below it.
        const SDL_FRect inner = jplay::inset(lr, 1.f, 1.f);
        const SDL_Rect clip = { (int)inner.x, (int)inner.y, (int)inner.w, (int)inner.h };
        SDL_Rect wasClip{};
        const bool hadClip = SDL_RenderClipEnabled(r);
        if (hadClip)
            SDL_GetRenderClipRect(r, &wasClip);
        SDL_SetRenderClipRect(r, &clip);

        const float glyphH = font->lineHeight();
        for (int i = 0; i < (int)options_.size(); ++i) {
            const float ry = lr.y - scroll_ + i * kComboRowH;
            // Off the top or bottom of the list: skipped outright rather than
            // drawn under the clip, which is what keeps a menu of every show on
            // an instance from costing a draw per row nobody can see.
            if (ry + kComboRowH < lr.y || ry > lr.y + lr.h)
                continue;
            if (i == hoverRow_) {
                jplay::drawRowHover(r, SDL_FRect{ lr.x + 1.f, ry, lr.w - 2.f, kComboRowH });
            } else if (i == selected_) {
                SDL_FRect hr{ lr.x + 1.f, ry, lr.w - 2.f, kComboRowH };
                SDL_SetRenderDrawColor(r, 50, 70, 120, 255);
                jplay::fillRect(r, &hr);
            }
            font->draw(r, lr.x + 6.f, ry + (kComboRowH - glyphH) * .5f,
                       kColText, options_[i].c_str());
        }

        // Restored rather than cleared: an open list can be drawn inside a dialog
        // that is itself clipped, and clearing would let these rows out of it.
        if (hadClip)
            SDL_SetRenderClipRect(r, &wasClip);
        else
            SDL_SetRenderClipRect(r, nullptr);
    }

    // Last, so it sits over the rows it scrolls, and recorded for the next press.
    // Draws nothing while the options fit, which is most boxes.
    drawScrollbar(r, jplay::inset(lr, 1.f, 1.f), content, scroll_, kNoScale, true, sb_);
}

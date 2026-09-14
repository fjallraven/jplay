#include "Dropdown.h"

#include "Layout.h"
#include "MenuDraw.h"
#include "PixelSnap.h"
#include "SkinColors.h"

#include <algorithm>

// ---- dropdown palette
// Static colors used across render(). The box fill is computed per open/closed
// state and stays inline. The bar background is also drawn as a muted overlay
// at reduced alpha when disabled — the constant carries the base RGB and the
// alpha is passed at that call site.
namespace {

// References into the shared palette; see jplay::colors(). The box fills come
// from the palette's own dropdown-bar entries rather than the kUiBtn* pair they
// used to copy.
const SDL_Color& kBarBg     = jplay::colors().dbarBg;      // bar background (+ disabled overlay base)
const SDL_Color& kSeparator = jplay::colors().dbarSep;     // bottom separator line
const SDL_Color& kBoxBg     = jplay::colors().dbarBox;     // box fill, closed
const SDL_Color& kBoxBgOpen = jplay::colors().dbarBoxOpen; // box fill, list open
const SDL_Color& kLabelText = jplay::colors().label;       // entry label
const SDL_Color& kBorder    = jplay::colors().borderStrong;// box + list outline
const SDL_Color& kValueText = jplay::colors().text;        // box value + option row text
const SDL_Color& kListBg    = jplay::colors().dropBg;      // open option list fill
const SDL_Color& kSelText   = jplay::colors().selText;     // selected option row text
// No palette entry for the chevron; it is a shade off the menu arrow.
constexpr SDL_Color kCaret     { 180, 184, 192, 255 }; // down-caret chevron

inline void setColor(SDL_Renderer* r, SDL_Color c) {
    SDL_SetRenderDrawColor(r, c.r, c.g, c.b, c.a);
}

} // namespace

int DropdownBar::add(const std::string& label, std::vector<std::string> options, int selected) {
    Entry e;
    e.label = label;
    e.options = std::move(options);
    e.selected = std::clamp(selected, 0, e.options.empty() ? 0 : (int)e.options.size() - 1);
    entries_.push_back(std::move(e));
    return (int)entries_.size() - 1;
}


const std::string& DropdownBar::value(int i) const {
    static const std::string kEmpty;
    const Entry& e = entries_[i];
    if (e.selected < 0 || e.selected >= (int)e.options.size())
        return kEmpty;
    return e.options[e.selected];
}

void DropdownBar::setOptions(int i, std::vector<std::string> options, int selected) {
    Entry& e = entries_[i];
    e.options = std::move(options);
    e.separators.clear();
    if (e.options.empty())
        e.selected = 0;
    else
        e.selected = std::clamp(selected, 0, (int)e.options.size() - 1);
}

void DropdownBar::setText(int i, const std::string& text) {
    Entry& e = entries_[i];
    e.options.assign(1, text);
    e.separators.clear();
    e.selected = 0;
}

namespace {
bool isSeparatorRow(const std::vector<int>& seps, int row) {
    return std::find(seps.begin(), seps.end(), row) != seps.end();
}
} // namespace

// ---------------------------------------------------------------- geometry

void DropdownBar::layout(SDL_Renderer* r, const SDL_FRect& bar) {
    using namespace jplay;
    bar_ = bar;
    float charW = textFont_ ? textFont_->charWidth() : 8.0f;
    // Boxes are cut left-to-right off a row centered in the bar. The row is given
    // an unbounded width rather than the bar's own: cuts clamp, so a bar too
    // narrow for its entries would collapse the trailing boxes to zero width
    // instead of letting them run past the edge as they do today.
    SDL_FRect row = centerV(bar, kBoxH);
    row.w = kUnbounded;
    gapLeft(row, kPadX);
    for (int i = 0; i < (int)entries_.size(); ++i) {
        auto& e = entries_[i];
        if (!e.visible) {
            if (i == open_) open_ = -1;
            e.box = {};
            continue;
        }
        // Reserve the label's actual rendered width (not size*charW, which uses
        // the widest glyph) so the label left edge — drawn right-aligned to the
        // box in render() — lands at a consistent x across rows/bars.
        float labelW = textFont_ ? textFont_->measure(r, e.label.c_str()) : (float)e.label.size() * charW;
        gapLeft(row, labelW + kLabelGap); // label sits left of the box
        e.box = cutLeft(row, e.boxW);
        gapLeft(row, kPadX);
    }
}

SDL_FRect DropdownBar::listRect(int i) const {
    const Entry& e = entries_[i];
    return SDL_FRect{ e.box.x, e.box.y + e.box.h, e.box.w, (float)e.options.size() * kRowH };
}

int DropdownBar::boxAt(float x, float y) const {
    for (int i = 0; i < (int)entries_.size(); ++i) {
        if (!entries_[i].visible) continue;
        const SDL_FRect& rect = entries_[i].box;
        if (x >= rect.x && x < rect.x + rect.w && y >= rect.y && y < rect.y + rect.h)
            return i;
    }
    return -1;
}

int DropdownBar::rowAt(int i, float x, float y) const {
    if (i < 0)
        return -1;
    SDL_FRect lr = listRect(i);
    if (x < lr.x || x >= lr.x + lr.w || y < lr.y || y >= lr.y + lr.h)
        return -1;
    int row = (int)((y - lr.y) / kRowH);
    if (row < 0 || row >= (int)entries_[i].options.size())
        return -1;
    return isSeparatorRow(entries_[i].separators, row) ? -1 : row; // separators aren't selectable
}

// ---------------------------------------------------------------- input

bool DropdownBar::handleEvent(const SDL_Event& e) {
    // While disabled the bar swallows nothing: clicks fall through to the host
    // and any open list is dismissed.
    if (!enabled_) {
        if (open_ >= 0) open_ = -1;
        return false;
    }

    // Open `box`'s list, giving the host a chance to (re)populate it first.
    auto openBox = [this](int box) {
        if (entries_[box].onOpen)
            entries_[box].onOpen();
        open_ = box;
        hoverRow_ = -1;
    };

    switch (e.type) {
    case SDL_EVENT_MOUSE_BUTTON_DOWN: {
        if (e.button.button != SDL_BUTTON_LEFT) {
            if (open_ >= 0) { open_ = -1; return true; }
            return false;
        }
        float x = e.button.x, y = e.button.y;
        if (open_ >= 0) {
            int row = rowAt(open_, x, y);
            if (row >= 0) {
                int box = open_;
                bool changed = (row != entries_[box].selected);
                entries_[box].selected = row;
                open_ = -1;
                if (changed && entries_[box].onSelect)
                    entries_[box].onSelect(row);
                return true;
            }
            int box = boxAt(x, y);
            if (box >= 0 && box != open_)
                openBox(box); // switch to another
            else
                open_ = -1;   // click elsewhere closes
            hoverRow_ = -1;
            return true; // any click while open is consumed (it dismisses)
        }
        int box = boxAt(x, y);
        if (box >= 0) {
            openBox(box);
            return true;
        }
        return false;
    }
    case SDL_EVENT_MOUSE_MOTION:
        if (open_ < 0)
            return false;
        hoverRow_ = rowAt(open_, e.motion.x, e.motion.y);
        return true; // keep hover updates from leaking to the host while open
    case SDL_EVENT_KEY_DOWN:
        if (open_ >= 0 && e.key.key == SDLK_ESCAPE) { open_ = -1; return true; }
        return false;
    default:
        return false;
    }
}

// ---------------------------------------------------------------- rendering

namespace {
// Trim s so it fits maxChars debug-font glyphs, marking truncation with an ellipsis.
std::string fit(const std::string& s, int maxChars) {
    if (maxChars <= 0)
        return std::string();
    if ((int)s.size() <= maxChars)
        return s;
    if (maxChars <= 2)
        return s.substr(0, maxChars);
    return s.substr(0, maxChars - 2) + "..";
}
} // namespace

void DropdownBar::render(SDL_Renderer* r) const {
    // Bar background + bottom separator line.
    setColor(r, kBarBg);
    jplay::fillRect(r, &bar_);
    setColor(r, kSeparator);
    jplay::drawLine(r, bar_.x, bar_.y + bar_.h - 1, bar_.x + bar_.w, bar_.y + bar_.h - 1);

    float glyphH = textFont_ ? textFont_->lineHeight() : 8.0f;
    float charW  = textFont_ ? textFont_->charWidth()  : 8.0f;
    for (int i = 0; i < (int)entries_.size(); ++i) {
        const Entry& e = entries_[i];
        if (!e.visible) continue;
        const SDL_FRect& rect = e.box;
        float textY = rect.y + (rect.h - glyphH) * 0.5f;
        int valChars = (int)((rect.w - kInnerPadX - kCaretW) / charW);

        // Label to the left of the box.
        if (textFont_) {
            float labelW = textFont_->measure(r, e.label.c_str());
            textFont_->draw(r, rect.x - kLabelGap - labelW, textY,
                            kLabelText, e.label.c_str());
        }

        // Box (lighter while its list is open).
        bool open = (i == open_);
        setColor(r, open ? kBoxBgOpen : kBoxBg);
        jplay::fillRect(r, &rect);
        setColor(r, kBorder);
        jplay::drawRect(r, &rect);

        // Current value.
        if (textFont_)
            textFont_->draw(r, rect.x + kInnerPadX, textY,
                            kValueText, fit(value(i), valChars).c_str());

        // Down caret (chevron) on the right edge.
        float cx = rect.x + rect.w - kCaretW * 0.5f - 3.0f;
        float cy = rect.y + rect.h * 0.5f - 1.0f;
        setColor(r, kCaret);
        jplay::drawCaretDown(r, cx, cy);
    }

    // When disabled, mute the whole bar by laying the background color back over
    // the controls at partial opacity — boxes and text read as faint/greyed.
    // Leave the renderer in the app-wide default blend mode (BLEND); resetting it
    // to NONE here leaked out and made later translucent fills (e.g. the timeline
    // in/out range) render as solid color while playback disables this bar.
    if (!enabled_) {
        SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
        SDL_SetRenderDrawColor(r, kBarBg.r, kBarBg.g, kBarBg.b, 150);
        jplay::fillRect(r, &bar_);
    }

    // Open option list, drawn last so it overlays neighbouring controls and the
    // content below the bar.
    if (open_ < 0)
        return;
    const Entry& e = entries_[open_];
    SDL_FRect lr = listRect(open_);
    setColor(r, kListBg);
    jplay::fillRect(r, &lr);
    setColor(r, kBorder);
    jplay::drawRect(r, &lr);

    for (int row = 0; row < (int)e.options.size(); ++row) {
        float ry = lr.y + row * kRowH;
        // Separator: a divider line centered in the row (spacing above/below);
        // never highlighted or labeled.
        if (isSeparatorRow(e.separators, row)) {
            float ly = ry + kRowH * 0.5f;
            setColor(r, kBorder);
            jplay::drawLine(r, lr.x + kInnerPadX, ly, lr.x + lr.w - kInnerPadX, ly);
            continue;
        }
        if (row == hoverRow_)
            jplay::drawRowHover(r, SDL_FRect{ lr.x + 1.0f, ry, lr.w - 2.0f, kRowH });
        bool sel = (row == e.selected);
        if (textFont_) {
            int rowChars = (int)((lr.w - 2.0f * kInnerPadX) / charW);
            SDL_Color col = sel ? kSelText : kValueText;
            textFont_->draw(r, lr.x + kInnerPadX, ry + (kRowH - glyphH) * 0.5f,
                            col, fit(e.options[row], rowChars).c_str());
        }
    }
}

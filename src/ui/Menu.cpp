#include "Menu.h"

#include "MenuDraw.h"
#include "PixelSnap.h"
#include "SkinColors.h"

#include <algorithm>

// ---- menu palette
// Static colors used across render(). Computed/per-state colors stay inline.
namespace {

// References into the live skin palette; see jplay::colors().
const SDL_Color& kSeparator        = jplay::colors().menuSep;         // dropdown separators
const SDL_Color& kTitleText        = jplay::colors().menuTitle;       // top-level menu titles (idle)
const SDL_Color& kTitleTextLit     = jplay::colors().menuTitleLit;    // ...when hovered or open
const SDL_Color& kDropdownBg       = jplay::colors().dropBg;          // dropdown panel fill
const SDL_Color& kTitleOpenBg      = jplay::colors().menuTitleBg;     // open-title bg: dropdown fill, a shade darker
const SDL_Color& kDropdownBorder   = jplay::colors().border;          // dropdown panel outline
const SDL_Color& kItemText         = jplay::colors().text;            // enabled item label
const SDL_Color& kItemTextDisabled = jplay::colors().menuDisabled;    // disabled item label
const SDL_Color& kShortcut         = jplay::colors().menuShortcut;    // right-aligned key hint
const SDL_Color& kShortcutDisabled = jplay::colors().menuShortcutOff; // key hint on a disabled item
const SDL_Color& kArrow            = jplay::colors().menuArrow;       // submenu arrow glyph
const SDL_Color& kCheck            = jplay::colors().menuCheck;       // active-item square
const SDL_Color& kSliderTrack      = jplay::colors().msliderTrack;    // slider groove fill
const SDL_Color& kSliderTrackEdge  = jplay::colors().msliderEdge;     // slider groove outline
const SDL_Color& kSliderFill       = jplay::colors().msliderFill;     // slider filled portion
const SDL_Color& kSliderKnob       = jplay::colors().msliderKnob;     // slider handle

constexpr float kSliderTrackH = 6.0f; // base (1x) groove height

inline void setColor(SDL_Renderer* r, SDL_Color c) {
    SDL_SetRenderDrawColor(r, c.r, c.g, c.b, c.a);
}

} // namespace

int MenuBar::addMenu(const std::string& title) {
    menus_.push_back(Menu{ title, {}, false });
    return (int)menus_.size() - 1;
}

void MenuBar::addItem(int menu, const std::string& label, Action action,
                      Enabled enabled, LabelFn labelFn, const std::string& shortcut,
                      Checked checked) {
    Item it{ label, shortcut, std::move(labelFn), std::move(action), -1, std::move(enabled) };
    it.checked = std::move(checked);
    menus_[menu].items.push_back(std::move(it));
}

void MenuBar::addSeparator(int menu) {
    menus_[menu].items.push_back(Item{ "", "", nullptr, nullptr, -1, nullptr });
}

void MenuBar::addSlider(int menu, const std::string& label, float minValue, float maxValue,
                        GetValue get, SetValue set, Action commit,
                        Enabled enabled, LabelFn labelFn) {
    Item it{ label, "", std::move(labelFn), nullptr, -1, std::move(enabled) };
    it.isSlider = true;
    it.minValue = minValue;
    it.maxValue = maxValue;
    it.get = std::move(get);
    it.set = std::move(set);
    it.commit = std::move(commit);
    menus_[menu].items.push_back(std::move(it));
}

int MenuBar::addSubmenu(int parentMenu, const std::string& label) {
    int idx = (int)menus_.size();
    menus_.push_back(Menu{ "", {}, true });
    menus_[parentMenu].items.push_back(Item{ label, "", nullptr, nullptr, idx, nullptr });
    return idx;
}

void MenuBar::clearItems(int menu) {
    menus_[menu].items.clear();
}

// ---------------------------------------------------------------- geometry

float MenuBar::textW(const std::string& s) const {
    if (!textFont_) return (float)s.size() * 8.0f;
    if (renderer_) return textFont_->measure(renderer_, s.c_str());
    return (float)s.size() * textFont_->charWidth();
}

void MenuBar::layout(float winW, float top, float barH, float leftInset, float scale) {
    winW_ = winW;
    top_ = top;
    barH_ = barH;
    scale_ = scale;
    titleRects_.clear();
    float x = leftInset;
    for (const auto& m : menus_) {
        if (m.isSubmenu) {
            titleRects_.push_back(SDL_FRect{ -9999.f, -9999.f, 0.f, 0.f });
            continue;
        }
        float w = textW(m.title) + 2.0f * padX();
        titleRects_.push_back(SDL_FRect{ x, top_, w, barH_ });
        x += w;
    }
}

float MenuBar::titleBgBottom() const {
    float glyphH = textFont_ ? textFont_->lineHeight() : 8.0f;
    return top_ + (barH_ + glyphH) * 0.5f + titleBgPadY();
}

SDL_FRect MenuBar::dropdownRect(int menu) const {
    const Menu& m = menus_[menu];
    float maxLabel = 0.0f;
    float maxShortcut = 0.0f;
    float h = 0.0f;
    bool hasSubmenuItem = false;
    for (const auto& it : m.items) {
        maxLabel = std::max(maxLabel, textW(it.label));
        maxShortcut = std::max(maxShortcut, textW(it.shortcut));
        h += rowH(it);
        if (it.submenuIdx >= 0) hasSubmenuItem = true;
    }
    float w = maxLabel + 2.0f * itemPadX();
    if (maxShortcut > 0.0f) w += shortcutGapX() + maxShortcut;
    if (hasSubmenuItem) w += submenuArrowW();
    w = std::max(w, titleRects_[menu].w);
    return SDL_FRect{ titleRects_[menu].x, titleBgBottom(), w, h };
}

SDL_FRect MenuBar::submenuDropdownRect() const {
    if (openMenu_ < 0 || openSubmenu_ < 0 || openSubmenuParentItem_ < 0)
        return {};
    SDL_FRect parentDD = dropdownRect(openMenu_);

    // Y of the submenu item row inside the parent dropdown.
    float itemY = parentDD.y;
    const Menu& pm = menus_[openMenu_];
    for (int i = 0; i < openSubmenuParentItem_; ++i)
        itemY += rowH(pm.items[i]);

    const Menu& sm = menus_[openSubmenu_];
    float maxLabel = 0.0f;
    float maxShortcut = 0.0f;
    float h = 0.0f;
    for (const auto& it : sm.items) {
        maxLabel = std::max(maxLabel, textW(it.label));
        maxShortcut = std::max(maxShortcut, textW(it.shortcut));
        h += rowH(it);
    }
    float w = maxLabel + 2.0f * itemPadX();
    if (maxShortcut > 0.0f) w += shortcutGapX() + maxShortcut;
    return SDL_FRect{ parentDD.x + parentDD.w, itemY, w, h };
}

// Groove rect of a slider row: inset the dropdown width, sitting on the row's
// second line (the label occupies the first). Mirrored by render().
SDL_FRect MenuBar::sliderTrackRect(int menu, int item) const {
    SDL_FRect dd = dropdownRect(menu);
    const Menu& m = menus_[menu];
    float cy = dd.y;
    for (int i = 0; i < item; ++i)
        cy += rowH(m.items[i]);
    const float trackH = kSliderTrackH * scale_;
    return SDL_FRect{ dd.x + itemPadX(), cy + itemH() + (itemH() - trackH) * 0.5f,
                      dd.w - 2.0f * itemPadX(), trackH };
}

void MenuBar::sliderSetFromX(int menu, int item, float mx) {
    const Item& it = menus_[menu].items[item];
    if (!it.set) return;
    SDL_FRect tr = sliderTrackRect(menu, item);
    float t = tr.w > 0.0f ? std::clamp((mx - tr.x) / tr.w, 0.0f, 1.0f) : 0.0f;
    it.set(it.minValue + t * (it.maxValue - it.minValue));
}

int MenuBar::titleAt(float x, float y) const {
    if (y < top_ || y > top_ + barH_)
        return -1;
    for (int i = 0; i < (int)titleRects_.size(); ++i) {
        if (menus_[i].isSubmenu) continue;
        const SDL_FRect& rect = titleRects_[i];
        if (x >= rect.x && x < rect.x + rect.w)
            return i;
    }
    return -1;
}

int MenuBar::itemAt(float x, float y) const {
    if (openMenu_ < 0) return -1;
    SDL_FRect dd = dropdownRect(openMenu_);
    if (x < dd.x || x > dd.x + dd.w || y < dd.y || y > dd.y + dd.h)
        return -1;
    const Menu& m = menus_[openMenu_];
    float cy = dd.y;
    for (int i = 0; i < (int)m.items.size(); ++i) {
        float h = rowH(m.items[i]);
        if (y >= cy && y < cy + h)
            return (m.items[i].label.empty() || !itemEnabled(m.items[i])) ? -1 : i;
        cy += h;
    }
    return -1;
}

int MenuBar::submenuItemAt(float x, float y) const {
    if (openSubmenu_ < 0) return -1;
    SDL_FRect smDD = submenuDropdownRect();
    if (x < smDD.x || x > smDD.x + smDD.w || y < smDD.y || y > smDD.y + smDD.h)
        return -1;
    const Menu& m = menus_[openSubmenu_];
    float cy = smDD.y;
    for (int i = 0; i < (int)m.items.size(); ++i) {
        float h = rowH(m.items[i]);
        if (y >= cy && y < cy + h)
            return (m.items[i].label.empty() || !itemEnabled(m.items[i])) ? -1 : i;
        cy += h;
    }
    return -1;
}

// ---------------------------------------------------------------- input

bool MenuBar::handleEvent(const SDL_Event& e) {
    switch (e.type) {
    case SDL_EVENT_MOUSE_BUTTON_DOWN: {
        if (e.button.button != SDL_BUTTON_LEFT) {
            if (openMenu_ >= 0) {
                openMenu_ = -1; openSubmenu_ = -1; openSubmenuParentItem_ = -1;
                return true;
            }
            return false;
        }
        // Click on a submenu item.
        if (openSubmenu_ >= 0) {
            int si = submenuItemAt(e.button.x, e.button.y);
            if (si >= 0 || [&]{
                    SDL_FRect sm = submenuDropdownRect();
                    return e.button.x >= sm.x && e.button.x <= sm.x + sm.w &&
                           e.button.y >= sm.y && e.button.y <= sm.y + sm.h;
                }()) {
                Action action = (si >= 0) ? menus_[openSubmenu_].items[si].action : nullptr;
                openMenu_ = -1; openSubmenu_ = -1; openSubmenuParentItem_ = -1;
                hoverItem_ = -1; submenuHoverItem_ = -1;
                if (action) action();
                return true;
            }
        }
        // Click on a top-level title.
        int t = titleAt(e.button.x, e.button.y);
        if (t >= 0) {
            openMenu_ = (openMenu_ == t) ? -1 : t;
            openSubmenu_ = -1; openSubmenuParentItem_ = -1;
            hoverItem_ = -1; submenuHoverItem_ = -1;
            return true;
        }
        // Click on a parent dropdown item.
        if (openMenu_ >= 0) {
            int it = itemAt(e.button.x, e.button.y);
            Action action = nullptr;
            if (it >= 0) {
                const Item& item = menus_[openMenu_].items[it];
                if (item.isSlider) {
                    // Grab the handle; the dropdown stays open for the drag.
                    sliderDragMenu_ = openMenu_;
                    sliderDragItem_ = it;
                    sliderSetFromX(openMenu_, it, e.button.x);
                    return true;
                }
                if (item.submenuIdx >= 0) {
                    // Toggle or open the submenu on click.
                    openSubmenu_ = (openSubmenu_ == item.submenuIdx) ? -1 : item.submenuIdx;
                    openSubmenuParentItem_ = (openSubmenu_ >= 0) ? it : -1;
                    submenuHoverItem_ = -1;
                    return true;
                }
                action = item.action;
            }
            openMenu_ = -1; openSubmenu_ = -1; openSubmenuParentItem_ = -1;
            hoverItem_ = -1; submenuHoverItem_ = -1;
            if (action) action();
            return true;
        }
        return false;
    }
    case SDL_EVENT_MOUSE_BUTTON_UP: {
        if (sliderDragItem_ >= 0 && e.button.button == SDL_BUTTON_LEFT) {
            Action commit = menus_[sliderDragMenu_].items[sliderDragItem_].commit;
            sliderDragMenu_ = -1;
            sliderDragItem_ = -1;
            if (commit) commit();
            return true;
        }
        return false;
    }
    case SDL_EVENT_MOUSE_MOTION: {
        hoverTitle_ = titleAt(e.motion.x, e.motion.y);
        if (sliderDragItem_ >= 0) {
            if (openMenu_ == sliderDragMenu_) { // dropped the drag if the menu closed under it
                sliderSetFromX(sliderDragMenu_, sliderDragItem_, e.motion.x);
                return true;
            }
            sliderDragMenu_ = -1;
            sliderDragItem_ = -1;
        }
        if (openMenu_ < 0) return false;

        int t = titleAt(e.motion.x, e.motion.y);
        if (t >= 0 && t != openMenu_) {
            openMenu_ = t;
            openSubmenu_ = -1; openSubmenuParentItem_ = -1;
            hoverItem_ = -1; submenuHoverItem_ = -1;
            return true;
        }

        // Check if over the submenu dropdown first.
        if (openSubmenu_ >= 0) {
            SDL_FRect smDD = submenuDropdownRect();
            if (e.motion.x >= smDD.x && e.motion.x <= smDD.x + smDD.w &&
                e.motion.y >= smDD.y && e.motion.y <= smDD.y + smDD.h) {
                submenuHoverItem_ = submenuItemAt(e.motion.x, e.motion.y);
                return true;
            }
        }

        // Check if over the parent dropdown.
        SDL_FRect dd = dropdownRect(openMenu_);
        if (e.motion.x >= dd.x && e.motion.x <= dd.x + dd.w &&
            e.motion.y >= dd.y && e.motion.y <= dd.y + dd.h) {
            hoverItem_ = itemAt(e.motion.x, e.motion.y);
            if (hoverItem_ >= 0) {
                const Item& item = menus_[openMenu_].items[hoverItem_];
                if (item.submenuIdx >= 0) {
                    openSubmenu_ = item.submenuIdx;
                    openSubmenuParentItem_ = hoverItem_;
                    submenuHoverItem_ = -1;
                } else {
                    openSubmenu_ = -1;
                    openSubmenuParentItem_ = -1;
                }
            }
            return true;
        }

        return true; // consume motion while a menu is open
    }
    case SDL_EVENT_WINDOW_MOUSE_LEAVE:
        hoverTitle_ = -1;
        return false;
    case SDL_EVENT_KEY_DOWN:
        if (openMenu_ >= 0 && e.key.key == SDLK_ESCAPE) {
            if (openSubmenu_ >= 0) {
                openSubmenu_ = -1; openSubmenuParentItem_ = -1;
            } else {
                openMenu_ = -1;
            }
            return true;
        }
        return false;
    default:
        return false;
    }
}

// ---------------------------------------------------------------- rendering

void MenuBar::drawCheck(SDL_Renderer* r, const Item& it, float labelX, float rowTop) const {
    if (!it.checked || !textFont_ || !it.checked())
        return;
    // The square goes in the blank the label's own leading spaces reserve for it:
    // the second-to-last of them, leaving one clear before the text. A row at any
    // indent therefore marks at its own depth — a nested radio row under a section
    // header lines up with its siblings, not with the top-level rows — without the
    // caller having to measure anything.
    const size_t firstText = it.label.find_first_not_of(' ');
    const int indent = firstText == std::string::npos ? 0 : (int)firstText;
    const float sp = textFont_->measure(r, " ");
    const float side = std::min(6.0f * scale_, itemH() * 0.5f);
    // Nudged left of that cell: the space glyph is narrower than the square, so
    // sitting centred in it leaves the square touching the first letter.
    const float cellX = labelX + (float)std::max(indent - 2, 0) * sp - 4.0f * scale_;
    // Centred on the letters, not on the line box they sit in: that box reserves
    // descender and line-gap room a menu label has no use for, so a square centred
    // in the row reads as low against the text beside it. Same top edge the font is
    // drawn from below, then down to the baseline and back up half a capital.
    const float textTop = rowTop + (itemH() - textFont_->lineHeight()) * .5f;
    const float cy = textTop + textFont_->ascent() - textFont_->capHeight() * .5f;
    SDL_FRect mk{ cellX + (sp - side) * .5f, cy - side * .5f, side, side };
    setColor(r, kCheck);
    jplay::fillRect(r, &mk);
}

void MenuBar::render(SDL_Renderer* r) {
    renderer_ = r; // lets textW() measure instead of estimating, from here on
    float glyphH = textFont_ ? textFont_->lineHeight() : 8.0f;
    float charW  = textFont_ ? textFont_->charWidth()  : 8.0f;
    const float titleY = (barH_ - glyphH) * 0.5f;

    for (int i = 0; i < (int)menus_.size(); ++i) {
        if (menus_[i].isSubmenu) continue;
        const SDL_FRect& tr = titleRects_[i];
        const bool lit = (i == openMenu_) || (i == hoverTitle_);
        if (lit) {
            // Hug the text, not the whole title cell. Measured width, not
            // charW * length — the font is proportional, so the cell's estimate
            // would pad unevenly L vs R. The bottom edge must stay in step with
            // titleBgBottom(), which is where the dropdown starts.
            const float textW = textFont_ ? textFont_->measure(r, menus_[i].title.c_str())
                                          : (float)menus_[i].title.size() * charW;
            SDL_FRect bg{ tr.x + padX() - titleBgPadX(), tr.y + titleY - titleBgPadY(),
                          textW + 2.0f * titleBgPadX(), glyphH + 2.0f * titleBgPadY() };
            setColor(r, kTitleOpenBg);
            jplay::fillRect(r, &bg);
        }
        if (textFont_)
            textFont_->draw(r, tr.x + padX(), tr.y + titleY,
                            lit ? kTitleTextLit : kTitleText,
                            menus_[i].title.c_str());
    }

    if (openMenu_ < 0) return;

    // Draw parent dropdown.
    SDL_FRect dd = dropdownRect(openMenu_);
    setColor(r, kDropdownBg);
    jplay::fillRect(r, &dd);
    setColor(r, kDropdownBorder);
    jplay::drawRect(r, &dd);

    const Menu& m = menus_[openMenu_];
    float cy = dd.y;
    for (int i = 0; i < (int)m.items.size(); ++i) {
        const Item& it = m.items[i];
        if (it.label.empty()) {
            setColor(r, kSeparator);
            jplay::drawLine(r, dd.x + 4.f, cy + sepH() * .5f,
                              dd.x + dd.w - 4.f, cy + sepH() * .5f);
            cy += sepH();
            continue;
        }
        bool enabled = itemEnabled(it);
        if (it.isSlider) {
            SDL_Color textCol = enabled ? kItemText : kItemTextDisabled;
            if (textFont_) {
                std::string dynLabel;
                const char* dispLabel = it.labelFn
                    ? (dynLabel = it.labelFn(), dynLabel.c_str())
                    : it.label.c_str();
                textFont_->draw(r, dd.x + itemPadX(), cy + (itemH() - glyphH) * .5f,
                                textCol, dispLabel);
            }
            SDL_FRect track = sliderTrackRect(openMenu_, i);
            setColor(r, kSliderTrack);
            jplay::fillRect(r, &track);
            float t = 0.0f;
            if (it.get && it.maxValue > it.minValue)
                t = std::clamp((it.get() - it.minValue) / (it.maxValue - it.minValue), 0.0f, 1.0f);
            float hx = track.x + t * track.w;
            if (enabled) {
                SDL_FRect fill{ track.x, track.y,
                                std::max(0.0f, hx - track.x), track.h };
                setColor(r, kSliderFill);
                jplay::fillRect(r, &fill);
            }
            setColor(r, kSliderTrackEdge);
            jplay::drawRect(r, &track);
            if (enabled) {
                SDL_FRect knob{ hx - 3.0f * scale_, track.y - 3.0f * scale_,
                                6.0f * scale_, track.h + 6.0f * scale_ };
                setColor(r, kSliderKnob);
                jplay::fillRect(r, &knob);
            }
            cy += rowH(it);
            continue;
        }
        bool isHovered = (i == hoverItem_) ||
                         (it.submenuIdx >= 0 && it.submenuIdx == openSubmenu_);
        if (isHovered)
            jplay::drawRowHover(r, SDL_FRect{ dd.x + 1.f, cy, dd.w - 2.f, itemH() });
        SDL_Color textCol = enabled ? kItemText : kItemTextDisabled;
        if (textFont_) {
            std::string dynLabel;
            const char* dispLabel = it.labelFn
                ? (dynLabel = it.labelFn(), dynLabel.c_str())
                : it.label.c_str();
            textFont_->draw(r, dd.x + itemPadX(), cy + (itemH() - glyphH) * .5f,
                            textCol, dispLabel);
            drawCheck(r, it, dd.x + itemPadX(), cy);
            if (!it.shortcut.empty()) {
                // Measured, not charW-estimated: the font is proportional, so an
                // estimate would leave the longest hint short of the right edge.
                float sw = textFont_->measure(r, it.shortcut.c_str());
                textFont_->draw(r, dd.x + dd.w - itemPadX() - sw,
                                cy + (itemH() - glyphH) * .5f,
                                enabled ? kShortcut : kShortcutDisabled,
                                it.shortcut.c_str());
            }
        }
        if (it.submenuIdx >= 0 && textFont_) {
            float arrowX = dd.x + dd.w - submenuArrowW() + (submenuArrowW() - charW) * .5f;
            textFont_->draw(r, arrowX, cy + (itemH() - glyphH) * .5f,
                            kArrow, ">");
        }
        cy += itemH();
    }

    // Draw submenu dropdown.
    if (openSubmenu_ < 0) return;

    SDL_FRect smDD = submenuDropdownRect();
    setColor(r, kDropdownBg);
    jplay::fillRect(r, &smDD);
    setColor(r, kDropdownBorder);
    jplay::drawRect(r, &smDD);

    const Menu& sm = menus_[openSubmenu_];
    float scy = smDD.y;
    for (int i = 0; i < (int)sm.items.size(); ++i) {
        const Item& it = sm.items[i];
        if (it.label.empty()) {
            setColor(r, kSeparator);
            jplay::drawLine(r, smDD.x + 4.f, scy + sepH() * .5f,
                              smDD.x + smDD.w - 4.f, scy + sepH() * .5f);
            scy += sepH();
            continue;
        }
        if (i == submenuHoverItem_)
            jplay::drawRowHover(r, SDL_FRect{ smDD.x + 1.f, scy, smDD.w - 2.f, itemH() });
        bool enabled = itemEnabled(it);
        SDL_Color textCol = enabled ? kItemText : kItemTextDisabled;
        if (textFont_) {
            std::string dynLabel;
            const char* dispLabel = it.labelFn
                ? (dynLabel = it.labelFn(), dynLabel.c_str())
                : it.label.c_str();
            textFont_->draw(r, smDD.x + itemPadX(), scy + (itemH() - glyphH) * .5f,
                            textCol, dispLabel);
            drawCheck(r, it, smDD.x + itemPadX(), scy);
            if (!it.shortcut.empty()) {
                float sw = textFont_->measure(r, it.shortcut.c_str());
                textFont_->draw(r, smDD.x + smDD.w - itemPadX() - sw,
                                scy + (itemH() - glyphH) * .5f,
                                enabled ? kShortcut : kShortcutDisabled,
                                it.shortcut.c_str());
            }
        }
        scy += itemH();
    }
}

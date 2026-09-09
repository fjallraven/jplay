#pragma once

#include "TextFont.h"

#include <SDL3/SDL.h>

#include <functional>
#include <string>
#include <vector>

// A generic, self-contained menu bar for SDL3 immediate-mode UIs.
//
// It renders with the built-in debug font and owns its own mouse/keyboard
// interaction. The host loop feeds it events first (handleEvent), then calls
// layout()/render() each frame. Item actions are arbitrary callbacks, so the
// bar knows nothing about the application it drives.
class MenuBar {
public:
    using Action = std::function<void()>;

    // Build the bar. addMenu() returns a stable menu index to pass to
    // addItem()/addSeparator() (indices never shift, so this is reallocation
    // safe regardless of how many menus are added later).
    int addMenu(const std::string& title);
    // enabled is an optional predicate evaluated each frame; when it returns
    // false the item is greyed out and ignores hover/click. Null = always on.
    using Enabled = std::function<bool()>;
    // labelFn, when set, overrides the displayed label each frame (e.g. to show
    // a checkmark prefix). The label string is still used for dropdown width
    // calculation, so keep it the same length as any labelFn output.
    using LabelFn = std::function<std::string()>;
    // shortcut, when non-empty (e.g. "Ctrl+S"), is drawn right-aligned in the
    // dropdown in a dimmer color. It is purely a hint: the host still owns the
    // key handling.
    // checked, when set and returning true, marks the row with a filled square in
    // the blank its label's own leading spaces reserve — so a checkable row keeps
    // the same text position whether it is marked or not, and the label need not
    // be rebuilt each frame to carry a marker glyph. Evaluated every frame.
    using Checked = std::function<bool()>;
    void addItem(int menu, const std::string& label, Action action,
                 Enabled enabled = nullptr, LabelFn labelFn = nullptr,
                 const std::string& shortcut = "", Checked checked = nullptr);
    void addSeparator(int menu);

    // A drag-adjustable slider row (label line + track line, so it occupies two
    // item heights). get supplies the current value each frame, set receives the
    // new one while dragging, and commit (optional) fires once on release. The
    // dropdown stays open while the handle is dragged. Top-level menus only.
    using GetValue = std::function<float()>;
    using SetValue = std::function<void(float)>;
    void addSlider(int menu, const std::string& label, float minValue, float maxValue,
                   GetValue get, SetValue set, Action commit = nullptr,
                   Enabled enabled = nullptr, LabelFn labelFn = nullptr);

    // Add a flyout submenu under parentMenu. Returns a new menu index;
    // populate it with addItem/addSeparator. The submenu opens to the right
    // when the user hovers over the parent item.
    int addSubmenu(int parentMenu, const std::string& label);

    // Remove all items from the given menu (keeps the menu entry itself). Safe to
    // call between frames; used to rebuild dynamic submenus (e.g. OCIO views).
    void clearItems(int menu);

    // Returns true if the event was consumed; the host should then skip its
    // own handling for that event.
    bool handleEvent(const SDL_Event& e);

    // Recompute geometry for the current window width; call before render().
    // top is the y of the bar's top edge and barH its height, so the menu can
    // be embedded in a host bar (e.g. a custom title bar) of a given size.
    // leftInset shifts the first title right, leaving room for host chrome on
    // the left (e.g. an application icon). scale multiplies the item/padding
    // metrics below (the host's DPI scale; the class stays app-agnostic
    // otherwise, so it takes a plain factor rather than reading a global).
    void setTextFont(TextFont* f) { textFont_ = f; }

    void layout(float winW, float top = 0.0f, float barH = kBarH, float leftInset = 0.0f,
                float scale = 1.0f);
    void render(SDL_Renderer* renderer);

    float height() const { return barH_; }
    bool isOpen() const { return openMenu_ >= 0; }

    // Dismiss any open menu/submenu (e.g. when the host opens a popup of its own).
    void close() {
        openMenu_ = -1; openSubmenu_ = -1; openSubmenuParentItem_ = -1;
        hoverItem_ = -1; submenuHoverItem_ = -1;
    }

    // True if the point is over a top-level menu title (so a host hit test can
    // keep it clickable instead of treating it as a window-drag region).
    bool pointOverTitle(float x, float y) const { return titleAt(x, y) >= 0; }

private:
    // Base (1x) metrics; scaled by `scale_` (set from layout()'s scale param) at use.
    static constexpr float kBarH = 24.0f;
    static constexpr float kItemH = 20.0f;
    static constexpr float kSepH = 7.0f;
    static constexpr float kPadX = 12.0f;   // padding around top-level titles
    static constexpr float kTitleBgPadX = 10.0f; // lit-title background: padding around the text
    static constexpr float kTitleBgPadY = 3.0f;
    static constexpr float kItemPadX = 10.0f;
    static constexpr float kSubmenuArrowW = 18.0f; // right-side space for the > arrow
    static constexpr float kShortcutGapX = 24.0f;  // min gap between label and shortcut

    float itemH() const { return kItemH * scale_; }
    float sepH() const { return kSepH * scale_; }
    float padX() const { return kPadX * scale_; }
    float titleBgPadX() const { return kTitleBgPadX * scale_; }
    float titleBgPadY() const { return kTitleBgPadY * scale_; }
    float itemPadX() const { return kItemPadX * scale_; }
    float submenuArrowW() const { return kSubmenuArrowW * scale_; }
    float shortcutGapX() const { return kShortcutGapX * scale_; }

    TextFont* textFont_ = nullptr;

    struct Item {
        std::string label;      // empty => separator; also used for dropdown width
        std::string shortcut;   // optional right-aligned key hint; empty => none
        LabelFn labelFn;        // if set, overrides the displayed text each frame
        Action action;
        int submenuIdx = -1;    // >= 0 => opens menus_[submenuIdx] as flyout
        Enabled enabled;        // null => always enabled
        bool isSlider = false;  // slider row (get/set/commit below drive it)
        float minValue = 0.0f, maxValue = 1.0f;
        GetValue get;
        SetValue set;
        Action commit;          // slider: fired on handle release
        Checked checked;        // null => never marked
    };

    bool itemEnabled(const Item& it) const { return !it.enabled || it.enabled(); }
    // Width of a label as it will actually be drawn. The font is proportional, so
    // charWidth() * length (measured from "M") overshoots badly on long rows; that
    // estimate is only the fallback for the first frame, before render() has handed
    // us a renderer to measure with.
    float textW(const std::string& s) const;
    // Draw an item's check mark, if it has one and it is on. labelX is where the
    // row's text starts and rowTop the row's upper edge.
    void drawCheck(SDL_Renderer* r, const Item& it, float labelX, float rowTop) const;
    // Row height: separators are thin, sliders take a label line plus a track line.
    float rowH(const Item& it) const {
        if (it.label.empty()) return sepH();
        return it.isSlider ? itemH() * 2.0f : itemH();
    }
    struct Menu {
        std::string title;
        std::vector<Item> items;
        bool isSubmenu = false; // owned by a parent item; no top-level title
    };

    std::vector<Menu> menus_;
    std::vector<SDL_FRect> titleRects_; // parallel to menus_; zero-rect for submenus

    mutable SDL_Renderer* renderer_ = nullptr; // captured in render(); for textW()

    float winW_ = 0.0f;
    float top_ = 0.0f;
    float barH_ = kBarH;
    float scale_ = 1.0f;

    int openMenu_ = -1;           // index into menus_, -1 = closed
    int hoverTitle_ = -1;         // top-level title under the cursor, -1 = none
    int hoverItem_ = -1;          // hovered item in the open menu
    int openSubmenu_ = -1;        // menu index of the open submenu, -1 = none
    int openSubmenuParentItem_ = -1; // item index in openMenu_ that owns openSubmenu_
    int submenuHoverItem_ = -1;   // hovered item within the open submenu
    int sliderDragMenu_ = -1;     // menu owning the slider being dragged, -1 = none
    int sliderDragItem_ = -1;     // item index of that slider

    // Bottom edge of the lit background behind a top-level title; dropdowns hang
    // off it rather than off the bar, so the two line up.
    float titleBgBottom() const;

    SDL_FRect dropdownRect(int menu) const;
    SDL_FRect submenuDropdownRect() const;
    SDL_FRect sliderTrackRect(int menu, int item) const;
    void sliderSetFromX(int menu, int item, float mx);
    int titleAt(float x, float y) const;
    int itemAt(float x, float y) const;
    int submenuItemAt(float x, float y) const;
};

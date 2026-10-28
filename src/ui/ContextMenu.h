#pragma once

#include "TextFont.h"
#include "Widgets.h"

#include <SDL3/SDL.h>

#include <functional>
#include <string>
#include <vector>

// A cursor-anchored popup menu for SDL3 immediate-mode UIs. It anchors its
// bottom edge at the open point and grows upward, closing when the user clicks
// outside it or presses Escape. Rows are either leaf items (with an action) or
// submenu parents (children fly out to the side). Self-contained like the other
// widgets: the host feeds it events first (handleEvent), then calls render()
// each frame while it is open (render() also recomputes geometry).
class ContextMenu {
public:
    using Action = std::function<void()>;
    // A table option that can be carried out of the menu: see Item::dragOut.
    using DragOut = std::function<bool()>;

    // A host-drawn flyout hung off a list row in place of a child list (see
    // Item::panel). The popup sizes it `minW` x `h`, places it beside the row as
    // it would a submenu, fills it with the popup skin and calls render() to draw
    // its content. While it is showing, every event is offered to handle() first,
    // so the panel can host its own controls (fields, buttons) without a click
    // there dismissing the popup.
    struct Panel {
        float h = 0.0f;
        float minW = 0.0f;
        std::function<void(SDL_Renderer*, const SDL_FRect&)> render;
        std::function<bool(const SDL_Event&, const SDL_FRect&)> handle; // true = consumed
    };

    struct Item {
        // A group divider: a thin rule instead of a row. Takes no hover, no click
        // and no label; list mode only.
        bool separator = false;
        std::string label;
        bool checked = false;         // draws a check mark on the left (current value)
        // Greyed out: the row draws in the disabled color and takes neither hover
        // nor clicks — for an action that is listed so it stays discoverable, but
        // has nothing to do right now. List mode only.
        bool disabled = false;
        // Right-aligned key hint on the row ("Alt+F"), drawn dimmer than the
        // label and in the disabled color when the row is greyed — the same shape
        // the menu bar gives its shortcuts. Widens the popup to fit. List mode only.
        std::string shortcut;
        Action action;                // leaf: run on click. Ignored when children set.
        // Set => the row can be dragged out of the menu, so its press is armed
        // rather than acted on: a release in place runs `action` as a click
        // always has, while moving past the drag threshold calls this instead.
        // Return true if the host took the gesture over, which closes the menu;
        // false leaves the menu up and the row un-acted (the drag had nothing to
        // carry). Table mode only.
        DragOut dragOut;
        std::vector<Item> children;   // non-empty => submenu parent
        // render set => the row is a flyout parent like a submenu one (arrow,
        // opens on hover), but what flies out is this host panel, not a list.
        // Ignored when children are set. List mode only.
        Panel panel;
        // Set => the row is a flyout parent whose flyout is a popup the host
        // owns (another ContextMenu, say): the row shows the arrow and stays
        // highlighted while open like any submenu parent, but nothing is drawn
        // here. sideOpen runs as the row opens — on hover, or a click — with the
        // row's rect, so the host can place its popup beside it; sideClose runs
        // as it folds (another row hovered, a toggle click, the menu closing).
        // Ignored when children or a panel are set. List mode only.
        std::function<void(const SDL_FRect& row)> sideOpen;
        Action sideClose;
        // Row fill for color-picker style lists: packed 0xRRGGBB, 0 = the normal
        // popup fill. The row is painted in the color instead of the selected /
        // hover fill, which shows as an outline instead. List mode only.
        uint32_t rowColor = 0;
        // Label color: packed 0xRRGGBB, 0 = the menu's own text color. Set by
        // hosts that carry a status in the row (the naming-config pickers color
        // an option from Python); wins over the checked row's tint, which the
        // check mark and the row fill already say.
        uint32_t textColor = 0;
        // A status the row carries alongside its label, the other way the pickers
        // annotate an option: it leaves the label and its color alone. The table
        // draws it as a small square in badgeColor (drawStatusSwatch) WITHOUT the
        // word — a column here is as narrow as its options, and a word per row is
        // width it does not have; `badge` only says whether there is a status to
        // draw. The word itself shows in the side panels, which have the room for
        // it (drawStatusBadge). badgeColor 0 = a neutral grey. Table mode only.
        std::string badge;
        uint32_t badgeColor = 0;
    };

    // A vertical column of a table menu: a header label pinned at the bottom of
    // the column, with its options stacked from the top down (top row = items[0]).
    struct Column {
        std::string label;
        std::vector<Item> items;
    };

    void setTextFont(TextFont* f) { textFont_ = f; }

    // Open with the popup's bottom-left anchored near (x, y); it grows upward.
    // Clamped to stay within the winW x winH window.
    // growDown: anchor the top-left at (x, y) and grow downward instead, for a
    // host whose click point has the room below it rather than above (the player
    // frame, as against a timeline clip with the tracks underneath).
    void open(float x, float y, float winW, float winH, std::vector<Item> items,
              bool growDown = false);

    // Open as a table: columns laid out side by side, each with a header label
    // pinned at the bottom and its options stacked from the top. The bottom-left is
    // anchored near (x, y); clamped to stay within the window.
    // keepMinSizes: keep the accumulated sticky column widths / row count instead
    // of resetting them. Set when re-opening in place during a cascade (a
    // navigation click that rebuilds the columns) so the panel doesn't jump
    // smaller; leave false for a genuinely fresh open.
    // growDown: anchor the top-left at (x, y) instead and grow downward, as open()'s.
    // beside: when non-empty, the table hangs off this rect's right edge instead
    // of centering on x — flipped to its left edge when there is no room — as a
    // submenu does off its parent row (see Item::sideOpen).
    // maxH: when > 0, the table (header included) is no taller than this — the
    // popup it flies out of, say; longer columns scroll within it.
    void openTable(float x, float y, float winW, float winH, std::vector<Column> columns,
                   bool keepMinSizes = false, bool growDown = false,
                   const SDL_FRect& beside = {}, float maxH = 0.0f);

    // Open as a swatch grid: the items laid out left-to-right, top-to-bottom in
    // `cols` columns and painted in their Item::rowColor with no label — a color
    // picker where the fill is the whole row. The bottom-left is anchored near
    // (x, y) and it grows upward, as in list mode.
    void openGrid(float x, float y, float winW, float winH, std::vector<Item> items, int cols);

    // Pixel height of a grid popup of this shape, so a host that wants the grid
    // *below* its button can offset the anchor by it.
    static float gridHeight(int items, int cols);

    // One option's display attributes, for relabelTable().
    struct Label {
        std::string text;
        uint32_t textColor = 0;
        std::string badge;
        uint32_t badgeColor = 0;
    };

    // Take new labels/colors into the open table in place, keeping the hovered
    // row, the per-column scroll and the laid-out geometry. For results that
    // arrive in instalments over seconds (the pickers' decoration pass): a
    // reopen would clear the highlight under a resting cursor and scroll a long
    // column the user is reading back to the top, several times over.
    // `columns` must still describe the same table — same column count, same
    // option count in each — which is exactly what decoration guarantees (it may
    // change no option's identity, order or number). Returns false if it doesn't,
    // so the host can fall back to reopening. Only the text, color and badge are
    // taken; the check marks and the click actions stay as they were.
    bool relabelTable(const std::vector<std::vector<Label>>& columns);

    void close();
    bool isOpen() const { return open_; }
    // True while a table column's scrollbar is held: the motion is the bar's
    // wherever the cursor goes, so a host routing it elsewhere should not.
    bool scrollDragging() const;
    // True while a row's Item::panel is the open flyout. For a host whose panel
    // holds state (a focused field) that must end when the flyout goes, which
    // hovering another row does without closing the menu.
    bool panelOpen() const;

    // Table "busy" state: while set, the panel stays open but option clicks are
    // ignored (a background query is resolving the click). Escape and a click
    // outside still work, so dismissing the panel cancels the query. The host
    // drives this and draws its own LOADING overlay over tableBounds().
    void setLoading(bool v) { loading_ = v; }

    // Returns true if the event was consumed (host should skip its own handling).
    bool handleEvent(const SDL_Event& e);

    // True if the point falls on the open popup, in whichever mode it was opened
    // (list rows, table columns, swatch grid) or on an open
    // flyout. False once it is closed. The popup already swallows the events that
    // land there; this is for a host whose widgets read the cursor position
    // directly each frame rather than from motion events, which would otherwise
    // hover under a panel drawn over them.
    bool pointInPopup(float x, float y) const;

    // Recomputes geometry for the current window/anchor, then draws. Call last so
    // the popup overlays everything.
    void render(SDL_Renderer* r);

    // Bounding box of the list popup as last laid out: the rows and any open
    // submenu or panel. Empty in table/grid mode, and until the first render
    // after open() (layout runs there). For a host that opens the menu on hover
    // and needs to know when the cursor has left it.
    SDL_FRect listBounds() const;

    // Bounding box of the table's columns as last laid out (empty if not a table
    // or nothing laid out yet). Stays valid after close() until the next open or
    // layout, so a host can anchor a transient overlay (e.g. "LOADING …") over the
    // panel that was just clicked, even though the click already closed the menu.
    SDL_FRect tableBounds() const;

private:
    static constexpr float kRowH   = 20.0f;
    static constexpr float kSepH   = 5.0f;  // height of a separator row

    // List geometry walks the items rather than multiplying by a row height,
    // since separator rows are shorter than the rest.
    static float rowHeight(const Item& it) { return it.separator ? kSepH : kRowH; }
    static float listHeight(const std::vector<Item>& items);
    static float rowTop(const std::vector<Item>& items, int index);
    // Index of the row `dy` pixels down the list's content, or -1 past the end
    // or on a separator (which is not selectable).
    static int rowIndexAt(const std::vector<Item>& items, float dy);
    static constexpr float kPadX   = 10.0f; // text inset from the box edge
    static constexpr float kMarkW  = 16.0f; // left column reserved for the check mark
    static constexpr float kArrowW = 16.0f; // right column reserved for the submenu arrow
    static constexpr float kShortcutGapX = 24.0f; // min gap between a label and its key hint
    static constexpr float kMinW   = 120.0f;
    static constexpr float kDragThreshold = 4.0f; // travel that turns an armed press into a drag
    static constexpr float kCell   = 18.0f; // grid mode: swatch cell pitch
    static constexpr float kGridPad = 3.0f; // grid mode: inset from the box edge

    TextFont* textFont_ = nullptr;
    bool open_ = false;
    bool table_ = false;   // true => columns_ table mode; false => items_ list mode
    bool grid_ = false;    // true => items_ drawn as a gridCols_-wide swatch grid
    bool growDown_ = false; // list/table mode: anchor the top at anchorY_, not the bottom

    std::vector<Item> items_;
    float anchorX_ = 0, anchorY_ = 0;
    float winW_ = 0, winH_ = 0;

    int hoverRoot_ = -1;   // hovered root row, -1 = none
    int openParent_ = -1;  // root row whose submenu is open, -1 = none
    int hoverChild_ = -1;  // hovered submenu row, -1 = none
    // Every change of openParent_ goes through here, so a row's sideOpen /
    // sideClose hooks run exactly once per open / fold.
    void setOpenParent(int row);

    SDL_FRect rootRect_{};  // recomputed in layout()
    SDL_FRect childRect_{}; // submenu or panel; valid only when openParent_ >= 0

    // Scroll offsets (pixels) for when a list is taller than the window and gets
    // clamped to it; each list/submenu scrolls independently via the wheel.
    float rootScroll_ = 0;
    float childScroll_ = 0;
    int scrolledParent_ = -1; // openParent_ that childScroll_ currently applies to

    // ---- table mode
    std::vector<Column> columns_;
    std::vector<SDL_FRect> colRects_; // per-column rect (header + options), recomputed in layout
    float tableY0_ = 0;               // top of the table
    SDL_FRect beside_{};              // openTable's beside: empty = center on the anchor
    float maxTableH_ = 0;             // openTable's maxH: 0 = only the window cap
    int tableRows_ = 0;               // option rows above the header (max column length)
    int visRows_ = 0;                 // visible option rows (<= tableRows_ when clamped to window)
    std::vector<float> colScroll_;    // per-column option scroll offset (pixels), parallel to columns_
    std::vector<ScrollbarDrag> colSb_; // per-column grabbable scrollbar, parallel to columns_
    int hoverCol_ = -1;               // hovered column, -1 = none
    int hoverItem_ = -1;              // hovered option index within hoverCol_, -1 = none/header
    bool loading_ = false;            // a click's background query is in flight (see setLoading)
    // Armed press on a dragOut row: acted on at release, or handed to the host
    // once it moves past kDragThreshold. -1 = nothing armed.
    int dragCol_ = -1, dragItem_ = -1;
    float dragPressX_ = 0, dragPressY_ = 0;

    // ---- grid mode
    int gridCols_ = 0;
    int hoverCell_ = -1;              // hovered swatch, -1 = none
    SDL_FRect gridRect_{};
    // Sticky minimum sizes carried across a cascade rebuild (keepMinSizes).
    // The panel only ever grows to fit new content: a column never gets narrower
    // than it has been, and the table never gets shorter. Reset on a fresh open.
    std::vector<float> minColWidths_; // per-column, parallel to columns_
    int minTableRows_ = 0;            // option rows above the header

    void layout(SDL_Renderer* r);
    void layoutTable(SDL_Renderer* r);
    void layoutGrid();
    int gridCellAt(float x, float y) const;
    float listWidth(SDL_Renderer* r, const std::vector<Item>& items, bool reserveArrow) const;
    int rootRowAt(float x, float y) const;
    int childRowAt(float x, float y) const;
    // Resolve (x,y) to a column and option index. Returns false if outside the
    // table; sets col, and item (>=0 for an option, -1 for the header row).
    bool tableCellAt(float x, float y, int& col, int& item) const;
};

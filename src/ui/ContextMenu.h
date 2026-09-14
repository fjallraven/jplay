#pragma once

#include "TextFont.h"

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
        std::vector<Item> children;   // non-empty => submenu parent
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
    // the column, with its options stacked above it (top row = items[0]).
    struct Column {
        std::string label;
        std::vector<Item> items;
    };

    void setTextFont(TextFont* f) { textFont_ = f; }

    // An optional host-drawn band pinned above the rows (list mode only). The
    // popup reserves `h` pixels there, widens to at least `minW`, fills the band
    // with the popup skin and calls render() to draw its content. While the menu
    // is open every event is offered to handle() first, so the band can host its
    // own controls (fields, buttons) without a click there dismissing the popup.
    struct Header {
        float h = 0.0f;
        float minW = 0.0f;
        std::function<void(SDL_Renderer*, const SDL_FRect&)> render;
        std::function<bool(const SDL_Event&, const SDL_FRect&)> handle; // true = consumed
    };

    // Open with the popup's bottom-left anchored near (x, y); it grows upward.
    // Clamped to stay within the winW x winH window.
    // growDown: anchor the top-left at (x, y) and grow downward instead, for a
    // host whose click point has the room below it rather than above (the player
    // frame, as against a timeline clip with the tracks underneath).
    // (Two overloads rather than a defaulted `Header header = {}`: GCC parses a
    // default argument before the nested Header's member initializers, so the
    // empty braces there fail to convert.)
    void open(float x, float y, float winW, float winH, std::vector<Item> items,
              Header header, bool growDown = false);
    void open(float x, float y, float winW, float winH, std::vector<Item> items) {
        open(x, y, winW, winH, std::move(items), Header{});
    }

    // Open as a table: columns laid out side by side, each with a header label
    // pinned at the bottom and its options growing upward. The bottom-left is
    // anchored near (x, y); clamped to stay within the window.
    // keepMinSizes: keep the accumulated sticky column widths / row count instead
    // of resetting them. Set when re-opening in place during a cascade (a
    // navigation click that rebuilds the columns) so the panel doesn't jump
    // smaller; leave false for a genuinely fresh open.
    void openTable(float x, float y, float winW, float winH, std::vector<Column> columns,
                   bool keepMinSizes = false);

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

    void close() { open_ = false; openParent_ = -1; hoverRoot_ = -1; hoverChild_ = -1;
                   hoverCol_ = -1; hoverItem_ = -1; hoverCell_ = -1; loading_ = false; }
    bool isOpen() const { return open_; }

    // Table "busy" state: while set, the panel stays open but option clicks are
    // ignored (a background query is resolving the click). Escape and a click
    // outside still work, so dismissing the panel cancels the query. The host
    // drives this and draws its own LOADING overlay over tableBounds().
    void setLoading(bool v) { loading_ = v; }

    // Returns true if the event was consumed (host should skip its own handling).
    bool handleEvent(const SDL_Event& e);

    // True if the point falls on the open popup, in whichever mode it was opened
    // (list rows and header band, table columns, swatch grid) or on an open
    // flyout. False once it is closed. The popup already swallows the events that
    // land there; this is for a host whose widgets read the cursor position
    // directly each frame rather than from motion events, which would otherwise
    // hover under a panel drawn over them.
    bool pointInPopup(float x, float y) const;

    // Recomputes geometry for the current window/anchor, then draws. Call last so
    // the popup overlays everything.
    void render(SDL_Renderer* r);

    // Bounding box of the list popup as last laid out: the rows, the header band
    // and any open submenu. Empty in table/grid mode, and until the first render
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
    static constexpr float kCell   = 18.0f; // grid mode: swatch cell pitch
    static constexpr float kGridPad = 3.0f; // grid mode: inset from the box edge

    TextFont* textFont_ = nullptr;
    bool open_ = false;
    bool table_ = false;   // true => columns_ table mode; false => items_ list mode
    bool grid_ = false;    // true => items_ drawn as a gridCols_-wide swatch grid
    bool growDown_ = false; // list mode: anchor the top at anchorY_, not the bottom

    std::vector<Item> items_;
    float anchorX_ = 0, anchorY_ = 0;
    float winW_ = 0, winH_ = 0;

    int hoverRoot_ = -1;   // hovered root row, -1 = none
    int openParent_ = -1;  // root row whose submenu is open, -1 = none
    int hoverChild_ = -1;  // hovered submenu row, -1 = none

    SDL_FRect rootRect_{};  // recomputed in layout()
    SDL_FRect childRect_{}; // valid only when openParent_ >= 0
    Header    header_;      // list mode only; render unset = no header band
    SDL_FRect headerRect_{}; // the band above rootRect_ (empty when no header)

    // Scroll offsets (pixels) for when a list is taller than the window and gets
    // clamped to it; each list/submenu scrolls independently via the wheel.
    float rootScroll_ = 0;
    float childScroll_ = 0;
    int scrolledParent_ = -1; // openParent_ that childScroll_ currently applies to

    // ---- table mode
    std::vector<Column> columns_;
    std::vector<SDL_FRect> colRects_; // per-column rect (header + options), recomputed in layout
    float tableY0_ = 0;               // top of the table
    int tableRows_ = 0;               // option rows above the header (max column length)
    int visRows_ = 0;                 // visible option rows (<= tableRows_ when clamped to window)
    std::vector<float> colScroll_;    // per-column option scroll offset (pixels), parallel to columns_
    int hoverCol_ = -1;               // hovered column, -1 = none
    int hoverItem_ = -1;              // hovered option index within hoverCol_, -1 = none/header
    bool loading_ = false;            // a click's background query is in flight (see setLoading)

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

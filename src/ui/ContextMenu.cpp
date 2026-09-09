#include "ContextMenu.h"
#include "MenuDraw.h"
#include "PixelSnap.h"
#include "UiColors.h"
#include "SkinColors.h"
#include "Widgets.h"

#include <algorithm>

// ---- palette (mirrors DropdownBar so the popup reads as the same widget family)
namespace {

// References into the live skin palette; see jplay::colors().
const SDL_Color& kListBg    = jplay::colors().dropBg;       // popup fill
const SDL_Color& kBorder    = jplay::colors().borderStrong; // outline
const SDL_Color& kValueText = jplay::colors().text;         // row text
const SDL_Color& kSelected  = kMenuSelectedBg;              // selected (checked) row background
const SDL_Color& kMark      = jplay::colors().selText;      // check mark + submenu arrow
const SDL_Color& kSepLine   = jplay::colors().menuSep;      // group divider rule
const SDL_Color& kShortcutTxt = jplay::colors().menuShortcut;    // right-aligned key hint
const SDL_Color& kShortcutOff = jplay::colors().menuShortcutOff; // key hint on a greyed row
constexpr SDL_Color kHeaderBg  { 30, 21, 36, 255 };    // table column header fill
constexpr SDL_Color kHeaderBgHot  { 36, 50, 96, 255 };    // header fill when hovered (matches sequence-bar blue)
constexpr SDL_Color kHeaderTxt    { 160, 165, 175, 255 }; // table column header label
constexpr SDL_Color kHeaderTxtHot { 255, 255, 255, 255 }; // header label when its column is under the cursor
constexpr SDL_Color kBorderHot    { 150, 165, 190, 255 }; // column outline when it is under the cursor
constexpr SDL_Color kDisabledTxt  { 92, 94, 101, 255 };   // greyed-out row label
constexpr SDL_Color kTableBg      { 17, 18, 21, 255 };       // table column fill (black when not hovered)
constexpr SDL_Color kTableBgHot   { 20, 21, 24, 255 };    // table column fill when hovered (dark grey)

inline void setColor(SDL_Renderer* r, SDL_Color c) {
    SDL_SetRenderDrawColor(r, c.r, c.g, c.b, c.a);
}

bool hasChildren(const ContextMenu::Item& it) { return !it.children.empty(); }

// A row's label color: grey when the row is disabled, else its own packed
// 0xRRGGBB when it carries one, else the menu's own text color.
SDL_Color labelColor(const ContextMenu::Item& it) {
    if (it.disabled)
        return kDisabledTxt;
    if (!it.textColor)
        return kValueText;
    return { (Uint8)((it.textColor >> 16) & 0xFF), (Uint8)((it.textColor >> 8) & 0xFF),
             (Uint8)(it.textColor & 0xFF), 255 };
}

} // namespace

float ContextMenu::listHeight(const std::vector<Item>& items) {
    float h = 0.0f;
    for (const auto& it : items)
        h += rowHeight(it);
    return h;
}

float ContextMenu::rowTop(const std::vector<Item>& items, int index) {
    float y = 0.0f;
    for (int i = 0; i < index && i < (int)items.size(); ++i)
        y += rowHeight(items[i]);
    return y;
}

int ContextMenu::rowIndexAt(const std::vector<Item>& items, float dy) {
    if (dy < 0.0f)
        return -1;
    float y = 0.0f;
    for (int i = 0; i < (int)items.size(); ++i) {
        float next = y + rowHeight(items[i]);
        if (dy < next)
            return (items[i].separator || items[i].disabled) ? -1 : i;
        y = next;
    }
    return -1;
}

void ContextMenu::open(float x, float y, float winW, float winH, std::vector<Item> items,
                       Header header, bool growDown) {
    items_ = std::move(items);
    columns_.clear();
    table_ = false;
    grid_ = false;
    growDown_ = growDown;
    header_ = std::move(header);
    headerRect_ = {};
    anchorX_ = x;
    anchorY_ = y;
    winW_ = winW;
    winH_ = winH;
    open_ = !items_.empty();
    hoverRoot_ = -1;
    openParent_ = -1;
    hoverChild_ = -1;
    hoverCol_ = -1;
    hoverItem_ = -1;
    rootRect_ = {};
    childRect_ = {};
    rootScroll_ = 0;
    childScroll_ = 0;
    scrolledParent_ = -1;
}

void ContextMenu::openTable(float x, float y, float winW, float winH, std::vector<Column> columns,
                            bool keepMinSizes) {
    columns_ = std::move(columns);
    items_.clear();
    table_ = true;
    grid_ = false;
    growDown_ = false;
    header_ = {};
    headerRect_ = {};
    anchorX_ = x;
    anchorY_ = y;
    winW_ = winW;
    winH_ = winH;
    open_ = !columns_.empty();
    hoverRoot_ = -1;
    openParent_ = -1;
    hoverChild_ = -1;
    hoverCol_ = -1;
    hoverItem_ = -1;
    colRects_.clear();
    colScroll_.clear();
    loading_ = false; // fresh columns: not waiting on a query
    if (!keepMinSizes) {
        minColWidths_.clear();  // fresh open: forget a prior cascade's min sizes
        minTableRows_ = 0;
    }
}

void ContextMenu::openGrid(float x, float y, float winW, float winH, std::vector<Item> items,
                           int cols) {
    open(x, y, winW, winH, std::move(items)); // same state reset as a list
    grid_ = true;
    gridCols_ = std::max(1, cols);
    hoverCell_ = -1;
    gridRect_ = {};
}

float ContextMenu::gridHeight(int items, int cols) {
    int rows = (items + std::max(1, cols) - 1) / std::max(1, cols);
    return (float)rows * kCell + 2.0f * kGridPad;
}

bool ContextMenu::relabelTable(const std::vector<std::vector<Label>>& columns) {
    if (!table_ || !open_ || columns.size() != columns_.size())
        return false;
    for (size_t c = 0; c < columns_.size(); ++c)
        if (columns[c].size() != columns_[c].items.size())
            return false;
    for (size_t c = 0; c < columns_.size(); ++c)
        for (size_t i = 0; i < columns[c].size(); ++i) {
            columns_[c].items[i].label      = columns[c][i].text;
            columns_[c].items[i].textColor  = columns[c][i].textColor;
            columns_[c].items[i].badge      = columns[c][i].badge;
            columns_[c].items[i].badgeColor = columns[c][i].badgeColor;
        }
    return true; // render() re-measures the columns from these each frame
}

// ---------------------------------------------------------------- geometry

float ContextMenu::listWidth(SDL_Renderer* r, const std::vector<Item>& items, bool reserveArrow) const {
    auto measure = [&](const std::string& s) {
        return textFont_ ? textFont_->measure(r, s.c_str()) : (float)s.size() * 8.0f;
    };
    float maxLabel = 0.0f, maxShortcut = 0.0f;
    for (const auto& it : items) {
        maxLabel = std::max(maxLabel, measure(it.label));
        maxShortcut = std::max(maxShortcut, measure(it.shortcut));
    }
    float w = kMarkW + maxLabel + kPadX + (reserveArrow ? kArrowW : kPadX);
    // The hints share the right inset with the submenu arrow, so a list with both
    // keeps them clear of each other.
    if (maxShortcut > 0.0f)
        w += kShortcutGapX + maxShortcut;
    return std::max(w, kMinW);
}

void ContextMenu::layoutTable(SDL_Renderer* r) {
    colRects_.clear();
    if (columns_.empty())
        return;

    auto measure = [&](const std::string& s) {
        return textFont_ ? textFont_->measure(r, s.c_str()) : (float)s.size() * 8.0f;
    };

    // Per-column width from the widest of its header and options; overall height
    // is one header row plus the longest column's option count.
    std::vector<float> widths(columns_.size());
    tableRows_ = 0;
    for (size_t c = 0; c < columns_.size(); ++c) {
        float maxLabel = measure(columns_[c].label);
        for (const auto& it : columns_[c].items)
            maxLabel = std::max(maxLabel, kMarkW + measure(it.label) +
                                             (it.badge.empty() ? 0.0f : statusSwatchWidth(1.0f)));
        widths[c] = std::max(maxLabel + kMarkW + kPadX, kMinW);
        tableRows_ = std::max(tableRows_, (int)columns_[c].items.size());
    }

    // Sticky min sizes across a cascade rebuild: never let a column get narrower
    // or the table shorter than it has previously been, so re-selecting content
    // with fewer/shorter options keeps the panel at its grown size instead of
    // jumping smaller. Cleared on a fresh open (see openTable).
    minColWidths_.resize(columns_.size(), 0.0f);
    for (size_t c = 0; c < columns_.size(); ++c) {
        widths[c] = std::max(widths[c], minColWidths_[c]);
        minColWidths_[c] = widths[c];
    }
    tableRows_ = std::max(tableRows_, minTableRows_);
    minTableRows_ = tableRows_;

    // Cap the table to 60% of the window height (reserving the header row);
    // longer columns then scroll independently within this shared viewport.
    int maxTotalRows = std::max(1, (int)(winH_ * 0.6f / kRowH));
    visRows_ = std::min(tableRows_, std::max(1, maxTotalRows - 1));

    float totalW = 0.0f;
    for (float w : widths) totalW += w;
    float totalH = (float)(visRows_ + 1) * kRowH; // +1 header row

    // Center the table horizontally on the cursor; anchor its bottom there and
    // grow upward, then clamp on screen.
    float x0 = anchorX_ - totalW * 0.5f;
    float y0 = anchorY_ - totalH;
    if (x0 + totalW > winW_) x0 = winW_ - totalW;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (y0 + totalH > winH_) y0 = winH_ - totalH;
    tableY0_ = y0;

    float cx = x0;
    for (size_t c = 0; c < columns_.size(); ++c) {
        colRects_.push_back(SDL_FRect{ cx, y0, widths[c], totalH });
        cx += widths[c];
    }

    // Keep per-column scroll offsets sized to the column set and in range.
    colScroll_.resize(columns_.size(), 0.0f);
    for (size_t c = 0; c < columns_.size(); ++c) {
        float maxS = std::max(0.0f, (float)((int)columns_[c].items.size() - visRows_) * kRowH);
        colScroll_[c] = std::clamp(colScroll_[c], 0.0f, maxS);
    }
}

SDL_FRect ContextMenu::listBounds() const {
    if (table_ || grid_ || rootRect_.w <= 0.0f)
        return {};
    float l = rootRect_.x, t = rootRect_.y;
    float r = l + rootRect_.w, b = t + rootRect_.h;
    auto join = [&](const SDL_FRect& o) {
        if (o.w <= 0.0f || o.h <= 0.0f)
            return;
        l = std::min(l, o.x);
        t = std::min(t, o.y);
        r = std::max(r, o.x + o.w);
        b = std::max(b, o.y + o.h);
    };
    join(headerRect_);
    if (openParent_ >= 0)
        join(childRect_);
    return { l, t, r - l, b - t };
}

SDL_FRect ContextMenu::tableBounds() const {
    if (colRects_.empty())
        return {};
    float l = colRects_.front().x;
    float t = colRects_.front().y;
    float r = l, b = t;
    for (const SDL_FRect& c : colRects_) {
        l = std::min(l, c.x);
        t = std::min(t, c.y);
        r = std::max(r, c.x + c.w);
        b = std::max(b, c.y + c.h);
    }
    return { l, t, r - l, b - t };
}

bool ContextMenu::tableCellAt(float x, float y, int& col, int& item) const {
    col = -1;
    item = -1;
    if (colRects_.empty())
        return false;
    const float hy = tableY0_ + visRows_ * kRowH; // header row top
    for (size_t c = 0; c < colRects_.size(); ++c) {
        const SDL_FRect& b = colRects_[c];
        if (x < b.x || x >= b.x + b.w || y < b.y || y >= b.y + b.h)
            continue;
        col = (int)c;
        if (y >= hy) { item = -1; return true; } // header row at the bottom
        // Options are bottom-aligned just above the header (item n-1 nearest it)
        // and shifted by this column's scroll; solve which option row y hits.
        int n = (int)columns_[c].items.size();
        float scroll = (c < colScroll_.size()) ? colScroll_[c] : 0.0f;
        int k = (int)((hy - (y - scroll)) / kRowH) + 1; // 1 = nearest the header
        int i = n - k;
        item = (i >= 0 && i < n) ? i : -1;
        return true;
    }
    return false;
}

void ContextMenu::layoutGrid() {
    const int rows = ((int)items_.size() + gridCols_ - 1) / gridCols_;
    const float w = (float)gridCols_ * kCell + 2.0f * kGridPad;
    const float h = (float)rows * kCell + 2.0f * kGridPad;
    // Anchor the bottom-left at the open point and grow upward, then clamp on screen.
    float x0 = anchorX_, y0 = anchorY_ - h;
    if (x0 + w > winW_) x0 = winW_ - w;
    if (x0 < 0) x0 = 0;
    if (y0 + h > winH_) y0 = winH_ - h;
    if (y0 < 0) y0 = 0;
    gridRect_ = SDL_FRect{ x0, y0, w, h };
}

int ContextMenu::gridCellAt(float x, float y) const {
    const SDL_FRect& b = gridRect_;
    const float gx = x - (b.x + kGridPad), gy = y - (b.y + kGridPad);
    if (gx < 0 || gy < 0)
        return -1;
    const int cx = (int)(gx / kCell), cy = (int)(gy / kCell);
    if (cx >= gridCols_)
        return -1;
    const int i = cy * gridCols_ + cx;
    return (i >= 0 && i < (int)items_.size()) ? i : -1;
}

void ContextMenu::layout(SDL_Renderer* r) {
    if (!open_)
        return;

    if (table_) { layoutTable(r); return; }
    if (grid_) { layoutGrid(); return; }

    bool anyChild = std::any_of(items_.begin(), items_.end(), hasChildren);
    float w = std::max(listWidth(r, items_, anyChild), header_.minW);
    // The header band sits above the rows; both move as one block.
    const float hh = header_.render ? header_.h : 0.0f;
    float fullH = listHeight(items_);
    float h = std::min(fullH, std::max(kRowH, winH_ - hh)); // clamp tall lists; they scroll

    // Anchor at the cursor and grow upward — or downward from it when the host
    // asked for that — then clamp on screen.
    float x0 = anchorX_;
    float y0 = growDown_ ? anchorY_ : anchorY_ - h - hh;
    if (x0 + w > winW_) x0 = winW_ - w;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (y0 + h + hh > winH_) y0 = winH_ - h - hh;
    headerRect_ = SDL_FRect{ x0, y0, w, hh };
    rootRect_ = SDL_FRect{ x0, y0 + hh, w, h };
    rootScroll_ = std::clamp(rootScroll_, 0.0f, std::max(0.0f, fullH - h));

    if (openParent_ >= 0 && openParent_ < (int)items_.size() && hasChildren(items_[openParent_])) {
        // Reset the submenu scroll when a different submenu opens.
        if (openParent_ != scrolledParent_) { childScroll_ = 0; scrolledParent_ = openParent_; }
        const auto& kids = items_[openParent_].children;
        float cw = listWidth(r, kids, false);
        float fullCh = listHeight(kids);
        float ch = std::min(fullCh, winH_);
        // Fly out to the right of the parent; flip to the left if it would overflow.
        float cx = rootRect_.x + rootRect_.w - 1.0f;
        if (cx + cw > winW_) cx = rootRect_.x - cw + 1.0f;
        if (cx < 0) cx = 0;
        // Align the submenu with the parent row's (scrolled) edge, growing the same
        // way the root list does: bottom-to-bottom upward, or top-to-top downward.
        float parentTop = rootRect_.y - rootScroll_ + rowTop(items_, openParent_);
        float cy = growDown_ ? parentTop : parentTop + kRowH - ch;
        if (cy < 0) cy = 0;
        if (cy + ch > winH_) cy = winH_ - ch;
        childRect_ = SDL_FRect{ cx, cy, cw, ch };
        childScroll_ = std::clamp(childScroll_, 0.0f, std::max(0.0f, fullCh - ch));
    } else {
        childRect_ = {};
    }
}

int ContextMenu::rootRowAt(float x, float y) const {
    const SDL_FRect& b = rootRect_;
    if (x < b.x || x >= b.x + b.w || y < b.y || y >= b.y + b.h)
        return -1;
    return rowIndexAt(items_, y - b.y + rootScroll_);
}

int ContextMenu::childRowAt(float x, float y) const {
    if (openParent_ < 0)
        return -1;
    const SDL_FRect& b = childRect_;
    if (x < b.x || x >= b.x + b.w || y < b.y || y >= b.y + b.h)
        return -1;
    const auto& kids = items_[openParent_].children;
    return rowIndexAt(kids, y - b.y + childScroll_);
}

// ---------------------------------------------------------------- input

bool ContextMenu::handleEvent(const SDL_Event& e) {
    if (!open_)
        return false;

    if (table_) {
        switch (e.type) {
        case SDL_EVENT_MOUSE_MOTION: {
            float x = e.motion.x, y = e.motion.y;
            // While loading the columns are hidden, so don't track hover.
            if (!loading_) {
                int col, item;
                // Track the hovered column even over its header or empty rows so
                // the whole column reads as the active panel while rendering.
                if (tableCellAt(x, y, col, item)) {
                    hoverCol_ = col;
                    hoverItem_ = item;
                } else {
                    hoverCol_ = -1;
                    hoverItem_ = -1;
                }
            }
            return true; // consume motion while open so host hover doesn't leak
        }
        case SDL_EVENT_MOUSE_WHEEL: {
            // Scroll the column under the cursor independently of the others.
            int col, item;
            if (tableCellAt(e.wheel.mouse_x, e.wheel.mouse_y, col, item) &&
                col >= 0 && col < (int)colScroll_.size()) {
                float maxS = std::max(0.0f,
                    (float)((int)columns_[col].items.size() - visRows_) * kRowH);
                colScroll_[col] = std::clamp(colScroll_[col] + e.wheel.y * kRowH * 2.0f,
                                             0.0f, maxS);
            }
            return true;
        }
        case SDL_EVENT_MOUSE_BUTTON_DOWN: {
            int col, item;
            if (tableCellAt(e.button.x, e.button.y, col, item)) {
                // Run the option's action but keep the panel open: the action
                // kicks off a background query and calls setLoading(true); the
                // host swaps in the result (or closes) when it completes. Ignore
                // clicks while a query is already in flight.
                if (item >= 0 && !loading_) {
                    Action a = columns_[col].items[item].action;
                    if (a) a();
                }
                return true; // clicks inside the table (incl. header) stay captured
            }
            // Outside: dismiss. Swallow a left click; let a right click fall
            // through so the host can reopen the menu elsewhere.
            close();
            return e.button.button != SDL_BUTTON_RIGHT;
        }
        case SDL_EVENT_KEY_DOWN:
            if (e.key.key == SDLK_ESCAPE) { close(); return true; }
            return false;
        default:
            return false;
        }
    }

    if (grid_) {
        switch (e.type) {
        case SDL_EVENT_MOUSE_MOTION:
            hoverCell_ = gridCellAt(e.motion.x, e.motion.y);
            return true; // consume motion while open so host hover doesn't leak
        case SDL_EVENT_MOUSE_BUTTON_DOWN: {
            int i = gridCellAt(e.button.x, e.button.y);
            if (i >= 0) {
                Action a = items_[i].action;
                close();
                if (a) a();
                return true;
            }
            // Outside the popup: dismiss. Swallow a left click (dismiss only); let a
            // right click fall through so the host can reopen the menu elsewhere.
            close();
            return e.button.button != SDL_BUTTON_RIGHT;
        }
        case SDL_EVENT_KEY_DOWN:
            if (e.key.key == SDLK_ESCAPE) { close(); return true; }
            return false;
        default:
            return false;
        }
    }

    // The header band owns its controls: give it first refusal on everything, so
    // clicking a field or typing into it isn't read as a row click / dismissal.
    if (header_.handle && header_.handle(e, headerRect_))
        return true;

    switch (e.type) {
    case SDL_EVENT_MOUSE_MOTION: {
        float x = e.motion.x, y = e.motion.y;
        int cr = childRowAt(x, y);
        if (cr >= 0) {
            hoverChild_ = cr; // stays within the open submenu
            return true;
        }
        int rr = rootRowAt(x, y);
        if (rr >= 0) {
            hoverRoot_ = rr;
            hoverChild_ = -1;
            // Hovering a parent opens its submenu; hovering a leaf closes any open one.
            openParent_ = hasChildren(items_[rr]) ? rr : -1;
        } else {
            hoverRoot_ = -1; // keep openParent_ so the cursor can travel to the submenu
        }
        return true; // consume motion while open so host hover doesn't leak
    }
    case SDL_EVENT_MOUSE_WHEEL: {
        // Scroll the submenu if the cursor is over it, else the root list.
        float x = e.wheel.mouse_x, y = e.wheel.mouse_y;
        auto within = [](const SDL_FRect& b, float px, float py) {
            return px >= b.x && px < b.x + b.w && py >= b.y && py < b.y + b.h;
        };
        if (openParent_ >= 0 && within(childRect_, x, y)) {
            float fullH = listHeight(items_[openParent_].children);
            childScroll_ = std::clamp(childScroll_ - e.wheel.y * kRowH * 2.0f,
                                      0.0f, std::max(0.0f, fullH - childRect_.h));
        } else if (within(rootRect_, x, y)) {
            float fullH = listHeight(items_);
            rootScroll_ = std::clamp(rootScroll_ - e.wheel.y * kRowH * 2.0f,
                                     0.0f, std::max(0.0f, fullH - rootRect_.h));
        }
        return true;
    }
    case SDL_EVENT_MOUSE_BUTTON_DOWN: {
        float x = e.button.x, y = e.button.y;
        int cr = childRowAt(x, y);
        if (cr >= 0) {
            Action a = items_[openParent_].children[cr].action;
            close();
            if (a) a();
            return true;
        }
        int rr = rootRowAt(x, y);
        if (rr >= 0) {
            if (hasChildren(items_[rr])) {
                openParent_ = (openParent_ == rr) ? -1 : rr; // toggle its submenu
                hoverChild_ = -1;
                return true;
            }
            Action a = items_[rr].action;
            close();
            if (a) a();
            return true;
        }
        // Outside the popup: dismiss. Swallow a left click (dismiss only); let a
        // right click fall through so the host can reopen the menu elsewhere.
        close();
        return e.button.button != SDL_BUTTON_RIGHT;
    }
    case SDL_EVENT_KEY_DOWN:
        if (e.key.key == SDLK_ESCAPE) { close(); return true; }
        return false;
    default:
        return false;
    }
}

// ---------------------------------------------------------------- rendering

namespace {

// A small check mark drawn as two strokes, vertically centered in the row.
void drawCheck(SDL_Renderer* r, float x, float cy) {
    jplay::drawLine(r, x, cy + 1.0f, x + 3.0f, cy + 4.0f);
    jplay::drawLine(r, x + 3.0f, cy + 4.0f, x + 8.0f, cy - 3.0f);
}

// A right-pointing arrow (submenu indicator).
void drawArrow(SDL_Renderer* r, float x, float cy) {
    jplay::drawLine(r, x, cy - 3.0f, x + 3.0f, cy);
    jplay::drawLine(r, x + 3.0f, cy, x, cy + 3.0f);
}

// A small chevron centered at (cx, cy); points up when `up`, else down. Hints
// that a scrollable list has more content in that direction.
void drawChevron(SDL_Renderer* r, float cx, float cy, bool up) {
    float dy = up ? 2.0f : -2.0f;
    jplay::drawLine(r, cx - 4.0f, cy + dy, cx, cy - dy);
    jplay::drawLine(r, cx, cy - dy, cx + 4.0f, cy + dy);
}

} // namespace

void ContextMenu::render(SDL_Renderer* r) {
    layout(r);
    if (!open_)
        return;

    float glyphH = textFont_ ? textFont_->lineHeight() : 8.0f;

    if (table_) {
        const float hy = tableY0_ + visRows_ * kRowH; // header row top
        for (size_t c = 0; c < colRects_.size(); ++c) {
            const SDL_FRect& b = colRects_[c];
            const Column& col = columns_[c];
            int n = (int)col.items.size();
            const bool colHovered = ((int)c == hoverCol_);
            const float scroll = (c < colScroll_.size()) ? colScroll_[c] : 0.0f;

            // Column body fill + outline.
            setColor(r, colHovered ? kTableBgHot : kTableBg);
            jplay::fillRect(r, &b);

            // Options: bottom-aligned just above the header, offset by the column's
            // scroll and clipped to the option viewport (above the header row).
            SDL_Rect clip{ (int)b.x, (int)tableY0_, (int)b.w, (int)(visRows_ * kRowH) };
            SDL_SetRenderClipRect(r, &clip);
            for (int i = 0; i < n; ++i) {
                float ry = hy - (float)(n - i) * kRowH + scroll;
                if (ry + kRowH <= tableY0_ || ry >= hy)
                    continue; // scrolled out of the viewport
                float cy = ry + kRowH * 0.5f;
                if (col.items[i].checked) {
                    SDL_FRect sr{ b.x + 1.0f, ry, b.w - 2.0f, kRowH };
                    setColor(r, kSelected);
                    jplay::fillRect(r, &sr);
                }
                if ((int)c == hoverCol_ && i == hoverItem_)
                    jplay::drawRowHover(r, SDL_FRect{ b.x + 1.0f, ry, b.w - 2.0f, kRowH });
                if (col.items[i].checked) {
                    setColor(r, kMark);
                    drawCheck(r, b.x + 4.0f, cy);
                }
                if (!col.items[i].badge.empty()) // color only: see Item::badge
                    drawStatusSwatch(r, SDL_FRect{ b.x, ry, b.w, kRowH },
                                     col.items[i].badgeColor, 1.0f);
                if (textFont_)
                    textFont_->draw(r, b.x + kMarkW, ry + (kRowH - glyphH) * 0.5f,
                                    labelColor(col.items[i]), col.items[i].label.c_str());
            }
            SDL_SetRenderClipRect(r, nullptr);

            // Scroll hints when this column has more options than fit.
            if (n > visRows_) {
                setColor(r, kMark);
                if (scroll < (float)(n - visRows_) * kRowH - 0.5f)
                    drawChevron(r, b.x + b.w - 8.0f, tableY0_ + 5.0f, true);
                if (scroll > 0.5f)
                    drawChevron(r, b.x + b.w - 8.0f, hy - 5.0f, false);
            }

            // Header row pinned at the bottom of the column.
            SDL_FRect hrect{ b.x, hy, b.w, kRowH };
            setColor(r, colHovered ? kHeaderBgHot : kHeaderBg);
            jplay::fillRect(r, &hrect);
            if (textFont_)
                textFont_->draw(r, b.x + kMarkW, hy + (kRowH - glyphH) * 0.5f,
                                colHovered ? kHeaderTxtHot : kHeaderTxt, col.label.c_str());

            setColor(r, colHovered ? kBorderHot : kBorder);
            jplay::drawRect(r, &b);
        }
        return;
    }

    if (grid_) {
        setColor(r, kListBg);
        jplay::fillRect(r, &gridRect_);
        for (int i = 0; i < (int)items_.size(); ++i) {
            const SDL_FRect cell{ gridRect_.x + kGridPad + (float)(i % gridCols_) * kCell,
                                  gridRect_.y + kGridPad + (float)(i / gridCols_) * kCell,
                                  kCell, kCell };
            const SDL_FRect sw{ cell.x + 1.0f, cell.y + 1.0f, cell.w - 2.0f, cell.h - 2.0f };
            const uint32_t rc = items_[i].rowColor;
            SDL_SetRenderDrawColor(r, (rc >> 16) & 0xFF, (rc >> 8) & 0xFF, rc & 0xFF, 255);
            jplay::fillRect(r, &sw);
            // The fill *is* the value, so neither state can repaint it: the current
            // swatch is outlined, the hovered one gets the wider cell outline.
            setColor(r, kMark);
            if (items_[i].checked)
                jplay::drawRect(r, &sw);
            if (i == hoverCell_)
                jplay::drawRect(r, &cell);
        }
        setColor(r, kBorder);
        jplay::drawRect(r, &gridRect_);
        return;
    }

    auto drawList = [&](const SDL_FRect& box, const std::vector<Item>& items,
                        int hovered, bool reserveArrow, float scroll) {
        setColor(r, kListBg);
        jplay::fillRect(r, &box);
        // Clip rows to the box so a scrolled list doesn't spill past its edges.
        SDL_Rect clip{ (int)box.x, (int)box.y, (int)box.w, (int)box.h };
        SDL_SetRenderClipRect(r, &clip);
        float rowY = box.y - scroll;
        for (int i = 0; i < (int)items.size(); ++i) {
            const float ry = rowY;
            const float rh = rowHeight(items[i]);
            rowY += rh;
            if (ry + rh <= box.y || ry >= box.y + box.h)
                continue; // scrolled out of view
            float cy = ry + rh * 0.5f;
            if (items[i].separator) {
                setColor(r, kSepLine);
                jplay::drawLine(r, box.x + 4.0f, cy, box.x + box.w - 4.0f, cy);
                continue;
            }
            const SDL_FRect rowRect{ box.x + 1.0f, ry, box.w - 2.0f, kRowH };
            const uint32_t rc = items[i].rowColor;
            if (rc) {
                // Color-picker row: the fill *is* the value, so the selected /
                // hover states can't repaint it — hover draws an outline instead.
                SDL_SetRenderDrawColor(r, (rc >> 16) & 0xFF, (rc >> 8) & 0xFF, rc & 0xFF, 255);
                jplay::fillRect(r, &rowRect);
                if (i == hovered) {
                    setColor(r, kMark);
                    jplay::drawRect(r, &rowRect);
                }
            } else {
                if (items[i].checked) {
                    setColor(r, kSelected);
                    jplay::fillRect(r, &rowRect);
                }
                if (i == hovered)
                    jplay::drawRowHover(r, rowRect);
            }
            if (items[i].checked) {
                setColor(r, kMark);
                drawCheck(r, box.x + 4.0f, cy);
            }
            if (textFont_)
                textFont_->draw(r, box.x + kMarkW, ry + (kRowH - glyphH) * 0.5f,
                                labelColor(items[i]), items[i].label.c_str());
            if (textFont_ && !items[i].shortcut.empty()) {
                // Measured, not estimated: the font is proportional, so the longest
                // hint would otherwise sit short of the right edge listWidth reserved.
                const std::string& sc = items[i].shortcut;
                float sw = textFont_->measure(r, sc.c_str());
                float inset = reserveArrow ? kArrowW : kPadX;
                textFont_->draw(r, box.x + box.w - inset - sw,
                                ry + (kRowH - glyphH) * 0.5f,
                                items[i].disabled ? kShortcutOff : kShortcutTxt,
                                sc.c_str());
            }
            if (reserveArrow && hasChildren(items[i])) {
                setColor(r, kMark);
                drawArrow(r, box.x + box.w - kArrowW + 4.0f, cy);
            }
        }
        SDL_SetRenderClipRect(r, nullptr);
        // Border drawn over the rows, which are inset by a pixel.
        setColor(r, kBorder);
        jplay::drawRect(r, &box);
        // Scroll hints when the list is taller than its box.
        float maxScroll = std::max(0.0f, listHeight(items) - box.h);
        if (maxScroll > 0.5f) {
            setColor(r, kMark);
            if (scroll > 0.5f)
                drawChevron(r, box.x + box.w * 0.5f, box.y + 5.0f, true);
            if (scroll < maxScroll - 0.5f)
                drawChevron(r, box.x + box.w * 0.5f, box.y + box.h - 5.0f, false);
        }
    };

    // Header band: the popup skin plus its own outline (which reads as the rule
    // between the band and the rows); the host draws the content inside it.
    if (header_.render && headerRect_.h > 0.0f) {
        setColor(r, kListBg);
        jplay::fillRect(r, &headerRect_);
        header_.render(r, headerRect_);
        setColor(r, kBorder);
        jplay::drawRect(r, &headerRect_);
    }

    bool anyChild = std::any_of(items_.begin(), items_.end(), hasChildren);
    // Keep the parent row highlighted while its submenu is open.
    int rootHover = (openParent_ >= 0) ? openParent_ : hoverRoot_;
    drawList(rootRect_, items_, rootHover, anyChild, rootScroll_);

    if (openParent_ >= 0 && openParent_ < (int)items_.size())
        drawList(childRect_, items_[openParent_].children, hoverChild_, false, childScroll_);
}

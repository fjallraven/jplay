#pragma once

#include "TextFont.h"

#include <SDL3/SDL.h>

#include <functional>
#include <string>
#include <vector>

// A horizontal row of labeled dropdown (combo-box) controls for SDL3 immediate-
// mode UIs. Self-contained like MenuBar: the host feeds it events first
// (handleEvent), then calls layout()/render() each frame. Only one dropdown is
// open at a time, and the open option list drops downward over whatever is drawn
// below the bar. Renders with the built-in debug font.
class DropdownBar {
public:
    // Add a labeled dropdown ("Department:", options...). Returns a stable index
    // (never shifts) usable with selected()/value().
    int add(const std::string& label, std::vector<std::string> options, int selected = 0);

    int selected(int i) const { return entries_[i].selected; }
    const std::string& value(int i) const;

    // Replace an entry's option list and current selection. Used to refresh a
    // dropdown's contents on the fly (e.g. the Version picker populated from a
    // get_versions() call just before its list opens). Clears any separator rows.
    void setOptions(int i, std::vector<std::string> options, int selected);

    // Mark option rows (by index) as non-selectable separators: they render as a
    // faint divider line, ignore hover, and can't be picked. Set after setOptions,
    // which clears the list. Out-of-range indices are ignored at render/hit-test.
    void setSeparators(int i, std::vector<int> rows) { entries_[i].separators = std::move(rows); }

    // Override the closed-box width for entry `i`. The default is kBoxW.
    // Call after setOptions when the content is known to need more space.
    void setBoxWidth(int i, float w) { entries_[i].boxW = w; }

    // Hide or show an individual entry (label + box). Hidden entries take no
    // layout space and ignore input.
    void setVisible(int i, bool v) { entries_[i].visible = v; }

    // Set the displayed text without supplying a full list (collapses to a
    // single-option entry showing `text`). Used to reflect the active clip's
    // version while the list stays closed.
    void setText(int i, const std::string& text);

    // Invoked when entry `i`'s box is clicked and its list is about to open.
    // The host can use this to (re)populate the entry via setOptions() first.
    void setOnOpen(int i, std::function<void()> cb) { entries_[i].onOpen = std::move(cb); }

    // Invoked when the user picks a different option in entry `i` (not fired when
    // re-selecting the row already current). The argument is the new selection
    // index; the entry's selected() is already updated when the callback runs.
    void setOnSelect(int i, std::function<void(int)> cb) { entries_[i].onSelect = std::move(cb); }

    // When disabled the bar ignores clicks (no list opens) and renders faintly.
    void setEnabled(bool enabled) { enabled_ = enabled; }
    bool enabled() const { return enabled_; }

    // Returns true if the event was consumed; the host should then skip its own
    // handling for that event.
    bool handleEvent(const SDL_Event& e);

    // Recompute control geometry for the strip `bar` the row lives in; call
    // before render().
    void setTextFont(TextFont* f) { textFont_ = f; }

    void layout(SDL_Renderer* r, const SDL_FRect& bar);
    void render(SDL_Renderer* r) const;

    bool isOpen() const { return open_ >= 0; }

private:
    static constexpr float kBoxW = 150.0f;    // width of a closed control box
    static constexpr float kBoxH = 20.0f;     // height of a closed control box
    static constexpr float kRowH = 20.0f;     // open-list option row height
    static constexpr float kPadX = 14.0f;     // gap between dropdown groups
    static constexpr float kLabelGap = 6.0f;  // gap between a label and its box
    static constexpr float kInnerPadX = 8.0f; // text inset inside a box / row
    static constexpr float kCaretW = 14.0f;   // space reserved for the caret

    struct Entry {
        std::string label;
        std::vector<std::string> options;
        int selected = 0;
        float boxW = kBoxW; // closed box width; override via setBoxWidth()
        SDL_FRect box{}; // closed control rect, recomputed in layout()
        bool visible = true;
        std::vector<int> separators; // option indices drawn as non-selectable dividers
        std::function<void()> onOpen; // called just before this entry's list opens
        std::function<void(int)> onSelect; // called when a different option is picked
    };
    TextFont* textFont_ = nullptr;
    std::vector<Entry> entries_;
    SDL_FRect bar_{};
    bool enabled_ = true;
    int open_ = -1;     // index of the open dropdown, -1 = closed
    int hoverRow_ = -1; // hovered option in the open list, -1 = none

    SDL_FRect listRect(int i) const;
    int boxAt(float x, float y) const;       // -1 if none
    int rowAt(int i, float x, float y) const; // -1 if none; needs i open
};

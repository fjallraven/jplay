#pragma once

#include "NineSlice.h"
#include "SkinColors.h"

#include <SDL3/SDL.h>

#include <string>
#include <vector>

// Skinned widget art.
//
// A skin is a directory of ".9" BMPs authored by uipage.html — one per widget
// surface, each carrying its stretch insets and content box in a 1px guide
// border (see NineSlice.h). They live in "skins/<name>/" next to the executable:
//
//   skins/default/button-idle.9.bmp
//
// There is one tileset, authored at 1x, for every display scale — NineSlice_Draw
// blits it at a whole number of device pixels so it stays crisp at 125% / 150% /
// 200% without a pack per scale. Adding a display scale therefore costs nothing,
// and re-skinning is one directory of bitmaps.
//
// Loading is best-effort by design. A missing pack, a missing file or a bad BMP
// leaves that surface null, and every call site keeps the flat fill + 1px border
// it drew before — so the app still runs from a bare build directory, and a skin
// that only replaces half the widgets is a valid skin.
//
// The registry is a global rather than an App member because Widgets.cpp,
// Menu.cpp, Dropdown.cpp and ContextMenu.cpp draw widgets without holding an App
// reference. jplay::gDeviceScale in PixelSnap.h is a global for the same reason.

namespace jplay {

// One entry per surface in a pack. Order must match kSliceFiles in Skin.cpp.
enum class SkinSlice {
    // No ChromeTitlebar: the menu bar is a horizontal ramp across the whole
    // window, which a stretched 9-slice centre cannot express. TitleBar.cpp draws
    // it from colors().titlebarLeft/Right.
    ChromeTopbar,
    ChromeNavstrip,
    ChromeDropdownbar,

    ButtonIdle,
    ButtonHover,
    ButtonDisabled,
    UiButtonIdle,
    UiButtonHover,
    UiButtonOn,
    UiButtonOnHover,
    NavButton,
    TopButtonIdle,
    TopButtonHover,
    TopButtonOn,
    TopButtonOnHover,

    InputIdle,
    InputFocus,
    PickerIdle,
    PickerOpen,
    DbarBoxIdle,
    DbarBoxOpen,

    List,
    Menu,
    MenuTitle,
    Tooltip,
    CurvePlot,

    SegmentOff,
    SegmentOn,
    CheckboxOff,
    CheckboxOn,
    Radio,

    ToolOff,
    ToolOn,
    TabOff,
    TabOn,

    SliderTrack,
    SliderKnob,
    MenuSliderGroove,
    MenuSliderKnob,

    ScrollbarTrack,
    ScrollbarThumb,
    ScrollbarThumbBare,

    Count
};

class Skin {
public:
    // Load skins/<name>/, replacing whatever was loaded before. Returns true when
    // at least one surface came in. The textures are renderer-bound, so this is
    // also how the skin is rebuilt after the renderer is recreated.
    bool load(SDL_Renderer* r, const std::string& name);

    void unload();

    bool loaded() const { return loaded_; }
    const std::string& name() const { return name_; }

    // Art for `s`, or nullptr when this pack does not provide it.
    const NineSlice* get(SkinSlice s) const;

    // Draw `s` to fill `dst`. Returns false without drawing when the surface is
    // absent, which is the caller's cue to fall back to its own fill + border.
    // `alpha` modulates the whole surface, for the few controls that are
    // deliberately translucent over the video — the art itself is opaque.
    bool draw(SDL_Renderer* r, SkinSlice s, const SDL_FRect& dst, Uint8 alpha = 255) const;

private:
    NineSlice slices_[(int)SkinSlice::Count];
    std::string name_;
    bool loaded_ = false;
};

// The one skin the UI draws from.
Skin& skin();

// The live palette, replaced wholesale by Skin::load.
//
// This returns a reference to a single global that load() overwrites *in place*,
// which is what lets a translation unit bind its file-local constant straight to
// a member and follow a skin change without doing anything:
//
//     static const SDL_Color& kColBorder = jplay::colors().border;
//
// The address of a member never changes; only its value does. That keeps the
// hundreds of existing use sites untouched — they still read a plain SDL_Color —
// at the cost of those constants no longer being constexpr.
const SkinColors& colors();

// Directory names under skins/ next to the executable, sorted. Empty when the
// directory is absent.
std::vector<std::string> skinNames();

} // namespace jplay

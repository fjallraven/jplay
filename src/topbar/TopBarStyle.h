#pragma once

// Shared visual style for the top-toolbar popups (Letterbox, the OCIO list menus,
// and the Sequence view-filter menu). These translation units — App_TopBar.cpp,
// App_Letterbox.cpp, App_Ocio.cpp, App_SequenceMenu.cpp — all draw the same
// floating-dropdown look, so the palette and the setColor helper live here rather
// than being copied into each. Kept out of the broadly-included AppInternal.h /
// UiColors.h because several other units define their own file-local kHeader /
// kLabel / kValue and would collide.

#include <SDL3/SDL.h>

#include "MenuDraw.h"
#include "PixelSnap.h"
#include "SkinColors.h"
#include "UiColors.h"

namespace jplay {

// From the shared palette where it names the same colour; see jplay::colors().
inline const SDL_Color& kPopupBg     = colors().dropBg; // floating dropdown menus (lighter than chrome)
inline const SDL_Color& kPanelBorder = colors().border;
inline const SDL_Color& kHeader      = colors().header;
inline const SDL_Color& kValue       = colors().value;
inline const SDL_Color& kCurrent     = kMenuSelectedBg;
// No palette entry: this popup's label is brighter than the panel --label, and
// the check tint and group header are local to the Sequence popup.
constexpr SDL_Color kLabel       { 220, 222, 228, 255 };
constexpr SDL_Color kCheck       { 120, 180, 255, 255 };
constexpr SDL_Color kGroupHead   { 255, 255, 255, 255 }; // project header in the Sequence popup

inline void setColor(SDL_Renderer* r, SDL_Color c) {
    SDL_SetRenderDrawColor(r, c.r, c.g, c.b, c.a);
}

// The floating panel these popups sit in.
inline void drawPopupPanel(SDL_Renderer* r, const SDL_FRect& box) {
    setColor(r, kPopupBg);
    fillRect(r, &box);
    setColor(r, kPanelBorder);
    drawRect(r, &box);
}

} // namespace jplay

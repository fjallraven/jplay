#pragma once

#include <SDL3/SDL.h>

#include "SkinColors.h"

// Shared UI palette for menu / popup widgets, so the same control style is used
// across the whole application (ContextMenu picker panels, the OCIO toolbar
// popups, etc.). Change here to retint every menu at once.

// Background fill of the currently-selected / active item in a menu or picker
// list. One canonical dark blue used by every menu that marks a chosen row.
// A reference into the shared palette; see jplay::colors().
inline const SDL_Color& kMenuSelectedBg = jplay::colors().rowSelected;

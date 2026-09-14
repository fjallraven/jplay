#pragma once

// Row-level drawing shared by every popup list.
//
// Menu.cpp, ContextMenu.cpp, Dropdown.cpp, Widgets.cpp (Combobox), the top-bar
// popups (App_Ocio / App_SequenceMenu / App_Letterbox) and the Settings pickers
// all draw the same hovered row and the same picker caret. The geometry used to
// be written out at each site — four different spellings of the same 1px inset —
// which is how two of the Settings pickers ended up square while the third was
// rounded. One definition each, so they cannot drift again.
//
#include "PixelSnap.h"
#include "SkinColors.h"

#include <SDL3/SDL.h>

namespace jplay {

// The hovered-row highlight. `row` is the full row; the highlight is drawn 1px
// inside it on every side, so it reads as a pill within the panel rather than
// butting up against its edge.
inline void drawRowHover(SDL_Renderer* r, const SDL_FRect& row) {
    const SDL_FRect hr{ row.x + 1.0f, row.y + 1.0f, row.w - 2.0f, row.h - 2.0f };
    const SDL_Color& color = colors().hover;
    SDL_SetRenderDrawColor(r, color.r, color.g, color.b, color.a);
    fillRect(r, &hr);
}

// The down-caret chevron on a closed picker box, centered at (cx, cy). The
// caller sets the draw colour; the strokes are diagonal, so PixelSnap passes
// them through unsnapped.
inline void drawCaretDown(SDL_Renderer* r, float cx, float cy) {
    drawLine(r, cx - 3.0f, cy, cx, cy + 3.0f);
    drawLine(r, cx, cy + 3.0f, cx + 3.0f, cy);
}

} // namespace jplay

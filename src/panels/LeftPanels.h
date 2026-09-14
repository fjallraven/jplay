#pragma once

#include <SDL3/SDL.h>

#include <cstdint>
#include <functional>

namespace jplay {

// One pane of the left icon strip: a toggle glyph, and the panel it opens.
//
// The panes are mutually exclusive -- at most one is open at a time -- so the
// app holds a single index into the registry (App::openPanel_) rather than a
// bool apiece. Everything the strip, the View > Panels menu, cinema mode, the
// layout and event dispatch need to know about a pane lives in one of these,
// which is what makes adding a pane a matter of appending an entry in
// App::registerLeftPanels() instead of editing a dozen switch statements
// spread across App.cpp.
//
// See docs/ADDING_A_PANEL.md for the whole procedure.
struct LeftPanelDesc {
    // Stable identity. `label` is the tooltip and the View > Panels caption and
    // is safe to reword; `id` is what anything persisted is filed under, so
    // renaming it is not.
    const char* id    = "";
    const char* label = "";

    // Toggle glyph, as an MDI codepoint. Write iconCp(ICON_MDI_FOO) rather than
    // a bare hex literal: the embedded font is subset to the glyphs the source
    // names, so spelling the macro is what keeps yours in the build, and a typo
    // becomes a compile error instead of a blank button. See IconFont.h.
    uint32_t icon = 0;

    // Open width in logical px. 0 takes the shared default (kLeftPanelW).
    float defaultW = 0.0f;
    // Whether the pane's right edge can be dragged to resize it.
    bool resizable = false;

    // ── Behaviour ────────────────────────────────────────────────────────────
    // Only `render` is required; every other callback may be left empty.

    // Draw the pane. Called once a frame while this pane is the open one, with
    // the pane's rect already reserved by the layout (see App::leftPaneRect).
    std::function<void()> render;

    // Take an event aimed at the pane. Return true to consume it, false to let
    // it fall through to whatever is underneath. Called for presses that land
    // inside the pane, after the app's own chrome has had its say.
    std::function<bool(const SDL_Event&)> handleEvent;

    // Take an event before the rest of the app sees it, wherever it landed.
    // This is for the things a pane owns that outlive a click inside it: an
    // open dropdown anchored over the stage, a focused text field that must
    // keep receiving keystrokes. Return true to consume.
    //
    // It runs after the timeline's inline track rename, which is modal while
    // it is live -- a click outside that field commits it and falls through to
    // here, so a pane's own field still gets focus on the same click.
    std::function<bool(const SDL_Event&)> handleEventEarly;

    // Mouse wheel, already known to be over the open pane and non-zero.
    // `delta` is SDL's wheel y; the cursor comes too, for a pane with more than
    // one scrollable region (the Project Explorer's bin, tree and info
    // sub-panel). Clamping the offset is the pane's own business.
    std::function<void(float delta, float mx, float my)> onWheel;

    // Called just after this pane becomes the open one, and just before it
    // stops being it. onClose is where a pane drops transient state: an
    // in-flight query it must not apply on reopen (Clip Source), or a focused
    // text field that would otherwise keep swallowing keystrokes (Sync Review).
    std::function<void()> onOpen;
    std::function<void()> onClose;

    // Colour for the toggle glyph, given whether the pane is open and the
    // colour the strip would use otherwise. This is how a pane reports state
    // from the strip without being open -- tech check warns while a mode is
    // live, sync review goes green while a session is up. Empty means "take
    // the default".
    std::function<SDL_Color(bool isOpen, SDL_Color fallback)> iconTint;

    // ── Runtime state, owned by the strip ────────────────────────────────────
    SDL_FRect btnRect{};    // toggle's place in the strip; zero-width if it did not fit
    float     userW = 0.0f; // width set by dragging the edge; 0 means defaultW
};

} // namespace jplay

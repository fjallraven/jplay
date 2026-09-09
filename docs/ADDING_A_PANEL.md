# Adding a left panel

The strip down the left edge of the window is a list. Each entry is one
`LeftPanelDesc` — an icon, a label, and the callbacks that draw and drive a
pane. The strip, its tooltips, the **View ▸ Panels** menu, cinema mode, compact
timeline, the layout, event dispatch and wheel routing all read that one list,
so adding a pane means appending to it rather than editing a dozen places.

Three steps, no more.

## 1. Write the pane

Add `src/panels/App_MyPanel.cpp`. The minimum is a render function:

```cpp
#include "App.h"
#include "AppInternal.h"
#include "Layout.h"

using namespace jplay; // the layout helpers, colors() and dpiScale live here

void App::renderMyPanel() {
    // beginLeftPanel paints the shared chrome — background, and the right edge
    // (a resize accent when your pane is resizable and under the cursor, a
    // divider otherwise) — and hands back the pane's rect.
    SDL_FRect panel = beginLeftPanel();
    SDL_FRect body  = inset(panel, 10.0f * dpiScale, 0.0f);
    gapTop(body, 8.0f);
    leftPanelHeader(body, "MY PANEL");
    gapTop(body, 10.0f);

    // Lay content out by cutting rows off `body` — see src/ui/Layout.h.
    SDL_FRect row = cutTop(body, textFont_.lineHeight());
    drawText(row.x, row.y, colors().text, "Hello");
}
```

Declare it in `App.h` beside the other panel members, and add the file to
`add_executable(jplay ...)` in `CMakeLists.txt`.

Your render function is called only while your pane is the open one, so it does
not need to check. Inside it, `openPanelW()` is your width and `leftPaneRect()`
is your rect.

## 2. Register it

Append an entry at the end of `registerLeftPanels()` in
`src/panels/App_NavPanel.cpp`:

```cpp
jplay::LeftPanelDesc mine;
mine.id     = "my-panel";        // stable key; label is free to reword, this is not
mine.label  = "My Panel";        // tooltip and View > Panels caption
mine.icon   = iconCp(ICON_MDI_STAR);
mine.render = [this] { renderMyPanel(); };
leftPanels_.push_back(mine);
```

That is enough for a working pane: a toggle in the strip in the position you
appended it, a hover tooltip, a **View ▸ Panels** row with a checkmark, and
correct behaviour under cinema mode and compact timeline.

**Use `iconCp(ICON_MDI_FOO)`, not a raw `0xF0123`.** The Material Design Icons
webfont is subset at build time to the glyphs the source names, and
`tools/gen_icon_font.py` finds them by scanning for `ICON_MDI_*` identifiers.
Spelling the macro is what keeps your glyph in the binary, and a typo becomes a
compile error rather than a button that silently draws nothing. (The tool also
recognises raw `0xFxxxx` literals as a safety net, but nothing checks those for
you.) Browse the names in `src/ui/IconsMaterialDesignIcons.h`.

## 3. Take input, if the pane needs it

Every callback below is optional.

| Field | When it runs |
| --- | --- |
| `handleEvent` | A press landed inside your pane. Return `true` to consume it. |
| `handleEventEarly` | Before the rest of the app, wherever the cursor is — for a dropdown that reaches past the pane, or a text field that must keep receiving keystrokes. |
| `onWheel(delta, mx, my)` | Wheel over your pane. The cursor comes too, for a pane with more than one scrollable region. Clamping is yours. |
| `onOpen` / `onClose` | Your pane became / stopped being the open one. `onClose` is where transient state goes: an in-flight query you must not apply on reopen, a focused field that would keep swallowing keys. |
| `iconTint(isOpen, fallback)` | Colour for your toggle glyph, so the strip can report state while the pane is shut — Tech Check warns when a mode is live, Sync Review goes green during a session. |

Two more descriptor fields:

- `defaultW` — open width in logical px before DPI scaling. `0` takes the
  shared default.
- `resizable` — gives the pane's right edge a drag handle. The width the user
  sets is kept in the descriptor's `userW`; nothing else to wire up.

## Things worth knowing

**The panes are mutually exclusive.** `openPanel_` is a single index into
`leftPanels_`, not a bool apiece, and `openLeftPanel()` is the only thing that
changes it — so opening a pane cannot forget to close another. Never set that
index directly; call `openLeftPanel()` or `toggleLeftPanel()`.

**`LeftPanelId` is for the core, not for you.** The enum in `App.h` names the
built-in panes' indices for the handful of places outside a pane's own file
that must name one specifically. A pane you append needs no entry, and the
assert in `registerLeftPanels()` allows for that — it checks that the built-ins
are the *first* `kPanelBuiltinCount` entries, not that they are the only ones.
If you do add your pane to the enum, append it past `kPanelBuiltinCount` so the
built-in indices do not shift.

**Order in the list is order on screen.** The strip stacks toggles in registry
order and the menu lists them in the same order. Appending puts your toggle at
the bottom, above nothing; insert earlier in the function to place it higher.

**A toggle that does not fit is skipped, not squashed.** On a short window the
strip stops placing buttons when it runs out of height; a skipped one gets a
zero rect, which draws nothing and hit-tests false.

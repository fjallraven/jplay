# Mouse and keyboard shortcuts

Press `H` in the app (or **Help ▸ Keyboard Shortcuts**) for the same list as an
overlay. Any click dismisses it.

## Keyboard

### General

| Key | Action |
| --- | --- |
| `H` | Show / hide the shortcut overlay |
| `F1` | Source of the clip under the playhead |
| `F2` / `F3` / `F4` / `F5` | Timeline / overview / layout / stack |
| `Up` / `Down` | Cycle the stack order (Stack view) |
| `F11` | Fullscreen video (`Esc` to leave) |
| `Tab` | Compact timeline (ruler only) |
| `L` | Toggle layout (tiled tracks) |
| `I` | Toggle the source inspector |
| `Backspace` | Back: out of a source view, or a view up |
| `Ctrl + N` | New project |
| `Ctrl + O` | Open project |
| `Ctrl + S` | Save project |
| `Ctrl + Shift + S` | Save project as |
| `Ctrl + W` | Close project |
| `Ctrl + Z` / `Ctrl + Y` | Undo / redo |
| `Q` | Quit |

### Player

| Key | Action |
| --- | --- |
| `R` / `G` / `B` | View the red / green / blue channel |
| `,` / `.` | Exposure −/+ ⅓ stop (`Shift`: 1 stop) |
| `E` + drag | Scrub exposure / gamma over the frame |
| `E` | Bypass exposure / gamma (A/B) |
| `P` | Toggle the pixel inspector |
| `F` | Fit frame or timeline |
| `Alt + F` | Fit the sequence under the playhead |
| `Shift + F` | Zoom to the clip under the playhead |
| `1` / `2` / `3` / `4` | Frame at 1:1 / 2:1 / 3:1 / 4:1 pixels |

### Playback

| Key | Action |
| --- | --- |
| `Space` | Play / pause |
| `Left` / `Right` | Step 1 frame (`Shift`: 10 frames) |
| `Ctrl + Up` / `Down` | Previous / next clip |
| `PgUp` / `PgDn` | Set the range to this clip, then step to the next / previous |
| `Shift + PgUp` / `PgDn` | Expand / contract the range by a clip each side |
| `Home` / `End` | Go to in / out point |
| `[` `]` | Set playback in / out point |
| `Shift + [` `]` | Clear playback in / out point |
| `X` | Set the range to the clip under the playhead |
| `Shift + X` / `\` | Clear the in / out range |

### Timeline

| Key | Action |
| --- | --- |
| `S` | Toggle snap |
| `D` | Disable / enable clip |
| `Ctrl + C` | Copy selected clip(s) |
| `Ctrl + V` | Paste at the playhead (first free track) |
| `Delete` | Delete the selected clip or gap |

## Mouse

### Over the frame

| Gesture | Action |
| --- | --- |
| Click | Play / pause (a press that releases without moving) |
| Drag | Jog the playhead |
| `E` + drag | Scrub exposure / gamma instead of jogging |
| Middle-drag | Pan the zoomed frame |
| Wheel | Zoom about the cursor |
| Right-click | Clip menu for the clip on screen |
| Drag (Draw panel open) | Draw a freehand stroke — see [draw tool](DRAW_TOOL.md) |

In **Overview** (`F3`) the wheel scrolls the grid instead, `Ctrl` + wheel
resizes its tiles, middle-drag scrolls it, a click jumps the playhead to that
clip, a double-click also marks the playback range to it, and `Shift` + click
extends the range to it.

### On the timeline

| Gesture | Action |
| --- | --- |
| Click the ruler or a track | Scrub |
| Drag a clip | Move it |
| Double-click empty space | Zoom-fit the sequence span under the cursor; again to zoom back out |
| Wheel | Zoom the time axis (over the track stack with more tracks than fit: scroll) |
| Right-click a clip | Clip actions — copy, paste, pair/unpair audio, … |
| `Ctrl` + right-click a clip | The metadata pickers: department / asset / version … |
| Drag the timeline's top edge | Resize the timeline; double-click restores the automatic height |
| Double-click a track label | Rename the track |
| Drag a track label | Reorder tracks |

Right-clicking a clip that is part of a selection keeps the whole selection, so
a pick applies to all of it.

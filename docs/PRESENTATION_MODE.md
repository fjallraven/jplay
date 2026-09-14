# Presentation mode


## Second display

A borderless fullscreen window on a **second display** that mirrors the clean
program frame: the graded and tech-checked image plus
[pencil annotations](DRAW_TOOL.md), at the same zoom and pan as the player, with
no status text, no LOADING badge, no tech-check legend, no grid.

Pick it under **Output ▸ Review: \<display name>**. The entry appears only when
a second display is connected, and needs the OpenGL renderer (the readback
path). It is mutually exclusive with the external video backends below.

## External video output

Also under the **Output** menu, in the same radio group:

| Backend | Notes |
| --- | --- |
| **NDI** | Video over the network. Greyed when the runtime is missing. |
| **SDI (DeckLink)** | Blackmagic SDI out. Linux only at present. |

The line under the active entry reports what the device is actually sending —
colorimetry and receiver count. The stream's colorimetry follows the frame
rather than a setting, so a PQ or HLG stream drops to Rec.709 the moment the
timeline reaches a video clip.

**Output ▸ Enable HDR Output** switches the renderer's output colorspace; it
takes effect on the next launch, since that is fixed at renderer creation. With
it on, a **Ref White** slider sets the paper-white reference in nits.

## Burn-in overlay

**View ▸ Show Overlay** burns the file name and the playhead readout over the
picture — on the main player *and* the review monitor. **View ▸ Overlay
Options** sets its alignment (top / bottom), size and colour. The readout
follows the [time format](GETTING_STARTED.md#timecode-or-frames) you picked.

## Letterbox matte

The **Letterbox** button in the top toolbar masks the program image to a target
aspect ratio with black bars: presets from 2.39:1 down, a custom entry, an
opacity slider, and **Fit View**, which frames the zoom on the masked region
rather than the full image.

The ratio and opacity live on the timeline, so they persist per project, and the
matte is applied by the player, the review monitor and the external output
alike. In a [sync session](SYNC_SESSION.md) it travels with the host's view,
because Fit View means the framing only matches when the matte does.

## Related

- [Sync session](SYNC_SESSION.md) — driving other people's players from yours.
- [Draw tool](DRAW_TOOL.md) — annotations, which the review monitor shows.
- [Shortcuts](SHORTCUTS.md)

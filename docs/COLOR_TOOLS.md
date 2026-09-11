# Colour tools

Two panels and one overlay: **Color Grading** for looking at the image
differently, **Tech Check** for finding what is wrong with it, and the pixel
inspector for the numbers behind both.

All three are open from the icon strip down the left edge, or
**View ▸ Panels**.

## Color Grading

One global grade, applied as a single GPU post-pass over the final
display-referred image — after OCIO for EXR, or over the raw RGBA for video. It
is a viewing grade for the session, not a per-clip decision saved into the
project, and the **Reset** button in the panel header puts it back.

The panel's top bar picks one of three tools.

### Basic

| Group | Control | Range | Default |
| --- | --- | --- | --- |
| Exposure | **Gain** | −5 … 5 | 0 |
| Exposure | **Gamma** | 0.1 … 4 | 1 |
| Colour | **Temperature** | −100 … 100 | 0 |
| Colour | **Tint** | −100 … 100 | 0 |
| Colour | **Saturation** | −100 … 100 | 0 |

### Curves

A **Luma / R / G / B** tab strip over an editable curve. Click to add a control
point, drag to move it, double-click a midpoint to remove it. Points are held in normalized
`[0,1]` space, sorted by x, and baked to a 256-entry LUT for the shader; the
default identity is just the two endpoints.

### Wheels

Three-way colour correction: a hue wheel and a **Luma** slider for **Shadows**,
**Midtones** and **Highlights**. On a wheel, angle is hue and radius is
saturation.

### Exposure without the panel

`,` and `.` nudge exposure by ⅓ of a stop (`Shift` for a full stop), `E` + drag
scrubs exposure and gamma over the frame, and tapping `E` bypasses both for an
A/B. In a [sync session](SYNC_SESSION.md) the host owns this and spectators
follow.

## Tech Check

Three mutually exclusive diagnostic modes. They persist when the panel is
closed, so you can turn one on and get the panel out of the way.

| Mode | What it does |
| --- | --- |
| **Luminance** | A scene-referred nit heatmap, computed *before* the display transform. EXR only. Draws a colour-ramp scale on the stage. |
| **Clipping Warning** | Flags crushed and blown pixels, as a display-referred pass after the grade. Draws a legend. |
| **Monochrome** | Drops chroma so you judge contrast and structure alone. |

The `R`, `G` and `B` keys reach the same slot with channel isolation — view the
red, green or blue channel on its own. They share the slot with the three modes
above, so turning one on turns the other off.

## Pixel inspector

`P`, or **View ▸ Pixel Inspector**. A probe overlay in the bottom corner of the
stage: a nearest-neighbour magnifier around the cursor with a crosshair on the
sampled pixel, and that pixel's value at three points of the pipeline.

| Row | What it reads |
| --- | --- |
| **SRC** | The decoded frame straight out of the cache — the same buffer the display path uploads. Scene-linear halves for EXR, code values otherwise. |
| **WORK** | That pixel through the OCIO input transform and the exposure, via the same CPU transform the export writer uses. |
| **DISP** | Read back off the program texture, so it carries the display transform, the grade and the tech mode exactly as the GPU produced them. |

Each stage is read where it actually exists rather than re-derived, so nothing
can drift out of step with what you are looking at.

## Related

- [Colour management](COLOUR_MANAGEMENT.md) — the display transform these sit
  on top of.
- [Colour inspect](COLOR_INSPECT.md) — what a clip *is*, colour-wise.

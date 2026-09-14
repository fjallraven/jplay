# Colour inspect

Answering "what am I actually looking at?" — which transform is on the image,
which file it came from, and what the numbers are.

## The pipeline, in order

```
decode → file colorspace → exposure → display transform (OCIO) → grade → tech mode → screen
         └─ SRC ────────┘  └─ WORK ─────────────────────────┘   └─ DISP ──────────┘
```

The three labels are the rows of the [pixel inspector](COLOR_TOOLS.md#pixel-inspector)
(`P`), which reads each stage where it actually exists rather than re-deriving
it. If a frame looks wrong, that is the fastest way to find *where* it went
wrong: correct in SRC and wrong in WORK is an input-colorspace problem, correct
in WORK and wrong in DISP is a display transform, a grade or a tech mode.

## Which transform is applied

With OCIO enabled, the four top-toolbar dropdowns always show the current
answer:

| Button | Meaning |
| --- | --- |
| **Display** | Output device transform. |
| **View** | View transform for that display. |
| **Look** | Optional look on top. |
| **File Colorspace** | What the decoded media is being treated as. |

With OCIO off, the built-in scene-linear → sRGB conversion is used and the
buttons are not laid out. See [colour management](COLOUR_MANAGEMENT.md).

Note what is *not* in that list: the **grade** and the **tech-check** mode sit
after the display transform and are viewing state, not part of the media's
colour interpretation. A grade left on is the most common reason two people
disagree about a frame — and in a [sync session](SYNC_SESSION.md) colour and
OCIO are deliberately not synced, so each viewer's is their own.

## Which file is on screen

**Source Inspector** (`I`, or **View ▸ Source Inspector**) is an overlay
describing whatever is under the playhead, in three parts:

- **Sequence** — name, shot count, clip count, duration.
- **Shot** — start, end, duration, cut in / cut out.
- **Clip** — name, path, track, start, end, duration, source in / out, type,
  resolution, frame count, fps, plus every value the
  [naming convention](GETTING_STARTED.md#the-naming-convention) captured for it.

It is informational: clicks pass through to the frame beneath, except on its
close button.

The same media block, plus file size, modification time and frame range, appears
at the bottom of the **Project Explorer**'s SOURCES tab for the selected source.

Two things change which file the path on screen resolves to, and neither shows
up as a different path in the inspector:

- the active [proxy mode](PROXY_MODES.md), which substitutes a representation at
  decode time,
- a [picker](GETTING_STARTED.md#the-naming-convention) switch, which swaps the
  clip's media outright — the **Clip Source** panel shows the current department,
  asset and version for the clip under the playhead.

## Related

- [Colour tools](COLOR_TOOLS.md) — grading, tech check, pixel inspector.
- [Colour management](COLOUR_MANAGEMENT.md) — OCIO config resolution.

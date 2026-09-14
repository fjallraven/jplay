# Colour management (OCIO)

OCIO is opt-in. By default jplay starts in the built-in scene-linear → sRGB
pipeline: no config is opened, and none of its startup cost is paid.

## Switching pipeline

Open the **Settings** panel and flip **PROJECT SETTINGS ▸ Color Management**
between `OCIO` and `sRGB`. The choice is saved with the project, so a `.jpproj`
saved in OCIO mode opens in OCIO mode.

To change the default a fresh session (and **File ▸ New Project**) starts in,
set it in `jplay_preferences.conf`:

```ini
[color_management]
color_pipeline = ocio
```

Anything other than `ocio` — including a missing key or a missing section —
reads as `srgb`.

## How an OCIO config path is resolved

In order, first hit is used:

1. **`$OCIO`** — environment variable.
2. **Resolved from the media file path.** Add an `[ocio]` section to
   `jplay_preferences.conf`, for example:

   ```ini
   [ocio]
   source_regex = /share/project/(?P<project_name>[^/]+)/
   ocio         = /share/project/{project_name}/config.ocio
   ```

   Named groups captured by `source_regex` are substituted into `ocio`.
3. **OCIO's built-in CG config.**

## Display transform

With OCIO enabled, four dropdowns appear in the top toolbar:

| Button | What it picks |
| --- | --- |
| **Display** | The output device transform. |
| **View** | The view transform applied for that display. |
| **Look** | An optional look on top of the view. |
| **File Colorspace** | The colorspace the decoded media is treated as. |

The chosen transforms are part of the project. They apply to the player, the
review monitor and the external (NDI / SDI) output alike.

## Related

- [Colour tools](COLOR_TOOLS.md) — grading and tech-check passes, which run
  after the display transform.
- [Colour inspect](COLOR_INSPECT.md) — reading pixel values at each stage of
  the pipeline.
- [Config files](CONFIG_FILES.md) — where `jplay_preferences.conf` lives and
  how its tiers merge.

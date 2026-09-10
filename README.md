# jplay

A lightweight, performance-first flipbook player for reviewing image sequences and
movies files.

## Features

- Real-time playback of EXR sequences, still images and movies (ProRes, H.264, MXF, …)
- Proxy/full media switching
- OpenColorIO
- Project/Timeline read/write OTIO support
- Extended desktop, NDI and DeckLink SDI support
- Local control channel (JSON over loopback TCP) for MCP servers etc

## Colour management (OCIO)

OCIO is opt-in. By defualt jplay starts in the built-in scene-linear → sRGB pipeline.
To switch to OCIO open up settings and enable under "Color Management".
You can also update jplay_preferences.conf:

```
[color_management]
color_pipeline = ocio
```
### How a OCIO config path is resolved

In order, first hit is used:

1. **`$OCIO`** — environment variable.
2. Resolve ocio config from filepath

   Add ocio section to jplay_preferences.conf, for example:

```ini
[ocio]
source_regex = /share/project/(?P<project_name>[^/]+)/
ocio         = /share/project/{project_name}/config.ocio
```

3. **OCIO built-in cg config**

## Dependencies

[SDL3](https://libsdl.org) ·
[FFmpeg](https://ffmpeg.org) ·
[OpenEXR](https://openexr.com) / [Imath](https://github.com/AcademySoftwareFoundation/Imath) ·
[OpenColorIO](https://opencolorio.org) ·
[OpenTimelineIO](https://opentimeline.io) ·
[shaderc](https://github.com/google/shaderc) ·
[pybind11](https://github.com/pybind/pybind11) ·
[FreeType](https://freetype.org) ·
[RapidJSON](https://rapidjson.org)

# Getting started

jplay opens on a launcher with four ways in:

| Button | What it does |
| --- | --- |
| **Open Project** | Opens an existing `.jpproj`. |
| **Import Media** | Adds files to the current project. |
| **Create From Directory** | Scans a folder tree and builds a whole timeline from it — see below. |
| **Create Empty Project** | An empty timeline to drag media onto. |

The left column of the launcher also lists recent projects, the studio's shared
shows, and any live [sync session](SYNC_SESSION.md) on the LAN.

You can also drag files or a folder onto the window, or pass paths on the
command line. Press `H` at any time for the [shortcut overlay](SHORTCUTS.md).

## Supported formats

**Video** — decoded with FFmpeg, so whatever codec your FFmpeg build supports:

```
.mov .mp4 .m4v .mkv .avi .mxf .webm .mpg .mpeg .m2v .m2ts .mts .ts .wmv .flv .y4m .ogv .3gp
```

**Images and image sequences**:

```
.exr .png .jpg .jpeg .tif .tiff
```

`.exr` goes through OpenEXR and stays scene-linear half-float all the way to
the display transform. The other formats load through SDL_image and keep their
bit depth (16-bit stills are not crushed to 8).

Numbered files sharing a stem collapse into a single image-sequence entry, so a
folder of `shot_0100.exr … shot_0199.exr` is one clip, not a hundred.

**Audio** — also FFmpeg, resampled to stereo float internally:

```
.wav .aif .aiff .flac .mp3 .m4a .aac .ogg .opus .wma
```

A container like `.mov` counts as video even when it carries sound. To pair a
separate audio file with an EXR sequence automatically, turn on
**Settings ▸ Audio ▸ Attach Audio to Image Sequence** — the pairing itself is
answered by the `query_audio` [Python callback](PYTHON.md).

## Timecode or frames

**View ▸ Time Format** switches every time readout between **Timecode**
(`HH:MM:SS:FF`, computed from the project frame rate) and **Frames** (the raw
frame number). It is a user preference, not a project setting, and persists
across sessions.

Below it, **View ▸ Frame Numbering** decides whether those numbers are
**Global** (position on the whole timeline) or **Clip** (relative to the start
of the clip under the playhead).

The project frame rate itself is **Settings ▸ PROJECT SETTINGS ▸ FPS**, which
offers the standard delivery rates with exact NTSC values for the fractional
ones.

## The naming convention

jplay reads structure out of your paths: which **sequence** a file belongs to,
which **shot**, which **department**, which **asset**, and which **version** or
**take**. That is what lets it name clips, group them, and swap one clip for
another version of the same shot.

The rules are regexes in `naming_convention.conf`, not code. Directory shapes
and filename shapes are matched separately, top to bottom, first match wins.
As shipped, directory layouts like

```
<sequence>/<shot>/<department>          AWK/010/Compositing
<sequence>__<shot>__<department>        AWK__010__Compositing
<sequence>/<shot>
<shot>/<department>
<shot>
```

are recognised, along with filenames that carry the same tokens
(`unh0400_0010_lighting.0994.exr`, `hero_comp_v012.1001.exr`, and so on).

What the convention captures shows up as:

- the second line of a clip's timeline label (configurable — see
  [config files](CONFIG_FILES.md)),
- the cascading dropdowns in the **Clip Source** panel and on `Ctrl`+right-click
  over a clip, which jump a clip to a different department, asset or version,
- the search terms the [MCP control channel](MCP_CONTROL_CHANNEL.md) accepts.

Sites adapt it by editing `naming_convention.conf` — and, where regexes are not
enough, by overriding a [Python callback](PYTHON.md). No code change is needed
to add or rename a level.

## Create From Directory

**Create From Directory** turns a folder tree into a timeline in one step:

1. It walks the tree and, for every directory that directly contains media,
   matches the path *relative to the root you picked* against the `[dir:*]`
   rules. A directory matching no rule is skipped.
2. Media inside a matched directory is collapsed into assets — EXR frame runs
   become one entry, version-tagged files group by version.
3. One clip per shot is chosen: the highest-ranked department and asset present
   (the ranking is `[preferences]` in `naming_convention.conf`, else
   alphabetical), at its latest version.
4. Shots are sorted by sequence, then by shot, in natural order. Each change of
   sequence name starts a new **sequence** on the timeline, and within a
   sequence the clips are laid end to end on one track — no gaps, no overlaps.

The result is a review timeline that matches how the show is organised on disk.
Discovery runs in the background with a progress bar, and each clip keeps the
full set of convention values as metadata, so the pickers work on it
immediately.

Dropping a folder onto the timeline is deliberately *not* this. That scans only
the top level of the folder, collapses numbered files into sequences, sorts them
by filename and appends them — no sequence/shot grouping at all.

## Exporting

**File ▸ Export** writes a movie, an image sequence, or an OTIO of the current
timeline. Movie export drives a separate bundled FFmpeg binary, so its encoder
set is wider than what jplay links against.

## Next

- [Mouse and keyboard shortcuts](SHORTCUTS.md)
- [Colour management](COLOUR_MANAGEMENT.md) · [Colour tools](COLOR_TOOLS.md)
- [Proxy modes](PROXY_MODES.md) · [Presentation mode](PRESENTATION_MODE.md)
- [Draw tool](DRAW_TOOL.md) · [Sync session](SYNC_SESSION.md)
- [Config files](CONFIG_FILES.md) · [Python](PYTHON.md)

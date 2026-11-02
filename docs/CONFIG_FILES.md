# Config files

jplay reads two configuration files, plus a settings file it writes itself.

| File | Owner | What it holds |
| --- | --- | --- |
| `jplay_preferences.conf` | site / user | Application defaults: colour pipeline, control channel, sync, timeline labels, shared projects. |
| `naming_convention.conf` | site / user | The regexes that turn paths into sequence / shot / department / asset / version, plus the picker list. |
| `~/.jplay/settings.conf` | the app | What the user changed in the UI. Written on every change; wins over the defaults above. |

## jplay_preferences.conf

INI-style: `[section]` headers with `key = value` options. Read once at startup.

Three files are read and merged **option by option**, weakest first, so each
tier overrides only the options it actually names:

1. `<exe dir>/jplay_preferences.conf` — the shipped default
2. `~/.jplay/jplay_preferences.conf` — deployed user override
3. `$JPLAY_PREFERENCES` — explicit override, wins

To change one setting, put just that section and key in your own file; there is
no need to copy the shipped one. A `~/.jplay/jplay_preferences.conf` of only

```ini
[control]
enabled = true
```

turns the control channel on and leaves everything else exactly as shipped.

### Sections

```ini
[shared_projects]
# Per-show project file: a template with a {project} placeholder. The extension
# picks the loader — .jpproj opens as a project, anything else is imported as an
# .otio. Either way the show comes in unsaved, so Save prompts for a location.

[timeline]
# Second row of a timeline clip's label (the first is always the source
# filename). {key} placeholders name picker keys — the [picker:*] sections of
# naming_convention.conf. A placeholder the clip carries no value for is
# dropped and the whitespace collapses.
clip_metadata_label = {department}

[control]
# The loopback listener an MCP server drives jplay through. Off by default, so a
# fresh install binds no socket and Windows raises no firewall prompt.
enabled = false
port    = 52154

[sync]
# Whether sync review may touch the network: LAN discovery, hosting, joining.
# Off by default, for the same reason.
enabled = false

[color_management]
# The colour pipeline the app starts in: srgb or ocio.
color_pipeline = srgb

[ocio]
# Optional: resolve an OCIO config from the media path. See COLOUR_MANAGEMENT.md.
source_regex = /share/project/(?P<project_name>[^/]+)/
ocio         = /share/project/{project_name}/config.ocio
```

`[control] enabled` and `[sync] enabled` are only *defaults*. Once a user has
saved settings of their own, `~/.jplay/settings.conf` wins — the toggles are
**Settings ▸ Advanced ▸ MCP Control Channel** and **Enable Sync Review Socket**.

## naming_convention.conf

Unlike the preferences, this file is **not** merged: the first file found wins.

1. `$JPLAY_FILE_NAMING_TEMPLATE` — explicit override
2. `~/.jplay/naming_convention.conf` — user override
3. `naming_convention.conf` next to a site's own `jplay_naming_convention.py`
4. `<install>/python/naming_convention.conf` — the shipped default

It carries four kinds of section, each order-sensitive (first full match wins,
so list the most specific layouts first):

| Section | Purpose |
| --- | --- |
| `[dir:<name>]` | `dir_regex` — a directory layout. Named groups: `sequence`, `shot`, `department`. |
| `[file:<name>]` | `file_regex` — a filename convention. Named groups: `asset`, `version`, `take`, `frame`, `extension`, and `sequence`/`shot`/`department` when the filename carries them. |
| `[picker:<key>]` | Declares one metadata dropdown. `key` names a regex group; `kind` is `dir`, `file` or `auto`; `label` is the caption; `sort = version` orders latest-first. Section order is display order. |
| `[preferences]` | Ranks departments and assets when **Create From Directory** must pick one clip per shot, lists the site's `departments`, and names the `multi_select_picker`. |

`{departments}` inside a `file_regex` is expanded at load time into an
alternation of `[preferences] departments`, which is what lets a rule peel a
department off a basename without mistaking half of a two-token asset name for
one.

Adding, removing or reordering a picker changes the metadata bar with no code
change. For what the config cannot express, see [Python](PYTHON.md).

## Summary

| File | Contents |
| --- | --- |
| `settings.conf` | UI preferences and the network toggles. Written by the app. |
| `jplay_preferences.conf` | Optional user override (see above). |
| `naming_convention.conf` | Optional user override (see above). |

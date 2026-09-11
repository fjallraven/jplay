# Python

jplay embeds a Python interpreter. The naming convention, the metadata pickers,
**Create From Directory**, the proxy modes and the discovery search the
[MCP control channel](MCP_CONTROL_CHANNEL.md) exposes are all implemented in
Python, in the `python/` directory next to the executable.

| File | Owner | Role |
| --- | --- | --- |
| `jplay_init.py` | shipped | Startup script. Auto-executed; claims the shipped callbacks. |
| `jplay_naming_core.py` | shipped | The whole implementation. Replaced on upgrade — do not edit. |
| `jplay_naming_convention.py` | **you** | Site overrides. Left alone on upgrade. |
| `jplay_discovery.py` | shipped | Project lookup and naming-convention search. |
| `naming_convention.conf` | site | The regexes and pickers — see [config files](CONFIG_FILES.md). |

Reach for the config first, a callback override second, and monkey-patching a
core helper last.

## jplay_init.py

At startup jplay scans every directory on `sys.path` and executes **every**
`jplay_init.py` it finds, in order. If none is found there, it falls back to the
one shipped beside the executable and puts that directory on `sys.path` first.

So a site deploys its own startup script by putting it on `PYTHONPATH`. The
shipped one guards against clobbering it:

```python
if "jplay_naming_convention" not in sys.modules:
    import jplay_naming_core
    jplay_naming_core.register_defaults()
    import jplay_naming_convention

import jplay_discovery
```

Registration is **last wins**. `register_defaults()` claims the shipped
callbacks, then `jplay_naming_convention` is imported, so anything it registers
replaces a shipped callback — and only that one. Every other callback, and every
helper beneath it, stays on the shipped implementation and keeps picking up its
fixes.

Turn on **Settings ▸ Advanced ▸ Debug Logging** to see what got loaded.

## jplay_naming_convention.py

This file is yours. As shipped it is essentially empty, and that is the intended
state: most sites need only `naming_convention.conf`. Use it for behaviour the
config cannot express — picker options from an asset database, a site's own OTIO
location, pairing a published quicktime's audio with an EXR sequence.

Register a callback with `jplay.register_callback(name, fn)`.

### The callbacks

| Callback | Asked for |
| --- | --- |
| `list_pickers()` | The configured pickers, in display order. |
| `describe_pickers(info)` | Per picker: its options and which is current. |
| `decorate_pickers(info, states)` | Relabel / recolour those options (optional). |
| `get_path_values(info)` | Every convention value the path carries. |
| `get_path_context(info)` | Its fixed (sequence, shot, department). |
| `resolve_path(info, key, value)` | The path after a picker is set to `value`. |
| `create_from_directory(root)` | Discover a tree's sequences and shots. |
| `get_project_path_from_media(info)` | The project's `.jpproj`, else its `.otio`. |
| `query_audio(info)` | Audio to pair with an EXR sequence. |
| `list_proxy_modes()` | Options for the global Proxy dropdown. |
| `resolve_proxy_path(path, mode)` | The file to decode for `path` under `mode`. |

`info` is a dict carrying at least `"path"`, the concrete media path.
`jplay_naming_core.py` carries the full contract and return shape for each —
read it there before replacing one.

`jplay_discovery.py` registers four more, for search rather than description:
`describe_convention()`, `list_projects()`, `find_media(query)` and
`find_versions(query)`. These are what the
[MCP control channel](MCP_CONTROL_CHANNEL.md) client turns "load the latest
animation versions on the frog project" into concrete paths with.

### Delegate rather than replace

Calling the shipped function and adjusting its answer keeps its handling of the
cases you are not thinking about — unversioned hero copies, folder-based
layouts, paths matching no rule at all:

```python
import jplay
import jplay_naming_core as core

def describe_pickers(info):
    states = core.describe_pickers(info)          # the on-disk answer ...
    for s in states:
        if s["key"] == "version":                 # ... then one picker from the db
            s["options"] = my_db.versions(info["path"])
            s["current_index"] = my_db.current_index(info["path"])
    return states

jplay.register_callback("describe_pickers", describe_pickers)
```

### Labels, colours and badges

A picker option is normally its own label. To say more — an approval state, a
status, whose version it is — give the option as a dict instead of a string:

- `value` is the identity the naming convention resolves paths with, and the
  only part sent back to the host.
- `label` is what the **Clip Source** panel and the clip right-click menu draw.
- `color` (`"#RRGGBB"` or an `(r, g, b)` tuple) tints that label.
- `badge` is a short word drawn as a filled chip right-aligned in the row, on
  `badge_color` as its background. The chip's text flips between black and white
  by how bright that background is, so one colour per status stays readable.

### Slow lookups

Annotating inside `describe_pickers` costs the option list whatever the lookup
costs — every picker query waits for it. When the annotations come from a
database or a REST call, register `decorate_pickers` instead: it runs as a
second pass over an already-resolved describe result, so the pickers appear at
disk speed and the badges arrive when they arrive.

Make it a **generator** when the whole answer is a long wait. Each `yield` is a
decorate result in its own right, applied to the open picker the moment it
arrives:

```python
def decorate_pickers(info, states):
    for s in states:
        if s["key"] != "version":
            continue
        batch = []
        for o in s["options"]:
            o["badge"], o["badge_color"] = my_db.status(o["value"])   # ~0.2s each
            batch.append(o)
            if len(batch) == 10:
                yield [{"key": "version", "options": batch}]
                batch = []
        if batch:
            yield [{"key": "version", "options": batch}]

jplay.register_callback("decorate_pickers", decorate_pickers)
```

The host stops pulling the generator as soon as the picker closes, so a
dismissed menu costs the lookups it had left rather than finishing them for a
view nobody is looking at. That check happens *between* yields — one yield that
takes twenty seconds cannot be interrupted, which is the whole reason to batch.

Only the four display keys (`label`, `color`, `badge`, `badge_color`) are read
back from a decorate pass: options cannot be added, removed or reordered there,
so decoration can never desync the pickers from what `describe_pickers`
reported.

Decoration runs on its own host thread and the picker never waits on it, so it
cannot freeze the UI — with one exception: work that holds the GIL without
releasing it (a tight Python loop, not a database call or a socket read) blocks
the interpreter. Do the waiting in something that releases.

### Config keys core ignores

A picker's config section may carry keys core knows nothing about; they arrive
untouched, so an override can be driven from the same `naming_convention.conf`
rather than a second config of its own:

```ini
[picker:version]
kind   = file
label  = Version
source = my_asset_db          # core ignores it; your override reads it
```

## Proxy callbacks

`list_proxy_modes()` and `resolve_proxy_path(path, mode)` drive the top-bar
**Proxy** dropdown — see [proxy modes](PROXY_MODES.md) for the full contract.

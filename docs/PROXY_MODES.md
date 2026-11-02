# Proxy modes

A proxy mode is a global answer to "which representation of this media should I
actually decode?" — half-res EXRs, a published quicktime, whatever your farm
renders. Switching mode reopens every clip's decoder against the file the mode
resolves to; the timeline, the cut and the metadata are untouched.

## Using it

1. Turn on **Settings ▸ General ▸ Use Proxy Media**. Off, the toolbar button is
   not laid out at all and playback always decodes the nominal file.
2. Pick a mode from the **Proxy** dropdown in the top toolbar, left of
   **Letterbox**. The built-in **Full** entry means "no substitution".

The selected mode is saved with the project, so it comes back with it.

A mode that has no substitute for a given clip — a shot with no proxy rendered
yet — falls back to that clip's nominal file. There is no error and no gap; the
rest of the timeline still plays proxies.

## Defining the modes

The mode list is not built in. It comes from two
[Python callbacks](PYTHON.md), so what a mode *is* is a site decision:

```python
import os, jplay

def list_proxy_modes():
    return [{"value": "proxy", "label": "Proxy (Half Res)"}]

def resolve_proxy_path(path, mode):
    if mode != "proxy":
        return None
    candidate = os.path.join(os.path.dirname(path), "proxy", os.path.basename(path))
    return candidate if os.path.exists(candidate) else None

jplay.register_callback("list_proxy_modes", list_proxy_modes)
jplay.register_callback("resolve_proxy_path", resolve_proxy_path)
```

- `value` is the identity that round-trips to `resolve_proxy_path` and is saved
  in the project. `label` is display-only.
- `resolve_proxy_path` returns the file to decode instead, or `None` for "no
  substitute — decode the nominal path".
- `""` is the built-in **Full** mode. It is a resolution too, not merely the
  absence of one: a nominal path may itself already be a proxy representation,
  so a site can map Full back to the full-res original.

`list_proxy_modes` is consulted lazily, on the dropdown's first open. A project
saved with a mode already picked shows the raw value until then.

## Slate frames

A published quicktime often opens on a slate card that is not part of the shot,
so its frame 0 is the shot's frame *n*. A mode declares that with
`slate_frames`, and jplay adds the offset to every read on media the mode
actually substituted — the timeline keeps showing shot frames.

A file that is *already* a slated representation — a published quicktime added
directly by a drop or a version jump, rather than reached through a proxy
mode — decodes nominally under every mode, Full included. Only the naming config
can say how much of its head is slate, which is what
`proxy_path_mode(path)` is asked.

## Scope

Proxy resolution is installed by the GUI only. The command-line tools that link
the same media code (`createproject`, `createclips`, …) never embed the
interpreter, so they always decode the nominal path regardless of any mode
string.

## Related

- [Python](PYTHON.md) — the callback contract.
- [Config files](CONFIG_FILES.md) — where the naming config that backs it lives.

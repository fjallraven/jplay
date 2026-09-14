# MCP control channel

A loopback-only TCP listener that lets another process on the same machine drive
jplay: open a project, OTIO or media source, query application state, and switch
clips by metadata pick. It was built for an MCP server to talk to, so an agent
can "load the latest animation versions on the frog project" — but the wire
format is plain enough for any language, or a hand-typed netcat session.

## Turning it on

Off by default, so a fresh install binds no socket and Windows raises no
firewall prompt.

- **Settings ▸ Advanced ▸ MCP Control Channel** — per user, persisted.
- Or as a site default, in `jplay_preferences.conf`:

  ```ini
  [control]
  enabled = true
  port    = 52154
  ```

Leave `port` unset or invalid and the compiled-in fallback (`45125`) is used.

## Finding the channel

When the port actually becomes this instance's, jplay writes
`~/.jplay/control.json`:

```json
{"port": 52154, "pid": 12345}
```

It is not removed on exit, so treat it as a hint and confirm with a `ping`
before trusting the pid.

Binding the port **is** jplay's single-instance lock. The first instance to
start owns the channel; later ones fail to bind and simply don't listen. The
bind is retried every couple of seconds, so if the owner exits a surviving
instance takes the channel over. Every instance that should share the lock must
agree on the port.

## Protocol

- TCP on `127.0.0.1` only — never `INADDR_ANY`. This is not a LAN service.
- One newline-terminated JSON request per connection, one newline-terminated
  JSON response back, then the connection closes.
- Requests are flat objects of scalars; nested objects and arrays are rejected.
- Requests are handled one at a time, so a second client waits behind the first.
- Every response carries `"ok"`. A failure is `{"ok":false,"error":"…"}`.

The socket thread waits up to 120 seconds for the main thread to answer, since a
command may open media off a network share. A timeout does not cancel the
command; it only gives up on reporting the result.

## Commands

### `ping`

```json
{"cmd":"ping"}
→ {"ok":true,"app":"jplay","pid":12345}
```

### `state`

```json
{"cmd":"state"}
```

Returns the project (`path`, `name`, `has_path`, `id`), transport (`fps`,
`playhead`, `length`, `in`, `out`, `playing`, `tracks`), the current view scope,
an array of `sequences` (id, name, project, clip count, start, end), an array of
`sources` (path, type, frames, width, height, whether it is in the timeline,
whether it failed to open), and `current_clip` — the clip under the playhead
with its track, timeline start, duration and source frame, or `null`.

`type` is `exr`, `image`, `video` or `audio`, so a caller can tell a
scene-linear EXR sequence from an 8-bit sRGB one.

### `open`

```json
{"cmd":"open","path":"<file|dir>","mode":"append|replace|bin|sequence"}
```

Dispatches on extension exactly as the command line does: `.jpproj` loads a
project, `.otio` imports one, anything else is media. A directory contributes
one entry per distinct image sequence or video, as a folder drop does.

`mode` applies to media only:

| Mode | Effect |
| --- | --- |
| `append` (default) | Auto-pick a track of the file's kind and append at its end. |
| `replace` | New project first — unsaved changes are discarded, by design. |
| `bin` | Pool only: the source appears in the SOURCES bin with no clip placed. |
| `sequence` | Create a new sequence, make it active, and add into it. |

Replies with `added`, `of`, the new `clip_ids`, and `sequence_id` where one was
created.

### `pick`

```json
{"cmd":"pick","key":"version","value":"v012","scope":"all|sequence|selection"}
```

The control-channel form of `Ctrl`+right-click ▸ *picker* ▸ *value*, applied to a
scope rather than to whatever a human had selected. `key` must name a configured
`[picker:*]`; `value` is resolved per clip by the naming layer's `resolve_path`.

| Scope | Clips |
| --- | --- |
| `all` (default) | Every clip in the timeline. |
| `sequence` | The sequence the view is filtered to. An error when the view covers several. |
| `selection` | Whatever a human left selected. |

Replies with `switched`, `of`, `in_scope` and a per-clip list of `from` / `to`.
`ok` reports that the command ran, not that every clip moved — a shot with no
media for that pick is an expected miss the per-clip list explains.

## Search, before opening

The channel deliberately has no search command. Turning "the latest animation
versions on the frog project" into concrete paths is the job of the Python
discovery layer, which the MCP server calls through the same interpreter:

| Callback | Answers |
| --- | --- |
| `describe_convention()` | The site's vocabulary — which levels its templates capture, which departments and assets are ranked. **Ask first**: it is what lets a caller avoid hardcoding "department" or "Animation" and still work at a site that calls them something else. |
| `list_projects()` | Known projects: the recent list plus the studio's shared shows. |
| `find_media(query)` | Search by sequence / shot / department / asset / version, returning concrete media paths. |
| `find_versions(query)` | The versions available for one identity, and which the query's selector picks. |

The defaults are filesystem-only and read everything they know from
`naming_convention.conf`. A studio with an asset database is expected to
override `find_versions` (so selectors like "pending review" resolve) and/or
`list_projects`, and keep the rest — see [Python](PYTHON.md).

## Security notes

The listener binds loopback only and accepts no remote connections. Anything
able to connect can already run code as the user, so there is no
authentication — but note that `open` with `mode: "replace"` discards unsaved
changes, and `pick` rewrites which media the timeline points at. Leave the
channel off unless something is driving it.

## Related

- [Sync session](SYNC_SESSION.md) — the *other* socket, for LAN review. Separate
  toggle, separate ports (UDP 45777 / TCP 45778).
- [Config files](CONFIG_FILES.md) · [Python](PYTHON.md)

"""Site-owned naming-convention overrides.

Nothing here as shipped, and that is the intended state: the whole implementation
lives in jplay_naming_core.py, and most sites need only naming_convention.conf —
the regexes, the picker list, the preference orders.

This file is for the rest: behaviour the config cannot express, such as picker
options from an asset database, a site's own OTIO document location, or pairing a
published quicktime's audio with an EXR sequence. It is yours to edit, and an
install upgrade leaves it alone — jplay_naming_core.py is what gets replaced.

HOW IT WORKS. jplay_init.py claims the shipped callbacks (jplay_naming_core's
register_defaults) and then imports this file. jplay.register_callback is
last-wins, so a callback registered here replaces the shipped one. Only that one:
the other callbacks, and every helper beneath them, stay on the shipped
implementation and keep picking up its fixes. That is why this file is worth
keeping thin — the more of core it restates, the more of core's fixes have to be
merged in by hand.

THE CALLBACKS, and what each is asked for (jplay_naming_core carries the full
contract and return shape for each — read it there before replacing one):

    list_pickers()                    the configured pickers, in display order
    describe_pickers(info)            per picker: its options and which is current
    decorate_pickers(info, states)    relabel / recolor those options (optional)
    get_path_values(info)             every convention value the path carries
    get_path_context(info)            its fixed (sequence, shot, department)
    resolve_path(info, key, value)    the path after a picker is set to `value`
    create_from_directory(root)       discover a tree's sequences/shots
    get_project_path_from_media(info) the project's .jpproj, else its .otio
    query_audio(info)                 audio to pair with an EXR sequence
    list_proxy_modes()                options for the global Proxy dropdown
    resolve_proxy_path(path, mode)    the file to decode for `path` under `mode`

`info` is a dict carrying at least "path", the concrete media path.

DELEGATE RATHER THAN REPLACE where you can. Calling the shipped function and
adjusting its answer keeps its handling of the cases you are not thinking about —
unversioned hero copies, folder-based layouts, paths matching no rule at all:

    import jplay
    import jplay_naming_core as core

    def describe_pickers(info):
        states = core.describe_pickers(info)      # the on-disk answer ...
        for s in states:
            if s["key"] == "version":             # ... then one picker from the db
                s["options"] = my_db.versions(info["path"])
                s["current_index"] = my_db.current_index(info["path"])
        return states

    jplay.register_callback("describe_pickers", describe_pickers)

LABELS, COLORS AND BADGES. A picker option is normally its own label, drawn in the
panel's own color. To say more than the name — an approval state, a status, whose
version it is — give the option as a dict instead of a string. "value" is the
identity (the version/asset/department the naming convention resolves paths with,
and the only part sent back to the host); "label" is what the Component Picker
panel, the Clip Source panel and the clip right-click menu draw, and "color"
("#RRGGBB", or an (r, g, b) tuple of 0-255 ints) tints that label.

"badge" says it without touching either. It is a short word — the status itself —
drawn as a filled chip right-aligned in the row, on "badge_color" as its
background, with the option's own label and color left as they were. The chip's
text is black or white by how bright that background is, so a site picks one
color per status and the word stays readable on all of them. In the two side
panels, that is; the clip right-click menu, whose columns are only as wide as
their options, draws "badge_color" as a small square and leaves the word to the
panels — so keep one color per status and the menu still says it. A bad color is
ignored rather than raised on, and pure black means "no color": the view's own for
"color", a neutral grey for "badge_color".

    def describe_pickers(info):
        states = core.describe_pickers(info)
        for s in states:
            if s["key"] == "version":
                s["options"] = [
                    {"value": v, "badge": "Approved", "badge_color": "#5FC46A"}
                    if my_db.approved(v) else v
                    for v in s["options"]
                ]
        return states

    jplay.register_callback("describe_pickers", describe_pickers)

That form costs the option lists whatever the lookup costs — every picker query
waits for it. When the annotations come from somewhere slow (a database, a farm,
a REST call), register decorate_pickers instead. It runs as a second pass over an
already-resolved describe result, so the pickers appear at disk speed and the
badges and colors arrive when they arrive:

    def decorate_pickers(info, states):
        for s in states:
            if s["key"] != "version":
                continue
            status = my_db.statuses(info["path"])     # {version: state}, one call
            for o in s["options"]:
                state = status.get(o["value"])
                if state:
                    o["badge"] = state
                    o["badge_color"] = {"Approved": "#5FC46A",
                                        "Pending":  "#E0B040"}.get(state)
        return states

    jplay.register_callback("decorate_pickers", decorate_pickers)

`states` arrives in describe_pickers' own shape with every option normalised to a
{"value", "label", "color", "badge", "badge_color"} dict, and comes back the same
way — return only the pickers you touched, and only the keys you changed. ONLY
those four display keys are read back: options cannot be added, removed or
reordered here and current_index is ignored, so decoration can never desync the
pickers from what describe_pickers reported. Change the option list there instead.

YIELD INSTEAD OF RETURN when the lookup is slow enough that the whole answer is a
long wait — a hundred versions at a fifth of a second each is twenty seconds of
nothing. Make decorate_pickers a generator and each yield is a decorate result in
its own right, in the same shape as the return value, applied to the open picker
the moment it arrives:

    def decorate_pickers(info, states):
        for s in states:
            if s["key"] != "version":
                continue
            batch = []
            for o in s["options"]:
                o["badge"], o["badge_color"] = my_db.status(o["value"])  # ~0.2s each
                batch.append(o)
                if len(batch) == 10:
                    yield [{"key": "version", "options": batch}]
                    batch = []
            if batch:
                yield [{"key": "version", "options": batch}]

    jplay.register_callback("decorate_pickers", decorate_pickers)

The options badge in tens rather than all at the end, and the host stops pulling
the generator as soon as the picker closes or the user clicks on — abandoning it
mid-run, so a dismissed menu costs the ninety lookups it had left rather than
finishing them for a view nobody is looking at. That check happens BETWEEN
yields: one that takes twenty seconds cannot be interrupted, which is the whole
reason to yield in batches rather than once at the end.

None of this can freeze the UI — decoration runs on its own host thread and the
picker never waits on it — with one exception worth knowing: work that holds the
GIL without releasing it (a tight Python loop, not a database call or a socket
read) blocks the interpreter, and the main thread does occasionally need it. Do
the waiting in something that releases: a DB client, requests, subprocess, sleep.

Nothing is picker-specific about either form — any picker can be annotated, and a
site can do both (color what it knows cheaply in describe_pickers, fill in the
rest from the database in decorate_pickers).

A picker's config section may carry keys core knows nothing about; they arrive in
core._PICKERS untouched, so an override can be driven from the same
naming_convention.conf rather than a second config of its own:

    [picker:version]
    kind   = file
    label  = Version
    source = my_asset_db          # core ignores it; the override above reads it

If you must change something *inside* a callback rather than the whole callback —
different capture-group names, a different sibling scan — assign over the core
module's attribute (core._match_dir_rel = mine) before the first query. It works,
but that helper is then forked, and stops inheriting changes. Reach for the config
first, a callback override second, and this last.
"""

# Imported for the delegate pattern above, and so this module stands on its own.
# Note the import does NOT register anything — jplay_init.py claims the shipped
# callbacks (core.register_defaults) before importing this file, so overrides here
# land afterwards and win.
import jplay_naming_core  # noqa: F401

import os, random, time, jplay

# Stand-in for a review database: the states a version can be in, each with the
# chip color that says it. The label and the color of the option are left alone —
# the status draws as a badge on the right of the row.
_STATUSES = [
    ("Approved",  "#5FC46A"),
    ("Pending",   "#E0B040"),
    ("On hold",   "#C4552F"),
    ("Retake",    "#B03050"),
    ("Published", "#4F8CD6"),
    ("WIP",       "#8A8F9A"),
]

def decorate_pickers(info, states):
    for s in states:
        if s["key"] != "version":
            continue
        batch = []
        for o in s["options"]:
            time.sleep(0.2)               # pretend it's a database, one call per version
            o["badge"], o["badge_color"] = random.choice(_STATUSES)
            batch.append(o)
            if len(batch) == 5:           # ... and hand over what's done so far
                yield [{"key": "version", "options": batch}]
                batch = []
        if batch:
            yield [{"key": "version", "options": batch}]
jplay.register_callback("decorate_pickers", decorate_pickers)

# Demo for the global Proxy dropdown (see createclips, which renders a
# half-resolution sibling of every clip into a "proxy" subfolder next to it).
# A real site would point "value" at whatever its render farm calls the proxy
# representation and resolve_proxy_path against wherever that farm publishes it.
def list_proxy_modes():
    return [{"value": "proxy", "label": "Proxy (Half Res)"}]
jplay.register_callback("list_proxy_modes", list_proxy_modes)

def resolve_proxy_path(path, mode):
    if mode != "proxy":
        return None
    candidate = os.path.join(os.path.dirname(path), "proxy", os.path.basename(path))
    return candidate if os.path.exists(candidate) else None
jplay.register_callback("resolve_proxy_path", resolve_proxy_path)

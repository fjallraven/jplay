"""Project lookup and naming-convention search.

jplay_naming_core.py answers "what is this path?" — every callback it
registers takes a concrete media path and describes it. None of them answers a
*search*: "which media is the latest Animation version in this project?". These
callbacks fill that gap, so an external driver (the Claude Code plugin under
claude-plugin/) can turn a request like "load the latest animation versions on
the frog project" into concrete paths before asking jplay to open them.

Registered callbacks:

  describe_convention()   the site's vocabulary — which levels its templates
                          capture, which departments/assets are ranked, whether
                          version status is tracked at all. Ask FIRST: it is what
                          lets a caller avoid hardcoding "department" or
                          "Animation" and still work at a site that calls them
                          something else.
  list_projects()         known projects: the recent list plus the studio's
                          shared shows.
  find_media(query)       search by sequence / shot / department / asset /
                          version, returning concrete media paths.
  find_versions(query)    the versions available for one identity, and which of
                          them the query's selector picks.

CUSTOMISING. These are ordinary host callbacks, so a site replaces one by
registering its own after this module is imported:

    import jplay, jplay_discovery
    jplay.register_callback("list_projects", my_show_database_lookup)
    jplay_discovery.set_version_provider(my_asset_database_lookup)

The defaults below are filesystem-only and read everything they know from
naming_convention.conf. A studio with an asset database is expected to override
find_versions (so selectors like "pending review" resolve) and/or list_projects,
and to keep the rest. find_media routes its version dimension through
find_versions, so replacing version policy alone changes both — use
set_version_provider() rather than register_callback() so it takes effect in
both places.

This module reaches into jplay_naming_core's private helpers (_analyze,
_collapse_media, _match_dir_rel, ...) deliberately: the two files ship and
version together as one unit, and re-implementing the matching rules here is
exactly the divergence that splitting dir shape from file shape in
naming_convention.conf exists to prevent. Core, not the site-owned
jplay_naming_convention.py — a search reads the shipped rules even where a site
has overridden how a single path is described.
"""

import configparser
import fnmatch
import os
import re

import jplay
import jplay_naming_core as nc


def _dbg(msg):
    """Log a step message when Settings > Python Debug is enabled."""
    if jplay.is_debug():
        jplay.log.info("[debug]   " + msg)


# ─────────────────── config resolution ───────────────────

def _preferences_path():
    """Resolve jplay_preferences.conf the way the app documents it:
        1. $JPLAY_PREFERENCES                       explicit override
        2. ~/.jplay/jplay_preferences.conf          deployed user override
        3. <exe dir>/jplay_preferences.conf         shipped default
    Step 3 needs the executable's directory, which only the host knows; $JPLAY_EXE
    supplies it out-of-process (the MCP plugin sets it), and failing that we fall
    back to the repo root, since this file lives in <root>/python."""
    env = os.environ.get("JPLAY_PREFERENCES")
    if env:
        return env
    user_dir = nc._user_config_dir()
    user = os.path.join(user_dir, "jplay_preferences.conf") if user_dir else ""
    if user and os.path.isfile(user):
        return user
    exe = os.environ.get("JPLAY_EXE")
    if exe:
        candidate = os.path.join(os.path.dirname(exe), "jplay_preferences.conf")
        if os.path.isfile(candidate):
            return candidate
    here = os.path.dirname(os.path.abspath(__file__))
    return os.path.join(os.path.dirname(here), "jplay_preferences.conf")


def _read_conf(path):
    cfg = configparser.ConfigParser()
    try:
        cfg.read(path)
    except configparser.Error as e:
        _dbg(f"cannot parse {path!r}: {e}")
    return cfg


def _version_statuses():
    """Version states this site recognises, from an optional
    `[preferences] version_statuses` list in the naming config. Empty (the
    shipped default) means version status is not tracked here at all — a
    filesystem layout carries no review state — so a selector like
    "pending review" is reported as unsupported rather than silently ignored."""
    cfg = _read_conf(nc._config_path())
    if not cfg.has_option("preferences", "version_statuses"):
        return []
    raw = cfg.get("preferences", "version_statuses")
    return [x.strip() for x in raw.split(",") if x.strip()]


# ─────────────────── convention vocabulary ───────────────────

# Every named group the two template sets can capture, in the order they nest.
_KNOWN_LEVELS = ("sequence", "shot", "department", "asset", "version", "frame")


def describe_convention():
    """What this site's naming convention can be asked about:

        levels            named groups the templates actually capture, so a
                          caller knows whether "department" exists here
        pickers           the metadata-bar dropdowns ([picker:*])
        departments       ranked department preference ([preferences])
        assets            ranked asset preference
        version_statuses  recognised review states; EMPTY means not tracked
        version_selectors everything find_versions accepts for `version`

    Static — independent of any path. A caller should read this before building a
    find_media query, so it uses this site's vocabulary rather than assuming the
    shipped example's."""
    statuses = _version_statuses()
    levels = [g for g in _KNOWN_LEVELS
              if any(g in t.anchored.groupindex for t in nc._DIRS)
              or any(g in rx.groupindex for _name, rx in nc._FILES)]
    return {
        "levels": levels,
        "pickers": [{"key": p["key"], "label": p["label"], "kind": p["kind"],
                     "sort": p["sort"]} for p in nc._PICKERS],
        "departments": list(nc._DEPT_PREFS),
        "assets": list(nc._ASSET_PREFS),
        "version_statuses": statuses,
        "version_selectors": ["latest", "all", "v<NNN>", "<number>"] + statuses,
        "dir_templates": [t.name for t in nc._DIRS],
        "file_templates": [name for name, _rx in nc._FILES],
        "media_extensions": sorted(nc._MEDIA_EXTS),
        "config_path": nc._config_path(),
    }


# ─────────────────── projects ───────────────────

def _recent_projects():
    """The recent-projects list from ~/.jplay/settings.conf, most-recent first.
    Format is one "id|unix|path" line per entry under [recentProjects], written
    by UserData.cpp."""
    base = nc._user_config_dir()
    path = os.path.join(base, "settings.conf") if base else ""
    if not path or not os.path.isfile(path):
        return []
    out = []
    in_section = False
    try:
        with open(path, "r", encoding="utf-8", errors="replace") as f:
            for line in f:
                line = line.strip()
                if line.startswith("["):
                    in_section = (line == "[recentProjects]")
                    continue
                if not in_section or not line:
                    continue
                parts = line.split("|", 2)
                if len(parts) != 3:
                    continue
                project_id, when, ppath = (p.strip() for p in parts)
                if not ppath:
                    continue
                root = os.path.dirname(ppath)
                name = os.path.splitext(os.path.basename(ppath))[0]
                otio = os.path.join(root, name + ".otio")
                out.append({
                    "name": name,
                    "code": "",
                    "root": root,
                    "project_file": ppath,
                    "otio": otio if os.path.isfile(otio) else "",
                    "id": project_id,
                    "last_opened": int(when) if when.isdigit() else 0,
                    "source": "recent",
                })
    except OSError as e:
        _dbg(f"cannot read {path!r}: {e}")
    return out


def _shared_projects():
    """The studio's shared shows, from jplay_preferences [shared_projects]:
    projects_ini_config lists "CODE,Long Name" lines (section headers ignored,
    matching SharedProjects::parse) and project_otio_path is a template with a
    {project} placeholder.

    Deliberately does NOT stat the resolved OTIO paths. jplay checks them on a
    work-queue thread under a wall-clock budget because std::filesystem::exists
    can hang forever on a dead mount; a synchronous stat here would hang the
    caller instead. Entries are candidates — reaching one is what confirms it."""
    cfg = _read_conf(_preferences_path())
    ini = cfg.get("shared_projects", "projects_ini_config", fallback="").strip()
    tmpl = cfg.get("shared_projects", "project_otio_path", fallback="").strip()
    if not ini or not tmpl or not os.path.isfile(ini):
        return []
    out = []
    try:
        with open(ini, "r", encoding="utf-8", errors="replace") as f:
            for line in f:
                line = line.strip()
                if not line or line[0] in "#;[" or "," not in line:
                    continue
                code, name = (x.strip() for x in line.split(",", 1))
                if not code:
                    continue
                otio = tmpl.replace("{project}", code)
                out.append({
                    "name": name or code,
                    "code": code,
                    "root": os.path.dirname(otio),
                    "project_file": "",
                    "otio": otio,
                    "id": "",
                    "last_opened": 0,
                    "source": "shared",
                })
    except OSError as e:
        _dbg(f"cannot read {ini!r}: {e}")
    return out


def list_projects():
    """Every project this site knows about: the recent list first (newest first),
    then the studio's shared shows. Each entry carries `name`, `code`, `root`
    (the media root to search), `project_file` (.jpproj, when there is one),
    `otio`, and `source` ("recent" or "shared").

    Existence is not verified — see _shared_projects. `root` for a recent project
    is simply the .jpproj's directory, which is the media root only when the
    project file sits at the top of its tree; pass an explicit `root` to
    find_media when it does not."""
    projects = _recent_projects() + _shared_projects()
    _dbg(f"list_projects -> {len(projects)}")
    return projects


# ─────────────────── versions ───────────────────

def _collapse(directory, files):
    """Group `directory`'s media into asset -> {version_number: (path, token)},
    keeping an image sequence's first frame as its representative.

    This is nc._collapse_media with one difference: it does NOT fold the versions
    of an image sequence together. _collapse_media checks for a `frame` group
    before it ever reads `version`, so main_v001.0001.exr .. main_v004.0001.exr
    all land on identity "main" at version 0 — the whole run collapses to its
    lowest frame across every version, which is v001, the OLDEST. Searching for
    "the latest version" on such a layout has to see the versions separately, so
    the frame collapse is keyed per (asset, version) here."""
    assets = {}
    first_frame = {}  # (asset, version_number) -> lowest frame number kept
    for name in files:
        full = os.path.join(directory, name)
        if not nc._is_media(name) or not os.path.isfile(full):
            continue
        em, _ = nc._match_file(name)
        if em is None:
            continue
        asset = nc._identity(em, name)
        token = nc._group(em, "version")
        number = nc._ver_number(token) if token else 0
        frame = nc._group(em, "frame")
        if frame is not None:
            key = (asset, number)
            seen = first_frame.get(key)
            if seen is not None and seen <= int(frame):
                continue
            first_frame[key] = int(frame)
        assets.setdefault(asset, {})[number] = (full, token or "")
    return assets


def _candidates(asset, versions):
    """Version candidates for one asset, newest first. `versions` is
    _collapse_media's {version_number: (path, token)} map. `status` is None —
    "not tracked" — because a filesystem layout carries no review state; an
    override that knows better fills it in."""
    return [{"asset": asset, "version": token, "number": number,
             "path": os.path.normpath(path), "status": None}
            for number, (path, token) in sorted(versions.items(), reverse=True)]


def _identity_dir(query):
    """The directory an identity dict names, for the folder-based layouts:
    root/sequence/shot/department, skipping the levels the query omits."""
    parts = [query.get("root") or ""]
    parts += [query.get(k) or "" for k in ("sequence", "shot", "department")]
    return os.path.join(*[p for p in parts if p]) if parts[0] else ""


def _versions_from_disk(query):
    """Every version of one asset on disk. `query` identifies it either by
    "path" (a concrete media file, whose asset identity and directory are read
    off it) or by root/sequence/shot/department + optional asset."""
    path = query.get("path") or ""
    if path:
        a = nc._analyze(path)
        directory, asset = a.directory, nc._identity(a.fm, a.base)
    else:
        directory, asset = _identity_dir(query), query.get("asset") or ""
    if not directory or not os.path.isdir(directory):
        return []
    try:
        files = os.listdir(directory)
    except OSError as e:
        _dbg(f"cannot list {directory!r}: {e}")
        return []
    assets = _collapse(directory, files)
    if not assets:
        return []
    target = asset if asset in assets else nc._pick(assets.keys(), nc._ASSET_PREFS)
    return _candidates(target, assets[target])


def _select_versions(candidates, selector):
    """Apply a version selector to candidates (newest first). Returns the
    find_versions result dict. `available` is always the full candidate list, so
    a caller can explain a miss instead of just reporting nothing."""
    result = {"selector": selector, "selected": [], "available": candidates,
              "supported": True, "note": ""}
    if not candidates:
        result["note"] = "no versions found"
        return result

    labels = [c["version"] or "(unversioned)" for c in candidates]
    sel = str(selector).strip()
    low = sel.lower()

    if low in ("", "latest"):
        result["selected"] = [candidates[0]]
        return result
    if low == "all":
        result["selected"] = list(candidates)
        return result

    # An explicit version: a token ("v003") or a bare number ("3").
    if re.fullmatch(r"v?\d+", low):
        number = nc._ver_number(low)
        hit = [c for c in candidates
               if c["number"] == number or c["version"].lower() == low]
        result["selected"] = hit
        if not hit:
            result["note"] = f"no version {sel!r}; available: {labels}"
        return result

    # Anything else is a review status. Statuses are site-defined, so there is no
    # enum to validate against — but if nothing carries a status, say so loudly
    # rather than quietly falling back to the latest version, which would load
    # the wrong media and look like it worked.
    present = sorted({c["status"] for c in candidates if c.get("status")})
    if not present:
        result["supported"] = False
        result["note"] = (f"this site does not track version status, so {sel!r} "
                          f"cannot be resolved; available versions: {labels}")
        return result
    hit = [c for c in candidates if (c.get("status") or "").lower() == low]
    result["selected"] = hit
    if not hit:
        result["note"] = f"no version with status {sel!r}; present: {present}"
    return result


def find_versions(query):
    """Resolve the version dimension for one asset.

    query:
      path        a concrete media file identifying the asset (preferred), or
      root/sequence/shot/department/asset
                  the same identity spelled out
      version     the selector: "latest" (default), "all", an explicit token or
                  number ("v003" / 3), or a review status ("pending review").
                  Statuses are site-defined; describe_convention()
                  ["version_statuses"] lists them, and an empty list there means
                  this site does not track them.
      candidates  optional pre-collected candidate list, so find_media does not
                  pay for a second directory listing. An override is free to
                  ignore it and ask its own source instead.

    Returns {selector, selected, available, supported, note}. `supported` is
    False when the selector cannot be answered at this site at all (a status
    selector against a filesystem layout); `selected` empty with supported True
    means the selector was understood and simply matched nothing."""
    selector = query.get("version") or "latest"
    candidates = query.get("candidates")
    if candidates is None:
        candidates = _versions_from_disk(query)
    candidates = sorted(candidates, key=lambda c: c.get("number", 0), reverse=True)
    result = _select_versions(candidates, selector)
    _dbg(f"find_versions({selector!r}) -> {len(result['selected'])} of "
         f"{len(candidates)}")
    return result


def set_version_provider(fn):
    """Replace the version lookup used by BOTH find_versions and find_media.

    find_media calls find_versions by module-global name, so registering a
    replacement with jplay.register_callback alone would change what an external
    caller gets while leaving find_media on the filesystem default. This rebinds
    both, which is almost always what a site overriding version policy wants."""
    global find_versions
    find_versions = fn
    jplay.register_callback("find_versions", fn)


# ─────────────────── media search ───────────────────

def _matches(value, pattern):
    """Case-insensitive match of a captured level against a query filter: exact,
    or a glob ("Culinary*"). An empty filter or "*" matches everything."""
    if not pattern or pattern == "*":
        return True
    value, pattern = (value or "").lower(), pattern.lower()
    return value == pattern or fnmatch.fnmatch(value, pattern)


def _resolve_root(query):
    """The directory to search: an explicit `root`, else the root of the named
    `project` (matched case-insensitively against list_projects' name or code).
    A project file rather than a directory resolves to its containing folder."""
    root = query.get("root") or ""
    if not root:
        name = (query.get("project") or "").strip().lower()
        if name:
            for p in list_projects():
                if name in (p["name"].lower(), (p["code"] or "").lower()):
                    root = p["root"]
                    break
    if not root:
        return ""
    if os.path.isfile(root):
        root = os.path.dirname(root)
    return os.path.abspath(root)


def find_media(query):
    """Search a project for media matching a naming-convention query.

    query (every filter optional):
      project / root   what to search; `root` wins. A project name is matched
                       against list_projects().
      sequence, shot, department, asset
                       level filters — exact (case-insensitive) or a glob.
      asset            omitted means "the preferred asset per shot" (the
                       [preferences] assets ranking, matching what CREATE FROM
                       DIRECTORY picks). Pass "*" for every asset instead.
      version          selector handed to find_versions; default "latest".
      limit            stop after this many results.

    Returns {root, media, count, unsupported, note}. Each media entry carries
    path, sequence, shot, department, asset, version, version_number and status.

    Unlike inspect_directory this does NOT stop at the first media-bearing
    directory per shot — that early exit is what makes filtering by department
    impossible, and filtering is the whole point here."""
    root = _resolve_root(query)
    out = {"root": root, "media": [], "count": 0, "unsupported": False, "note": ""}
    if not root:
        out["note"] = "no root: pass \"root\", or a \"project\" that list_projects knows"
        return out
    if not os.path.isdir(root):
        out["note"] = f"not a directory: {root}"
        return out

    seq_f = query.get("sequence") or ""
    shot_f = query.get("shot") or ""
    dept_f = query.get("department") or ""
    asset_f = query.get("asset") or ""
    selector = query.get("version") or "latest"
    try:
        limit = int(query.get("limit") or 0)
    except (TypeError, ValueError):
        limit = 0

    # (sequence, shot, department) -> [(asset, versions)], so the preferred-asset
    # choice below can be made per shot the way inspect_directory makes it.
    groups = {}
    for dirpath, subdirs, files in os.walk(root):
        # Deterministic descent, and skip dot-directories (.thumbnails and the
        # like hold no media but would otherwise be matched and listed).
        subdirs[:] = sorted((d for d in subdirs if not d.startswith(".")),
                            key=nc._natural_key)
        if not files:
            continue
        rel = os.path.relpath(dirpath, root).replace("\\", "/")
        if rel == "." or nc._match_dir_rel(rel) is None:
            continue
        assets = _collapse(dirpath, files)
        if not assets:
            continue
        for asset, versions in assets.items():
            # Levels come from a representative path rather than from the
            # directory match, so the filename-wins precedence of _seq_shot_dept
            # applies — a {sequence}_{shot}_{department} basename convention puts
            # several shots in one folder, and those must not collapse together.
            rep = versions[max(versions)][0]
            seq, shot, dept = nc._seq_shot_dept(rep)
            if not (_matches(seq, seq_f) and _matches(shot, shot_f)
                    and _matches(dept, dept_f)):
                continue
            if asset_f and asset_f != "*" and not _matches(asset, asset_f):
                continue
            groups.setdefault((seq, shot, dept), []).append((asset, versions))

    for key in sorted(groups, key=lambda k: tuple(nc._natural_key(x) for x in k)):
        seq, shot, dept = key
        entries = groups[key]
        if not asset_f:
            # No asset filter: one clip per shot, the preferred asset.
            chosen = nc._pick([a for a, _v in entries], nc._ASSET_PREFS)
            entries = [(a, v) for a, v in entries if a == chosen]
        for asset, versions in sorted(entries, key=lambda e: nc._natural_key(e[0])):
            candidates = _candidates(asset, versions)
            picked = find_versions({"path": candidates[0]["path"],
                                    "candidates": candidates,
                                    "version": selector})
            if not picked.get("supported", True):
                # The selector can't be answered at this site at all, so it will
                # fail identically for every shot: report once and stop.
                out["unsupported"] = True
                out["note"] = picked.get("note", "")
                out["media"] = []
                out["count"] = 0
                return out
            for c in picked["selected"]:
                out["media"].append({
                    "path": c["path"],
                    "sequence": seq,
                    "shot": shot,
                    "department": dept,
                    "asset": c["asset"],
                    "version": c["version"],
                    "version_number": c["number"],
                    "status": c["status"],
                })
                if limit and len(out["media"]) >= limit:
                    out["count"] = len(out["media"])
                    out["note"] = f"stopped at limit {limit}"
                    return out

    out["count"] = len(out["media"])
    if not out["media"]:
        out["note"] = "nothing matched"
    _dbg(f"find_media({root!r}) -> {out['count']}")
    return out


# Wire up to the host, alongside the naming-convention callbacks. `jplay` is the
# embedded module the C++ side injects; the Claude Code plugin supplies the same
# surface as a stub so these are usable out of process too.
jplay.register_callback("describe_convention", describe_convention)
jplay.register_callback("list_projects", list_projects)
jplay.register_callback("find_media", find_media)
jplay.register_callback("find_versions", find_versions)

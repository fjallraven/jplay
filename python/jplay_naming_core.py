"""The shipped naming-convention implementation: every host callback that answers
"what is this path?", plus the matching machinery behind them.

This file ships with the install and is replaced wholesale on upgrade — do not
edit it. Sites customise in one of two places:

  naming_convention.conf   the regexes, the picker list, the preference orders.
                           Enough for most layouts, and nothing to maintain.

  jplay_naming_convention.py   a site-owned Python file, for behaviour the config
                           cannot express (options from an asset database, a
                           published-quicktime audio pairing, ...). It overrides
                           a callback by re-registering it; see that file.

register_defaults() at the bottom claims each callback below under its host name,
and jplay.register_callback is last-wins, so a site replaces one behaviour by
registering its own afterwards — which is why jplay_init.py calls register_defaults
before importing the site file. Importing this module registers nothing on its own.
Overriding one callback leaves the other seven — and every helper below — on the
shipped implementation, so a core fix still reaches the site. That is the point of
the split: the old arrangement had the whole file in site hands, and a fix to any
part of it had to be merged by hand into every fork.

A site that needs to reach *inside* a callback — different capture-group names,
a different sibling scan — can assign over an attribute of this module
(jplay_naming_core._match_dir_rel = mine) before the first query. That works, but
it is a genuine fork of that helper: it re-inherits nothing when the shipped one
changes. Prefer the config, then a callback override, then this."""

import os
import re
import time
import traceback
import configparser
from functools import wraps

import jplay


# ─────────────────── naming config ───────────────────
#
# The config splits directory shape from filename shape into two independent,
# order-sensitive rule sets, plus a list of metadata-bar pickers (see
# naming_convention.conf):
#   [dir:*]    match a directory layout   -> sequence, shot, department
#   [file:*]   match a filename basename  -> asset, version, frame, extension
#   [picker:*] a metadata-bar dropdown over one of those groups (kind=dir|file)
# Both the metadata pickers and CREATE FROM DIRECTORY consume the same rules, so
# there is a single source of truth for the naming convention.
#
# `_config_path()` resolves the config so a deployed user can override the
# install default with their own copy (see below). A [picker:*] section may carry
# keys this module knows nothing about; they are passed through to the picker dict
# untouched, which is how a site-owned override reads its own settings from the
# same config rather than parsing a second one.

# Container extensions treated as media (mirrors MediaScan.cpp). A file is only
# considered by the pickers / discovery when its extension is in here, so the
# catch-all [file:plain] rule can stay generic without matching .txt/.png/etc.
_VIDEO_EXTS = {
    ".mov", ".mp4", ".m4v", ".mkv", ".avi", ".mxf", ".webm", ".mpg", ".mpeg",
    ".m2v", ".m2ts", ".mts", ".ts", ".wmv", ".flv", ".y4m", ".ogv", ".3gp",
}
_MEDIA_EXTS = _VIDEO_EXTS | {".exr"}


class _DirTemplate:
    """A [dir:*] layout, compiled two ways: `anchored` for root-relative
    discovery matching, and `tail` for the pickers, which see an absolute
    directory path with no known root (any leading directories are allowed)."""
    def __init__(self, name, pattern):
        self.name = name
        self.anchored = re.compile(pattern, re.IGNORECASE)
        body = pattern[1:] if pattern.startswith("^") else pattern
        self.tail = re.compile(r"(?:.*/)?" + body, re.IGNORECASE)


def _user_config_dir():
    """The app's per-user data dir (~/.jplay), matching UserData.cpp. Empty
    string if neither USERPROFILE nor HOME is set."""
    home = os.environ.get("USERPROFILE") or os.environ.get("HOME") or ""
    return os.path.join(home, ".jplay") if home else ""


def _site_module_dir():
    """The directory holding the site-owned jplay_naming_convention.py, if one is
    reachable — located without importing it (this runs while core is still being
    imported, and jplay_init.py imports core first). Empty when it resolves to
    nothing, and it may well resolve to this same directory for a plain install.

    Only needed because a deployment can point $PYTHONPATH / $JPLAY_PYTHON_DIR at
    a folder holding both its override file and its own naming_convention.conf.
    Before the core/site split those were one file, so "next to the module" found
    that conf; this keeps that working now that core lives elsewhere."""
    try:
        import importlib.util
        spec = importlib.util.find_spec("jplay_naming_convention")
    except Exception:
        return ""
    origin = getattr(spec, "origin", "") if spec else ""
    return os.path.dirname(os.path.abspath(origin)) if origin else ""


def _config_path():
    """Resolve the naming_convention.conf to load, in priority order:
        1. $JPLAY_FILE_NAMING_TEMPLATE      explicit override (unchanged)
        2. ~/.jplay/naming_convention.conf  user override (deployed customisation)
        3. <site override's dir>/naming_convention.conf   deployed alongside the
           site's jplay_naming_convention.py (see _site_module_dir)
        4. <this dir>/naming_convention.conf  install default (shipped)
    First existing file wins. If none exist, returns the install-default path so
    the "missing sections" error below names a concrete file."""
    env = os.environ.get("JPLAY_FILE_NAMING_TEMPLATE")
    if env:
        return env
    install = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                           "naming_convention.conf")
    user_dir = _user_config_dir()
    user = os.path.join(user_dir, "naming_convention.conf") if user_dir else ""
    if user and os.path.isfile(user):
        return user
    site_dir = _site_module_dir()
    site = os.path.join(site_dir, "naming_convention.conf") if site_dir else ""
    if site and os.path.isfile(site):
        return site
    return install


def _load_config():
    """Load [dir:*] / [file:*] / [picker:*] templates and [preferences] from the
    naming config. Returns (dir_templates, file_templates, pickers, dept_prefs,
    asset_prefs, multi_picker). `pickers` is a list of {key, kind, label, sort, ...}
    in section order (the metadata-bar display order), carrying any further keys the
    section set verbatim for site overrides to read. `multi_picker` is the
    [preferences] multi_select_picker key, "" when unset — see list_pickers.

    A file_regex may carry the placeholder {departments}, expanded here into an
    alternation of the [preferences] departments list — one source of truth for
    what a department is called, and the only way a rule can peel a department off
    a basename without swallowing the second half of a two-token asset name:

        main_Compositing_v001_0001.exr -> asset=main department=Compositing
        bg_smoke_v001.0001.exr         -> no match (asset stays bg_smoke)

    With no departments configured the placeholder becomes a never-matching group,
    which switches those rules off rather than letting them match everything."""
    conf_path = _config_path()
    cfg = configparser.ConfigParser()
    cfg.read(conf_path)

    def prefs(key):
        if cfg.has_option("preferences", key):
            return [x.strip() for x in cfg.get("preferences", key).split(",") if x.strip()]
        return []

    dept_prefs = prefs("departments")
    # "(?!)" never matches, so an unconfigured departments list disables the rules
    # that use the placeholder instead of turning them into catch-alls.
    dept_alt = "|".join(re.escape(d) for d in dept_prefs) or "(?!)"

    dirs, files, pickers = [], [], []
    for section in cfg.sections():
        if section.startswith("dir:"):
            dirs.append(_DirTemplate(section[len("dir:"):], cfg.get(section, "dir_regex")))
        elif section.startswith("file:"):
            # str.replace, not str.format: a site regex may legitimately contain
            # {2,4} repetition braces that format() would choke on.
            pattern = cfg.get(section, "file_regex").replace("{departments}", dept_alt)
            files.append((section[len("file:"):], re.compile(pattern, re.IGNORECASE)))
        elif section.startswith("picker:"):
            key = section[len("picker:"):]
            # Every option in the section is carried, so a site key (say a
            # `source` naming an asset database) reaches an overriding callback
            # through _PICKERS without this module having to know it exists. The
            # four below are the ones the shipped code reads, defaulted here.
            p = {k: v.strip() for k, v in cfg.items(section)}
            p["key"] = key
            p.setdefault("kind", "file")
            p.setdefault("label", key)
            p.setdefault("sort", "natural")
            pickers.append(p)

    multi = cfg.get("preferences", "multi_select_picker", fallback="").strip()

    if not dirs or not files:
        raise RuntimeError(
            f"naming config needs [dir:*] and [file:*] sections: {conf_path}")
    return dirs, files, pickers, dept_prefs, prefs("assets"), multi


_DIRS, _FILES, _PICKERS, _DEPT_PREFS, _ASSET_PREFS, _MULTI_PICKER = _load_config()


# ─────────────────── cache ───────────────────

def _freeze(v):
    if isinstance(v, dict):
        return tuple(sorted((k, _freeze(val)) for k, val in v.items()))
    if isinstance(v, (list, tuple)):
        return tuple(_freeze(i) for i in v)
    return v


def timed_cache(seconds):
    """Per-call cache that expires entries after `seconds`."""
    def decorator(fn):
        _cache = {}

        @wraps(fn)
        def wrapper(*args, **kwargs):
            try:
                key = (_freeze(args), _freeze(kwargs))
                entry = _cache.get(key)
                if entry is not None and time.monotonic() < entry[1]:
                    if jplay.is_debug():
                        path = args[0].get("path", "") if args else ""
                        jplay.log.info(f"[debug] {fn.__name__}({path!r}) [cached] -> {entry[0]!r}")
                    return entry[0]
                result = fn(*args, **kwargs)
                _cache[key] = (result, time.monotonic() + seconds)
                if jplay.is_debug():
                    path = args[0].get("path", "") if args else ""
                    jplay.log.info(f"[debug] {fn.__name__}({path!r}) -> {result!r}")
                return result
            except Exception:
                traceback.print_exc()
                raise

        return wrapper
    return decorator


def _dbg(msg):
    """Log a step message when Settings > Python Debug is enabled."""
    if jplay.is_debug():
        jplay.log.info("[debug]   " + msg)


def _group(m, name):
    """Value of named group `name` if the template defines it and it matched,
    else None (a non-participating optional group is None in groupdict)."""
    return m.groupdict().get(name) if m else None


def _ver_number(ver_token):
    """The integer buried in a version token ('v003' -> 3, 'tk01' -> 1); 0 if none."""
    m = re.search(r"\d+", ver_token or "")
    return int(m.group()) if m else 0


def _ver_keys():
    """Picker keys declaring `sort = version`, in declaration order — the groups
    that hold a version-like numeric token rather than an identity. The shipped
    config gives ["version", "take"]; a site that spells its iterations some third
    way adds one [picker:*] section and every helper below follows.

    Nothing in this module names "version" directly: a version-like group is
    excluded from a clip's identity, indexes the "latest" of an asset, and is
    re-selected by number rather than by string — all of which `take` needs too."""
    return [p["key"] for p in _PICKERS if p["sort"] == "version"]


def _ver_token(m):
    """The version-like token a match carries: the value of the first _ver_keys()
    group that participated, or None. The version and take groups are mutually
    exclusive by convention, so at most one is ever present:
        _ver_token(<asset='beauty' version='v001'>) -> "v001"
        _ver_token(<asset='main' take='tk01'>)      -> "tk01"
        _ver_token(<asset='plate' frame='0007'>)    -> None
    """
    for key in _ver_keys():
        val = _group(m, key)
        if val is not None:
            return val
    return None


# Groups that vary within one clip (or aren't part of its identity), so they are
# excluded when deriving the grouping key below. Every version-like group counts:
# a take run has to collapse to one asset identity exactly as a version run does.
_NON_IDENTITY = {"frame", "extension"} | set(_ver_keys())


def _identity(em, name):
    """The grouping identity of a media file: stable across its frames and
    versions, and unique per distinct clip in a directory. It is the file match's
    identity groups joined — an explicit `asset`, or a {sequence}_{shot}_{department}
    triple, whatever the matched [file:*] rule captured minus version/frame/extension.
    Falls back to the extension-stripped stem when the file matched no convention.
    Because the frame group is excluded, an image sequence collapses to one entry.

    `em` is a [file:*] match (or None), `name` the basename it came from:
        _identity(_match_file("beauty_v001.mov")[0],  "beauty_v001.mov")   -> "beauty"
        _identity(_match_file("beauty_v002.mov")[0],  "beauty_v002.mov")   -> "beauty"
        _identity(_match_file("plate.0007.exr")[0],   "plate.0007.exr")    -> "plate"
        _identity(_match_file("unh0400_0010_lighting.0994.exr")[0], ...)
                                                    -> "unh0400_0010_lighting"
        _identity(None, "whatever.mov")                                   -> "whatever"
    """
    if em is not None:
        parts = [v for k, v in em.groupdict().items()
                 if k not in _NON_IDENTITY and v is not None]
        if parts:
            return "_".join(parts)
    return os.path.splitext(name)[0]


# ─────────────────── matching ───────────────────

def _norm(path):
    """Forward-slash form of a path, so the regexes see one separator on either
    platform: r"D:\\show\\house\\school3" -> "D:/show/house/school3"."""
    return path.replace("\\", "/")


def _is_media(name):
    """Whether a basename carries a media extension (_MEDIA_EXTS):
        "beauty_v001.mov" -> True, "plate.0007.exr" -> True, "notes.txt" -> False."""
    return os.path.splitext(name)[1].lower() in _MEDIA_EXTS


def _match_file(basename):
    """First [file:*] template that matches `basename`. Returns (match, name),
    or (None, None) if none matched. Takes a bare basename, not a path, and does
    not check the extension is media — callers gate with _is_media first, which is
    what lets [file:plain] stay a catch-all:
        "beauty_v001.mov"  -> (<asset='beauty' version='v001' extension='mov'>,
                               'versioned')
        "beauty_v003.0001.exr"
                           -> (<asset='beauty' version='v003' frame='0001'
                                extension='exr'>, 'versioned_sequence')
        "unh0400_0010_lighting.0994.exr"
                           -> (<sequence='unh0400' shot='0010'
                                department='lighting' frame='0994'
                                extension='exr'>, 'seq_shot_dept')
        "plate.0007.exr"   -> (<asset='plate' frame='0007' extension='exr'>,
                               'exr_sequence')
        "notes.txt"        -> (<asset='notes' extension='txt'>, 'plain')
        "beauty"           -> (None, None)   (no extension: matches nothing)
    """
    for name, rx in _FILES:
        m = rx.match(basename)
        if m:
            return m, name
    return None, None


def _project_doc(directory):
    """The project document governing `directory`: the nearest ancestor (itself
    included) holding "{dir}/{basename(dir)}" with a project extension — ".jpproj"
    first, ".otio" only if no .jpproj sits beside it. Returns the _norm'd path of
    the document, or None when no ancestor publishes either.

    The one place in this module that consults the disk rather than the path, and
    the only thing that can say where a project starts: a [dir:*] template match
    cannot (see _match_dir).

        "/show/house/school3/comp"  -> "/show/show.jpproj"   (if that file exists)
        "/nowhere/at/all"           -> None
    """
    walked = directory
    while walked:
        name = os.path.basename(walked)
        if name:
            for ext in (".jpproj", ".otio"):
                candidate = f"{walked}/{name}{ext}"
                if os.path.isfile(candidate):
                    return candidate
        parent = walked.rpartition("/")[0]
        if parent == walked:
            break
        walked = parent
    return None


def _project_root(directory):
    """The directory `_project_doc` found the project document in, or None."""
    doc = _project_doc(directory)
    return doc.rpartition("/")[0] if doc else None


def _match_dir(dirpath_norm):
    """First [dir:*] template matching an absolute directory path. Returns
    (match, template) or (None, None). Spans in the match index into
    `dirpath_norm`.

    Takes a _norm'd directory with no filename on the end. Leading directories are
    absorbed by the tail's `(?:.*/)?`, so the deepest components decide which
    template wins — but the slide stops at the project root, which _project_root
    reads off the disk. Without that floor a layout shallower than the widest
    template is read one level too high, silently renaming everything:

        "<root>/house/school3/comp"  -> sequence='house' shot='school3'
                                        department='comp'      [seq_shot_dept]
        "<root>/house/school3"       -> sequence='house' shot='school3'
                                                               [seq_shot]
            — with no root to stop it, seq_shot_dept would win this one instead,
              calling <root> the sequence and 'school3' a department.
        "<root>"                     -> (None, None)
            — the media sits in the project root: there are no layout levels.
        ""                           -> (None, None)

    Media under no project at all keeps the unfloored slide; there is nothing
    better to go on. Because every template is a catch-all over folder *names*, a
    match here says only how many levels are present, not that they mean what they
    are called; that is why _eff_kind prefers a filename group over this.
    """
    root = _project_root(dirpath_norm)
    pos = 0 if root is None else len(root) + 1
    if pos >= len(dirpath_norm) and dirpath_norm:
        return None, None  # the media sits in the project root itself
    for tmpl in _DIRS:
        m = tmpl.tail.match(dirpath_norm, pos)
        if m:
            return m, tmpl
    return None, None


def _match_dir_rel(rel):
    """First [dir:*] template whose anchored form matches a root-relative
    directory path. Returns (sequence, shot, department) or None.

    Unlike _match_dir this takes a path already made relative to the chosen
    discovery root, forward-slashed, with no leading "./" or drive — os.path.relpath
    output. Anchored at both ends, so the component *count* has to match a template
    exactly; a level the template omits comes back None:
        "house/school3/comp" -> ("house", "school3", "comp")   [dir:seq_shot_dept]
        "house/school3"      -> ("house", "school3", None)     [dir:seq_shot]
        "housebad"           -> (None, "housebad", None)       [dir:shot]
        "house/school3/comp/aov" -> None   (four levels: no template; skipped)
        "/show/house/school3/comp" -> None (absolute: pass a root-relative path)
    """
    for tmpl in _DIRS:
        m = tmpl.anchored.match(rel)
        if m:
            g = m.groupdict()
            return g.get("sequence"), g.get("shot"), g.get("department")
    return None


class _PathInfo:
    """Decomposition of a media path: fm/fname is the [file:*] match on the
    basename; dm/dtmpl is the [dir:*] match on the directory; `directory`
    is the normalised directory string the spans index into."""
    __slots__ = ("directory", "base", "fm", "fname", "dm", "dtmpl")


def _analyze(path):
    """Decompose one media file path into the _PathInfo the helpers below all take
    as their `a` argument. Takes a path to a *file* (a directory would be split at
    its last component and matched as a basename), absolute or relative, either
    separator. Never returns None: an unrecognised path yields a _PathInfo whose
    fm/dm are None.

        _analyze("/show/house/school3/comp/beauty_v001.mov")
            .directory = "/show/house/school3/comp"
            .base      = "beauty_v001.mov"
            .fm/.fname = <asset='beauty' version='v001' extension='mov'>, 'versioned'
            .dm/.dtmpl = <sequence='house' shot='school3' department='comp'>,
                         <_DirTemplate 'seq_shot_dept'>
        _analyze("beauty_v001.mov")        # no directory part
            .directory = "";  .base = "beauty_v001.mov";  .dm = None
    """
    norm = _norm(path)
    if "/" in norm:
        directory, base = norm.rsplit("/", 1)
    else:
        directory, base = "", norm
    directory = directory.rstrip("/")
    info = _PathInfo()
    info.directory = directory
    info.base = base
    info.fm, info.fname = _match_file(base)
    info.dm, info.dtmpl = _match_dir(directory)
    return info


def _index_dir(directory):
    """Map asset -> {version_number: filename} for media files in `directory`.
    Files without a version group index at 0, so versionless layouts still
    resolve. An EXR frame run collapses to its first frame (matching MediaScan
    and discovery). Asset falls back to the filename stem.

    `directory` is a path with no filename on it ("" means the cwd); the values are
    bare basenames, not paths. Unreadable directory -> {}. Throughout the picker
    helpers below the worked example is a directory holding

        beauty_v001.mov  beauty_v002.mov  hero_v001.mov
        plate.0007.exr   plate.0008.exr   notes.txt

    for which _index_dir returns
        {"beauty": {1: "beauty_v001.mov", 2: "beauty_v002.mov"},
         "hero":   {1: "hero_v001.mov"},
         "plate":  {0: "plate.0007.exr"}}
    The frame run keys at 0 under its lowest frame, and notes.txt is dropped as
    non-media. Note a versioned EXR run (beauty_v003.0001.exr) also keys at 0 —
    the frame branch is taken first, so only one version of it survives here."""
    try:
        entries = os.listdir(directory or ".")
    except OSError:
        return {}
    index = {}
    exr_min = {}  # asset -> lowest frame number seen, to keep the first frame
    for entry in entries:
        if not _is_media(entry) or not os.path.isfile(os.path.join(directory or ".", entry)):
            continue
        em, _ = _match_file(entry)
        if em is None:
            continue
        asset = _identity(em, entry)
        frame = _group(em, "frame")
        if frame is not None:
            num = int(frame)
            if asset not in exr_min or num < exr_min[asset]:
                exr_min[asset] = num
                index.setdefault(asset, {})[0] = entry
            continue
        token  = _ver_token(em)
        number = _ver_number(token) if token else 0
        index.setdefault(asset, {})[number] = entry
    return index


# ─────────────────── directory discovery ───────────────────

def _natural_key(s):
    """Sort key so digit runs order numerically ('shot2' < 'shot10')."""
    return [int(t) if t.isdigit() else t.lower() for t in re.split(r"(\d+)", s or "")]


def _collapse_media(directory, files):
    """Group the media in `directory` into asset -> {version_number: (path, token)}.
    A [file:*] match with a 'frame' group collapses the whole run to its first
    frame (versionless); a 'version' group indexes by version number; otherwise
    the file indexes at version 0.

    `files` is a list of basenames in `directory` (the os.walk triple's third item);
    paths in the result are os.path.join'd back onto `directory`, and `token` is the
    version as written, "" when the file carries none. The _index_dir of discovery,
    differing in that it is handed its listing and returns full paths. Over the same
    six files as _index_dir above:
        _collapse_media("/show/house/school3/comp",
                        ["beauty_v001.mov", "beauty_v002.mov", "hero_v001.mov",
                         "plate.0007.exr", "plate.0008.exr", "notes.txt"])
        -> {"beauty": {1: ("/show/house/school3/comp/beauty_v001.mov", "v001"),
                       2: ("/show/house/school3/comp/beauty_v002.mov", "v002")},
            "hero":   {1: ("/show/house/school3/comp/hero_v001.mov",   "v001")},
            "plate":  {0: ("/show/house/school3/comp/plate.0007.exr",  "")}}
    """
    assets = {}
    exr_seqs = {}  # asset -> (min_frame_number, path)
    for name in files:
        full = os.path.join(directory, name)
        if not _is_media(name) or not os.path.isfile(full):
            continue
        em, _ = _match_file(name)
        if em is None:
            continue
        asset = _identity(em, name)
        frame = _group(em, "frame")
        if frame is not None:
            num = int(frame)
            cur = exr_seqs.get(asset)
            if cur is None or num < cur[0]:
                exr_seqs[asset] = (num, full)
            continue
        token  = _ver_token(em)
        number = _ver_number(token) if token else 0
        assets.setdefault(asset, {})[number] = (full, token or "")
    for asset, (_num, path) in exr_seqs.items():
        assets.setdefault(asset, {}).setdefault(0, (path, ""))
    return assets


def _pick(candidates, prefs):
    """Highest-ranked candidate present in `prefs` (case-insensitive), else the
    first in natural-alphabetical order."""
    cand = list(candidates)
    for p in prefs:
        for c in cand:
            if c.lower() == p.lower():
                return c
    return sorted(cand, key=_natural_key)[0]


def inspect_directory(root, progress=None):
    """Traverse `root` and discover its sequences/shots/media for CREATE FROM
    DIRECTORY. Every directory that directly contains media is matched (by its
    path relative to `root`) against the [dir:*] templates; directories that
    match none are skipped. Returns one entry per shot, ordered by sequence then
    shot:
        {"sequence", "shot", "department", "asset", "version", "path"}
    choosing the default department/asset (per the [preferences] lists, else
    alphabetical) and the latest version. Empty strings denote levels the layout
    omits (e.g. no sequence or department).

    `progress` is the host's jplay.Progress when the walk runs off the main
    thread: the directory being visited is reported through it, and the walk
    bails out (returning []) once the user cancels. The total is unknown until
    the walk ends, so the fraction stays indeterminate.

    `root` is a directory the user chose, absolute or relative (it is abspath'd);
    the depth below it is what the [dir:*] templates are matched against, so the
    same tree picked one level up or down discovers different shots. For

        /show/house/school3/comp/beauty_v001.mov
        /show/house/school3/comp/beauty_v002.mov
        /show/house/school4/comp/beauty_v001.mov

    inspect_directory("/show") -> [
        {"sequence": "house", "shot": "school3", "department": "comp",
         "asset": "beauty", "version": "v002",
         "path": "/show/house/school3/comp/beauty_v002.mov"},
        {"sequence": "house", "shot": "school4", ... "version": "v001", ...},
    ]
    — latest version per shot, one entry per shot, sorted by sequence then shot.
    inspect_directory("/show/house") instead reads school3/comp as [dir:seq_shot],
    giving sequence="school3", shot="comp". [] when the tree holds no media in any
    directory the templates recognise."""
    root = os.path.abspath(root)
    # (sequence, shot) -> (department, {asset: {version_number: (path, token)}}).
    # First media-bearing directory found for a shot wins; the shot is then
    # considered satisfied and its other departments/assets are not scanned.
    shots = {}
    for dirpath, subdirs, files in os.walk(root):
        # Deterministic descent so "first-found wins" is reproducible, not left to
        # the filesystem's scandir order.
        if dirpath.startswith("."):
            continue
        subdirs.sort(key=_natural_key)
        rel = os.path.relpath(dirpath, root).replace("\\", "/")
        if progress is not None:
            if progress.cancelled():
                _dbg(f"inspect_directory({root!r}) cancelled at {rel!r}")
                return []
            progress.update(-1.0, f"Scanning {rel}")
        if not files:
            continue
        if rel == ".":
            continue  # loose files directly under the root: not a shot
        matched = _match_dir_rel(rel)
        if matched is None:
            _dbg(f"inspect_directory: no [dir:*] template for {rel!r}; skipped")
            continue
        seq, shot, dept = matched
        key = (seq or "", shot or "")
        if key in shots:
            # A video or exr sequence was already found for this shot in an
            # earlier directory; no need to look for more assets or departments.
            continue
        assets = _collapse_media(dirpath, files)
        if not assets:
            continue  # matched the layout but holds no recognised media
        shots[key] = (dept or "", assets)

    result = []
    for key in sorted(shots, key=lambda k: (_natural_key(k[0]), _natural_key(k[1]))):
        seq, shot = key
        dept, by_asset = shots[key]
        asset    = _pick(by_asset.keys(), _ASSET_PREFS)
        versions = by_asset[asset]
        number   = max(versions)
        path, token = versions[number]
        result.append({
            "sequence":   seq,
            "shot":       shot,
            "department": dept,
            "asset":      asset,
            "version":    token,
            "path":       os.path.normpath(path),
        })
    _dbg(f"inspect_directory({root!r}) -> {len(result)} shot(s)")
    return result


def _seq_shot_dept(path):
    """Resolve (sequence, shot, department) for one media path. Filename groups
    win over the directory match (the filename is the specific, intentional
    signal — see _eff_kind); any level the layout omits is ''.

    Takes a full media file path:
        "/show/house/school3/comp/beauty_v001.mov"  -> ("house", "school3", "comp")
                                    (all three from the folders; the basename has none)
        "/show/house/unh0400_0010/lighting/unh0400_0010_lighting.0994.exr"
                                    -> ("unh0400", "0010", "lighting")
                                    (the basename overrides the folders' "house" /
                                     "unh0400_0010")
        "/show/beauty_v001.mov"     -> ("", "show", "")
                                    ([dir:shot] matches any single folder, so a
                                     lone parent directory reads as the shot)
        "beauty_v001.mov"           -> ("", "", "")   (no directory to match)
    """
    a = _analyze(path)
    def pick(key):
        v = _group(a.fm, key)
        if v is None:
            v = _group(a.dm, key)
        return v or ""
    return pick("sequence"), pick("shot"), pick("department")


def get_path_context(media_info):
    """The structural (sequence, shot, department) a media path resolves to, as
    {"sequence", "shot", "department"}. get_path_values reports the same groups under
    their own names; this is the fixed-shape triple, with "" for the levels the layout
    omits. Used by "Show in Sequence" to name/find the sequence.

    `media_info` is the host's dict for one media; only its "path" is read (a missing
    or empty one yields all-"" rather than an error):
        {"path": "/show/house/school3/comp/beauty_v001.mov"}
            -> {"sequence": "house", "shot": "school3", "department": "comp"}
        {"path": "/show/house/unh0400_0010/lighting/unh0400_0010_lighting.0994.exr"}
            -> {"sequence": "unh0400", "shot": "0010", "department": "lighting"}
    """
    seq, shot, dept = _seq_shot_dept(media_info.get("path", ""))
    return {"sequence": seq, "shot": shot, "department": dept}


def get_project_path_from_media(media_info):
    """Derive the project file path from a media file's path.

    A project is a directory publishing a document named after itself, so this
    walks up from the media's own directory and returns the first ancestor that
    holds "{dir}/{basename(dir)}" with either project extension — ".jpproj" first,
    ".otio" only if no .jpproj sits beside it. Unlike the other callbacks this one
    answers from disk, and returns "" when no ancestor publishes either document
    (the host then has nothing to offer for that media).

    Accepts either the host's media dict or a bare path string, and the returned
    path keeps the input's separators — a backslash path answers in backslashes:

        "/show/house/school3/comp/beauty_v001.mov"  ->  "/show/show.jpproj"
        "/loose/beauty_v001.mov"                    ->  "/loose/loose.jpproj"

    The nearest publishing ancestor wins, so a shot folder carrying its own
    document beats the show above it. An empty path returns "" without touching
    the disk; a bare filename is resolved against the working directory.

    Deliberately *not* derived from the [dir:*] match, which is what this used to
    do (root = everything preceding the earliest matched group). That is backwards:
    the templates are catch-alls over folder names, so on a layout with fewer levels
    than the widest one they slide up and swallow the project folder itself. The
    dependency now runs the other way — _match_dir floors its slide at the root
    found here.
    """
    path = media_info.get("path") if isinstance(media_info, dict) else media_info
    if not path:
        return ""

    norm = _norm(path)
    directory = norm.rsplit("/", 1)[0].rstrip("/") if "/" in norm else ""
    doc = _project_doc(directory or _norm(os.getcwd()))
    if doc is None:
        _dbg(f"get_project_path_from_media({path!r}): no project file at or above "
             f"{directory!r}")
        return ""
    # Maintain backslashes if the original input path used them
    if "\\" in path:
        doc = doc.replace("/", "\\")
    _dbg(f"get_project_path_from_media({path!r}) -> {doc!r}")
    return doc


# ─────────────────── pickers ───────────────────
#
# Every metadata-bar dropdown is declared by a [picker:*] section (see
# naming_convention.conf) and served by the four generic callbacks below — there
# is no per-key code. A picker's `key` names a regex group; its `kind` selects how
# options are gathered:
#   dir  — the value is a directory component; options are the sibling directories
#          at that level (holding higher-level dirs fixed via the captured path).
#   file — the value is captured from the filename; options are the distinct values
#          of that group among sibling media files, holding every earlier-declared
#          file picker fixed at the current file's value. So `asset` (declared
#          first) lists all assets, and `version` (declared after it) lists the
#          versions of the current asset — the old get_assets/get_versions split,
#          now falling out of section order.


def _eff_kind(a, p):
    """A picker's effective kind for THIS path. Explicit 'dir'/'file' pass through;
    'auto' prefers file — a captured filename group is the specific, intentional
    signal, whereas the [dir:*] templates are catch-alls (`shot` matches any single
    folder, `seq_shot` any two), so they'd otherwise claim the group on almost any
    path. Falls back to dir when the filename carries no such group.

    Takes no path: `a` is a _PathInfo from _analyze(path) and `p` is one [picker:*]
    dict out of _PICKERS ({"key": "department", "kind": "auto", "label": ...}).
    Returns the string "dir" or "file" — never None. With the shipped config's
    `department` picker (kind=auto):
        _eff_kind(_analyze("/show/house/school3/comp/beauty_v001.mov"), p)
            -> "dir"    ([file:versioned] captured no department; the folder has it)
        _eff_kind(_analyze(".../unh0400_0010/lighting/unh0400_0010_lighting.0994.exr"), p)
            -> "file"   ([file:seq_shot_dept] captured department="lighting")
    A picker declaring kind=file or kind=dir returns that verbatim for every path,
    so only kind=auto ever consults `a`:
        _eff_kind(a, {"key": "version", "kind": "file", ...}) -> "file"   (always)
    """
    kind = p["kind"]
    if kind != "auto":
        return kind
    return "file" if _group(a.fm, p["key"]) is not None else "dir"


def _file_keys():
    """Picker keys that read from the filename, in declaration order. 'auto' keys
    are included: when one resolves to dir for a given path its filename group is
    simply absent, so holding it fixed (below) is a no-op there."""
    return [p["key"] for p in _PICKERS if p["kind"] in ("file", "auto")]


def _dir_picker_options(a, key):
    """(current_index, options) for a dir-kind picker: the values `key` takes across
    the sibling directories at its level. (-1, []) if the layout has no such group
    here.

    The group need not be a whole path component: in a flat layout one folder
    carries several groups (AWK__010__Compositing), so the options are gathered a
    *component* at a time — take the component the capture sits in, list that
    component's parent, splice each sibling name in whole, re-match the [dir:*]
    tail, and keep the group's value when every other directory group still agrees
    with the current path. In a one-group-per-component layout that reduces to
    listing the sibling directories, which is what it used to do; in a flat one it
    is the difference between a working Department picker and listing a directory
    that does not exist (".../AWK__010__").

    `a` is a _PathInfo, `key` a group name ("department", "shot", "sequence").
    Options are natural-sorted by the sibling folder they came from; the index
    points at the path's current value, or -1 if it is not among them. For a =
    _analyze("/show/house/school3/comp/beauty_v001.mov"), where "/show/house/school3"
    holds anim/, comp/ and lighting/:
        _dir_picker_options(a, "department") -> (1, ["anim", "comp", "lighting"])
        _dir_picker_options(a, "shot")       -> (0, ["school3", "school4"])
        _dir_picker_options(a, "asset")      -> (-1, [])   (no such directory group)
    and for a = _analyze("/out/AWK__010__Compositing/main_v001.0001.exr") where /out
    holds AWK__010__Animation, AWK__010__Compositing and AWK__020__Compositing:
        _dir_picker_options(a, "department") -> (1, ["Animation", "Compositing"])
            — AWK__020__Compositing is skipped: its `shot` disagrees, so it is a
              different shot's folder rather than this shot's other department.
    An unreadable parent directory also gives (-1, [])."""
    current_val = _group(a.dm, key)
    if current_val is None:
        return -1, []
    directory = a.directory
    comp_start = directory.rfind("/", 0, a.dm.start(key)) + 1
    next_slash = directory.find("/", a.dm.end(key))
    comp_end   = len(directory) if next_slash < 0 else next_slash
    parent = directory[:comp_start].rstrip("/")
    tail   = directory[comp_end:]  # the components below, kept fixed
    try:
        entries = os.listdir(parent or ".")
    except OSError as e:
        _dbg(f"picker {key!r}: cannot list {parent!r}: {e}")
        return -1, []
    # Every other directory group is held at the current path's value, so a
    # sibling folder that names a different shot (or sequence) is not mistaken for
    # another option of this one.
    held = {g: v for g, v in a.dm.groupdict().items() if v is not None and g != key}
    options = []
    for entry in sorted(entries, key=_natural_key):
        if not os.path.isdir(os.path.join(parent or ".", entry)):
            continue
        cand = (parent + "/" if parent else "") + entry + tail
        m, _tmpl = _match_dir(cand)
        val = _group(m, key)
        if val is None or any(_group(m, g) != v for g, v in held.items()):
            continue
        if val not in options:
            options.append(val)
    if not options:
        return -1, []
    idx = options.index(current_val) if current_val in options else -1
    return idx, options


def _file_picker_options(a, key, sort):
    """(current_index, options) for a file-kind picker: distinct values of group
    `key` among sibling media, holding every earlier-declared file picker fixed.
    The current file need not carry the group itself — an unversioned file (e.g.
    asset.0001.exr) still lists the versions of its versioned siblings. Such a
    versionless file is the asset's hero copy, which by convention is its latest
    version, so the newest option is reported as current.

    `a` is a _PathInfo, `key` a group name, `sort` the picker's "version" (numeric,
    latest first, capped at 20) or anything else (natural-alphabetical). Options are
    the captured strings themselves, version tokens included as written. For a =
    _analyze("/show/house/school3/comp/beauty_v001.mov") over _index_dir's six files:
        _file_picker_options(a, "asset",   "natural")
            -> (0, ["beauty", "hero", "plate"])
            — every asset in the directory, image sequences included (plate's two
              frames contribute one option); notes.txt is not media.
        _file_picker_options(a, "version", "version") -> (1, ["v002", "v001"])
            — only beauty's versions: `asset` is declared earlier, so it is held
              at "beauty" and hero/plate are not consulted.
        _file_picker_options(a, "department", "natural") -> (-1, [])
            (no sibling filename captures one under [file:versioned])
    (-1, []) too when the basename matched no [file:*] rule or the directory cannot
    be listed."""
    if a.fm is None:
        return -1, []
    keys   = _file_keys()
    higher = keys[:keys.index(key)] if key in keys else []
    fixed  = {g: _group(a.fm, g) for g in higher}
    current_val = _group(a.fm, key)
    try:
        entries = os.listdir(a.directory or ".")
    except OSError as e:
        _dbg(f"picker {key!r}: cannot list {a.directory!r}: {e}")
        return -1, []

    found = set()
    for entry in entries:
        if not _is_media(entry):
            continue
        em, _ = _match_file(entry)
        if em is None or any(_group(em, g) != fixed[g] for g in higher):
            continue
        val = _group(em, key)
        if val is not None:
            found.add(val)
    if not found:
        return -1, []

    if sort == "version":
        options = sorted(found, key=_ver_number, reverse=True)[:20]
    else:
        options = sorted(found, key=_natural_key)
    idx = options.index(current_val) if current_val in options else -1
    if idx < 0 and current_val is None and sort == "version":
        idx = 0  # versionless hero copy: current = the latest (options sort newest-first)
    return idx, options


def _picker_value(a, p):
    """The path's current value for picker `p` (never option-gathering). Reads the
    filename group, falling back to the directory group when the filename doesn't
    carry it — folder-based layouts keep sequence/shot/department in the path, so
    get_path_values still reports e.g. the sequence for a {seq}/{shot}/{dept} tree
    even though no Sequence dropdown is shown there (this only affects the reported
    value, not which pickers describe_pickers exposes).

    `a` is a _PathInfo, `p` one [picker:*] dict. Always a string, "" when the path
    carries nothing for that picker. For
    a = _analyze("/show/house/school3/comp/beauty_v001.mov"):
        department -> "comp"     (from the folder; the basename has none)
        asset      -> "beauty"
        version    -> "v001"
    and for a = _analyze(".../unh0400_0010/lighting/unh0400_0010_lighting.0994.exr"):
        department -> "lighting" (from the basename)
        asset      -> ""         ([file:seq_shot_dept] has no asset group, and the
                                  stem fallback applies only when nothing matched)
        version    -> ""
    """
    if _eff_kind(a, p) == "dir":
        return _group(a.dm, p["key"]) or ""
    val = _group(a.fm, p["key"])
    if val is None:
        val = _group(a.dm, p["key"])  # folder-based layout: the value lives in the path
    if val is None and p["key"] == "asset" and a.fm is None:
        val = os.path.splitext(a.base)[0]  # bare stem when no file convention matched
    return val or ""


# ─────────────────── callbacks ───────────────────

def list_pickers():
    """The configured pickers, in display order: [{key, label, kind, multi_commit}].
    Static (independent of any path); the host builds the metadata-bar controls from
    it.

    `multi_commit` is true for at most one picker — the [preferences]
    multi_select_picker. With several clips selected the host stops the picker menu
    there: that picker commits for the whole selection and the ones after it are
    hidden, each clip resolving the pick and then taking the top option of the last
    picker on its own. Nothing flagged (the key unset, or naming a picker that is not
    configured) leaves the host to fall back to the picker before the last."""
    flagged = _MULTI_PICKER.lower()
    return [{"key": p["key"], "label": p["label"], "kind": p["kind"],
             "multi_commit": bool(flagged) and p["key"].lower() == flagged}
            for p in _PICKERS]


def _order_by_prefs(options, current_idx, prefs):
    """Reorder `options` so entries named in `prefs` come first, in prefs order
    (case-insensitive), followed by the rest in their original order. Returns
    (new_current_index, reordered_options)."""
    current_val = options[current_idx] if 0 <= current_idx < len(options) else None
    remaining = list(options)
    ordered = []
    for p in prefs:
        for o in list(remaining):
            if o.lower() == p.lower():
                ordered.append(o)
                remaining.remove(o)
    ordered.extend(remaining)
    idx = ordered.index(current_val) if current_val in ordered else -1
    return idx, ordered


@timed_cache(seconds=2)
def describe_pickers(media_info):
    """Resolve every picker against media_info['path'], returning only those that
    apply (their group matched and at least one option exists):
        [{key, current_index, options}]
    The host uses this to show/hide each control and fill its option list. A picker
    with no options is omitted entirely, so the list is usually shorter than
    list_pickers(); an empty or missing path returns [].

        {"path": "/show/house/school3/comp/beauty_v001.mov"} -> [
            {"key": "department", "current_index": 2,
             "options": ["lighting", "anim", "comp"]},
            # natural order was anim/comp/lighting; "lighting" leads because
            # [preferences] departments names it exactly (the match is
            # case-insensitive but whole-string, so "Compositing" misses "comp")
            {"key": "asset",   "current_index": 0,
             "options": ["beauty", "hero", "plate"]},
            {"key": "version", "current_index": 1, "options": ["v002", "v001"]},
        ]

    An option is a plain string here — it labels itself and draws in the view's own
    color. The host also accepts {"value", "label", "color", "badge",
    "badge_color"} in its place, and a
    decorate_pickers callback can annotate an option list afterwards; core does
    neither. See jplay_naming_convention.py for both.
    """
    path = media_info.get("path")
    if not path:
        return []
    a = _analyze(path)
    out = []
    for p in _PICKERS:
        if _eff_kind(a, p) == "dir":
            idx, opts = _dir_picker_options(a, p["key"])
        else:
            idx, opts = _file_picker_options(a, p["key"], p["sort"])
        # Department options follow the [preferences] departments ranking (listed
        # first, in that order); everything else keeps its natural-alpha order.
        if p["key"] == "department" and _DEPT_PREFS and opts:
            idx, opts = _order_by_prefs(opts, idx, _DEPT_PREFS)
        if opts:
            out.append({"key": p["key"], "current_index": idx, "options": opts})
    _dbg(f"describe_pickers({path!r}) -> {[o['key'] for o in out]}")
    return out


def get_path_values(media_info):
    """Every naming-convention value the path carries: {group: value}. The picker
    keys are always present, read via _picker_value (so a folder-based layout still
    reports its value and asset falls back to the bare stem); alongside them comes
    every other named group the directory and filename regexes matched — the sequence
    and shot the path sits under, the extension. The filename wins over the directory
    where both capture a group, being the more specific of the two.

    The frame group is left out: it indexes one file within a media rather than
    identifying it. Two media that agree on every value here are siblings under the
    same shot, which is what lets the Component Picker tell how deeply a target change
    reaches (see pickerDivergenceIndex).

    Reads media_info["path"] only. Every picker key is present even when empty; the
    other keys appear only if a regex captured them:
        {"path": "/show/house/school3/comp/beauty_v001.mov"} ->
            {"sequence": "house", "shot": "school3", "department": "comp",
             "asset": "beauty", "version": "v001", "extension": "mov"}
        {"path": ".../unh0400_0010/lighting/unh0400_0010_lighting.0994.exr"} ->
            {"sequence": "unh0400", "shot": "0010", "department": "lighting",
             "extension": "exr", "asset": "", "version": ""}
            — sequence/shot come from the basename, overriding the folders' "house"
              and "unh0400_0010"; no frame key despite the file carrying 0994.
    """
    a = _analyze(media_info.get("path", ""))
    out = {}
    for m in (a.dm, a.fm):
        if m is None:
            continue
        for key, val in m.groupdict().items():
            if key != "frame" and val is not None:
                out[key] = val
    for p in _PICKERS:
        out[p["key"]] = _picker_value(a, p)
    return out


def _pick_in_dir(directory, asset, want_version):
    """Pick a media file in `directory` for `asset` (falling back to the first
    asset present), at `want_version` if given and available, else the latest.
    Returns a path or None. Used after a dir picker rewrites the directory.

    `asset` is an identity string as _identity produces it, `want_version` an int
    version number (not a token) or None. The returned path is os.path.normpath'd,
    so it comes back in the platform's separators. Against _index_dir's directory
    (beauty at v001/v002, hero at v001, a plate.#### run):
        _pick_in_dir("/show/house/school4/comp", "beauty", None)
            -> "/show/house/school4/comp/beauty_v002.mov"    (latest)
        _pick_in_dir("/show/house/school4/comp", "beauty", 1)
            -> "/show/house/school4/comp/beauty_v001.mov"
        _pick_in_dir("/show/house/school4/comp", "nosuch", None)
            -> "/show/house/school4/comp/beauty_v002.mov"    (first asset instead)
        _pick_in_dir("/show/house/empty", "beauty", None) -> None
    """
    index = _index_dir(directory)  # asset -> {version_number: filename}
    if not index:
        return None
    target = asset if asset in index else sorted(index)[0]
    versions = index.get(target)
    if not versions:
        return None
    number = want_version if (want_version is not None and want_version in versions) else max(versions)
    return os.path.normpath(f"{directory}/{versions[number]}")


def _resolve_file(a, key, value):
    """Re-select a sibling media file after a file-kind picker changes. Mirrors the
    option lists: pin every earlier-declared file picker to the current file's
    value, set `key` to `value`, leave later pickers free, then take the requested
    version (for a version-like picker — `version` or `take`) or the latest.
    Returns a path or None. When a
    change leaves later groups free (e.g. changing sequence with several shots), the
    lowest natural-order match of the chosen version is picked as the representative.

    `a` is a _PathInfo, `key` a picker key and `value` the option string the user
    chose — exactly as it appeared in that picker's options, so a version arrives as
    its token ("v002"), not a number. The result is os.path.normpath'd and stays in
    a.directory; None when nothing there satisfies the request. For a =
    _analyze("/show/house/school3/comp/beauty_v001.mov") in that same directory:
        _resolve_file(a, "version", "v002") -> ".../comp/beauty_v002.mov"
        _resolve_file(a, "asset",   "hero") -> ".../comp/hero_v001.mov"
        _resolve_file(a, "asset",   "beauty") -> ".../comp/beauty_v001.mov"
            — the current file already satisfies it, so it stays put rather than
              jumping to the latest version.
        _resolve_file(a, "asset",   "nosuch") -> None
    """
    keys   = _file_keys()
    higher = keys[:keys.index(key)] if key in keys else []
    want   = {g: _group(a.fm, g) for g in higher if _group(a.fm, g) is not None}
    ver_keys = _ver_keys()
    if key not in ver_keys:
        want[key] = value  # a version-like key is matched by number below, not by token

    directory = a.directory
    try:
        entries = os.listdir(directory or ".")
    except OSError:
        return None

    matches = []  # (path, version_number, frame, entry)
    for entry in entries:
        if not _is_media(entry) or not os.path.isfile(os.path.join(directory or ".", entry)):
            continue
        em, _ = _match_file(entry)
        if em is None or any(_group(em, g) != want[g] for g in want):
            continue
        frame  = _group(em, "frame")
        token  = _ver_token(em)
        matches.append((os.path.normpath(f"{directory}/{entry}"),
                        _ver_number(token) if token else 0,
                        int(frame) if frame is not None else 0, entry))
    if not matches:
        return None

    # Stay on the current file if it still satisfies the new constraints — a no-op
    # or a change orthogonal to what the path already pins shouldn't jump elsewhere.
    cur = os.path.normpath(f"{directory}/{a.base}")
    if key not in ver_keys and any(mm[0] == cur for mm in matches):
        return cur

    if key in ver_keys:
        wanted = _ver_number(value)
        nums = {mm[1] for mm in matches}
        number = wanted if wanted in nums else max(nums)
    else:
        number = max(mm[1] for mm in matches)
    # Representative of the chosen version: natural-first asset, then its first frame.
    cands = sorted((mm for mm in matches if mm[1] == number),
                   key=lambda mm: (_natural_key(mm[3]), mm[2]))
    return cands[0][0]


def resolve_path(media_info, key, value):
    """Resolve the on-disk media path after the user sets picker `key` to `value`.
    A dir picker rewrites that directory component and re-selects the current asset
    within it; a file picker re-selects a sibling by its captured groups (asset
    picks the asset, version the version, sequence/shot/department their token).
    Returns the full path, or None.

    `key` must name a configured picker and `value` be one of the option strings
    describe_pickers offered for it; anything else returns None. Starting from
    {"path": "/show/house/school3/comp/beauty_v001.mov"}:
        ("version", "v002")    -> "/show/house/school3/comp/beauty_v002.mov"
        ("asset",   "hero")    -> "/show/house/school3/comp/hero_v001.mov"
        ("department", "lighting")  -> "/show/house/school3/lighting/<its latest>"
            — a dir picker here, so the folder is swapped and the current asset
              re-picked inside it (any asset there, if "beauty" is absent).
        ("shot", "school4")    -> None   (no [picker:shot] is configured)
    """
    _dbg(f"resolve_path: path={media_info.get('path')!r} {key}={value!r}")
    path = media_info.get("path")
    if not path:
        return None
    pdef = next((p for p in _PICKERS if p["key"] == key), None)
    if pdef is None:
        _dbg(f"resolve_path: no picker {key!r} -> None")
        return None

    a = _analyze(path)

    if _eff_kind(a, pdef) == "dir" and _group(a.dm, key) is not None:
        target_dir = (a.directory[:a.dm.start(key)] + value
                      + a.directory[a.dm.end(key):]).rstrip("/")
        current_asset = _identity(a.fm, a.base)
        resolved = _pick_in_dir(target_dir, current_asset, None)
    else:
        resolved = _resolve_file(a, key, value)

    _dbg(f"resolve_path: -> {resolved!r}")
    return resolved


def query_audio(media_info):
    """Called just before an EXR sequence is added to the timeline (interactive
    and command-line adds only). Return the audio to pair with it, or None to add
    no audio.

    `media_info` is a dict with at least "path" — the resolved concrete frame path
    of the EXR sequence being added.

    Return None to skip audio. To attach audio, return either:
        {"path": "/abs/path/to/track.wav", "offset": 0}
    or a tuple:
        ("/abs/path/to/track.wav", 0)
    where "offset" is the audio's source-in point in FRAMES (at the project fps).
    The host places the audio on a track just under the EXR clip, spanning the
    exact same timeline in/out range as the clip; `offset` trims into the audio
    file so it lines up.

    Stub: returns None (no audio). Replace the body with your production lookup —
    e.g. derive a matching .wav from the shot in `media_info["path"]`.
    """
    _dbg(f"query_audio: path={media_info.get('path')!r}")
    return None


def register_defaults():
    """Claim every host callback for the shipped implementation above.

    Called by jplay_init.py, and by nothing else — deliberately not done at import
    time. Registration is last-wins and global, so an import that registered would
    silently replace a site's overrides whenever it happened to run later, and
    plenty of things import this module only for its helpers (jplay_discovery does,
    for the matching rules). Claiming callbacks is a decision the startup file
    makes once; importing this module is not.

    jplay_init.py calls this BEFORE importing jplay_naming_convention, so the
    site-owned file can replace any of these by registering the same name. The
    names are that override surface: keep them stable."""
    jplay.register_callback("list_pickers", list_pickers)
    jplay.register_callback("describe_pickers", describe_pickers)
    jplay.register_callback("get_path_values", get_path_values)
    jplay.register_callback("get_path_context", get_path_context)
    jplay.register_callback("resolve_path", resolve_path)
    jplay.register_callback("create_from_directory", inspect_directory)
    jplay.register_callback("get_project_path_from_media", get_project_path_from_media)
    jplay.register_callback("query_audio", query_audio)

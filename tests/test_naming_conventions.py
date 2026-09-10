"""Standalone mock tests for jplay_naming_core: the naming conventions it reads a project tree through.
"""

import os
import sys
import types
import tempfile
import shutil
from collections import namedtuple


# ─────────────── stub the host `jplay` module before import ───────────────
_jplay = types.ModuleType("jplay")
_jplay.register_callback = lambda *a, **k: None
_jplay.is_debug = lambda: False
_jplay.log = types.SimpleNamespace(info=lambda *a, **k: None)
sys.modules["jplay"] = _jplay

_HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(_HERE, os.pardir, "python"))
import jplay_naming_core as nc  # noqa: E402


# ─────────────────────────── tiny fs builder ───────────────────────────

def touch(path):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "wb") as f:
        f.write(b"\0")


def make_exr_seq(directory, stem, frames, sep="_"):
    """Write stem_0001.exr .. for the given frame numbers."""
    for n in frames:
        touch(os.path.join(directory, f"{stem}{sep}{n:04d}.exr"))


# ─────────────────────────── test harness ───────────────────────────

_failures = []


def check(name, cond, detail=""):
    status = "PASS" if cond else "FAIL"
    print(f"  [{status}] {name}" + (f"  -- {detail}" if detail and not cond else ""))
    if not cond:
        _failures.append(name)


def by_shot(results):
    return {(r["sequence"], r["shot"]): r for r in results}


# ─────────────────────────── tests ───────────────────────────

def test_single_shot_exr_sequence():
    print("test_single_shot_exr_sequence")
    root = tempfile.mkdtemp()
    try:
        comp = os.path.join(root, "The Grand Forest Feast", "The First Bite", "Compositing")
        make_exr_seq(comp, "main", range(1, 25))
        res = inspect(root)
        check("one shot discovered", len(res) == 1, f"got {len(res)}")
        r = res[0]
        check("sequence", r["sequence"] == "The Grand Forest Feast", r["sequence"])
        check("shot", r["shot"] == "The First Bite", r["shot"])
        check("department", r["department"] == "Compositing", r["department"])
        check("asset", r["asset"] == "main", r["asset"])
        check("path is first frame",
              os.path.basename(r["path"]) == "main_0001.exr", r["path"])
    finally:
        shutil.rmtree(root, ignore_errors=True)


def test_early_exit_first_department_wins():
    """Shot with media in two departments -> only the first (natural-sorted)
    department is scanned; the second is skipped entirely."""
    print("test_early_exit_first_department_wins")
    root = tempfile.mkdtemp()
    try:
        shot = os.path.join(root, "Seq", "Battle Stance")
        # Animation sorts before Compositing; both hold a valid exr sequence.
        make_exr_seq(os.path.join(shot, "Animation"), "main", range(1, 6))
        make_exr_seq(os.path.join(shot, "Compositing"), "main", range(1, 6))
        res = inspect(root)
        check("exactly one entry for the shot", len(res) == 1, f"got {len(res)}")
        r = res[0]
        check("first-found department wins (Animation, not preferred Compositing)",
              r["department"] == "Animation", r["department"])
        check("path lives under the winning department",
              "Animation" in r["path"] and "Compositing" not in r["path"], r["path"])
    finally:
        shutil.rmtree(root, ignore_errors=True)


def test_empty_first_department_falls_through():
    """A first-sorted department that matches the layout but holds NO media must
    not satisfy the shot; a later department with media should still win."""
    print("test_empty_first_department_falls_through")
    root = tempfile.mkdtemp()
    try:
        shot = os.path.join(root, "Seq", "Core Overdrive")
        # Animation has only a non-media file -> not satisfied.
        touch(os.path.join(shot, "Animation", "notes.txt"))
        make_exr_seq(os.path.join(shot, "Compositing"), "main", range(1, 6))
        res = inspect(root)
        check("one entry", len(res) == 1, f"got {len(res)}")
        check("department fell through to Compositing",
              res and res[0]["department"] == "Compositing",
              res[0]["department"] if res else "no result")
    finally:
        shutil.rmtree(root, ignore_errors=True)


def test_versioned_latest_and_asset_pref():
    """Within the winning directory, latest version and preferred asset win."""
    print("test_versioned_latest_and_asset_pref")
    root = tempfile.mkdtemp()
    try:
        comp = os.path.join(root, "Seq", "Shot01", "Compositing")
        # two assets, several versions; 'main' is the preferred asset.
        for v in ("v001", "v002", "v003"):
            touch(os.path.join(comp, f"main_{v}.mov"))
            touch(os.path.join(comp, f"bgprep_{v}.mov"))
        res = inspect(root)
        check("one shot", len(res) == 1, f"got {len(res)}")
        r = res[0]
        check("preferred asset 'main' chosen", r["asset"] == "main", r["asset"])
        check("latest version chosen", r["version"] == "v003", r["version"])
        check("path matches asset+version",
              os.path.basename(r["path"]) == "main_v003.mov", r["path"])
    finally:
        shutil.rmtree(root, ignore_errors=True)


def test_multiple_shots_ordering():
    print("test_multiple_shots_ordering")
    root = tempfile.mkdtemp()
    try:
        for shot in ("shot2", "shot10", "shot1"):
            make_exr_seq(os.path.join(root, "SeqA", shot, "Compositing"),
                         "main", range(1, 4))
        res = inspect(root)
        shots = [r["shot"] for r in res]
        check("three shots", len(res) == 3, f"got {len(res)}")
        check("natural ordering (shot1, shot2, shot10)",
              shots == ["shot1", "shot2", "shot10"], str(shots))
    finally:
        shutil.rmtree(root, ignore_errors=True)


def test_loose_files_at_root_ignored():
    print("test_loose_files_at_root_ignored")
    root = tempfile.mkdtemp()
    try:
        touch(os.path.join(root, "stray_0001.exr"))
        make_exr_seq(os.path.join(root, "Seq", "ShotX", "Compositing"),
                     "main", range(1, 4))
        res = inspect(root)
        check("only the real shot, root strays ignored", len(res) == 1, f"got {len(res)}")
    finally:
        shutil.rmtree(root, ignore_errors=True)


def inspect(root):
    r = nc.inspect_directory(root)
    print(f"    inspect_directory -> {len(r)} shot(s)")
    return r


# ─────────────────── take / flat / dept-in-name layouts ───────────────────

def test_take_tree_latest():
    """A take tag indexes an asset exactly as a version tag does: the latest take
    wins and the token is reported under the same "version" field."""
    print("test_take_tree_latest")
    root = tempfile.mkdtemp()
    try:
        comp = os.path.join(root, "AWK", "010", "Compositing")
        for t in ("tk01", "tk02", "tk03"):
            touch(os.path.join(comp, f"main_{t}.mov"))
        res = inspect(root)
        check("one shot", len(res) == 1, f"got {len(res)}")
        r = res[0]
        check("asset is the take-stripped stem", r["asset"] == "main", r["asset"])
        check("latest take chosen", r["version"] == "tk03", r["version"])
        check("path matches the latest take",
              os.path.basename(r["path"]) == "main_tk03.mov", r["path"])
    finally:
        shutil.rmtree(root, ignore_errors=True)


def test_take_exr_run_collapses():
    """main_tk01_0001.exr .. — the take must not read as a department (the old
    [file:seq_shot_dept] match made the FRAME the department), and the run has to
    collapse to one clip at its first frame."""
    print("test_take_exr_run_collapses")
    root = tempfile.mkdtemp()
    try:
        comp = os.path.join(root, "AWK", "010", "Compositing")
        make_exr_seq(comp, "main_tk01", range(1, 13))
        res = inspect(root)
        check("one shot", len(res) == 1, f"got {len(res)}")
        r = res[0]
        check("asset", r["asset"] == "main", r["asset"])
        check("department from the folder, not the frame number",
              r["department"] == "Compositing", r["department"])
        check("path is the first frame",
              os.path.basename(r["path"]) == "main_tk01_0001.exr", r["path"])
    finally:
        shutil.rmtree(root, ignore_errors=True)


def test_flat_folder_layout():
    """One folder carrying sequence, shot and department."""
    print("test_flat_folder_layout")
    root = tempfile.mkdtemp()
    try:
        make_exr_seq(os.path.join(root, "AWK__010__Compositing"), "main_v001",
                     range(1, 5), sep=".")
        res = inspect(root)
        check("one shot", len(res) == 1, f"got {len(res)}")
        r = res[0]
        check("sequence", r["sequence"] == "AWK", r["sequence"])
        check("shot", r["shot"] == "010", r["shot"])
        check("department", r["department"] == "Compositing", r["department"])
        check("asset", r["asset"] == "main", r["asset"])
    finally:
        shutil.rmtree(root, ignore_errors=True)


def test_dept_in_filename_not_absorbed():
    """main_Compositing_v001.mov keeps its department out of the asset, while a
    two-token asset that merely looks similar (bg_smoke_v001.mov) does not."""
    print("test_dept_in_filename_not_absorbed")
    m, rule = nc._match_file("main_Compositing_v001_0001.exr")
    g = m.groupdict() if m else {}
    check("dept peeled off the basename",
          g.get("asset") == "main" and g.get("department") == "Compositing"
          and g.get("version") == "v001", f"{rule}: {g}")
    m2, rule2 = nc._match_file("bg_smoke_v001.0001.exr")
    g2 = m2.groupdict() if m2 else {}
    check("a two-token asset is left alone",
          g2.get("asset") == "bg_smoke" and g2.get("department") is None,
          f"{rule2}: {g2}")
    m3, _ = nc._match_file("main_take01.0001.exr")
    g3 = m3.groupdict() if m3 else {}
    check("take tag captured, not read as seq/shot/dept",
          g3.get("asset") == "main" and g3.get("take") == "take01"
          and g3.get("frame") == "0001", str(g3))


# ─────────────────────────── picker tests ───────────────────────────

def picker_keys(path):
    return [d["key"] for d in nc.describe_pickers({"path": path})]


def options_for(path, key):
    for d in nc.describe_pickers({"path": path}):
        if d["key"] == key:
            return d["options"]
    return []


def test_pickers_version_tree():
    print("test_pickers_version_tree")
    root = tempfile.mkdtemp()
    try:
        shot = os.path.join(root, "AWK", "010")
        for dept in ("Compositing", "Lighting"):
            for v in ("v001", "v002"):
                touch(os.path.join(shot, dept, f"main_{v}.mov"))
        path = os.path.join(shot, "Compositing", "main_v001.mov")
        keys = picker_keys(path)
        check("department / asset / version, no take section",
              keys == ["department", "asset", "version"], str(keys))
        check("both departments offered",
              sorted(options_for(path, "department")) == ["Compositing", "Lighting"],
              str(options_for(path, "department")))
        check("versions latest-first",
              options_for(path, "version") == ["v002", "v001"],
              str(options_for(path, "version")))
    finally:
        shutil.rmtree(root, ignore_errors=True)


def test_pickers_take_tree():
    print("test_pickers_take_tree")
    root = tempfile.mkdtemp()
    try:
        shot = os.path.join(root, "AWK", "010")
        for dept in ("Compositing", "Lighting"):
            for t in ("tk01", "tk02"):
                make_exr_seq(os.path.join(shot, dept), f"main_{t}", range(1, 4))
        path = os.path.join(shot, "Compositing", "main_tk01_0001.exr")
        keys = picker_keys(path)
        check("department / asset / take, no empty version section",
              keys == ["department", "asset", "take"], str(keys))
        check("takes latest-first",
              options_for(path, "take") == ["tk02", "tk01"],
              str(options_for(path, "take")))
        check("asset is the take-stripped stem",
              options_for(path, "asset") == ["main"], str(options_for(path, "asset")))
        resolved = nc.resolve_path({"path": path}, "take", "tk02")
        check("switching take re-selects the sibling run's first frame",
              bool(resolved) and os.path.basename(resolved) == "main_tk02_0001.exr",
              str(resolved))
        resolved = nc.resolve_path({"path": path}, "department", "Lighting")
        check("switching department keeps the asset",
              bool(resolved) and "Lighting" in resolved
              and os.path.basename(resolved).startswith("main_"), str(resolved))
    finally:
        shutil.rmtree(root, ignore_errors=True)


def test_pickers_flat_folder():
    """The Department picker on a flat SEQ__SHOT__DEPT folder: options come from
    the sibling folders' department *component*, and folders belonging to another
    shot are not offered."""
    print("test_pickers_flat_folder")
    root = tempfile.mkdtemp()
    try:
        for folder in ("AWK__010__Animation", "AWK__010__Compositing",
                       "AWK__020__Compositing"):
            make_exr_seq(os.path.join(root, folder), "main_v001", range(1, 4), sep=".")
        path = os.path.join(root, "AWK__010__Compositing", "main_v001.0001.exr")
        keys = picker_keys(path)
        check("department / asset / version", keys == ["department", "asset", "version"],
              str(keys))
        opts = options_for(path, "department")
        check("this shot's departments only (AWK__020__* excluded)",
              sorted(opts) == ["Animation", "Compositing"], str(opts))
        resolved = nc.resolve_path({"path": path}, "department", "Animation")
        check("switching department rewrites the department component only",
              bool(resolved) and "AWK__010__Animation" in resolved, str(resolved))
    finally:
        shutil.rmtree(root, ignore_errors=True)


def test_pickers_dept_in_filename():
    print("test_pickers_dept_in_filename")
    root = tempfile.mkdtemp()
    try:
        shot = os.path.join(root, "AWK", "010")
        for dept in ("Compositing", "Lighting"):
            for v in ("v001", "v002"):
                touch(os.path.join(shot, f"main_{dept}_{v}.mov"))
        path = os.path.join(shot, "main_Compositing_v001.mov")
        keys = picker_keys(path)
        check("department / asset / version", keys == ["department", "asset", "version"],
              str(keys))
        opts = options_for(path, "department")
        check("departments read from the sibling filenames",
              sorted(opts) == ["Compositing", "Lighting"], str(opts))
        check("versions of this department only",
              options_for(path, "version") == ["v002", "v001"],
              str(options_for(path, "version")))
        resolved = nc.resolve_path({"path": path}, "department", "Lighting")
        check("switching department stays in the folder",
              bool(resolved) and os.path.basename(resolved).startswith("main_Lighting_"),
              str(resolved))
    finally:
        shutil.rmtree(root, ignore_errors=True)




# ─────────────────── project document lookup ───────────────────


_SEQ, _SHOT, _DEPT = "AWK", "010", "Compositing"


# --------------------- the layouts under test ---------------------
#
# The directory and filename shapes naming_convention.conf has to read, one
# entry per shape rather than per site. Each is built below with dest_dir and
# out_stem, so a layout is described once and both the tree and the expectation
# follow from the same spec.
#
#   dirs: 'flat' packs Seq and Shot into one folder name, 'nested' keeps them
#         apart
#   dept: 'drop' discards the Department level, 'dir' keeps it as a directory,
#         'name' folds it into the file name
#   tag:  how the version number is spelled, formatted with the version integer
#   sep:  separator between the file name stem and the frame number
Template = namedtuple("Template", "dirs dept tag sep")

TEMPLATES = {
    #                      dirs      dept    tag           sep
    "flat":       Template("flat",   "drop", "v{:03d}",    "."),
    "nested":     Template("nested", "drop", "v{:03d}",    "_"),
    "short-ver":  Template("flat",   "drop", "v{:02d}",    "."),
    "take":       Template("nested", "drop", "tk{:02d}",   "_"),
    "take-long":  Template("flat",   "drop", "take{:02d}", "."),
    "dept":       Template("nested", "dir",  "v{:03d}",    "."),
    "dept-flat":  Template("flat",   "dir",  "v{:03d}",    "."),
    "dept-name":  Template("nested", "name", "v{:03d}",    "_"),
    "dept-take":  Template("nested", "dir",  "tk{:02d}",   "_"),
}


def dest_dir(dst_root, seq, shot, dept, spec):
    """The directory one sequence lands in under the given template."""
    keep = dept if spec.dept == "dir" else ""
    if spec.dirs == "flat":
        folder = f"{seq}__{shot}__{keep}" if keep else f"{seq}__{shot}"
        return os.path.join(dst_root, folder)
    parts = [seq, shot] + ([keep] if keep else [])
    return os.path.join(dst_root, *parts)


def out_stem(base, dept, ver_tag, spec):
    """File name without frame number or extension, e.g. 'main_v001'."""
    if spec.dept == "name" and dept:
        return f"{base}_{dept}_{ver_tag}"
    return f"{base}_{ver_tag}"


def _template_expectation(spec):
    """What one template's output should parse to: the department (empty when
    the template drops that level) and which version-like group holds its
    tag."""
    dept = _DEPT if spec.dept in ("dir", "name") else ""
    token = "take" if spec.tag.format(1).lower().startswith(("tk", "take")) else "version"
    return dept, token


def test_layout_anchored_to_project_root():
    """Every directory layout, anchored at the project root.

    The [dir:*] tails slide to the deepest components, so a layout shallower than
    the widest template reads one level too high — <proj>/<seq>/<shot>/ gets
    claimed by [dir:seq_shot_dept], naming the project folder the sequence. The
    published project document is what stops the slide.

    Each template gets its own project root, built by dest_dir/out_stem from
    the same spec the expectation is derived from, plus the [dir:*] sections no
    template produces and the two ways out of the template set (media at the
    root, a tree deeper than any template)."""
    print("test_layout_anchored_to_project_root")
    root = tempfile.mkdtemp()
    try:
        for name, spec in TEMPLATES.items():
            proj = os.path.join(root, name)
            touch(os.path.join(proj, name + ".jpproj"))
            tag = spec.tag.format(1)
            out = dest_dir(proj, _SEQ, _SHOT, _DEPT, spec)
            stem = out_stem("main", _DEPT, tag, spec)
            make_exr_seq(out, stem, range(1, 4), sep=spec.sep)
            media = os.path.join(out, f"{stem}{spec.sep}0001.exr")

            dept, token = _template_expectation(spec)
            ctx = nc.get_path_context({"path": media})
            check(f"--template {name}: sequence/shot/department",
                  ctx == {"sequence": _SEQ, "shot": _SHOT, "department": dept},
                  f"{os.path.relpath(media, proj)} -> {ctx}")
            vals = nc.get_path_values({"path": media})
            check(f"--template {name}: asset and {token} tag",
                  vals.get("asset") == "main" and vals.get(token) == tag, str(vals))
            keys = picker_keys(media)
            want = (["department"] if dept else []) + ["asset", token]
            check(f"--template {name}: pickers {want}", keys == want, str(keys))

        # --mp4 writes one file per clip instead of a frame run; same directories,
        # so only the [file:*] rule differs.
        proj = os.path.join(root, "as_mp4")
        touch(os.path.join(proj, "as_mp4.jpproj"))
        spec = TEMPLATES["dept-take"]
        out = dest_dir(proj, _SEQ, _SHOT, _DEPT, spec)
        touch(os.path.join(out, out_stem("main", _DEPT, "tk01", spec) + ".mp4"))
        media = os.path.join(out, "main_tk01.mp4")
        check("--mp4 clip reads like its frame run",
              nc.get_path_context({"path": media})
              == {"sequence": _SEQ, "shot": _SHOT, "department": _DEPT}
              and nc.get_path_values({"path": media}).get("take") == "tk01",
              str(nc.get_path_values({"path": media})))

        # [dir:shot] — one level under the root. No template emits it; the config
        # declares it, and [dir:shot_dept] is unreachable once anchored because
        # [dir:seq_shot] is declared first and claims every two-level tree.
        proj = os.path.join(root, "one_level")
        touch(os.path.join(proj, "one_level.jpproj"))
        make_exr_seq(os.path.join(proj, _SHOT), "main_v001", range(1, 4), sep=".")
        ctx = nc.get_path_context(
            {"path": os.path.join(proj, _SHOT, "main_v001.0001.exr")})
        check("[dir:shot]: a lone folder is the shot",
              ctx == {"sequence": "", "shot": _SHOT, "department": ""}, str(ctx))

        # media in the project root itself: no layout levels at all
        proj = os.path.join(root, "at_the_root")
        touch(os.path.join(proj, "at_the_root.jpproj"))
        touch(os.path.join(proj, "main_v001.mov"))
        ctx = nc.get_path_context({"path": os.path.join(proj, "main_v001.mov")})
        check("media at the root has no sequence/shot/department",
              ctx == {"sequence": "", "shot": "", "department": ""}, str(ctx))

        # deeper than any template: the slide resumes, but still cannot pass the root
        proj = os.path.join(root, "too_deep")
        touch(os.path.join(proj, "too_deep.jpproj"))
        deep = os.path.join(proj, _SEQ, _SHOT, _DEPT, "aov")
        make_exr_seq(deep, "main_v001", range(1, 4), sep=".")
        ctx = nc.get_path_context({"path": os.path.join(deep, "main_v001.0001.exr")})
        check("a fourth level slides within the project, not above it",
              ctx == {"sequence": _SHOT, "shot": _DEPT, "department": "aov"}, str(ctx))

        # and with no document anywhere above, the slide is unfloored again
        proj = os.path.join(root, "unpublished")
        shot = os.path.join(proj, _SEQ, _SHOT)
        make_exr_seq(shot, "main_tk01", range(1, 4))
        ctx = nc.get_path_context({"path": os.path.join(shot, "main_tk01_0001.exr")})
        check("unfloored slide misreads the levels",
              ctx == {"sequence": "unpublished", "shot": _SEQ, "department": _SHOT},
              str(ctx))
    finally:
        shutil.rmtree(root, ignore_errors=True)


def test_project_path_from_media():
    """get_project_path_from_media walks up to the nearest directory publishing a
    document named after itself. It must not lean on the [dir:*] match: a take
    tree is only <root>/<sequence>/<shot>/, one level shallower than
    [dir:seq_shot_dept], whose sliding tail swallows the project folder and used
    to put the root one level too high — leaving "Open Project" hidden."""
    print("test_project_path_from_media")
    root = tempfile.mkdtemp()
    try:
        # two-level take tree: <proj>/<sequence>/<shot>/main_tk01_####.exr
        proj = os.path.join(root, "frog_take_naming")
        shot = os.path.join(proj, "Culinary Preparation", "Arranging the Platters")
        make_exr_seq(shot, "main_tk01", range(1, 4))
        touch(os.path.join(proj, "frog_take_naming.otio"))
        touch(os.path.join(proj, "frog_take_naming.jpproj"))
        media = os.path.join(shot, "main_tk01_0001.exr")
        found = nc.get_project_path_from_media({"path": media})
        jpproj = os.path.join(proj, "frog_take_naming.jpproj")
        check("take tree finds its project", found == jpproj, found)
        # the answer keeps the separators the query came in with
        slashed = nc.get_project_path_from_media({"path": nc._norm(media)})
        check("forward-slash query answers forward-slashed",
              slashed == nc._norm(jpproj), slashed)

        # three-level tree: <proj>/<sequence>/<shot>/<department>/main_####.exr
        proj2 = os.path.join(root, "frog")
        comp = os.path.join(proj2, "Culinary Preparation", "Arranging the Platters",
                            "Compositing")
        make_exr_seq(comp, "main", range(1, 4))
        touch(os.path.join(proj2, "frog.jpproj"))
        found = nc.get_project_path_from_media(
            {"path": os.path.join(comp, "main_0001.exr")})
        check("nested tree unaffected",
              found == os.path.join(proj2, "frog.jpproj"), found)

        # .otio only
        proj3 = os.path.join(root, "otio_only")
        touch(os.path.join(proj3, "a", "b", "main_0001.exr"))
        touch(os.path.join(proj3, "otio_only.otio"))
        found = nc.get_project_path_from_media(
            {"path": os.path.join(proj3, "a", "b", "main_0001.exr")})
        check(".otio used when no .jpproj",
              found == os.path.join(proj3, "otio_only.otio"), found)

        # nothing published anywhere above the media
        proj4 = os.path.join(root, "unpublished")
        touch(os.path.join(proj4, "a", "main_0001.exr"))
        found = nc.get_project_path_from_media(
            {"path": os.path.join(proj4, "a", "main_0001.exr")})
        check("no document -> empty", found == "", found)
        check("empty path -> empty", nc.get_project_path_from_media({"path": ""}) == "")
    finally:
        shutil.rmtree(root, ignore_errors=True)


def main():
    tests = [
        test_single_shot_exr_sequence,
        test_early_exit_first_department_wins,
        test_empty_first_department_falls_through,
        test_versioned_latest_and_asset_pref,
        test_multiple_shots_ordering,
        test_loose_files_at_root_ignored,
        test_take_tree_latest,
        test_take_exr_run_collapses,
        test_flat_folder_layout,
        test_dept_in_filename_not_absorbed,
        test_pickers_version_tree,
        test_pickers_take_tree,
        test_pickers_flat_folder,
        test_pickers_dept_in_filename,
        test_layout_anchored_to_project_root,
        test_project_path_from_media,
    ]
    for t in tests:
        t()
    print()
    if _failures:
        print(f"FAILED ({len(_failures)}): {', '.join(_failures)}")
        return 1
    print(f"OK — all {len(tests)} test groups passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())

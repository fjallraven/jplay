"""Standalone mock tests for jplay_discovery.

Note this test against the vanilla jplay_discovery module

If you have your own setup this needs to be updated, or ignored :)
"""

import os
import sys
import types
import tempfile
import shutil


# ─────────────── stub the host `jplay` module before import ───────────────
_jplay = types.ModuleType("jplay")
_jplay.register_callback = lambda *a, **k: None
_jplay.is_debug = lambda: False
_jplay.log = types.SimpleNamespace(info=lambda *a, **k: None,
                                   error=lambda *a, **k: None)
sys.modules["jplay"] = _jplay

_HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(_HERE, os.pardir, "python"))
import jplay_discovery as disc  # noqa: E402


# ─────────────────────────── tiny fs builder ───────────────────────────

def touch(path):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "wb") as f:
        f.write(b"\0")


def make_seq(directory, stem, frames=range(1, 4)):
    for n in frames:
        touch(os.path.join(directory, f"{stem}.{n:04d}.exr"))


def make_tree(root):
    """A frog-shaped {sequence}/{shot}/{department} tree:
      SeqA/Shot01/Animation    main v001..v003, playblast v001..v002 (exr runs)
      SeqA/Shot01/Compositing  main, unversioned exr run
      SeqA/Shot02/Animation    main v001..v002 (movies, not sequences)
      SeqB/Shot10/Lighting     main v001 only
      .thumbnails              non-media, must be ignored
    """
    anim1 = os.path.join(root, "SeqA", "Shot01", "Animation")
    for v in ("v001", "v002", "v003"):
        make_seq(anim1, f"main_{v}")
    for v in ("v001", "v002"):
        make_seq(anim1, f"playblast_{v}")
    make_seq(os.path.join(root, "SeqA", "Shot01", "Compositing"), "main")
    anim2 = os.path.join(root, "SeqA", "Shot02", "Animation")
    for v in ("v001", "v002"):
        touch(os.path.join(anim2, f"main_{v}.mov"))
    make_seq(os.path.join(root, "SeqB", "Shot10", "Lighting"), "main_v001")
    touch(os.path.join(root, ".thumbnails", "cafe.jpg"))


# ─────────────────────────── test harness ───────────────────────────

_failures = []


def check(name, cond, detail=""):
    status = "PASS" if cond else "FAIL"
    print(f"  [{status}] {name}" + (f"  -- {detail}" if detail and not cond else ""))
    if not cond:
        _failures.append(name)


def names(media):
    return [os.path.basename(m["path"]) for m in media]


# ─────────────────────────── tests ───────────────────────────

def test_describe_convention():
    print("test_describe_convention")
    c = disc.describe_convention()
    for level in ("sequence", "shot", "department", "asset", "version"):
        check(f"level {level} advertised", level in c["levels"], str(c["levels"]))
    check("pickers carry key/label/kind",
          all({"key", "label", "kind"} <= set(p) for p in c["pickers"]))
    check("shipped config tracks no version status", c["version_statuses"] == [],
          str(c["version_statuses"]))
    check("latest and all are always selectable",
          {"latest", "all"} <= set(c["version_selectors"]))
    check("config_path names a real file", os.path.isfile(c["config_path"]),
          c["config_path"])


def test_department_filter():
    """The whole reason find_media exists: inspect_directory's per-shot early exit
    means only one department per shot is ever reported."""
    print("test_department_filter")
    root = tempfile.mkdtemp()
    try:
        make_tree(root)
        anim = disc.find_media({"root": root, "department": "Animation"})
        comp = disc.find_media({"root": root, "department": "Compositing"})
        check("Animation found in both its shots", anim["count"] == 2,
              f"{anim['count']}: {names(anim['media'])}")
        check("Compositing found despite sharing a shot with Animation",
              comp["count"] == 1, f"{comp['count']}: {names(comp['media'])}")
        check("department is reported back",
              all(m["department"] == "Animation" for m in anim["media"]))
        check("case-insensitive department filter",
              disc.find_media({"root": root, "department": "animation"})["count"] == 2)
        check("glob department filter",
              disc.find_media({"root": root, "department": "Comp*"})["count"] == 1)
    finally:
        shutil.rmtree(root, ignore_errors=True)


def test_latest_version_of_versioned_sequence():
    """nc._collapse_media folds main_v001..v003 into one versionless entry at the
    lowest frame — i.e. v001. find_media must see the versions separately and
    pick v003."""
    print("test_latest_version_of_versioned_sequence")
    root = tempfile.mkdtemp()
    try:
        make_tree(root)
        r = disc.find_media({"root": root, "department": "Animation",
                             "shot": "Shot01"})
        check("one clip for the shot", r["count"] == 1, str(names(r["media"])))
        m = r["media"][0]
        check("latest version chosen (v003, not v001)", m["version"] == "v003",
              m["version"])
        check("path is the first frame of that version",
              os.path.basename(m["path"]) == "main_v003.0001.exr",
              os.path.basename(m["path"]))
        check("version_number is numeric", m["version_number"] == 3,
              str(m["version_number"]))
    finally:
        shutil.rmtree(root, ignore_errors=True)


def test_explicit_version():
    print("test_explicit_version")
    root = tempfile.mkdtemp()
    try:
        make_tree(root)
        r = disc.find_media({"root": root, "department": "Animation",
                             "shot": "Shot01", "version": "v002"})
        check("explicit token honoured",
              names(r["media"]) == ["main_v002.0001.exr"], str(names(r["media"])))
        r = disc.find_media({"root": root, "department": "Animation",
                             "shot": "Shot01", "version": "all"})
        check("all versions returned newest-first",
              [m["version"] for m in r["media"]] == ["v003", "v002", "v001"],
              str([m["version"] for m in r["media"]]))
    finally:
        shutil.rmtree(root, ignore_errors=True)


def test_unversioned_sequence():
    print("test_unversioned_sequence")
    root = tempfile.mkdtemp()
    try:
        make_tree(root)
        r = disc.find_media({"root": root, "department": "Compositing"})
        check("one clip", r["count"] == 1, str(names(r["media"])))
        m = r["media"][0]
        check("no version token", m["version"] == "", repr(m["version"]))
        check("first frame of the run",
              os.path.basename(m["path"]) == "main.0001.exr",
              os.path.basename(m["path"]))
    finally:
        shutil.rmtree(root, ignore_errors=True)


def test_asset_selection():
    print("test_asset_selection")
    root = tempfile.mkdtemp()
    try:
        make_tree(root)
        pref = disc.find_media({"root": root, "department": "Animation",
                                "shot": "Shot01"})
        check("no asset filter -> the preferred asset only",
              [m["asset"] for m in pref["media"]] == ["main"],
              str([m["asset"] for m in pref["media"]]))
        every = disc.find_media({"root": root, "department": "Animation",
                                 "shot": "Shot01", "asset": "*"})
        check("asset='*' -> every asset",
              sorted(m["asset"] for m in every["media"]) == ["main", "playblast"],
              str(sorted(m["asset"] for m in every["media"])))
        check("each asset at its own latest version",
              sorted((m["asset"], m["version"]) for m in every["media"])
              == [("main", "v003"), ("playblast", "v002")],
              str(sorted((m["asset"], m["version"]) for m in every["media"])))
        one = disc.find_media({"root": root, "department": "Animation",
                               "shot": "Shot01", "asset": "playblast"})
        check("named asset filter",
              names(one["media"]) == ["playblast_v002.0001.exr"],
              str(names(one["media"])))
    finally:
        shutil.rmtree(root, ignore_errors=True)


def test_sequence_filter_and_limit():
    print("test_sequence_filter_and_limit")
    root = tempfile.mkdtemp()
    try:
        make_tree(root)
        check("sequence filter",
              disc.find_media({"root": root, "sequence": "SeqB"})["count"] == 1)
        check("sequence glob",
              disc.find_media({"root": root, "sequence": "Seq*"})["count"] == 4)
        r = disc.find_media({"root": root, "limit": 2})
        check("limit caps results", r["count"] == 2, str(r["count"]))
        check("limit is reported", "limit" in r["note"], r["note"])
        check("dot-directories ignored",
              all(".thumbnails" not in m["path"]
                  for m in disc.find_media({"root": root})["media"]))
    finally:
        shutil.rmtree(root, ignore_errors=True)


def test_find_versions_selectors():
    print("test_find_versions_selectors")
    root = tempfile.mkdtemp()
    try:
        make_tree(root)
        path = os.path.join(root, "SeqA", "Shot01", "Animation",
                            "main_v003.0001.exr")

        v = disc.find_versions({"path": path})
        check("default selector is latest", v["selector"] == "latest")
        check("latest -> v003", [c["version"] for c in v["selected"]] == ["v003"],
              str([c["version"] for c in v["selected"]]))
        check("available lists every version newest-first",
              [c["version"] for c in v["available"]] == ["v003", "v002", "v001"],
              str([c["version"] for c in v["available"]]))

        check("bare number selector",
              [c["version"] for c in
               disc.find_versions({"path": path, "version": "2"})["selected"]]
              == ["v002"])

        miss = disc.find_versions({"path": path, "version": "v009"})
        check("missing version: understood but empty",
              miss["supported"] and not miss["selected"])
        check("missing version explains what exists", "v003" in miss["note"],
              miss["note"])

        status = disc.find_versions({"path": path, "version": "pending review"})
        check("status selector reported unsupported, not silently ignored",
              status["supported"] is False and status["selected"] == [])
        check("unsupported note names the selector",
              "pending review" in status["note"], status["note"])

        ident = disc.find_versions({
            "root": root, "sequence": "SeqA", "shot": "Shot01",
            "department": "Animation", "asset": "playblast"})
        check("identity dict resolves the same way",
              [c["version"] for c in ident["selected"]] == ["v002"],
              str([c["version"] for c in ident["selected"]]))
    finally:
        shutil.rmtree(root, ignore_errors=True)


def test_find_media_reports_unsupported_selector():
    print("test_find_media_reports_unsupported_selector")
    root = tempfile.mkdtemp()
    try:
        make_tree(root)
        r = disc.find_media({"root": root, "version": "pending review"})
        check("unsupported flag set", r["unsupported"] is True)
        check("no media returned", r["count"] == 0 and r["media"] == [])
        check("note explains why", "version status" in r["note"], r["note"])
    finally:
        shutil.rmtree(root, ignore_errors=True)


def test_version_provider_override():
    """A site with an asset database replaces the version lookup; find_media must
    pick it up, which is what set_version_provider is for."""
    print("test_version_provider_override")
    root = tempfile.mkdtemp()
    original = disc.find_versions
    try:
        make_tree(root)

        def fake(query):
            cands = query.get("candidates") or []
            for c in cands:
                c["status"] = "pending review" if c["number"] == 2 else "approved"
            return disc._select_versions(cands, query.get("version") or "latest")

        disc.set_version_provider(fake)
        r = disc.find_media({"root": root, "department": "Animation",
                             "shot": "Shot01", "version": "pending review"})
        check("status selector now resolves", r["unsupported"] is False, r["note"])
        check("it picked the version the provider tagged",
              names(r["media"]) == ["main_v002.0001.exr"], str(names(r["media"])))
        check("status is reported back",
              r["media"] and r["media"][0]["status"] == "pending review")
    finally:
        disc.find_versions = original
        shutil.rmtree(root, ignore_errors=True)


def test_list_projects():
    print("test_list_projects")
    home = tempfile.mkdtemp()
    saved = {k: os.environ.get(k) for k in ("USERPROFILE", "HOME")}
    try:
        proj_root = os.path.join(home, "shows", "frog")
        touch(os.path.join(proj_root, "frog.jpproj"))
        touch(os.path.join(proj_root, "frog.otio"))
        conf_dir = os.path.join(home, ".jplay")
        os.makedirs(conf_dir, exist_ok=True)
        with open(os.path.join(conf_dir, "settings.conf"), "w",
                  encoding="utf-8") as f:
            f.write("[recentProjects]\n")
            f.write(f"abc123|1785229453|{os.path.join(proj_root, 'frog.jpproj')}\n")
            f.write("|1780000000|" + os.path.join(home, "gone.jpproj") + "\n")
            f.write("[preferences]\ntimeFormatFrames=1\n")
        os.environ["USERPROFILE"] = home
        os.environ["HOME"] = home

        projects = disc.list_projects()
        check("both recent entries returned", len(projects) == 2, str(len(projects)))
        frog = projects[0]
        check("name is the project-file stem", frog["name"] == "frog", frog["name"])
        check("root is the project's directory", frog["root"] == proj_root,
              frog["root"])
        check("sibling otio detected",
              frog["otio"] == os.path.join(proj_root, "frog.otio"), frog["otio"])
        check("id and timestamp carried through",
              frog["id"] == "abc123" and frog["last_opened"] == 1785229453)
        check("source labelled", frog["source"] == "recent", frog["source"])
        check("missing sibling otio left empty", projects[1]["otio"] == "",
              projects[1]["otio"])
        check("[preferences] lines not mistaken for projects",
              all(p["name"] in ("frog", "gone") for p in projects),
              str([p["name"] for p in projects]))

        # A project name resolves to its root without an explicit root.
        r = disc.find_media({"project": "FROG"})
        check("project name resolves case-insensitively (no media, but found)",
              r["root"] == proj_root, f"{r['root']} / {r['note']}")
        check("unknown project explains itself",
              disc.find_media({"project": "nope"})["note"].startswith("no root"),
              disc.find_media({"project": "nope"})["note"])
    finally:
        for k, v in saved.items():
            if v is None:
                os.environ.pop(k, None)
            else:
                os.environ[k] = v
        shutil.rmtree(home, ignore_errors=True)


def test_preferences_cascade():
    print("test_preferences_cascade")
    home = tempfile.mkdtemp()
    exe_dir = tempfile.mkdtemp()
    saved = {k: os.environ.get(k)
             for k in ("USERPROFILE", "HOME", "JPLAY_EXE", "JPLAY_PREFERENCES")}
    try:
        # The shipped default, standing in for <exe dir>/jplay_preferences.conf.
        with open(os.path.join(exe_dir, "jplay_preferences.conf"), "w",
                  encoding="utf-8") as f:
            f.write("[timeline]\nclip_metadata_label = {department}\n")
            f.write("[control]\nenabled = false\nport = 52154\n")
        os.environ["JPLAY_EXE"] = os.path.join(exe_dir, "jplay.exe")
        os.environ["USERPROFILE"] = home
        os.environ["HOME"] = home
        os.environ.pop("JPLAY_PREFERENCES", None)

        cfg = disc._read_conf(disc._preferences_paths())
        check("shipped default read when it is the only tier",
              cfg.get("control", "port") == "52154",
              cfg.get("control", "port", fallback="<missing>"))

        # A user file naming a single option overrides only that option.
        conf_dir = os.path.join(home, ".jplay")
        os.makedirs(conf_dir, exist_ok=True)
        with open(os.path.join(conf_dir, "jplay_preferences.conf"), "w",
                  encoding="utf-8") as f:
            f.write("[control]\nenabled = true\n")
        cfg = disc._read_conf(disc._preferences_paths())
        check("user file overrides the option it names",
              cfg.get("control", "enabled") == "true",
              cfg.get("control", "enabled", fallback="<missing>"))
        check("options the user file omits keep the shipped value",
              cfg.get("control", "port") == "52154",
              cfg.get("control", "port", fallback="<missing>"))
        check("sections the user file omits survive intact",
              cfg.get("timeline", "clip_metadata_label") == "{department}",
              cfg.get("timeline", "clip_metadata_label", fallback="<missing>"))

        # $JPLAY_PREFERENCES is the strongest tier, and also merges.
        env_conf = os.path.join(exe_dir, "explicit.conf")
        with open(env_conf, "w", encoding="utf-8") as f:
            f.write("[control]\nport = 60001\n")
        os.environ["JPLAY_PREFERENCES"] = env_conf
        cfg = disc._read_conf(disc._preferences_paths())
        check("$JPLAY_PREFERENCES wins for the option it names",
              cfg.get("control", "port") == "60001",
              cfg.get("control", "port", fallback="<missing>"))
        check("$JPLAY_PREFERENCES leaves the user tier in force elsewhere",
              cfg.get("control", "enabled") == "true",
              cfg.get("control", "enabled", fallback="<missing>"))

        # A path that does not exist must not wipe the tiers below it.
        os.environ["JPLAY_PREFERENCES"] = os.path.join(exe_dir, "absent.conf")
        cfg = disc._read_conf(disc._preferences_paths())
        check("missing $JPLAY_PREFERENCES file falls through",
              cfg.get("control", "port") == "52154",
              cfg.get("control", "port", fallback="<missing>"))
    finally:
        for k, v in saved.items():
            if v is None:
                os.environ.pop(k, None)
            else:
                os.environ[k] = v
        shutil.rmtree(home, ignore_errors=True)
        shutil.rmtree(exe_dir, ignore_errors=True)


def main():
    tests = [
        test_describe_convention,
        test_department_filter,
        test_latest_version_of_versioned_sequence,
        test_explicit_version,
        test_unversioned_sequence,
        test_asset_selection,
        test_sequence_filter_and_limit,
        test_find_versions_selectors,
        test_find_media_reports_unsupported_selector,
        test_version_provider_override,
        test_list_projects,
        test_preferences_cascade,
    ]
    for t in tests:
        t()
    print()
    if _failures:
        print(f"FAILED ({len(_failures)}): {', '.join(_failures)}")
        return 1
    print(f"OK - all {len(tests)} test groups passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())

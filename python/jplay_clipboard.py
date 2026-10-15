import base64
import glob
import html
import json
import os
import re
from urllib.parse import unquote, urlparse

import jplay

_PARSERS = []


def register_parser(fn):
    """Ask `fn` first on every paste the host cannot resolve itself."""
    _PARSERS.insert(0, fn)


def _dbg(msg):
    """Log a step message when Settings > Python Debug is enabled."""
    if jplay.is_debug():
        jplay.log.info("[debug]   clipboard: " + msg)


# ---------------------------------------------------------------------------
# Existence, with frame patterns.

_FRAME_TOKEN = re.compile(r"%0?\d*d|#+|@+")


def _exists(path):
    if os.path.exists(path):
        return True
    name = os.path.basename(path)
    if not _FRAME_TOKEN.search(name):
        return False
    # A pattern exists when any frame of it does.
    pattern = os.path.join(os.path.dirname(path),
                           _FRAME_TOKEN.sub("[0-9]*", glob.escape(name)))
    return next(glob.iglob(pattern), None) is not None


# ---------------------------------------------------------------------------
# Brute force: anything that looks like an absolute path, in whatever the
# clipboard holds -- prose, a log line, JSON, HTML, a base64 blob. Registered
# first so it is asked last, after every parser that understands its format.

# Paths ask the disk, on the UI thread and often over NFS: a clipboard full of
# path-like noise must not stall the paste.
BRUTE_FORCE_MAX_CANDIDATES = 200
_BRUTE_FORCE_MAX_CHARS = 4 * 1024 * 1024

# An absolute path up to the first whitespace or quote. Not after a word
# character, '.' or ':', so "and/or", "./rel" and "https://host/x" are not paths.
_UNQUOTED_PATH = re.compile(
    r"""(?<![\w.:/\\])(?:/|[A-Za-z]:[\\/]|\\\\)[^\s"'<>|*?{}\[\]`]+""")
# A quoted one may hold spaces.
_QUOTED_PATH = re.compile(r"""(["'])((?:/|[A-Za-z]:[\\/]|\\\\)[^"'\n<>|*?]+)\1""")
_FILE_URL = re.compile(r"""file://[^\s"'<>]+""")
_BASE64_RUN = re.compile(r"[A-Za-z0-9+/]{64,}={0,2}")


def _brute_force_texts(content):
    """Every text to scan: each flavour, its unescaped form, and decoded base64."""
    for text in content.values():
        text = text[:_BRUTE_FORCE_MAX_CHARS]
        yield text
        # HTML entities and JSON escapes ("\/", "\\") hide paths from the regexes.
        unescaped = html.unescape(text).replace("\\/", "/").replace("\\\\", "\\")
        if unescaped != text:
            yield unescaped
        for m in _BASE64_RUN.finditer(text):
            try:
                yield base64.b64decode(m.group(0)).decode("utf-8")
            except (ValueError, UnicodeDecodeError):
                pass


def _path_depth(path):
    return len([p for p in re.split(r"[\\/]+", path) if p])


def parse_brute_force(content):
    paths = []
    seen = set()

    def add(path):
        path = path.strip().rstrip(".,;:)")
        if path in seen:
            return
        seen.add(path)
        # "/tmp" or "C:\" as a paste is a stray match, not a request to scan it.
        if _path_depth(path) < 2:
            return
        paths.append(path)

    for text in _brute_force_texts(content):
        for m in _FILE_URL.finditer(text):
            add(unquote(urlparse(m.group(0)).path))
        for m in _QUOTED_PATH.finditer(text):
            add(m.group(2))
        for m in _UNQUOTED_PATH.finditer(text):
            add(m.group(0))
        if len(paths) >= BRUTE_FORCE_MAX_CANDIDATES:
            break
    return paths[:BRUTE_FORCE_MAX_CANDIDATES]


register_parser(parse_brute_force)


# ---------------------------------------------------------------------------

def resolve_clipboard(content):
    """Media paths the clipboard names, or None when no parser claims it."""
    for parser in _PARSERS:
        try:
            paths = parser(content)
        except Exception as e:  # one broken parser must not cost the others
            _dbg(f"{getattr(parser, '__name__', parser)} raised: {e!r}")
            continue
        if not paths:
            continue
        seen = set()
        found = [p for p in paths if not (p in seen or seen.add(p)) and _exists(p)]
        _dbg(f"{getattr(parser, '__name__', parser)}: {len(found)}/{len(paths)} exist")
        if found:
            return found
    return None


jplay.register_callback("resolve_clipboard", resolve_clipboard)

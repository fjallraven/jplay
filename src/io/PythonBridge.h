#pragma once
#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

// Runtime queries into the callbacks jplay_init.py registered via
// jplay.register_callback() during startup (see PythonStartup.h). Every
// function below returns false if Python is not ready (jplayPythonReady()),
// the named callback was never registered, or it raised.

// One metadata-bar picker declared in the naming config's [picker:*] sections.
// `key` names a regex group; `kind` is "dir" or "file"; `label` is the caption.
//
// `multiCommit` marks the picker a multi-clip pick stops at (the naming config's
// [preferences] multi_select_picker): with several clips selected the picker menu
// hides everything after it and that picker commits for the whole selection. At
// most one picker carries it, and a config naming none is normal — the host then
// falls back to the picker before the last (see App::buildPickerColumns).
struct PickerDef {
    std::string key;
    std::string label;
    std::string kind;
    bool multiCommit = false;
};

// One selectable option of a picker. `value` is the identity — the string
// resolve_path and the commit chain are given, and the only part the naming
// convention knows about. `label` is what the UI draws and `color` how it draws
// it; the two differ only when Python annotated the option, either inline in
// describe_pickers or afterwards through decorate_pickers. Never pass `label`
// back into a Python query.
//
// `badge` is the other way to annotate, and the one that leaves the label alone:
// a short status word drawn as a filled chip right-aligned in the row. Its text
// is black or white by the chip's brightness, so `badgeColor` is a background,
// not a text color.
struct PickerOption {
    std::string value;
    std::string label;       // == value unless decorated
    uint32_t color = 0;      // packed 0xRRGGBB text color, 0 = the view's own
    std::string badge;       // right-aligned status chip; empty = none
    uint32_t badgeColor = 0; // packed 0xRRGGBB chip fill, 0 = the chip's own grey
};

// A picker resolved against a concrete media path: the option list and which one
// the path currently matches (-1 if none). Only pickers that apply to the path
// are returned by jplayDescribePickers.
struct PickerState {
    std::string key;
    std::vector<PickerOption> options;
    int currentIndex = -1;
};

// The configured pickers, in display order (calls list_pickers()). Static — the
// set does not depend on any path. Returns true on success; false if Python is
// not ready, the callback is not registered, or it raised.
bool jplayListPickers(std::vector<PickerDef>& out);

// Resolve every picker against `path` (calls describe_pickers({"path": path})),
// filling `out` with only those that apply and have options. Returns true on a
// successful call (even when `out` is empty); false if Python is not ready, the
// callback is not registered, or it raised.
//
// An entry of a picker's "options" list is either a plain string (the option is
// its own label, undecorated) or a dict {"value", "label", "color", "badge",
// "badge_color"} — so a site that already knows an option's status can label,
// color or badge it in the one call that gathers the options. "color" and
// "badge_color" are "#RRGGBB" (the leading # optional) or an (r, g, b) tuple of
// 0-255 ints; None, a missing key or an unparseable value all mean "the view's
// own color", as does pure black.
bool jplayDescribePickers(const std::string& path, std::vector<PickerState>& out);

// Whether a decorate_pickers callback exists at all, so a caller can skip the
// worker hop when no site registered one. Answered once and cached: the
// callbacks are registered at interpreter startup and never change, and this is
// called from the main thread while a decorate may be running — taking the GIL
// here would stall the UI for exactly as long as the callback holds it.
bool jplayHasDecoratePickers();

// Hand an already-resolved describe result to the optional decorate_pickers
// callback — decorate_pickers({"path": path}, states) — and fold its answer back
// into `states`. This is the slow lane: a site whose labels come from a database
// or a farm query registers this rather than doing the lookup inside
// describe_pickers, so the option lists show immediately and the annotations
// land when they land.
//
// `states` goes out in the same shape describe_pickers returns, with every
// option normalised to a {"value", "label", "color", "badge", "badge_color"}
// dict, and the callback returns states in that shape too — it may return only
// the pickers it cares about, and only the keys it changes. Merged back per
// (picker key, option value): ONLY `label`, `color`, `badge` and `badge_color`
// are taken. Options cannot be added, removed
// or reordered here and current_index is ignored, so decoration can never
// desync the cascade from what describe_pickers reported — change the option
// list in describe_pickers instead.
//
// STREAMING. decorate_pickers may be a generator (or return any other iterator)
// instead of returning the finished list, for the case that motivates this
// callback at all: a hundred options behind a lookup that takes twenty seconds.
// Each yielded value is a decorate result in its own right — the same list of
// picker dicts, partial in both pickers and options — merged as it arrives and
// handed to `onChunk`, so the options color in batches instead of all at the
// end. `onChunk` runs on the CALLING thread with the GIL held, and is given
// `states` in its merged-so-far form: copy out of it and marshal to the UI
// thread; do not call into Python or block on the main thread from it.
//
// `cancelled`, when given, is polled before each resume: once it returns true
// the generator is abandoned where it stands (never resumed; closed when the
// last reference to it goes) and the call returns with what it had merged. A
// yield in progress cannot be interrupted, so a callback that does all twenty
// seconds of work in one step is uncancellable — which is the reason to yield
// in batches rather than once at the end.
//
// Returns true when at least one label, color or badge actually changed (so a caller
// can skip re-laying out a view for nothing); false on no change, no callback,
// Python not ready, or a raise. A raise mid-stream keeps the batches that
// already merged rather than discarding them.
bool jplayDecoratePickers(const std::string& path, std::vector<PickerState>& states,
                          const std::function<void(const std::vector<PickerState>&)>& onChunk = {},
                          const std::function<bool()>& cancelled = {});

// Every naming-convention value the path carries (calls get_path_values({"path":
// path})): the picker keys plus the other named groups the convention matched —
// the sequence/shot it sits under, the extension — keyed by group name. Used to
// populate Media metadata at load / creation. Returns true on success; false if
// Python is not ready, the callback is not registered, or it raised.
bool jplayGetPathValues(const std::string& path,
                        std::map<std::string, std::string>& out);

// Batch form: `out` is resized to paths.size() and filled in the same order, one
// map per path. Importing a timeline tags every media at once, and the work per
// path is only a regex match, so taking the GIL once for the lot rather than per
// path is what keeps that from dominating. A path the callback raises on gets an
// empty map and does not abort the rest. Returns true if the callback ran (even
// when every path came back empty); false if Python is not ready or the callback
// is not registered.
bool jplayGetPathValues(const std::vector<std::string>& paths,
                        std::vector<std::map<std::string, std::string>>& out);

// The structural sequence/shot/department a media path resolves to (calls
// get_path_context({"path": path})). Unlike get_path_values, this reports the
// sequence/shot even for folder-based layouts where they live in the directory
// tree rather than as pickers. Omitted levels come back empty. Returns true on
// success; false if Python is not ready, the callback is not registered, or it
// raised.
bool jplayGetPathContext(const std::string& path, std::string& sequence,
                         std::string& shot, std::string& department);

// Resolve the media path that results from setting picker `key` to `value`.
// Calls resolve_path({"path": path}, key, value) and writes the returned path to
// outPath. Returns true on a non-empty result; false if Python is not ready, the
// callback is not registered, it raised, or it returned no path.
bool jplayResolvePath(const std::string& path, const std::string& key,
                      const std::string& value, std::string& outPath);

// One shot discovered by CREATE FROM DIRECTORY. Empty strings mean the chosen
// layout omits that level (e.g. no sequence or no department).
struct DiscoveredShot {
    std::string sequence;
    std::string shot;
    std::string department;
    std::string asset;
    std::string version;
    std::string path;
};

// Traverse `root` on the embedded interpreter (the create_from_directory
// callback registered by jplay_init.py), classifying its media into
// sequences/shots per the naming config. Fills `out` with one entry per shot
// (default department/asset, latest version), ordered by sequence then shot.
// Returns true on a successful call (even when nothing was discovered); false
// if Python is not ready, the callback is not registered, or it raised.
//
// The walk is long enough to run off the main thread, so an optional progress
// channel can be handed in: when the callback declares a `progress` parameter it
// is passed as `progress=` (as jplay.Progress) and the callback is expected to
// report the directory it is walking and to bail out when cancelled() goes true
// — in which case this returns false with `out` left as it was. A callback
// without that parameter is called with `root` alone, exactly as before.
class ProgressReporter;
bool jplayInspectDirectory(const std::string& root, std::vector<DiscoveredShot>& out,
                           ProgressReporter* progress = nullptr);

// What a text/URL drop turned out to refer to (see jplayResolveDropText).
struct DropResolution {
    std::string sequence;
    std::vector<std::string> paths; // media to bring in, in the order to lay it out
};

// Ask Python what a dropped text payload refers to (the resolve_drop_text
// callback). Dropping a row out of a web app hands over a URL naming the row,
// not a path on disk, so nothing here can resolve it — only the site's callback
// knows the tracker it came from. `proxyMode` is the host's effective proxy mode,
// passed through so the representation chosen matches what is on screen.
//
// Returns true when the callback claimed the payload — including when it claimed
// it and resolved no media, which is a real answer the caller reports rather than
// a failure. False when Python is not ready, no site registered the callback, it
// raised, or it answered None (the payload was not its to handle).
//
// Runs a network round-trip in the site's callback, so call it off the main
// thread. A tracker list can hold hundreds of versions, so — as with
// jplayInspectDirectory — an optional progress channel is handed in when the
// callback declares a `progress` parameter: it reports what it is looking up and
// bails out when cancelled() goes true. Cancellation is the callback's to honour;
// a callback that ignores it (or does not take the parameter) simply runs to
// completion, so the caller checks cancelled() itself before using the result.
bool jplayResolveDropText(const std::string& text, const std::string& proxyMode,
                          DropResolution& out, ProgressReporter* progress = nullptr);

// Resolve the project document the media at `path` belongs to (the
// get_project_path_from_media callback), for the "Show in Sequence" / "Open
// Project" actions. The callback answers from disk, preferring a published
// .jpproj over an .otio, so a path returned here exists: on success writes it to
// outPath and returns true. False if Python is not ready, the callback is not
// registered, it raised, or it returned no path (the project published neither
// document).
bool jplayProjectPathFromMedia(const std::string& path, std::string& outPath);

// Ask the naming config whether an EXR sequence about to be added should pull in
// a paired audio file (the query_audio callback). Calls query_audio({"path":
// path}); the callback returns either None (skip audio) or the audio to load —
// as a dict {"path": str, "offset": int} or a (path, offset) tuple, where offset
// is the audio's source-in point in frames. On a real result fills outPath and
// outOffset and returns true; returns false if Python is not ready, the callback
// is not registered, it raised, or it returned None / an empty path.
bool jplayQueryAudio(const std::string& path, std::string& outPath, int64_t& outOffset);

// Like jplayQueryAudio but WITHOUT the on-disk existence check: fills outPath
// with the candidate audio path (and outOffset) derived purely from `path`, so
// the host can batch the existence stat across threads. Returns false when the
// path has no derivable audio (same rejection rules as jplayQueryAudio).
//
// `allowScan` is passed to the callback as {"allow_scan": true} and licenses it
// to look at the disk for a near-miss alternative when the derived path is not
// there — the convention may then answer with a differently named sibling. It
// costs the round-trips the string derivation avoids, so ask for it only for the
// paths whose derived candidate failed to open, and ask from a worker thread:
// this is safe to call concurrently (the GIL is taken per call and Python drops
// it while it waits on the filesystem).
bool jplayDeriveAudio(const std::string& path, std::string& outPath, int64_t& outOffset,
                      bool allowScan = false);

// One entry of the global "Proxy" dropdown (src/ProxyMode.h / the top-bar menu).
// `value` is the identity handed to resolve_proxy_path; `label` is what the UI
// draws. The built-in "Full" entry (value "") is added by the host, never by
// Python.
struct ProxyModeOption {
    std::string value;
    std::string label;
    // Optional "slate_frames": leading frames of the substituted file that are
    // slate rather than shot, skipped by every read (see ProxyMode.h). 0 when the
    // mode declares none.
    int64_t slateFrames = 0;
    // Optional "default": true on the mode a project with no selection of its own
    // opens in, for a site whose media is published as proxies and where Full is
    // the exception rather than the starting point. False on every mode when no
    // entry claims it, which leaves that project on the host's "Full" as before.
    bool isDefault = false;
};

// The Python-supplied proxy modes, in display order (calls list_proxy_modes()).
// Static — does not depend on any path, mirrors jplayListPickers. Returns true
// on success (even when `out` comes back empty); false if Python is not ready,
// the callback is not registered, or it raised.
bool jplayListProxyModes(std::vector<ProxyModeOption>& out);

// Resolve the file to actually decode for `path` under proxy mode `mode` (calls
// resolve_proxy_path(path, mode)), `mode` being "" for the built-in "Full" — see
// ProxyMode.h on why that one is asked as well. Called from Media::ensureOpen(),
// which may run on any FrameCache/ThumbnailCache worker thread — safe to call
// concurrently, like jplayDeriveAudio (the GIL is taken per call). Returns true
// on a non-empty result written to outPath; false if Python is not ready, the
// callback is not registered, it raised, or it returned None / "" (meaning
// "no substitute for this source — decode the nominal path instead").
bool jplayResolveProxyPath(const std::string& path, const std::string& mode,
                           std::string& outPath);

// Which proxy mode `path` already IS (calls proxy_path_mode(path)): "" for the
// full-res representation, otherwise a value from list_proxy_modes. This is the
// question resolve_proxy_path cannot answer — it returns None both for "this
// path already is the mode you asked for" and for "this source has no such
// representation", and both decode the nominal path. Classifying the file that
// was actually opened is what lets the top-bar button say it is showing full res
// under a proxy mode rather than silently claiming the mode applied.
// Returns true when the callback answered with a string (the empty one included,
// so test the return value, not outMode); false when Python is not ready, the
// callback is not registered, it raised, or it returned None, all of which mean
// "unknown, say nothing".
bool jplayProxyPathMode(const std::string& path, std::string& outMode);

// How many leading frames of the file at `path` are slate rather than shot,
// given what representation that path already is: proxy_path_mode(path) names it
// and list_proxy_modes' "slate_frames" says what that representation carries in
// front of the shot. 0 for the full-res frames, for a path Python does not
// classify, and whenever either callback is missing or raises.
//
// This is the slate of the media's OWN file, which is what a media that got no
// proxy substitute decodes — see jplay::pathSlateFrames in ProxyMode.h, whose
// resolver this is. Called from Media::ensureOpen() on FrameCache/ThumbnailCache
// worker threads, so it is safe to call concurrently (the GIL is taken once per
// call, for both callbacks together); both are pure string work at this site.
int64_t jplayPathSlateFrames(const std::string& path);

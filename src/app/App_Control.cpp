// Local control channel dispatch. ControlServer.cpp owns the socket; everything
// here runs on the main thread from run(), so it may touch app state freely —
// the same discipline drainSync() follows for sync-review messages.
//
// Commands (one flat JSON object per request, one JSON object back):
//
//   {"cmd":"ping"}
//   {"cmd":"state"}
//   {"cmd":"open","path":"<file|dir>","mode":"append|replace|bin|sequence"}
//   {"cmd":"pick","key":"<picker>","value":"<option>","scope":"all|sequence|selection"}
//
// `open` dispatches on extension exactly as the command line does (see the argv
// loop in App::init): .jpproj loads a project, .otio imports one, anything else
// is media and `mode` decides where it lands. `mode` is ignored for the two
// project kinds — both inherently replace the open project, and both land on a
// view covering every sequence they brought in.
//
// Deliberately NOT routed through the file-browser wrappers (openProjectDialog(),
// addMediaViaBrowser()): those raise modal dialogs, which would
// block the main thread until a human clicked while the client sat waiting.
// Control commands only ever call the load primitives directly.

#include "App.h"
#include "AppInternal.h"
#include "ControlServer.h"
#include "ImageSeq.h"
#include "MediaScan.h"

#ifdef _WIN32
#include <process.h> // _getpid
#else
#include <unistd.h>  // getpid
#endif

#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>

using namespace jplay;

namespace fs = std::filesystem;

namespace {

// The pid lets a caller correlate this channel with the process it launched.
long currentPid() {
#ifdef _WIN32
    return (long)_getpid();
#else
    return (long)::getpid();
#endif
}

// Paths and names are already UTF-8 throughout the app (SDL hands argv and file
// drops over as UTF-8, and that is what the decoders want), and jsonEscape passes
// bytes >= 0x80 through untouched, so no transcoding is needed in either
// direction — the wire and the app agree on the encoding.
std::string jsonStr(const std::string& s) {
    return "\"" + control::jsonEscape(s) + "\"";
}

std::string okReply(const std::string& fields = {}) {
    return fields.empty() ? R"({"ok":true})" : "{\"ok\":true," + fields + "}";
}

std::string errReply(const std::string& msg) {
    return "{\"ok\":false,\"error\":" + jsonStr(msg) + "}";
}

// Wire name for the state reply's source type. Image sequences report the
// concrete format ("exr" as before, else "image") so a client can tell a
// scene-linear EXR sequence from an 8-bit sRGB one.
const char* clipTypeName(const Media& m) {
    switch (m.type()) {
    case ClipType::ImageSequence: return ImageSeq::isExrPath(m.path()) ? "exr" : "image";
    case ClipType::Audio:         return "audio";
    default:                      return "video";
    }
}

// Look a key up, returning `def` when absent.
std::string field(const std::map<std::string, std::string>& req, const char* key,
                  const std::string& def = {}) {
    auto it = req.find(key);
    return it == req.end() ? def : it->second;
}

} // namespace

std::string App::controlStateJson() {
    std::ostringstream o;
    o << "{\"ok\":true";
    o << ",\"app\":\"jplay\",\"pid\":" << currentPid();

    // "has_path" is literally "associated with a file on disk" — jplay tracks no
    // per-edit dirty flag, so this is not a claim about unsaved changes.
    o << ",\"project\":{\"path\":" << jsonStr(projectPath_)
      << ",\"name\":" << jsonStr(fileLabel(projectPath_))
      << ",\"has_path\":" << (projectHasPath_ ? "true" : "false")
      << ",\"id\":" << jsonStr(projectId_) << "}";

    o << ",\"fps\":" << timeline_.fps
      << ",\"playhead\":" << timeline_.playhead
      << ",\"length\":" << timeline_.length()
      << ",\"in\":" << timeline_.inPoint
      << ",\"out\":" << timeline_.outPoint
      << ",\"playing\":" << (playing_ ? "true" : "false")
      << ",\"tracks\":" << trackCount();

    o << ",\"view\":{\"sequence_index\":" << viewSeqIdx_
      << ",\"active_sequence_index\":" << activeSequenceIdx_ << "}";

    auto regs = timeline_.seqRegions();
    o << ",\"sequences\":[";
    for (size_t i = 0; i < timeline_.sequences.size(); ++i) {
        const Sequence& seq = timeline_.sequences[i];
        if (i) o << ',';
        o << "{\"id\":" << seq.id
          << ",\"name\":" << jsonStr(seq.name)
          << ",\"project\":" << jsonStr(timeline_.projectNameOfSeq(seq))
          << ",\"clips\":" << seq.clips.size()
          << ",\"start\":" << regs[i].start
          << ",\"end\":" << regs[i].end << "}";
    }
    o << "]";

    o << ",\"sources\":[";
    bool first = true;
    for (Media* m : sortedSources()) {
        if (!first) o << ',';
        first = false;
        MediaInfo info = m->info();
        o << "{\"path\":" << jsonStr(m->path())
          << ",\"type\":\"" << clipTypeName(*m) << "\""
          << ",\"frames\":" << info.frameCount
          << ",\"width\":" << info.width
          << ",\"height\":" << info.height
          << ",\"in_timeline\":" << (sourceInTimeline(m) ? "true" : "false")
          << ",\"open_failed\":" << (m->openFailed() ? "true" : "false") << "}";
    }
    o << "]";

    o << ",\"current_clip\":";
    if (const Clip* clip = playheadClip()) {
        auto media = timeline_.findMediaById(clip->mediaId);
        o << "{\"id\":" << clip->id
          << ",\"path\":" << jsonStr(media ? media->path() : std::string())
          << ",\"track\":" << clip->track
          << ",\"timeline_start\":" << clip->timelineStart
          << ",\"duration\":" << clip->duration
          << ",\"source_frame\":" << (clip->sourceOffset + timeline_.playhead - clip->timelineStart)
          << "}";
    } else {
        o << "null";
    }

    o << "}";
    return o.str();
}

// Add every media item in `paths` and return how many landed. `mode` has already
// been validated by controlOpen.
std::string App::controlAddMedia(const std::vector<std::string>& paths,
                                 const std::string& mode) {
    if (mode == "replace")
        newProject(); // force: unsaved changes are discarded, by design

    int sequenceId = -1;
    if (mode == "sequence") {
        addSequence(); // creates it, makes it active, and scopes the view to it
        sequenceId = timeline_.sequences.empty() ? -1 : timeline_.sequences.back().id;
    }

    std::vector<int> clipIds;
    int added = 0;
    std::string firstError;

    for (const std::string& p : paths) {
        if (mode == "bin") {
            // Pool-only: the source shows in the SOURCES bin (dimmed, since
            // sourceInTimeline() is false) with no clip anywhere. Already a
            // supported state — nothing in the panel needs to know.
            if (ensureMedia(p, mediaTypeForPath(p))) {
                ++added;
            } else if (firstError.empty()) {
                firstError = "failed to open " + p;
            }
            continue;
        }
        // A clip-placing mode: addMediaFile's default placement (auto-pick a
        // track of the file's kind, append at its end) — the same path a
        // command-line argument takes. The id the new clip will get is
        // nextClipId_, captured before the call as App::init does.
        const int clipId = nextClipId_;
        if (addMediaFile(p)) {
            ++added;
            clipIds.push_back(clipId);
        } else if (firstError.empty()) {
            firstError = "failed to add " + p;
        }
    }

    if (added == 0)
        return errReply(firstError.empty() ? "nothing was added" : firstError);

    if (mode == "bin")
        hostSnapshotDirty_ = true; // no clip was placed, so nothing else marks it

    std::ostringstream o;
    o << "\"added\":" << added << ",\"of\":" << paths.size();
    o << ",\"clip_ids\":[";
    for (size_t i = 0; i < clipIds.size(); ++i)
        o << (i ? "," : "") << clipIds[i];
    o << "]";
    if (sequenceId >= 0)
        o << ",\"sequence_id\":" << sequenceId;
    if (!firstError.empty())
        o << ",\"warning\":" << jsonStr(firstError);
    return okReply(o.str());
}

std::string App::controlOpen(const std::map<std::string, std::string>& req) {
    // Kept as UTF-8 and handed downstream untouched, so an "open" behaves exactly
    // like the same path passed on the command line.
    const std::string path = field(req, "path");
    if (path.empty())
        return errReply("open requires a \"path\"");

    // fs::u8path, not fs::path: MSVC reads a narrow std::string in the ANSI
    // codepage, which would fail to find a path with non-ASCII characters that
    // the decoders themselves open fine.
    const fs::path fsPath = fs::u8path(path);
    std::error_code ec;
    if (!fs::exists(fsPath, ec))
        return errReply("no such path: " + path);
    const bool isDir = fs::is_directory(fsPath, ec);

    // Project kinds replace whatever is open; mode does not apply to them.
    if (!isDir && hasExtension(path, ".jpproj")) {
        loadProject(path);
        // loadProject only reports failure via setStatus, but it leaves
        // projectPath_/projectHasPath_ untouched on a failed load (see the early
        // return in App_Project.cpp), so this distinguishes the two reliably.
        if (!projectHasPath_ || projectPath_ != path)
            return errReply("failed to load project: " + path);
        // Land on every sequence rather than the view scope the file happened to be
        // saved with. Opening over the channel is "show me this project", not a
        // resume of one person's last view, and the caller's next look at the
        // timeline should cover all of it — what picking the project's own row in
        // the Sequence popup does. A restored project scope covers only the
        // sequences of that one project, so this is the "All" filter instead: every
        // sequence the file holds, whichever project each came from.
        setSequenceView(-1);
        return okReply("\"loaded\":\"project\",\"path\":" + jsonStr(path));
    }
    if (!isDir && hasExtension(path, ".otio")) {
        loadOtio(path);
        // Same pattern: both of these are set only on the success path.
        if (loadOrigin_ != LoadOrigin::Otio || loadSourceLabel_ != fileLabel(path))
            return errReply("failed to import otio: " + path);
        return okReply("\"loaded\":\"otio\",\"path\":" + jsonStr(path));
    }

    const std::string mode = field(req, "mode", "append");
    if (mode != "append" && mode != "replace" && mode != "bin" && mode != "sequence")
        return errReply("unknown mode \"" + mode + "\" (append|replace|bin|sequence)");

    std::vector<std::string> paths;
    if (isDir) {
        // One entry per distinct EXR sequence / video, as a folder drop does.
        paths = MediaScan::collectDirectoryMedia(fsPath);
        if (paths.empty())
            return errReply("no supported media in " + path);
    } else {
        paths.push_back(path);
    }
    return controlAddMedia(paths, mode);
}

// Clips the "pick" command's `scope` names, in timeline order. Returns empty with
// `err` set for an unknown scope, and empty with `err` clear when the scope is
// valid but holds no clips (an empty project, or nothing selected).
std::vector<int> App::controlPickScope(const std::string& scope, std::string& err) const {
    std::vector<int> ids;
    if (scope == "selection") {
        // Whatever a human left selected. Included for completeness — a client has
        // no way to set the selection over this channel.
        ids = selectedClipIds_;
        return ids;
    }
    // "sequence" narrows to the sequence the view is filtered to; with an All or
    // project-wide filter there is no single one, which is an error rather than a
    // silent fall back to every clip.
    int only = -1;
    if (scope == "sequence") {
        only = filteredSeqIdx();
        if (only < 0) {
            err = "scope \"sequence\" needs the view filtered to one sequence; "
                  "it currently covers several (use scope \"all\")";
            return ids;
        }
    } else if (scope != "all") {
        err = "unknown scope \"" + scope + "\" (all|sequence|selection)";
        return ids;
    }
    for (size_t i = 0; i < timeline_.sequences.size(); ++i) {
        if (only >= 0 && (int)i != only)
            continue;
        for (const Clip& clip : timeline_.sequences[i].clips)
            ids.push_back(clip.id);
    }
    return ids;
}

// Switch clips to the media one metadata-picker pick names — the control-channel
// form of Ctrl+right-click > <picker> > <value>, applied to a scope rather than to
// whatever a human had selected. `key` must name a configured [picker:*]; `value`
// is resolved per clip by the naming layer's resolve_path, so it is only ever as
// valid as that site's convention (a value with no media for a given shot is
// reported as a miss, not an error).
std::string App::controlPick(const std::map<std::string, std::string>& req) {
    const std::string key   = field(req, "key");
    const std::string value = field(req, "value");
    if (key.empty() || value.empty())
        return errReply("pick requires \"key\" and \"value\"");

    buildMetaPickers(); // lazy: needs Python ready, which it is by the time we serve
    if (metaPickers_.empty())
        return errReply("no metadata pickers are configured (naming layer not loaded?)");
    std::string keys;
    bool known = false;
    for (const MetaPicker& p : metaPickers_) {
        keys += (keys.empty() ? "" : "|") + p.key;
        known = known || p.key == key;
    }
    if (!known)
        return errReply("unknown picker \"" + key + "\" (" + keys + ")");

    const std::string scope = field(req, "scope", "all");
    std::string err;
    std::vector<int> ids = controlPickScope(scope, err);
    if (!err.empty())
        return errReply(err);
    if (ids.empty())
        return errReply(scope == "selection" ? "no clips are selected"
                                             : "no clips in scope \"" + scope + "\"");

    std::vector<PickerSwitch> results = switchClipsByPicker(key, value, ids);

    int switched = 0;
    for (const PickerSwitch& r : results)
        if (r.switched)
            ++switched;

    std::ostringstream o;
    // `of` counts the clips actually considered, which is not the scope size:
    // switchClipsByPicker skips clips with no media (an audio-only clip, a source
    // that never opened), and a client comparing "switched 34 of 34" against a
    // 40-clip timeline needs in_scope to make the arithmetic add up.
    o << "\"key\":" << jsonStr(key) << ",\"value\":" << jsonStr(value)
      << ",\"scope\":" << jsonStr(scope)
      << ",\"switched\":" << switched << ",\"of\":" << results.size()
      << ",\"in_scope\":" << ids.size();
    o << ",\"clips\":[";
    for (size_t i = 0; i < results.size(); ++i) {
        const PickerSwitch& r = results[i];
        if (i) o << ',';
        o << "{\"clip_id\":" << r.clipId
          << ",\"switched\":" << (r.switched ? "true" : "false")
          << ",\"from\":" << jsonStr(r.from)
          << ",\"to\":" << jsonStr(r.to);
        if (!r.note.empty())
            o << ",\"note\":" << jsonStr(r.note);
        o << "}";
    }
    o << "]";
    // ok reports that the command ran, not that every clip moved: a shot with no
    // version for this pick is an expected outcome the per-clip list explains.
    return okReply(o.str());
}

// Publish <~/.jplay>/control.json so a tool can find the channel without the
// port being compiled into it. Written when the port actually becomes ours —
// which may be long after startup, if another instance had it first. Not removed
// on exit (jplay quits via _Exit), so a reader must treat it as a hint and
// confirm with a "ping" before trusting the pid.
void App::advertiseControlChannel() {
    const std::string& base = UserData::dir();
    if (base.empty())
        return; // no writable location; the fixed port still works
    std::ofstream f(base + "/control.json", std::ios::binary | std::ios::trunc);
    if (!f)
        return;
    f << "{\"port\":" << control_.port() << ",\"pid\":" << currentPid() << "}\n";
}

void App::drainControl() {
    if (!controlAdvertised_ && control_.listening()) {
        advertiseControlChannel();
        controlAdvertised_ = true;
    }

    for (control::Request& r : control_.drain()) {
        std::string response;
        try {
            std::map<std::string, std::string> req;
            if (!control::parseFlatObject(r.json, req)) {
                response = errReply("malformed request: expected one flat JSON object per line");
            } else {
                const std::string cmd = field(req, "cmd");
                if (cmd == "ping") {
                    response = okReply("\"app\":\"jplay\",\"pid\":" +
                                       std::to_string(currentPid()));
                } else if (cmd == "state") {
                    response = controlStateJson();
                } else if (cmd == "open") {
                    response = controlOpen(req);
                } else if (cmd == "pick") {
                    response = controlPick(req);
                } else if (cmd.empty()) {
                    response = errReply("missing \"cmd\"");
                } else {
                    response = errReply("unknown cmd \"" + cmd + "\" (ping|state|open|pick)");
                }
            }
        } catch (const std::exception& e) {
            response = errReply(std::string("command failed: ") + e.what());
        } catch (...) {
            response = errReply("command failed");
        }
        // The socket thread is blocked on this promise; it must always be set.
        r.reply.set_value(std::move(response));
    }
}

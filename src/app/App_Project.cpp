// Project translation unit: project (de)serialization (save / load / new / OTIO
// import + export), the recent-projects launcher shown over the empty player, and
// the background media-metadata refresh. App members, split out of App.cpp to keep
// that file manageable.

#include "App.h"
#include "AppInternal.h"

#include "ImageSeq.h"
#include "Layout.h"
#include "OtioExport.h"
#include "OtioImport.h"
#include "Preferences.h"
#include "Project.h"
#include "SkinColors.h"
#include "ProxyMode.h"
#include "PythonBridge.h"
#include "PythonStartup.h"
#include "SharedProjects.h"

#include <SDL3/SDL_dialog.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <ctime>
#include <filesystem>
#include <functional>
#include <thread>
#include <unordered_map>
#include <unordered_set>

namespace fs = std::filesystem;
using namespace jplay;

namespace {

// Downscale of a frame to a project thumbnail at most `maxWidth` wide (never
// upscaled), preserving its display aspect ratio (the frame's pixel aspect is
// applied here, so an anamorphic plate is stored in its true shape).
Project::Thumbnail makeThumbnail(const Frame& src, int maxWidth) {
    Project::Thumbnail t;
    if (src.width <= 0 || src.height <= 0)
        return t;

    const int sw = src.width, sh = src.height;
    const double dispW = sw * (src.pixelAspect > 0.0f ? src.pixelAspect : 1.0f);
    const int tw = std::min(maxWidth, (int)std::lround(dispW));
    const int th = std::max(1, (int)std::lround((double)tw * sh / dispW));
    renderFrameScaled(src, tw, th, t.rgba);
    if (t.rgba.empty())
        return t; // nothing to scale: the thumbnail stays invalid
    t.width = tw;
    t.height = th;
    return t;
}

// Read a project document into a scratch Timeline, the extension picking the
// loader: the OTIO importer for an .otio, the .jpproj deserialiser otherwise.
// Both kinds come back from the naming convention (get_project_path_from_media
// prefers a published .jpproj), so the two grafting actions below take either.
// Not a whole-project open — nothing here touches the loaded project's state.
bool loadProjectDocument(const std::string& path, Timeline& tl, int& nextClipId,
                         int& nextSeqId, int& nextShotId, std::string& err) {
    if (hasExtension(path, ".otio"))
        return OtioImport::load(path, tl, nextClipId, nextSeqId, nextShotId, err);
    std::string projectId; // dropped: the graft keeps the open project's own id
    if (!Project::load(path, tl, nextClipId, nextSeqId, nextShotId, projectId, err))
        return false;
    // Both grafting actions group by Sequence::projectId, which OtioImport stamps
    // from the file it read. A .jpproj carries whatever it was saved with, and some
    // of its sequences may belong to no project at all (ones built by "Create from
    // directory", say) — read as a project document they are this project's own, so
    // stamp them with it.
    int pid = -1;
    for (Sequence& s : tl.sequences)
        if (s.projectId < 0) {
            if (pid < 0)
                pid = tl.projectIdForPath(path, fs::u8path(path).stem().u8string());
            s.projectId = pid;
        }
    return true;
}

} // namespace

// ---------------------------------------------------------------- unsaved changes

// Hash the project as it would be written to disk. Project::saveBuffer produces
// the exact .jpproj byte layout, so this covers every field a save persists and
// cannot drift out of step with the edit paths the way a hand-maintained dirty
// flag does — a new kind of edit is caught the day it becomes serializable.
//
// The playhead is the one deliberate exclusion: it is persisted, but scrubbing
// and playback would otherwise mark the project dirty within a second of it
// being opened, which makes the prompt noise rather than a warning.
uint64_t App::projectSignature() {
    const int64_t playhead = timeline_.playhead;
    timeline_.playhead = 0;
    std::string buf, err;
    const bool ok = Project::saveBuffer(buf, timeline_, projectId_, err, viewState());
    timeline_.playhead = playhead;
    if (!ok)
        return savedSignature_; // cannot tell: claim clean rather than nag
    uint64_t h = 1469598103934665603ull; // FNV-1a
    for (unsigned char c : buf) {
        h ^= c;
        h *= 1099511628211ull;
    }
    return h;
}

// Is there anything in the project at all? Media in the bin counts: a project
// can have sources imported and nothing placed yet, and that is still work.
bool App::hasProjectContent() const {
    if (!timeline_.media.empty())
        return true;
    return timeline_.hasClips();
}

// "Just show me this file": a single clip sitting in the auto-created "Default
// Sequence" is what dropping one source in (or opening one from the command
// line) leaves behind, and re-doing it costs one drag. Prompting there is noise,
// so it is treated as nothing to lose — no matter what else is in the bin.
bool App::isSingleClipSession() const {
    if (timeline_.sequences.size() != 1)
        return false;
    const Sequence& s = timeline_.sequences.front();
    return s.name == "Default Sequence" && s.clips.size() <= 1;
}

// Gate a destructive action behind the unsaved-changes prompt. The buttons are
// the platform-standard Cancel / Don't Save / Save; `title` is what names the
// action being gated ("Quit jplay"). Cancel just drops `proceed` — the safe
// answer to a prompt the user backed out of is to do nothing.
void App::confirmDiscard(const char* title, std::function<void()> proceed) {
    if (!warnUnsaved_ || projectFromShared_ || !hasProjectContent() ||
        isSingleClipSession() || !projectDirty()) {
        proceed();
        return;
    }

    const std::string msg =
        std::string("\"") + (projectHasPath_ ? fileLabel(projectPath_) : std::string("Untitled")) +
        "\" has unsaved changes.";
    // Both the Don't Save and Save buttons need the action, and only one of them
    // ever runs, so they share one copy of it.
    auto action = std::make_shared<std::function<void()>>(std::move(proceed));

    showDialog(title, msg,
               { { "Cancel", nullptr },
                 { "Don't Save", [action] { (*action)(); } },
                 { "Save", [this, action] {
                       if (projectHasPath_) {
                           saveProject();
                           (*action)();
                           return;
                       }
                       // Never saved: the location chooser is async, so park the
                       // action and let processPendingDialogs run it once the file
                       // has actually been written.
                       afterSave_ = *action;
                       saveProjectDialog();
                   } } },
               /*escIdx*/ 0, /*enterIdx*/ 2);
    // Same switch as Settings > "Warn on Unsaved Changes", offered where the
    // warning actually annoys. Applied on the tick rather than on dismissal, so
    // it holds even if the user then backs out with Cancel.
    setDialogCheckbox("Don't warn me again", false, [this](bool on) {
        warnUnsaved_ = !on;
        writePrefs();
    });
}

void App::requestQuit() {
    confirmDiscard("Quit jplay", [this] { quit_ = true; });
}

// File > New Project (Ctrl+N) and the launcher's CREATE EMPTY PROJECT: clear to
// an empty timeline the user can work in straight away. Deliberately not the
// launcher — "New Project" promises a project, not a picker; Close Project is
// the action that goes back to the picker.
void App::requestNewProject() {
    confirmDiscard("New Project", [this] {
        newProject();
        hideLauncher_ = true;
    });
}

void App::requestCloseProject() {
    confirmDiscard("Close Project", [this] { closeProject(); });
}

// ---------------------------------------------------------------- project IO

void App::saveProject() {
    if (projectId_.empty())
        projectId_ = UserData::newProjectId();

    // Capture the frame currently under the playhead as a 256px-wide thumbnail
    // and store it out-of-band, keyed by project id *and* path (the .jpproj no
    // longer embeds it): the id survives a Save As, so each saved copy needs the
    // path in the key to get a thumbnail of its own. If it isn't cached (nothing
    // decoded, or playhead over empty space) the thumbnail stays invalid and the
    // previous one on disk, if any, is kept.
    if (const Clip* c = timeline_.clipAt(timeline_.playhead); c && !c->mediaId.empty()) {
        CacheKey key{ c->mediaId, c->sourceOffset + (timeline_.playhead - c->timelineStart) };
        if (FramePtr f = cache_->get(key)) {
            Project::Thumbnail thumb = makeThumbnail(*f, 256);
            if (thumb.valid())
                UserData::writeThumbnail(projectId_, projectPath_, thumb);
        }
    }

    std::string err;
    if (Project::save(projectPath_, timeline_, projectId_, err, viewState())) {
        int clipCount = 0;
        timeline_.forEachClip([&](const Clip&) { ++clipCount; });
        setStatus("SAVED " + fileLabel(projectPath_) +
                  "  clips:" + std::to_string(clipCount) +
                  "  shots:" + std::to_string(timeline_.shots.size()));
        recordCurrentProject();
        savedSignature_ = projectSignature(); // disk and memory now agree
        projectFromShared_ = false; // it has a .jpproj of its own now: prompt as normal
    } else {
        setStatus("SAVE FAILED: " + err, 5000);
    }
    updateWindowTitle();
}

void App::newProject() {
    work_.reset(); // shut down background jobs from the previous project
    joinRefresh();
    timeline_ = Timeline{};
    undoStack_.clear();
    nextClipId_ = 1;
    nextSeqId_  = 1;
    nextShotId_ = 1;
    nextTransitionId_ = 1;
    selectedTransitionId_ = -1;
    activeSequenceIdx_ = 0;
    viewHistory_.clear();      // the scopes it names belong to the outgoing project
    // Fresh project → the default colour-management mode, which is the one
    // [color_management] color_pipeline names (see OcioManager::prefersOcio), not a
    // fixed one: a site that ships "srgb" gets sRGB out of File > New too.
    timeline_.ocioEnabled = OcioManager::prefersOcio();
    ocio_.setEnabled(timeline_.ocioEnabled);
    reinitOcioForFirstSource();              // loads the config if this just turned OCIO on
    ocio_.setPreferredView(timeline_.ocioView); // empty: every config back on its default view
    // Timeline{} above reset the selection, so a fresh project starts on the
    // config's default mode, not necessarily Full.
    adoptDefaultProxyMode();
    pushProxyMode();
    resetThumbnailState();                   // tiles, textures and previews belong to the old pool
    // Forget the previously saved file so the next save prompts for a location.
    projectPath_ = "project.jpproj";
    projectHasPath_ = false;
    projectFromShared_ = false;
    projectId_.clear(); // a fresh project gets a new id on its first save
    cache_->clear();
    clearFramePreview();
    playing_ = false;
    hasTexture_ = false;
    displayedKey_ = CacheKey{};
    recentDirty_ = true;   // the empty player will show the recent list again
    hideLauncher_ = false; // ...unless the caller re-hides it (New Project does)
    fitView();
    if (gridView()) startClipThumbnails(); // restart thumbnail generation for the new clip set
    resetProjectScopeState();
    viewSeqIdx_ = 0; // "Sequence" view filter lands on the first (default) sequence
    // Stamped last: viewSeqIdx_ feeds the signature, so an earlier stamp would
    // leave the fresh project looking dirty the moment it is created.
    savedSignature_ = projectSignature(); // a fresh project has nothing to lose
    setStatus("NEW PROJECT");
    updateWindowTitle();
    refreshHostSnapshot(); // if hosting a sync session, push the empty project to viewers
}

// Put the project away and go back to the launcher. Same teardown as
// newProject() — the difference is only where it leaves you: New drops you on
// an empty timeline ready to work, Close hands you back the picker.
void App::closeProject() {
    // Land back on the Timeline: the Overview's contact sheet and the Layout's
    // tiles are both views of the clips that are about to go away. Done ahead of
    // newProject() so its gridView() thumbnail restart isn't fired for a clip set
    // that is being cleared.
    setPlayerStage(PlayerStage::Frame);
    newProject(); // leaves hideLauncher_ false, so the launcher comes back
    setStatus("PROJECT CLOSED");
}

void App::loadProject(const std::string& path, bool shared) {
    // Loading no longer opens any media: the timeline appears instantly from the
    // metadata cached in the project file. Decoders open lazily on first frame
    // request, and refreshMediaMetadata() re-probes anything that's gone stale.
    work_.reset(); // shut down background jobs from the previous project
    joinRefresh();

    std::string err;
    std::string id;
    Project::ViewState savedView;
    if (!Project::load(path, timeline_, nextClipId_, nextSeqId_, nextShotId_, id, err,
                       &savedView)) {
        setStatus("LOAD FAILED: " + err, 5000);
        return; // failure leaves projectPath_/projectHasPath_ as they were
    }
    // A shared project is brought in as a copy: the studio's file is never made
    // the current one, so Save prompts for a location (as it does for a shared
    // .otio) and the copy takes a fresh id on that first save.
    projectPath_ = shared ? "project.jpproj" : path;
    projectHasPath_ = !shared;
    projectFromShared_ = shared;
    projectId_ = shared ? std::string{} : id;
    // Projects saved with a start offset baked in (the leading sequence was removed
    // after its frames were laid out) open on frame 0 instead.
    timeline_.normalizeLead();
    ocio_.setEnabled(timeline_.ocioEnabled); // restore the project's color-management mode
    // The view the project was reviewed through. Handed over as a preference rather
    // than assigned: the config this project's media resolves to may not be loaded
    // yet (it is picked per source as the playhead reaches each clip), and it is on
    // that activation that the name has a display to be validated against.
    ocio_.setPreferredView(timeline_.ocioView);
    // Restore the project's Proxy dropdown selection, unless the feature is
    // globally disabled (Settings > Enable Proxy) -- then it stays Full even
    // though timeline_.proxyMode still records the saved selection. A project
    // that records none takes the naming config's default mode first.
    adoptDefaultProxyMode();
    pushProxyMode();
    resetThumbnailState();                   // tiles, textures and previews belong to the old pool
    undoStack_.clear();
    adoptLoadedTransitions();
    cache_->clear();
    clearFramePreview();
    playing_ = false;
    hasTexture_ = false;
    displayedKey_ = CacheKey{};
    // Restore the saved view scope — one sequence, or a whole project. Files saved
    // without one (or saved in the old "All" view) land on the first sequence. The
    // current frame was already restored via timeline_.playhead, so the scope is
    // applied without moving it.
    applyViewState(savedView);
    if (contentW_ <= 0.0f) pendingFit_ = true; // loaded before first layout (CLI arg)
    // No missing-source scan: the timeline appears instantly from the cached
    // metadata. Missing sources surface lazily on first decode (clip turns red;
    // click to relocate). A fresh project starts with no learned relocate rules.
    relocateRules_.clear();
    relocateRuleTried_.clear();
    loadOrigin_ = LoadOrigin::Project;
    loadSourceLabel_ = fileLabel(path);
    finishLoad();
}

void App::loadOtio(const std::string& path, bool shared) {
    // Import is a one-way bring-in: the .otio populates a fresh, unsaved project
    // (saving still targets a .jpproj). Media open lazily and have their metadata
    // probed by the same background refresh as a normal load.
    work_.reset(); // shut down background jobs from the previous project
    joinRefresh();

    Timeline tl;
    int nextId = 1, nextSeq = 1, nextShot = 1;
    std::string err;
    if (!OtioImport::load(path, tl, nextId, nextSeq, nextShot, err)) {
        setStatus("OTIO IMPORT FAILED: " + err, 5000);
        return;
    }
    timeline_ = std::move(tl);
    undoStack_.clear();
    nextClipId_ = nextId;
    nextSeqId_  = nextSeq;
    nextShotId_ = nextShot;
    adoptLoadedTransitions(); // .otio transitions came in alongside the clips
    activeSequenceIdx_ = 0;
    viewHistory_.clear();      // the scopes it names belong to the outgoing project
    projectPath_ = "project.jpproj"; // unsaved: next save prompts for a location
    projectHasPath_ = false;
    projectFromShared_ = shared;
    projectId_.clear();
    cache_->clear();
    clearFramePreview();
    playing_ = false;
    hasTexture_ = false;
    displayedKey_ = CacheKey{};
    resetProjectScopeState();
    // The whole .otio is now in: mark its record read whole, so the Sequence popup
    // can group its sequences under a project header (and stops offering to open it).
    const int projId = timeline_.projectIdForPath(path, fs::path(path).stem().string());
    if (SourceProject* rec = timeline_.findProjectById(projId))
        rec->openedWhole = true;
    // Every sequence of the file came in, so view them all as one project scope —
    // exactly what clicking the popup's project header does — rather than landing
    // on the first sequence alone. Membership is Sequence::projectId, stamped by
    // OtioImport from the same file.
    std::vector<int> seqIds;
    for (const Sequence& s : timeline_.sequences)
        if (s.projectId == projId)
            seqIds.push_back(s.id);
    if (!seqIds.empty()) {
        setProjectView(projId, std::move(seqIds));
    } else { // nothing carries the project's name (unstamped .otio): first sequence
        viewSeqIdx_ = 0;
        fitToFilteredSequence();
    }
    if (contentW_ <= 0.0f) pendingFit_ = true; // loaded before first layout (CLI arg)
    relocateRules_.clear();
    relocateRuleTried_.clear();
    loadOrigin_ = LoadOrigin::Otio;
    loadSourceLabel_ = fileLabel(path);
    finishLoad();
    // The importer builds picture clips only, so pair aligned audio the same way
    // the grafting paths do (Show in Sequence / Open Project). Gated on the
    // interpreter being up, which also excludes the command-line .otio: init()
    // runs before run() spawns Python startup, so there is nothing to query yet.
    if (attachAudioToSeq_ && jplayPythonReady())
        findAndAttachAudioAll(); // idempotent: skips clips that already have audio
}

// Build a fresh, unsaved project from a directory chosen in the launcher. The
// embedded interpreter (jplay_init.inspect_directory) walks the tree and
// classifies its media into sequences/shots per the naming config; then we open
// each discovered media, and lay one Shot + Clip per shot end-to-end, one
// Sequence per sequence name. Mirrors loadOtio's bring-in flow.
//
// Both halves are slow on a network share (a full tree walk, then one decoder
// open per shot), so they run on the progress worker: the Python walk reports
// the directory it is visiting, the open loop reports "n/total", and Cancel
// aborts either. Everything that touches the app's state — tearing down the
// previous project's background jobs and swapping the timeline in — happens in
// the completion on the main thread, so a cancelled or empty scan leaves the
// current project exactly as it was.
void App::createProjectFromDirectory(const std::string& root) {
    // Everything the worker produces, handed to the completion.
    struct DirBuild {
        Timeline tl;
        int  nextId = 1, nextSeq = 1, nextShot = 1;
        int64_t frames = 0;
        size_t discovered = 0;
        bool inspectOk = false;
    };
    auto out = std::make_shared<DirBuild>();

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "[create-from-dir] scanning \"%s\"", root.c_str());
    beginProgress("Inspecting directory " + fs::path(root).filename().string(),
        [root, out](ProgressReporter& pr) {
            pr.update(-1.f, "Scanning " + root + "…");
            std::vector<DiscoveredShot> discovered;
            out->inspectOk = jplayInspectDirectory(root, discovered, &pr);
            out->discovered = discovered.size();
            if (!out->inspectOk || pr.cancelled())
                return;

            Timeline& tl = out->tl;
            int64_t cursor = 0;
            std::string curSeqName;
            bool haveSeq = false;
            size_t index = 0;

            for (const auto& ds : discovered) {
                if (pr.cancelled()) {
                    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                                "[create-from-dir] cancelled after %zu/%zu media",
                                index, discovered.size());
                    return;
                }
                ++index;
                pr.update((float)index / (float)discovered.size(),
                          "Opening " + std::to_string(index) + "/" +
                          std::to_string(discovered.size()) + ": " +
                          fs::path(ds.path).filename().string());

                ClipType type = ImageSeq::isSequencePath(ds.path) ? ClipType::ImageSequence
                                                                 : ClipType::Video;

                auto media = std::make_shared<Media>(type, ds.path);
                if (!media->refreshMetadata()) { // need the frame count now to lay clips out
                    SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                                "[create-from-dir] cannot open, skipping: %s", ds.path.c_str());
                    continue;
                }
                media->setName(!ds.shot.empty() ? ds.shot : fs::path(ds.path).stem().string());
                // Every naming-convention value the path carries, rather than a
                // fixed department/version/asset triple: which groups a path fills is
                // the convention's business, so a take-named tree would otherwise get
                // its tag written under `version` with `take` left unset, and a
                // department living in the filename would be lost. The mismatch
                // desyncs pickerDivergenceIndex until the Component Picker re-reads
                // the path for itself. Same loop as unpackClip's.
                std::map<std::string, std::string> vals;
                jplayGetPathValues(media->resolvedPath(), vals);
                for (auto& kv : vals)
                    media->setMetaValue(kv.first, std::move(kv.second));
                int64_t frames = std::max<int64_t>(media->info().frameCount, 1);
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                            "[create-from-dir] opened %s: %lld frames, seq=\"%s\", shot=\"%s\"",
                            ds.path.c_str(), (long long)frames,
                            ds.sequence.c_str(), media->name().c_str());
                tl.media[media->id()] = media;

                // A change of sequence name starts a new Sequence (entries arrive
                // grouped by sequence, in order).
                std::string seqName = ds.sequence.empty() ? "Default Sequence" : ds.sequence;
                if (!haveSeq || seqName != curSeqName) {
                    Sequence s;
                    s.id = out->nextSeq++;
                    s.name = seqName;
                    tl.sequences.push_back(std::move(s));
                    curSeqName = seqName;
                    haveSeq = true;
                }
                Sequence& seq = tl.sequences.back();

                Shot sh;
                sh.id = out->nextShot++;
                sh.name = media->name();
                sh.timelineStart = cursor;
                sh.duration = frames;
                sh.initCut(0, frames);
                tl.shots.push_back(sh);

                Clip c;
                c.id = out->nextId++;
                c.mediaId = media->id();
                c.track = 0;
                c.timelineStart = cursor;
                c.duration = frames;
                c.sourceOffset = 0;
                c.shotId = sh.id;
                seq.clips.push_back(c);
                seq.shotIds.push_back(sh.id);

                cursor += frames;
            }
            out->frames = cursor;
        },
        [this, root, out] { finishCreateFromDirectory(root, out->tl, out->nextId, out->nextSeq,
                                                      out->nextShot, out->frames,
                                                      out->discovered, out->inspectOk); });
}

// Main-thread completion of createProjectFromDirectory: adopt the timeline the
// worker built, or report why nothing was built. `tl` is consumed.
void App::finishCreateFromDirectory(const std::string& root, Timeline& tl,
                                    int nextId, int nextSeq, int nextShot,
                                    int64_t frames, size_t discovered, bool inspectOk) {
    if (progress_.cancelled()) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "[create-from-dir] cancelled by the user -> project unchanged");
        setStatus("CREATE FROM DIRECTORY: CANCELLED", 3000);
        return;
    }
    if (!inspectOk) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "[create-from-dir] jplayInspectDirectory failed for \"%s\" -> abort, "
                    "project unchanged", root.c_str());
        setStatus("CREATE FROM DIRECTORY: PYTHON NOT READY", 5000);
        return;
    }
    if (discovered == 0) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "[create-from-dir] no shots discovered under \"%s\" -> abort, "
                    "project unchanged", root.c_str());
        setStatus("NO MEDIA DISCOVERED IN " + fileLabel(root), 5000);
        return;
    }
    if (!tl.hasClips()) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "[create-from-dir] all %zu discovered entr%s failed to open -> abort, "
                    "project unchanged", discovered, discovered == 1 ? "y" : "ies");
        setStatus("NO MEDIA DISCOVERED IN " + fileLabel(root), 5000);
        return;
    }
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "[create-from-dir] built %zu sequence(s), %d clip(s), %lld frames total",
                tl.sequences.size(), nextId - 1, (long long)frames);

    work_.reset(); // shut down background jobs from the previous project
    joinRefresh();

    timeline_ = std::move(tl);
    undoStack_.clear();
    nextClipId_ = nextId;
    nextSeqId_  = nextSeq;
    nextShotId_ = nextShot;
    activeSequenceIdx_ = 0;
    viewHistory_.clear();      // the scopes it names belong to the outgoing project
    projectPath_ = "project.jpproj"; // unsaved: next save prompts for a location
    projectHasPath_ = false;
    projectFromShared_ = false;
    projectId_.clear();
    cache_->clear();
    clearFramePreview();
    playing_ = false;
    hasTexture_ = false;
    displayedKey_ = CacheKey{};
    resetProjectScopeState();
    viewSeqIdx_ = 0; // "Sequence" view filter lands on the first sequence
    fitToFilteredSequence();
    relocateRules_.clear();
    relocateRuleTried_.clear();
    loadOrigin_ = LoadOrigin::Directory;
    loadSourceLabel_ = fileLabel(root);
    finishLoad();
}

// "Show in Sequence" (not-present branch): graft one sequence from an
// already-loaded project OTIO into the current timeline. The OTIO (produced by
// create_otio_project) is the whole project; here we take only the sequence the
// originating clip belongs to and append it as a new sequence, sharing media with
// the existing pool (Media ids are path-derived, so a path already loaded is
// reused). The imported Media are unopened — refreshMediaMetadata() probes them
// after the graft. The shot named `keepShot` has its media swapped to `keepPath`
// (the opened clip), so the originating media survives instead of being replaced
// by the OTIO's published version. Returns the new sequence index, or -1 when the
// sequence isn't in the OTIO / has no clips.
int App::graftOtioSequence(Timeline& src, const std::string& seqName, const std::string& sceneName,
                           const std::string& keepShot, const std::string& keepPath) {
    // Locate the source sequence. create_otio_project tags each clip with two
    // independent names: sequence_name — the shot's sequence display name out of
    // watchtower's sequences.json, which import folds into Sequence::name — and
    // scene_name, the <scene> element of the <project>/<scene>/... asset
    // path, which lands on Shot::sceneName. The two are unrelated strings and often
    // differ, and a caller resolving names from a media path only recovers the
    // scene, so try both as a sequence name before falling back to the scene tags:
    // Sequence::name against seqName, then against sceneName, then the first source
    // sequence carrying a shot whose scene_name matches.
    Sequence* srcSeq = nullptr;
    auto findByName = [&](const std::string& name) -> Sequence* {
        if (name.empty())
            return nullptr;
        for (Sequence& s : src.sequences)
            if (s.name == name) return &s;
        return nullptr;
    };
    srcSeq = findByName(seqName);
    if (!srcSeq)
        srcSeq = findByName(sceneName);
    if (!srcSeq && !sceneName.empty()) {
        for (Sequence& s : src.sequences) {
            for (int sid : s.shotIds)
                if (const Shot* sh = src.findShotById(sid))
                    if (sh->sceneName == sceneName) { srcSeq = &s; break; }
            if (srcSeq) break;
        }
    }
    if (!srcSeq || srcSeq->clips.empty()) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "[jplay] graftOtioSequence: no non-empty source sequence for "
                    "sequence \"%s\" / scene \"%s\" -> abort",
                    seqName.c_str(), sceneName.c_str());
        return -1;
    }

    Sequence seq;
    seq.id = nextSeqId_++;
    // The project it came from, for popup grouping: the source document's record
    // mapped into this timeline (matched on path, so a same-stem project stays
    // separate). Not marked opened whole — this is one sequence out of it.
    if (const SourceProject* sp = src.findProjectById(srcSeq->projectId))
        seq.projectId = timeline_.projectIdForPath(sp->path, sp->name);
    // Prefer the source sequence's real name (from OTIO sequence_name); fall back
    // to the scene name when the source sequence carries no name of its own.
    if (!srcSeq->name.empty()) {
        seq.name = srcSeq->name;
    } else {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "[jplay] graftOtioSequence: source sequence unnamed; naming grafted sequence after scene \"%s\"",
                    sceneName.c_str());
        seq.name = sceneName;
    }
    int64_t cursor = timeline_.length(); // append after existing content; repack re-bases

    // Lay the source clips end-to-end (create_otio_project emits no intra-sequence
    // gaps), one Shot + Clip each, preserving the OTIO's source offset and cut.
    for (const Clip& sc : srcSeq->clips) {
        auto media = src.findMediaById(sc.mediaId);
        if (!media)
            continue;
        // Reuse the existing pool entry for this path; otherwise adopt the
        // imported (unopened) Media. Media ids are path-derived, so equal paths
        // map to the same id across timelines.
        auto existing = timeline_.media.find(media->id());
        if (existing == timeline_.media.end())
            timeline_.media[media->id()] = media;
        else
            media = existing->second;

        const Shot* srcShot = src.findShotById(sc.shotId);
        const std::string shotName = srcShot ? srcShot->name : media->name();

        Shot sh;
        sh.id = nextShotId_++;
        sh.name = shotName;
        // Carry the scene tag across: it is the only thing tying the grafted
        // sequence back to a media path, whose naming convention yields a scene and
        // not the sequence_name this sequence is named after. Without it a second
        // "Show in Sequence" on the same scene can't recognize what it grafted the
        // first time and grafts a duplicate.
        if (srcShot)
            sh.sceneName = srcShot->sceneName;
        sh.timelineStart = cursor;
        sh.duration = sc.duration;
        sh.initCut(sc.sourceOffset, sc.sourceOffset + sc.duration);
        timeline_.shots.push_back(sh);

        Clip c;
        c.id = nextClipId_++;
        c.mediaId = media->id();
        c.track = 0;
        c.timelineStart = cursor;
        c.duration = sc.duration;
        c.sourceOffset = sc.sourceOffset;
        c.shotId = sh.id;
        seq.clips.push_back(c);
        seq.shotIds.push_back(sh.id);

        cursor += sc.duration;
    }

    if (seq.clips.empty()) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "[jplay] graftOtioSequence: no clips grafted for \"%s\" (all media missing) -> abort",
                    seq.name.c_str());
        --nextSeqId_; // roll back the unused id
        return -1;
    }
    timeline_.sequences.push_back(std::move(seq));
    const int newIdx = (int)timeline_.sequences.size() - 1;
    // Keep the originating clip's media for its shot (the OTIO carries the
    // preferred published version). replaceClipMedia keeps the clip's position.
    if (!keepShot.empty() && !keepPath.empty()) {
        for (Clip& c : timeline_.sequences[newIdx].clips) {
            const Shot* s = timeline_.findShotById(c.shotId);
            if (!s || s->name != keepShot)
                continue;
            auto m = timeline_.findMediaById(c.mediaId);
            if (!m || m->resolvedPath() != keepPath) {
                replaceClipMedia(c, keepPath);
            }
            break;
        }
    }

    timeline_.repackSequences();
    return newIdx;
}

// "Show in Sequence": the clip under the playhead resolves (via the naming
// convention) to a sequence it is not currently in. If that sequence already
// exists in the project, scope to it and navigate to the matching shot,
// replacing that shot's media with the opened clip when they differ. Otherwise
// discover the whole sequence from disk and append it as a new sequence (the
// originating clip stays where it is), then scope to it and navigate.
void App::showInSequence() {
    const Clip* active = getTopMostClipAtFrame(timeline_.playhead);
    if (!active || active->mediaId.empty()) {
        return;
    }
    auto media = timeline_.findMediaById(active->mediaId);
    if (!media) {
        return;
    }
    const std::string openedPath = media->resolvedPath();
    std::string sceneName  = media->metaValue("scene");
    std::string sequenceName  = "";
    std::string shotName = media->metaValue("shot");
    // The metadata is only cached when the media was added with Python ready;
    // if it wasn't resolved, parse the file path now via the naming convention
    // (and cache the result back so later actions don't have to re-parse). Uses
    // get_path_context for its fixed sequence/shot/department shape; note the
    // convention's "sequence" is cached here under the "scene" key, so this lookup
    // is independent of the "sequence" get_path_values writes.
    if (sceneName.empty() || shotName.empty()) {
        std::string ctxScene, ctxShot, ctxDept;
        if (jplayGetPathContext(openedPath, ctxScene, ctxShot, ctxDept)) {
            if (sceneName.empty() && !ctxScene.empty())  { sceneName  = ctxScene;  media->setMetaValue("scene", ctxScene); }
            if (shotName.empty() && !ctxShot.empty()) { shotName = ctxShot; media->setMetaValue("shot", ctxShot); }
        } else {
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "[jplay] Show in Sequence: path context lookup failed for \"%s\"", openedPath.c_str());
        }
    }
    // All the path resolves is the scene (the naming convention's "sequence",
    // cached under the "scene" key) — the OTIO's sequence_name is watchtower's
    // sequence display name, which is a different string on most projects. Pass the
    // scene as both: graftOtioSequence tries it as a sequence name and then against
    // the clips' scene_name tags, so either spelling finds the sequence.
    if (sequenceName.empty())
        sequenceName = sceneName;

    const int owner = timeline_.seqIndexOfClip(active->id);

    // Clip-local source frame under the playhead right now; preserved across the
    // jump so the readout lands on the same frame of the same shot in the target
    // sequence instead of snapping to the clip's first frame.
    const int64_t curSrcFrame =
        active->sourceOffset + (timeline_.playhead - active->timelineStart);

    // A clip's shot name: its linked Shot's name (set by discovery/import) if any,
    // else the resolved shot cached on its media.
    auto shotNameOfClip = [&](const Clip& c) -> std::string {
        if (c.shotId >= 0)
            for (const Shot& s : timeline_.shots)
                if (s.id == c.shotId)
                    return s.name;
        if (auto m = timeline_.findMediaById(c.mediaId))
            return m->metaValue("shot");
        return {};
    };

    // Already present as its own sequence? A loaded sequence is named with the
    // OTIO's sequence_name while all the path resolved is the scene, so accept
    // either spelling — the sequence's own name, or the scene_name its shots carry.
    // Scope to it and reconcile the shot: if that shot lives in the sequence, point
    // it at the opened clip (when they differ) and land on the same clip-local
    // frame. Returns true when handled.
    auto jumpToExistingSequence = [&]() -> bool {
        int found = -1;
        // By sequence name, in either spelling the caller resolved. Checked across
        // the whole list first: a name is the exact identity, a scene only narrows
        // to a folder that may hold several sequences.
        for (int i = 0; i < (int)timeline_.sequences.size() && found < 0; ++i) {
            const Sequence& s = timeline_.sequences[i];
            if (i != owner && !s.name.empty() && (s.name == sequenceName || s.name == sceneName))
                found = i;
        }
        // Else by the scene tags its shots carry — how a sequence grafted out of the
        // OTIO, named after its sequence_name, is recognized from a media path.
        for (int i = 0; i < (int)timeline_.sequences.size() && found < 0; ++i)
            if (i != owner && timeline_.sequenceHasScene(timeline_.sequences[i], sceneName))
                found = i;
        if (found < 0) {
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "[jplay] Show in Sequence: no existing sequence for sequence \"%s\" / "
                        "scene \"%s\"; will resolve OTIO",
                        sequenceName.c_str(), sceneName.c_str());
            return false;
        }
        scopeToSequence(found);
        Sequence& seq = timeline_.sequences[found];
        // What to call it in the log and status: its own name, falling back to the
        // scene when it was matched by tag and carries none.
        const std::string& shownName = seq.name.empty() ? sceneName : seq.name;
        Clip* match = nullptr;
        for (Clip& c : seq.clips)
            if (shotName.empty() || shotNameOfClip(c) == shotName) { match = &c; break; }
        if (match) {
            const int64_t tgtStart = match->timelineStart;
            const int64_t tgtOff   = match->sourceOffset;
            const int64_t tgtDur   = match->duration;
            auto mm = timeline_.findMediaById(match->mediaId);
            if (!mm || mm->resolvedPath() != openedPath) {
                replaceClipMedia(*match, openedPath); // keeps the clip's position
                if (gridView()) startClipThumbnails(); // new source -> regenerate grid thumbnail
            }
            int64_t at = std::clamp<int64_t>(tgtStart + (curSrcFrame - tgtOff),
                                             tgtStart, tgtStart + tgtDur - 1);
            setPlayhead(at);
        }
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "[jplay] Show in Sequence: matched existing sequence[%d] name=\"%s\" for "
                    "scene \"%s\" (matchedShot=%s)",
                    found, seq.name.c_str(), sceneName.c_str(), match ? "yes" : "no");
        setStatus("SHOWING SEQUENCE \"" + shownName + "\"");
        return true;
    };

    // By the clip's cached name (empty when its metadata wasn't resolved).
    if (jumpToExistingSequence()) {
        //if (attachAudioToSeq_)  // dont think we need this..
        //    findAndAttachAudioAll(); // idempotent: skips clips that already have audio
        return;
    }

    // Not present: resolve the project document (a published .jpproj, else the
    // .otio create_otio_project emits) via the naming convention, then load it off
    // the main thread (a single network file read, covered by the progress bar) and
    // graft the matching sequence into the current project on the main thread. The
    // graft keeps the originating clip's media for its own shot.
    std::string projPath;
    if (!jplayProjectPathFromMedia(openedPath, projPath)) {
        setStatus("SHOW IN SEQUENCE: NO PROJECT FOR \"" + sceneName + "\"", 5000);
        return;
    }
    auto src    = std::make_shared<Timeline>();
    auto ok     = std::make_shared<bool>(false);
    beginProgress("Show in Sequence: " + sceneName,
        [projPath, src, ok](ProgressReporter& pr) {
            pr.update(-1.0, "Loading sequence…");
            int nc = 1, ns = 1, nsh = 1;
            std::string err;
            *ok = loadProjectDocument(projPath, *src, nc, ns, nsh, err);
            if (!*ok)
                SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                             "[jplay] Show in Sequence: project load failed: %s", err.c_str());
        },
        [this, sequenceName, sceneName, shotName, curSrcFrame, openedPath, src, ok] {
            if (progress_.cancelled()) {
                setStatus("SHOW IN SEQUENCE: CANCELLED", 3000);
                return;
            }
            if (!*ok) {
                setStatus("SHOW IN SEQUENCE: PROJECT LOAD FAILED", 5000);
                return;
            }
            int newIdx = graftOtioSequence(*src, sequenceName, sceneName, shotName, openedPath);
            if (newIdx < 0) {
                setStatus("NO SHOTS FOUND FOR SEQUENCE \"" + sceneName + "\"", 5000);
                return;
            }
            // The grafted media came out of the project document knowing nothing of
            // the naming config, exactly as an OTIO import's do. Tag before the
            // probe, for the reason finishLoad gives.
            tagMediaPathValues();
            refreshMediaMetadata(); // probe the grafted (unopened) media in the background
            scopeToSequence(newIdx);
            // A clip's shot name (linked Shot name, else the media's cached shot).
            auto shotNameOfClip = [&](const Clip& c) -> std::string {
                if (c.shotId >= 0)
                    for (const Shot& s : timeline_.shots)
                        if (s.id == c.shotId)
                            return s.name;
                if (auto m = timeline_.findMediaById(c.mediaId))
                    return m->metaValue("shot");
                return {};
            };
            for (Clip& c : timeline_.sequences[newIdx].clips)
                if (!shotName.empty() && shotNameOfClip(c) == shotName) {
                    int64_t at = std::clamp<int64_t>(c.timelineStart + (curSrcFrame - c.sourceOffset),
                                                     c.timelineStart, c.timelineStart + c.duration - 1);
                    setPlayhead(at);
                    break;
                }
            setStatus("BUILT SEQUENCE \"" + sceneName + "\" (" +
                      std::to_string((int)timeline_.sequences[newIdx].clips.size()) + " SHOT(S))");
            if (attachAudioToSeq_)
                findAndAttachAudioAll(); // idempotent: skips clips that already have audio
        });
}

// What the clip under the playhead can open, from its naming convention: the
// sequence its path points at, and the project that sequence belongs to. Feeds the
// two "Open …" buttons in the top bar. Both actions graft out of the same project
// document, so neither is offered unless that document is actually on disk.
//
// The lookup is an interpreter call that stats a network path, so it is done
// once per media path and remembered in openTargetCache_. With allowResolve false
// (playback) a path that isn't cached yet simply offers nothing rather than paying
// for the lookup mid-play; it resolves as soon as playback stops.
void App::resolveOpenTargets(bool allowResolve) {
    openSeqName_.clear();
    openProjName_.clear();
    openProjPath_.clear();

    const Clip* active = getTopMostClipAtFrame(timeline_.playhead);
    if (!active || active->mediaId.empty())
        return;
    auto media = timeline_.findMediaById(active->mediaId);
    if (!media)
        return;
    const std::string path = media->resolvedPath();

    auto it = openTargetCache_.find(path);
    if (it == openTargetCache_.end()) {
        if (!allowResolve)
            return;
        if (!jplayPythonReady())
            return; // not cached: retried once Python finishes starting up
        OpenTarget t;
        // The project document, as "{project_root}/{project_name}" with a .jpproj
        // or .otio extension. The naming convention derives it from the media path
        // and returns only what it found on disk, so nothing resolved here can fail
        // the load for being unpublished. fs::u8path, not fs::path: MSVC reads a
        // narrow std::string in the ANSI codepage, and these paths are UTF-8.
        std::string projPath;
        if (jplayProjectPathFromMedia(path, projPath)) {
            t.projPath = projPath;
            t.projName = fs::u8path(projPath).stem().u8string();
            // Cached on the media when it was added with Python ready; parsed from
            // the path now otherwise (and cached back), as showInSequence does.
            t.seqName = media->metaValue("scene");
            if (t.seqName.empty()) {
                std::string ctxScene, ctxShot, ctxDept;
                if (jplayGetPathContext(path, ctxScene, ctxShot, ctxDept) && !ctxScene.empty()) {
                    t.seqName = ctxScene;
                    media->setMetaValue("scene", ctxScene);
                }
            }
        } else {
            SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION,
                         "[jplay] No project document for \"%s\"; nothing to open from it",
                         path.c_str());
        }
        it = openTargetCache_.emplace(path, std::move(t)).first;
    }
    const OpenTarget& t = it->second;
    if (t.projPath.empty())
        return; // nothing published to graft from

    // What the view currently shows is timeline state, not a property of the path, so
    // it is filtered here rather than baked into the cache. The test is where you are
    // looking, not what happens to be loaded: from an unrelated sequence (a "Default
    // Sequence", say) both actions still have somewhere to take you, and both handle
    // an already-loaded target without re-reading the project document.
    //
    // The project drops out only while the view is scoped to that whole project.
    const SourceProject* scoped = timeline_.findProjectById(viewProjId_);
    const bool viewingThatProject =
        scoped && (!scoped->path.empty() && !t.projPath.empty() ? scoped->path == t.projPath
                                                                : scoped->name == t.projName);
    if (!viewingThatProject) {
        openProjName_ = t.projName;
        openProjPath_ = t.projPath;
    }
    // The sequence drops out once the clip under the playhead lives in it: there is
    // nowhere left to go, and grafting it again would duplicate it. Scoping to a
    // sequence puts the playhead in it, so this covers "already viewing it" too.
    //
    // t.seqName is the scene the path resolves to, which names a sequence built
    // from a directory but not one grafted out of the OTIO — that one is named after
    // its sequence_name and can span several scenes. So the scene tags its shots
    // carry answer this too, or the graft would go unrecognized and every click
    // would append another copy of it.
    if (!t.seqName.empty()) {
        const int owner = timeline_.seqIndexOfClip(active->id);
        const bool alreadyIn = owner >= 0 && owner < (int)timeline_.sequences.size()
                            && (timeline_.sequences[owner].name == t.seqName
                                || timeline_.sequenceHasScene(timeline_.sequences[owner], t.seqName));
        if (!alreadyIn)
            openSeqName_ = t.seqName;
    }
}

// Group the loaded sequences by project and lay the Sequence popup's rows out in
// one pass, so the renderer and the click handler agree on what each row is.
// Membership is Sequence::projectId — stamped by OtioImport from the file it read
// and carried across a graft — so a header lists exactly the sequences of that
// project and nothing else. Only projects read whole become groups; a sequence
// belonging to no project (every one the user creates), or to a project not opened
// whole, is listed last as a row of its own.
void App::buildSequenceMenuRows() {
    seqMenuProjects_.clear();
    sequenceMenuRows_.clear();

    // The project each sequence is grouped under, or -1 for a row of its own.
    std::vector<int> seqProj(timeline_.sequences.size(), -1);
    for (size_t i = 0; i < timeline_.sequences.size(); ++i) {
        const SourceProject* proj =
            timeline_.findProjectById(timeline_.sequences[i].projectId);
        // A header claims to list every sequence of its project, which isn't true
        // until the project file itself was loaded; until then these stay ungrouped
        // and the top bar's "Open Project" button stays on offer.
        if (!proj || !proj->openedWhole)
            continue;
        seqProj[i] = proj->id;
        auto it = std::find_if(seqMenuProjects_.begin(), seqMenuProjects_.end(),
                               [&](const SeqMenuProject& p) { return p.id == proj->id; });
        if (it == seqMenuProjects_.end())
            seqMenuProjects_.push_back({ proj->id, proj->name, { timeline_.sequences[i].id } });
        else
            it->seqIds.push_back(timeline_.sequences[i].id);
    }

    auto add = [&](SeqMenuRow::Kind kind, std::string label, int seqIdx, int projIdx, bool indent) {
        SeqMenuRow r;
        r.kind = kind;
        r.label = std::move(label);
        r.seqIdx = seqIdx;
        r.projIdx = projIdx;
        r.indent = indent;
        sequenceMenuRows_.push_back(std::move(r));
    };

    for (int p = 0; p < (int)seqMenuProjects_.size(); ++p) {
        add(SeqMenuRow::Kind::Project, seqMenuProjects_[p].name, -1, p, false);
        for (size_t i = 0; i < timeline_.sequences.size(); ++i)
            if (seqProj[i] == seqMenuProjects_[p].id)
                add(SeqMenuRow::Kind::Sequence, timeline_.sequences[i].name, (int)i, -1, true);
    }
    for (size_t i = 0; i < timeline_.sequences.size(); ++i)
        if (seqProj[i] < 0 && !timeline_.sequences[i].temporary)
            add(SeqMenuRow::Kind::Sequence, timeline_.sequences[i].name, (int)i, -1, false);
}

std::string App::shotNameOfClip(const Clip& c) const {
    if (c.shotId >= 0)
        for (const Shot& s : timeline_.shots)
            if (s.id == c.shotId)
                return s.name;
    if (auto m = timeline_.findMediaById(c.mediaId))
        return m->metaValue("shot");
    return {};
}

void App::restoreViewPlayhead(const std::string& shotName, const std::string& mediaPath,
                              int64_t srcFrame) {
    if (shotName.empty() && mediaPath.empty())
        return;
    // The media path is the stronger match — the same shot can appear as a clip on a
    // different source (the OTIO's published version vs. what was open) — so a path
    // hit wins outright, and a shot-name hit is only a fallback.
    const Clip* best = nullptr;
    bool byPath = false;
    for (int si : viewSeqIndices()) {
        for (const Clip& c : timeline_.sequences[si].clips) {
            if (c.audio)
                continue; // the playhead follows the video program
            bool pathHit = false;
            if (!mediaPath.empty())
                if (auto m = timeline_.findMediaById(c.mediaId))
                    pathHit = m->resolvedPath() == mediaPath;
            if (pathHit) {
                best   = &c;
                byPath = true;
                break;
            }
            if (!best && !shotName.empty() && shotNameOfClip(c) == shotName)
                best = &c;
        }
        if (byPath)
            break;
    }
    if (!best)
        return;
    setPlayhead(std::clamp<int64_t>(best->timelineStart + (srcFrame - best->sourceOffset),
                                    best->timelineStart,
                                    best->timelineStart + best->duration - 1));
}

// "Open Project: <name>", from the top-bar button: load the
// project document off the main thread (one network file read, covered by the
// progress bar) and graft every sequence in it the project doesn't already have by
// name, then view the whole project. Additive — the existing clips stay where they
// are, and media are shared by path, so nothing already loaded is duplicated.
//
// As "Show in Sequence" does, the shot under the playhead is carried across: the
// project view otherwise opens on the project's first frame, which is a different
// shot in a different sequence from the one being reviewed.
void App::openResolvedProject(const std::string& projPath, const std::string& projectName) {
    // The clip under the playhead now, as (media path, shot name, clip-local source
    // frame) — the clip itself can't be held onto across the graft, which may
    // reallocate the sequences' clip vectors.
    std::string curPath, curShot;
    int64_t curSrcFrame = 0;
    if (const Clip* active = getTopMostClipAtFrame(timeline_.playhead)) {
        curShot     = shotNameOfClip(*active);
        curSrcFrame = active->sourceOffset + (timeline_.playhead - active->timelineStart);
        if (auto m = timeline_.findMediaById(active->mediaId))
            curPath = m->resolvedPath();
    }

    // Already read whole this session: everything it holds is loaded, so this is
    // purely a change of view and the file doesn't need re-reading. Matched on the
    // path, so a different project that happens to share this one's stem still gets
    // read rather than silently resolving to this one.
    if (const SourceProject* done = timeline_.findProjectByPath(projPath, projectName);
        done && done->openedWhole) {
        std::vector<int> seqIds;
        for (const Sequence& s : timeline_.sequences)
            if (s.projectId == done->id)
                seqIds.push_back(s.id);
        if (!seqIds.empty()) {
            setProjectView(done->id, std::move(seqIds));
            restoreViewPlayhead(curShot, curPath, curSrcFrame);
            setStatus("VIEWING PROJECT \"" + projectName + "\"");
            return;
        }
    }
    auto src = std::make_shared<Timeline>();
    auto ok  = std::make_shared<bool>(false);
    beginProgress("Open Project: " + projectName,
        [projPath, src, ok](ProgressReporter& pr) {
            pr.update(-1.0, "Loading project…");
            int nc = 1, ns = 1, nsh = 1;
            std::string err;
            *ok = loadProjectDocument(projPath, *src, nc, ns, nsh, err);
            if (!*ok)
                SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                             "[jplay] Open Project: load failed: %s", err.c_str());
        },
        [this, projPath, projectName, curShot, curPath, curSrcFrame, src, ok] {
            if (progress_.cancelled()) {
                setStatus("OPEN PROJECT: CANCELLED", 3000);
                return;
            }
            if (!*ok) {
                setStatus("OPEN PROJECT: LOAD FAILED", 5000);
                return;
            }
            // Recorded before the graft loop: the project file was read whole, so its
            // header is earned even when every sequence in it was already loaded
            // (grafted == 0 below) — otherwise the popup would keep offering to open
            // a project that has nothing left to bring in.
            const int projId = timeline_.projectIdForPath(projPath, projectName);
            if (SourceProject* rec = timeline_.findProjectById(projId))
                rec->openedWhole = true;
            int grafted = 0;
            for (const Sequence& s : src->sequences) {
                if (s.name.empty())
                    continue; // nothing to match or name the graft after
                bool present = false;
                // Only this project's own sequences, plus any belonging to none, can
                // already stand for one of its sequences. A same-named sequence of a
                // *different* project is a different sequence and gets grafted.
                for (Sequence& e : timeline_.sequences)
                    if (e.name == s.name && (e.projectId == projId || e.projectId < 0)) {
                        present = true;
                        // The project file names it as one of its own, so adopt it
                        // into the group: it may have been grafted one sequence at a
                        // time before the project was opened.
                        if (e.projectId < 0)
                            e.projectId = projId;
                        break;
                    }
                if (present)
                    continue;
                const int idx = graftOtioSequence(*src, s.name, s.name, "", "");
                if (idx < 0)
                    continue;
                ++grafted;
            }
            // Put this project's sequences into the order the document lists them.
            // The grafts above append, so a sequence already loaded — by "Show in
            // Sequence", or built by "Create from directory" — keeps whatever slot
            // it had, usually the first, and the rest of the project lands behind
            // it instead of around it.
            reorderProjectSequences(projId, *src);
            // View the whole project, exactly as clicking its header row does:
            // every sequence carrying this project's name (freshly grafted plus any
            // adopted above), not just the first one brought in. Grafting nothing is
            // not a failure — a project whose sequences were all already loaded (e.g.
            // built by "Create from directory", which leaves them unstamped until the
            // adoption above) still switches to its project view.
            std::vector<int> seqIds;
            for (const Sequence& s : timeline_.sequences)
                if (s.projectId == projId)
                    seqIds.push_back(s.id);
            if (seqIds.empty()) { // named nothing we hold and brought nothing in
                setStatus("NO SEQUENCES IN \"" + projectName + "\"", 5000);
                return;
            }
            if (grafted) {
                tagMediaPathValues();   // grafted media arrive untagged (see showInSequence)
                refreshMediaMetadata(); // probe the grafted (unopened) media in the background
            }
            setProjectView(projId, std::move(seqIds));
            restoreViewPlayhead(curShot, curPath, curSrcFrame);
            if (grafted)
                setStatus("OPENED \"" + projectName + "\" (" + std::to_string(grafted) +
                          " SEQUENCE(S))");
            else
                setStatus("VIEWING PROJECT \"" + projectName + "\"");
            if (grafted && attachAudioToSeq_)
                findAndAttachAudioAll(); // idempotent: skips clips that already have audio
        });
}

// Rewrite the slots this project's sequences occupy so they run in the order the
// project document lists them, leaving every other sequence at its own index — a
// sequence of another project, or one of the user's own, never moves. Called from
// openResolvedProject once its grafts are in; the timeline order is what both the
// project view (viewSeqIndices) and the packed layout (seqRegions) follow, so
// without this the project plays in the order the sequences happened to arrive.
void App::reorderProjectSequences(int projId, const Timeline& src) {
    std::vector<int> slots; // this project's indices, ascending
    for (int i = 0; i < (int)timeline_.sequences.size(); ++i)
        if (timeline_.sequences[i].projectId == projId)
            slots.push_back(i);
    if (slots.size() < 2)
        return;

    // Rank by position in the document, matched on name as the graft loop does. One
    // the document doesn't name ranks past every named one and keeps its relative
    // order among the rest.
    auto rank = [&](const Sequence& s) {
        for (size_t i = 0; i < src.sequences.size(); ++i)
            if (src.sequences[i].name == s.name)
                return (int)i;
        return (int)src.sequences.size();
    };
    std::vector<int> order = slots;
    std::stable_sort(order.begin(), order.end(), [&](int a, int b) {
        return rank(timeline_.sequences[a]) < rank(timeline_.sequences[b]);
    });
    if (order == slots)
        return; // already in document order

    // Pin the timeline's leading edge, as the Project Explorer's drag-reorder does:
    // the repack would otherwise re-base everything on whichever sequence is now
    // first (see Timeline::repackSequences).
    int64_t lead = 0;
    for (const Sequence& s : timeline_.sequences) {
        int64_t a = 0, b = 0;
        if (timeline_.sequenceSpan(s, a, b)) { lead = a; break; }
    }
    std::vector<Sequence> reordered;
    reordered.reserve(order.size());
    for (int i : order)
        reordered.push_back(std::move(timeline_.sequences[i]));
    for (size_t k = 0; k < slots.size(); ++k)
        timeline_.sequences[slots[k]] = std::move(reordered[k]);
    timeline_.repackSequences(&lead);
    timeline_.clampPlayhead();
    // activeSequenceIdx_ / the view scope are re-derived by the setProjectView call
    // that follows, so no index held elsewhere survives this.
}

// ------------------------------------------------------ missing-source relocation

// A relocated candidate is accepted only if the same-named file is present and,
// for an image sequence, the frame count matches the count cached in the project
// (so a same-named-but-different sequence is rejected).
bool App::candidateMatches(const Media& m, const std::string& candidatePath) const {
    if (m.type() == ClipType::ImageSequence) {
        auto files = ImageSeq::files(candidatePath);
        if (files.empty())
            return false;
        int64_t want = m.info().frameCount;
        return want <= 0 || (int64_t)files.size() == want;
    }
    std::error_code ec;
    // is_regular_file() already returns false for a missing path, so this is a
    // single stat (no redundant exists() probe).
    return fs::is_regular_file(candidatePath, ec);
}

// Try each learned path-prefix substitution against the media's path; the first
// whose substituted path exists on disk wins. Deliberately a plain existence
// check (no frame-count/content validation): the rule came from a relocation the
// user just confirmed, so a same-named file at the mirrored location is trusted.
bool App::tryRelocateRules(const Media& m, std::string& out) const {
    const std::string& p = m.path();
    for (const auto& r : relocateRules_) {
        if (p.size() >= r.oldPrefix.size() &&
            p.compare(0, r.oldPrefix.size(), r.oldPrefix) == 0) {
            std::string cand = r.newPrefix + p.substr(r.oldPrefix.size());
            std::error_code ec;
            if (fs::exists(cand, ec)) {
                out = std::move(cand);
                return true;
            }
        }
    }
    return false;
}

// Learn a substitution rule from a resolved pair: strip the longest common
// suffix, and the remaining leading fragments are the (old -> new) prefixes
// (e.g. "D:\...\main.exr" + "C:\dev\...\main.exr" -> "D:" -> "C:\dev").
void App::addRuleFromPaths(const std::string& oldPath, const std::string& newPath) {
    size_t i = oldPath.size(), j = newPath.size();
    while (i > 0 && j > 0 && oldPath[i - 1] == newPath[j - 1]) { --i; --j; }
    RelocateRule r{ oldPath.substr(0, i), newPath.substr(0, j) };
    if (r.oldPrefix != r.newPrefix)
        relocateRules_.push_back(std::move(r));
}

// Point a media item at a new path while keeping its id (clips reference media
// by id, so the id must stay stable). Carries name/metadata/pickers across;
// dimensions/length are re-probed by the metadata refresh after resolution.
void App::relocateMedia(const std::string& mediaId, const std::string& newPath) {
    auto it = timeline_.media.find(mediaId);
    if (it == timeline_.media.end())
        return;
    auto old = it->second;
    auto fresh = std::make_shared<Media>(old->type(), newPath, mediaId);
    fresh->setName(old->name());
    for (const auto& kv : old->meta())
        fresh->setMetaValue(kv.first, kv.second);
    if (old->pickersResolved())
        fresh->setApplicablePickers(old->applicablePickers());
    // Keep the cached dimensions/length so clip layout stays valid immediately,
    // but clear the freshness hash so the background refresh re-probes the new
    // files (the old hash fingerprinted the now-stale path).
    MediaInfo info = old->info();
    info.freshHash = 0;
    fresh->setInfo(info);
    it->second = std::move(fresh);
}

// Proactive sweep over the whole project, run right after a successful relocation:
// every *other* source whose path starts with the same (now-moved) directory
// prefix gets the substitution applied, and is repointed if the new file exists.
// No content check and no wait for the clip to be reached — just string-replace
// the prefix and relocate when the destination is present. Callers clear the
// frame cache afterwards.
void App::propagateRelocateRules() {
    for (const auto& kv : timeline_.media) {
        std::string relocated;
        if (tryRelocateRules(*kv.second, relocated))
            relocateMedia(kv.first, relocated);
    }
    relocateRuleTried_.clear(); // sweep already covered everything the rules match
}

// Per-frame safety net (main thread): if a clip turns up missing later (during
// playback/caching) and a learned rule now resolves it, fix it silently. Cheap
// and a no-op until a rule is learned; each missing source is tried once per rule
// set. The proactive sweep above handles the common case; this catches sources
// whose destination files appear only after the relocation.
void App::resolveMissingWithRules() {
    if (relocateRules_.empty())
        return;
    bool anyResolved = false;
    for (const auto& kv : timeline_.media) {
        const std::string& id = kv.first;
        Media& m = *kv.second;
        if (!m.openFailed() || relocateRuleTried_.count(id))
            continue;
        relocateRuleTried_.insert(id); // try each missing source once per rule set
        std::string relocated;
        if (tryRelocateRules(m, relocated)) {
            relocateMedia(id, relocated);
            anyResolved = true;
        }
    }
    if (anyResolved) {
        cache_->clear();       // drop the now-stale "missing" gaps so frames re-decode
        clearFramePreview();
        hasTexture_ = false;
        displayedKey_ = CacheKey{};
    }
}

// Open the Missing Source modal for `m`, pre-filling the directory field with the
// missing file's current folder so the user need only edit the changed prefix.
void App::openRelocateModal(const Media& m) {
    relocatingMediaId_ = m.id();
    relocateModalOpen_ = true;
    relocateError_.clear();
    relocateDirInput_.setText(fs::path(m.path()).parent_path().string());
    relocateDirInput_.setFocus(true);
    SDL_StartTextInput(window_);
}

void App::closeRelocateModal() {
    relocateModalOpen_ = false;
    relocateDirInput_.setFocus(false);
    relocateError_.clear();
    SDL_StopTextInput(window_);
}

// Validate the directory typed/browsed into the field and, on success, learn a
// path-prefix rule + relocate this clip; on failure show an inline error and stay
// open. The learned rule is then swept across the whole project so every other
// source under the same moved directory is repointed silently.
void App::doRelocateFromField() {
    auto m = timeline_.findMediaById(relocatingMediaId_);
    if (!m) { closeRelocateModal(); return; }

    std::string dir = relocateDirInput_.text();
    if (dir.empty()) { relocateError_ = "Enter or browse to a folder."; return; }

    std::string base = fs::path(m->path()).filename().string();
    std::string cand = (fs::path(dir) / base).string();
    if (!candidateMatches(*m, cand)) {
        relocateError_ = "Folder has no \"" + base + "\" with a matching frame count.";
        return;
    }

    addRuleFromPaths(m->path(), cand);
    relocateMedia(m->id(), cand);
    propagateRelocateRules();    // silently repoint every sibling under the same moved prefix
    cache_->clear();             // drop the stale "missing" frames so relocated clips re-decode
    clearFramePreview();
    hasTexture_ = false;
    displayedKey_ = CacheKey{};
    closeRelocateModal();
    resolveMissingWithRules();   // immediately fix any siblings the new rule covers
}

// Consume input while the modal is open (host calls this before other handlers
// and skips them). Enter = Relocate, Esc = Cancel (just dismisses; nothing to undo).
void App::handleRelocateModalEvent(const SDL_Event& e) {
    if (e.type == SDL_EVENT_KEY_DOWN) {
        if (e.key.key == SDLK_ESCAPE) { closeRelocateModal(); return; }
        if (e.key.key == SDLK_RETURN || e.key.key == SDLK_KP_ENTER) { doRelocateFromField(); return; }
    }

    relocateDirInput_.handleEvent(e);

    if (relocateBrowseBtn_.handleEvent(e)) {
        const char* loc = relocateDirInput_.text().empty() ? nullptr
                                                            : relocateDirInput_.text().c_str();
        SDL_ShowOpenFolderDialog(&App::onRelocateChosen, this, window_, loc, false);
        return;
    }
    if (relocateBtn_.handleEvent(e))       { doRelocateFromField(); return; }
    if (relocateCancelBtn_.handleEvent(e)) { closeRelocateModal(); return; }
}

// Final status + housekeeping after a load. A project load trusts its cached
// metadata and does no probing (missing sources surface lazily); imports and
// directory builds refresh metadata in the background as before.
void App::finishLoad() {
    // A project is now open, so the launcher is done for good — emptying its
    // timeline (deleting the last clip) must not bring it back over the user's
    // work. newProject() clears the flag again.
    hideLauncher_ = true;
    // A .jpproj load matches a file on disk, so it starts clean. An import or a
    // directory build does not: it is unsaved work by construction (both leave
    // projectHasPath_ false), so it must start dirty. 0 is the "matches nothing"
    // signature — projectSignature() hashes a buffer that always carries the
    // JPLY magic, so it does not collide with a real project in practice.
    savedSignature_ = (loadOrigin_ == LoadOrigin::Project) ? projectSignature() : 0;
    reinitOcioForFirstSource(); // a loaded project may supply the first source path
    // Tag before the probe, not after: tagMediaPathValues reads each media's
    // resolvedPath(), which takes the same lock refreshMediaMetadata's workers hold
    // for the whole of a decoder open. Spawning the probe first put this loop behind
    // one network open per media — the entire probe pass, on the main thread.
    tagMediaPathValues(); // untagged media (an OTIO import's) get their picker values
    if (loadOrigin_ != LoadOrigin::Project)
        refreshMediaMetadata();
    if (gridView()) startClipThumbnails(); // restart thumbnail generation for the loaded clip set

    std::string status;
    switch (loadOrigin_) {
    case LoadOrigin::Project: {
        int clipCount = 0;
        timeline_.forEachClip([&](const Clip&) { ++clipCount; });
        status = "LOADED " + loadSourceLabel_ +
                 "  clips:" + std::to_string(clipCount) +
                 "  shots:" + std::to_string(timeline_.shots.size());
        break;
    }
    case LoadOrigin::Otio:
        status = "IMPORTED " + loadSourceLabel_;
        break;
    case LoadOrigin::Directory:
        status = "CREATED FROM " + loadSourceLabel_ +
                 "  sequences:" + std::to_string(timeline_.sequences.size()) +
                 "  shots:" + std::to_string(timeline_.shots.size());
        break;
    }

    setStatus(status, 3000);
    if (loadOrigin_ == LoadOrigin::Project)
        recordCurrentProject(); // opening also bumps recency

    relocatingMediaId_.clear();
    updateWindowTitle();
    refreshHostSnapshot(); // if hosting a sync session, push the new project to viewers
}

// Styled in-app modal for a missing source: shows the missing path, an editable
// New Directory field with a Browse button, and Relocate / Cancel. Widget rects
// are laid out here each frame; handleRelocateModalEvent hit-tests against them
// (same one-frame-lag pattern as ExportDialog). Draw last.
void App::renderRelocateModal() {
    if (!relocateModalOpen_)
        return;

    const float scale = dpiScale;
    const float dw   = 560.f * scale;
    const float padX = 16.f * scale;
    const float padY = 14.f * scale;
    const float rowH = 22.f * scale;
    const float gap  =  8.f * scale;

    TextFont* font = &textFont_;
    const float glyphH = font->lineHeight();

    // Palette (matches ExportDialog).
    const SDL_Color kOverlay  {   0,   0,   0, 160 };
    const SDL_Color kDialogBg {  30,  31,  37, 255 };
    const SDL_Color kBorder   {  80,  82,  90, 255 };
    const SDL_Color kTitleBg  {  38,  39,  47, 255 };
    const SDL_Color kSeparator{  70,  72,  80, 255 };
    const SDL_Color kTitleText{ 225, 228, 235, 255 };
    const SDL_Color kLabel    { 150, 155, 165, 255 };
    const SDL_Color kPathText { 210, 214, 222, 255 };
    const SDL_Color kErrText  { 220,  80,  80, 255 };

    // Height: title + label + path + field row + (reserved) error line + buttons.
    float innerH = rowH + padY;              // title bar
    innerH += padY + glyphH + gap * .5f;     // "Source not found:" label
    innerH += glyphH + gap * 1.5f;           // path line
    innerH += rowH + gap;                    // directory field row
    innerH += glyphH + gap;                  // inline error line (reserved)
    innerH += rowH + padY;                   // buttons
    const float dh = innerH;

    relocateDialogRect_ = { std::round((winW_ - dw) * .5f),
                            std::round((winH_ - dh) * .5f), dw, dh };
    const SDL_FRect& dlg = relocateDialogRect_;

    auto setC = [&](SDL_Color c) { SDL_SetRenderDrawColor(renderer_, c.r, c.g, c.b, c.a); };

    setC(kOverlay);
    SDL_FRect full{ 0, 0, winW_, winH_ };
    jplay::fillRect(renderer_, &full);
    setC(kDialogBg); jplay::fillRect(renderer_, &dlg);
    setC(kBorder);   jplay::drawRect(renderer_, &dlg);

    SDL_FRect titleBar{ dlg.x, dlg.y, dw, rowH + padY };
    setC(kTitleBg); jplay::fillRect(renderer_, &titleBar);
    setC(kSeparator);
    jplay::drawLine(renderer_, dlg.x, dlg.y + titleBar.h, dlg.x + dw, dlg.y + titleBar.h);
    drawText(dlg.x + padX, dlg.y + (titleBar.h - glyphH) * .5f, kTitleText, "Missing Source");

    // Left-elide a path to fit maxW pixels ("...tail"), keeping the filename visible.
    auto elide = [&](const std::string& s, float maxW) -> std::string {
        if (font->measure(renderer_, s.c_str()) <= maxW)
            return s;
        std::string tail = s;
        while (!tail.empty() &&
               font->measure(renderer_, ("..." + tail).c_str()) > maxW)
            tail.erase(0, 1);
        return "..." + tail;
    };

    float y = dlg.y + titleBar.h + padY;

    auto m = timeline_.findMediaById(relocatingMediaId_);
    std::string path = m ? m->path() : std::string();
    drawText(dlg.x + padX, y, kLabel, "Source not found:");
    y += glyphH + gap * .5f;
    drawText(dlg.x + padX, y, kPathText, elide(path, dw - padX * 2.f).c_str());
    y += glyphH + gap * 1.5f;

    // Directory field + Browse button on its right.
    const float browseW = font->measure(renderer_, "Browse") + 20.f * scale;
    const float fieldX = dlg.x + padX;
    const float fieldW = dw - padX * 2.f - gap - browseW;
    relocateDirInput_.setRect({ fieldX, y, fieldW, rowH });
    relocateBrowseBtn_.setRect({ fieldX + fieldW + gap, y, browseW, rowH });
    relocateBrowseBtn_.setLabel("Browse");
    relocateDirInput_.render(renderer_, font);
    relocateBrowseBtn_.render(renderer_, font);
    y += rowH + gap;

    if (!relocateError_.empty())
        drawText(dlg.x + padX, y, kErrText, relocateError_.c_str());
    y += glyphH + gap;

    // Bottom buttons, right-aligned: Relocate | Cancel.
    const float btnH = rowH;
    const float btnY = dlg.y + dh - padY - btnH;
    struct BtnDef { Button* b; const char* label; };
    BtnDef defs[] = {
        { &relocateBtn_,       "Relocate" },
        { &relocateCancelBtn_, "Cancel" },
    };
    const int nBtn = (int)(sizeof(defs) / sizeof(defs[0]));
    float widths[nBtn], total = 0.f;
    for (int i = 0; i < nBtn; ++i) {
        widths[i] = font->measure(renderer_, defs[i].label) + 20.f * scale;
        total += widths[i];
    }
    total += gap * (nBtn - 1);
    float bx = dlg.x + dw - padX - total;
    for (int i = 0; i < nBtn; ++i) {
        defs[i].b->setRect({ bx, btnY, widths[i], btnH });
        defs[i].b->setLabel(defs[i].label);
        defs[i].b->render(renderer_, font);
        bx += widths[i] + gap;
    }
}

// ---------------------------------------------------------- modal message dialog

// The styled stand-in for SDL_ShowMessageBox: a title, a message and a row of
// buttons, drawn in the app's own chrome instead of the platform's. It cannot
// block, so a caller that used to read the returned button index puts its
// follow-up work in that button's action.
void App::showDialog(std::string title, std::string message,
                     std::vector<DialogButton> buttons, int escIdx, int enterIdx) {
    if (msgDialogOpen_)
        return; // one at a time: a second prompt would silently replace the first

    msgDialogOpen_    = true;
    msgDialogTitle_   = std::move(title);
    msgDialogButtons_ = std::move(buttons);
    msgDialogWidgets_.assign(msgDialogButtons_.size(), Button{});
    msgDialogEsc_     = escIdx;
    msgDialogEnter_   = enterIdx;

    msgDialogLines_.clear();
    for (size_t b = 0;;) {
        size_t e = message.find('\n', b);
        msgDialogLines_.push_back(message.substr(b, e == std::string::npos ? e : e - b));
        if (e == std::string::npos)
            break;
        b = e + 1;
    }
}

void App::setDialogCheckbox(std::string label, bool checked,
                            std::function<void(bool)> onCheck) {
    msgDialogCheckLabel_ = std::move(label);
    msgDialogChecked_    = checked;
    msgDialogOnCheck_    = std::move(onCheck);
    msgDialogCheckRect_  = {};
}

void App::closeDialog() {
    msgDialogOpen_ = false;
    msgDialogTitle_.clear();
    msgDialogLines_.clear();
    msgDialogButtons_.clear();
    msgDialogWidgets_.clear();
    msgDialogEsc_ = msgDialogEnter_ = -1;
    msgDialogCheckLabel_.clear();
    msgDialogChecked_ = false;
    msgDialogOnCheck_ = nullptr;
    msgDialogCheckRect_ = {};
}

// Close before acting: an action may raise the next dialog (Save -> the unsaved
// prompt's own follow-up), and showDialog refuses while one is still up.
void App::runDialogButton(int idx) {
    std::function<void()> action;
    if (idx >= 0 && idx < (int)msgDialogButtons_.size())
        action = std::move(msgDialogButtons_[idx].action);
    closeDialog();
    if (action)
        action();
}

// Enter/Esc pick their nominated buttons; every other event is swallowed so the
// UI underneath stays frozen.
void App::handleDialogEvent(const SDL_Event& e) {
    if (e.type == SDL_EVENT_KEY_DOWN) {
        if (e.key.key == SDLK_ESCAPE) { runDialogButton(msgDialogEsc_); return; }
        if (e.key.key == SDLK_RETURN || e.key.key == SDLK_KP_ENTER) {
            runDialogButton(msgDialogEnter_);
            return;
        }
    }
    if (!msgDialogCheckLabel_.empty() && e.type == SDL_EVENT_MOUSE_BUTTON_DOWN &&
        e.button.button == SDL_BUTTON_LEFT &&
        inRect(msgDialogCheckRect_, e.button.x, e.button.y)) {
        msgDialogChecked_ = !msgDialogChecked_;
        if (msgDialogOnCheck_)
            msgDialogOnCheck_(msgDialogChecked_);
        return;
    }
    for (size_t i = 0; i < msgDialogWidgets_.size(); ++i)
        if (msgDialogWidgets_[i].handleEvent(e)) { runDialogButton((int)i); return; }
}

// Laid out each frame; handleDialogEvent hit-tests the button rects set here on
// the previous frame (same one-frame lag as the other modals). Draw last.
void App::renderDialog() {
    if (!msgDialogOpen_)
        return;

    const float scale   = dpiScale;
    const float padX    = 16.f * scale;
    const float padY    = 14.f * scale;
    const float rowH    = 22.f * scale;
    const float gap     =  8.f * scale;
    const float lineGap =  4.f * scale;

    TextFont* font = &textFont_;
    const float glyphH = font->lineHeight();

    // Palette (matches renderRelocateModal / ExportDialog).
    const SDL_Color kOverlay  {   0,   0,   0, 160 };
    const SDL_Color kDialogBg {  30,  31,  37, 255 };
    const SDL_Color kBorder   {  80,  82,  90, 255 };
    const SDL_Color kTitleBg  {  38,  39,  47, 255 };
    const SDL_Color kSeparator{  70,  72,  80, 255 };
    const SDL_Color kTitleText{ 225, 228, 235, 255 };
    const SDL_Color kMsgText  { 210, 214, 222, 255 };

    // Buttons size to their labels, so unlike the platform box a long one is not
    // clipped. Widths are needed before the dialog width: the row can be wider
    // than the message.
    const size_t nBtn = msgDialogButtons_.size();
    std::vector<float> btnW(nBtn);
    float btnRowW = 0.f;
    for (size_t i = 0; i < nBtn; ++i) {
        btnW[i] = font->measure(renderer_, msgDialogButtons_[i].label.c_str()) + 20.f * scale;
        btnRowW += btnW[i] + (i ? gap : 0.f);
    }

    // The optional checkbox shares the button row, on the left, so it widens the
    // dialog by its own width plus a gap that keeps it clear of the buttons.
    const float chkBox = 13.f * scale;
    const float chkGap =  6.f * scale;
    float chkW = 0.f;
    if (!msgDialogCheckLabel_.empty())
        chkW = chkBox + chkGap + font->measure(renderer_, msgDialogCheckLabel_.c_str());

    float textW = 0.f;
    for (const std::string& l : msgDialogLines_)
        textW = std::max(textW, font->measure(renderer_, l.c_str()));

    const float minW = 320.f * scale;
    const float rowW = btnRowW + (chkW > 0.f ? chkW + gap * 2.f : 0.f);
    float dw = std::max({ textW, rowW, minW - padX * 2.f }) + padX * 2.f;
    dw = std::min(dw, std::max(minW, winW_ - 80.f * scale));

    float dh = rowH + padY;                                              // title bar
    dh += padY;                                                          // top padding
    dh += glyphH * (float)msgDialogLines_.size()
          + lineGap * (float)(msgDialogLines_.size() - 1);               // message lines
    dh += gap * 1.5f;
    dh += rowH + padY;                                                   // button row

    SDL_FRect dlg{ std::round((winW_ - dw) * .5f),
                   std::round((winH_ - dh) * .5f), dw, dh };

    auto setC = [&](SDL_Color c) { SDL_SetRenderDrawColor(renderer_, c.r, c.g, c.b, c.a); };

    setC(kOverlay);
    SDL_FRect full{ 0, 0, winW_, winH_ };
    jplay::fillRect(renderer_, &full);
    setC(kDialogBg); jplay::fillRect(renderer_, &dlg);
    setC(kBorder);   jplay::drawRect(renderer_, &dlg);

    SDL_FRect titleBar{ dlg.x, dlg.y, dw, rowH + padY };
    setC(kTitleBg); jplay::fillRect(renderer_, &titleBar);
    setC(kSeparator);
    jplay::drawLine(renderer_, dlg.x, dlg.y + titleBar.h, dlg.x + dw, dlg.y + titleBar.h);
    drawText(dlg.x + padX, dlg.y + (titleBar.h - glyphH) * .5f, kTitleText, msgDialogTitle_);

    float y = dlg.y + titleBar.h + padY;
    for (const std::string& line : msgDialogLines_) {
        std::string s = line;
        while (s.size() > 1 && font->measure(renderer_, s.c_str()) > dw - padX * 2.f)
            s.erase(s.size() - 1);
        if (!s.empty())
            drawText(dlg.x + padX, y, kMsgText, s);
        y += glyphH + lineGap;
    }

    // Button row, right-aligned along the bottom edge.
    const float btnY = dlg.y + dh - padY - rowH;

    // Checkbox, left-aligned on the same row and vertically centred against it.
    if (chkW > 0.f) {
        const SDL_FRect box{ dlg.x + padX, std::round(btnY + (rowH - chkBox) * .5f),
                             chkBox, chkBox };
        if (msgDialogChecked_) {
            setC(colors().accent);
            jplay::fillRect(renderer_, &box);
            SDL_FRect inner = inset(box, 3.f, 3.f);
            setC(kTitleText);
            jplay::fillRect(renderer_, &inner);
        }
        setC(kBorder);
        jplay::drawRect(renderer_, &box);
        drawText(box.x + chkBox + chkGap, std::round(btnY + (rowH - glyphH) * .5f),
                 kMsgText, msgDialogCheckLabel_);
        msgDialogCheckRect_ = { box.x, box.y, chkW, chkBox };
    }

    float bx = dlg.x + dw - padX - btnRowW;
    for (size_t i = 0; i < nBtn; ++i) {
        msgDialogWidgets_[i].setRect({ bx, btnY, btnW[i], rowH });
        msgDialogWidgets_[i].setLabel(msgDialogButtons_[i].label);
        msgDialogWidgets_[i].render(renderer_, font);
        bx += btnW[i] + gap;
    }
}

// ---------------------------------------------------------- generic progress modal

// Kick off a long action on a background thread with a shared progress channel.
// `work` runs off the main thread (it may hold the GIL and call into Python);
// `onDone` runs on the main thread from pollProgress() once the worker returns,
// so it can safely mutate the timeline / UI. One task at a time.
void App::beginProgress(const std::string& title,
                        std::function<void(ProgressReporter&)> work,
                        std::function<void()> onDone) {
    if (progressRunning_.load(std::memory_order_acquire)) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "[jplay] \"%s\" ignored: \"%s\" is still running",
                    title.c_str(), progressTitle_.c_str());
        return;
    }
    progressTitle_ = title;
    progress_.reset();
    progressStartTick_ = SDL_GetTicks();
    progressOnDone_ = std::move(onDone);
    progressWorkerDone_.store(false, std::memory_order_relaxed);
    progressRunning_.store(true, std::memory_order_release);
    progressThread_ = std::thread([this, work = std::move(work)] {
        work(progress_);
        progressWorkerDone_.store(true, std::memory_order_release);
    });
}

// Called every frame from the run loop: when the worker has finished, join it and
// run the completion on this (main) thread.
void App::pollProgress() {
    if (!progressRunning_.load(std::memory_order_acquire))
        return;
    if (!progressWorkerDone_.load(std::memory_order_acquire))
        return;
    if (progressThread_.joinable())
        progressThread_.join();
    progressRunning_.store(false, std::memory_order_release);
    auto done = std::move(progressOnDone_);
    progressOnDone_ = nullptr;
    if (done)
        done();
}

// Modal input while the dialog is up: Esc or the Cancel button request
// cancellation (the worker observes it at its next checkpoint); everything else
// is swallowed so the underlying UI stays frozen.
void App::handleProgressEvent(const SDL_Event& e) {
    if (e.type == SDL_EVENT_KEY_DOWN && e.key.key == SDLK_ESCAPE) {
        progress_.requestCancel();
        return;
    }
    if (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN && e.button.button == SDL_BUTTON_LEFT &&
        inRect(progressCancelRect_, e.button.x, e.button.y))
        progress_.requestCancel();
}

// Centered modal with a title, the worker's current message, a progress bar
// (indeterminate when the fraction is negative) and a Cancel button. Laid out
// each frame; handleProgressEvent hit-tests progressCancelRect_ (same one-frame
// lag as the other modals). Draw last.
void App::renderProgressOverlay() {
    if (!progressVisible())
        return;

    const float scale = dpiScale;
    const float dw   = 420.f * scale;
    const float padX = 16.f * scale;
    const float padY = 14.f * scale;
    const float rowH = 22.f * scale;
    const float gap  =  8.f * scale;
    const float barH = 10.f * scale;

    TextFont* font = &textFont_;
    const float glyphH = font->lineHeight();

    // Palette (matches renderRelocateModal / ExportDialog).
    const SDL_Color kOverlay  {   0,   0,   0, 160 };
    const SDL_Color kDialogBg {  30,  31,  37, 255 };
    const SDL_Color kBorder   {  80,  82,  90, 255 };
    const SDL_Color kTitleBg  {  38,  39,  47, 255 };
    const SDL_Color kSeparator{  70,  72,  80, 255 };
    const SDL_Color kTitleText{ 225, 228, 235, 255 };
    const SDL_Color kMsgText  { 210, 214, 222, 255 };
    const SDL_Color kTrackBg  {  42,  43,  50, 255 };
    const SDL_Color kFill     {  90, 130, 200, 255 };
    const SDL_Color kBtnText  { 210, 214, 222, 255 };

    // Height: title bar + message line + bar + Cancel button.
    float dh = rowH + padY;            // title bar
    dh += padY + glyphH + gap;         // message line
    dh += barH + gap * 1.5f;           // progress bar
    dh += rowH + padY;                 // Cancel button row

    SDL_FRect dlg{ std::round((winW_ - dw) * .5f),
                   std::round((winH_ - dh) * .5f), dw, dh };

    auto setC = [&](SDL_Color c) { SDL_SetRenderDrawColor(renderer_, c.r, c.g, c.b, c.a); };

    setC(kOverlay);
    SDL_FRect full{ 0, 0, winW_, winH_ };
    jplay::fillRect(renderer_, &full);
    setC(kDialogBg); jplay::fillRect(renderer_, &dlg);
    setC(kBorder);   jplay::drawRect(renderer_, &dlg);

    SDL_FRect titleBar{ dlg.x, dlg.y, dw, rowH + padY };
    setC(kTitleBg); jplay::fillRect(renderer_, &titleBar);
    setC(kSeparator);
    jplay::drawLine(renderer_, dlg.x, dlg.y + titleBar.h, dlg.x + dw, dlg.y + titleBar.h);
    drawText(dlg.x + padX, dlg.y + (titleBar.h - glyphH) * .5f, kTitleText, progressTitle_);

    float y = dlg.y + titleBar.h + padY;

    // Worker message (elided to fit the dialog width). Cancellation is
    // cooperative -- the worker only stops at its next checkpoint, which can be a
    // network round-trip away -- so it is acknowledged here; without that the
    // click reads as having been ignored.
    std::string msg = progress_.cancelled() ? "Cancelling..." : progress_.message();
    if (!msg.empty()) {
        while (msg.size() > 1 && font->measure(renderer_, msg.c_str()) > dw - padX * 2.f)
            msg.erase(msg.size() - 1);
        drawText(dlg.x + padX, y, kMsgText, msg);
    }
    y += glyphH + gap;

    // Progress bar: determinate fill, or a sweeping segment when indeterminate.
    SDL_FRect track{ dlg.x + padX, y, dw - padX * 2.f, barH };
    setC(kTrackBg); jplay::fillRect(renderer_, &track);
    float frac = progress_.fraction();
    if (frac >= 0.f) {
        SDL_FRect fill{ track.x, track.y, track.w * std::clamp(frac, 0.f, 1.f), track.h };
        setC(kFill); jplay::fillRect(renderer_, &fill);
    } else {
        const float period = 1200.f;
        float t = (SDL_GetTicks() % (uint64_t)period) / period;
        float segW = track.w * 0.3f;
        float sx = track.x + (track.w + segW) * t - segW;
        float x0 = std::max(sx, track.x);
        float x1 = std::min(sx + segW, track.x + track.w);
        if (x1 > x0) { SDL_FRect seg{ x0, track.y, x1 - x0, track.h }; setC(kFill); jplay::fillRect(renderer_, &seg); }
    }
    setC(kBorder); jplay::drawRect(renderer_, &track);
    y += barH + gap * 1.5f;

    // Cancel button, right-aligned.
    const float btnW = font->measure(renderer_, "Cancel") + 20.f * scale;
    progressCancelRect_ = { dlg.x + dw - padX - btnW, dlg.y + dh - padY - rowH, btnW, rowH };
    float mx, my;
    uiMouse(mx, my);
    bool hover = inRect(progressCancelRect_, mx, my);
    setC(hover ? kTitleBg : kTrackBg); jplay::fillRect(renderer_, &progressCancelRect_);
    setC(kBorder); jplay::drawRect(renderer_, &progressCancelRect_);
    drawText(progressCancelRect_.x + (btnW - font->measure(renderer_, "Cancel")) * .5f,
             progressCancelRect_.y + (rowH - glyphH) * .5f, kBtnText, "Cancel");
}

// ---------------------------------------------------------------- recent projects

void App::recordCurrentProject() {
    if (!projectHasPath_)
        return; // never-saved scratch project: nothing stable to record
    UserData::RecentProject rp;
    std::error_code ec;
    fs::path abs = fs::absolute(projectPath_, ec);
    rp.path = ec ? projectPath_ : abs.string();
    rp.id = projectId_;
    rp.savedUnix = (int64_t)std::time(nullptr);
    UserData::recordRecent(rp);
    recentDirty_ = true;
}

void App::openRecentProject(const std::string& path) {
    confirmDiscard("Open Project", [this, path] { loadProject(path); });
}

void App::clearRecentTiles() {
    for (auto& t : recent_)
        if (t.tex)
            SDL_DestroyTexture(t.tex);
    recent_.clear();
}

namespace {
// The CPU-side result of loading one recent entry off the UI thread: the
// decoded thumbnail lives in `rgba` until the main thread turns it into an SDL
// texture (texture creation must happen on the render thread).
struct LoadedRecent {
    std::string path;
    std::string id;
    std::string label;
    int64_t savedUnix = 0;
    std::vector<uint8_t> rgba; // empty when there is no thumbnail
    int w = 0, h = 0;
};

// Format a recorded save/open time for the recent list, e.g. "Jul 03, 2026 14:32".
// Returns empty for legacy entries with no timestamp.
std::string formatSavedTime(int64_t unixTime) {
    if (unixTime <= 0)
        return std::string();
    std::time_t tt = (std::time_t)unixTime;
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &tt);
#else
    localtime_r(&tt, &tm);
#endif
    char buf[64];
    std::strftime(buf, sizeof(buf), "%b %d, %Y  %H:%M", &tm);
    return buf;
}
} // namespace

// Reload the recent-projects list on the work queue: the disk I/O (reading the
// recent list and decoding each thumbnail) runs on a pool thread; the completion
// callback runs on the main thread, where it builds the SDL textures and swaps
// them into recent_ for renderMainPanels() to draw.
void App::submitRecentRefresh() {
    auto out = std::make_shared<std::vector<LoadedRecent>>();
    work_.submit(
        [out](const std::atomic<bool>& stop) {
            for (const auto& e : UserData::loadRecent()) {
                if (stop.load())
                    return; // project switched / quitting: abandon
                LoadedRecent lr;
                lr.path = e.path;
                lr.id = e.id;
                lr.label = fs::path(e.path).filename().string(); // basename as stored
                lr.savedUnix = e.savedUnix;
                Project::Thumbnail th;
                if (!e.id.empty() && UserData::readThumbnail(e.id, e.path, th) && th.valid()) {
                    lr.rgba = std::move(th.rgba);
                    lr.w = th.width;
                    lr.h = th.height;
                }
                out->push_back(std::move(lr));
            }
        },
        [this, out] {
            clearRecentTiles();
            for (auto& lr : *out) {
                RecentTile t;
                t.path = std::move(lr.path);
                t.id = std::move(lr.id);
                t.label = std::move(lr.label);
                if (std::string ts = formatSavedTime(lr.savedUnix); !ts.empty())
                    t.lastOpened = "Last Opened " + ts;
                if (!lr.rgba.empty() && lr.w > 0 && lr.h > 0) {
                    t.tex = SDL_CreateTexture(renderer_, SDL_PIXELFORMAT_RGBA32,
                                              SDL_TEXTUREACCESS_STATIC, lr.w, lr.h);
                    if (t.tex) {
                        SDL_SetTextureScaleMode(t.tex, SDL_SCALEMODE_LINEAR);
                        SDL_UpdateTexture(t.tex, nullptr, lr.rgba.data(), lr.w * 4);
                        t.texW = lr.w;
                        t.texH = lr.h;
                    }
                }
                recent_.push_back(std::move(t));
            }
        });
}

// ---------------------------------------------------------------- shared projects

// Populate the SHARED PROJECTS column from the studio shows-config. Parsing runs
// on a pool thread (SharedProjects::parse); its completion fans out one probe job
// per show (submitSharedProbe), so shows drop into the column individually as each
// confirms its project file exists rather than all at once at the end. Prefs keys
// (under [shared_projects]): projects_ini_config (path to the shows list) and
// project_path (a template with a {project} placeholder, resolving to a .otio or
// a .jpproj). Opening a project calls work_.reset(), which cancels any
// pending/in-flight probes cleanly.
void App::submitSharedRefresh() {
    sharedProjects_.clear(); // fresh scan: shows drop back in as they confirm
    auto entries = std::make_shared<std::vector<SharedProjects::Entry>>();
    work_.submit(
        [entries](const std::atomic<bool>&) {
            std::string iniPath = Preferences::get("shared_projects", "projects_ini_config");
            std::string pathTmpl = Preferences::get("shared_projects", "project_path");
            if (iniPath.empty() || pathTmpl.empty())
                return;
            *entries = SharedProjects::parse(iniPath, pathTmpl);
        },
        [this, entries] {
            // The probes run independently, but the column is filled in a single
            // swap once the last of them reports: appending show by show would
            // grow the column (and move its rows under the pointer) N times.
            auto pending = std::make_shared<std::vector<SharedProject>>();
            auto outstanding = std::make_shared<int>((int)entries->size());
            for (auto& e : *entries)
                submitSharedProbe(std::move(e), pending, outstanding);
        });
}

// Check one show's project file on the work queue; if it exists, append it to the column.
// The existence stat is guarded by a wall-clock budget and polls the stop flag, so
// a hung mount can't stall the pool and work_.reset() (on project open) releases it
// promptly — its completion is then discarded, so nothing is appended after reset.
void App::submitSharedProbe(SharedProjects::Entry entry,
                            std::shared_ptr<std::vector<SharedProject>> pending,
                            std::shared_ptr<int> outstanding) {
    auto e = std::make_shared<SharedProjects::Entry>(std::move(entry));
    auto present = std::make_shared<bool>(false);
    work_.submit(
        [e, present](const std::atomic<bool>& stop) {
            *present = SharedProjects::exists(e->path, 4000, stop);
        },
        [this, e, present, pending, outstanding] {
            // Completions run on the main thread, so the counter needs no atomics.
            if (*present)
                pending->push_back(SharedProject{ e->code, e->name, e->path });
            if (--*outstanding == 0)
                sharedProjects_ = std::move(*pending);
        });
}

// Bring in a shared project. The extension picks the loader, exactly as for a
// dropped file or an Open: a .jpproj loads, anything else imports as an .otio.
// Either way the `shared` flag leaves the project unsaved (projectHasPath_ =
// false, projectPath_ reset), so the shared path is never made the current file,
// Save prompts for a new location, and leaving it again does not prompt over
// that permanent unsaved state (see projectFromShared_).
void App::openSharedProject(const std::string& path) {
    confirmDiscard("Open Shared Project", [this, path] {
        if (hasExtension(path, ".jpproj"))
            loadProject(path, true);
        else
            loadOtio(path, true);
    });
}

// Launcher drawn over the empty player: two side-by-side columns, centred in the
// player at a fixed size so nothing ever moves under the cursor. The right one is
// CREATE PROJECT (action buttons duplicating what the File menu carries). The left
// one is a tabbed list: RECENT PROJECTS (thumbnail rows), SHARED PROJECTS and
// SYNC. All three tabs are always in the strip; the two filled by background scans
// draw faded and take no click until their scan reports (and SYNC fades back out
// when its beacons time out), so a scan landing mid-launch lights a tab up instead
// of moving anything. Every row, button and live tab is clickable (see
// handleEvent); their hit rects are stashed here.
// The column's tabs, in strip order. The labels live out here because the column
// has to be sized wide enough for the whole strip before anything is drawn.
static const char* const kLauncherTabLabels[3] = {
    "RECENT PROJECTS", "SHARED PROJECTS", "SYNC SESSION",
};

void App::renderMainPanels() {
    if (recentDirty_) {
        // Kick the reload onto the work queue; the tiles populate when its
        // completion callback runs on the main thread (a frame or more later).
        // Clear the flag now so we submit exactly one job, not one per frame.
        recentDirty_ = false;
        submitRecentRefresh();
    }
    if (sharedDirty_) {
        // Kick the studio-shows scan once; results populate the SHARED PROJECTS
        // column when the background job's completion callback runs (see
        // submitSharedRefresh). Non-blocking, like the SYNC SESSION discovery.
        sharedDirty_ = false;
        submitSharedRefresh();
    }

    float mx = 0.0f, my = 0.0f;
    uiMouse(mx, my);

    // Refilled below by whichever tab draws a scrollbar; the other tab's stays
    // empty, so a press can be offered to both without testing the tab.
    recentSb_.bar = {};
    sharedSb_.bar = {};

    const float headerH = 26.0f;
    const float colGap = 48.0f;

    // ---- CREATE PROJECT section geometry: the action buttons
    // OPEN PROJECT takes both project kinds (.jpproj and .otio), so there is no
    // separate import button. Ordered by how often each is what the user came
    // for: opening an existing project first, and CREATE EMPTY — which really
    // just dismisses the launcher onto a blank timeline — last.
    const char* btnLabels[4] = {
        "Open Project",
        "Import Media",
        "Create From Directory",
        "Create Empty Project",
    };
    SDL_FRect* btnRects[4] = { &openProjectBtn_, &importMediaBtn_, &createFromDirBtn_, &createEmptyBtn_ };
    // Glyph in front of each label (ICON_MDI_FOLDER_OPEN, ICON_MDI_FILE_IMPORT,
    // ICON_MDI_FOLDER_PLUS, ICON_MDI_FILE_PLUS) — names listed so the font
    // subsetter keeps them; drawn by codepoint below.
    const uint32_t btnIcons[4] = { 0xF0257, 0xF0220, 0xF0770, 0xF0752 };
    const float btnH = 44.0f;
    const float btnGap = 10.0f;
    float btnW = 260.0f; // widened below to fit the longest label (+ icon slot)
    for (const char* l : btnLabels)
        btnW = std::max(btnW, textFont_.measure(renderer_, l) + btnH + 22.0f);
    float createColH = headerH + 4 * btnH + 3 * btnGap;

    // ---- RECENT PROJECTS section geometry: the thumbnail list
    // Rows match the CREATE PROJECT buttons: same visible height and gap, so
    // rowH is the per-row stride (button height + gap).
    const float rowDrawH = btnH;          // visible row height
    const float rowGap = btnGap;          // gap below each row
    const float rowH = rowDrawH + rowGap; // stride between rows
    const float pad = 8.0f;
    const float thumbH = rowDrawH - 2.0f * pad;
    const float thumbW = thumbH * 16.0f / 9.0f;   // a 16:9 slot; the image is letterboxed inside
    // Size the list to the widest tile rather than a fixed fraction of the window,
    // so short project names don't leave a very wide column. Row layout is
    // pad | thumb | 14 | text | 12 | close(16) | pad; the "Last Opened" subline is
    // drawn at 0.8 scale. Clamped so it never gets too narrow or absurdly wide.
    const float kCloseSz = 16.0f;
    // Seeded with the width of a reference "Last Opened" subline: every populated
    // row carries one, so reserving it up front keeps the column the same width
    // before and after the (async) tiles land, instead of widening — and shifting
    // the core columns — a frame or two into the launch.
    float maxTextW = textFont_.measure(renderer_, "Last Opened Jan 01, 2026  00:00") * 0.8f;
    for (const auto& t : recent_) {
        maxTextW = std::max(maxTextW, textFont_.measure(renderer_, t.label.c_str()));
        if (!t.lastOpened.empty())
            maxTextW = std::max(maxTextW, textFont_.measure(renderer_, t.lastOpened.c_str()) * 0.8f);
    }
    // Show at most 4 rows; if there are more the list scrolls (recentScroll_).
    const int kMaxVisibleRows = 4;
    // An overflowing list gets a grabbable scrollbar down the right edge of the
    // viewport (see scrollbarStripW). Reserve that width in the column so the bar
    // sits beside the rows instead of over their right edge and close cross.
    const float sbW = (int)recent_.size() > kMaxVisibleRows
                          ? scrollbarStripW(dpiScale, true) : 0.0f;
    float neededW = pad + thumbW + 14.0f + maxTextW + 12.0f + kCloseSz + pad + sbW;
    // The tab strip is always all three tabs wide (the two scanned ones show
    // disabled rather than absent), so the column can never be narrower than it —
    // a constant, so this doesn't move anything either.
    const float tabGap = 4.0f;
    const float tabH = headerH - 4.0f; // also the CREATE PROJECT header's height
    float stripW = -tabGap;
    for (const char* l : kLauncherTabLabels)
        stripW += textFont_.measure(renderer_, l) + 2.0f * pad + tabGap;
    const float listW = std::max(std::clamp(neededW, 260.0f, 520.0f), stripW);
    const float rowW = listW - sbW;   // rows stop short of the scrollbar
    int count = std::min((int)recent_.size(), kMaxVisibleRows);

    // ---- the two scanned tabs. Both lists are filled by background scans, so each
    // tab goes live only once it has something to offer: host beacons arriving (or
    // timing out — discovery runs while the launcher is up, see drainSync), and the
    // studio-shows scan reporting (see submitSharedRefresh). Until then the tab is
    // still in the strip, drawn faded and taking no click.
    auto beacons = syncSession_.beacons();
    launcherSyncRows_.clear();
    const bool showSync = !beacons.empty();
    const bool showShared = !sharedProjects_.empty();

    launcherTabs_.clear();
    launcherTabs_.push_back({ LauncherTab::Recent, kLauncherTabLabels[0], true, {} });
    launcherTabs_.push_back({ LauncherTab::Shared, kLauncherTabLabels[1], showShared, {} });
    launcherTabs_.push_back({ LauncherTab::Sync, kLauncherTabLabels[2], showSync, {} });
    // A tab that lost its data while it was the active one (a beacon list timing
    // out) hands the column back to RECENT rather than showing nothing.
    bool tabLive = false;
    for (const auto& t : launcherTabs_)
        tabLive = tabLive || (t.tab == launcherTab_ && t.enabled);
    if (!tabLive)
        launcherTab_ = LauncherTab::Recent;

    // ---- block geometry. The tabbed list column and CREATE PROJECT are the whole
    // launcher, and both are always there: the pair is centred in the player and
    // never moves, whatever the scans report and whichever tab is up.
    const float coreW = listW + colGap + btnW;
    // Height is a constant — a full kMaxVisibleRows list — so neither rows arriving
    // as the async scans report nor a tab switch drifts the block vertically.
    const float blockH = std::max(createColH, headerH + kMaxVisibleRows * rowH);
    const float by = std::floor(playerRect_.y + (playerRect_.h - blockH) * 0.5f);
    const float xRecent = std::floor(playerRect_.x + (playerRect_.w - coreW) * 0.5f);
    const float xCreate = xRecent + listW + colGap;

    // ===== section: CREATE PROJECT (the right column)
    // The header is drawn as a chip in the same style the list column's active tab
    // gets, so the two columns read as headed the same way. It isn't clickable —
    // there is only one — so it never takes the inactive or hover palette.
    {
        SDL_FRect r{ xCreate, by, textFont_.measure(renderer_, "CREATE PROJECT") + 2.0f * pad, tabH };
        SDL_SetRenderDrawColor(renderer_, kUiBtnBgHover.r, kUiBtnBgHover.g, kUiBtnBgHover.b, 220);
        jplay::fillRect(renderer_, &r);
        SDL_SetRenderDrawColor(renderer_, kUiBtnBorderHover.r, kUiBtnBorderHover.g,
                               kUiBtnBorderHover.b, kUiBtnBorderHover.a);
        jplay::drawRect(renderer_, &r);
        drawText(r.x + pad, r.y + (tabH - textFont_.lineHeight()) * 0.5f,
                 { 215, 215, 225, 255 }, "CREATE PROJECT");
    }
    float cy = by + headerH;
    for (int i = 0; i < 4; ++i) {
        SDL_FRect b{ xCreate, cy, btnW, btnH };
        *btnRects[i] = b;
        bool hover = inRect(b, mx, my);
        // 220: translucent over the video.
        {
            SDL_Color bg = hover ? kUiBtnBgHover : kUiBtnBg;
            SDL_Color border = hover ? kUiBtnBorderHover : kUiBtnBorder;
            SDL_SetRenderDrawColor(renderer_, bg.r, bg.g, bg.b, 220);
            jplay::fillRect(renderer_, &b);
            SDL_SetRenderDrawColor(renderer_, border.r, border.g, border.b, border.a);
            jplay::drawRect(renderer_, &b); // 1px lighter border
        }
        SDL_FRect iconR{ b.x, b.y, btnH, btnH };
        icons_.drawGlyph(renderer_, btnIcons[i], iconR, { 200, 205, 215, 255 }, 0.30f);
        drawText(b.x + btnH, b.y + (btnH - textFont_.lineHeight()) * 0.5f, { 215, 215, 225, 255 }, btnLabels[i]);
        cy += btnH + btnGap;
    }

    // ===== the list column's header: the tab strip.
    const float viewTop = by + headerH;
    {
        float tx = xRecent;
        for (auto& tab : launcherTabs_) {
            SDL_FRect r{ tx, by, textFont_.measure(renderer_, tab.label) + 2.0f * pad, tabH };
            tab.rect = r;
            const bool active = tab.tab == launcherTab_;
            const bool hover = tab.enabled && !active && inRect(r, mx, my);
            { // same palette as the rows below, so the strip reads as part of them
                SDL_Color bg = (active || hover) ? kUiBtnBgHover : kUiBtnBg;
                SDL_Color border = active ? kUiBtnBorderHover : kUiBtnBorder;
                // A tab with no data yet keeps its place but recedes: half the fill
                // and border a live one gets, so the strip says the list isn't there
                // yet rather than hiding that the tab exists.
                SDL_SetRenderDrawColor(renderer_, bg.r, bg.g, bg.b, tab.enabled ? 220 : 110);
                jplay::fillRect(renderer_, &r);
                SDL_SetRenderDrawColor(renderer_, border.r, border.g, border.b,
                                       tab.enabled ? border.a : (Uint8)(border.a / 2));
                jplay::drawRect(renderer_, &r);
            }
            SDL_Color fg = !tab.enabled ? SDL_Color{ 25, 25, 35, 255 }
                         : active       ? SDL_Color{ 215, 215, 225, 255 }
                                        : SDL_Color{ 150, 150, 160, 255 };
            drawText(r.x + pad, r.y + (tabH - textFont_.lineHeight()) * 0.5f, fg, tab.label);
            tx += r.w + tabGap;
        }
    }

    // Only the active tab draws its rows, so the other lists give up their hit
    // rects: a stale rect would keep taking clicks from under the tab that
    // replaced it. (launcherSyncRows_ is cleared above and only refilled below.)
    if (launcherTab_ != LauncherTab::Recent) {
        recentListRect_ = SDL_FRect{};
        for (auto& t : recent_) {
            t.rect = SDL_FRect{};
            t.closeRect = SDL_FRect{};
        }
    }
    if (launcherTab_ != LauncherTab::Shared) {
        sharedListRect_ = SDL_FRect{};
        for (auto& sp : sharedProjects_)
            sp.rect = SDL_FRect{};
    }
    if (launcherTab_ != LauncherTab::Sync)
        launcherSyncListRect_ = SDL_FRect{};

    // ===== tab: SHARED PROJECTS (studio shows with a project file on disk)
    // A click brings the show in as an unsaved project.
    if (launcherTab_ == LauncherTab::Shared) {
        const int sharedCount = std::min((int)sharedProjects_.size(), kMaxVisibleRows);
        const float viewH = sharedCount * rowH;
        sharedListRect_ = SDL_FRect{ xRecent, viewTop, listW, viewH };
        float maxScroll = std::max(0.0f, (float)sharedProjects_.size() * rowH - viewH);
        sharedScroll_ = std::clamp(sharedScroll_, 0.0f, maxScroll);
        // An overflowing list gets a grabbable scrollbar down the right edge of the
        // viewport (see scrollbarStripW). Keep the rows short of it so the bar sits
        // beside them instead of over their label.
        const float shSbW = maxScroll > 0.0f ? scrollbarStripW(dpiScale, true) : 0.0f;
        const float shRowW = listW - shSbW;

        SDL_Rect clip{ (int)xRecent, (int)viewTop, (int)listW, (int)viewH };
        SDL_SetRenderClipRect(renderer_, &clip);
        for (int i = 0; i < (int)sharedProjects_.size(); ++i) {
            SharedProject& sp = sharedProjects_[i];
            float y = viewTop + i * rowH - sharedScroll_;
            if (y + rowH <= viewTop || y >= viewTop + viewH) {
                sp.rect = SDL_FRect{}; // scrolled out of view — not clickable
                continue;
            }
            SDL_FRect r{ xRecent, y, shRowW, btnH };
            bool fullyVisible = y >= viewTop - 0.5f && y + btnH <= viewTop + viewH + 0.5f;
            sp.rect = fullyVisible ? r : SDL_FRect{};
            bool hover = fullyVisible && inRect(r, mx, my);
            { // translucent over the video
                SDL_Color bg = hover ? kUiBtnBgHover : kUiBtnBg;
                SDL_Color border = hover ? kUiBtnBorderHover : kUiBtnBorder;
                SDL_SetRenderDrawColor(renderer_, bg.r, bg.g, bg.b, 220);
                jplay::fillRect(renderer_, &r);
                SDL_SetRenderDrawColor(renderer_, border.r, border.g, border.b, border.a);
                jplay::drawRect(renderer_, &r);
            }
            SDL_FRect iconR{ r.x, r.y, btnH, btnH };
            icons_.drawGlyph(renderer_, 0xF0770, iconR, // ICON_MDI_FOLDER_OPEN
                             { 200, 205, 215, 255 }, 0.30f);
            std::string label = sp.code + " - " + sp.name;
            drawText(r.x + btnH, r.y + (btnH - textFont_.lineHeight()) * 0.5f,
                     { 215, 215, 225, 255 },
                     fitText(label, shRowW - btnH - 8.0f).c_str());
        }
        SDL_SetRenderClipRect(renderer_, nullptr);

        // Scrollbar on the right edge of the viewport, only when it overflows.
        drawScrollbar(renderer_, sharedListRect_, (float)sharedProjects_.size() * rowH,
                      sharedScroll_, dpiScale, true, sharedSb_);
    }

    // ===== tab: SYNC SESSION (one row per live LAN host beacon; a click joins)
    if (launcherTab_ == LauncherTab::Sync) {
        const int syncCount = std::min((int)beacons.size(), kMaxVisibleRows);
        const float viewH = syncCount * rowH;
        launcherSyncListRect_ = SDL_FRect{ xRecent, viewTop, listW, viewH };
        float maxScroll = std::max(0.0f, (float)beacons.size() * rowH - viewH);
        launcherSyncScroll_ = std::clamp(launcherSyncScroll_, 0.0f, maxScroll);
        const float syncSbW = maxScroll > 0.0f ? 6.0f * dpiScale : 0.0f;
        const float syncRowW = listW - syncSbW;

        SDL_Rect clip{ (int)xRecent, (int)viewTop, (int)listW, (int)viewH };
        SDL_SetRenderClipRect(renderer_, &clip);
        for (int i = 0; i < (int)beacons.size(); ++i) {
            const auto& b = beacons[i];
            float y = viewTop + i * rowH - launcherSyncScroll_;
            if (y + rowH <= viewTop || y >= viewTop + viewH)
                continue; // scrolled out of view — and so not joinable
            SDL_FRect r{ xRecent, y, syncRowW, btnH };
            bool fullyVisible = y >= viewTop - 0.5f && y + btnH <= viewTop + viewH + 0.5f;
            bool hover = fullyVisible && inRect(r, mx, my);
            { // translucent over the video
                SDL_Color bg = hover ? kUiBtnBgHover : kUiBtnBg;
                SDL_Color border = hover ? kUiBtnBorderHover : kUiBtnBorder;
                SDL_SetRenderDrawColor(renderer_, bg.r, bg.g, bg.b, 220);
                jplay::fillRect(renderer_, &r);
                SDL_SetRenderDrawColor(renderer_, border.r, border.g, border.b, border.a);
                jplay::drawRect(renderer_, &r);
            }
            SDL_FRect iconR{ r.x, r.y, btnH, btnH };
            icons_.drawGlyph(renderer_, 0xF0849, iconR, // ICON_MDI_ACCOUNT_GROUP
                             { 200, 205, 215, 255 }, 0.34f);
            // Host name on top, dimmer "hostname (ip)" beneath it.
            std::string name = b.username.empty() ? b.hostname : b.username;
            const float lh = textFont_.lineHeight();
            const float subScale = 0.8f;
            float blockTextH = lh + lh * subScale;
            float ty = r.y + (btnH - blockTextH) * 0.5f;
            drawText(r.x + btnH, ty, { 215, 215, 225, 255 },
                     fitText(name, syncRowW - btnH - 8.0f).c_str());
            std::string sub = b.hostname.empty() ? b.ip : (b.hostname + "  (" + b.ip + ")");
            textFont_.draw(renderer_, r.x + btnH, ty + lh, { 130, 130, 140, 255 },
                           fitText(sub, syncRowW - btnH - 8.0f).c_str(), subScale);
            if (fullyVisible)
                launcherSyncRows_.push_back({ b.ip, b.tcpPort, r });
        }
        SDL_SetRenderClipRect(renderer_, nullptr);
        drawScrollbar(renderer_, launcherSyncListRect_, (float)beacons.size() * rowH,
                      launcherSyncScroll_, dpiScale, true);
    }

    // ===== tab: RECENT PROJECTS
    if (launcherTab_ != LauncherTab::Recent)
        return;
    if (recent_.empty()) {
        drawText(xRecent + 2.0f, viewTop + 4.0f, { 120, 120, 130, 255 }, "NO RECENT PROJECTS");
        return;
    }

    // Fixed-height viewport of up to kMaxVisibleRows; extra rows scroll through
    // it (recentScroll_, driven by the wheel in handleEvent). Clip so a
    // partially-scrolled row peeks in/out rather than overflowing the block.
    const float viewH = count * rowH;
    recentListRect_ = SDL_FRect{ xRecent, viewTop, listW, viewH };
    float maxScroll = std::max(0.0f, (float)recent_.size() * rowH - viewH);
    recentScroll_ = std::clamp(recentScroll_, 0.0f, maxScroll);

    SDL_Rect listClip{ (int)xRecent, (int)viewTop, (int)listW, (int)viewH };
    SDL_SetRenderClipRect(renderer_, &listClip);

    for (int i = 0; i < (int)recent_.size(); ++i) {
        if(i > 20){
            // dont show more than 20 recent projects
            break;
        }
        RecentTile& t = recent_[i];
        float y = viewTop + i * rowH - recentScroll_;
        if (y + rowH <= viewTop || y >= viewTop + viewH) {
            t.rect = SDL_FRect{};      // scrolled out of view — not clickable
            t.closeRect = SDL_FRect{};
            continue;
        }
        SDL_FRect row{ xRecent, y, rowW, rowDrawH };
        // Only act on fully-visible rows so a clipped peek row isn't clickable.
        bool fullyVisible = y >= viewTop - 0.5f && y + rowDrawH <= viewTop + viewH + 0.5f;
        t.rect = fullyVisible ? row : SDL_FRect{};
        bool hover = fullyVisible && inRect(row, mx, my);

        // Same palette as the CREATE PROJECT / SHARED / SYNC rows — these are
        // launcher buttons too, they just carry a thumbnail. 220: translucent
        // over the video.
        {
            SDL_Color bg = hover ? kUiBtnBgHover : kUiBtnBg;
            SDL_Color border = hover ? kUiBtnBorderHover : kUiBtnBorder;
            SDL_SetRenderDrawColor(renderer_, bg.r, bg.g, bg.b, 220);
            jplay::fillRect(renderer_, &row);
            SDL_SetRenderDrawColor(renderer_, border.r, border.g, border.b, border.a);
            jplay::drawRect(renderer_, &row);
        }

        // Remove-from-recents cross on the right edge of the row.
        const float closeSz = 16.0f;
        SDL_FRect close{ row.x + row.w - closeSz - pad,
                         row.y + (row.h - closeSz) * 0.5f, closeSz, closeSz };
        t.closeRect = fullyVisible ? close : SDL_FRect{};
        bool closeHover = fullyVisible && inRect(close, mx, my);
        {
            float cp = 4.0f; // inset of the X strokes within the box
            Uint8 c = closeHover ? 235 : 150;
            SDL_SetRenderDrawColor(renderer_, c, c, c, 255);
            jplay::drawLine(renderer_, close.x + cp, close.y + cp,
                           close.x + close.w - cp, close.y + close.h - cp);
            jplay::drawLine(renderer_, close.x + close.w - cp, close.y + cp,
                           close.x + cp, close.y + close.h - cp);
        }

        SDL_FRect slot{ row.x + pad, row.y + pad, thumbW, thumbH };
        if (t.tex && t.texW > 0 && t.texH > 0) {
            float sc = std::min(slot.w / (float)t.texW, slot.h / (float)t.texH);
            float dw = t.texW * sc, dh = t.texH * sc;
            SDL_FRect dst{ slot.x + (slot.w - dw) * 0.5f, slot.y + (slot.h - dh) * 0.5f, dw, dh };
            SDL_RenderTexture(renderer_, t.tex, nullptr, &dst);
        } else {
            SDL_SetRenderDrawColor(renderer_, 22, 22, 26, 255);
            jplay::fillRect(renderer_, &slot);
            //drawText(slot.x + 8.0f, slot.y + slot.h * 0.5f - 4.0f, { 90, 90, 100, 255 }, "NO PREVIEW");
        }

        float textX = slot.x + slot.w + 14.0f;
        if (t.lastOpened.empty()) {
            drawText(textX, row.y + rowDrawH * 0.5f - 4.0f,
                     { 215, 215, 225, 255 }, t.label);
        } else {
            // Basename on top, dimmer "Last Opened <date>" beneath it.
            const float lh = textFont_.lineHeight();
            const float dateScale = 0.8f;
            const float blockTextH = lh + lh * dateScale;
            float ty = row.y + (rowDrawH - blockTextH) * 0.5f;
            drawText(textX, ty, { 215, 215, 225, 255 }, t.label);
            textFont_.draw(renderer_, textX, ty + lh, { 130, 130, 140, 255 },
                           t.lastOpened.c_str(), dateScale);
        }
    }
    SDL_SetRenderClipRect(renderer_, nullptr);

    // Scrollbar on the right edge of the viewport, only when the list overflows.
    drawScrollbar(renderer_, recentListRect_, (float)recent_.size() * rowH,
                  recentScroll_, dpiScale, true, recentSb_);
}

// ---------------------------------------------------------------- metadata refresh

// Parallel, hash-gated refresh of cached media metadata. Spawns a small worker
// pool over a snapshot of the media pool; each worker re-probes only media whose
// on-disk fingerprint has changed (and so also flags media that have gone
// missing). Fresh media are left untouched, doing zero work.
void App::refreshMediaMetadata() {
    joinRefresh();
    std::vector<std::shared_ptr<Media>> pool;
    pool.reserve(timeline_.media.size());
    for (const auto& kv : timeline_.media)
        pool.push_back(kv.second);
    if (pool.empty())
        return;
    auto next = std::make_shared<std::atomic<size_t>>(0);
    int n = std::clamp((int)std::thread::hardware_concurrency() - 1, 1, 8);
    n = std::min<int>(n, (int)pool.size());
    for (int t = 0; t < n; ++t) {
        refreshThreads_.emplace_back([this, pool, next] {
            for (;;) {
                if (refreshStop_.load())
                    return;
                size_t i = next->fetch_add(1);
                if (i >= pool.size())
                    return;
                // Metadata an importer already supplied from a verified sidecar: the
                // open this would cost buys nothing (see Media::infoTrusted).
                if (pool[i]->infoTrusted())
                    continue;
                if (!pool[i]->fresh())
                    pool[i]->refreshMetadata();
            }
        });
    }
}

// Fill in the naming-convention metadata (department, asset, version, ... — the
// naming config's picker keys) of media that carry none, so the timeline clip's
// second row has something to draw (see clipMetadataTemplate). Every other way
// media enters a project tags it as it goes — addMediaFile, the clip media swap,
// CREATE FROM DIRECTORY — and a .jpproj restores what it saved, so in practice
// this is the OTIO import path: OtioImport builds each Media from the file's
// target_url alone and knows nothing of the naming config.
//
// Media already carrying metadata are skipped, which makes this a no-op for the
// other load paths rather than a second round of queries.
//
// The values come from Python (the naming config's regexes, and any site override
// of them, live there), so the query runs off the main thread — one task for the
// whole pool, taking the GIL once — and the completion writes the metadata back on
// the main thread, the only thread that writes Media's metadata map. Python is
// started from run(), after init() has already loaded a command-line .otio, so
// with the interpreter not yet up the pass defers itself to the run loop instead.
void App::tagMediaPathValues() {
    pendingPathValueTag_ = false;
    std::vector<std::string> ids, paths;
    for (const auto& kv : timeline_.media) {
        if (!kv.second || !kv.second->meta().empty())
            continue;
        ids.push_back(kv.first);
        // The naming convention wants a real frame number, not a "####" placeholder.
        // An .otio whose media wasn't verified on disk keeps its frame-pattern path and
        // supplies a hint instead — a plausible frame for parsing only, which costs no
        // open (see Media::namingPathHint). Everything else names a concrete frame
        // already, bar a "####" command-line argument, where an opened sequence
        // contributes its first frame via resolvedPath().
        const std::string& hint = kv.second->namingPathHint();
        paths.push_back(hint.empty() ? kv.second->resolvedPath() : hint);
    }
    if (paths.empty())
        return;
    if (!jplayPythonReady()) {
        pendingPathValueTag_ = true;
        return;
    }
    auto values = std::make_shared<std::vector<std::map<std::string, std::string>>>();
    // pickerWork_ rather than work_: work_ is held while media plays, and pressing
    // play right after an import would otherwise leave the clips unlabelled for as
    // long as playback lasts. This is one short task, so it neither delays nor is
    // delayed by the picker clicks that queue there.
    pickerWork_.submit(
        [paths, values](const std::atomic<bool>&) { jplayGetPathValues(paths, *values); },
        [this, ids, values] {
            for (size_t i = 0; i < ids.size() && i < values->size(); ++i) {
                auto m = timeline_.findMediaById(ids[i]);
                if (!m)
                    continue; // media dropped while the query was in flight
                for (auto& kv : (*values)[i])
                    m->setMetaValue(kv.first, std::move(kv.second));
            }
        });
}

void App::joinRefresh() {
    refreshStop_ = true;
    for (auto& t : refreshThreads_)
        if (t.joinable())
            t.join();
    refreshThreads_.clear();
    refreshStop_ = false;
}

void App::updateWindowTitle() {
    // Only a project actually on disk names the window; an unsaved session's
    // projectPath_ is just a placeholder for the next save dialog, not a file.
    // Just the filename: a full path is wide enough to run under the menu bar.
    std::string title = "";
    if (projectHasPath_)
        title = fs::u8path(projectPath_).filename().u8string();
    SDL_SetWindowTitle(window_, title.c_str()); // taskbar / OS text
    titleBar_.setTitle(title);                  // custom chrome
}

// ---------------------------------------------------------------- sequence view
// Active-sequence view scoping (fit + playhead). The view-filter picker itself is
// the top-toolbar "Sequence" button/popup (see App_Letterbox.cpp).

// ---------------------------------------------------------------- view history

App::ViewHistoryEntry App::currentViewEntry() const {
    ViewHistoryEntry e;
    if (projScoped())
        e.projId = viewProjId_;
    else if (int fsi = filteredSeqIdx(); fsi >= 0)
        e.seqId = timeline_.sequences[fsi].id;
    return e; // {-1,-1} = All
}

void App::pushViewHistory(const ViewHistoryEntry& next) {
    if (viewHistoryLock_)
        return; // we are replaying history, not making it
    ViewHistoryEntry cur = currentViewEntry();
    if (cur == next)
        return; // re-picking the scope we are already in is not a step
    if (!viewHistory_.empty() && viewHistory_.back() == cur)
        return;
    viewHistory_.push_back(cur);
    if (viewHistory_.size() > kMaxViewHistory)
        viewHistory_.erase(viewHistory_.begin());
}

// Backspace: return to the scope before this one. Entries pointing at a sequence
// or project that has since been deleted are dropped and the next one tried.
void App::goBackView() {
    // A scratch view is its own step back, and a more exact one than the history
    // can express: it restores the playhead and the zoom too, so looking at a
    // source (or a layout) and pressing Backspace lands where the look started
    // rather than at the top of the sequence it was started from.
    if (scratchActive()) {
        dropScratchView();
        return;
    }
    viewHistoryLock_ = true;
    while (!viewHistory_.empty()) {
        ViewHistoryEntry e = viewHistory_.back();
        viewHistory_.pop_back();
        if (e.projId >= 0) {
            std::vector<int> seqIds;
            for (const Sequence& s : timeline_.sequences)
                if (s.projectId == e.projId)
                    seqIds.push_back(s.id);
            if (seqIds.empty())
                continue;
            setProjectView(e.projId, std::move(seqIds));
        } else if (e.seqId >= 0) {
            int idx = -1;
            for (int i = 0; i < (int)timeline_.sequences.size(); ++i)
                if (timeline_.sequences[i].id == e.seqId)
                    idx = i;
            if (idx < 0)
                continue;
            setSequenceView(idx);
        } else {
            scopeToAll();
        }
        viewHistoryLock_ = false;
        return;
    }
    viewHistoryLock_ = false;
    setStatus("NO PREVIOUS VIEW", 2000);
}

// --------------------------------------------------------------- scratch views
// A sequence that stands in for the cut while you look at something: the source
// view ("show me just this source") and the Layout stage's comparison stack. Both
// are marked Sequence::temporary, so neither reaches the project file or the dirty
// signature and neither shows up in the sequence lists — the cut the user is
// working on is left exactly as it was, which is what makes these cheap enough to
// reach for from a double-click or a single key.
//
// There is one slot, so opening either closes whatever was up and nothing
// accumulates. The scratch sequence is always the last in the vector, which is
// what lets it be dropped without invalidating any other index.

// Stand up the empty sequence and scope to it. The caller fills it.
int App::beginScratchSequence(const std::string& name, ScratchKind kind) {
    // Put back whatever a view already up replaced, so the return state recorded
    // below describes the real scope rather than the previous scratch view.
    dropScratchView();

    scratchReturn_ = { viewSeqIdx_, viewProjId_, viewProjSeqIds_, activeSequenceIdx_,
                       timeline_.playhead, viewStart_, framesPerPx_,
                       selectedClipIds_, selectedClipId_ };

    Sequence s;
    s.id = nextSeqId_++;
    s.temporary = true;
    s.name = name;
    timeline_.sequences.push_back(std::move(s));
    timeline_.repackSequences();
    const int idx = (int)timeline_.sequences.size() - 1;
    scratchSeqId_ = timeline_.sequences[idx].id;
    scratchKind_ = kind;
    scratchTrackCount_ = 1;
    // The rows on screen are this sequence's alone from here on: the cut's clips
    // must not type a row, occupy it or extend it (see Timeline::trackScopeSeqId).
    timeline_.trackScopeSeqId = scratchSeqId_;

    // Scope before the fill: addMediaFileAt routes the clip into the filtered
    // sequence. The history is locked out of it — the view carries its own, more
    // exact return state (playhead and zoom included), so it needs no entry of its
    // own, and leaving the history untouched means the Backspace after the one that
    // closes the view still walks the real scopes.
    viewHistoryLock_ = true;
    scopeToSequence(idx);
    viewHistoryLock_ = false;
    return idx;
}

// Open `path` on its own, playhead on source frame `srcFrame`.
void App::openSourceView(const std::string& path, int64_t srcFrame) {
    if (path.empty())
        return;

    const fs::path p = fs::u8path(path);
    const std::string name =
        "SOURCE: " + hashSeqStem(p.stem().u8string(),
                                 mediaTypeForPath(path) == ClipType::ImageSequence) +
        p.extension().u8string();
    const int idx = beginScratchSequence(name, ScratchKind::Source);

    const int newClipId = nextClipId_; // the clip addMediaFileAt is about to create
    // queryAudio false: pairing audio would put a pool entry (and possibly a whole
    // audio track) into the project on behalf of a view that isn't part of it.
    if (addMediaFileAt(path, /*track=*/-1, timeline_.seqRegions()[idx].start,
                       /*queryAudio=*/false) < 0) {
        dropScratchView(); // rejected (open failed); it set the status
        return;
    }
    if (const Clip* c = clipById(newClipId))
        setPlayhead(c->timelineStart + std::clamp<int64_t>(srcFrame, 0, c->duration - 1));
}

// F1: match-frame out of the program into the top-most clip's source, so the
// handles either side of its cut are there to look at.
void App::openSourceViewAtPlayhead() {
    const Clip* c = getTopMostClipAtFrame(timeline_.playhead);
    if (!c) {
        setStatusWarn("NO CLIP UNDER THE PLAYHEAD", 2000);
        return;
    }
    auto m = timeline_.findMediaById(c->mediaId);
    if (!m) {
        setStatusWarn("CLIP HAS NO SOURCE", 2000);
        return;
    }
    openSourceView(m->path(), c->sourceOffset + (timeline_.playhead - c->timelineStart));
}

void App::dropScratchView() {
    if (!scratchActive())
        return;
    const int id = scratchSeqId_;
    const bool wasLayout = layoutSeqActive();
    scratchSeqId_ = -1;
    scratchKind_ = ScratchKind::None;
    scratchTrackCount_ = 1;
    timeline_.trackScopeSeqId = -1; // the project's own stack is back on screen

    for (int i = 0; i < (int)timeline_.sequences.size(); ++i)
        if (timeline_.sequences[i].id == id) {
            timeline_.sequences.erase(timeline_.sequences.begin() + i);
            break;
        }
    focusedShotId_ = -1;
    infoClipId_ = -2;     // force the info overlay to rebuild
    timeline_.repackSequences();

    // The scope, exactly as it was. viewHistory_ is deliberately untouched: opening
    // the view added no entry to it (see beginScratchSequence).
    const ScratchReturn& r = scratchReturn_;
    viewSeqIdx_ = r.seqIdx;
    viewProjId_ = r.projId;
    viewProjSeqIds_ = r.projSeqIds;
    activeSequenceIdx_ =
        std::clamp(r.activeSeqIdx, 0, std::max((int)timeline_.sequences.size() - 1, 0));
    viewStart_ = r.viewStart;
    framesPerPx_ = r.framesPerPx;
    viewInitialized_ = true;
    // The selection the view was opened from, minus anything deleted since. The
    // view's own clips have just gone with it, so nothing of it survives here.
    clearClipSelection();
    for (int cid : r.selectedClipIds)
        if (clipById(cid))
            selectedClipIds_.push_back(cid);
    if (clipById(r.selectedClipId))
        selectedClipId_ = r.selectedClipId;
    setPlayhead(r.playhead);
    hostSnapshotDirty_ = true; // re-push the project to spectators if hosting

    // The Layout and Stack stages exist only for the sequence just dropped, so
    // leaving it leaves them. Written directly rather than through setPlayerStage,
    // which is what calls this — it assigns stage_ straight afterwards, and the
    // paths that drop a view without naming a stage (Backspace, F2, picking a
    // sequence) want exactly this.
    if (wasLayout && compareStage()) {
        stage_ = PlayerStage::Frame;
        restoreCompactAfterCompare();
    }
}

void App::dropScratchViewUnless(int keepSeqId) {
    if (scratchActive() && scratchSeqId_ != keepSeqId)
        dropScratchView();
}

void App::setSequenceView(int seqIdx) {
    const int seqId = seqIdx >= 0 && seqIdx < (int)timeline_.sequences.size()
                          ? timeline_.sequences[seqIdx].id
                          : -1;
    // Always last in the vector, so dropping it leaves seqIdx pointing where it did.
    dropScratchViewUnless(seqId);
    pushViewHistory({seqId, -1});
    clearProjectView();     // a single sequence and a project scope are exclusive
    viewSeqIdx_ = seqIdx;   // -1 = All
    focusedShotId_ = -1;    // changing the view filter drops any shot focus
    int fsi = filteredSeqIdx();
    // Selecting a sequence in the view filter also makes it the active
    // sequence (where freshly added clips land).
    if (fsi >= 0)
        activeSequenceIdx_ = fsi;
    fitToFilteredSequence();
    if (fsi >= 0) {
        const Sequence& seq = timeline_.sequences[fsi];
        int64_t a = 0, b = 0;
        const bool hasSpan = timeline_.sequenceSpan(seq, a, b);
        // Scoping to the sequence the playhead is already inside is a narrowing of
        // the view, not a jump: leave the current clip and frame alone.
        if (hasSpan && timeline_.playhead >= a && timeline_.playhead < b)
            return;
        // Move playhead to the earliest shot in the sequence.
        // Fall back to the sequence's first clip frame if there are no shots.
        int64_t target = a;
        bool foundShot = false;
        for (int shotId : seq.shotIds) {
            if (const Shot* s = timeline_.findShotById(shotId)) {
                if (!foundShot || s->timelineStart < target) {
                    target = s->timelineStart;
                    foundShot = true;
                }
            }
        }
        // ...unless the playhead is already inside this sequence: picking the
        // sequence you are watching is a change of scope, not of position, so
        // the frame under the player stays put.
        if (timeline_.playhead >= a && timeline_.playhead < b)
            target = timeline_.playhead;
        setPlayhead(target);
    }
}

// Scope to every sequence of one project (the Sequence popup's project rows).
// viewSeqIdx_ drops to -1 so filteredSeqIdx() reports "not a single sequence",
// while the project's own sequences stay in view; the first of them becomes the
// active sequence, so freshly added clips still land somewhere in the project.
void App::setProjectView(int projId, std::vector<int> seqIds) {
    dropScratchViewUnless(-1);
    pushViewHistory({-1, projId});
    viewProjId_ = projId;
    viewProjSeqIds_ = std::move(seqIds);
    viewSeqIdx_ = -1;
    focusedShotId_ = -1;
    std::vector<int> idxs = viewSeqIndices();
    if (!idxs.empty())
        activeSequenceIdx_ = idxs.front();
    int64_t a = 0, b = 0;
    if (viewSpan(a, b)) {
        fitRange(a, b);
        setPlayhead(a);
    } else {
        fitView();
    }
}

Project::ViewState App::viewState() const {
    Project::ViewState v;
    // A scratch view reports the scope it was opened from: the temporary sequence it
    // points at is not written, so storing its index would leave the file naming a
    // sequence that isn't there — and it would make merely looking at a source (or a
    // layout) read as an edit to the dirty signature, which is this same state hashed.
    v.seqIdx = scratchActive() ? scratchReturn_.seqIdx : viewSeqIdx_;
    v.projId = scratchActive() ? scratchReturn_.projId : viewProjId_;
    return v;
}

// Restore the scope a load handed back. Unlike the picker paths (setSequenceView /
// setProjectView) the playhead stays where the file put it — only the horizontal
// fit follows the scope. A project scope whose sequences are all gone, or a state
// with no scope at all, falls back to the first sequence.
void App::applyViewState(const Project::ViewState& v) {
    viewHistory_.clear(); // a loaded scope is a starting point, not a step back from
    resetProjectScopeState();
    viewSeqIdx_ = 0;
    if (v.projId >= 0) {
        std::vector<int> seqIds;
        for (const Sequence& s : timeline_.sequences)
            if (s.projectId == v.projId)
                seqIds.push_back(s.id);
        if (!seqIds.empty()) {
            viewProjId_ = v.projId;
            viewProjSeqIds_ = std::move(seqIds);
            viewSeqIdx_ = -1; // the two scopes are exclusive
            activeSequenceIdx_ = viewSeqIndices().front();
            fitToFilteredSequence();
            return;
        }
    }
    if (v.seqIdx >= 0 && v.seqIdx < (int)timeline_.sequences.size())
        viewSeqIdx_ = v.seqIdx;
    activeSequenceIdx_ = std::max(viewSeqIdx_, 0);
    fitToFilteredSequence();
}

std::vector<int> App::viewSeqIndices() const {
    std::vector<int> out;
    if (int fsi = filteredSeqIdx(); fsi >= 0) {
        out.push_back(fsi);
        return out;
    }
    if (projScoped()) {
        // By id, in timeline order: a sequence deleted since the scope was set
        // simply drops out of it.
        for (int i = 0; i < (int)timeline_.sequences.size(); ++i)
            if (std::find(viewProjSeqIds_.begin(), viewProjSeqIds_.end(),
                          timeline_.sequences[i].id) != viewProjSeqIds_.end())
                out.push_back(i);
        if (!out.empty())
            return out;
    }
    out.resize(timeline_.sequences.size());
    for (int i = 0; i < (int)out.size(); ++i)
        out[i] = i;
    return out;
}

void App::forEachViewClip(const std::function<void(const Clip&)>& fn) const {
    if (viewAll()) {
        timeline_.forEachClip(fn);
        return;
    }
    for (int si : viewSeqIndices())
        for (const Clip& c : timeline_.sequences[si].clips)
            fn(c);
}

bool App::viewSpan(int64_t& start, int64_t& end) const {
    bool any = false;
    for (int si : viewSeqIndices()) {
        int64_t a = 0, b = 0;
        if (!timeline_.sequenceSpan(timeline_.sequences[si], a, b))
            continue;
        start = any ? std::min(start, a) : a;
        end   = any ? std::max(end, b) : b;
        any = true;
    }
    return any;
}

bool App::scopeRange(int64_t& start, int64_t& end) const {
    if (viewAll())
        return false;
    if (viewSpan(start, end))
        return true;
    // Scoped to sequence(s) with no clips yet: the scope is the packed insert point
    // of the first of them (where a dropped clip lands).
    std::vector<int> idxs = viewSeqIndices();
    if (idxs.empty())
        return false;
    start = end = timeline_.seqRegions()[idxs.front()].start;
    return true;
}

void App::playbackRange(int64_t& lo, int64_t& hi) const {
    lo = std::min(timeline_.inPoint, timeline_.effectiveOut());
    hi = std::max(timeline_.inPoint, timeline_.effectiveOut());
    int64_t a = 0, b = 0;
    if (scopeRange(a, b)) {
        const int64_t last = std::max(a, b - 1); // empty scope: the insert point itself
        lo = std::clamp(lo, a, last);
        hi = std::clamp(hi, a, last);
    }
}

std::string App::sequenceViewLabel() const {
    if (int fsi = filteredSeqIdx(); fsi >= 0)
        return timeline_.sequences[fsi].name;
    if (const SourceProject* p = timeline_.findProjectById(viewProjId_))
        return p->name;
    return "All";
}

void App::fitToFilteredSequence() {
    int64_t a = 0, b = 0;
    if (!viewAll() && viewSpan(a, b))
        fitRange(a, b);
    else
        fitView();
}

// ---------------------------------------------------------------- file dialogs
// Native file/folder choosers (open / save / import OTIO / export / create from
// directory), their async result callbacks, and the main-thread pump that
// applies the deferred results. Moved from App.cpp.

namespace {
// Open accepts either kind of project file and dispatches on the extension (see
// processPendingDialogs), the same way a dropped file and a command-line
// argument already do. Save only ever writes a .jpproj — the .otio is an export,
// not a round-trip format (see OtioExport.h).
const SDL_DialogFileFilter kOpenFilters[] = {
    { "jplay / OpenTimelineIO project", "jpproj;otio" },
    { "jplay project", "jpproj" },
    { "OpenTimelineIO", "otio" },
    { "All files", "*" },
};
const SDL_DialogFileFilter kProjectFilters[] = {
    { "jplay project", "jpproj" },
    { "All files", "*" },
};
const SDL_DialogFileFilter kOtioFilters[] = {
    { "OpenTimelineIO", "otio" },
    { "All files", "*" },
};
} // namespace

void App::openProjectDialog() {
    // Gate before the chooser, not after: backing out of the unsaved-changes
    // prompt should cost nothing, and the same order the OTIO export warning uses.
    confirmDiscard("Open Project", [this] {
        SDL_ShowOpenFileDialog(&App::onOpenChosen, this, window_, kOpenFilters,
                               SDL_arraysize(kOpenFilters), nullptr, false);
    });
}

void App::saveProjectDialog() {
    SDL_ShowSaveFileDialog(&App::onSaveChosen, this, window_, kProjectFilters,
                           SDL_arraysize(kProjectFilters), projectPath_.c_str());
}

void App::saveProjectQuick() {
    if (projectHasPath_)
        saveProject();      // overwrite the current file
    else
        saveProjectDialog(); // never saved yet -> prompt for a location
}

void App::exportOtio() {
    // Default the name to the project's, with the extension swapped.
    auto chooser = [this] {
        const std::string suggested =
            fs::u8path(projectPath_).replace_extension(".otio").u8string();
        SDL_ShowSaveFileDialog(&App::onExportOtioChosen, this, window_, kOtioFilters,
                               SDL_arraysize(kOtioFilters), suggested.c_str());
    };

    // Warn before the file chooser, so backing out costs nothing. A project with
    // nothing unrepresentable goes straight to the chooser.
    std::vector<std::string> lost = OtioExport::survey(timeline_);
    if (lost.empty()) {
        chooser();
        return;
    }

    std::string msg = "OpenTimelineIO stores the edit only. This project's\n"
                      "remaining state is not written and will be lost:\n";
    for (const std::string& line : lost)
        msg += "\n  - " + line;
    msg += "\n\nSave a .jpproj to keep all of it.";

    showDialog("Export OTIO", msg,
               { { "Cancel", nullptr }, { "Export Anyway", chooser } },
               /*escIdx*/ 0, /*enterIdx*/ 1);
}

void App::createFromDirDialog() {
    confirmDiscard("Create From Directory", [this] {
        SDL_ShowOpenFolderDialog(&App::onCreateFromDirChosen, this, window_, nullptr, false);
    });
}

// Build a per-timeline-frame snapshot and open the export dialog.
static void buildFrameSnapshot(const Timeline& tl,
                                const std::function<const Clip*(int64_t)>& clipAt,
                                std::vector<ExportDialog::FrameSource>& out) {
    int64_t len = tl.length();
    out.resize((size_t)std::max<int64_t>(len, 0));
    for (int64_t f = 0; f < len; ++f) {
        const Clip* c = clipAt(f);
        if (!c || c->mediaId.empty()) continue;
        auto m = tl.findMediaById(c->mediaId);
        if (!m) continue;
        out[f] = { m->path(), m->type(),
                   c->sourceOffset + (f - c->timelineStart) };
    }
}

std::map<std::string, OcioManager::CpuTransform> App::exportColorTransforms() {
    std::map<std::string, OcioManager::CpuTransform> out;
    if (!ocio_.isReady() || !ocio_.isEnabled())
        return out;
    for (auto& kv : timeline_.media) {
        auto& m = kv.second;
        if (!m || m->type() == ClipType::Audio)
            continue;
        std::string cs = mediaColorSpace(*m);
        if (cs.empty())
            continue;
        // No exposure: the export writes the display rendering, not the grade. The
        // grade is a review control and has never been baked into an export; gain
        // is part of it, so it stays out here too.
        out.emplace(m->path(), ocio_.cpuTransformFor(cs, 0.0f));
    }
    return out;
}

void App::openExportMovie() {
    // Determine output resolution from the clip under the playhead (or first clip).
    int w = 0, h = 0;
    const Clip* c = getTopMostClipAtFrame(timeline_.playhead);
    if (!c) {
        // Fall back to the first clip in the timeline.
        timeline_.forEachClip([&](const Clip& cl) {
            if (!w) {
                auto m = timeline_.findMediaById(cl.mediaId);
                if (m) { auto info = m->info(); w = info.width; h = info.height; }
            }
        });
    } else {
        auto m = timeline_.findMediaById(c->mediaId);
        if (m) { auto info = m->info(); w = info.width; h = info.height; }
    }

    exportDialog_.open(ExportDialog::Mode::Movie,
                       timeline_.length(), timeline_.inPoint, timeline_.outPoint,
                       timeline_.fps, w, h, window_);

    std::vector<ExportDialog::FrameSource> frames;
    buildFrameSnapshot(timeline_, [this](int64_t f) { return getTopMostClipAtFrame(f); }, frames);
    exportDialog_.setFrames(std::move(frames));
    exportDialog_.setColorTransforms(exportColorTransforms());
}

void App::openExportImageSequence() {
    int w = 0, h = 0;
    const Clip* c = getTopMostClipAtFrame(timeline_.playhead);
    if (!c) {
        timeline_.forEachClip([&](const Clip& cl) {
            if (!w) {
                auto m = timeline_.findMediaById(cl.mediaId);
                if (m) { auto info = m->info(); w = info.width; h = info.height; }
            }
        });
    } else {
        auto m = timeline_.findMediaById(c->mediaId);
        if (m) { auto info = m->info(); w = info.width; h = info.height; }
    }

    exportDialog_.open(ExportDialog::Mode::ImageSequence,
                       timeline_.length(), timeline_.inPoint, timeline_.outPoint,
                       timeline_.fps, w, h, window_);

    std::vector<ExportDialog::FrameSource> frames;
    buildFrameSnapshot(timeline_, [this](int64_t f) { return getTopMostClipAtFrame(f); }, frames);
    exportDialog_.setFrames(std::move(frames));
    exportDialog_.setColorTransforms(exportColorTransforms());
}

void App::onOpenChosen(void* userdata, const char* const* filelist, int) {
    App* self = static_cast<App*>(userdata);
    if (!filelist || !filelist[0]) // null => error; empty first entry => cancelled
        return;
    std::lock_guard<std::mutex> lock(self->dialogMutex_);
    self->pendingOpenPath_ = filelist[0];
}

void App::onSaveChosen(void* userdata, const char* const* filelist, int) {
    App* self = static_cast<App*>(userdata);
    std::lock_guard<std::mutex> lock(self->dialogMutex_);
    // Flagged even when cancelled: an action parked on this save (afterSave_)
    // has to be dropped rather than left to fire on some later Save As.
    self->pendingSaveResolved_ = true;
    if (filelist && filelist[0])
        self->pendingSavePath_ = filelist[0];
}

void App::onExportOtioChosen(void* userdata, const char* const* filelist, int) {
    App* self = static_cast<App*>(userdata);
    if (!filelist || !filelist[0]) // null => error; empty first entry => cancelled
        return;
    std::lock_guard<std::mutex> lock(self->dialogMutex_);
    self->pendingExportOtioPath_ = filelist[0];
}

void App::onMediaChosen(void* userdata, const char* const* filelist, int) {
    App* self = static_cast<App*>(userdata);
    if (!filelist || !filelist[0]) // null => error; empty first entry => cancelled
        return;
    std::lock_guard<std::mutex> lock(self->dialogMutex_);
    for (const char* const* p = filelist; *p; ++p)
        self->pendingMediaPaths_.push_back(*p);
}

void App::onReplaceSourceChosen(void* userdata, const char* const* filelist, int) {
    App* self = static_cast<App*>(userdata);
    if (!filelist || !filelist[0]) // null => error; empty first entry => cancelled
        return;
    std::lock_guard<std::mutex> lock(self->dialogMutex_);
    self->pendingReplaceSourcePath_ = filelist[0];
}

void App::onCreateFromDirChosen(void* userdata, const char* const* filelist, int) {
    App* self = static_cast<App*>(userdata);
    if (!filelist || !filelist[0]) // null => error; empty first entry => cancelled
        return;
    std::lock_guard<std::mutex> lock(self->dialogMutex_);
    self->pendingCreateFromDir_ = filelist[0];
}

void App::onRelocateChosen(void* userdata, const char* const* filelist, int) {
    App* self = static_cast<App*>(userdata);
    std::lock_guard<std::mutex> lock(self->dialogMutex_);
    // A cancelled/failed browse (null or empty first entry) resumes with an empty
    // dir, which re-shows the Missing Source dialog for the same source.
    self->pendingRelocateDir_ = (filelist && filelist[0]) ? filelist[0] : std::string();
    self->pendingRelocateDirReady_ = true;
}

void App::processPendingDialogs() {
    std::string openPath, savePath, exportOtioPath, createFromDir, relocateDir, replacePath;
    std::vector<std::string> mediaPaths;
    bool relocateReady = false;
    bool saveResolved = false;
    {
        std::lock_guard<std::mutex> lock(dialogMutex_);
        openPath.swap(pendingOpenPath_);
        savePath.swap(pendingSavePath_);
        saveResolved = pendingSaveResolved_;
        pendingSaveResolved_ = false;
        exportOtioPath.swap(pendingExportOtioPath_);
        mediaPaths.swap(pendingMediaPaths_);
        createFromDir.swap(pendingCreateFromDir_);
        replacePath.swap(pendingReplaceSourcePath_);
        if (pendingRelocateDirReady_) {
            relocateReady = true;
            pendingRelocateDirReady_ = false;
            relocateDir.swap(pendingRelocateDir_);
        }
    }
    // A relocate Browse result just fills the modal's directory field (the user
    // still confirms with Relocate). Ignore an empty/cancelled browse.
    if (relocateReady && relocateModalOpen_ && !relocateDir.empty()) {
        relocateDirInput_.setText(relocateDir);
        relocateError_.clear();
    }
    if (!createFromDir.empty())
        createProjectFromDirectory(createFromDir);
    // One Open for both project kinds: the extension picks the loader, exactly as
    // for a dropped file (see handleEvent) or a command-line argument.
    if (!openPath.empty()) {
        if (hasExtension(openPath, ".otio"))
            loadOtio(openPath);
        else
            loadProject(openPath);
    }
    if (!savePath.empty()) {
        if (fs::path(savePath).extension() != ".jpproj")
            savePath += ".jpproj";
        projectPath_ = savePath;
        projectHasPath_ = true;
        saveProject();
    }
    // Resume (or drop) whatever was waiting on that Save As. Only a save that
    // actually wrote a file lets the action through; cancelling the chooser
    // cancels the New / Close / Open / Quit it was standing in for.
    if (saveResolved && afterSave_) {
        std::function<void()> resume = std::move(afterSave_);
        afterSave_ = nullptr;
        if (!savePath.empty())
            resume();
    }
    // Export leaves projectPath_/projectHasPath_ alone: the .otio never becomes
    // the current file, so Save still targets the .jpproj.
    if (!exportOtioPath.empty()) {
        if (fs::path(exportOtioPath).extension() != ".otio")
            exportOtioPath += ".otio";
        std::string err;
        if (OtioExport::save(exportOtioPath, timeline_, err))
            setStatus("EXPORTED " + fileLabel(exportOtioPath), 3000);
        else
            setStatus("OTIO EXPORT FAILED: " + err, 5000);
    }
    // A Replace Source pick applies to the source the context menu was opened on,
    // pinned by id when the chooser was raised (browseReplaceSource). Consumed
    // either way, so a stale pin can never catch a later pick.
    if (!replacePath.empty() && !replaceSourceMediaId_.empty()) {
        std::string mediaId;
        mediaId.swap(replaceSourceMediaId_);
        replaceSourceMedia(mediaId, replacePath);
    }
    if (!mediaPaths.empty()) {
        // Same placement as dropping files on the frame view: auto-pick a track by
        // media kind (track -1), at the playhead, laying selections end-to-end.
        dropInsertTrack_ = -1;
        dropInsertFrame_ = timeline_.playhead;
        for (const auto& p : mediaPaths) {
            std::error_code ec;
            if (fs::is_directory(p, ec)) {
                addMediaFolder(p);
            } else {
                int64_t end = addMediaFileAt(p, dropInsertTrack_,
                                             dropInsertFrame_);
                if (end >= 0)
                    dropInsertFrame_ = end;
            }
        }
    }
}

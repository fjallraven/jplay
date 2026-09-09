#pragma once

#include "Project.h" // Project::Thumbnail

#include <cstdint>
#include <string>
#include <vector>

// Per-user application data under ~/.jplay (the "settings" and "projects"
// folders): the recent-projects list (settings.conf) and out-of-band project
// thumbnails (projects/<id>). All of it is best-effort: if the home location
// can't be written we fall back to %TMP%/%TEMP%, and if even that fails every
// call degrades to a harmless no-op rather than failing a save.
namespace UserData {

struct RecentProject {
    std::string path;      // .jpproj path (absolute when resolvable)
    std::string id;        // project id keying the thumbnail (empty for legacy files)
    int64_t savedUnix = 0; // time of the save/open that recorded this entry
};

// Application-wide display preferences persisted in settings.conf alongside the
// recent-projects list. Defaults match the in-code defaults of the App members,
// except for the two networking switches: those default to whatever
// jplay_preferences.conf says (see loadPrefs), so a site can ship its own
// starting position for them.
struct Prefs {
    bool timeFormatFrames = false;  // false = Timecode, true = Frames
    bool frameNumberingClip = false; // false = Global numbering, true = Clip
    bool pythonDebug = false;       // log Python callback inputs/outputs
    bool showFramePreview = true;   // thumbnail above the ruler hover indicator
    bool snapPlayhead = true;       // scrubbing snaps the playhead to clip start frames
    bool audioScrub = false;        // play short audio grains while scrubbing the ruler
    bool clipWaveform = false;      // waveform inside the video clip under the playhead
    bool attachAudioToSeq = false;   // pair aligned audio (query_audio) with added EXR sequences
    bool warnUnsaved = true;        // prompt before discarding a project with unsaved changes
    int  sourceThumbSize = 16;      // SOURCES media-bin thumbnail size in device px (min = names only)
    int  sourceSort = 1;            // SOURCES bin order (App::PeSort): 0 = by name, 1 = by sequence
    float cacheGb = 8.0f;           // frame-cache budget in GiB (fills until this full, then evicts)
    int  decodeThreads = 0;         // frame-cache decode workers; 0 = Auto (cores - 2, capped at 8)
    float nitRef = 100.0f;          // scene-linear 1.0 = N nits, for the Luminance heatmap
    bool hdrOutput = false;         // create the renderer with SRGB_LINEAR output for HDR display (restart required)
    float hdrRefWhiteNits = 100.0f; // master paper/diffuse-white nits mapped to panel SDR white (PQ HDR)
    float uiScale = 0.0f;           // UI Scale: device pixels per logical unit; 0 = Auto (follow the display)
    bool syncNetwork = false;       // sync review may open sockets; off means no port is bound at launch
    int  syncPort = 45778;          // TCP port a sync host listens on (syncreview::kDefaultPort)
    bool mcpEnabled = false;        // open the local control channel the MCP server drives
    bool proxyEnabled = false;      // show the top-bar Proxy dropdown and allow non-Full modes
    int  gridThumbH = 60;          // Overview grid tile height in device px (ctrl+wheel over the grid)
    float volume = 1.0f;            // master audio output gain, 0..1
    bool muted = false;             // master mute (keeps `volume` so unmuting restores it)
    bool frameOverlay = false;      // burn-in over the program image (file name + time)
    bool frameOverlayBottom = false; // false = top edge of the picture, true = bottom
    int  overlaySize = 0;           // burn-in text size (index into App::kOverlaySizes)
    int  overlayColor = 0;          // burn-in text colour (index into App::kOverlayColors)
    bool compactTimeline = false;   // timeline collapsed to its ruler + cache strip (TAB / View row)
};

// Base data directory (~/.jplay), created on first use. Falls back to
// %TMP%/.jplay then %TEMP%/.jplay when the home location isn't writable.
// Empty only when no writable location was found; cached after the first call.
const std::string& dir();

// <dir>/projects, created on demand. Empty when dir() is empty.
std::string projectsDir();

// A fresh project id (32 lowercase hex chars).
std::string newProjectId();

// Recent projects, most-recent first, capped at 20, read from settings.conf.
std::vector<RecentProject> loadRecent();

// Move `rp` to the front (dedup by path), cap at 20, and rewrite settings.conf.
// No-op when dir() is empty or rp.path is empty.
void recordRecent(const RecentProject& rp);

// Drop the recent entry matching `path` (dedup rules of recordRecent) and
// rewrite settings.conf. No-op when dir() is empty.
void removeRecent(const std::string& path);

// Display preferences from settings.conf. Absent keys keep their defaults —
// which for syncNetwork/mcpEnabled come from jplay_preferences.conf ([sync] and
// [control] `enabled`), so an untouched machine follows the deployed config.
Prefs loadPrefs();

// Persist display preferences, preserving the recent-projects list. No-op when
// dir() is empty.
void savePrefs(const Prefs& prefs);

// Thumbnail file IO at <projectsDir>/<id>-<hash of projectPath>. The path is
// part of the key because the project id deliberately survives a Save As (it is
// the project's identity, see App::projectId_), so id alone would have every
// copy overwriting -- and displaying -- one shared thumbnail. The path is
// normalised to an absolute lexical form before hashing, so callers may pass a
// relative one. write is a no-op for an empty id or invalid image; read returns
// false when the file is absent or unreadable.
bool writeThumbnail(const std::string& id, const std::string& projectPath,
                    const Project::Thumbnail& t);
bool readThumbnail(const std::string& id, const std::string& projectPath,
                   Project::Thumbnail& t);

} // namespace UserData

#pragma once

#include "ContextMenu.h"
#include "ControlServer.h"
#include "Dropdown.h"
#include "ExportDialog.h"
#include "FrameCache.h"
#include "IconFont.h"
#include "LeftPanels.h"
#include "OcioManager.h"
#include "OcioGpu.h"
#include "HdrColorPass.h"
#include "Output.h"
#include "Progress.h"
#include "SyncSession.h"
#include "Grade.h"
#include "GradeGpu.h"
#include "TextFont.h"
#include "Menu.h"
#include "ThumbnailCache.h"
#include "Timeline.h"
#include "TitleBar.h"
#include "Undo.h"
#include "UserData.h"
#include "WaveformCache.h"
#include "Widgets.h"
#include "WorkQueue.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

struct DiscoveredShot; // defined in PythonBridge.h; used by createProjectFromDirectory
struct PickerState;    // defined in PythonBridge.h; passed to rebuildClipPickerMenu
struct PickerOption;   // ditto; one row of a PickerColumn
class Media;           // defined in Media.h; used in method signatures below
class AudioEngine;     // defined in AudioEngine.h; audio playback for the video under the playhead
namespace SharedProjects { struct Entry; } // defined in SharedProjects.h; passed to submitSharedProbe

class App {
public:
    App();
    ~App();
    bool init(int argc, char** argv);
    void run();

private:
    // SDL
    SDL_Window* window_ = nullptr;
    SDL_Renderer* renderer_ = nullptr;
    SDL_GPUDevice* gpuDevice_ = nullptr; // owns the SPIR-V SDL_GPU device when HDR uses the "gpu" renderer
    SDL_Texture* texture_ = nullptr;
    SDL_Cursor* cursorDefault_ = nullptr;
    SDL_Cursor* cursorEWResize_ = nullptr;
    SDL_Cursor* cursorNSResize_ = nullptr; // timeline top-edge resize
    SDL_Cursor* cursorCrosshair_ = nullptr;
    SDL_Cursor* cursorMove_ = nullptr;   // overwrite clip drag
    SDL_Cursor* cursorIBeam_ = nullptr;  // ripple clip drag (insertion bar)
    int texW_ = 0, texH_ = 0;
    // Pixel aspect of what texture_ currently holds (pixel width / height; 1 =
    // square). texW_/texH_ count stored pixels, so an anamorphic source has to be
    // stretched horizontally by this to display in its true shape — see texDispW.
    float texPa_ = 1.0f;
    CacheKey displayedKey_;
    // Second half of the program texture's identity. A dissolve blends two cached
    // frames, so displayedKey_ alone no longer says whether the texture already
    // holds what this frame needs — without these, the first frame of a span would
    // render and every later one would be skipped as already-displayed. Outside a
    // transition displayedKeyB_ is a default (empty mediaId) and the mix is 0.
    CacheKey displayedKey2_;
    float displayedMix_ = 0.0f;
    float displayedFade_ = 1.0f;
    // Blend scratch for a dissolve or a fade: the composited result standing in for
    // a decoded frame through the rest of the pipeline (OCIO, grade, tech check all
    // run on it unchanged). conformFrame_ holds a secondary that had to be fitted to
    // the primary's size first; ocioMixFrame_ holds the scene-linear side of a
    // mixed-space dissolve after its display transform, so the pair can be mixed
    // display-referred (see renderPlayer). All reused across frames.
    Frame mixFrame_;
    Frame conformFrame_;
    Frame ocioMixFrame_;
    bool hasTexture_ = false;
    bool programShown_ = false; // a live program frame is on screen this frame (else black); drives the review window
    bool inDrawFrame_ = false;  // re-entrancy guard: SDL calls that pump events (window create/fullscreen) must not recursively re-draw

    // App icon shown at the left of the title bar (loaded from the embedded
    // executable resource; see AppIcon / jplay.rc).
    SDL_Texture* iconTex_ = nullptr;

    // Window menu under that icon (Minimize / Maximize-Restore / Close): the
    // borderless window's stand-in for the native system menu.
    ContextMenu appIconMenu_;

    // Material Design Icons webfont (embedded; see icon_font_data.h) used to draw
    // toolbar glyphs (currently the directory / explorer toggle). Owns its font
    // and glyph textures; see IconFont.
    IconFont icons_;

    // UI text font loaded from font.ttf (next to the executable). Used everywhere
    // SDL_RenderDebugText was previously used; see TextFont.
    TextFont textFont_;
    // Larger variant of the same face for emphasised titles (e.g. Overview
    // sequence headers) — rendered at native point size so it stays crisp
    // rather than scaling the small font up.
    TextFont headerFont_;

    // Model
    Timeline timeline_;
    std::unique_ptr<FrameCache> cache_;
    int nextClipId_ = 1;
    int nextSeqId_  = 1;
    int nextShotId_ = 1;
    int nextTransitionId_ = 1;
    int activeSequenceIdx_ = 0; // index into timeline_.sequences; always valid
    int focusedShotId_ = -1;    // Shot::id the view is scoped to, or -1 (none); only meaningful while a sequence is focused
    std::string projectPath_ = "project.jpproj";
    bool projectHasPath_ = false; // true once associated with a file on disk
    // Opened from the launcher's SHARED PROJECTS column. Such a project is a
    // read-only view of a show's shared file that can be reopened at any time,
    // and it is unsaved by construction (so permanently "dirty") — prompting to save it
    // on every Quit/Open would be noise, so confirmDiscard skips the prompt.
    // Cleared the moment the project becomes the user's own: any other load, a
    // New, or a Save that gives it a .jpproj of its own.
    bool projectFromShared_ = false;
    std::string projectId_;       // stable id keying the on-disk thumbnail; assigned on first save

    // Recent projects shown on the empty player. Each tile owns a thumbnail
    // texture (built lazily from ~/.jplay/projects/<id>) and a hit rect that is
    // refreshed every render for click handling.
    struct RecentTile {
        std::string path;
        std::string id;
        std::string label;     // basename as stored on disk
        std::string lastOpened;// "Last Opened <date>", precomputed for display
        SDL_Texture* tex = nullptr;
        int texW = 0, texH = 0;
        SDL_FRect rect{};
        SDL_FRect closeRect{}; // remove-from-recents cross on the right edge
    };
    std::vector<RecentTile> recent_;
    float recentScroll_ = 0.0f;   // RECENT PROJECTS list vertical scroll (px)
    SDL_FRect recentListRect_{};  // viewport of the recent list, for wheel hit-testing
    ScrollbarDrag recentSb_;      // its scrollbar, grabbable
    bool recentDirty_ = true; // reload list + rebuild textures on next empty render

    // SHARED PROJECTS launcher column: studio shows discovered by scanning the
    // shows-config (jplay_preferences [shared_projects]) and keeping those whose
    // project file exists on disk. Scanned on the work queue so the network stats
    // never block the UI; the column appears whenever the shows-config is set
    // (mirrors SYNC SESSION, which appears only when live). Loading one brings in
    // its .otio or .jpproj as an unsaved project — the shared path is never made
    // the current file, so Save prompts for a new location.
    struct SharedProject {
        std::string code; // short show code, e.g. "TON"
        std::string name; // long name, e.g. "Positano"
        std::string path; // resolved, verified-to-exist .otio or .jpproj path
        SDL_FRect rect{}; // hit rect, rebuilt each render
    };
    std::vector<SharedProject> sharedProjects_;
    float sharedScroll_ = 0.0f;   // SHARED PROJECTS list vertical scroll (px)
    SDL_FRect sharedListRect_{};  // viewport of the shared list, for wheel hit-testing
    ScrollbarDrag sharedSb_;      // its scrollbar, grabbable
    bool sharedDirty_ = true;     // kick the background scan once on next launcher render

    // The launcher's left column is tabbed: RECENT PROJECTS, SHARED PROJECTS and
    // SYNC. All three tabs are always in the strip, so it never changes width;
    // the two fed by background scans draw faded and take no click until their
    // scan has something to offer (and SYNC fades back out when its beacons time
    // out). A tab that loses its data while it is the active one falls back to
    // RECENT.
    enum class LauncherTab { Recent, Shared, Sync };
    LauncherTab launcherTab_ = LauncherTab::Recent;
    struct LauncherTabBtn { LauncherTab tab; const char* label; bool enabled; SDL_FRect rect; };
    std::vector<LauncherTabBtn> launcherTabs_; // hit rects, rebuilt each render
    bool hideLauncher_ = false; // set by CREATE EMPTY PROJECT and by finishLoad()
                                // (any project opened); reset by newProject()
    // The launcher shows only on an empty project the user hasn't dismissed via
    // CREATE EMPTY PROJECT and that never had a project loaded into it — once
    // one has, emptying the timeline must not pop the launcher back over the
    // user's work. When false, the (possibly empty) timeline and side panel
    // render instead.
    bool launcherVisible() const { return !timeline_.hasClips() && !hideLauncher_; }

    // Cinema mode (F11, Esc to leave): the program image alone, fullscreen. Every
    // band of chrome — title bar, menu, top toolbar, left panes, timeline,
    // inspector — collapses to nothing in computeLayout() and is skipped in
    // render(), so playerRect_ comes out as the whole window; the frame overlays
    // that were already on (info, tech legend, pixel probe) keep drawing.
    // The docked panes are genuinely closed rather than merely left undrawn, and
    // these two remember what to reopen on the way out. See setCinemaMode().
    bool cinemaMode_ = false;
    int  cinemaPanel_ = -1;        // LeftPanel open on entry, cast to int; -1 for none
    bool cinemaInspector_ = false; // inspector was open on entry

    // Compact timeline (TAB, or View > Compact Timeline): the timeline keeps its
    // info bar, ruler and cache strip and drops everything below them - sequence
    // bar, shot bar, track stack - so the player takes the height back. Unlike
    // cinema mode the rest of the chrome stays exactly where it is; computeLayout()
    // is the only place that reads this, and the bands it zeroes take their hit
    // regions with them (overTrackArea() bounds on tracksViewH_, which comes out 0).
    bool compactTimeline_ = false;
    // What compactTimeline_ was when the Layout / Stack stage was entered. Those
    // stages force compact and refuse to leave it, so the way out has to put back
    // what the user had rather than leaving the tracks collapsed behind them.
    bool compactBeforeCompare_ = false;

    // CREATE PROJECT launcher buttons (left of the recent list on the empty
    // player). Hit rects are refreshed every render for click handling.
    SDL_FRect createEmptyBtn_{}, createFromDirBtn_{}, openProjectBtn_{}, importMediaBtn_{};

    // Process launch instant, taken first thing in init(). Kept so run() can
    // report when the first frame actually reaches the screen — the number that
    // matters for perceived launch speed, not when init() returned.
    std::chrono::steady_clock::time_point launchTime_{};

    // Playback
    bool quit_ = false;
    bool playing_ = false;
    int playDir_ = 1;
    double playAcc_ = 0.0;
    Uint64 lastTickMs_ = 0;
    // Visible playback rate (debug readout): timestamps (ms) of distinct frames
    // actually shown in the player over the last ~1s. fpsVisible_ is the rate
    // derived from them — reads the project fps when playback keeps up and sags
    // only when frames are genuinely held/dropped.
    std::deque<Uint64> visibleFrameTimes_;
    double fpsVisible_ = 0.0;
    // The value actually drawn: fpsVisible_ snapped to the project rate and then
    // held until it moves a full tenth, so the readout does not flicker between
    // adjacent tenths (23.9 / 24.0) on sub-frame ripple.
    double fpsShown_ = 0.0;
    // Tick playback began: the lower bound of the measuring window, so the idle
    // time before Play is not mistaken for a stall while the window fills.
    Uint64 fpsWindowFrom_ = 0;
    // True while playback is holding this tick for an uncached frame; passed to
    // the audio engine so it pauses in lockstep with the frozen video.
    bool playbackStalled_ = false;

    // Clip-at-a-time review state (X, PgUp/PgDn, Shift+PgUp/PgDn). The anchor is
    // the clip the range was last marked from and rangeExpand_ how many clips are
    // included on each side of it; the range is always recomputed from the pair,
    // so a range set some other way ([ ]) just re-anchors instead of drifting.
    int rangeAnchorClipId_ = -1;
    int rangeExpand_ = 0;

    // Audio playback. updateAudio() gathers everything audible at the playhead
    // (the program video's embedded audio plus every audio clip covering it) and
    // hands the set to the AudioEngine mixer, slaved to the video-master
    // playhead; null construction is harmless (no output device).
    std::unique_ptr<AudioEngine> audio_;
    void updateAudio();

    // Audio output device selection (from the "Output" menu). Enumerated once at
    // startup — no runtime hotplug. audioDeviceId_ drives AudioEngine's stream;
    // SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK means "follow the system default".
    struct AudioDeviceEntry { SDL_AudioDeviceID id; std::string name; };
    std::vector<AudioDeviceEntry> audioDevices_;
    SDL_AudioDeviceID audioDeviceId_ = SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK;
    void enumerateAudioDevices();

    // Timeline view (frames <-> pixels)
    double viewStart_ = 0.0;     // timeline frame at the left edge
    double framesPerPx_ = 0.25;
    bool viewInitialized_ = false;
    bool pendingFit_ = false;    // a load fit before any layout ran (contentW_==0,
                                 // e.g. a project/otio passed on the command line);
                                 // computeLayout() re-fits once the width is known
    float viewContentW_ = 0.0f;  // contentW_ the current zoom was fit to; used to
                                 // rescale framesPerPx_ when the width changes
    bool scrubbing_ = false;
    // Shift+drag on the ruler paints the playback range instead of scrubbing: the
    // press anchors the in point and the drag trails the out point, leaving the
    // playhead alone. A press-release that never moved clears the range, so the
    // gesture undoes itself rather than leaving a one-frame loop behind.
    bool rangeDragging_ = false;
    bool rangeDragMoved_ = false;
    int64_t rangeDragAnchor_ = 0;
    bool panning_ = false;
    // Middle-drag over the grid. The grid has no zoomed frame to pan, so the drag
    // scrolls the grid instead: the content follows the hand.
    bool gridPanning_ = false;
    int hoveredClipId_ = -1;

    // Frame view (player image) zoom/pan. Global — applies to every clip — and
    // deliberately not persisted in the project. frameZoom_ == 1.0 is fit-to-window;
    // the pan offset is in screen pixels on top of the centered fit position.
    float frameZoom_ = 1.0f;
    float framePanX_ = 0.0f, framePanY_ = 0.0f;
    bool framePanning_ = false; // middle-mouse drag in the player area
    // A left press over the frame is one of two gestures, told apart by whether
    // the cursor moves: a drag jogs the playhead (pausing playback as soon as it
    // does), a press-release in place toggles play/pause. So the press only arms,
    // and the first frame's worth of horizontal travel promotes it to a jog.
    // The jog is anchored to the frame it was promoted on plus the travel since,
    // so dragging back to that point lands on it again rather than accumulating
    // rounding drift.
    bool playerClickArmed_ = false; // press seen over the frame, not yet a jog
    bool playerScrubbing_ = false;  // promoted: the drag is jogging the playhead
    bool playerClickActivating_ = false; // the press also re-activated the window
    Uint64 focusGainedMs_ = 0;      // when the window last took keyboard focus
    int64_t playerScrubStart_ = 0;  // playhead when the jog was promoted
    float playerScrubDx_ = 0.0f;    // horizontal pixels travelled since the press
    // Exposure / gamma "virtual slider": hold E and left-drag over the frame —
    // horizontal scrubs exposure in stops, vertical scrubs gamma. Holding E is
    // what keeps this off the jog gesture, so the press is only claimed while the
    // key is down. Tapping E with no drag toggles the bypass instead, which is
    // told apart from a scrub by expScrubbed_ (set on the press, cleared on the
    // key release). See the exposure* members in App_Grade.cpp.
    bool expHeld_ = false;      // E is down
    bool expDragging_ = false;  // left button down in a scrub
    bool expScrubbed_ = false;  // a scrub happened during this hold: the release is not a tap
    float expScrubDx_ = 0.0f, expScrubDy_ = 0.0f; // travel since the press
    float expScrubGain0_ = 0.0f, expScrubGamma0_ = 1.0f; // values when the scrub began
    // Bypass toggle: the values stashed while it is off, so the next tap restores
    // them. Covers both, since one gesture sets both.
    bool expBypassed_ = false;
    float expBypassGain_ = 0.0f, expBypassGamma_ = 1.0f;
    bool tlHoverActive_ = false; // cursor over the scrub zone (ruler): show playhead preview
    float tlHoverX_ = 0.0f;      // cursor x while tlHoverActive_
    // ── Ruler hover frame-preview thumbnail (Settings-gated, pause only) ──────
    // A ~128px-wide thumbnail drawn above the faded hover indicator showing the
    // frame under the cursor. Frames are resolved at most once per 5px of cursor
    // travel, then either lifted from the resident full-res FrameCache or decoded
    // off the main thread, downscaled once, and kept in a small in-memory LRU
    // keyed by (mediaId, sourceFrame). Color conversion is skipped — the source's
    // baked 8-bit RGBA is used directly. Cleared on project change.
    struct PreviewThumb { std::vector<uint8_t> rgba; int w = 0, h = 0; };
    std::unordered_map<CacheKey, PreviewThumb, CacheKeyHash> previewCache_;
    std::vector<CacheKey> previewLru_;                            // MRU at back
    std::unordered_set<CacheKey, CacheKeyHash> previewInflight_;  // decode in flight
    CacheKey previewKey_;                 // frame wanted under the cursor
    int previewClipId_ = -1;              // clip under the cursor at last resolve
    bool previewHasKey_ = false;
    float previewLastResolveX_ = 0.0f;    // cursor x at last resolve (5px throttle)
    bool previewResolvedOnce_ = false;
    uint64_t previewGen_ = 0;             // bumped on project change; drops stale results
    SDL_Texture* previewTex_ = nullptr;   // reusable upload target
    int previewTexW_ = 0, previewTexH_ = 0;
    CacheKey previewDisplayedKey_;        // key currently uploaded to previewTex_
    int previewDisplayedClipId_ = -1;     // clip the uploaded thumbnail belongs to
    bool previewDisplayedValid_ = false;
    // Slow-network guard: set by a preview worker when a single frame read exceeds
    // kPreviewSlowMs. Once latched, ensurePreviewThumb() stops decoding on demand
    // and only shows previews for frames already resident in the playback cache, so
    // scrubbing over network media never leaves blocking reads hanging. Reset on
    // project change (clearFramePreview).
    std::atomic<bool> previewSlowAccess_{ false };
    // Thumbnail keys the hover preview has already offered a frame for. Keeps a
    // scrub from re-probing the disk (and re-submitting a job) every 5px once a
    // clip's Overview/SOURCES thumbnails have been dealt with.
    std::unordered_set<std::string> previewThumbWritten_;

    static constexpr int kPreviewW = 128;   // thumbnail width in px
    static constexpr size_t kPreviewMax = 64; // LRU entry cap
    static constexpr int64_t kPreviewSlowMs = 500; // read slower than this → degrade

    void updateFramePreview();            // resolve hovered frame + kick decode
    void ensurePreviewThumb(const CacheKey& key, std::shared_ptr<Media> media,
                            int64_t srcFrame, const std::string& clipKey,
                            const std::string& srcKey);
    // Reuse a hovered-frame decode to fill in missing Overview / SOURCES thumbnails.
    void fillThumbsFromPreviewFrame(FramePtr frame, const std::string& clipKey,
                                    const std::string& srcKey);
    void storePreview(const CacheKey& key, PreviewThumb&& thumb); // insert + bound LRU
    void clearFramePreview();             // drop cache + reset (on project change)
    void renderFramePreview();            // draw the overlay above the ruler

    // Device pixels per logical unit: the display's content scale, or the UI Scale
    // preference when that is not Auto. Layout and input are in logical units and
    // the renderer scales them (SDL_SetRenderLogicalPresentation in computeLayout),
    // so the UI is laid out identically at any DPI; only the resolution it is drawn
    // at changes. Fonts are rasterized at this density so text stays crisp — see
    // setUiScale().
    float uiScale_ = 1.0f;
    // The UI Scale preference (settings panel, persisted): device pixels per logical
    // unit, or 0 for Auto — take the display's scale and follow it when the window
    // moves to another monitor.
    float uiScalePref_ = 0.0f;
    // Adopt `scale` as the device scale and re-rasterize the fonts for it. No-op if
    // the scale is unchanged. Needs a live renderer (it rebuilds the glyph caches).
    void setUiScale(float scale);
    // Push the 640x360-of-layout floor to SDL for the current uiScale_. Re-applied
    // whenever the scale changes, so the minimum stays a layout size rather than a
    // pixel size.
    void applyWindowMinimumSize();
    // (Re)rasterize the UI fonts at the current uiScale_. Also the way to rebind
    // them after the renderer is recreated.
    void reloadUiFonts();

    // SDL_GetMouseState in logical units. Event coordinates are already converted
    // (handleEvent), but a direct query returns window coordinates, which differ
    // from logical ones whenever uiScale_ != 1.
    void uiMouse(float& x, float& y) const;

    // Layout, recomputed every frame — in logical units, not device pixels.
    float winW_ = 0, winH_ = 0;
    SDL_FRect playerRect_{}, tlRect_{}, infoRect_{}, rulerRect_{}, cacheStripRect_{}, seqBarRect_{}, shotBarRect_{};
    // Flattened clip band, above the cache strip and the same height: compact mode
    // only, where it is the only thing left showing where the cuts are. Zero-height
    // otherwise, which is how the render pass reads "not this frame".
    SDL_FRect clipStripRect_{};
    SDL_FRect prevClipBtnRect_{}, playBtnRect_{}, nextClipBtnRect_{}; // transport buttons, centered in the info bar
    int hoveredTransport_ = -1;     // 0=prev clip, 1=play/pause, 2=next clip; -1 = none
    // Master volume, at the right end of the info bar: a speaker button that
    // toggles mute, then a horizontal slider. volume_ survives a mute so unmuting
    // restores the level; both are persisted through writePrefs().
    SDL_FRect volumeBtnRect_{};     // speaker (mute toggle)
    SDL_FRect volumeSliderRect_{};  // slider hit region (taller than the drawn track)
    bool hoveredVolumeBtn_ = false;
    bool hoveredVolumeSlider_ = false;
    bool volumeDragging_ = false;
    float volume_ = 1.0f;           // master output gain, 0..1
    bool muted_ = false;
    void setVolume(float v);        // clamp, unmute, push to the AudioEngine
    void toggleMute();
    bool helpOpen_ = false;          // keyboard-shortcuts overlay panel visible
    bool aboutOpen_ = false;         // version / build-info overlay panel visible
    SDL_FRect magnetBtnRect_{};     // snap-to-clip toggle, left-aligned in the info bar
    bool hoveredMagnet_ = false;
    // Tool-mode pair, right of the snap toggle: an icon-only radio couple, drawn
    // and hit-tested like it (see App::drawFrame's info bar).
    SDL_FRect cursorToolBtnRect_{};
    SDL_FRect razorToolBtnRect_{};
    bool hoveredCursorTool_ = false;
    bool hoveredRazorTool_ = false;
    int hoveredTrackMenu_ = -1;     // track whose header burger is hovered, else -1
    // Top toolbar: a thin bar between the title bar and the video frame, spanning
    // the frame width (right of the left panels). Hosts the Letterbox button.
    SDL_FRect topBarRect_{};
    // OCIO Display: a button in the top toolbar showing the active display space;
    // clicking opens a popup listing the config's available displays. See App_Letterbox.cpp.
    SDL_FRect ocioDisplayBtnRect_{};         // "Display" button, in the top toolbar
    bool hoveredOcioDisplay_ = false;
    bool ocioDisplayMenuOpen_ = false;       // popup visible
    SDL_FRect ocioDisplayMenuRect_{};        // popup panel rect (set each frame in renderOcioDisplayMenu)
    // Scratch frames for a dissolve whose halves are read in different colour
    // spaces: each is transformed on its own before the two are blended.
    Frame ocioMixFrameB_;

    std::vector<SDL_FRect> ocioDisplayRowRects_; // display-row hit rects, parallel to ocio_.displays()
    int ocioDisplayHoverRow_ = -1;           // hovered display row, -1 = none
    // OCIO View Transform: a button in the top toolbar (right of Display Space)
    // showing the active view; clicking opens a popup listing the current display's
    // views. Styled like the Display Space button. See App_Letterbox.cpp.
    SDL_FRect ocioViewBtnRect_{};            // "View" button, in the top toolbar
    bool hoveredOcioView_ = false;
    bool ocioViewMenuOpen_ = false;          // popup visible
    SDL_FRect ocioViewMenuRect_{};           // popup panel rect (set each frame in renderOcioViewMenu)
    std::vector<SDL_FRect> ocioViewRowRects_; // view-row hit rects, parallel to ocio_.views(display)
    int ocioViewHoverRow_ = -1;              // hovered view row, -1 = none
    // OCIO File Colorspace: a button in the top toolbar (right of View Transform)
    // showing the active input color space; clicking opens a popup listing the
    // config's color spaces. Styled like the Display Space button. See App_Letterbox.cpp.
    SDL_FRect ocioInputCsBtnRect_{};         // "File Colorspace" button, in the top toolbar
    bool hoveredOcioInputCs_ = false;
    bool ocioInputCsMenuOpen_ = false;       // popup visible
    SDL_FRect ocioInputCsMenuRect_{};        // popup panel rect (set each frame in renderOcioInputCsMenu)
    std::vector<SDL_FRect> ocioInputCsRowRects_; // colorspace-row hit rects, parallel to ocio_.colorSpaces()
    int ocioInputCsHoverRow_ = -1;           // hovered colorspace row, -1 = none
    // OCIO Look: a button in the top toolbar (between View Transform and File
    // Colorspace) showing the active Look; clicking opens a popup listing "None" +
    // the config's named Looks. Styled like the Display Space button. See App_Letterbox.cpp.
    SDL_FRect ocioLookBtnRect_{};            // "Look" button, in the top toolbar
    bool hoveredOcioLook_ = false;
    bool ocioLookMenuOpen_ = false;          // popup visible
    SDL_FRect ocioLookMenuRect_{};           // popup panel rect (set each frame in renderOcioLookMenu)
    std::vector<SDL_FRect> ocioLookRowRects_; // look-row hit rects, parallel to the "None" + ocio_.looks() list
    int ocioLookHoverRow_ = -1;              // hovered look row, -1 = none
    // Shared scroll offset for whichever OCIO list popup is open (only one is open
    // at a time); reset to 0 when a popup is opened. Its scrollbar is grabbable,
    // and shares one drag state for the same reason.
    float ocioMenuScroll_ = 0.0f;
    ScrollbarDrag ocioMenuSb_;
    // Sequence view filter: a left-aligned button in the top toolbar whose label
    // shows the selected sequence; clicking opens a popup listing every
    // sequence. Replaces the former "Sequence" menu-bar menu.
    SDL_FRect sequenceBtnRect_{};            // "Sequence" button, left of the top toolbar
    bool hoveredSequence_ = false;
    bool sequenceMenuOpen_ = false;          // popup visible
    SDL_FRect sequenceMenuRect_{};           // popup panel rect (set each frame in renderSequenceMenu)
    // One row of the popup. The rows are built when it opens (buildSequenceMenuRows)
    // rather than derived per frame, because what a click does now depends on the
    // row kind: a clickable header per project opened whole this session with that
    // project's sequences indented under it, then the sequences belonging to no
    // opened project. `rect` is filled in by renderSequenceMenu and hit-tested by
    // sequenceMenuHandleEvent. Opening something not loaded yet is not in here — the
    // two top-bar buttons below do that.
    struct SeqMenuRow {
        enum class Kind { Project, Sequence };
        Kind kind = Kind::Sequence;
        std::string label;
        int seqIdx = -1;     // Kind::Sequence: index into timeline_.sequences
        int projIdx = -1;    // Kind::Project: index into seqMenuProjects_
        bool indent = false; // a sequence listed under a project header
        SDL_FRect rect{};
    };
    std::vector<SeqMenuRow> sequenceMenuRows_;
    // The projects the loaded sequences belong to, in first-seen order. Both are
    // held by stable id — Timeline::SourceProject::id and Sequence::id — so neither
    // a reorder nor two same-named projects can repoint a scope.
    struct SeqMenuProject { int id = -1; std::string name; std::vector<int> seqIds; };
    std::vector<SeqMenuProject> seqMenuProjects_;
    int sequenceHoverRow_ = -1;              // hovered popup row, -1 = none
    // Scroll state for the popup, which caps its height to the window like the
    // OCIO list popups do and scrolls when the rows don't fit. sequenceScrollToRow_
    // is set by keyboard nav and consumed by the next render, so arrowing past the
    // viewport edge brings the row into view instead of losing it.
    float sequenceMenuScroll_ = 0.0f;
    ScrollbarDrag sequenceMenuSb_;
    int sequenceScrollToRow_ = -1;
    // What the clip under the playhead can open, refreshed every frame by
    // resolveOpenTargets and read by the two top-bar buttons below. An empty name
    // means that one is not on offer: nothing resolved, no published project
    // document, or the view is already showing it.
    std::string openSeqName_;   // sequence the clip's naming resolves to, and isn't in
    std::string openProjName_;  // project the clip's path resolves to, not the scoped view
    std::string openProjPath_;  // that project's .jpproj/.otio path, verified to exist
    // "Open Project: <name>" / "Open Sequence: <name>" buttons, left of the Sequence
    // button in the top toolbar: the only way in to something the project hasn't
    // loaded yet. Absent whenever the names above are empty.
    SDL_FRect openProjBtnRect_{};
    SDL_FRect openSeqBtnRect_{};
    bool hoveredOpenProj_ = false;
    bool hoveredOpenSeq_ = false;
    // Resolving those names is an embedded-interpreter call plus a stat on a network
    // path, so each media path is resolved once and remembered here. This holds only
    // what the naming convention and the disk say; whether a target is still worth
    // offering (sequence already grafted, project already opened whole) is timeline
    // state and is re-checked every frame instead. A path that resolved to nothing
    // stays negative for the session — publishing a project while jplay runs won't be
    // picked up until a restart.
    struct OpenTarget {
        std::string projPath;  // the project file, verified on disk; empty = nothing to open
        std::string projName;  // its stem
        std::string seqName;   // the naming convention's sequence for that media
    };
    std::unordered_map<std::string, OpenTarget> openTargetCache_; // media path -> the above
    // Proxy: a toggle-label button in the top toolbar, immediately left of
    // Letterbox, that opens a popup for picking the global media-representation
    // mode: the built-in "Full" plus whatever the naming config's
    // list_proxy_modes() callback supplies. The chosen mode lives on the
    // Timeline (per-project) and is mirrored into jplay::setProxyMode, which
    // drives what every open Media actually decodes (see ProxyMode.h). See
    // App_ProxyMenu.cpp.
    // Distinct from PythonBridge.h's ProxyModeOption (the raw callback result);
    // buildProxyModes() copies from one to the other, "Full" prepended.
    struct ProxyModeEntry { std::string value; std::string label; int64_t slate = 0;
                            bool isDefault = false; };
    SDL_FRect proxyBtnRect_{};       // "Proxy" button, in the top toolbar
    bool hoveredProxy_ = false;
    bool proxyMenuOpen_ = false;     // popup visible
    SDL_FRect proxyMenuRect_{};      // popup panel rect (set each frame in renderProxyMenu)
    std::vector<SDL_FRect> proxyRowRects_; // mode-row hit rects, parallel to proxyModes_
    int proxyHoverRow_ = -1;         // hovered mode row, -1 = none
    float proxyMenuScroll_ = 0.0f;
    ScrollbarDrag proxyMenuSb_;
    // list_proxy_modes() is consulted at most once per run, and only on the
    // dropdown's first open — never at startup — so a site with no proxy
    // config pays nothing for this feature. "Full" (value "") is always first
    // and is never supplied by Python.
    bool proxyModesBuilt_ = false;
    std::vector<ProxyModeEntry> proxyModes_;
    // Letterbox: a toggle-label button in the top toolbar that opens a popup for
    // picking the matte aspect ratio (presets + custom entry) and bar opacity.
    // The chosen ratio/opacity live on the Timeline (per-project). See App_Letterbox.cpp.
    SDL_FRect letterboxBtnRect_{};  // "Letterbox" button, in the top toolbar
    bool hoveredLetterbox_ = false;
    bool letterboxMenuOpen_ = false;         // popup visible
    SDL_FRect letterboxMenuRect_{};          // popup panel rect (set each frame in renderLetterboxMenu)
    std::vector<SDL_FRect> letterboxRowRects_; // preset-row hit rects, parallel to the preset table
    int letterboxHoverRow_ = -1;             // hovered preset row, -1 = none
    SDL_FRect letterboxFldBox_{};            // custom-ratio field box
    TextInput letterboxFld_;                 // custom aspect-ratio entry ("2.39" or "4:3")
    // "Fit View": scale the image so the matte's visible region fills the view and
    // the bars fall outside it, i.e. the framing shows with no black edges. A view
    // preference like the frame zoom, so it is not persisted in the project — but it
    // is host-driven in a sync session, since the framing depends on it.
    bool letterboxFitView_ = false;
    SDL_FRect letterboxFitRowRect_{};        // "Fit View" checkbox row hit rect
    bool letterboxFitHover_ = false;         // cursor over that row
    SDL_FRect letterboxOpacityRect_{};       // opacity slider hit region
    bool letterboxOpacityDrag_ = false;      // opacity slider drag in progress
    std::vector<uint8_t> letterboxOutBuf_;   // native-res program copy with bars baked, for NDI/SDI
    std::vector<Imath::half> letterboxOutBufHdr_; // same, for the scRGB half program image
    // Pencil annotation: white markup drawn over the frame. Strokes are stored in
    // image coordinates so they track the frame under zoom/pan; per-point width
    // simulates pencil pressure from cursor speed, with tapered start/end tips (see
    // App_Player.cpp). annotStrokes_ is the live edit buffer for one displayed
    // frame; it is flushed into / loaded from the owning clip's per-source-frame
    // annotation map (Clip::annotations) as the playhead moves, so markup persists
    // and is saved with the project. See syncAnnotBuffer/flushAnnotBuffer.
    std::vector<AnnotStroke> annotStrokes_;
    bool annotDrawing_ = false;      // a stroke is in progress
    int annotClipId_ = -1;           // clip the live buffer belongs to (-1 = none)
    int64_t annotFrame_ = 0;         // source frame the live buffer belongs to
    float annotLastSX_ = 0, annotLastSY_ = 0;      // last sample (screen px), for velocity
    uint64_t annotLastT_ = 0;        // last sample time (ms)
    float annotSmoothHw_ = 0;        // smoothed screen half-width (px)
    // Draw-tool panel: shown left of the frame while the Draw pane is open. Holds a
    // Clear button, a Pencil Size slider (a multiplier on the speed-based width),
    // and a hue wheel that sets the global stroke color for all markup.
    float pencilSizeMul_ = 1.0f;                   // Pencil Size: multiplies the pressure-sim width
    // Color wheel state: a filled HSL disk. Angle picks hue; radius picks lightness
    // (white at the center, black at the outer edge) with saturation fixed at 100%.
    // Default is white (lightness 1) to match prior markup.
    float pencilHue_ = 0.0f;         // 0..360 degrees
    float pencilLight_ = 1.0f;       // 0..1 (1 = white center, 0 = black edge)
    SDL_FRect drawPanelRect_{};      // whole panel column (set in layout; 0 width when hidden)
    SDL_FRect drawClearRect_{};      // Clear button hit region
    SDL_FRect drawSizeRect_{};       // Pencil Size slider hit region
    SDL_FRect drawHueRect_{};        // color wheel hit region (square bounding box)
    bool drawDragSize_ = false;      // dragging the size slider
    bool drawDragWheel_ = false;     // dragging inside the color wheel
    bool hoveredDrawClear_ = false;
    bool pencilOverFrame_ = false;   // pencil mode on and cursor over the frame: show crosshair
    float headerX_ = 0;     // left edge of the timeline content (after the track gutter)
    float contentW_ = 0;    // width of the timeline content area
    float tracksTop_ = 0;   // y of the first (topmost) track row, just below the ruler/bars
    // Cached row-top y for each track (size trackCount+1; last entry = bottom of
    // the stack). Rebuilt once per computeLayout so trackRowY() is an O(1) lookup
    // instead of an O(tracks·clips) resum in the per-clip render path.
    std::vector<float> trackRowTop_;
    // The heights those tops were summed from, cached for the same reason and
    // rebuilt in the same place: a row's height comes from its type, which
    // Timeline::trackKind derives by scanning every clip, so asking per clip
    // costs tracks·clips a frame. Empty until the first computeLayout.
    std::vector<float> trackRowH_;

    // The track stack is a scrolling viewport rather than something the timeline
    // grows to fit: the timeline takes at most kTimelineMaxFrac of the window, and
    // its top edge can be dragged shorter still, so a project with more tracks than
    // that scrolls instead of pushing the player off the screen.
    //
    // tracksTop_ is the *viewport's* top; the rows start trackScroll_ above it,
    // which trackRowY() already folds in — so every hit test and draw that goes
    // through it scrolls for free. What does need saying is that the bottom of the
    // track area is tracksViewBottom(), not tracksTop_ + tracksTotalH(): those two
    // are only the same when nothing is scrolled away.
    float tracksViewH_ = 0.0f;   // visible height of the track area
    float trackScroll_ = 0.0f;   // how far the stack is scrolled up (0 = at the top)
    ScrollbarDrag trackSb_;      // its scrollbar, grabbable
    // Timeline height: 0 = auto (as tall as its content, capped at 40% of the
    // window), otherwise the height the user dragged the top edge to. tlMinH_ /
    // tlMaxH_ are that clamp, published by computeLayout so the drag can apply the
    // same limits live — a drag that stored an out-of-range height would spring the
    // timeline open again the moment a track was added.
    float tlUserH_ = 0.0f;
    float tlMinH_ = 0.0f, tlMaxH_ = 0.0f;
    bool tlResizing_ = false;        // dragging the timeline's top edge
    bool tlResizeHovered_ = false;   // cursor over that edge

    // Zoom-fit control in the info bar, right of "Snap": a "Fit To" button that
    // drops fitMenu_ with all four fits on hover and does nothing else — the list
    // is one cursor-travel away instead of behind a caret of its own, and the
    // button itself is a label for it rather than a fit of its own. The fits are
    // recomputed every frame (layoutFitButtons); a fit that would leave the view
    // where it already is reads as disabled, which greys its row in the menu.
    enum class FitBtnKind { Scope, Sequence, Range, Clip };
    struct FitBtn {
        FitBtnKind kind = FitBtnKind::Scope;
        const char* label = "";    // menu row text
        const char* shortcut = ""; // key hint, right-aligned in the row
        int64_t a = 0, b = 0;    // the frame range this fit applies
        int seqIdx = -1;         // Sequence only: which sequence a/b came from
        bool enabled = false;    // false: this fit is where the view already is
    };
    std::vector<FitBtn> fitBtns_;
    bool hoveredFit_ = false;
    SDL_FRect fitBarBand_{};   // info-bar slot right of "Snap", sized for the button below
    SDL_FRect fitBtnRect_{};   // "Zoom To": hover drops fitMenu_
    // Shared by computeLayout (which reserves the slot) and drawFitButtons (which
    // fills it), so the reservation and the drawing can never disagree on width.
    static constexpr const char* kFitBtnLabel = "Zoom To";

    // Left panels: a fixed 64px icon strip, plus an optional ProjectExplorer that
    // expands to its right. Both run from under the title bar down to the top of
    // the (full-width) timeline; the player and metadata bar start at
    // panelsRight_ (their left edge) instead of x=0. The timeline itself always
    // spans the full window width along the bottom, so the panels stop at
    // panelsBottom_ rather than the window bottom.
    float panelsBottom_ = 0;           // bottom edge of the left panels (= timeline top)

    // ── The left panes ───────────────────────────────────────────────────────
    // The icon strip down the left edge, and the mutually-exclusive panels its
    // toggles open. leftPanels_ is the registry (built by registerLeftPanels in
    // App_NavPanel.cpp); openPanel_ indexes it, or is -1 when no pane is open
    // and the stage has the full width.
    //
    // The panes being mutually exclusive is why this is one index rather than a
    // bool apiece: opening a pane cannot forget to close another, and code that
    // asks "is anything open" does not have to enumerate them. Adding a pane
    // means appending one registry entry -- see docs/ADDING_A_PANEL.md.
    std::vector<jplay::LeftPanelDesc> leftPanels_;
    int openPanel_ = -1;

    // Registry indices of the built-in panes, in the order registerLeftPanels()
    // appends them; kept in step by an assert there. Only panes that code
    // outside their own translation unit has to name need an entry, which for a
    // pane a fork appends is usually none of them.
    enum LeftPanelId : int {
        kPanelProjectExplorer = 0,
        kPanelClipSource,
        kPanelGrade,
        kPanelTech,
        kPanelDraw,
        kPanelSync,
        kPanelSettings,
        kPanelBuiltinCount,
    };

    void registerLeftPanels();                  // builds leftPanels_; App_NavPanel.cpp
    bool panelOpen(int id) const { return openPanel_ == id; }
    void openLeftPanel(int id);                 // -1 closes; closes whatever was open first
    void toggleLeftPanel(int id);               // a pane's own toggle: closes it if open
    int  leftPanelAt(float mx, float my) const; // strip button under a point; -1 = none
    void layoutLeftPanelStrip(float topH);      // places every toggle down the strip
    float leftPaneWidth(int id) const;          // pane's open width, drag override applied
    float openPanelW() const;                   // width of the open pane; 0 if none
    SDL_FRect leftPaneRect() const;             // the open pane's bounds
    void renderLeftPanel();                     // the open pane draws itself
    // Chrome every pane shares: the fill and the right edge (resize accent or
    // divider). Returns the pane's rect to lay content into.
    SDL_FRect beginLeftPanel();
    // Cuts the header row off `body`, draws `title` in it, and returns the row
    // so controls can be right-aligned into what is left.
    SDL_FRect leftPanelHeader(SDL_FRect& body, const char* title);

    // Edge-drag resize. Only a pane whose descriptor says `resizable` has an
    // edge; at most one pane is open, so at most one edge exists at a time.
    int panelResizeEdgeAt(float mx, float my) const; // pane index, or -1
    int panelResizing_ = -1;      // pane being drag-resized; -1 = none
    int panelResizeHover_ = -1;   // pane whose edge the cursor is over; -1 = none
    bool panelResizeActive(int id) const {
        return panelResizing_ == id || panelResizeHover_ == id;
    }

    // Which side-panel resize edge a point falls on, if any. The zone straddles
    // the edge (kOverviewHandleHitW/2 into each side), so it reaches in over the
    // panel's own scrollbar, which sits right against that edge: the scrollbar
    // press asks this first and leaves those pixels to the resize.
    float panelsRight_ = 0;            // right edge of the left panels (= playerRect_.x)
    std::vector<std::string> selectedSourcePaths_; // ProjectExplorer rows selected for removal
    std::string selectionAnchorPath_;  // anchor row for Shift-range selection
    // Which selection the Delete key acts on. A source row and a timeline clip can
    // be selected at the same time, so the one the user touched last owns the key:
    // set when a source row is clicked (clickExplorerRow), cleared by any timeline
    // clip / dissolve / gap selection (clearClipSelection and friends).
    bool binSelectionActive_ = false;
    SDL_FRect peAddRect_{}, peRemoveRect_{}; // ProjectExplorer source + / - buttons
    SDL_FRect peSeqAddRect_{}, peSeqRemoveRect_{}; // sequence add / remove buttons
    struct ExplorerRow { std::string path; SDL_FRect rect; };
    std::vector<ExplorerRow> explorerRows_;  // source rows, rebuilt each render

    // PROJECT panel tabs: split the SOURCES bin and the SEQUENCES tree onto two
    // tabs so only one section shows at a time. 0 = SOURCES, 1 = SEQUENCES.
    enum { PeTabSources = 0, PeTabSequences = 1 };
    int peActiveTab_ = PeTabSources;
    SDL_FRect peTabSources_{}, peTabSequences_{};

    // Project Explorer tree: collapsible SEQUENCES ▸ shots above the SOURCES bin.
    // Rows are rebuilt each render for hit-testing; field rects drive inline edit.
    struct PeRow {
        enum class Kind { Seq, Shot } kind = Kind::Seq;
        int id = -1;            // sequence id (Seq) or shot id (Shot)
        int seqId = -1;         // owning sequence id (Shot rows)
        SDL_FRect rect{};       // whole row
        SDL_FRect caret{};      // expand/collapse caret (Seq)
        SDL_FRect fColor{};     // background-color swatch button (Seq)
        SDL_FRect fEye{};       // focus-in-timeline eye button (Seq)
        SDL_FRect fName{};      // editable name field
        SDL_FRect fA{}, fB{};   // start / end fields
        SDL_FRect fCutIn{}, fCutOut{}; // cut in / out fields (Shot)
    };
    std::vector<PeRow> peRows_;
    std::set<int> peCollapsedSeqs_;          // collapsed sequence ids
    float peSeqScroll_ = 0.0f;               // SEQUENCES tree vertical scroll
    ScrollbarDrag peSeqSb_;                  // its scrollbar, grabbable
    float peSeqListTop_ = 0.0f;              // scrollable band top (below the fixed header)
    float peSeqListBottom_ = 0.0f;           // scrollable band bottom
    // Scrollable band of the SOURCES list, rebuilt each render. A click inside it
    // that misses every row is a click on empty bin space, and drops the selection.
    SDL_FRect peSourceBand_{};
    float peSourceScroll_ = 0.0f;            // SOURCES list vertical scroll
    // Set by the arrow keys to a path the next render must scroll into view (the
    // keyboard moves the selection through binGroups(), which knows nothing about
    // where the rows land). Cleared once that render has honoured it.
    std::string peRevealPath_;
    ScrollbarDrag peSourceSb_;               // its scrollbar, grabbable
    // Inline edit: a single focused TextInput over one field at a time.
    enum class PeEdit { None, SeqName, ShotName, ShotStart, ShotEnd, ShotCutIn, ShotCutOut };
    PeEdit peEdit_ = PeEdit::None;
    int peEditId_ = -1;                      // sequence or shot id being edited
    TextInput peEditField_;
    SDL_FRect peEditRect_{};
    // Drag-to-reorder: sequences (anywhere in the tree) or shots (within their
    // owning sequence only). Exactly one of the two ids is set at a time.
    int   peDragSeqId_ = -1;                 // sequence row being dragged, or -1
    int   peDragShotId_ = -1;                // shot row being dragged, or -1
    int   peDragShotSeqId_ = -1;             // owning sequence id of the dragged shot
    bool  peDragOnName_ = false;             // seq drag armed on the name (plain click toggles collapse)
    float peDragY_ = 0.0f;                   // current cursor y during a reorder drag
    float peDragPressY_ = 0.0f;              // press y, for the move threshold
    bool  peDragActive_ = false;             // moved past the click threshold

    // SOURCES filter: a "Filter:" entry under the section header narrows the bin to
    // sources whose displayed name contains the text (case-insensitive). Transient
    // UI state — never persisted, and cleared by the X button inside the field.
    TextInput peFilterFld_;
    SDL_FRect peFilterClearRect_{};          // X (clear) button, inside the field's right edge
    // "Filter on Timeline Visible" toggle, right of the field: narrows the bin to
    // the sources with a clip on screen in the timeline right now — the in-view
    // sequences' clips overlapping the visible frame range — so panning or zooming
    // the timeline re-lists the bin. Transient like the text filter above.
    bool peFilterTlVisible_ = false;
    SDL_FRect peFilterTlVisibleRect_{};      // toggle button, right of the field

    // SOURCES bin: a single "Size:" slider controls the media-bin presentation.
    // At the minimum it is a plain name list (no thumbnails generated); larger it
    // becomes thumbnail rows and then an Overview-style grid. The size persists in
    // UserData prefs. Thumbnails are generated in the background (sourceThumbs_,
    // the same on-disk cache the Overview pane uses) keyed per source; the pane is
    // mutually exclusive with the Overview pane, so the two caches/texture maps
    // are never live at once.
    float peThumbSize_ = 16.0f;              // media-bin thumbnail size, device px (min = names only)
    SDL_FRect peSizeSliderRect_{};           // Size: slider track
    bool  peSizeDragging_ = false;           // dragging the Size slider handle
    // Bin sort order, chosen from the icon button right of the Size slider. Name
    // is alphabetical; Sequence (the default) splits the bin into a labelled
    // section per sequence (see binGroups). The enum values are what UserData
    // prefs store, so keep their order.
    enum class PeSort { Name, Sequence };
    PeSort peSort_ = PeSort::Sequence;
    SDL_FRect peSortBtnRect_{};              // sort icon button, right of the Size slider
    ContextMenu peSortMenu_;                 // sort-order popup, drops below the button
    // "Go To Current" target button, left of the sort button. The click only sets
    // the pending flag; the scroll is applied by the next render, which is where
    // the bin's row/grid geometry (and so the target's offset) is known.
    SDL_FRect peGoToCurrentRect_{};
    bool peGoToCurrentPending_ = false;
    // In Sequence order the bin sections collapse the way the SEQUENCES rows do:
    // a caret on the section header hides that sequence's sources. Keyed by header
    // text — the sections are rebuilt from the timeline each frame and carry no id.
    std::set<std::string> peCollapsedBinGroups_;
    struct BinHeaderRow { std::string header; SDL_FRect rect{}; };
    std::vector<BinHeaderRow> peBinHeaders_; // section header rows, rebuilt each render
    ContextMenu peMediaMenu_;                // right-click popup on a bin row (copy path / reveal)
    ContextMenu peSeqColorMenu_;             // sequence background-color palette popup
    ThumbnailCache sourceThumbs_;
    std::unordered_map<std::string, SDL_Texture*> sourceThumbTex_; // key -> texture
    std::vector<std::string> sourceThumbGen_; // source paths last handed to sourceThumbs_.start()
    // Only the cells the bin actually draws are thumbnailed, so a large media pool
    // doesn't decode a frame per source for cells that aren't on screen.
    // sourceThumbGen_ above is the drawn set the current run covers;
    // sourceThumbPending_ holds the drawn set waiting out the settle delay, so a
    // run of wheel notches restarts the workers once at the end, not on every notch.
    std::vector<std::string> sourceThumbPending_;
    Uint64 sourceThumbPendingMs_ = 0;

    // Shared per-clip thumbnail cache: 128px BMP tiles generated in the background
    // (thumbs_) and lazily loaded into textures (thumbTex_). Feeds the grid view.
    ThumbnailCache thumbs_;
    std::unordered_map<std::string, SDL_Texture*> thumbTex_; // key -> loaded thumbnail texture

    // Only the tiles the grid actually draws are thumbnailed, so a long timeline
    // doesn't decode a frame per clip for cells that aren't on screen.
    // thumbStartedIds_ is the drawn set the current run covers; thumbPending*
    // hold the drawn set waiting out the settle delay, so a run of wheel notches
    // restarts the workers once at the end rather than on every notch.
    std::vector<int> thumbStartedIds_;
    std::vector<int> thumbPendingIds_;
    Uint64 thumbPendingMs_ = 0;

    // When each key was last found missing on disk, so a tile the workers have yet
    // to write is probed at this interval rather than once per drawn frame — a grid
    // of placeholders would otherwise stat every tile every frame, which is felt as
    // soon as the thumbnail directory lives on a share. Shared by the grid and the
    // bin; their key spaces are disjoint.
    static constexpr Uint64 kThumbProbeMs = 250;
    std::unordered_map<std::string, Uint64> thumbMissAt_;

    // What the player stage shows. The three are mutually exclusive — each owns
    // playerRect_ outright — so they are one value rather than a flag each.
    //   Frame    — the single program image; the player as it has always been.
    //   Overview — TAB: a contact sheet of every clip in the viewed sequences,
    //              grouped by sequence (the grid* state below).
    //   Layout   — L: every track carrying a clip at the playhead, tiled, all
    //              playing live (the layout* state further down).
    //   Stack    — F5: the same comparison set as the Layout, but one image at a
    //              time — the top-most clip, with Up/Down rotating which that is
    //              (the stack* state further down).
    enum class PlayerStage { Frame, Overview, Layout, Stack };
    PlayerStage stage_ = PlayerStage::Frame;
    bool gridView() const { return stage_ == PlayerStage::Overview; }
    bool layoutView() const { return stage_ == PlayerStage::Layout; }
    bool stackView() const { return stage_ == PlayerStage::Stack; }
    // The two stages built on the comparison scratch sequence: they stand it up
    // the same way and differ only in how they show it, so switching between them
    // keeps the set rather than rebuilding it (see setPlayerStage).
    bool compareStage() const { return layoutView() || stackView(); }

    // Overview grid: the clip under the playhead plays live in its cell; the rest
    // use the thumbnail cache (thumbs_ / thumbTex_). The grid is independent of the
    // timeline's zoom and scroll: tiles are a fixed height and the grid scrolls
    // when they overflow. gridCells_ holds the tiles actually on screen, rebuilt
    // each render for hit-testing (a cell click jumps the playhead to that clip)
    // and as the set to thumbnail — offscreen rows are neither drawn nor decoded.
    struct GridCell { int clipId = 0; SDL_FRect rect{}; };
    std::vector<GridCell> gridCells_;

    // Tile height in device px, and the grid's scroll offset (px of content above
    // the top of the area, clamped by the render once the content height is
    // known). The height is the one size control: the wheel scrolls, ctrl+wheel
    // resizes, and the choice persists in UserData prefs.
    float gridThumbH_ = 60.0f;
    float gridScroll_ = 0.0f;
    // Pending "put this on screen" request, applied by the next render, which is
    // where the tile geometry is known: the clip under the playhead when the grid
    // opens.
    bool gridScrollToLive_ = false;
    // The clip the grid last saw under the playhead. When the playhead moves onto
    // a different one — scrubbing, stepping, playing — its row is scrolled into
    // view if it had fallen offscreen, by the smallest move that reveals it so a
    // settled grid is left where the user put it.
    int gridLiveClip_ = 0;
    // A tile resize rewraps every row, so tiles glide to their new slot instead of
    // jumping there. gridTileRects_ remembers where each was last drawn;
    // gridAnimMs_ is the previous render's tick, for a framerate-independent step.
    std::unordered_map<int, SDL_FRect> gridTileRects_;
    Uint64 gridAnimMs_ = 0;
    // Lets the grid's scrollbar be grabbed and dragged (see ScrollbarDrag).
    ScrollbarDrag gridSb_;

    // ---- Layout stage ---------------------------------------------------
    //
    // Every track carrying a visible clip at the playhead, tiled and all playing
    // live. The set is derived, never stored: sequences are concatenated end to
    // end (Timeline::repackSequences), so only one is live at any frame and the
    // tiles are simply that sequence's tracks, top first, bounded by trackCount.
    // Nothing is added to the project format — stacking frame-aligned versions on
    // separate tracks is already what addMediaAlignedToCurrentClip does, and this
    // is the view of it.
    //
    // Tile 0 (the top track) is the *program*: it is the clip renderPlayer builds
    // texture_ from, so it alone carries the annotations, the letterbox and
    // whatever the output devices and the review window are fed. The tiles below
    // it are comparison images and stop at the display transform — no readback, no
    // compositing (a dissolve on a lower track shows its outgoing clip only).
    struct LayoutSlot {
        SDL_Texture* tex = nullptr;
        int w = 0, h = 0;              // texture dimensions
        float pa = 1.0f;               // pixel aspect of the frame in it
        bool has = false;              // tex holds a rendered frame
        CacheKey key{};                // which frame that is (empty = none yet)
        int clipId = 0;                // clip it was built for
        std::string cs;                // OCIO colour space it was rendered through
    };
    // Slots for tiles 1..N-1, indexed by tile order (slot i serves tile i+1).
    // Kept across frames so a settled layout re-uploads nothing; freed with the
    // renderer (see freeLayoutSlots).
    std::vector<LayoutSlot> layoutSlots_;
    // This render's tile rects, top row first — the cells, not the fitted images.
    // Rebuilt every render and read by programDstRect / the zoom anchor / the click
    // hit-test. One per row of the stage, so the arrangement only changes when the
    // rows do, not as the playhead crosses in and out of clips.
    std::vector<SDL_FRect> layoutTiles_;
    // Clips the tiles show, parallel to layoutTiles_; -1 where the row has no clip
    // at this frame. Ids rather than pointers: an edit between renders can move the
    // clip vector out from under them.
    std::vector<int> layoutClipIds_;
    // Stack stage: when the basename overlay stops being drawn. Armed by each
    // cycleStack; 0 (or any tick in the past) means nothing is showing.
    Uint64 stackOverlayUntil_ = 0;
    // Tile the program occupies — the row its clip sits on, which is row 0 only
    // while row 0 has a frame at the playhead. renderPlayer draws that one tile
    // (from texture_, with the annotations and the letterbox); renderLayoutTiles
    // draws all the others.
    int layoutProgramTile_ = 0;
    // The program's cell. Only valid with tiles laid out; the clamp guards against
    // an index left over from a render whose row count differed.
    const SDL_FRect& layoutProgramCell() const {
        size_t i = layoutProgramTile_ > 0 ? (size_t)layoutProgramTile_ : 0;
        if (i >= layoutTiles_.size())
            i = layoutTiles_.size() - 1;
        return layoutTiles_[i];
    }

    // Which sequence / shot the view is currently zoom-fit to via a double-click
    // on its timeline bar (index / Shot::id, or -1 when none). Purely drives the
    // fit ↔ zoom-back-out toggle; unlike focus scoping these never filter what is
    // shown, and they are cleared on any manual zoom or pan since the view no
    // longer matches.
    int zoomFitSeqIdx_  = -1;
    int zoomFitShotId_  = -1;

    // Inspector panel: toggled with 'i'; shows sequence/shot/clip info for
    // whatever is active under the playhead. A translucent overlay centred on the
    // video frame, sized each frame to fit its own text (see renderInspector);
    // only its close (X) button takes clicks, the rest passes through to the frame.
    bool inspectorOpen_ = false;
    SDL_FRect inspectorRect_{};      // panel bounds (for the wheel hit-test)
    SDL_FRect inspectorCloseRect_{}; // close (X) button on the first header row
    float inspectorScroll_ = 0.0f;   // vertical scroll of the info list (px)
    // "Properties" on a source's right-click menu shows its file + media info in a
    // sub-panel at the bottom of the SOURCES tab (see renderSourceInfoPanel).
    // inspectMediaPath_ holds that source's path; empty means the sub-panel is
    // closed. The file facts below are cached and only recomputed when the
    // inspected source changes (avoids stat() every frame).
    std::string inspectMediaPath_;
    std::string inspectFileInfoPath_; // path the cached facts below apply to
    std::string inspectModTime_;      // filesystem last-write time "YYYY-MM-DD HH:MM"
    std::string inspectFileSize_;     // on-disk size, human-readable
    std::string inspectFrameRange_;   // EXR sequence frame range + count (else empty)
    float sourceInfoScroll_ = 0.0f;   // source-info sub-panel vertical scroll (px)
    float sourceInfoContentH_ = 0.0f; // last frame's sub-panel content height (for clamping)
    SDL_FRect sourceInfoRect_{};       // source-info sub-panel bounds (for wheel hit-test)
    SDL_FRect sourceInfoCloseRect_{};  // close (X) button on the sub-panel's MEDIA header row
    // The inspector shows only the timeline (playhead) view.
    bool inspectorVisible() const { return inspectorOpen_; }

    // Color grading panel (left-expand pane, mutually exclusive with the
    // ProjectExplorer / Overview). A single global/session grade (grade_) is
    // applied as a GPU post-pass in renderPlayer. See App_Grade.cpp.
    float gradeScroll_ = 0.0f;         // vertical scroll within the active tool
    float gradeContentH_ = 0.0f;       // last tool's content height (for clamping)
    int gradeTool_ = 0;                // active tool: 0 basic, 1 curves, 2 wheels
    grade::State grade_;
    GradeGpu gradeGpu_;
    // The grade/tech-check shaders take ~75 ms to compile and most sessions
    // never grade, so they are built on first use rather than at launch.
    // Cleared on a renderer rebuild, which invalidates their GL objects.
    bool gradeGpuTried_ = false;
    void ensureGradeGpu();
    SDL_FRect gradeToolRects_[3]{};    // top-bar tool buttons (hit-tested)

    // Interactive-widget hit regions, rebuilt each render in renderGradePanel.
    struct GradeSlider { float* value=nullptr; float lo=0, hi=1, def=0; SDL_FRect rect{}; int grad=0; const char* label=""; };
    std::vector<GradeSlider> gradeSliders_;
    struct GradeWheelHit { grade::Wheel* wheel=nullptr; SDL_FRect rect{}; };
    std::vector<GradeWheelHit> gradeWheels_;
    // Tech-check panel (left-expand pane). Diagnostic view modes applied as the
    // final GPU pass after the grade; the active mode persists when the panel is
    // closed, and its legend overlays draw on the stage. See App_TechCheck.cpp.
    // Red/Green/Blue isolate one channel as grayscale; they share this
    // mutually-exclusive slot and are driven by the R / G / B keys only (no pill).
    enum class TechMode { None = 0, Luminance = 1, Clipping = 2, Monochrome = 3,
                          Red = 4, Green = 5, Blue = 6 };
    TechMode techMode_ = TechMode::None;
    bool nitHeatmapShown_ = false;     // set each frame: true when the Luminance heatmap actually rendered (EXR only)
    SDL_FRect techPillRects_[3]{};     // the three mode toggle buttons

    // Clip Source panel (left-expand pane, mutually exclusive with the other
    // left panes). The same cascading naming-config pickers as the clip right-click
    // menu, stacked vertically and driven by the selected clip — or, with nothing
    // selected, the clip under the frame indicator — rather than by a click.
    // See App_ClipSource.cpp.
    float clipSourceScroll_ = 0.0f;        // content scroll offset in px (clamped while rendering)
    // Set by the arrow keys: the next render scrolls the commit picker's current
    // row into view once it knows where that row landed, then clears the flag.
    bool clipSourceRevealVersion_ = false;
    // The describe_pickers result the panel is currently showing, and the clip +
    // media it belongs to. Refreshed only when the panel opens or a different clip /
    // source becomes the panel's target — see updateClipSourceData(). The clip
    // id is part of the key, not just the media id: two adjacent clips can share one
    // source, and a commit has to target the clip the panel is actually showing.
    std::vector<PickerState> clipSourceStates_;
    int clipSourceClipId_ = -1;            // representative clip the states describe (-1 = nothing resolved)
    std::string clipSourceMediaId_;        // its media id at the time of the query
    // The whole clip set the states belong to — panelCascade_.clipIds as it stood at
    // the query, kept separately so a changed timeline selection is diffed against it
    // even while a pick is navigating the cascade. More than one entry puts the panel
    // in its multi-clip mode, like a right-click on a multi-selection.
    std::vector<int> clipSourceSelIds_;
    bool clipSourceDirty_ = true;          // a refresh is due (set on open / media change)
    bool clipSourceNoPickers_ = false;     // resolved, but the path matched no picker
    // Option rows laid out by the last render, for hit-testing.
    struct ClipSourceRow { SDL_FRect rect; int col; int opt; };
    std::vector<ClipSourceRow> clipSourceRows_;
    // A press on the commit picker's row (Version) is armed rather than acted on: it
    // can still become a drag of that pick's media onto the timeline. A release in
    // place commits it; crossing the move threshold starts the drag instead.
    bool clipSourceDragArmed_ = false;
    std::string clipSourceDragKey_, clipSourceDragValue_;
    std::string clipSourceDragLabel_;  // the option as drawn (decorated); the ghost card's caption
    float clipSourceDragPressX_ = 0.0f, clipSourceDragPressY_ = 0.0f;
    // Ctrl / Shift on a commit row marks it rather than replacing anything: the marks
    // are a hand-picked set the next drag carries out as a group. Held as option
    // values, not row indices, so a re-describe that reorders the list keeps them.
    // Dropped whenever the panel re-describes, navigates or commits a pick.
    std::vector<std::string> clipSourceMarked_;
    std::string clipSourceMarkAnchor_; // shift-range anchor (a commit option value)

    // Settings panel (left-expand pane, mutually exclusive with the other left
    // panes). Houses project FPS and app-level preferences. See App_Settings.cpp.
    float settingsScroll_ = 0.0f;       // vertical scroll within the panel body
    float settingsContentH_ = 0.0f;     // last frame's content height (for clamping)
    TextInput   fpsFld_;               // FPS editable field
    bool        fpsFldListOpen_ = false;
    int         fpsFldListHover_ = -1;
    SDL_FRect   fpsFldBox_{};          // FPS box rect (set each frame in renderSettingsPanel)
    SDL_FRect   uiScaleBox_{};         // UI Scale picker box (set each frame in renderSettingsPanel)
    bool        uiScaleListOpen_ = false;
    int         uiScaleListHover_ = -1;
    TextInput   cacheGbFld_;           // frame-cache size (GiB) editable field
    SDL_FRect   cacheGbFldBox_{};      // its box rect (set each frame in renderSettingsPanel)
    float       cacheGb_ = 8.0f;       // current frame-cache budget in GiB
    TextInput   decodeThreadsFld_;     // frame-cache decode-worker count editable combo
    SDL_FRect   decodeThreadsFldBox_{};// its box rect (set each frame in renderSettingsPanel)
    bool        decodeThreadsListOpen_ = false;
    int         decodeThreadsListHover_ = -1;
    SDL_FRect   decodeThreadsListRect_{}; // open list rect (set each frame; flips up when the panel edge is close)
    int         decodeThreads_ = 0;    // stored worker count; 0 = Auto (see decodeThreadsActive_)
    int         decodeThreadsActive_ = 0; // what the pool was actually built with, this run
    TextInput   nitRefFld_;            // HDR reference: scene-linear 1.0 = N nits (drives the Luminance heatmap)
    SDL_FRect   nitRefFldBox_{};       // its box rect (set each frame in renderTechPanel)
    float       nitRef_ = 100.0f;      // scene-linear 1.0 mapped to this many nits

    // External video output (NDI / SDI). Each newly-composited program frame is
    // read back off the GPU and pushed to the selected device. See Output.h. NDI
    // and the review monitor (below) are never used at once, so both are chosen
    // from a single radio-style top-level "Output" menu (populateOutputMenu).
    OutputManager output_;

    // Review-monitor output: a borderless full-screen window on a second display
    // showing the clean program frame + annotations at the mirrored GUI zoom/pan
    // (no status text, no grid/overview). Frames arrive via the shared OutputManager
    // GL readback. While active the review renderer carries vsync (playback's timing
    // master) and the GUI renderer presents unthrottled. See App_Review.cpp.
    SDL_Window*   reviewWindow_   = nullptr;
    SDL_Renderer* reviewRenderer_ = nullptr;
    TextFont reviewTextFont_; // burn-in text for that renderer (glyph textures are per-renderer)
    SDL_GPUDevice* reviewGpuDevice_ = nullptr; // owns the review renderer's SDL_GPU device when it runs HDR (scRGB)
    bool          reviewHdr_      = false;    // review renderer got an HDR (scRGB) swapchain; feeds the float path
    SDL_Texture*  reviewTex_      = nullptr; // SDR review: pre-tonemapped 8-bit program image
    int           reviewTexW_ = 0, reviewTexH_ = 0;
    // HDR review runs the display transform on its OWN device, straight from the
    // scene-linear source — reading the main renderer's transformed frame back to CPU
    // instead cost 8-12 ms/frame and made playback CPU-bound. reviewSrcTex_ holds the
    // scene-linear program frame; hdrColorPassReview_ transforms it in
    // renderReviewWindow. See hdrColorPassReview_ for why this is not the per-monitor
    // display feature that was removed.
    SDL_Texture*  reviewSrcTex_   = nullptr;  // scene-linear source on the review device (RGBA half)
    int           reviewSrcW_ = 0, reviewSrcH_ = 0;
    bool          reviewSrcSceneLinear_ = false; // reviewSrcTex_ holds EXR scene-linear (transform applies) vs video (blit)
    bool          reviewEnabled_  = false;   // user toggle (Settings OUTPUT)
    SDL_DisplayID reviewDisplay_  = 0;        // target display id; 0 = none chosen
    bool          reviewFrameFresh_ = false;  // reviewTex_ holds the latest program frame

    // ── Sync review (LAN session) ─────────────────────────────────────────
    // One peer hosts and broadcasts transport events (play/pause/seek/sequence);
    // spectators load the host's project over TCP and follow along. Colour/OCIO
    // are deliberately not synced. See SyncSession + App_Sync.cpp.
    syncreview::Session syncSession_;
    bool hoveredSessionBtn_ = false;
    TextInput sessionUserFld_;         // username used when hosting / joining
    TextInput sessionHostFld_;         // manual host name or IP[:port] for Join
    TextInput sessionPortFld_;         // TCP port this machine hosts on
    // Master switch for all sync-review networking, persisted in settings.conf and
    // OFF by default: while it is off no socket is opened (no discovery listener,
    // no host listener, no join), and the rest of the SESSION panel is disabled.
    bool syncNetwork_ = false;
    int  syncPort_ = syncreview::kDefaultPort; // committed value of sessionPortFld_
    // Panel hit rects, rebuilt each render.
    SDL_FRect sessionNetChkRect_{};    // "Enable network" checkbox
    SDL_FRect sessionCreateRect_{};    // "Create Session" button
    SDL_FRect sessionJoinManualRect_{};// "Join" (manual host field) button
    SDL_FRect sessionLeaveRect_{};     // "Leave" button (while in a session)
    struct SessionBeaconRow { std::string ip; uint16_t port; SDL_FRect rect; };
    std::vector<SessionBeaconRow> sessionBeaconRows_;
    // SYNC SESSION launcher column: one clickable row per discovered host beacon,
    // rebuilt every render (see renderMainPanels). Shown only when a live LAN
    // session exists; clicking a row joins that host.
    std::vector<SessionBeaconRow> launcherSyncRows_;
    float launcherSyncScroll_ = 0.0f;   // SYNC SESSION tab vertical scroll (px)
    SDL_FRect launcherSyncListRect_{};  // its viewport, for wheel hit-testing
    // Host broadcast tracking: only emit an event when the synced state actually
    // changes (see broadcastHostState). A scrub sends just the final frame, since
    // seeks are only emitted while not actively scrubbing.
    bool lastSentPlaying_ = false;
    int64_t lastSentFrame_ = -1;
    int32_t lastSentSeq_ = -2;         // -2 = never sent (differs from -1 = All)
    // Frame view (zoom/pan) broadcast tracking. Only the host drives zoom/pan; the
    // state travels as a zoom factor + the normalized image point held at the
    // player center, so spectators reproduce the framing regardless of window size.
    float lastSentZoom_ = -1.0f;       // -1 = never sent
    float lastSentU_ = 0.5f, lastSentV_ = 0.5f;
    int lastPeerCount_ = 0;            // re-push the view when a new spectator joins
    // Viewer exposure/gamma, sent the same way (and re-sent to a late joiner).
    // 999 = never sent; the real range can't reach it.
    float lastSentExpGain_ = 999.0f, lastSentExpGamma_ = 999.0f;
    // Letterbox matte, sent the same way. It has to travel with the view: Fit View
    // frames on the masked region, so a spectator that disagrees about the ratio or
    // the flag reproduces the host's zoom/pan against a different fit and lands on
    // a different framing. -1 = never sent (a real ratio is 0 or positive).
    float lastSentMatteRatio_ = -1.0f;
    float lastSentMatteOpacity_ = -1.0f;
    bool lastSentMatteFit_ = false;
    // Set by structural edits (add/remove source, add sequence) so broadcastHostState
    // re-pushes the full project snapshot to spectators. Coalesced to one broadcast
    // per frame, so a multi-file add sends a single refreshed snapshot.
    bool hostSnapshotDirty_ = false;
    // Spectator input lock: while following a host, local transport is ignored so
    // it can't fight incoming events.
    bool transportLocked() const { return syncSession_.role() == syncreview::Role::Spectator; }
    // A spectator attempted a host-controlled action; flash a brief note so the
    // input doesn't just silently vanish. Returns true so a call site can bail
    // with `if (transportLocked()) { spectatorLocked("CHANGE FRAME"); ... }`.
    bool spectatorLocked(const char* action) {
        setStatus(std::string("SPECTATOR NOT ALLOWED TO ") + action, 2500);
        return true;
    }
    // ── Local control channel ─────────────────────────────────────────────
    // Loopback TCP listener that lets another local process (an MCP server) open
    // media/projects and query state. Owning the port is what makes this the
    // "first" instance; a second jplay just doesn't listen. See ControlServer.h
    // and App_Control.cpp.
    control::Server control_;
    bool controlAdvertised_ = false;                 // handshake file written for this instance
    void drainControl();                            // handle queued commands (per frame)
    void advertiseControlChannel();                 // write ~/.jplay/control.json
    std::string controlStateJson();                 // the "state" reply
    std::string controlOpen(const std::map<std::string, std::string>& req);
    std::string controlAddMedia(const std::vector<std::string>& paths,
                                const std::string& mode);
    std::string controlPick(const std::map<std::string, std::string>& req);
    // Clips the "pick" command's `scope` names, in timeline order.
    std::vector<int> controlPickScope(const std::string& scope, std::string& err) const;

    void drainSync();                  // apply inbound events / handle host loss (per frame)
    void broadcastHostState();         // emit changed transport state (host, per frame)
    void loadProjectFromBuffer(const std::string& bytes); // spectator: adopt host's project
    void renderSessionPanel();         // left-side SESSION panel (when open)
    bool sessionHandleEvent(const SDL_Event& e); // returns true if consumed
    void refreshHostSnapshot();        // (re)serialize the current project for joiners
    void startJoinFromField();         // parse the manual host field and connect
    void joinHost(const std::string& host, uint16_t port); // connect as spectator
    void leaveSession();               // end the session and reset broadcast tracking
    void toggleSyncNetwork();          // flip syncNetwork_, closing sockets when off
    void commitSyncPort();             // validate + persist the port field
    void broadcastAnnotations();       // host: send the frame's combined stroke set
    void sendStrokeToHost();           // spectator: send my just-completed stroke to the host
    void broadcastClearStrokes();      // clear the frame's markup (host broadcasts; spectator asks host)

    SDL_FRect gradeEnableRect_{};      // master enable checkbox in the panel header
    SDL_FRect gradeResetRect_{};       // "Reset all" button in the panel header
    SDL_FRect gradeCurveRect_{};       // curve editor plotting area
    SDL_FRect gradeCurveTabs_[4]{};    // Luma/R/G/B tab buttons
    int gradeCurveChannel_ = 0;        // 0 master,1 R,2 G,3 B
    // Drag state
    int gradeDragSlider_ = -1;
    int gradeDragWheel_ = -1;
    int gradeDragCurvePt_ = -1;
    // Histogram of the displayed frame (luma + per-channel), for curves/auto-match.
    std::array<float, 64> gradeHisto_{};
    std::array<float, 64> gradeHistoR_{}, gradeHistoG_{}, gradeHistoB_{};
    CacheKey gradeHistoKey_{ "", -999 }; // frame the histogram was computed for (empty media never matches)

    // Clip drag/drop
    bool draggingClip_ = false;
    int dragClipId_ = -1;
    double dragGrabOffset_ = 0.0; // cursor frame minus clip start at grab time
    int dragTargetTrack_ = 0;
    int64_t dragTargetStart_ = 0;
    bool dragOverlaps_ = false; // drop would overlap a neighbour -> ripple it forward
    // Cross-sequence guard: a clip may only be rearranged within its own
    // sequence. When a drag would land in a different sequence's region the drop
    // is rejected and that region is flagged so the timeline can draw a red box.
    bool dragRejected_ = false;
    int  dragRejectSeq_ = -1;   // sequence index the drag is illegally hovering, or -1
    bool dragBadType_ = false;  // drop would land a clip on a track of the wrong type
    // A left-press on a clip is "pending" until the cursor moves past a small
    // threshold: under it the press is a plain selection, over it becomes a drag.
    int pendingDragClipId_ = -1;
    float dragPressX_ = 0.0f;
    float dragPressY_ = 0.0f;
    bool dragPressWasSelected_ = false; // press was on the already-selected clip
    int selectedClipId_ = -1; // primary (last-clicked) selected clip; inspector/disable act on it
    // Full multi-selection set (Shift+click toggles membership). Always contains
    // selectedClipId_ as its primary; empty means nothing selected. Every selected
    // clip draws the selection border and is moved/deleted as a group.
    std::vector<int> selectedClipIds_;
    // Group drag: original (track,start) of each dragged clip captured at grab, so
    // the same (dTrack,dFrame) delta applies to the whole selection each motion.
    // `audio` records each clip's track group: the delta applies within a clip's
    // own group (audio clips stay on audio tracks, video clips on video tracks).
    struct DragClipOrig { int id; int track; bool audio; int64_t start; int64_t duration; };
    std::vector<DragClipOrig> dragOrig_;

    // Clip clipboard (Ctrl+C / Ctrl+V). Copies of the clips selected at copy
    // time, with timelineStart rebased so the earliest one sits at 0 and sorted
    // by (track, start); paste re-anchors the group at the playhead. Entries
    // whose media is no longer in the pool (new/other project) are skipped.
    std::vector<Clip> clipboard_;

    // Gap: empty span between two clips on one track. Hovered gaps draw a gray
    // dotted box; a clicked one is selected and Delete closes it (global ripple).
    // track < 0 means "none".
    int hoverGapTrack_ = -1;
    int64_t hoverGapStart_ = 0, hoverGapEnd_ = 0;
    int selectedGapTrack_ = -1;
    int64_t selectedGapStart_ = 0, selectedGapEnd_ = 0;

    // Track reorder drag (gutter label -> another row). Armed on a press in a
    // track header, promoted to a live drag past the motion threshold so a plain
    // click still reaches the remove cross. trackDropIns_ is the insertion
    // boundary the release would use, -1 while the drop would change nothing.
    int trackDragFrom_ = -1;        // pressed/dragged row (-1 = nothing armed)
    bool trackDragging_ = false;    // threshold crossed; ghost + markers are drawn
    float trackDragPressY_ = 0.0f;  // press y, for the threshold test
    float trackDragGrabDY_ = 0.0f;  // press y minus the row's top, so the ghost holds its grip
    float trackDragGhostY_ = 0.0f;  // top of the ghost row, updated on motion
    int trackDropIns_ = -1;

    // Inline track rename (double-click a track header). trackNameEdit_ is the row
    // being edited, -1 when none; the field draws over that header's label and its
    // text is restricted to [A-Za-z0-9 _] so a track name stays a plain label.
    int trackNameEdit_ = -1;
    TextInput trackNameField_;

    // Clip trim (edge resize). A left-press on a clip's left/right edge starts a
    // trim drag; the proposed geometry is previewed live and applied on release.
    bool trimmingClip_ = false;
    int  trimClipId_ = -1;
    int  trimEdge_ = 0;                 // 0 = left edge, 1 = right edge
    int64_t trimNewStart_ = 0;          // proposed timelineStart during the drag
    int64_t trimNewDuration_ = 0;       // proposed duration during the drag
    int64_t trimNewSourceOffset_ = 0;   // proposed sourceOffset during the drag
    bool trimHover_ = false;            // cursor is over a clip edge (resize cursor)

    // Transition (dissolve on a cut). Selection is separate from the clip
    // selection because the box sits on top of two clips: -1 means none, and
    // selecting one clears the clip/gap selection so Delete is unambiguous.
    int selectedTransitionId_ = -1;
    bool resizingTransition_ = false;
    int  resizeTransId_ = -1;
    int  resizeTransEdge_ = 0;          // 0 = left edge (inFrames), 1 = right (outFrames)
    int64_t resizeTransNewIn_ = 0;      // proposed inFrames during the drag
    int64_t resizeTransNewOut_ = 0;     // proposed outFrames during the drag

    // Clip fade drag: the grab dot at a ramp's apex, dragged along the clip's top
    // edge. Same begin/update/commit shape as clip trim.
    bool fadingClip_ = false;
    int  fadeClipId_ = -1;
    int  fadeEdge_ = 0;                 // 0 = fade in (head), 1 = fade out (tail)
    int64_t fadeNewFrames_ = 0;         // proposed ramp length during the drag

    // Curve-edit mode: one track expanded into a full-height lane where each of
    // its clips' parameter curve is drawn over the clip and edited in place. The
    // other rows collapse to zero height (trackRowH), so the lane is the only
    // thing under the shot bar. -1 = off.
    //
    // Parameterised rather than hard-wired to volume: the lane draws and edits a
    // Curve given a range and a formatter, so a video parameter joins by adding a
    // CurveParam case and a clipCurve() binding — nothing in the drawing or the
    // hit testing needs to know which one it is.
    enum class CurveParam { Volume };
    int        curveTrack_ = -1;
    CurveParam curveParam_ = CurveParam::Volume;
    float      curveLaneH_ = 0.0f;      // resolved in computeLayout (see curveLaneHeight)
    // Live drag. ptIdx -1 means the start box, which translates the whole curve;
    // >= 0 is that point. The pre-drag snapshot the gesture undoes back to lives
    // with the rest of the undo machinery (curveDragBefore_), since ContentSnapshot
    // is not declared yet up here.
    int  curveDragClipId_ = -1;
    int  curveDragPtIdx_ = -1;
    float curveDragGrabDv_ = 0.0f;      // value under the cursor minus the grabbed value
    bool curveDragChanged_ = false;     // the drag actually altered the curve
    int  curveHoverClipId_ = -1;
    int  curveHoverPtIdx_ = -2;         // -2 = nothing, -1 = the start box, >= 0 a point
    float curveHoverX_ = 0.0f;          // cursor, for the ghost point on the line.
    float curveHoverY_ = 0.0f;          // Not tlHoverX_: that one only tracks the
                                        // ruler band above the rows.


    // External file drag-hover (preview box) + drop placement. A dropped file
    // whose kind doesn't match the target track's existing type is rejected
    // (see addMediaFileAt); the empty trailing track accepts either kind.
    bool fileHoverActive_ = false;
    int fileHoverTrack_ = 0;
    int64_t fileHoverStart_ = 0;
    // Placeholder length in frames. 0 = the usual fixed kHoverBoxPx guess width
    // (the real length is unknown until the drop); non-zero when the landing span
    // is already known, as it is for an aligned Clip Source drop.
    int64_t fileHoverSpan_ = 0;
    bool dropFirstFile_ = true;   // first DROP_FILE of the current drop batch
    int dropInsertTrack_ = 0;
    int64_t dropInsertFrame_ = 0; // advances per file for end-to-end placement

    // Media-bin → timeline drag (drag a SOURCES row onto the tracks to add a clip)
    int binDragRow_ = -1;          // armed explorerRows_ index on press (-1 = none)
    bool binDragging_ = false;     // motion threshold crossed; live drag in progress
    bool binDragReselect_ = false; // on a no-drag release, collapse selection to the pressed row
    float binDragPressX_ = 0.0f, binDragPressY_ = 0.0f;
    std::vector<std::string> binDragPaths_; // source paths captured at drag start
    // Ghost-card caption override. Empty for a bin drag (the card names the source
    // file); a Clip Source drag sets it to the picked item's own label, since
    // that — not the resolved filename — is what the user grabbed.
    std::string binDragLabel_;
    // Clip Source drag: the clip whose picker the dragged item came from
    // (-1 = a plain bin drag). Hovering the free video row directly under that
    // clip is a special drop — see pickerAlignDropClip.
    int binDragPickerClipId_ = -1;

    // Drop-action chooser over the video frame: while media is dragged onto the
    // frame (a bin drag or an OS file drag) three boxes offer view / replace /
    // add-to-track instead of a single implied action.
    bool playerDropActive_ = false; // a drag is over the frame and the boxes are up
    int  playerDropHover_ = -1;     // box under the cursor (0..2), -1 = none
    bool playerDropOnFrame_ = false;// the current OS drop batch landed on the chooser
    int  playerDropAction_ = -1;    // box that batch chose (-1 = off the boxes: View)

    // Transient status line
    std::string status_;
    Uint64 statusUntil_ = 0;
    bool statusWarn_ = false; // draw status_ as a warning (see setStatusWarn)

    // Media-info overlay (toggled with 'i'). Describes the clip under the
    // playhead; fields are rebuilt only when that clip changes (infoClipId_).
    bool showInfo_ = false;
    int infoClipId_ = -1;
    std::vector<InfoField> infoFields_;

    // ── Pixel inspector (App_PixelInspector.cpp) ─────────────────────────────
    // A probe on the program image, toggled with 'P': an overlay in a bottom corner
    // of the frame with a nearest-neighbour magnifier around the cursor, a crosshair
    // on the sampled pixel, and that pixel's value at three points of the pipeline —
    // as decoded, in the OCIO working space after exposure, and as displayed (post
    // display transform, grade and tech mode).
    //
    // Sampled once per rendered frame by samplePixelInspector(), called from
    // renderPlayer where the program texture is current; renderPixelInspector() only
    // draws what it left here. Not persisted: like the media-info overlay this is a
    // look-at-this-now tool rather than a project setting.
    bool pixelInspectorOpen_ = false;
    struct PixelProbe {
        bool valid = false;     // the cursor is over the program image
        int  x = 0, y = 0;      // the sampled pixel, in stored image pixels
        // As decoded: scene-linear float for an EXR, code values at the buffer's own
        // depth (srcMax) for an integer source — which is what a tech check wants to
        // read. Only RGB is reported; src[3] is carried solely to feed the working
        // transform a complete pixel.
        bool  srcValid = false;
        bool  srcFloat = false; // scene-linear float vs code values
        float src[4]{};
        float srcMax = 255.0f;  // full-scale code value when !srcFloat
        // Scene-linear working space, after the input transform and the exposure.
        // Only when colour management is on.
        bool  workValid = false;
        float work[3]{};
        // Display-referred, read back off the program texture: post OCIO display
        // transform, grade and tech mode — exactly what is on screen.
        bool    dispValid = false;
        uint8_t disp[3]{};
        // Mid-dissolve the cache holds the two halves, not the blend, so the source
        // and working rows describe the outgoing clip alone and say so.
        bool dissolve = false;
    };
    PixelProbe probe_;
    // The panel's bounds as last drawn: a cursor reaching the panel sends it to the
    // other bottom corner rather than let it sit over the pixels being probed.
    SDL_FRect probePanelRect_{};
    // Which bottom corner the panel is drawn in, flipped when the cursor reaches it.
    bool probeRight_ = false;
    // The CPU transform the working row goes through, cached across frames — building
    // it per sample would re-derive processors on every mouse move. Keyed by the
    // colour space and OCIO version it was built for, plus the exposure baked in.
    std::string probeXfCs_;
    int         probeXfVersion_ = -1;
    float       probeXfGain_    = 0.0f;
    OcioManager::CpuTransform probeXf_;

    // Custom window chrome (borderless window) + menu bar.
    TitleBar titleBar_;
    MenuBar menuBar_;

    // Double-click on the title bar's empty area maximizes / restores. The press
    // itself never arrives as a mouse event there — the window manager takes it
    // for a window move — so the gesture is timed from SDL_EVENT_WINDOW_HIT_TEST,
    // which the X11 backend emits per swallowed press. hitTest records
    // whether that press was over the title bar's drag strip; see handleEvent.
    bool titleDragHit_ = false;
    Uint64 titleDragClickMs_ = 0;

    // OpenColorIO: display transform applied at review time to EXR frames.
    OcioManager ocio_;
    OcioGpu     ocioGpu_;             // GPU display transform (when GL backend active)

    // The OCIO colour space `m` is read in: its explicit override when the user set
    // one, else the config's answer for it (file rules, then the container's own
    // colour tags), resolved once per media per config and cached on the Media.
    // Empty when colour management is off or no config loaded.
    std::string mediaColorSpace(Media& m);

    // The media under the playhead, which is what the File Colorspace control
    // reads and writes — a colour space belongs to a source, not to the session.
    // Null over a gap, or when the clip there has no resolvable media.
    std::shared_ptr<Media> playheadMedia();

    // What the File Colorspace button shows: the space the media under the playhead
    // is read in, prefixed to mark it as resolved rather than chosen. "File
    // Colorspace" when there is no media to speak of.
    std::string ocioInputCsLabel();

    // Every source in the project with the colour transform its exported frames go
    // through, keyed by media path (see ExportDialog::setColorTransforms). Built on
    // the main thread when the export dialog opens, because the encoder runs on a
    // worker and OCIO processors have to be resolved before it starts. Empty when
    // colour management is off, which exports the decoded frames untouched.
    std::map<std::string, OcioManager::CpuTransform> exportColorTransforms();

    // The tiles in the Overview grid and the SOURCES bin, the timeline's hover
    // preview and the launcher's project tile all show the frame's own 8-bit
    // encoding — an EXR's scene-linear halves through the sRGB curve (baked by
    // ExrSource), a video's or a still's decode as it came. No config, no view, no
    // per-media colour resolution: a tile is identified by its source and frame
    // alone, so it survives every change to the review selection and costs the main
    // thread a hash to ask for. The grid and the player do disagree on colour on a
    // show with a strong look; that is the price of a contact sheet that never
    // stalls the UI to build one.
    //
    // Dropped when the media pool changes under them, which is the one thing that
    // does invalidate what is on screen.
    void resetThumbnailState();
    HdrColorPass hdrColorPass_;       // SDL_GPU render-state color pass (HDR / "gpu" backend)
    // Second instance of the SAME pass, bound to the review monitor's own GPU device
    // (SDL GPU objects can't cross devices). It is fed the identical processor, version
    // token, encoding and primaries as hdrColorPass_ — this is NOT the per-monitor
    // display feature that was removed; there is still exactly ONE OCIO display and ONE
    // HDR sink at a time (the main window when nothing is out, the review monitor / SDI
    // otherwise — see hdrOutput_ below). It exists purely so the review monitor can
    // transform on its own GPU instead of round-tripping a readback through the CPU.
    HdrColorPass hdrColorPassReview_;
    int ocioDisplaySubmenuIdx_ = -1; // menu index of the Display Space flyout
    int ocioViewSubmenuIdx_    = -1; // menu index of the View Transform flyout
    int ocioLookSubmenuIdx_    = -1; // menu index of the Look flyout
    int ocioInputCsSubmenuIdx_ = -1; // menu index of the File Colorspace flyout
    std::vector<uint8_t> ocioPixels_; // scratch buffer reused across renderPlayer() calls (CPU fallback)
    // HDR (SRGB_LINEAR) composite scratch: RGBA half, linear extended-range, reused
    // across frames. Feeds the float program texture when hdrActive_ is set.
    std::vector<Imath::half> hdrPixels_;
    // Whether the current HDR program texture is colour-managed — it holds the
    // frame in its own colour space for the OCIO render-state pass to transform.
    // False when colour management is off, or when a mixed-space dissolve already
    // transformed the two halves on the CPU; the texture is then drawn directly.
    bool hdrFrameManaged_ = false;
    // HDR external-output path: the color pass renders the display image into this
    // offscreen float target so it can be read back (NDI / review monitor) — there
    // is no on-screen display texture to sample otherwise. Read back + tonemapped to
    // SDR 8-bit in hdrReadback_.
    SDL_Texture* hdrProgramTex_ = nullptr;
    int hdrProgW_ = 0, hdrProgH_ = 0;
    std::vector<uint8_t> hdrReadback_;
    // The same readback kept as scRGB half (linear, extended range) for an output
    // device that encodes HDR itself — see OutputFrame::scRgb.
    std::vector<Imath::half> hdrReadbackHalf_;

    // Export dialog (modal overlay).
    ExportDialog exportDialog_;

    // Generic modal message dialog (App_Project.cpp) — the app-styled stand-in
    // for SDL_ShowMessageBox. Unlike the platform box it does not block, so a
    // caller that used to branch on the return value puts its follow-up work in
    // the chosen button's action instead.
    struct DialogButton {
        std::string label;
        std::function<void()> action; // null = dismiss only
    };
    bool msgDialogOpen_ = false;
    std::string msgDialogTitle_;
    std::vector<std::string> msgDialogLines_;    // message, split on '\n'
    std::vector<DialogButton> msgDialogButtons_; // drawn left→right in this order
    std::vector<Button> msgDialogWidgets_;       // hover/click state, one per button
    int msgDialogEsc_ = -1;                      // button Esc picks (-1 = dismiss without acting)
    int msgDialogEnter_ = -1;                    // button Enter picks
    // Optional checkbox on the left of the button row ("don't ask again" and the
    // like). Empty label = no checkbox; toggling runs onCheck straight away, so
    // the choice sticks whichever button the user then picks.
    std::string msgDialogCheckLabel_;
    bool msgDialogChecked_ = false;
    std::function<void(bool)> msgDialogOnCheck_;
    SDL_FRect msgDialogCheckRect_{};

    // Generic modal progress overlay. A background worker thread reports through
    // progress_; the main loop polls it each frame to draw the dialog, then runs
    // progressOnDone_ on the main thread once the worker finishes. Any long
    // action can drive it via beginProgress().
    ProgressReporter      progress_;
    std::thread           progressThread_;
    std::atomic<bool>     progressRunning_{ false };    // worker thread is active
    std::atomic<bool>     progressWorkerDone_{ false }; // worker done; finalize on main thread
    std::function<void()> progressOnDone_;              // run on the main thread at completion
    std::string           progressTitle_;
    SDL_FRect             progressCancelRect_{};        // hit-tested by handleProgressEvent
    uint64_t              progressStartTick_ = 0;       // SDL_GetTicks() when the task began

    // Sequence view filter, driven by the top-toolbar "Sequence" button. >= 0
    // restricts the timeline view and playback to that sequence (index into
    // timeline_.sequences); -1 = All (shows every sequence) is no longer offered
    // by the picker, but is still reachable from the Project Explorer's "All"
    // root and a double-click on a filtered timeline. Defaults to the first
    // sequence ("Default Sequence" in a fresh project).
    int viewSeqIdx_ = 0;
    // Project view scope, driven by the Sequence popup's project rows: the view
    // covers every sequence of one project (a Timeline::SourceProject id) instead of
    // a single one, and viewSeqIdx_ is -1 for the duration (the two scopes are
    // mutually exclusive; setting either clears the other). Sequences are held by
    // stable id, so a reorder keeps the scope pointing at the same content. Only the
    // project id is saved with the project (and shipped to sync spectators) — the id
    // list is rebuilt on load from Sequence::projectId, which is what the popup
    // groups by anyway.
    int viewProjId_ = -1;
    std::vector<int> viewProjSeqIds_;
    // Backspace view history: the scopes visited before the current one, oldest
    // first. Entries hold ids rather than indices — deleting a sequence shifts
    // every index after it, and a stale index would take Backspace somewhere the
    // user never was. An entry whose sequence or project is gone is skipped when
    // it comes back up.
    struct ViewHistoryEntry {
        int seqId = -1;  // single-sequence scope; -1 = All, or a project scope
        int projId = -1; // project scope; -1 = none
        bool operator==(const ViewHistoryEntry& o) const {
            return seqId == o.seqId && projId == o.projId;
        }
    };
    std::vector<ViewHistoryEntry> viewHistory_;
    bool viewHistoryLock_ = false; // set while going back, so the replay adds no entry
    static constexpr size_t kMaxViewHistory = 64;

    // Scratch view: a temporary sequence that stands in for the cut while it is up
    // and is discarded the moment the scope leaves it. Sequence::temporary keeps it
    // out of the file, the dirty signature and every sequence list, which is what
    // makes it safe to leave the project untouched. There is only ever one — opening
    // a second replaces the first — and it is always the last sequence in the vector,
    // so dropping it leaves every other index valid.
    //
    // Two kinds share the one slot:
    //   Source — one media on its own, from a bin double-click or F1 on the clip
    //            under the playhead (openSourceView). One track.
    //   Layout — the clips being compared, one per video track and aligned on their
    //            absolute source frames, so the Layout stage tiles them by track
    //            (openLayoutView). As many tracks as there are clips.
    enum class ScratchKind { None, Source, Layout };
    ScratchKind scratchKind_ = ScratchKind::None;
    int scratchSeqId_ = -1;     // the temporary sequence's id, or -1 for none
    int scratchTrackCount_ = 1; // rows it stands up, in place of the project's (see trackCount)
    bool scratchActive() const { return scratchSeqId_ >= 0; }
    bool sourceViewActive() const { return scratchKind_ == ScratchKind::Source; }
    bool layoutSeqActive() const { return scratchKind_ == ScratchKind::Layout; }
    // Everything a scratch view puts back when it closes, so Backspace out of one
    // lands exactly where it was opened from — scope, playhead, selection and the
    // timeline's zoom/pan, none of which the generic view history carries.
    struct ScratchReturn {
        int seqIdx = -1;             // viewSeqIdx_
        int projId = -1;             // viewProjId_
        std::vector<int> projSeqIds; // viewProjSeqIds_
        int activeSeqIdx = 0;        // activeSequenceIdx_
        int64_t playhead = 0;
        double viewStart = 0.0;
        double framesPerPx = 1.0;
        // The clip selection the view was opened from. A Layout view is built out
        // of it, and clearing it on the way in would mean toggling the stage twice
        // laid out something different the second time.
        std::vector<int> selectedClipIds;
        int selectedClipId = -1;
    };
    ScratchReturn scratchReturn_;
    int outputMenuIdx_ = -1;     // top-level "Output" menu index
    size_t outputMenuSig_ = (size_t)-1; // signature of the last-built Output item set
    bool timeFormatFrames_ = false;
    bool showAsFrames() const { return timeFormatFrames_; }

    // Frame Numbering: false = Global (readout counts from the timeline/sequence
    // origin, default), true = Clip (readout counts from the current clip's start).
    bool frameNumberingClip_ = false;

    // Color Management (PROJECT SETTINGS): OCIO display transform vs. the built-in
    // linear→sRGB fallback. Backed by timeline_.ocioEnabled (persisted per-project)
    // and ocio_.setEnabled(); the toggle keeps both in sync.
    SegToggle colorMgmtToggle_{};

    bool pythonDebug_ = false;
    SDL_FRect pythonDebugRect_{};

    // Show a small thumbnail above the ruler hover indicator (pause only).
    bool showFramePreview_ = true;

    // Play short audio grains at the playhead while scrubbing the ruler. Off by
    // default; persisted in user prefs.
    bool audioScrub_ = false;

    // Draw the embedded audio track's waveform inside a video clip on the
    // timeline. Off by default; persisted in user prefs. Deliberately narrow: it
    // draws for the clip under the playhead only, and only over that clip's
    // in/out range - embedded audio is demuxed out of the video container, so a
    // whole-track envelope would read the entire file. Decoding is requested only
    // while stopped; an already-decoded envelope stays visible during playback.
    bool clipWaveform_ = false;

    // Burn-in over the program image: file name on the left, the playhead readout
    // on the right, along the top or bottom edge of the picture (inside the
    // letterbox matte, so it never lands on a bar). Off by default; persisted in
    // user prefs. The readout follows the Time Format / Frame Numbering prefs, so
    // it always reads the same as the ruler.
    bool frameOverlay_ = false;
    bool frameOverlayBottom_ = false;
    // Size and colour of the burn-in text, as indices into the two tables below.
    // The text is drawn at the font's own rasterized size (never stretched), so a
    // size change reopens the overlay fonts rather than scaling their glyphs.
    int overlaySize_ = 0;
    int overlayColor_ = 0;
    // Rebuilt once per render by updateFrameOverlay(); both sinks draw these.
    std::string overlayFile_;
    std::string overlayTime_;
    // Burn-in font for the main renderer, kept apart from textFont_ because it is
    // opened at the size the Size menu asks for. The review window has its own
    // (reviewTextFont_): glyph textures belong to the renderer that made them.
    TextFont overlayFont_;

public:
    // The Size and Colour choices, shared by the View menu rows that pick them and
    // by the drawing code that applies them, so labels and effects cannot drift.
    struct OverlaySizeOption { const char* name; float pt; };
    struct OverlayColorOption { const char* name; SDL_Color c; };
    static constexpr OverlaySizeOption kOverlaySizes[] = {
        { "Small",  10.0f }, // the UI font's own size: crisp, and matches the chrome
        { "Medium", 14.0f },
        { "Large",  20.0f },
    };
    static constexpr OverlayColorOption kOverlayColors[] = {
        { "White",  { 240, 240, 240, 255 } },
        { "Yellow", { 255, 214,  64, 255 } },
        { "Green",  { 120, 232, 140, 255 } },
        { "Black",  {  12,  12,  12, 255 } },
    };

private:

    // Pair aligned audio (via the naming config's query_audio callback) with a
    // freshly added EXR sequence. On by default; persisted in user prefs.
    bool attachAudioToSeq_ = false;
    SDL_FRect attachAudioToSeqRect_{};

    // Prompt before an action that would discard unsaved changes (Quit, New,
    // Close, Open). On by default; persisted in user prefs.
    bool warnUnsaved_ = true;
    SDL_FRect warnUnsavedRect_{};

    // Settings-panel mirror of the sync panel's master switch (syncNetwork_);
    // both rows drive toggleSyncNetwork(), so they share one persisted pref.
    SDL_FRect syncNetworkRect_{};

    // Open the local control channel (the loopback listener the MCP server drives).
    // Off unless jplay_preferences.conf says otherwise, so a fresh install binds no
    // socket; toggling it starts/stops control_ live. Persisted in user prefs.
    bool mcpEnabled_ = false;
    SDL_FRect mcpEnabledRect_{};

    // Gates the top-bar Proxy dropdown entirely: off, the button is not laid out
    // (zero width — see the top-bar measurement pass) and the active mode is
    // forced to Full regardless of timeline_.proxyMode. Off by default, like the
    // other opt-in Advanced switches above. Persisted in user prefs.
    bool proxyEnabled_ = true;
    SDL_FRect proxyEnabledRect_{};

    // HDR output. When enabled (persisted in prefs) the main window uses SDL's "gpu"
    // (Vulkan) renderer so the SDL_GPU HdrColorPass runs. That pass renders the
    // transformed program into an offscreen float target, which is what an external
    // sink (review monitor / NDI) is fed from.
    //   hdrPipeline_ — the gpu backend + HdrColorPass is active (drives the float render
    //                  path and HDR-sink capability).
    //   hdrActive_   — the MAIN window's swapchain is scRGB (HDR on-screen). Only decides
    //                  whether the main window draws straight to its swapchain (HDR) or via
    //                  the offscreen target that SDL blits with a linear->sRGB encode (SDR).
    // There is only ever ONE HDR sink: the main window when nothing else is out, and the
    // review monitor / SDI otherwise (the main window then drops to sRGB). syncHdrSink_()
    // enforces that. The renderer's output colorspace is fixed at creation (GPU_SetVSync
    // re-asserts it), so switching means a full renderer rebuild; the hdrOutput_ pref
    // itself still needs a restart because it decides the backend.
    bool hdrOutput_ = false;
    bool hdrPipeline_ = false;
    bool hdrActive_ = false;
    bool hdrSinkOwned_ = false; // last external-sink state syncHdrSink_ acted on (edge trigger)
    float hdrHeadroom_ = 1.0f;
    float hdrSdrWhite_ = 1.0f; // renderer SDR white point (scRGB units); logged at startup
    // HDR paper-white control: the master nit level mapped to panel SDR white for
    // PQ displays. Lower = brighter image. Persisted; tuned via the integer slider
    // at the top of the "Output" menu (shown only while hdrOutput_ is on, and greyed
    // there unless the HDR pipeline is actually active).
    float hdrRefWhiteNits_ = 100.0f;
    static constexpr float kHdrRefMinNits = 48.0f;
    static constexpr float kHdrRefMaxNits = 300.0f;
    // HDR sink diagnostics (terminal log), gated on "Debug Logging" (jplayDebugLogging).
    // The routing decision is re-evaluated every frame, so each distinct state is
    // logged once, on change — silent in steady state. Both markers are reset when the
    // checkbox is toggled so re-enabling re-reports the state as it stands.
    uint32_t hdrDiagReadFmt_ = 0;     // last SDL_RenderReadPixels surface format logged
    std::string hdrDiagSink_;         // last logged sink/feed configuration line

    // EXR clips added from the command line whose audio pairing was deferred:
    // media loads during init(), before the Python interpreter is ready, so the
    // query_audio lookup is run once from the run() loop after Python comes up.
    std::vector<int> pendingCmdlineAudioClipIds_;

    // Scrubbing the playhead snaps to nearby clip start frames (magnet toggle in
    // the info bar). Enabled by default; persisted in user prefs.
    bool snapPlayhead_ = true;

    // Timeline tool mode (the button pair beside the snap toggle; V and C).
    // Cursor is everything the timeline has always done: scrub, drag, trim.
    // Razor turns a click into a cut of the clip under it - so while it is on the
    // drag/trim/fade probes are skipped, since they would fight the razor for the
    // same press. Deliberately not persisted: a modal tool that survived a
    // restart would have the next launch cutting clips on its first click.
    enum class TimelineTool { Cursor, Razor };
    TimelineTool timelineTool_ = TimelineTool::Cursor;
    bool razorMode() const { return timelineTool_ == TimelineTool::Razor; }
    void setTimelineTool(TimelineTool t); // selects (never toggles) and reports it
    bool razorOverTracks() const;         // cursor is inside the track rows right now
    // The cut the razor is currently offering: the hovered clip and the frame the
    // line sits on (snapped when snapPlayhead_). razorClipId_ is -1 when the
    // cursor is over no clip, which is also what suppresses the line.
    int razorClipId_ = -1;
    int64_t razorCutFrame_ = 0;
    // Split the clip at a timeline frame: the head keeps [start, frame), a new
    // clip takes the rest. Cuts the clip's whole audio-follows-video link group
    // at the same frame. No-op when the frame is at or outside the clip's edges
    // (a cut there would leave a zero-length clip). Undoable in one step.
    void splitClipAt(int clipId, int64_t frame);
    // Cursor x -> the frame the razor line lands on for a clip, snapped to nearby
    // clip edges and the playhead when snapPlayhead_ is on, and clamped so the
    // cut always leaves at least one frame on each side.
    int64_t razorFrameFor(const Clip& c, float x) const;

    // Clip-drop mode when a moved clip overlaps others. Overwrite (the default):
    // carve room out of both sides — a clip the drop lands inside is split in
    // two, and any clip fully covered is removed. Ripple: push everything
    // downstream, cut nothing. Not a persisted setting: the mode is Overwrite
    // unless Ctrl is held, and is re-read from the keyboard every frame while a
    // clip drag is live (see update()).
    enum class DropMode { Overwrite, Ripple };
    DropMode dragDropMode_ = DropMode::Overwrite;
    static bool rippleModifierHeld(); // Ctrl down right now

    // The naming-config [picker:*] set (key + display label), in display order,
    // loaded lazily from list_pickers once Python is ready. Consumed by the clip
    // right-click picker menu (openClipPickerMenu); there is no top metadata bar.
    // One clip's outcome from switchClipsByPicker. `switched` is the only success:
    // `to` empty means nothing resolved, and `to` set with switched false means the
    // clip was already there (or the new media would not open) — see `note`.
    struct PickerSwitch {
        int clipId = 0;
        std::string from;     // the clip's media path before the pick
        std::string to;       // what the pick resolved to, when it resolved
        bool switched = false;
        std::string note;     // why not, when it did not switch
    };
    std::vector<PickerSwitch> switchClipsByPicker(const std::string& key,
                                                  const std::string& value,
                                                  const std::vector<int>& clipIds);

    struct MetaPicker {
        std::string key;   // naming-config picker key ("department", "asset", ...)
        std::string label; // display caption ("Department", "Asset", ...)
        // The config's multi_select_picker: where a menu opened on several clips
        // stops (see PickerDef::multiCommit and buildPickerColumns).
        bool multiCommit = false;
    };
    std::vector<MetaPicker> metaPickers_;
    bool metaPickersBuilt_ = false;  // list_pickers has been consumed into metaPickers_

    // Right-click popup on a timeline clip: lists the clip's applicable pickers
    // (Department/Asset/Version) so a value can be swapped in without moving the
    // playhead. Populated on demand from describe_pickers of the clicked clip.
    ContextMenu clipMenu_;

    // One picker cascade in progress. The pickers navigate rather than commit on
    // every click: choosing an upstream picker (Department/Asset) re-resolves a
    // representative path and refreshes the downstream pickers in place; only the
    // last one commits the source switch. See App_TimelineClip.cpp.
    //
    // Every picker option runs a Python query (resolve / describe) that would
    // freeze the UI if run inline, so clicks run on pickerWork_ while the view
    // stays up showing an animated LOADING overlay. queryId is bumped for every
    // new query and whenever the view closes; a completion whose captured id no
    // longer matches is a stale/cancelled result and is dropped (the background
    // call still finishes, it just touches nothing). Closing the view is therefore
    // the cancel.
    //
    // A decorate_pickers pass (the optional labels/colors second pass) is the one
    // query that can run for tens of seconds — a site looking a hundred versions
    // up in a database — so it does not use that completion route at all: it
    // publishes through the channel below as it goes. The worker merges each
    // batch the callback yields into `states` under `mtx` and raises `dirty`; the
    // main thread lifts a snapshot out once a frame and hands it to the view.
    // Nothing here touches the GIL, so the UI never waits on the callback; the
    // colors simply arrive in batches. Setting `cancel` abandons the generator at
    // its next resume — see startPickerDecorate.
    struct PickerDecorateStream {
        std::mutex mtx;
        std::vector<PickerState> states;  // latest merged snapshot (guarded by mtx)
        bool dirty = false;               // a batch landed since the last poll (guarded by mtx)
        std::atomic<bool> cancel{ false };
        std::atomic<bool> done{ false };  // the call returned (finished or cancelled)
    };
    struct PickerCascade {
        std::vector<int> clipIds;   // clips the commit applies to
        std::string repPath;        // representative path, advanced as the user navigates
        std::vector<std::pair<std::string, std::string>> chain; // (key,value) picks so far, in display order
        uint64_t queryId = 0;
        bool loading = false;       // a query is in flight (the menu shows LOADING; the panel just waits)
        // The decoration in flight for this cascade, at most one. Held here and by
        // the worker; dropping this end is the cancel. decorateQueryId is the
        // queryId the decoration belongs to — once the cascade's own id moves past
        // it (a click, a new target clip, a close) the stream is stale.
        std::shared_ptr<PickerDecorateStream> decorate;
        uint64_t decorateQueryId = 0;
        // How this cascade's view takes an instalment of decorated states. Set by
        // the view when it opens the cascade and kept for its lifetime — it is a
        // property of the view, not of one query, and every describe along the way
        // decorates through the same one.
        std::function<void(const std::vector<PickerState>&)> decorateApply;
    };
    // Two independent cascades: the clip right-click menu and the Clip Source
    // panel. They are separate so right-clicking a clip while the panel is open
    // doesn't clobber the panel's half-navigated chain (and vice versa).
    PickerCascade menuCascade_;
    PickerCascade panelCascade_;
    float pickerMenuX_ = 0, pickerMenuY_ = 0; // clipMenu_ anchor

    // One picker as a view sees it, derived from the cascade + a describe_pickers
    // result by buildPickerColumns(). The clip menu maps these to table columns,
    // the Clip Source panel to vertically stacked sections.
    struct PickerColumn {
        std::string key;                   // naming-config picker key
        std::string label;                 // display caption
        std::vector<PickerOption> options; // empty => rendered but cleared (awaiting an upstream pick)
        int  checkedIdx = -1;              // option to mark as current, -1 = none
        bool commit = false;               // clicking an option commits rather than navigates
    };

    // Info-bar toolbox popup: clip-level actions, opened from the toolbox icon
    // left of "Show in Sequence". Grows upward from the button. Also carries the
    // clip right-click menu, which adds the Clip Range header band below.
    ContextMenu clipToolboxMenu_;

    // Info-bar zoom-fit popup, opened from the caret on the "Fit" button. Grows
    // upward from the button, like the toolbox menu beside it.
    ContextMenu fitMenu_;

    // Track-header popup, opened from the burger in the left gutter: the row's
    // enable/disable toggle and its remove.
    ContextMenu trackMenu_;

    // "Clip Range" header on the clip right-click popup: a ruler showing the cut
    // range within the full source, editable in/out fields, and the two range
    // presets. -1 when the popup has no range section (a clip whose media is
    // gone). Frame values are shown in media numbering — the
    // source's first frame number plus the offset, out inclusive — matching the
    // readout the timeline shows while drag-trimming. See App_TimelineClip.cpp.
    int clipRangeClipId_ = -1;
    TextInput clipRangeIn_, clipRangeOut_;   // the two editable fields
    bool clipRangeEditing_ = false;          // a field has focus (text input is on)

    std::mutex dialogMutex_;
    std::string pendingOpenPath_;
    std::string pendingSavePath_;
    bool pendingSaveResolved_ = false;           // Save As dialog closed, cancel included
    std::string pendingExportOtioPath_;          // .otio chosen via the Export OTIO dialog
    std::vector<std::string> pendingMediaPaths_; // files chosen via the ProjectExplorer browse dialog
    std::string pendingCreateFromDir_;           // folder chosen via CREATE FROM DIRECTORY
    std::string pendingReplaceSourcePath_;       // file chosen via the Replace Source dialog
    std::string replaceSourceMediaId_;           // media that pick replaces (main thread only)

    // ---- Missing-source relocation ----------------------------------------
    // Load no longer scans for missing files: the timeline appears instantly and
    // a source is only discovered missing lazily, when the cache/playback fails
    // to open it (Media::openFailed()). Such clips render red and, when clicked,
    // open the Missing Source modal.
    enum class LoadOrigin { Project, Otio, Directory };
    struct RelocateRule { std::string oldPrefix, newPrefix; };
    LoadOrigin loadOrigin_ = LoadOrigin::Project;
    std::string loadSourceLabel_;             // fileLabel of the loaded file/dir (for status)
    std::vector<RelocateRule> relocateRules_; // learned path substitutions (persist for the session)
    std::unordered_set<std::string> relocateRuleTried_; // media ids a rule has been tried on (avoids re-globbing)
    std::string relocatingMediaId_;           // media the modal is currently relocating
    bool pendingRelocateDirReady_ = false;    // browse folder chosen (guarded by dialogMutex_)
    std::string pendingRelocateDir_;

    // The Missing Source modal (styled in-app overlay; see App_Project.cpp).
    bool relocateModalOpen_ = false;
    TextInput relocateDirInput_;              // editable new-directory field
    Button relocateBrowseBtn_;                // opens the folder picker to fill the field
    Button relocateBtn_, relocateCancelBtn_;
    SDL_FRect relocateDialogRect_{};
    std::string relocateError_;               // inline validation message (empty = none)

    void buildMenu();
    void populateOcioViewSubmenu(); // (re)fill ocioViewSubmenuIdx_ — called at init and on display change
    void setSequenceView(int seqIdx); // apply a view filter (-1 = All) + fit/playhead, from the Sequence button
    void populateOutputMenu();      // (re)fill the "Output" menu: HDR controls + backends + review displays
    void updateOutputMenu();        // rebuild the Output menu when the display/backend set changes (per frame)
    void buildMetaPickers(); // load the config picker list (lazy; needs Python ready)
    // Index into timeline_.sequences for the active filter, or -1 (a project scope
    // or All — both cover more than one sequence).
    int filteredSeqIdx() const {
        return (viewSeqIdx_ >= 0 && viewSeqIdx_ < (int)timeline_.sequences.size()) ? viewSeqIdx_ : -1;
    }
    bool projScoped() const { return timeline_.findProjectById(viewProjId_) != nullptr; }
    // No scope at all: every sequence is in view.
    bool viewAll() const { return filteredSeqIdx() < 0 && !projScoped(); }
    void clearProjectView() { viewProjId_ = -1; viewProjSeqIds_.clear(); }
    // The scope is tied to the loaded set of clips, so whatever replaces the timeline
    // drops it. Which projects were opened whole needs no reset: it lives in the
    // timeline (SourceProject::openedWhole) and is replaced along with it.
    void resetProjectScopeState() {
        clearProjectView();
        scratchSeqId_ = -1;
        scratchKind_ = ScratchKind::None;
        scratchTrackCount_ = 1;
        timeline_.trackScopeSeqId = -1;
        // The Layout and Stack stages *are* their scratch sequence, and that went
        // with the timeline being replaced here — without one there is nothing to
        // tile or stack, so the frame is where a fresh set of clips lands.
        if (compareStage())
            stage_ = PlayerStage::Frame;
    }
    // Scope the view to every sequence of one project, as the popup's project rows
    // do: fits the project's span and lands the playhead at its start.
    void setProjectView(int projId, std::vector<int> seqIds);
    // The scope Backspace would return to. pushViewHistory records the current
    // scope ahead of a switch to "next" (and does nothing when that is where we
    // already are); goBackView pops the newest still-reachable entry and applies
    // it exactly as the Sequence picker would.
    ViewHistoryEntry currentViewEntry() const;
    void pushViewHistory(const ViewHistoryEntry& next);
    void goBackView();
    // Stand up an empty scratch sequence named `name`, record what it replaces and
    // scope to it; returns its index (always the last). Shared by the two kinds —
    // the caller fills it with clips and sets scratchTrackCount_.
    int beginScratchSequence(const std::string& name, ScratchKind kind);
    // Show one source on its own in a scratch sequence (see scratchSeqId_).
    // `srcFrame` is the source frame to land the playhead on, so F1 can match-frame
    // out of a clip and see the handles either side of its cut; 0 is the source's
    // start, which is where a bin double-click opens it.
    void openSourceView(const std::string& path, int64_t srcFrame = 0);
    // F1: the source of the top-most clip under the playhead, at the same frame.
    void openSourceViewAtPlayhead();
    // Discard the scratch view and put back what it replaced (scope, playhead,
    // selection, zoom/pan), including the view-history step scoping to it added. A
    // no-op when none is up.
    void dropScratchView();
    // Every scope change other than entering the scratch view itself drops it: the
    // view only exists while it is what you are looking at. `keepSeqId` is the
    // sequence being scoped to, so opening the view doesn't immediately close it.
    void dropScratchViewUnless(int keepSeqId);
    // The view scope in the form the project file stores it, and its restore.
    Project::ViewState viewState() const;
    void applyViewState(const Project::ViewState& state);
    // The sequence indices the current view covers: the filtered one, the scoped
    // project's (in timeline order), or all of them.
    std::vector<int> viewSeqIndices() const;
    // fn(const Clip&) over the clips of every in-view sequence.
    void forEachViewClip(const std::function<void(const Clip&)>& fn) const;
    // Absolute [start,end) the in-view sequences span; false when they hold no clips.
    bool viewSpan(int64_t& start, int64_t& end) const;
    // Absolute [start,end) the current scope occupies, false in the All view. Same
    // as viewSpan for a populated scope; a scope whose sequences hold no clips
    // (a freshly added one) reports the zero-width point where its clips will
    // land, so "outside the scope" is well defined before the first clip arrives.
    bool scopeRange(int64_t& start, int64_t& end) const;
    // The frames playback and the ruler's in/out highlight cover: the in/out range
    // confined to the current scope, as [lo, hi] inclusive. A scoped sequence plays
    // only its own frames, so the range can never reach in front of it (which would
    // read as a negative frame in the scope-relative numbering).
    void playbackRange(int64_t& lo, int64_t& hi) const;
    // The Sequence button's label: the scoped project, the filtered sequence, or "All".
    std::string sequenceViewLabel() const;
    // Fit the view to the current scope (the whole timeline in the All view). Does
    // not move the playhead.
    void fitToFilteredSequence();
    // Like timeline_.clipAt() but respects the active sequence filter. Hidden
    // clips are skipped unless includeHidden is set.
    const Clip* getTopMostClipAtFrame(int64_t frame, bool includeHidden = false) const;
    // What the program image at one timeline frame is made of. `a` is always the
    // clip getTopMostClipAtFrame would return, so outside a transition this says
    // exactly what the player has always shown. Inside one, `b` is the other side
    // of the cut and `mix` is b's weight (a at 1-mix), sampled so neither end of
    // the span is a degenerate pure-a / pure-b frame.
    // Compositing is capped at two layers, deliberately: "fade over the track
    // below" needs exactly one layer down, and supporting a fade over a fade over a
    // plate would turn the player into a general N-layer compositor for very little
    // gain. `b` is that one lower/other layer, whatever put it there.
    struct ProgramSource {
        const Clip* a = nullptr; int64_t aSrc = 0;
        const Clip* b = nullptr; int64_t bSrc = 0; // null = nothing to blend with
        float mix = 0.0f;                          // weight of b (0 = pure a)
        // Opacity of `a` when there is no `b` to fade over: the clip dims toward
        // black. Always 1 when b is set, since the fade is expressed as `mix` then.
        float fade = 1.0f;
    };
    ProgramSource programSourceAt(int64_t frame) const;
    // Next visible clip below `aboveTrack` at `frame` — the second layer of the
    // composite. Same filter as getTopMostClipAtFrame (audio and hidden clips are
    // skipped), so a disabled clip lets the one under *it* show through instead.
    const Clip* getClipBelow(int64_t frame, int aboveTrack) const;
    // Drop transitions that have gone stale (see Timeline::resolveTransition) and
    // re-clamp the survivors against the handles their clips still have — a trim
    // that eats into a handle must shorten the dissolve that was spending it.
    // Called from the edit-commit sites, after positions have settled.
    void pruneTransitions();
    // After a timeline arrives from disk or a sync host: rebuild the dissolve id
    // allocator from the loaded ids (Project::load does not carry it) and drop any
    // that no longer resolve.
    void adoptLoadedTransitions();
    // The live transition covering `frame` on `track`, or null. Used by the
    // timeline's hit tests and draw pass.
    Transition* transitionAt(int track, int64_t frame);
    // Add a dissolve on the cut at the out point of clip `aClipId`, using the
    // default duration (1 s at project fps) clamped to the available handles.
    // Reports why it can't via setStatus.
    void addDissolveAtClipOut(int aClipId);
    // Add a dissolve on the cut nearest the playhead, on the playhead's clip's
    // track (Ctrl+D).
    void addDissolveAtPlayhead();
    void deleteSelectedTransition();
    // Set a clip's head/tail fade, clamped to Timeline::fadeLimit. `frames` of 0
    // removes it. edge: 0 = fade in (head), 1 = fade out (tail).
    void setClipFade(int clipId, int edge, int64_t frames);
    // Add a default fade (1 s at project fps, clamped) to every selected clip, or
    // to the clip under the playhead when nothing is selected.
    void addFadeToSelection(int edge);
    // Fade drag. fadeHandleAt finds the clip whose head/tail ramp apex dot is under
    // (x, y) — the dot sits on the clip's top edge, clear of the trim zones at the
    // sides. A clip with no fade yet offers a dot in each top corner, so the gesture
    // both creates and resizes.
    Clip* fadeHandleAt(int track, float x, float y, int& edge);
    void beginClipFade(Clip& c, int edge);
    void updateClipFade(float mouseX);
    void commitClipFade();
    // Frames of contiguous lower-track coverage running inward from the clip's head
    // (edge 0) or tail (edge 1), capped at Timeline::fadeLimit. That run is the fade
    // length addFadeToSelection uses, so the ramp spans exactly the overlap the
    // compositor can blend over. 0 when nothing shows through that edge.
    int64_t fadeCoverageFrames(const Clip& c, int edge) const;

    // ---- curve lane (App_CurveLane.cpp) ------------------------------------
    bool curveMode() const { return curveTrack_ >= 0; }
    // Enter the lane on `track` / leave it. Entering from a clip's menu is the
    // only way in; the row header's button is the way out (Esc also leaves).
    void enterCurveMode(int track, CurveParam param);
    void exitCurveMode();
    // The curve `curveParam_` names on a clip. The one place the mode's parameter
    // binds to a field, so adding a parameter touches nothing else.
    Curve& clipCurve(Clip& c) const;
    const Curve& clipCurve(const Clip& c) const;
    void curveRange(float& lo, float& hi) const;  // value range the lane maps
    std::string curveValueLabel(float v) const;   // readout next to a dragged point
    // Height the expanded lane takes: what the whole stack took before, so the
    // player does not jump, floored at something actually editable.
    float curveLaneHeight() const;
    // The lane's plot area for one clip (its band, inset so points at the range
    // ends stay inside), and the mapping between a value and a y inside it.
    SDL_FRect curvePlotRect(const Clip& c) const;
    float curveValueToY(const SDL_FRect& plot, float v) const;
    float curveYToValue(const SDL_FRect& plot, float y) const;
    void renderCurveLane();
    // Mouse in the lane. Each returns true when it consumed the event, so the
    // timeline's own clip drag / trim / gap handling stays out of the lane.
    bool curveLaneMouseDown(float mx, float my, bool rightButton, int clicks);
    bool curveLaneMouseMotion(float mx, float my);
    bool curveLaneMouseUp();
    // Clip under mx on the lane's row, plus the point/box `my` is on
    // (-2 = none, -1 = the start box, >= 0 a point index).
    Clip* curveClipAt(float mx, float my, int& ptIdx);
    // Memoized topmost visible clip under the playhead for the current frame.
    // Several panel renderers each need it within one render() pass; without this
    // they'd each redo the full O(clips) scan of the same frame. Valid only during
    // a single render(): the cache is reset on both entry and exit (see App::render),
    // and the clip set is immutable while drawing, so no invalidation inside the pass
    // is needed. A call from outside a pass therefore always recomputes - it must,
    // since an edit between passes can reallocate the clip vector it points into.
    const Clip* playheadClip() const {
        if (!playheadClipValid_) {
            playheadClipCache_ = getTopMostClipAtFrame(timeline_.playhead);
            playheadClipValid_ = true;
        }
        return playheadClipCache_;
    }
    mutable const Clip* playheadClipCache_ = nullptr;
    mutable bool        playheadClipValid_ = false;
    // Per-track index of the view's video clips, sorted by timelineStart, so
    // "topmost clip at frame f" is a binary search per row instead of a scan of
    // every clip in the view. Clips never overlap within a row, so one
    // upper_bound plus a step back answers each row exactly.
    //
    // Built once per drawFrame and torn down on the way out, on the same bargain
    // playheadClip() takes above: the clip set is immutable for the length of a
    // pass, but between passes an edit can reallocate the vectors these point
    // into. Outside a pass clipIndexValid_ is false and the lookups fall back to
    // the linear scan, so event handlers and the control channel are unaffected.
    void buildClipIndex();
    std::vector<std::vector<const Clip*>> clipIndex_; // [track] -> clips, by start
    bool clipIndexValid_ = false;
    // Topmost clip at `frame` off clipIndex_. Only valid while the index is.
    const Clip* indexedClipAt(int64_t frame, bool includeHidden, int belowTrack) const;
    // Display index of a picker key within metaPickers_ (-1 if not configured).
    int pickerDisplayIndex(const std::string& key) const;
    // Turn an already-resolved describe_pickers result into the per-picker view
    // model, honouring `c`'s navigation state: pickers that don't apply to the
    // current path are dropped, ones past the active choice are returned cleared,
    // and the last configured picker is always present and flagged as the commit.
    // A cascade covering several clips stops earlier — at the config's
    // multi_select_picker — returns nothing past it, and lists that picker's
    // options as the values the selection itself carries (selectionPickerValues).
    // View-agnostic — both the clip menu and the Clip Source panel build from
    // this so the cascade rules live in one place (the panel is always one clip,
    // so the multi rule never touches it).
    std::vector<PickerColumn> buildPickerColumns(const std::vector<PickerState>& states,
                                                 const PickerCascade& c) const;
    // The distinct values `key` currently takes across `clipIds`, in selection
    // order — the option list buildPickerColumns gives the commit picker when the
    // cascade covers several clips, in place of the one clip's describe result.
    // Cached media metadata, so no Python query.
    std::vector<PickerOption> selectionPickerValues(const std::string& key,
                                                    const std::vector<int>& clipIds) const;
    // Build clipMenu_'s columns from already-resolved picker `states` and open the
    // table. The describe_pickers call that produces `states` runs off the main
    // thread (see startPickerNavigate); this does only the column layout + wiring.
    // navigating: this rebuild is an in-place cascade step (a picker click), not a
    // fresh open — keep the menu's accumulated minimum column widths / row count
    // so the panel doesn't jump smaller.
    void rebuildClipPickerMenu(const std::vector<PickerState>& states, bool navigating);
    // Apply a decoration's labels/colors to the open clip menu without reopening
    // it — see the definition in App_TimelineClip.cpp.
    void relabelClipPickerMenu(const std::vector<PickerState>& states);
    // A picker option was clicked: run its Python query on pickerWork_ while the
    // view shows LOADING, then apply the result on the main thread (unless the view
    // closed / a newer click superseded it — see PickerCascade::queryId). `onDone`
    // runs on the main thread for a surviving completion, after the cascade state
    // has been updated, and is how each view refreshes itself.
    // Navigate: re-resolve the representative path for an upstream pick, then
    // re-describe. onDone(states, ok): ok=false means describe_pickers failed and
    // there is nothing to show, so the view should dismiss/clear itself. A pick with
    // no media behind it sets the "NO MEDIA FOR …" status here and reports ok=true
    // with the cascade left untouched, so the view re-renders unchanged and the user
    // can pick again.
    // Commit: resolve the chosen chain for every clip in the cascade, then swap
    // each clip's media; `onDone` runs once afterwards. With several clips the
    // commit lands on an upstream picker (see buildPickerColumns), so each clip
    // finishes on the top option of the last picker — its own latest — and a clip
    // the pick doesn't resolve for is left alone with a warning. Several clips is
    // also several Python round-trips, so that path resolves on the progress
    // worker behind a modal dialog (beginProgress) instead of pickerWork_.
    void startPickerNavigate(PickerCascade& c, const std::string& key,
                             const std::string& value, int myIdx,
                             std::function<void(const std::vector<PickerState>&, bool)> onDone);
    void startPickerCommit(PickerCascade& c, const std::string& key,
                           const std::string& value, std::function<void()> onDone);
    // Optional second pass over a describe result the view is already showing:
    // hands `states` to Python's decorate_pickers (see PythonBridge.h) on
    // decorateWork_ and calls the cascade's decorateApply on the main thread with
    // the decorated states — once if the callback returns them, repeatedly if it
    // yields them in batches — so the view redraws itself with each instalment.
    // Deliberately does NOT bump the cascade's queryId or set `loading`: decoration
    // is cosmetic, so it must neither cancel a query nor make the view look busy,
    // and a click that arrives meanwhile supersedes it by bumping the id itself.
    // Starting one cancels the cascade's previous decoration; there is only ever
    // one per cascade, and it is the newest describe that gets decorated.
    // No-op when no site registered the callback, which is the shipped state.
    void startPickerDecorate(PickerCascade& c, const std::vector<PickerState>& states);
    // Main thread, per frame: hand each cascade whatever its decoration has merged
    // so far to the view, and drop decorations the cascade has moved past (which
    // also tells the worker to abandon the generator).
    void pollPickerDecorations();
    // Let go of `c`'s decoration: the worker stops at its next resume and nothing
    // it produces is applied from here on.
    void cancelPickerDecorate(PickerCascade& c);
    // Resolve `c`'s navigated chain plus one final pick to a media path, the way a
    // commit does, but without touching any clip. For the panel's drag-out, which
    // carries the pick's media instead of swapping it in. Runs the Python resolves
    // inline (a couple of calls on a deliberate gesture, like openClipPickerMenu's
    // own describe). False when the pick has no media behind it. `missedPick`, when
    // given, reports that `value` itself didn't resolve and `out` is therefore the
    // path the chain had reached — a different source than the caller asked for.
    bool resolvePickerPick(const PickerCascade& c, const std::string& key,
                           const std::string& value, std::string& out,
                           bool* missedPick = nullptr) const;
    // If a query is in flight but the clip menu has closed, treat the close as a
    // cancel: bump the cascade's queryId so the pending completion is dropped. Per
    // frame. (The panel cancels the same way from closeClipSource().)
    void cancelPickerIfClosed();
    // Draw the animated LOADING overlay over `bounds` while `c`'s query is in
    // flight (no-op otherwise). Used over the clip menu's table only; the
    // Clip Source panel deliberately shows no overlay.
    void renderPickerLoading(const PickerCascade& c, const SDL_FRect& bounds);
    // "Unpack Clip": for every option of picker `key` other than the clip's
    // current one, add a new clip mirroring the source clip's span but with its
    // media resolved to that option, stacked on the first free track below.
    void unpackClip(int clipId, const std::string& key);
    // Append an "Unpack Clip" row with one child per configured picker. Queries no
    // Python beyond the one-off picker list, so opening a menu never waits on a
    // describe; a child with nothing to unpack reports that when clicked.
    // Used by the clip right-click menu.
    void appendUnpackItems(const Clip& clip, std::vector<ContextMenu::Item>& items);
    // Append the "Find and Attach Audio" row (Current / All Clips) when `clip` is
    // an image sequence with no audio under it. Used by the same menu.
    void appendAttachAudioItems(const Clip& clip, std::vector<ContextMenu::Item>& items);
    // Right-click on a timeline clip: the clip-actions menu for the clip under
    // (mx,my), or — with Ctrl held — clipMenu_ listing its applicable pickers.
    void handleTimelineRightClick(float mx, float my);
    // Shared tail of both: select the clip if it isn't already in the selection,
    // then open the clip-actions popup — or, with Ctrl held, its picker menu.
    void openClipRightClickMenu(const Clip& clip, float mx, float my, bool growDown = false);
    // Clip-actions popup (copy / copy source path / paste / unpack), anchored at
    // the cursor. Copy and paste act on the selection, like Ctrl+C / Ctrl+V.
    void openClipContextMenu(const Clip& clip, float mx, float my, bool growDown);
    // The popup's Clip Range band: drawn into / driven by the box ContextMenu
    // reserves for it. handleEvent returns true when the band consumed the event.
    struct ClipRangeLayout {
        SDL_FRect title, ruler, ends, inField, outField, srcBtn, shotBtn;
    };
    // Sub-rects derived from the band's box alone, so the render pass and the
    // event pass can never disagree on where a control is.
    ClipRangeLayout clipRangeLayout(const SDL_FRect& box, bool hasShot) const;
    float clipRangeBtnW(const char* label) const;  // label width + button padding
    float clipRangeBandW(bool hasShot) const;      // narrowest the band can be drawn
    // Frames the clip's media can supply, and the real frame number of source
    // frame 0 (1001 for shot.1001.exr; 0 for video/stills).
    void clipRangeSource(const Clip& c, int64_t& frames, int64_t& base) const;
    void renderClipRangeHeader(SDL_Renderer* r, const SDL_FRect& box);
    bool clipRangeHandleEvent(const SDL_Event& e, const SDL_FRect& box);
    void endClipRangeEdit(bool apply);   // apply (or drop) the focused field's text
    void clipRangeCloseIfMenuClosed();   // per-frame: the popup closing ends the edit
    // Set the clip's source range from 0-based source frames (out inclusive),
    // keeping its timeline position. Clamped to the media; undoable as one step.
    void applyClipRange(int clipId, int64_t inSrc, int64_t outSrc);
    // Open the cascading picker menu for one or more clips. Navigation uses the
    // first clip as the representative for building columns; the final commit
    // applies the navigated chain to every listed clip. Several clips also stop
    // the menu at the config's multi_select_picker — see buildPickerColumns.
    void openClipPickerMenu(const std::vector<int>& clipIds, float mx, float my);
    // Swap `clip`'s media to `newPath`, matching the previous clip span where the
    // new media allows, and dropping the old media from the pool if now unused.
    void replaceClipMedia(Clip& clip, const std::string& newPath);
    void openProjectDialog(); // pick a .jpproj or .otio; the extension picks the loader
    void saveProjectDialog();
    void saveProjectQuick(); // overwrite current file, or prompt if never saved
    // "Export OTIO": warn about everything in this project the .otio cannot carry
    // (see OtioExport::survey), then prompt for a location if the user goes ahead.
    // Export only — the project keeps its own .jpproj identity either way.
    void exportOtio();
    void createFromDirDialog();                        // pick a folder, then discover a project from it
    void createProjectFromDirectory(const std::string& root); // discover + build off the main thread (progress modal)
    // Main-thread completion of the above: adopt the worker's timeline (consumed)
    // or report why nothing came back.
    void finishCreateFromDirectory(const std::string& root, Timeline& tl,
                                   int nextId, int nextSeq, int nextShot,
                                   int64_t frames, size_t discovered, bool inspectOk);
    void showInSequence();      // expand the active clip into its full sequence (jump if it already exists)
    // "Show in Sequence" graft: from an already-loaded project OTIO (`src`),
    // append the sequence named `seqName` into the current timeline as a new
    // sequence, remapping clip/shot/sequence ids and sharing media with the
    // existing pool (Media ids are path-derived, so a path already loaded is
    // reused). The shot named `keepShot` has its media swapped to `keepPath` — the
    // clip the action was invoked on — so the originating media is preserved
    // rather than replaced by the OTIO's published version. The sequence is looked
    // up by `seqName`, then by `sceneName` (the OTIO's sequence_name and scene_name
    // are unrelated strings, and a caller resolving names off a media path only
    // recovers the scene), then by the shots' scene_name tags. Returns the new
    // sequence index, or -1 when none of the three finds a sequence with clips.
    int  graftOtioSequence(Timeline& src, const std::string& seqName, const std::string& sceneName,
                           const std::string& keepShot, const std::string& keepPath);
    // "Open Project: <name>" from the top bar: load `projPath` (a .jpproj or an
    // .otio) and graft every sequence in it the project doesn't already have (by
    // name), then scope to the first one brought in. The existing clips stay put.
    void openResolvedProject(const std::string& projPath, const std::string& projectName);
    // Re-slot this project's sequences into the order `src` (the project document
    // just read) lists them, so an already-loaded sequence stops sitting ahead of
    // the ones grafted around it. Only the indices this project already occupies
    // are rewritten; every other sequence keeps its own.
    void reorderProjectSequences(int projId, const Timeline& src);
    // A clip's shot name: its linked Shot's name (set by discovery/import) if any,
    // else the resolved shot cached on its media.
    std::string shotNameOfClip(const Clip& clip) const;
    // Put the playhead back on the shot it was on before a view change (both
    // setProjectView and scopeToSequence land it on the scope's first frame): the
    // clip in the current view whose media is `mediaPath`, else one whose shot is
    // `shotName`, at the same clip-local source frame. No-op when nothing matches.
    void restoreViewPlayhead(const std::string& shotName, const std::string& mediaPath,
                             int64_t srcFrame);
    // Refresh what the clip under the playhead can open (openSeqName_ /
    // openProjName_ / openProjPath_) out of openTargetCache_. allowResolve = false is
    // cache-only, so during playback this never calls the interpreter or stats a path.
    void resolveOpenTargets(bool allowResolve);
    // Group the loaded sequences by project and lay out the popup's rows. Also a
    // naming-convention lookup per sequence, so likewise once per popup open.
    void buildSequenceMenuRows();
    void openExportMovie();
    void openExportImageSequence();
    void processPendingDialogs();
    static void SDLCALL onOpenChosen(void* userdata, const char* const* filelist, int filter);
    static void SDLCALL onSaveChosen(void* userdata, const char* const* filelist, int filter);
    static void SDLCALL onExportOtioChosen(void* userdata, const char* const* filelist, int filter);
    static void SDLCALL onMediaChosen(void* userdata, const char* const* filelist, int filter);
    static void SDLCALL onReplaceSourceChosen(void* userdata, const char* const* filelist, int filter);
    static void SDLCALL onCreateFromDirChosen(void* userdata, const char* const* filelist, int filter);
    static SDL_HitTestResult SDLCALL hitTest(SDL_Window* win, const SDL_Point* area, void* data);
    // Synchronous event watch so the window keeps repainting during the OS's
    // modal resize/move loop (which otherwise parks run() inside SDL_PollEvent).
    static bool SDLCALL onWatchEvent(void* userdata, SDL_Event* e);
    void drawFrame(bool isLiveMovingOrResizing = false); // one layout+render pass, shared by run() and the resize watch

    void setStatus(const std::string& msg, Uint64 durationMs = 3000);
    // The same transient line drawn as a warning (red badge) rather than the
    // plain one, for a failure the user has to notice — a picker pick that
    // loaded nothing. Always paired with an SDL_LogWarn carrying the detail
    // (which clip, which path) the one-line badge has no room for.
    void setStatusWarn(const std::string& msg, Uint64 durationMs = 6000);
    bool addMediaFile(const std::string& path);
    void addMediaFolder(const std::string& dir); // add all supported media in a dropped folder, end-to-end
    // The media kind implied by a path's extension.
    static ClipType mediaTypeForPath(const std::string& path);
    // Find or create the media-pool entry for `path`. A new entry is opened (audio
    // is probed for its duration instead), gets its metadata and naming-convention
    // values filled in, and is inserted into timeline_.media. Returns null when the
    // file can't be opened, having already set a status message.
    //
    // A pool entry with no clip referencing it is a valid state: it shows in the
    // SOURCES bin dimmed (sourceInTimeline() is false) and can be dragged onto the
    // timeline later. That is what the control channel's "bin" mode produces.
    std::shared_ptr<Media> ensureMedia(const std::string& path, ClipType type);
    // Add one media file as a clip on `track`. A negative `track` auto-picks a
    // suitable row (first matching-type or empty track). An explicit `track` whose
    // existing type conflicts with the file's kind is rejected (empty tracks accept
    // either kind, adopting it). Returns the clip end frame, -1 on failure/reject.
    // When `queryAudio` is true and the file is an EXR sequence, the naming config's
    // query_audio callback is consulted to pair an aligned audio clip below it (see
    // addPairedAudio); bulk adds pass false to skip per-file audio queries.
    // srcIn/srcDur pin the clip's source range instead of taking the whole source
    // (both -1 = whole source); they are clamped to what the source actually has.
    // keepView leaves the current zoom/pan untouched: a drop the user aimed at a
    // spot on screen shouldn't move that spot. It only applies once the timeline
    // has content — the very first clip still fits the view to itself.
    int64_t addMediaFileAt(const std::string& path, int track, int64_t start, bool queryAudio = true,
                           int64_t srcIn = -1, int64_t srcDur = -1, bool keepView = false);
    // Place the audio the query_audio callback returned on a track just below the
    // just-added EXR clip, spanning the same [start, start+dur) with sourceOffset =
    // the callback's frame offset. Prefers the row directly below; else the first
    // free audio track; else a fresh audio track appended at the bottom.
    // probedDurationSec >= 0 supplies a duration already read off-thread (the
    // bulk pass probes in parallel) so this never opens the file on the main
    // thread; < 0 falls back to opening it here (the single-clip path).
    // The new audio clip is linked to videoClipId, so it follows that picture clip
    // through selection, moves, trims and deletion (see Clip::linkedTo).
    void addPairedAudio(Sequence& seq, int videoTrack, int videoClipId,
                        int64_t start, int64_t dur,
                        const std::string& audioPath, int64_t sourceOffset,
                        double probedDurationSec = -1.0);
    // On-demand version of the query_audio pairing above, for the clip menu's
    // "Find and Attach Audio" action: consult the naming config for the clip's
    // EXR media and, if it returns audio, pair it below the clip.
    void findAndAttachAudio(int clipId);
    // Bulk version: walk every clip in the timeline and attach paired audio to
    // each EXR clip that doesn't already have audio under it. When `onlyClipIds`
    // is non-null, only those clips are considered (used for deferred command-
    // line pairing); null means every EXR clip.
    void findAndAttachAudioAll(const std::vector<int>* onlyClipIds = nullptr);
    // Ripple clips on one track of one sequence forward to clear [start, start+dur),
    // so an edit can never open a gap in a neighbouring sequence's internal layout.
    void rippleMakeRoomInSeq(Sequence& s, int track, int64_t start, int64_t duration, int excludeId = -1);
    // Ripple/insert drop for a clip landing on [start, start+duration): every
    // non-dragged clip on the track that starts at or after `start` is pushed
    // forward so the span is clear. Nothing is trimmed or removed. The Ctrl-held
    // clip-drop mode.
    void applyRippleDrop(int track, int64_t start, int64_t duration, const std::vector<int>& excludeIds);
    // Overwrite drop for a clip landing on [start, start+duration): clips
    // overlapped on their tail are trimmed back to `start`, clips overlapped on
    // their head have their in-point advanced to `start+duration`, and clips fully
    // covered are removed (with their shot bars). Nothing ripples. A clip spanning
    // the whole span is split — its head is kept up to `start` and a new clip
    // carries the tail from `start+duration`. excludeIds stay untouched (dragged
    // clips must not cut one another). The default clip-drop mode.
    void applyOverwriteDrop(int track, int64_t start, int64_t duration,
                            const std::vector<int>& excludeIds);
    // Dispatch a clip drop to the active dragDropMode_ handler (overwrite/ripple).
    void applyDrop(int track, int64_t start, int64_t duration, const std::vector<int>& excludeIds);
    // First track suitable for `audio`-kind media: the first track already of that
    // kind, else the first empty track, else the last track.
    int firstTrackForKind(bool audio) const;
    void syncShotsToClips(); // drag each shot's timelineStart onto its clip (cut in/out left intact)
    bool overTrackArea(float x, float y) const; // cursor is within the timeline track rows
    void dropTargetAt(float x, float y, int& track, int64_t& start) const;
    void resolveDropTarget(float x, float y, int& track, int64_t& start) const; // cursor over timeline, else playhead
    void updateFileHover(float x, float y);
    // A text/URL drop rather than a file one (SDL_EVENT_DROP_TEXT): hand the
    // payload to Python, which knows what a dragged web-app row refers to, and
    // bring in whatever media it resolves to. See App_Timeline.cpp.
    void onDropText(const std::string& text, float x, float y);
    void handleEvent(SDL_Event& e);
    void onKeyDown(const SDL_KeyboardEvent& k);
    void onKeyUp(const SDL_KeyboardEvent& k);
    void update();
    void submitCacheRequests();
    // Guard against re-deriving an unchanged prefetch window every frame: a
    // signature over everything the wanted set is built from, plus when it was last
    // published (see kPrefetchRefreshMs).
    uint64_t cacheReqSig_ = 0;
    double   cacheReqAtMs_ = 0.0;
    // Cache-strip index: resident source frames per media, sorted. Rebuilt on a
    // timer rather than per frame -- see renderTimeline.
    std::map<std::string, std::vector<int64_t>> cacheStripIndex_;
    double cacheStripAtMs_ = -1.0;
    void computeLayout();
    void render();
    void loadAppIcon();   // build iconTex_ from the embedded executable icon
    void drawAppIcon();   // draw iconTex_ in the title bar, left of the menu
    SDL_FRect appIconRect() const;               // the icon's box in the title bar
    void openAppIconMenu();                      // window menu, dropped under the icon
    bool appIconHandleEvent(const SDL_Event& e); // icon clicks + its popup; true if consumed
    void renderPlayer();
    // ── Pixel inspector (App_PixelInspector.cpp) ─────────────────────────
    void samplePixelInspector(); // probe_ <- the cursor and this frame's pipeline
    void renderPixelInspector(); // draw the overlay from probe_
    // ── Letterbox matte (App_Letterbox.cpp) ──────────────────────────────
    bool letterboxActive() const { return timeline_.letterboxRatio > 0.0; }
    std::string letterboxLabel() const;           // active ratio, named ("2.39:1", "off")
    void renderTopBar();                          // draw the top toolbar + its buttons
    void renderTopBarTooltips();                  // hover labels under the toolbar buttons
    void closeTopBarPopups();                     // dismiss all top-toolbar popups (OCIO/sequence/letterbox)
    bool anyTopBarPopupOpen() const {             // is any top-toolbar popup currently showing?
        return sequenceMenuOpen_ || ocioDisplayMenuOpen_ || ocioViewMenuOpen_ ||
               ocioLookMenuOpen_ || ocioInputCsMenuOpen_ || letterboxMenuOpen_ ||
               proxyMenuOpen_;
    }
    bool anyContextMenuOpen() const {             // is any cursor-anchored popup currently showing?
        return clipMenu_.isOpen() || clipToolboxMenu_.isOpen() || fitMenu_.isOpen() ||
               peSortMenu_.isOpen() ||
               peMediaMenu_.isOpen() || peSeqColorMenu_.isOpen() || appIconMenu_.isOpen();
    }
    void openOcioDisplayMenu();                   // toggle the display-space popup
    bool ocioDisplayMenuHandleEvent(const SDL_Event& e); // popup events; true if consumed
    void renderOcioDisplayMenu();                 // draw the display-space popup (overlay, top-most)
    void openOcioViewMenu();                      // toggle the view-transform popup
    bool ocioViewMenuHandleEvent(const SDL_Event& e);    // popup events; true if consumed
    void renderOcioViewMenu();                    // draw the view-transform popup (overlay, top-most)
    void openOcioLookMenu();                      // toggle the look popup
    bool ocioLookMenuHandleEvent(const SDL_Event& e);    // popup events; true if consumed
    void renderOcioLookMenu();                    // draw the look popup (overlay, top-most)
    void openOcioInputCsMenu();                   // toggle the file-colorspace popup
    bool ocioInputCsMenuHandleEvent(const SDL_Event& e); // popup events; true if consumed
    void renderOcioInputCsMenu();                 // draw the file-colorspace popup (overlay, top-most)
    // Shared scrollable list-popup impl backing the three OCIO toolbar buttons.
    void renderOcioListMenu(const char* header, const SDL_FRect& btn,
                            const std::vector<std::string>& items,
                            const std::string& active, float& scroll,
                            int hoverRow, std::vector<SDL_FRect>& outRows,
                            SDL_FRect& outPanel);
    bool ocioListMenuHandleEvent(const SDL_Event& e, const SDL_FRect& btn,
                                 const std::vector<std::string>& items,
                                 const SDL_FRect& panel,
                                 const std::vector<SDL_FRect>& rows,
                                 float& scroll, int& hoverRow, bool& open,
                                 const std::function<void(const std::string&)>& onPick);
    // ── Proxy popup (App_ProxyMenu.cpp) ──────────────────────────────────
    // The button's label: the selected mode, plus — when the clip under the
    // playhead is not actually on it — the representation it is really showing,
    // "Proxy (Full)". Not const: it classifies through Python and memoises.
    std::string proxyModeLabel();
    std::string proxyLabelFor(const std::string& value) const; // display label for a mode value
    // Which mode `path` already is, per the naming config (jplayProxyPathMode).
    // False means "no answer" — unknown file, no callback, interpreter not up —
    // and the caller then says nothing rather than guessing. Memoised per path,
    // which needs no invalidation: it is a property of the file, not of the mode.
    bool proxyModeOfPath(const std::string& path, std::string& outMode);
    std::unordered_map<std::string, std::optional<std::string>> proxyPathModes_;
    void buildProxyModes();                       // lazily populate proxyModes_ (first open only)
    void openProxyMenu();                         // toggle the proxy-mode popup
    bool proxyMenuHandleEvent(const SDL_Event& e); // popup events; true if consumed
    void renderProxyMenu();                       // draw the proxy-mode popup (overlay, top-most)
    // Pushes (proxyEnabled_ ? timeline_.proxyMode : "") into jplay::setProxyMode and
    // does the live-switch housekeeping (cache flush, thumbnail regen, re-render).
    // Called on a dropdown pick, a project load, and the Enable Proxy toggle.
    void applyProxyMode();
    // The mode half of applyProxyMode() on its own: pushes the effective mode and
    // its slate-frame count into ProxyMode.h without touching the caches. For the
    // project-load paths, which clear those themselves.
    void pushProxyMode();
    // Starts a project that carries no proxy selection of its own on the mode the
    // naming config marks as the default one. Called from the two project-open
    // paths, ahead of their pushProxyMode().
    void adoptDefaultProxyMode();
    // Its deferred half, run from the run loop once the interpreter is up (the
    // command-line load path asks before it is). See App_ProxyMenu.cpp.
    void applyPendingProxyDefault();
    bool pendingProxyDefault_ = false;
    void openSequenceMenu();                      // toggle the sequence-view popup
    bool sequenceMenuHandleEvent(const SDL_Event& e);    // popup events; true if consumed
    void renderSequenceMenu();                    // draw the sequence-view popup (overlay, top-most)
    void openLetterboxMenu();                     // toggle the aspect-ratio/opacity popup
    bool letterboxMenuHandleEvent(const SDL_Event& e); // popup events; true if consumed
    void renderLetterboxMenu();                   // draw the popup (overlay, top-most)
    void setLetterboxRatio(double ratio);         // apply a target ratio (0 = off) + refresh sinks
    // Fill up to two matte-bar rects for content rect c at the active target ratio.
    // Returns the count (0 when inactive or the content already matches the ratio).
    int  letterboxBars(const SDL_FRect& c, SDL_FRect out[2]) const;
    void drawLetterbox(SDL_Renderer* r, const SDL_FRect& content) const; // overlay bars
    // Program pixels to submit to the external output: the clean image when no
    // matte is active, else a native-res copy with the bars baked in (via
    // letterboxOutBuf_). Returned buffer is valid until the next call.
    const uint8_t* letterboxForOutput(const uint8_t* px, int w, int h);
    const Imath::half* letterboxForOutput(const Imath::half* px, int w, int h);
    void renderTimeline();
    void renderSidePanel();        // 64px icon strip + directory toggle button
    void renderIconStripTooltips();// hover labels for the icon strip; drawn after panels
    void renderProjectExplorer();  // sequences/shots tree + source bin (only when open)
    // Presses inside the open pane: the +/- buttons, the MEDIA sub-panel's close
    // box, source-row select / double-click / right-click, and the click on empty
    // bin space that drops the selection. Returns true if it took the event.
    bool projectExplorerHandleEvent(const SDL_Event& e);
    void refreshExplorerOrder();   // re-sort each sequence's shots into timeline order
    // Reorder a shot within its sequence: move it to insertIdx in the shot list,
    // then re-lay the sequence's shots (and their clips) contiguously in the new
    // order. Returns true if the order actually changed.
    bool reorderShotInSequence(int seqId, int shotId, int insertIdx);
    void drawPeButton(const SDL_FRect& b, bool plus, bool hover); // small +/- section button
    void renderInspector();        // inspector overlay, centred on the frame (only when open)
    void renderSourceInfoPanel(const SDL_FRect& area); // selected-source info, bottom of SOURCES tab
    Media* inspectedMedia() const;                     // media at inspectMediaPath_, or null
    // SOURCES thumbnail view. Defined in App_ProjectExplorer.cpp.
    void syncSourceThumbs(const std::vector<Media*>& cells); // (re)start generation for the drawn cells
    void freeSourceThumbs();                       // stop the worker and destroy loaded textures
    SDL_Texture* sourceThumb(const std::string& key); // lazy-load one source thumbnail
    std::string sourceThumbKey(const Media* m) const;  // stable per-source thumbnail filename stem
    void writePrefs();                             // persist all UserData prefs from current members

    // Color grading panel. Defined in App_Grade.cpp.
    void renderGradePanel();              // panel background, top bar, active tool
    bool gradeHandleEvent(const SDL_Event& e); // returns true if the event was consumed
    void computeGradeHistogram();         // (re)build gradeHisto_* for the displayed frame
    void markGradeDirty() { hasTexture_ = false; } // force the player texture to rebuild + regrade
    // ── Player exposure / gamma (no panel needed) ─────────────────────────
    // Drive grade_.gain (f-stops) and grade_.gamma from the player: `stops` steps
    // the exposure, scrub() runs the E+drag virtual slider off its total travel,
    // toggleBypass() flips the exposure to 0 and back for an A/B. All three clamp
    // to the Color panel's slider ranges, mark the grade dirty, and flash the
    // value over the frame.
    void exposureNudge(float stops);
    void exposureScrub(float dx, float dy);
    void exposureToggleBypass();
    void exposureStatus();     // flash the current exposure/gamma over the frame
    // Tool renderers: cut their rows off `body`, which is taller than the
    // viewport because the content scrolls. The caller measures the y advance to
    // get the content height for scroll clamping.
    void renderGradeBasic(SDL_FRect& body);
    void renderGradeCurves(SDL_FRect& body);
    void renderGradeWheels(SDL_FRect& body);

    // Tech-check panel. Defined in App_TechCheck.cpp.
    void renderTechPanel();               // panel background + three mode pills
    bool techHandleEvent(const SDL_Event& e); // returns true if the event was consumed
    bool techNitHandleEvent(const SDL_Event& e);   // HDR nit-reference editable field events
    void renderTechOverlay();             // luminance / clipping legend on the stage
    void setTechMode(TechMode m);         // toggle/switch the active mode, force re-render
    // Clip Source panel. Defined in App_ClipSource.cpp.
    void renderClipSourcePanel();    // panel background + the stacked picker sections
    bool clipSourceHandleEvent(const SDL_Event& e); // option-row hit-test; true if consumed
    // Promote an armed commit-row press into the media-bin drag (ghost card, the
    // frame's drop chooser, drop onto the tracks), carrying the pick's resolved
    // media. False — and the arm dropped — when the pick resolves to nothing.
    bool beginClipSourceDrag();
    // Ctrl/Shift on a commit row: mark it for the next drag instead of replacing.
    void markClipSourceOption(const PickerColumn& p, int opt, bool ctrl, bool shift);
    // The armed press turned out to be a plain click: perform the media swap.
    void commitClipSourcePick();
    // Arrow-key move through the commit (version) section: picks the option `dir`
    // places from the current one and swaps to it, exactly as clicking that row does.
    void stepClipSourceVersion(int dir);
    // Aligned drop: a Clip Source drag hovering the free video row directly
    // under its own clip, within that clip's span. Reports the clip above (the
    // one to line up with), else nullptr for the ordinary cursor-following drop.
    const Clip* pickerAlignDropClip(float x, float y) const;
    // Source range that puts `dst` frame-for-frame under `ref`, matched on absolute
    // (editorial) frame numbers so two versions numbered differently still line up.
    // srcIn/srcDur are in dst's own indexing; startShift is the delay to add to the
    // clip's timeline start when dst begins after the reference's in point. False =
    // no overlap, so place the whole source. Needs dst open (see ensureMedia).
    bool alignedSourceRange(const Clip& ref, Media& dst,
                            int64_t& srcIn, int64_t& srcDur, int64_t& startShift);
    // The clip the panel describes and commits to: the primary timeline selection
    // when there is one, otherwise the clip under the frame indicator. Render-path
    // only (it can go through playheadClip()'s per-frame memo).
    const Clip* clipSourceTarget() const;
    // The clips the panel's cascade covers: the whole timeline selection whenever it
    // holds more than one clip — the panel then behaves like the right-click menu on
    // a multi-selection, stopping at the config's multi_select_picker and applying a
    // pick to every clip — otherwise just clipSourceTarget(). Clips with no media are
    // dropped: they have no path to describe and nothing to swap. The first entry is
    // the representative the describe runs against.
    std::vector<int> clipSourceTargetIds() const;
    // Re-describe the pickers for the target clip when a refresh is due (panel just
    // opened, or a different clip / media became the target). Not while the playhead
    // drives the panel and playback is running: crossing clip boundaries would fire a
    // Python query per boundary. Per frame.
    void updateClipSourceData();
    // Display index of the first configured picker whose value differs between two
    // medias, or metaPickers_.size() when every one matches. Compares the cached
    // path-derived metadata (Media::meta), so it costs no Python — it is what lets a
    // target change narrow, or skip outright, the re-describe.
    int pickerDivergenceIndex(const Media& a, const Media& b) const;
    // Close the panel: drop the resolved state and cancel any in-flight query so a
    // late completion can't touch the next clip's state. Safe when already closed.
    void closeClipSource();

    void renderSettingsPanel();           // panel background + Project / JPLAY sections
    bool settingsHandleEvent(const SDL_Event& e); // Time Format toggle hit-test
    bool settingsFpsHandleEvent(const SDL_Event& e); // FPS editable combo events
    bool settingsUiScaleHandleEvent(const SDL_Event& e); // UI Scale picker events
    bool settingsCacheHandleEvent(const SDL_Event& e); // cache-size / decode-threads combo events
    void buildSettingsBar();              // (stub; FPS field needs no setup)
    // Shared slider widget: cuts its block off `body`, draws it and registers a
    // hit region in gradeSliders_.
    void gradeSlider(SDL_FRect& body, const char* label, float* value,
                     float lo, float hi, float def, int grad = 0);

    // Clip thumbnail cache + player grid view. Defined in App_Overview.cpp.
    const Sequence* sequenceOfClip(int clipId) const; // first sequence whose members include clipId
    std::string clipThumbKey(const Clip& c) const;     // stable hash(sequence name + shot name)
    SDL_Texture* overviewThumb(const std::string& key); // lazy-load <key>.jpg into a texture (null if absent)
    void freeOverviewTextures();
    void startClipThumbnails();     // (re)start background thumbnail generation for the drawn tiles
    void syncGridThumbnails();      // restart generation when the drawn tile set has changed and settled
    // Switch the player stage, running the leave/enter side effects of both ends
    // (the Overview's thumbnail workers, the Layout's slot textures). The single
    // way stage_ is written, so leaving a stage cleans up however it was left.
    void setPlayerStage(PlayerStage s);
    // Fullscreen the window and hide every band of chrome, or put it all back.
    void setCinemaMode(bool on);
    // Collapse the timeline to its ruler + cache strip, or restore the full stack.
    // Collapsing normally takes the docked pane with it (compact is a request for
    // the image); the Layout stage passes closePanes=false, since entering a stage
    // is not that request. Expanding is refused while the Layout is up.
    void setCompactTimeline(bool on, bool closePanes = true, bool persist = true);
    // Leaving the Layout / Stack stage: undo the compact the stage forced, unless
    // the user was already compact on the way in. A no-op from inside a stage.
    void restoreCompactAfterCompare();
    // Overview grid (player stage): F3, or the View row, via setPlayerStage.
    void renderGridView(const Clip* liveClip); // draw the clip grid into the player area
    void gridClickClip(const Clip& c, bool doubleClick, bool shiftHeld); // tile click: playhead / range
    void gridSetThumbH(float h); // ctrl+wheel over the grid: resize the tiles (clamped, persisted)
    // Lazy-load <key>.jpg from `cache` into `texMap` (caching even a null-on-failure
    // to avoid re-hitting disk each frame); returns null without caching while the
    // worker hasn't produced the file yet. Backs overviewThumb and sourceThumb.
    SDL_Texture* loadThumbTexture(ThumbnailCache& cache,
                                  std::unordered_map<std::string, SDL_Texture*>& texMap,
                                  const std::string& key);

    // The highest-precision buffer a decoded frame carries, and the OcioGpu format
    // describing it. Defined in App_Player.cpp; shared with App_Layout.cpp, whose
    // tiles go through the same display transform.
    static void frameInput(const Frame& f, const void*& pixels, OcioGpu::InputFormat& fmt);

    // Layout stage (player stage): L toggles single frame <-> tiled tracks.
    // Defined in App_Layout.cpp.
    void toggleLayoutView();
    // Stand up the Layout stage's scratch sequence out of the clips being compared:
    // the timeline selection, or the clip under the playhead when nothing is
    // selected, one per video track and aligned on absolute source frames. False
    // (with a status set) when there is nothing to lay out, which is the caller's
    // signal not to enter the stage.
    bool openLayoutView();
    // What the Layout stage tiles show at `frame`: one entry per video row, top row
    // first, null where that row has no clip there — so the entry count is the tile
    // count and does not move with the playhead. Also the set submitCacheRequests
    // has to keep resident, which is why it is a plain query rather than
    // render-time state.
    void layoutClipsAt(int64_t frame, std::vector<const Clip*>& out) const;
    // The clip a Layout row belongs to, ignoring the playhead: what an empty tile
    // is still labelled with.
    const Clip* layoutRowClip(int track) const;
    // Lay `n` tiles of display aspect `ar` out over the player area, filling
    // layoutTiles_. Auto-packed: of every rows x cols that holds n, the one whose
    // cell fits the largest image wins.
    void buildLayoutTiles(int n, float ar);
    // Build and draw every tile but the program's (renderPlayer draws that one).
    // `invalidate` re-renders every tile even where its frame has not changed — the
    // program's own "rendering inputs changed" signal, which covers the grade, the
    // tech mode and the display transform the tiles share with it.
    void renderLayoutTiles(bool invalidate);
    // A tile's label bar + border. Called for the comparison tiles by
    // renderLayoutTiles and for the program tile by renderPlayer, after its image.
    // `c` may be null: the row's own clip names the tile then.
    void drawLayoutTileChrome(const SDL_FRect& cell, const Clip* c, int track);
    // Destroy the slot textures. Called before the renderer goes away.
    void freeLayoutSlots();
    // Tile index under (x,y), or -1. Anchors the zoom and answers tile clicks.
    int layoutTileAt(float x, float y) const;

    // Stack stage (player stage): F5. The Layout's comparison sequence shown one
    // image at a time — the top-most clip, which is what the Frame stage already
    // draws, so the stage adds no render path of its own. Defined in App_Layout.cpp
    // alongside the sequence the two share.
    //
    // Rotate which clip is on top by `dir` rows (+1 = the one below becomes the
    // program, -1 = the one above), and arm the name overlay.
    void cycleStack(int dir);
    // The stack from the top down: one entry per video row, the clip that owns it,
    // null for a row with none. layoutRowClip for every row at once, so the order
    // is the rotation order rather than what reaches the playhead.
    void stackRowClips(std::vector<const Clip*>& out) const;
    // The name overlay a cycle arms: the program's basename over the two under it,
    // centred and fading out. A no-op once stackOverlayUntil_ has passed.
    void renderStackOverlay();

    // ProjectExplorer (left media bin)
    // One section of the bin. PeSort::Name yields a single headerless group
    // holding the whole list, so the bin renderer only has to know about groups.
    struct BinGroup {
        std::string header;           // section title; empty = draw no header row
        std::vector<Media*> sources;
    };
    std::vector<Media*> sortedSources() const;  // pool entries, alphabetical by basename
    std::vector<Media*> binSources() const;     // sortedSources() narrowed by the Filter: row
    std::vector<BinGroup> binGroups() const;    // binSources() split into the sections to draw
    void openPeSortMenu();                      // sort button: drop the order menu below it
    bool sourceInTimeline(const Media* m) const; // any clip references this source
    // Media ids of the clips currently on screen in the timeline: the in-view
    // sequences' clips overlapping the visible frame range. Backs the
    // peFilterTlVisible_ toggle.
    std::unordered_set<std::string> timelineVisibleMediaIds() const;
    bool sourceSelected(const Media* m) const;   // m's path is in the current selection
    void clickExplorerRow(int index, SDL_Keymod mods); // plain / Ctrl-toggle / Shift-range
    // Arrow-key move through the SOURCES bin: collapses the selection onto the
    // source `dir` places (one item, in either layout) from the current one, in
    // the displayed order, and asks the next render to scroll it into view.
    void stepBinSelection(int dir);
    void pressExplorerRow(int index, SDL_Keymod mods); // arm a possible bin→timeline drag on press
    // Right-click on a source row: select it (unless already selected) and open
    // peMediaMenu_ at the cursor with the copy / reveal / properties actions for
    // its path.
    void openBinContextMenu(int index, float mx, float my);
    // Double-click a source row: scope to "Default Sequence" (creating it if the
    // project has none) and put the source in it, clearing whatever it held
    // (replaceContents=false appends instead, for the 2nd+ item of one gesture).
    void openSourceInDefaultSequence(const std::string& path, bool replaceContents = true);
    void updateBinDrag(float x, float y);              // live preview while dragging a source onto the tracks
    void commitBinDrag(float x, float y);              // drop the dragged sources onto the timeline
    void renderBinDragGhost();                         // translucent dragged-item card under the cursor
    void addMediaViaBrowser();      // open file dialog, then add like a frame-view drop
    // Remove the selected sources and every clip on them. Nothing ripples, so the
    // frames those clips held are left empty. A source that is used in the timeline
    // is always confirmed; alwaysConfirm confirms the unused ones too, for the
    // context menu and the Delete key, where — unlike the minus button — the
    // gesture doesn't say what it is about to do.
    void removeSelectedSource(bool alwaysConfirm = false);
    // Context menu "Replace Source": pin `path`'s pool entry and open the file
    // chooser; the swap happens when the pick comes back (processPendingDialogs).
    void browseReplaceSource(const std::string& path);
    // Point every clip that uses `oldMediaId` at the pool entry for `newPath`,
    // keeping each clip's source in point and duration. A shorter new source
    // shortens the clips that no longer fit (in point pulled back to its last
    // frame when even that is out of range); nothing ripples, so the frames a
    // shortened clip gives up are left as empty space. The old pool entry goes
    // away with it, which is why this clears the undo stack.
    void replaceSourceMedia(const std::string& oldMediaId, const std::string& newPath);
    // Sequences/shots tree (top of the Project Explorer).
    bool projectTreeHandleEvent(const SDL_Event& e); // returns true if consumed
    void addSequence();             // create a new empty sequence, make it active
    // Color button on a sequence row: drop the 8x8 swatch grid below it.
    void openSeqColorMenu(int seqId, const SDL_FRect& btn);
    void removeActiveSequence();    // delete the active sequence (and its clips), confirm if non-empty
    void removeSequenceAt(int idx); // the unconditional delete, past the confirmation
    void scopeToSequence(int seqIdx); // set active + drive the View dropdown to this sequence
    void scopeToAll();                // drive the View dropdown back to "All"
    void scopeToShot(int shotIdx);    // fit the view to one shot (within the focused sequence)
    void unscopeShot();               // clear shot focus, re-fit to the focused sequence
    void beginPeEdit(PeEdit kind, int id, const SDL_FRect& rect, const std::string& initial);
    void commitPeEdit();            // apply the focused inline edit, then clear it
    void cancelPeEdit() { peEdit_ = PeEdit::None; peEditId_ = -1; SDL_StopTextInput(window_); }
    int64_t seqOffsetForShot(int shotId) const; // owning sequence's display offset (0 if none)
    void updateFrameOverlay();  // rebuild overlayFile_/overlayTime_ for this frame
    // Point size the burn-in is rasterized at, from the Size choice.
    float overlayFontPt() const { return kOverlaySizes[overlaySize_].pt; }
    void reloadOverlayFont();       // (re)open overlayFont_ at that size (main renderer)
    void reloadReviewOverlayFont(); // the same for the review window, if one is open
    // Draw the burn-in along one edge of the program image rect, with a font that
    // must belong to the same renderer: TextFont caches its glyph textures, and a
    // texture is owned by the renderer that made it. No-op unless frameOverlay_ is on.
    void drawFrameOverlay(SDL_Renderer* r, const TextFont& font,
                          const SDL_FRect& content) const;
    void updateInfoOverlay(); // rebuild infoFields_ when the playhead's clip changes
    void renderInfoOverlay();
    void drawText(float x, float y, SDL_Color c, const std::string& s);
    // Intrinsic width of an icon+label toolbar button of height h: a square icon
    // of that height, the label, and horizontal padding. `caret` adds room for
    // the down-caret that marks the button as a picker. The single source of this
    // metric — the info bar, the top bar and their draw code all size from it.
    float iconTextBtnW(const std::string& label, float h, bool caret = false) const;
    // One top-toolbar button: neutral fill + border, square icon well at the left
    // edge, label after it, optional picker caret at the right. `active` is the
    // Letterbox button's "matte on" blue tint; every other button passes false.
    // Sized by iconTextBtnW above, so the two stay in step.
    void drawTopBarBtn(const SDL_FRect& box, uint32_t icon, const std::string& label,
                       bool hovered, bool active, bool caret);
    // Longest prefix of s whose rendered width fits within maxW pixels (using
    // the actual proportional-font metrics, not a fixed per-char estimate).
    std::string fitText(const std::string& s, float maxW);
    // s broken at spaces into lines that each fit within maxW. A single word
    // wider than maxW is left over-long on its own line rather than split.
    std::vector<std::string> wrapText(const std::string& s, float maxW);

    double frameToX(double frame) const;
    double xToFrame(double x) const;
    void fitView();
    void fitRange(int64_t a, int64_t b);
    // Fit the current scope: the scoped sequence/project span, or the whole
    // concatenated timeline in the All view. fitScopeFpp() is the framesPerPx that
    // fit lands on, i.e. the most zoomed-out the view can usefully get.
    void fitScope();
    double fitScopeFpp() const;
    double fitRangeFpp(int64_t a, int64_t b) const;
    // True when the view already sits exactly where fitRange(a, b) would put it
    // (to within half a pixel). This is what disables a fit button: a fit that
    // would leave the view where it already is has nothing to do.
    bool viewFitsRange(int64_t a, int64_t b) const;
    bool playheadSeqRange(int64_t& a, int64_t& b, int& seqIdx) const;
    // Rebuild fitBtns_ for the current view + playhead (once per computeLayout),
    // draw the group in the info bar, and apply one when clicked.
    void layoutFitButtons();
    void drawFitButtons();
    void openFitMenu();
    // Fed every mouse-motion event before fitMenu_ sees it: opens the popup when
    // the cursor enters the button and drops it when the cursor leaves both.
    void fitHoverEvent(const SDL_Event& e);
    void applyFitButton(const FitBtn& b);
    void zoomAt(float mouseX, double factor);
    // As zoomAt, but anchored on a frame held at `screenFrac` (0..1) across the
    // content width instead of on a timeline x. The Overview grid zooms this way:
    // its tiles have no timeline x to anchor on, so it names the frame to keep.
    void zoomAtFrame(double anchorFrame, double screenFrac, double factor);
    // The program texture's width in display pixels: texW_ stretched by its pixel
    // aspect. Every fit-to-rect pairs this with texH_ so non-square-pixel media
    // lands in the right shape, and it is the space annotation strokes are stored
    // in (identical to texW_ for the square-pixel case, which is everything but an
    // anamorphic EXR).
    float texDispW() const { return (float)texW_ * texPa_; }

    // Player frame zoom/pan (independent of the timeline view above).
    SDL_FRect frameDstRect() const;                       // current on-screen image rect
    // The area the program image is fitted into: playerRect_ normally, tile 0's
    // cell on the Layout stage.
    SDL_FRect programViewRect() const;
    // The rect the *program* image is drawn into. Everything glued to the program
    // image — the annotations, the letterbox — goes through this so it follows the
    // program into its tile.
    SDL_FRect programDstRect() const;
    // Fit dispW x h into `view`, aspect-preserving, then apply the frame zoom/pan.
    // The shared core of frameDstRect and the Layout tiles: passing each tile's
    // cell through this is what makes the zoom synced across them — one
    // frameZoom_/framePan for every tile, and the cells are all the same size.
    SDL_FRect fitImageIn(const SDL_FRect& view, float dispW, float h) const;
    // The scale dispW x h lands at inside `view` at frameZoom_ == 1 — fitting the
    // whole image normally, the letterbox-masked region when Fit View is on.
    float fitScaleIn(const SDL_FRect& view, float dispW, float h) const;
    // The area the frame zoom/pan is anchored in: playerRect_, or the tile under
    // the cursor on the Layout stage (so zooming toward a point in tile 3 pins that
    // point in tile 3 rather than in a cell the cursor is nowhere near).
    SDL_FRect frameViewRect(float cx, float cy) const;
    void fitFrameView();                                  // reset zoom/pan to fit-to-window
    void zoomFrameAt(float cx, float cy, float factor);   // zoom toward (cx,cy)
    void zoomFramePixelScale(float cx, float cy, int mult); // mult device pixels per image pixel
    // Sync-review view transfer: encode zoom/pan as a zoom factor + the normalized
    // image point at the player center, and reconstruct pan from that on a peer.
    void frameViewCenter(float& zoom, float& u, float& v) const;
    void applyFrameView(float zoom, float u, float v);
    bool inPlayerView(float x, float y) const;            // (x,y) over the frame (not panels)
    // Drop-action chooser drawn over the frame while media is dragged onto it.
    // Shared by the bin drag and the OS file drag; see App_Player.cpp.
    // The actions on offer, in box order; returns how many. Box index and action id
    // are separate: the ids are what applyPlayerDrop switches on, so a stage can
    // offer a subset of them without renumbering.
    // Boxes are laid out in rows, one per box except the pair that shares one.
    static constexpr int kPlayerDropBoxes = 6;
    int  playerDropActions(int out[kPlayerDropBoxes]) const;
    int  playerDropBoxRects(SDL_FRect out[kPlayerDropBoxes]) const; // lay the boxes out; 0 if it doesn't apply
    bool playerDropChooserApplies() const;            // false on an empty timeline / too small a frame
    bool playerDropBoxEnabled(int action) const;      // Replace needs a clip under the indicator
    int  playerDropBoxAt(float x, float y) const;     // action under (x,y), else -1
    void renderPlayerDropBoxes();
    // Route one dragged source to the chosen action; `first` marks the leading item
    // of a multi-item drag.
    void applyPlayerDrop(int action, const std::string& path, bool first);
    // "Add Clip to Track": place `path` at the start frame of the clip under the
    // indicator, on the first row (top down) whose span there is free.
    bool addMediaAlignedToCurrentClip(const std::string& path);
    void renderAnnotations();                             // draw pencil strokes over the frame
    void annotationGeometry(const SDL_FRect& dst, std::vector<SDL_Vertex>& verts,
                            std::vector<int>& idx) const; // stroke geometry for a dst rect
    void renderDrawPanel();                               // draw-tool column, left of the frame (pencil mode)
    bool drawPanelHandlePress(float mx, float my);        // Clear/size/hue press; true if consumed
    void pencilColorRGB(float& r, float& g, float& b) const; // current stroke color, 0..1
    void annotBeginStroke(float sx, float sy);            // start a stroke at a screen point
    void annotAppendPoint(float sx, float sy);            // extend the in-progress stroke
    void syncAnnotBuffer();                               // flush/load the live buffer as the frame changes
    void flushAnnotBuffer();                              // persist the live buffer into its owning clip
    // Clip shown at an absolute timeline frame + the media source frame it maps
    // to; null if nothing is under `frame`. Used to key annotations by source frame.
    Clip* clipAtSourceFrame(int64_t frame, int64_t& srcFrame);

    // Review-monitor window (second display); see App_Review.cpp.
    bool reviewActive() const { return reviewWindow_ != nullptr; }
    void syncReviewWindow();                              // reconcile with reviewEnabled_/reviewDisplay_
    void openReviewWindow(SDL_DisplayID display);         // create the window on `display`
    void closeReviewWindow();                             // tear down window/renderer/texture
    void feedReviewFrame(const uint8_t* rgba, int w, int h); // upload one program frame (SDR RGBA8)
    // Upload one scene-linear program frame (RGBA half) to the HDR review monitor's
    // own device; renderReviewWindow applies the display transform there.
    void feedReviewSource_(const Imath::half* rgba, int w, int h);
    bool ensureHdrProgramTex_(int w, int h);  // (re)create the offscreen HDR program target
    // Read the current render target (hdrProgramTex_) back to CPU: `sdr8` fills
    // hdrReadback_ (RGBA8, tonemapped) for an SDR sink, `half` fills hdrReadbackHalf_
    // (RGBA half scRGB, untouched) for an HDR one. Both may be wanted at once. Returns
    // false if the read failed.
    bool readbackHdrProgram_(int w, int h, bool sdr8, bool half);
    // Does the offscreen HDR program target currently hold a genuine HDR rendering —
    // scene-linear source through a PQ or HLG display transform, so scRGB values above
    // paper white carry real highlight detail — and if so, which transfer should the
    // sink re-encode with? None means everything else (video, OCIO off, the nit heatmap,
    // an SDR display transform, or an HDR review monitor having taken the reference and
    // left the main pass on the SDR companion), all served correctly by the 8-bit path.
    OutputTransfer hdrProgramTransfer_() const;
    SDL_Renderer* createGpuRenderer_(unsigned colorspace); // create the "gpu" renderer + gpuDevice_ with the given output colorspace
    void rebuildRenderer_(bool toHdr); // recreate the main renderer as HDR (scRGB) or SDR when the window's display changes
    // Is an external video sink running or requested? While one is, it owns HDR and
    // the main window presents sRGB. NDI and the review monitor are mutually exclusive.
    bool externalSinkActive() const { return output_.active() || reviewEnabled_ || reviewActive(); }
    // Is an HDR review monitor presenting, making it the reference display? Its OCIO
    // display (Rec.2100-PQ, ...) then belongs to it alone; the main window is on an SDR
    // swapchain where those code values are wrong, so it renders through the config's
    // SDR companion display instead. See OcioManager::sdrProcessor().
    bool mainForcedSdrOcio() const { return reviewActive() && reviewHdr_; }
    void syncHdrSink_(); // reconcile the main window's swapchain with externalSinkActive()
    // Dump what SDL believes about every display's HDR state and which swapchain
    // compositions `w`/`dev` can actually present. The main-window and review-monitor
    // HDR decisions hinge on these two signals and they can disagree with each other
    // (and with the Windows HDR toggle), so log both wherever we choose a swapchain.
    void logHdrDisplayState_(const char* when, SDL_Window* w, SDL_GPUDevice* dev);
    // Should `w` be given an scRGB (HDR) swapchain on the display it currently sits on?
    // Deliberately NOT gated on SDL_PROP_RENDERER_HDR_ENABLED_BOOLEAN — see App.cpp.
    bool windowCanPresentHdr_(SDL_Window* w, SDL_GPUDevice* dev) const;
    void renderReviewWindow();                            // clear + frame + annotations + present
    SDL_FRect reviewDstRect() const;                      // mirrored dst rect in review-window space
    void setPlayhead(int64_t f);
    int64_t scrubFrame(double x) const; // x -> frame, snapping to clip starts when snapPlayhead_
    void followPlayhead();

    // Transport
    void togglePlay();              // flip play/pause (space bar + info-bar button)
    // dir<0: previous clip's first frame; dir>0: next clip's first frame. With
    // markRange the playback range follows the jump (the clip landed on becomes
    // the range), which is what the PgUp/PgDn bindings do.
    void jumpClip(int dir, bool markRange = false);
    void markClipRange(const Clip& c);  // playback range = this clip; re-anchors the expansion
    void expandClipRange(int dir);      // dir>0 / dir<0: one clip more / less on each side of the anchor
    void drawTransportButton(const SDL_FRect& r, bool hovered); // info-bar button background
    void fillTriangle(float ax, float ay, float bx, float by, float cx, float cy, SDL_Color c);

    // Tracks. One stack of rows stacked downward from tracksTop_. Each track's
    // type is derived from its clips (video full height, audio half height); the
    // single trailing empty placeholder row is drawn at half a video row's height
    // and adopts the type of whatever first lands on it.
    // A scratch view stands up its own rows: the project's tracks are all empty
    // here (their clips belong to sequences the view is scoped out of, and the
    // track helpers are scoped to match — see Timeline::trackScopeSeqId), and an
    // empty row per project track is nothing to look at. A source view is one clip,
    // so one row; a Layout view is one row per clip it lays out, plus the trailing
    // spare ensureTrailingEmptyTrack keeps. timeline_.trackCount is left alone
    // throughout — the project's real stack is untouched and comes straight back
    // when the view is dropped, and it is a saved field, so growing it for a view
    // would write the view into the file and the dirty signature.
    int trackCount() const { return scratchActive() ? scratchTrackCount_ : timeline_.trackCount; }
    // The row count a scratch view owns, so ensureTrailingEmptyTrack and the "every
    // row is taken" append path grow the right one.
    int& mutableTrackCount() { return scratchActive() ? scratchTrackCount_ : timeline_.trackCount; }
    // Width of the left track-label gutter this frame. Compact mode has no track
    // rows to label, so the gutter collapses and the ruler, cache strip and
    // flattened clip band run to the window's left edge. Everything else derives
    // from headerX_ / contentW_, which computeLayout builds from this.
    float headerW() const;
    float trackRowH(int track) const;   // height of one row (varies by track type)
    float trackRowY(int track) const;   // y of a row's top (sum of the heights above it)
    float tracksTotalH() const;        // height of the whole stack (may exceed the viewport)
    float tracksViewBottom() const { return tracksTop_ + tracksViewH_; }
    float tracksMaxScroll() const { return std::max(0.0f, tracksTotalH() - tracksViewH_); }
    // Cursor is on the timeline's top-edge resize handle (a band straddling it).
    bool overTimelineEdge(float mx, float my) const;
    // The vertical band every clip-sized box on `track` sits in: the track's row
    // inset top and bottom, by less on the half-height audio rows. Shared by the
    // timeline's draw pass and the fade-handle hit test, which has to agree with it
    // on where a clip's top edge is.
    SDL_FRect clipBandRect(int track, bool audio) const;
    SDL_FRect trackHeaderRect(int track) const;
    // Burger button at a row's middle right: opens the row's popup menu (switch
    // the track off/on, remove it).
    SDL_FRect trackMenuRect(int track) const;
    void openTrackMenu(int track);
    void toggleTrackDisabled(int track);
    // Selection becomes exactly the clips on `track` (plus their linked audio, as
    // every other selection gesture does).
    void selectTrackClips(int track);
    // The audio a row sounds: its own clips on an audio row, the audio linked to
    // them on a video row. Empty when the row has none, which is what keeps the
    // menu entry off such a row.
    std::vector<int> trackAudioClipIds(int track) const;
    // Hide/unhide that audio in one go: mute while any of it still sounds, unmute
    // once all of it is silent.
    void toggleTrackAudioMuted(int track);
    // Track row under a y position, clamped into the valid range.
    int trackFromY(float y) const;
    void ensureTrailingEmptyTrack();      // keep exactly one empty trailing row
    void requestRemoveTrack(int track);
    void removeTrack(int track);

    // Label a track row shows: its custom name if it has one, else the derived
    // "Video n" / "Audio n" / "Track n" (numbered per kind, top to bottom).
    std::string trackLabel(int track) const;
    // Inline rename of a track header. The field is committed by Enter or a click
    // outside it, abandoned by Esc; an all-blank name clears the custom label.
    SDL_FRect trackNameFieldRect(int track) const;
    void beginTrackNameEdit(int track);
    void commitTrackNameEdit();
    void cancelTrackNameEdit();
    // Consumes typing/Enter/Esc and the click that ends the edit. False when no
    // rename is open, or the event isn't one it wants.
    bool trackNameEditHandleEvent(const SDL_Event& e);

    // Track reorder (drag a track's label in the left gutter up or down). Rows
    // are pure index space -- a track *is* the set of clips carrying its number --
    // so a reorder is a renumbering of clip.track, nothing more. The trailing
    // spare row is excluded: it neither moves nor takes a drop, so it stays at
    // the bottom where ensureTrailingEmptyTrack wants it.
    int trackReorderRows() const;                 // rows that participate (spare excluded)
    int trackDropBoundaryAt(float y) const;       // nearest insertion boundary to y
    void reorderTrack(int from, int ins);         // lift row `from`, re-insert at boundary `ins`

    // Clip drag/drop
    Clip* clipById(int id);
    Clip* clipAtTrack(int track, int64_t frame);
    // If `frame` on `track` lies in empty space between two clips, fill the gap's
    // [start,end) and return true. Leading/trailing space is not a gap.
    bool gapAtTrack(int track, int64_t frame, int64_t& gapStart, int64_t& gapEnd) const;
    int64_t snapDragStart(double desiredStart, int64_t duration, int excludeId) const;
    void beginClipDrag(Clip& c, float mouseX);
    void updateClipDrag(float mouseX, float mouseY);
    void commitClipDrag(bool duplicate = false);
    // Clip trim (edge resize). clipEdgeAt finds the clip whose left/right edge is
    // under x on a track (edge: 0=left, 1=right), null if none.
    Clip* clipEdgeAt(int track, float x, int& edge);
    // Cursor x -> the frame a trim edge lands on. When snapPlayhead_ is on, the
    // edge snaps to the in/out points of the clips on the row directly above
    // `track` (so an audio handle lands on the cuts of the video it belongs to)
    // and to the playhead. `track` is the dragged clip's own row.
    int64_t snapTrimFrame(double x, int track) const;
    void beginClipTrim(Clip& c, int edge);
    void updateClipTrim(float mouseX);
    void commitClipTrim();
    void deleteSelectedClip();
    void deleteSelectedGap(); // close the selected gap; clips at/after it ripple back across all tracks
    // Transition resize. Mirrors the clip-trim gesture: the dragged edge tracks
    // the cursor live from the *New_ fields, and only the commit writes the model.
    // transitionEdgeAt finds a transition whose left/right box edge is under x
    // (edge: 0=left = inFrames, 1=right = outFrames), null if none.
    Transition* transitionEdgeAt(int track, float x, int& edge);
    void beginTransitionResize(const Transition& t, int edge);
    void updateTransitionResize(float mouseX);
    void commitTransitionResize();
    // Clip copy/paste. Paste drops the clipboard at the playhead, each clip on
    // the topmost track that can take it; trackCount() when none can (caller
    // adds a row).
    void copySelectedClips();
    void pasteClips();
    int firstFreeTrackFor(bool audio, int64_t start, int64_t duration) const;
    // Clip multi-selection helpers (selectedClipIds_ is the set, selectedClipId_ the primary).
    bool isClipSelected(int id) const;
    void selectClipSingle(int id); // selection becomes exactly {id} plus its linked audio
    void toggleClipSelection(int id); // add id if absent, remove if present (with its linked audio)
    void clearClipSelection();     // select nothing
    // Append every linked follower of the clips already in `ids` (transitively,
    // and skipping duplicates). Selection and delete both route through this, so
    // linked audio comes along however the video clip was picked.
    void addLinkedFollowers(std::vector<int>& ids) const;
    // Link the selected audio clips to the one selected video clip / drop the
    // links in the selection. Both are one undo step. The two predicates say
    // whether the clip right-click menu offers each row.
    void linkSelectedClips();
    void unlinkSelectedClips();
    // Propagate a trim of `parentId` (source-offset and duration deltas) onto the
    // audio linked to it, clamped to what that audio source has.
    void trimLinkedFollowers(int parentId, int64_t dSrc, int64_t dDur);
    // Re-derive every follower's start from its parent. Call after an edit that
    // moved clips on one track only (ripple, close-gap, overwrite drop).
    void resyncLinkedClips();
    bool selectionCanLink() const; // one video clip + audio not already following it
    bool selectionHasLink() const; // any live link among the selected clips
    void drawDottedRect(const SDL_FRect& r, SDL_Color c);

    // Snapshot of clip/shot positions so ripple edits (move, close-gap) undo as
    // one step. Restore reassigns track/start; durations and cuts are untouched.
    struct PositionSnapshot {
        struct ClipPos { int id; int track; int64_t start; };
        struct ShotPos { int id; int64_t start; };
        std::vector<ClipPos> clips;
        std::vector<ShotPos> shots;
        // Dissolves, whole (they are few and tiny), one entry per sequence in
        // order. A move or trim can break the adjacency one is anchored to, or eat
        // the handle it spends, so pruneTransitions shortens or drops it — undo has
        // to put it back with the geometry that produced it.
        std::vector<std::vector<Transition>> transitions;
    };
    PositionSnapshot capturePositions() const;
    void restorePositions(const PositionSnapshot& s);

    // The editable geometry of a named set of clips. A trim changes duration and
    // source range — which a PositionSnapshot does not carry — for the trimmed
    // clip *and* every clip linked to it, so its undo pairs the two snapshots.
    struct ClipGeom {
        int id = 0;
        int64_t start = 0, duration = 0, sourceOffset = 0, linkOffset = 0;
    };
    std::vector<ClipGeom> captureClipGeom(const std::vector<int>& ids) const;
    void restoreClipGeom(const std::vector<ClipGeom>& g);

    // Whole clip/shot/track snapshot, for edits that add or remove clips (media
    // drops) or resize them and their neighbours (clip media replace) — things a
    // PositionSnapshot cannot restore because it only records where clips sit.
    struct ContentSnapshot {
        std::vector<Sequence> sequences;
        std::vector<Shot> shots;
        int trackCount = 0;
        std::vector<std::string> trackNames;
        std::vector<uint8_t> disabledTracks;
    };
    ContentSnapshot captureContent() const;
    void restoreContent(const ContentSnapshot& s);
    // Record `before` -> the current state as one undo step labelled `label`.
    void pushContentUndo(const std::string& label, const ContentSnapshot& before);
    // State the curve lane's active drag started from, so the whole gesture lands
    // as one step on release (see App_CurveLane.cpp). Here rather than with the
    // mode's other members because ContentSnapshot is only declared this far down.
    ContentSnapshot curveDragBefore_;

    // Undo/redo. undoStack_ records reversible clip edits (move, delete); it is
    // cleared whenever the timeline is replaced wholesale (new/load/import) or
    // structurally reshuffled (track removal).
    UndoStack undoStack_;
    void undo();
    void redo();

    // Ensure at least one sequence exists; creates "Default Sequence" if not.
    // Must be called before adding any clip. Returns ref to the active sequence.
    Sequence& ensureDefaultSequence();
    Sequence& activeSequence() { return timeline_.sequences[activeSequenceIdx_]; }

    void saveProject();
    // Open a .jpproj. `shared` marks it as opened from the SHARED PROJECTS column:
    // the file is then loaded as an unsaved copy rather than adopted as the current
    // project file (see projectFromShared_).
    void loadProject(const std::string& path, bool shared = false);
    // Import an .otio as a new, unsaved project. `shared` marks it as opened from
    // the SHARED PROJECTS column (see projectFromShared_).
    void loadOtio(const std::string& path, bool shared = false);

    // When the app started with no source (and no $OCIO), OcioManager fell back to
    // the built-in config. Once the first real source arrives (open project, add
    // media, drop), retry init() with a representative path so a preferences-based
    // config can be derived. No-op once a non-built-in config is active.
    void reinitOcioForFirstSource();

    // Missing-source handling (App_Project.cpp).
    void finishLoad();                 // metadata refresh (imports only) + final status
    void propagateRelocateRules();     // after a relocation, silently repoint every sibling under the moved prefix
    void resolveMissingWithRules();    // per-frame: auto-relocate newly-missing clips via learned rules
    void openRelocateModal(const Media& media);  // show the Missing Source modal for `media`
    void closeRelocateModal();
    void handleRelocateModalEvent(const SDL_Event& e); // consume input while the modal is open
    void renderRelocateModal();                  // draw the modal overlay (call last)
    void renderHelpPanel();                      // keyboard-shortcuts overlay, centered (call last)
    void renderAboutPanel();                     // version / build-info overlay, centered (call last)

    // Generic modal progress task (App_Project.cpp). beginProgress runs `work` on
    // a background thread with a shared ProgressReporter, showing a modal dialog
    // until it finishes, then runs `onDone` on the main thread. No-op if a task is
    // already running.
    void beginProgress(const std::string& title,
                       std::function<void(ProgressReporter&)> work,
                       std::function<void()> onDone);
    void pollProgress();                          // main loop: finalize a finished worker
    void handleProgressEvent(const SDL_Event& e); // consume input while the dialog is up
    void renderProgressOverlay();                 // draw the modal overlay (call last)

    // Modal message dialog (App_Project.cpp). Buttons are drawn left→right in the
    // order given; escIdx/enterIdx name the ones Esc and Enter pick (-1 = neither,
    // which just dismisses). No-op if a dialog is already up.
    void showDialog(std::string title, std::string message,
                    std::vector<DialogButton> buttons, int escIdx, int enterIdx);
    // Add a checkbox to the open dialog's button row; `onCheck` runs on each
    // toggle. Call right after showDialog.
    void setDialogCheckbox(std::string label, bool checked, std::function<void(bool)> onCheck);
    void closeDialog();
    void runDialogButton(int idx);                // dismiss, then run that button's action
    void handleDialogEvent(const SDL_Event& e);   // consume input while it is open
    void renderDialog();                          // draw the modal overlay (call last)
    bool progressActive() const { return progressRunning_.load(std::memory_order_acquire); }
    // The dialog only appears (and becomes modal) once a task has run past this
    // delay, so brief discoveries never flash a dialog.
    static constexpr uint64_t kProgressShowDelayMs = 200;
    bool progressVisible() const {
        return progressActive() && SDL_GetTicks() - progressStartTick_ >= kProgressShowDelayMs;
    }
    void doRelocateFromField();                  // validate the field's dir and apply / show inline error
    bool candidateMatches(const Media& media, const std::string& candidatePath) const;
    bool tryRelocateRules(const Media& media, std::string& out) const;
    void addRuleFromPaths(const std::string& oldPath, const std::string& newPath);
    void relocateMedia(const std::string& mediaId, const std::string& newPath);
    static void SDLCALL onRelocateChosen(void* userdata, const char* const* filelist, int filter);
    void newProject(); // clear timeline + forget any saved-file association
    void closeProject(); // clear timeline and return to the launcher
    void updateWindowTitle();

    // Unsaved-changes guard (App_Project.cpp). "Dirty" is derived, not tracked
    // per-edit: projectSignature() hashes the project serialized exactly as it
    // would be written to disk, and savedSignature_ is stamped whenever the two
    // are known to agree (after a save, a load, or a new project). Nothing has
    // to remember to flag an edit, so no edit path can silently go unflagged.
    // The playhead is excluded — scrubbing and playback are not edits.
    uint64_t projectSignature();
    bool projectDirty() { return projectSignature() != savedSignature_; }
    bool hasProjectContent() const; // any clip placed or any media in the bin
    bool isSingleClipSession() const; // one clip in a lone "Default Sequence": nothing to lose
    // Gate a destructive action behind the unsaved-changes prompt: runs
    // `proceed` now if there is nothing to lose or the user discards/saves,
    // drops it if they cancel. A Save that needs a location defers `proceed`
    // until the async Save As lands (see processPendingDialogs).
    void confirmDiscard(const char* title, std::function<void()> proceed);
    // The gated entry points. newProject()/closeProject()/quit_ stay ungated so
    // non-interactive callers (the control channel) can still force a replace.
    void requestNewProject();
    void requestCloseProject();
    void requestQuit();
    uint64_t savedSignature_ = 0;   // projectSignature() as of the last save/load
    std::function<void()> afterSave_; // action waiting on an in-flight Save As

    // Recent projects (empty-player launcher).
    void recordCurrentProject();   // push current path/id to ~/.jplay/settings.conf
    void openRecentProject(const std::string& path);
    void submitRecentRefresh();    // background-load list + thumbnails; build textures on the main thread
    void clearRecentTiles();       // destroy thumbnail textures
    void renderMainPanels();   // draw the list in the empty player area

    // Shared projects (empty-player launcher, studio shows).
    void submitSharedRefresh();    // parse the shows-config, then fan out one existence probe per show
    // Probe one show; the hits accumulate in `pending` and are swapped into
    // sharedProjects_ in one go when `outstanding` (the probe count) hits zero.
    void submitSharedProbe(SharedProjects::Entry entry,
                           std::shared_ptr<std::vector<SharedProject>> pending,
                           std::shared_ptr<int> outstanding);
    void openSharedProject(const std::string& path);     // bring in a shared .otio/.jpproj (unsaved)

    // Background work/job queue: runs tasks off the UI thread (e.g. the initial
    // recent-projects load); completion callbacks are marshaled back to the main
    // thread by drainCompletions() in the run loop. Paused while playing media,
    // reset on project load / quit. Declared early so it is torn down (workers
    // joined) before the SDL members it might reference are destroyed.
    WorkQueue work_;

    // A dedicated single-thread queue for the clip picker's Python queries
    // (resolve/describe). Separate from work_ so it is never paused during
    // playback (an interactive picker click must resolve even while playing), and
    // single-threaded because the GIL serialises Python anyway and we want
    // latest-click-wins rather than concurrency. drainCompletions() runs its
    // callbacks on the main thread, same as work_.
    WorkQueue pickerWork_{ 1 };

    // Decoration (labels/colors) runs on its own thread rather than pickerWork_:
    // it is the one picker query that can take tens of seconds, and behind a
    // single shared queue every click the user made meanwhile would sit behind it
    // — the pickers would answer at database speed, which is the whole thing
    // decoration exists to avoid. Its jobs carry no completion callback (results
    // are published through PickerDecorateStream), so nothing drains it.
    WorkQueue decorateWork_{ 1 };

    // OCIO transform prewarming for the clip ahead of the playhead (warmOcioAhead).
    // Its own thread, and deliberately never paused: unlike work_ it is needed most
    // *while* media plays, which is when a cut arrives. Its jobs carry no completion
    // callback (their only effect is OCIO's internal caches), so nothing drains it.
    WorkQueue ocioWarmWork_{ 1 };

    // Peak envelopes for the waveforms drawn inside audio clips, decoded on
    // work_ in the background.
    WaveformCache waveforms_;

    // Background metadata refresh: after a load, probe media whose cached
    // freshness hash no longer matches the file(s) on disk, in parallel. Media
    // that are still fresh are skipped entirely (their cached info is trusted).
    void refreshMediaMetadata();
    void joinRefresh(); // stop + join any in-flight refresh workers
    std::vector<std::thread> refreshThreads_;
    std::atomic<bool> refreshStop_{ false };

    // Background naming-convention tagging: fill in the picker metadata of media
    // that carry none (an OTIO import builds Media from the file's paths alone).
    // Set when the pass had to be skipped because Python was not up yet, and
    // re-run from the run loop once it is.
    void tagMediaPathValues();
    // Load the OCIO transform the upcoming clip needs before the playhead gets there.
    void warmOcioAhead();
    bool pendingPathValueTag_ = false;

    // Python startup: scans sys.path for jplay_init.py after the UI opens.
    std::thread pythonThread_;
};

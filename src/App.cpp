#include "App.h"
#include "AppInternal.h"
#include "Layout.h"

#include "AppIcon.h"
#include "AudioEngine.h"
#include "ProxyMode.h"
#include "PythonBridge.h"
#include "PythonStartup.h"
#include "SkinColors.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

using namespace jplay;

namespace {

// ---- app palette
// The opaque black backdrop used by render() to clear the window each frame.
constexpr SDL_Color kBlack         {   0,   0,   0, 255 }; // window / frame clear

inline void setColor(SDL_Renderer* r, SDL_Color c) {
    SDL_SetRenderDrawColor(r, c.r, c.g, c.b, c.a);
}

// Core-only layout / tuning constants. Shared layout constants (track sizes,
// panel widths) live in AppInternal.h so the split translation units can see
// them; these are used only here.
float kTimelinePad = 6.0f;
float kIconPadX = 8.0f;   // gap on each side of the title-bar app icon
float kIconSize = 18.0f;  // displayed icon edge within the title bar
float kMenuLeftInset = kIconPadX + kIconSize + kIconPadX; // first menu title x
const int kPrefetchAhead = 64;  // bootstrap-only forward window; see submitCacheRequests
const int kPrefetchBehind = 16; // ceiling on the look-behind, which the budget sizes
// Ceiling on the forward prefetch window, in seconds of playback. The cache
// budget alone used to size it, which on small proxy frames reaches thousands of
// frames -- and the whole window is re-derived on the main thread every time it
// changes, at a clip lookup and a cache request per frame. This much lead already
// rides out any stall the decoders can recover from; past it the window costs
// main-thread time for a head start playback will never spend.
const double kPrefetchLeadSeconds = 20.0;
// Ceiling on how long an unchanged wanted set is left standing (see
// submitCacheRequests). Async state the signature cannot see -- media metadata
// arriving, a decoder failing -- resolves within this.
const double kPrefetchRefreshMs = 500.0;

// How long init() waits for the embedded interpreter to answer what proxy mode a
// project should open in before giving that up and leaving it to the run loop.
// Generous against a normal startup (~60 ms here) and short enough that a site
// whose jplay_init.py hangs still gets its window at roughly the old time.
const double kProxyModeWaitMs = 250.0;

// Mix one value into a running signature. Cheap and order-dependent; used to tell
// whether the prefetch window still describes the same work, never persisted.
inline void mixSig(uint64_t& h, uint64_t v) {
    h ^= v + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2);
}

} // namespace

App::App() = default; // out-of-line so members with forward-declared types (AudioEngine) resolve here

App::~App() {
    audio_.reset(); // destroy the SDL audio stream while the audio subsystem is up
    work_.reset(); // drop queued/in-flight jobs + their callbacks before SDL teardown
    progress_.requestCancel(); // let any in-flight progress worker bail at its next checkpoint
    if (progressThread_.joinable()) progressThread_.join();
    if (pythonThread_.joinable()) pythonThread_.join();
    joinRefresh(); // stop background media probing
    cache_.reset(); // join workers before SDL teardown
    thumbs_.stop(); // join the thumbnail worker before SDL teardown
    freeOverviewTextures(); // destroy thumbnail textures while the renderer is alive
    freeSourceThumbs();     // SOURCES-bin thumbnail worker + textures
    freeLayoutSlots();      // Layout stage tile textures

    closeReviewWindow(); // review monitor window/renderer/texture (while SDL is up)
    clearRecentTiles();
    if (iconTex_) SDL_DestroyTexture(iconTex_);
    if (texture_) SDL_DestroyTexture(texture_);
    if (previewTex_) SDL_DestroyTexture(previewTex_);
    textFont_.destroy(); // UI text textures + font + TTF_Quit (while renderer alive)
    headerFont_.destroy();
    icons_.destroy(); // folder glyph texture + font + TTF_Quit (while renderer alive)
    if (cursorDefault_) SDL_DestroyCursor(cursorDefault_);
    if (cursorEWResize_) SDL_DestroyCursor(cursorEWResize_);
    if (cursorNSResize_) SDL_DestroyCursor(cursorNSResize_);
    if (cursorCrosshair_) SDL_DestroyCursor(cursorCrosshair_);
    if (cursorMove_) SDL_DestroyCursor(cursorMove_);
    if (cursorIBeam_) SDL_DestroyCursor(cursorIBeam_);
    if (hdrProgramTex_) SDL_DestroyTexture(hdrProgramTex_);
    hdrColorPass_.shutdown(); // release GPU shader/state before the renderer + device
    if (renderer_) SDL_DestroyRenderer(renderer_);
    if (gpuDevice_) SDL_DestroyGPUDevice(gpuDevice_); // HDR "gpu" renderer's device (destroy after the renderer)
    if (window_) SDL_DestroyWindow(window_);
    SDL_Quit();
}

// Create the SDL "gpu" (Vulkan) renderer for the main window with the given output
// colorspace (SRGB_LINEAR -> scRGB/HDR swapchain, SRGB -> SDR swapchain), creating
// gpuDevice_ first if needed. Returns null if the device or renderer can't be created.
SDL_Renderer* App::createGpuRenderer_(unsigned colorspace) {
    if (!gpuDevice_)
        gpuDevice_ = SDL_CreateGPUDevice(SDL_GPU_SHADERFORMAT_SPIRV, false, nullptr);
    if (!gpuDevice_)
        return nullptr;
    SDL_PropertiesID props = SDL_CreateProperties();
    SDL_SetStringProperty(props, SDL_PROP_RENDERER_CREATE_NAME_STRING, "gpu");
    SDL_SetPointerProperty(props, SDL_PROP_RENDERER_CREATE_WINDOW_POINTER, window_);
    SDL_SetPointerProperty(props, SDL_PROP_RENDERER_CREATE_GPU_DEVICE_POINTER, gpuDevice_);
    SDL_SetBooleanProperty(props, SDL_PROP_RENDERER_CREATE_GPU_SHADERS_SPIRV_BOOLEAN, true);
    SDL_SetNumberProperty(props, SDL_PROP_RENDERER_CREATE_OUTPUT_COLORSPACE_NUMBER, (Sint64)colorspace);
    SDL_Renderer* r = SDL_CreateRendererWithProperties(props);
    SDL_DestroyProperties(props);
    return r;
}

// The main window moved to a display whose HDR capability differs from the current
// swapchain composition, so we recreate the renderer to match: SDR (SRGB) when leaving
// an HDR display, HDR (scRGB) when arriving on one. SDL's "gpu" renderer fixes its
// composition at creation and re-asserts it on every vsync change, so we can't switch it
// live — we recreate the whole renderer. Every main-renderer-owned GPU object is torn
// down and rebuilt (fonts/icons/app-icon) or cleared so it lazily reloads (thumbnails,
// recent tiles, frame/preview/program textures). The HDR pipeline stays on either way
// (still the gpu backend, offscreen float target), so HDR review output continues.
// output_ is left untouched — on the gpu backend it holds no renderer-bound GL objects,
// so any active NDI sender stays connected across the rebuild.
void App::rebuildRenderer_(bool toHdr) {
    if (!renderer_ || !hdrPipeline_ || hdrActive_ == toHdr)
        return;
    if (jplayDebugLogging())
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "Main window changed display; recreating the renderer as %s.",
                    toHdr ? "HDR" : "SDR");

    SDL_FlushRenderer(renderer_); // drain queued work against the old renderer first

    // 1) Release / clear every main-renderer-owned GPU object.
    hdrColorPass_.shutdown();
    freeOverviewTextures();       // thumbTex_ (reloaded lazily from disk)
    freeLayoutSlots();            // Layout tiles (rebuilt from the cache next render)
    clearRecentTiles();           // recent_ tiles (rebuilt on recentDirty_)
    for (auto& kv : sourceThumbTex_)   // SOURCES-bin thumbnails (reloaded lazily)
        if (kv.second) SDL_DestroyTexture(kv.second);
    sourceThumbTex_.clear();
    sourceThumbGen_.clear();
    if (iconTex_)       { SDL_DestroyTexture(iconTex_);       iconTex_ = nullptr; }
    if (texture_)       { SDL_DestroyTexture(texture_);       texture_ = nullptr; }
    if (previewTex_)    { SDL_DestroyTexture(previewTex_);    previewTex_ = nullptr; }
    if (hdrProgramTex_) { SDL_DestroyTexture(hdrProgramTex_); hdrProgramTex_ = nullptr; }
    textFont_.destroy();
    headerFont_.destroy();
    icons_.destroy();

    // Reset trackers so lazily-rebuilt objects are recreated cleanly next frame.
    texW_ = texH_ = 0; hasTexture_ = false; displayedKey_ = CacheKey{};
    hdrProgW_ = hdrProgH_ = 0;
    previewTexW_ = previewTexH_ = 0; previewDisplayedValid_ = false;
    recentDirty_ = true;

    // 2) Recreate the device + renderer with a swapchain matching the new display.
    SDL_DestroyRenderer(renderer_);
    renderer_ = nullptr;
    SDL_DestroyGPUDevice(gpuDevice_);
    gpuDevice_ = nullptr;
    renderer_ = createGpuRenderer_(toHdr ? SDL_COLORSPACE_SRGB_LINEAR : SDL_COLORSPACE_SRGB);
    if (renderer_ && toHdr) {
        // Confirm the new display really can present HDR (Windows HDR may be off there).
        // If not, fall back to an SDR swapchain.
        SDL_PropertiesID rp = SDL_GetRendererProperties(renderer_);
        bool granted = windowCanPresentHdr_(window_, gpuDevice_);
        if (granted) {
            hdrHeadroom_ = SDL_GetFloatProperty(rp, SDL_PROP_RENDERER_HDR_HEADROOM_FLOAT, 1.0f);
            hdrSdrWhite_ = SDL_GetFloatProperty(rp, SDL_PROP_RENDERER_SDR_WHITE_POINT_FLOAT, 1.0f);
        } else {
            SDL_DestroyRenderer(renderer_);
            renderer_ = nullptr;
            SDL_DestroyGPUDevice(gpuDevice_);
            gpuDevice_ = nullptr;
            renderer_ = createGpuRenderer_(SDL_COLORSPACE_SRGB);
            toHdr = false;
        }
    }
    if (!renderer_) {
        // Extremely unlikely; drop to OpenGL SDR (loses the HDR pipeline / HDR review).
        renderer_ = SDL_CreateRenderer(window_, "opengl");
        if (!renderer_) renderer_ = SDL_CreateRenderer(window_, nullptr);
        hdrPipeline_ = false;
        toHdr = false;
    }
    hdrActive_ = toHdr;
    if (!renderer_) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Renderer rebuild failed: %s", SDL_GetError());
        quit_ = true; // cannot continue without a renderer
        return;
    }

    // 3) Reapply renderer settings + rebuild renderer-bound resources.
    SDL_SetRenderVSync(renderer_, reviewActive() ? 0 : 1); // review monitor stays the timing master
    SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_BLEND);
    loadAppIcon();
    icons_.load(renderer_);
    reloadUiFonts();
    ocioGpu_.init(renderer_);   // GL-only; stays inert on the gpu backend
    gradeGpuTried_ = false;     // GL objects died with the old context; rebuilt on next use
    hdrColorPass_.init(renderer_);
    if (!hdrPipeline_) {
        // Dropped to the GL fallback: the offscreen float readback that fed the sinks
        // is gone, so stop claiming it and re-acquire the GL readback path instead.
        output_.setExternalFeed(false);
        output_.init(renderer_);
    }

    setStatus(hdrActive_ ? "HDR enabled on the main window."
                         : "HDR disabled: main window is now sRGB.", 4000);
}

// Enforce the single-HDR-sink rule: while an external output (review monitor / SDI /
// NDI) is running or requested it owns HDR and the main window presents sRGB; with
// nothing out, the main window takes HDR back if its display can grant it.
//
// Edge-triggered on the sink state, for two reasons: a to-HDR rebuild that the display
// refuses leaves hdrActive_ false, and a level-triggered check would then rebuild on
// every frame; and on shutdown the review window is still open when reviewEnabled_ goes
// false, so waiting for the *next* frame lets syncReviewWindow close it first — the two
// never hold an HDR swapchain at the same time.
// Should this window get an scRGB (HDR) swapchain on the display it currently sits on?
//
// The two signals we trust are the display being in HDR mode and the GPU device being
// able to present HDR_EXTENDED_LINEAR to this window's surface.
//
// SDL_PROP_RENDERER_HDR_ENABLED_BOOLEAN is deliberately NOT the gate. SDL derives it
// from SDL_PROP_DISPLAY_HDR_HEADROOM_FLOAT > 1.0, and some Windows/driver combinations
// report a headroom of 1.0 for a display that is genuinely in HDR mode (observed on a
// Dell AW3423DW: DISPLAY_HDR_ENABLED=yes and every swapchain composition supported, yet
// RENDERER_HDR_ENABLED=false). Believing it made us destroy the working Vulkan renderer
// and fall back to OpenGL, whose fullscreen surface then knocked the monitor out of HDR
// altogether — which is exactly the "enabling output disables HDR" symptom.
bool App::windowCanPresentHdr_(SDL_Window* w, SDL_GPUDevice* dev) const {
    if (!w || !dev)
        return false;
    SDL_DisplayID d = SDL_GetDisplayForWindow(w);
    if (!SDL_GetBooleanProperty(SDL_GetDisplayProperties(d),
                                SDL_PROP_DISPLAY_HDR_ENABLED_BOOLEAN, false))
        return false;
    return SDL_WindowSupportsGPUSwapchainComposition(
        dev, w, SDL_GPU_SWAPCHAINCOMPOSITION_HDR_EXTENDED_LINEAR);
}

void App::logHdrDisplayState_(const char* when, SDL_Window* w, SDL_GPUDevice* dev) {
    if (!jplayDebugLogging())
        return;
    int n = 0;
    SDL_DisplayID* ds = SDL_GetDisplays(&n);
    SDL_DisplayID here = w ? SDL_GetDisplayForWindow(w) : 0;
    for (int i = 0; i < n; ++i) {
        const char* nm = SDL_GetDisplayName(ds[i]);
        bool hdr = SDL_GetBooleanProperty(SDL_GetDisplayProperties(ds[i]),
                                          SDL_PROP_DISPLAY_HDR_ENABLED_BOOLEAN, false);
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "%s: display %u \"%s\"%s SDL_PROP_DISPLAY_HDR_ENABLED=%s",
                    when, ds[i], nm ? nm : "?",
                    ds[i] == here ? " [window is here]" : "", hdr ? "yes" : "NO");
    }
    SDL_free(ds);
    if (w && dev) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "%s: swapchain composition support — SDR=%s SDR_LINEAR=%s "
                    "HDR_EXTENDED_LINEAR=%s HDR10_ST2084=%s", when,
                    SDL_WindowSupportsGPUSwapchainComposition(dev, w, SDL_GPU_SWAPCHAINCOMPOSITION_SDR) ? "yes" : "no",
                    SDL_WindowSupportsGPUSwapchainComposition(dev, w, SDL_GPU_SWAPCHAINCOMPOSITION_SDR_LINEAR) ? "yes" : "no",
                    SDL_WindowSupportsGPUSwapchainComposition(dev, w, SDL_GPU_SWAPCHAINCOMPOSITION_HDR_EXTENDED_LINEAR) ? "yes" : "NO",
                    SDL_WindowSupportsGPUSwapchainComposition(dev, w, SDL_GPU_SWAPCHAINCOMPOSITION_HDR10_ST2084) ? "yes" : "no");
    }
}

void App::syncHdrSink_() {
    if (!hdrPipeline_ || !gpuDevice_ || !window_)
        return;
    bool sink = externalSinkActive();
    if (sink == hdrSinkOwned_)
        return;
    hdrSinkOwned_ = sink;
    if (sink) {
        if (jplayDebugLogging())
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "HDR sink: external output claimed it (ndi=%s review=%s); "
                        "main window %s.",
                        output_.activeId().empty() ? "off" : output_.activeId().c_str(),
                        reviewEnabled_ ? "on" : "off",
                        hdrActive_ ? "rebuilding as SDR" : "already SDR");
        if (hdrActive_)
            rebuildRenderer_(false);
    } else {
        bool displayHdr = windowCanPresentHdr_(window_, gpuDevice_);
        if (jplayDebugLogging())
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "HDR sink: no external output; main window display HDR-capable=%s, "
                        "currently %s.",
                        displayHdr ? "yes" : "no", hdrActive_ ? "HDR" : "SDR");
        if (!hdrActive_ && displayHdr)
            rebuildRenderer_(true);
    }
}

bool App::init(int argc, char** argv) {
    launchTime_ = std::chrono::steady_clock::now();
    const auto launchTime = launchTime_;

    // Make sure our own SDL_LogInfo lines reach the terminal regardless of SDL's
    // per-platform default priority. The HDR / display diagnostics and the [startup]
    // timings are additionally gated on the Settings panel's "Debug Logging" (see
    // jplayDebugLogging).
    SDL_SetLogPriority(SDL_LOG_CATEGORY_APPLICATION, SDL_LOG_PRIORITY_INFO);

    // Seed the sync-session username from the environment up front so it's
    // populated for BOTH the SESSION side panel and the launcher's SYNC SESSION
    // column (which calls joinHost() directly, without the panel ever opening).
    {
        const char* envUser = std::getenv("USER");       // Linux / macOS
        if (!envUser) envUser = std::getenv("LOGNAME");  // Linux fallback
        if (!envUser) envUser = std::getenv("USERNAME"); // Windows
        if (envUser) sessionUserFld_.setText(envUser);
    }

    auto elapsedMs = [&launchTime] {
        return std::chrono::duration<double, std::milli>(
                   std::chrono::steady_clock::now() - launchTime).count();
    };
    // Preferences that have to be in force before the renderer exists: UI Scale (the
    // glyph atlases are rasterized for it) and HDR output (the renderer's output
    // colorspace is fixed at creation). Debug Logging comes along for the ride — the
    // renderer/HDR diagnostics and the [startup] timings below are gated on it and
    // would otherwise be silenced by its default-off value; that is also why this is
    // read before SDL comes up rather than alongside the subsystems it configures.
    // The rest of the preferences are applied further down, once the subsystems they
    // configure exist.
    const UserData::Prefs early = UserData::loadPrefs();
    jplaySetDebugLogging(early.pythonDebug);

    if (!SDL_Init(SDL_INIT_VIDEO)) {
        std::fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return false;
    }
    if (jplayDebugLogging())
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "  [startup] SDL_Init: %.1f ms", elapsedMs());

    // The embedded interpreter, started here rather than at the top of run(). The
    // proxy mode a project opens in is Python's answer (list_proxy_modes), and no
    // frame can be decoded before it is known -- so started after init() the
    // interpreter's ~60 ms of startup is 60 ms the playhead's read spends waiting,
    // while here it overlaps SDL_CreateRenderer, which needs none of it. The
    // resolver hook has to be installed first: it is the only way Media::ensureOpen()
    // -- which is also linked into the command-line tools, and so knows nothing of
    // pybind11 -- reaches Python.
    jplaySetPythonDebug(early.pythonDebug); // set before the interpreter, so its startup traces show
    jplay::setProxyPathResolver([](const std::string& path, const std::string& mode,
                                   std::string& outPath) {
        return jplayResolveProxyPath(path, mode, outPath);
    });
    // Same reason, for the slate a media's own file carries when no substitution
    // happened (a published quicktime added directly): only Python knows.
    jplay::setSlateFramesResolver([](const std::string& path) {
        return jplayPathSlateFrames(path);
    });
    pythonThread_ = spawnPythonStartup();
    // Pass through the click that re-activates the window instead of swallowing it.
    // Without this, clicking our custom min/max/close chrome (or anything else)
    // while the window is unfocused only raises focus and the button does nothing.
    SDL_SetHint(SDL_HINT_MOUSE_FOCUS_CLICKTHROUGH, "1");
    // Hint the compatibility 3.2 profile that OCIO's emitted GLSL needs. These
    // are read by SDL's OpenGL render backend when it creates its context in
    // SDL_CreateRenderer below; we intentionally do NOT pass SDL_WINDOW_OPENGL
    // here. That flag forces SDL to build a full GL context (loading the GPU
    // vendor's OpenGL driver, ~200 ms cold) inside SDL_CreateWindow, blocking
    // the window from appearing. The GL renderer creates its own context anyway,
    // so the up-front context is pure duplicated cost.
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 2);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_COMPATIBILITY);

    // HIGH_PIXEL_DENSITY asks for a backbuffer at the display's true resolution
    // rather than one point-sized and upscaled by the compositor. It is a no-op on
    // Windows (window sizes there are already device pixels); it is what keeps
    // macOS/Wayland from handing us a half-resolution surface.
    //
    // HIDDEN until the first frame is drawn (see run()). Because we deliberately
    // create without SDL_WINDOW_OPENGL, SDL_CreateRenderer("opengl") reconfigures
    // the window for GL — and on Windows that has no in-place path, so SDL
    // destroys the native window and builds a new one. Shown, that reads as a
    // blank window flashing up and closing before the real UI appears.
    window_ = SDL_CreateWindow("jplay", 1280, 800,
                               SDL_WINDOW_RESIZABLE | SDL_WINDOW_BORDERLESS |
                               SDL_WINDOW_HIGH_PIXEL_DENSITY | SDL_WINDOW_HIDDEN);
    if (!window_) {
        std::fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
        return false;
    }
    if (jplayDebugLogging())
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "  [startup] SDL_CreateWindow: %.1f ms", elapsedMs());

    // Resolve the device scale: a fixed UI Scale preference wins, otherwise read it
    // from the display. Nothing in the layout is scaled by hand — computeLayout()
    // hands this to SDL_SetRenderLogicalPresentation and the renderer does it, so the
    // UI is laid out in 1x units everywhere and simply drawn at a higher resolution.
    // A fixed preference also forces the scale *up* on a 1x monitor, which is how the
    // high-DPI look can be checked without one.
    {
        uiScalePref_ = early.uiScale > 0.0f ? std::clamp(early.uiScale, 0.5f, 4.0f) : 0.0f;
        float detected = SDL_GetWindowDisplayScale(window_);
        if (detected <= 0.0f) detected = 1.0f;
        uiScale_ = uiScalePref_ > 0.0f ? uiScalePref_ : detected;
    }
    applyWindowMinimumSize();
    titleBar_.setWindow(window_);
    // Borderless: drive window move/resize ourselves via a hit test.
    SDL_SetWindowHitTest(window_, &App::hitTest, this);

    // Audio comes up after the window is created: opening the default output
    // device costs real time (WASAPI/CoreAudio), and nothing before this point
    // needs it. Best-effort — a headless/CI box or a machine with no output
    // device must still run for review, and AudioEngine no-ops without one.
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "Audio unavailable, continuing without sound: %s", SDL_GetError());
    }
    // Opening the default output device costs 100-300 ms on WASAPI, and creating
    // the renderer below blocks this thread for far longer, so open it on a
    // worker and overlap the two. SDL_OpenAudioDeviceStream is documented safe
    // from any thread; the subsystem init above is not, hence the split. Joined
    // before the event watch is registered, so nothing can reach audio_ (still
    // null here) before it is built.
    //
    // Held by a joining guard: the renderer failure below returns early, and a
    // std::thread destroyed while still joinable calls std::terminate(), which
    // would turn that clean error exit (and any exception thrown in between)
    // into a crash.
    struct JoinGuard {
        std::thread t;
        ~JoinGuard() { if (t.joinable()) t.join(); }
    } audioOpen{std::thread([this] { audio_ = std::make_unique<AudioEngine>(); })};

    // The OCIO config load costs ~70 ms and needs neither the renderer nor GL, while
    // SDL_CreateRenderer below blocks this thread for far longer — so overlap the two,
    // exactly as with the audio device above. Joined at the original init() call site,
    // which is still well before the first frame, so a frame can never reach the
    // screen without the display transform available.
    //
    // The sample path driving the preferences-based $OCIO lookup has to come from argv
    // here: a .jpproj/.otio only names its media once loaded, and loading needs the
    // renderer. Those two keep the original serial path.
    // The colour pipeline this session starts in, read before any OCIO work is
    // scheduled: with the default ("srgb") no config is opened at all, so none of
    // the load below is paid for. A project saved in OCIO mode turns it back on as
    // it loads, as does the Settings toggle — both then load the config through
    // reinitOcioForFirstSource().
    timeline_.ocioEnabled = OcioManager::prefersOcio(); // saving this session records it
    ocio_.setEnabled(timeline_.ocioEnabled);

    std::string ocioSample;
    bool ocioNeedsProject = false;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (hasExtension(arg, ".jpproj") || hasExtension(arg, ".otio")) {
            ocioNeedsProject = true;
            break;
        }
        if (ocioSample.empty()) ocioSample = arg;
    }
    JoinGuard ocioOpen;
    if (!ocioNeedsProject && ocio_.isEnabled()) {
        ocio_.resolveEnv(ocioSample); // writes $OCIO; main thread only
        ocioOpen.t = std::thread([this, ocioSample] { ocio_.init(ocioSample); });
    }

    // ---------------------------------------------------------------------
    // Everything the first frame's decode depends on, deliberately ahead of
    // SDL_CreateRenderer below. An EXR read off network storage is ~200 ms of
    // per-stream latency and the renderer is ~160 ms of GL setup; neither needs
    // the other, so the read is started first and the renderer built while it is
    // in flight. Nothing between here and the renderer touches renderer_ (the
    // fonts, icons and GPU passes that do all come after it).
    // ---------------------------------------------------------------------

    // Joined before the load rather than after the renderer: adding a bare media
    // file resolves the config from that first source on this thread
    // (reinitOcioForFirstSource), and with the load moved up that would now run
    // alongside the very ocio_.init() this thread is in. The overlap it buys is
    // given up for the media-file launch -- the project launch, which is the one
    // this reordering is for, never spawns it (ocioNeedsProject above).
    if (ocioOpen.t.joinable())
        ocioOpen.t.join();

    // Decode pool size. Auto (the stored 0) leaves the machine two cores for the
    // UI and the OS; a pinned count overrides it, which is what 4K EXR review
    // wants -- there a decode is hundreds of ms of mostly-inflate work and the
    // pool's width is what caps how many frames a second can be readied. The
    // workers are spawned once here, so a change to this only lands on restart.
    decodeThreads_ = early.decodeThreads;
    decodeThreadsActive_ = decodeThreads_ > 0
        ? std::clamp(decodeThreads_, 1, 64)
        : std::clamp((int)std::thread::hardware_concurrency() - 2, 2, 8);
    // Built at the stored budget rather than a fixed one, so a large cache is
    // sized from the start instead of being grown out from under the first
    // decodes by the setMaxBytes further down.
    cacheGb_ = std::clamp(early.cacheGb, 0.25f, 64.0f);
    cache_ = std::make_unique<FrameCache>((size_t)(cacheGb_ * (1024.0 * 1024.0 * 1024.0)),
                                          decodeThreadsActive_);

    // Read early (like mcpEnabled_ below), not from the `prefs` block further down:
    // a .jpproj given on the command line loads before that block runs, and its
    // gate on timeline_.proxyMode needs the real preference by then.
    proxyEnabled_ = early.proxyEnabled;
    // Read early for the same reason: a bare media argument below is added with
    // the query_audio pairing enabled, and its gate needs the real preference
    // rather than this member's default.
    attachAudioToSeq_ = early.attachAudioToSeq;

    // Baseline for the unsaved-changes check, taken before the command line can
    // put anything in the project: an untouched launch has nothing to lose, so
    // quitting straight out of the launcher must not prompt. A .jpproj/.otio
    // argument re-stamps this via finishLoad(); bare media files deliberately
    // do not, since that really is an unsaved project.
    savedSignature_ = projectSignature();

    // The implicit project a launch starts in — the one bare media arguments and
    // everything opened from the launcher land in — is never built by newProject(),
    // so it takes the naming config's default proxy mode here instead. If the
    // interpreter started at the top of init() is not up yet this only arms the
    // deferred pass (waited for below); a .jpproj/.otio argument overrides it with
    // its own selection.
    adoptDefaultProxyMode();
    // adoptDefaultProxyMode() only records the selection; the decoders read the
    // ProxyMode global, so a mode adopted here has to be pushed as well -- exactly
    // the pairing loadProject()/newProject() use. Without it a bare media argument
    // (which goes through neither) opens against Full while the dropdown says
    // otherwise: before the interpreter moved into init() this call could never
    // resolve a mode, so the push came later from applyPendingProxyDefault().
    pushProxyMode();

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (hasExtension(arg, ".jpproj")) {
            loadProject(arg);
        } else if (hasExtension(arg, ".otio")) {
            loadOtio(arg);
        } else {
            // The new clip takes the current nextClipId_; Python may not be ready
            // yet, so no paired audio is added inline (see the deferred pass in run()).
            int clipId = nextClipId_;
            if (addMediaFile(arg))
                pendingCmdlineAudioClipIds_.push_back(clipId);
        }
    }

    // The decode cannot start until the proxy mode is settled: submitCacheRequests()
    // holds the whole prefetch back while pendingProxyDefault_ is set, because
    // decoding the representation the project is about to stop using only feeds
    // applyProxyMode()'s flush. The interpreter has had the window creation to come
    // up and is normally ready by now; the rest of its startup is worth waiting out
    // here, since leaving the mode to the run loop costs the read everything init()
    // has left to do. A slow or broken jplay_init.py must not hold the window back
    // though, so the wait is bounded and simply falls through to the deferred pass.
    if (pendingProxyDefault_ && timeline_.hasClips()) {
        const double waitStart = elapsedMs();
        while (!jplayPythonReady() && elapsedMs() - waitStart < kProxyModeWaitMs)
            SDL_Delay(1);
        if (jplayPythonReady())
            applyPendingProxyDefault();
        if (jplayDebugLogging())
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "  [startup] proxy mode: %.1f ms (%s)",
                        elapsedMs() - waitStart,
                        pendingProxyDefault_ ? "interpreter not up, deferred to run()"
                                             : "resolved");
    }
    // First frames requested here rather than from the first drawFrame(): the
    // playhead's read is the longest single item in a launch and this is the
    // earliest point it can start. drawFrame() republishes the same wanted set
    // every tick, so this only moves the start earlier -- it owns nothing.
    submitCacheRequests();
    if (jplayDebugLogging())
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "  [startup] first frames requested: %.1f ms",
                    elapsedMs());

    // The OCIO display+view processor, built here rather than on the first drawn
    // frame. It is one config->getProcessor() call -- 350-420 ms on this show's
    // config -- and it needs no frame and no media, only the config the load above
    // has just settled. Left lazy it lands in the middle of the first frame-bearing
    // drawFrame(), with the decoded image already sitting in the cache waiting on
    // it; here it runs alongside SDL_CreateRenderer and the first EXR reads.
    // OcioManager has no internal locking, so this worker owns ocio_ until it is
    // joined below, before init() next touches it. Spawned only when the config is
    // already up: sRGB launches open none, and there is then nothing to warm.
    // The source under the playhead has to be selected first, exactly as the player
    // does before its own transformFor(): the display processor is built against the
    // active config and the media's context variables ([ocio] context -- a config
    // with per-shot LUTs resolves them from these), so warming before they are set
    // builds a processor for the wrong context and the first frame simply rebuilds it.
    JoinGuard ocioWarm;
    if (ocio_.isEnabled() && ocio_.isReady()) {
        const ProgramSource ps = programSourceAt(timeline_.playhead);
        auto warmMedia = ps.a ? timeline_.findMediaById(ps.a->mediaId) : nullptr;
        if (warmMedia) {
            ocio_.setActiveConfigForPath(warmMedia->path());
            ocio_.setContextForMedia(warmMedia->meta());
            ocioWarm.t = std::thread([this] { ocio_.warmDisplay(); });
        }
    }

    hdrOutput_ = early.hdrOutput; // read before the window, with the UI scale

    // HDR output requires an SRGB_LINEAR (scRGB) swapchain, which SDL's OpenGL
    // backend rejects ("Unsupported output colorspace"). When HDR is requested we
    // use SDL's cross-platform "gpu" renderer instead — backed by Vulkan (Win/Linux),
    // D3D12 or Metal — which grants HDR and, via SDL_GPURenderState, lets the color
    // pipeline attach custom fragment shaders to normal draws (SDL owns all the
    // command-buffer/sync/swapchain work). We create an explicit SPIR-V GPU device so
    // the backend is Vulkan on every platform and we ship one shader format. The GL
    // OCIO/grade passes stay inert on this backend (they report not-ready); the
    // dedicated SDL_GPU color passes take over. If the gpu renderer can't be created
    // (no Vulkan, etc.) we drop to the GL SDR renderer below.
    if (hdrOutput_) {
        // Build the "gpu" (Vulkan) renderer on which HdrColorPass runs. The HDR pipeline
        // renders the transformed program into an offscreen float target, so it stays
        // useful (HDR review output) even when the main window is on an SDR display. The
        // main window's own swapchain is scRGB only when its launch display is in HDR
        // mode; otherwise it is a plain SDR swapchain, which is valid on any monitor and
        // can be dragged around without the "unsupported swapchain composition" failure.
        //
        // The display being in HDR mode is a *necessary* condition for the scRGB
        // swapchain — it is the first clause of windowCanPresentHdr_ — and it reads
        // off the video subsystem, so it is answerable before any device exists. On
        // an SDR display the scRGB attempt is therefore guaranteed to be handed back,
        // and asking for SDR up front skips a whole Vulkan device + renderer
        // create/destroy round trip. The reverse is not decidable here:
        // SDL_WindowSupportsGPUSwapchainComposition needs a live device, so a display
        // that reports HDR still gets the create-and-probe below.
        const bool displayInHdr = SDL_GetBooleanProperty(
            SDL_GetDisplayProperties(SDL_GetDisplayForWindow(window_)),
            SDL_PROP_DISPLAY_HDR_ENABLED_BOOLEAN, false);
        if (!displayInHdr) {
            logHdrDisplayState_("Main window", window_, nullptr); // no device yet: displays only
            renderer_ = createGpuRenderer_(SDL_COLORSPACE_SRGB);
        } else {
            // Try an HDR (scRGB) swapchain first and probe whether the display granted it.
            renderer_ = createGpuRenderer_(SDL_COLORSPACE_SRGB_LINEAR);
            if (renderer_) {
                logHdrDisplayState_("Main window", window_, gpuDevice_);
                SDL_PropertiesID rp = SDL_GetRendererProperties(renderer_);
                hdrHeadroom_ = SDL_GetFloatProperty(rp, SDL_PROP_RENDERER_HDR_HEADROOM_FLOAT, 1.0f);
                hdrSdrWhite_ = SDL_GetFloatProperty(rp, SDL_PROP_RENDERER_SDR_WHITE_POINT_FLOAT, 1.0f);
                hdrActive_   = windowCanPresentHdr_(window_, gpuDevice_);
                if (jplayDebugLogging())
                    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                                "Main window: asked for scRGB, keeping it=%s "
                                "(RENDERER_HDR_ENABLED=%s headroom=%.2f sdr_white=%.2f)",
                                hdrActive_ ? "yes" : "NO",
                                SDL_GetBooleanProperty(rp, SDL_PROP_RENDERER_HDR_ENABLED_BOOLEAN, false)
                                    ? "yes" : "no",
                                hdrHeadroom_, hdrSdrWhite_);
                if (!hdrActive_) {
                    // The display reports HDR but won't present the scRGB composition:
                    // recreate as a plain SDR swapchain. Still the gpu backend, so the
                    // HDR pipeline (and HDR review output) stays available; only the
                    // main window shows SDR.
                    SDL_DestroyRenderer(renderer_);
                    renderer_ = nullptr;
                    SDL_DestroyGPUDevice(gpuDevice_);
                    gpuDevice_ = nullptr;
                    renderer_ = createGpuRenderer_(SDL_COLORSPACE_SRGB);
                }
            }
        }
        hdrPipeline_ = (renderer_ != nullptr);
        if (hdrPipeline_) {
            if (jplayDebugLogging())
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                            "HDR pipeline: renderer=%s, main-window HDR=%s, headroom=%.2f, sdr_white=%.2f",
                            SDL_GetRendererName(renderer_) ? SDL_GetRendererName(renderer_) : "?",
                            hdrActive_ ? "yes" : "no (SDR display)", hdrHeadroom_, hdrSdrWhite_);
            if (!hdrActive_)
                setStatus("HDR output: main window is SDR (display not in HDR mode); an HDR review monitor still works.", 4000);
        } else {
            setStatus("HDR output enabled but the SDL_GPU renderer is unavailable.", 3000);
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "HDR output enabled but the SDL_GPU renderer is unavailable (%s); "
                        "falling back to SDR OpenGL.", SDL_GetError());
            if (gpuDevice_) {
                SDL_DestroyGPUDevice(gpuDevice_);
                gpuDevice_ = nullptr;
            }
        }
    }
    // Prefer the OpenGL backend so the OCIO GPU path can hook into it; fall back
    // to the platform default (OCIO then runs on the CPU).
    if (!renderer_)
        renderer_ = SDL_CreateRenderer(window_, "opengl");
    if (!renderer_) {
        std::fprintf(stderr, "OpenGL renderer unavailable (%s); using default backend, "
                             "OCIO will run on CPU\n", SDL_GetError());
        renderer_ = SDL_CreateRenderer(window_, nullptr);
    }
    if (!renderer_) {
        std::fprintf(stderr, "SDL_CreateRenderer failed: %s\n", SDL_GetError());
        return false;
    }
    if (jplayDebugLogging())
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "  [startup] SDL_CreateRenderer: %.1f ms", elapsedMs());
    // Which GL implementation we actually got. A software rasteriser ("GDI
    // Generic", llvmpipe) explains both a slow launch and a slow player, and is
    // otherwise invisible.
    if (jplayDebugLogging())
    {
        if (auto glGetString = (const unsigned char* (*)(unsigned int))SDL_GL_GetProcAddress("glGetString")) {
            auto str = [&](unsigned int e) {
                const unsigned char* s = glGetString(e);
                return s ? (const char*)s : "?";
            };
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "GL: %s / %s / %s",
                        str(0x1F00 /*GL_VENDOR*/), str(0x1F01 /*GL_RENDERER*/),
                        str(0x1F02 /*GL_VERSION*/));
        }
    }
    SDL_SetRenderVSync(renderer_, 1);
    SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_BLEND);
    cursorDefault_   = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_DEFAULT);
    cursorEWResize_  = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_EW_RESIZE);
    cursorNSResize_  = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_NS_RESIZE);
    cursorCrosshair_ = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_CROSSHAIR);
    cursorMove_      = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_MOVE);
    cursorIBeam_     = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_TEXT);
    loadAppIcon();
#ifndef _WIN32
    // Windows picks the window/taskbar icon up from the embedded resource; on
    // other platforms set it explicitly from the same bundled icon so the WM
    // (X11 taskbar, alt-tab) shows it too.
    {
        std::vector<uint8_t> rgba;
        int iw = 0, ih = 0;
        if (loadAppIconRGBA(128, rgba, iw, ih) && iw > 0 && ih > 0) {
            SDL_Surface* iconSurf = SDL_CreateSurfaceFrom(
                iw, ih, SDL_PIXELFORMAT_RGBA32, rgba.data(), iw * 4);
            if (iconSurf) {
                SDL_SetWindowIcon(window_, iconSurf);
                SDL_DestroySurface(iconSurf);
            }
        }
    }
#endif
    icons_.load(renderer_); // embedded MDI webfont for toolbar glyphs
    {
        reloadUiFonts();
        menuBar_.setTextFont(&textFont_);
        clipMenu_.setTextFont(&textFont_);
        clipToolboxMenu_.setTextFont(&textFont_);
        fitMenu_.setTextFont(&textFont_);
        trackMenu_.setTextFont(&textFont_);
        peSortMenu_.setTextFont(&textFont_);
        peMediaMenu_.setTextFont(&textFont_);
        peSeqColorMenu_.setTextFont(&textFont_);
        appIconMenu_.setTextFont(&textFont_);
        titleBar_.setTextFont(&textFont_);
    }

    audioOpen.t.join(); // audio_ is live from here (see the spawn above)

    // Repaint live during the OS modal resize/move loop (see onWatchEvent).
    // Registered after the renderer exists so the watch never fires without one.
    SDL_AddEventWatch(&App::onWatchEvent, this);

    // Local control channel. Binding the loopback port is what makes this the
    // instance external tools drive; a second jplay fails to bind and simply
    // doesn't listen (it keeps retrying, so it takes over if this one exits).
    // Commands are only ever applied from run(), via drainControl().
    // Opt-in (Settings > "MCP Control Channel", defaulted by [control] enabled in
    // jplay_preferences.conf): left off, no socket is created, so a fresh install
    // never trips the Windows firewall prompt.
    mcpEnabled_ = early.mcpEnabled;
    if (mcpEnabled_)
        control_.start();

    // ocio_ belongs to the warm worker until here (see the spawn above); everything
    // between the two -- renderer, fonts, icons, audio -- keeps off it. Joined rather
    // than left running into the run loop: the window is held the little this still
    // has left instead of coming up on a frame that cannot be colour-managed yet, so
    // the UI and the picture appear together.
    if (ocioWarm.t.joinable()) {
        const double warmT0 = elapsedMs();
        ocioWarm.t.join();
        if (jplayDebugLogging())
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "  [startup] ocio warm: waited %.1f ms (done at %.1f ms)",
                        elapsedMs() - warmT0, elapsedMs());
    }

    // A representative source path lets OcioManager derive the config from
    // jplay_preferences.conf when $OCIO is unset (see OcioManager::init).
    std::string samplePath;
    for (const auto& kv : timeline_.media) {
        if (kv.second && !kv.second->path().empty()) { samplePath = kv.second->path(); break; }
    }
    if (ocioOpen.t.joinable())
        ocioOpen.t.join();      // ran alongside SDL_CreateRenderer; ocio_ is live from here
    else if (ocio_.isEnabled() && !ocio_.isReady())
        ocio_.init(samplePath); // project/OTIO: samplePath only exists now
                                // (already loaded when the project itself asked for OCIO)
    if (ocio_.isReady()) {
        const char* ocioEnv = std::getenv("OCIO");
        SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION, "OCIO config: %s (from %s)",
                     ocioEnv && ocioEnv[0] ? ocioEnv : "built-in CG config",
                     ocioEnv && ocioEnv[0] ? "$OCIO / preferences" : "fallback");
    }
    ocioGpu_.init(renderer_); // GPU path; init() returns false on non-GL backends (then CPU is used)
    // gradeGpu_ is built lazily on the first graded/tech-check frame (see
    // ensureGradeGpu) — its shaders cost more than everything else in init()
    // outside the renderer, and most sessions never grade.
    hdrColorPass_.init(renderer_); // HDR color pass (SDL_GPU backend only; no-op otherwise)
    // On the SDL_GPU (HDR) backend OCIO runs via hdrColorPass_, not the GL path.
    SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION, "OCIO transform: %s",
                 ocioGpu_.isReady()      ? "GPU (GL)"
                 : hdrColorPass_.isReady() ? "GPU (SDL_GPU / HDR)"
                                           : "CPU");
    output_.init(renderer_);   // external video output (NDI/SDI); readback needs the GL backend
    if (hdrPipeline_)
        output_.setExternalFeed(true); // SDL_GPU path reads back the float program + feeds submitFrame itself
    buildMenu();
    // The metadata pickers are config-driven and served by Python (not ready
    // until run()), so they are built lazily from openClipPickerMenu — not here.
    buildSettingsBar();
    {
        UserData::Prefs prefs = UserData::loadPrefs();
        timeFormatFrames_ = prefs.timeFormatFrames;
        frameNumberingClip_ = prefs.frameNumberingClip;
        pythonDebug_ = prefs.pythonDebug;
        jplaySetPythonDebug(pythonDebug_);
        jplaySetDebugLogging(pythonDebug_);
        showFramePreview_ = prefs.showFramePreview;
        snapPlayhead_ = prefs.snapPlayhead;
        audioScrub_ = prefs.audioScrub;
        clipWaveform_ = prefs.clipWaveform;
        frameOverlay_ = prefs.frameOverlay;
        frameOverlayBottom_ = prefs.frameOverlayBottom;
        overlaySize_ = std::clamp(prefs.overlaySize, 0, (int)std::size(kOverlaySizes) - 1);
        overlayColor_ = std::clamp(prefs.overlayColor, 0, (int)std::size(kOverlayColors) - 1);
        // Straight to the member rather than through setCompactTimeline: there is no
        // pane to close and no status line to raise before the first frame is up.
        compactTimeline_ = prefs.compactTimeline;
        volume_ = std::clamp(prefs.volume, 0.0f, 1.0f);
        muted_ = prefs.muted;
        if (audio_) // the device thread was joined above
            audio_->setVolume(muted_ ? 0.0f : volume_);
        warnUnsaved_ = prefs.warnUnsaved;
        peThumbSize_ = (float)std::clamp(prefs.sourceThumbSize, 16, 128);
        gridThumbH_ = (float)std::clamp(prefs.gridThumbH, 24, 480);
        peSort_ = (PeSort)std::clamp(prefs.sourceSort, 0, 1);
        cacheGb_ = std::clamp(prefs.cacheGb, 0.25f, 64.0f);
        cache_->setMaxBytes((size_t)(cacheGb_ * (1024.0 * 1024.0 * 1024.0)));
        decodeThreads_ = prefs.decodeThreads; // pool already built from `early` above
        nitRef_ = std::clamp(prefs.nitRef, 1.0f, 10000.0f);
        hdrOutput_ = prefs.hdrOutput; // already applied at renderer creation above
        // prefs.uiScale likewise: applied to uiScalePref_ before the window was made.
        hdrRefWhiteNits_ = std::clamp(prefs.hdrRefWhiteNits, 10.0f, 1000.0f);
        // prefs.mcpEnabled was applied from `early` at control_.start() above.
        // prefs.proxyEnabled was applied from `early` above, for the same reason.
        // prefs.attachAudioToSeq likewise: the command-line media add needs it.
        syncNetwork_ = prefs.syncNetwork;
        syncPort_ = (prefs.syncPort > 0 && prefs.syncPort <= 65535) ? prefs.syncPort
                                                                    : syncreview::kDefaultPort;
        sessionPortFld_.setText(std::to_string(syncPort_));
    }
    updateWindowTitle();
    if (jplayDebugLogging())
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "  [startup] init() total: %.1f ms", elapsedMs());
    lastTickMs_ = SDL_GetTicks();
    return true;
}

// Compile the grade/tech-check shaders the first time a frame actually needs
// them (see the call in renderPlayer). One attempt only: on a non-GL backend
// init() fails and must not be retried every frame.
void App::ensureGradeGpu() {
    if (gradeGpuTried_)
        return;
    gradeGpuTried_ = true;
    auto t0 = std::chrono::steady_clock::now();
    gradeGpu_.init(renderer_);
}

void App::reinitOcioForFirstSource() {
    // Also the point OCIO is loaded at all: the sRGB pipeline opens no config, so
    // the manager may still be empty here — turning OCIO on (a project's saved mode,
    // the Settings toggle) leaves it to this call to bring one up.
    if (!ocio_.isEnabled())
        return;
    const bool wasReady = ocio_.isReady();
    // Only the built-in fallback is worth replacing: a config from $OCIO or a
    // prior preferences match is already the intended one, so leave it be.
    if (wasReady && !ocio_.usingBuiltinConfig())
        return;
    // A representative image/video path lets init() derive the config from
    // jplay_preferences.conf (audio paths never match the [ocio] rules).
    std::string samplePath;
    for (const auto& kv : timeline_.media) {
        if (kv.second && kv.second->type() != ClipType::Audio && !kv.second->path().empty()) {
            samplePath = kv.second->path();
            break;
        }
    }
    if (samplePath.empty() && wasReady)
        return; // no source to resolve a better config from, and one is already loaded
    if (ocio_.init(samplePath) && (!wasReady || !ocio_.usingBuiltinConfig())) {
        const char* ocioEnv = std::getenv("OCIO");
        if (jplayDebugLogging())
        {
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "OCIO %s: %s", wasReady ? "re-init from first source" : "init",
                        ocioEnv && ocioEnv[0] ? ocioEnv : "built-in CG config");
        }
        hasTexture_ = false; // force a re-render through the newly resolved transform
    }
}

// ---------------------------------------------------------------- menu + dialogs

void App::buildMenu() {
    // Anything that acts on the current project is greyed when there is no
    // project to act on, rather than sitting enabled and doing nothing — the
    // launcher leaves most of this menu inert, and it should say so.
    auto hasContent = [this] { return hasProjectContent(); };

    int file = menuBar_.addMenu("File");
    // New and Close both clear the project; they differ only in where they
    // leave you. New drops you on an empty timeline ready to work, Close hands
    // you back the launcher. Close is enabled whenever a project view is up,
    // including an emptied one, since returning to the launcher is still
    // meaningful there.
    //
    // New stays live on the launcher even with nothing to clear: there it is the
    // menu's version of CREATE EMPTY PROJECT, dismissing the launcher onto a
    // blank timeline. The one state where it really does nothing is a launcher
    // already dismissed onto an empty project, and only that state greys it.
    menuBar_.addItem(file, "New Project", [this] { requestNewProject(); },
                     [this] { return hasProjectContent() || launcherVisible(); },
                     nullptr, "Ctrl+N");
    menuBar_.addSeparator(file);
    menuBar_.addItem(file, "Open Project", [this] { openProjectDialog(); },
                     nullptr, nullptr, "Ctrl+O");
    menuBar_.addItem(file, "Save Project", [this] { saveProjectQuick(); },
                     hasContent, nullptr, "Ctrl+S");
    menuBar_.addItem(file, "Save Project As", [this] { saveProjectDialog(); },
                     hasContent, nullptr, "Ctrl+Shift+S");
    menuBar_.addItem(file, "Close Project", [this] { requestCloseProject(); },
                     [this] { return !launcherVisible(); },
                     nullptr, "Ctrl+W");
    menuBar_.addSeparator(file);
    menuBar_.addItem(file, "Import Media", [this] { addMediaViaBrowser(); });
    {
        // Export needs at least one placed clip; disable on an empty project.
        auto hasClips = [this] {
            bool any = false;
            timeline_.forEachClip([&](const Clip&) { any = true; });
            return any;
        };
        int exportSub = menuBar_.addSubmenu(file, "Export");
        menuBar_.addItem(exportSub, "Export Movie...",
                         [this] { openExportMovie(); }, hasClips);
        menuBar_.addItem(exportSub, "Export Image Sequence...",
                         [this] { openExportImageSequence(); }, hasClips);
        menuBar_.addItem(exportSub, "Export OTIO...",
                         [this] { exportOtio(); }, hasClips);
    }
    menuBar_.addSeparator(file);
    menuBar_.addItem(file, "Quit", [this] { requestQuit(); }, nullptr, nullptr, "Q");

    int edit = menuBar_.addMenu("Edit");
    menuBar_.addItem(edit, "Undo", [this] { undo(); },
                     [this] { return undoStack_.canUndo(); }, nullptr, "Ctrl+Z");
    menuBar_.addItem(edit, "Redo", [this] { redo(); },
                     [this] { return undoStack_.canRedo(); }, nullptr, "Ctrl+Y");

    // ── View ──
    // The display state you flip while reviewing, gathered in one place: what the
    // player is showing, the left panes (whose only other affordance is an
    // unlabelled glyph in the icon strip), and the timeline display preferences.
    // The preferences are mirrored here rather than moved — the Settings panel
    // stays the complete list and this is the fast path, the same way the sync
    // socket sits both there and on the sync panel. Everything but the preferences
    // greys out while the launcher is up, since the player stage and the icon
    // strip are both hidden there (see render()).
    //
    // Marked rows carry a filled square in the blank their label's leading spaces
    // reserve (MenuBar's `checked` predicate), so the text sits in the same place
    // whether a row is active or not.
    auto onStage = [this] { return !launcherVisible(); };
    int view = menuBar_.addMenu("View");
    // Fullscreen gets a section of its own at the top because it is not one of the
    // stages below but a frame around whichever of them is up: the same picture,
    // with every band of chrome — this menu included — taken away. Esc leaves it
    // too, which is why the row names only the key that also enters it.
    menuBar_.addItem(view, "  Fullscreen", [this] { setCinemaMode(!cinemaMode_); },
                     onStage, nullptr, "F11", [this] { return cinemaMode_; });
    // In that section for the same reason: not one of the stages but a frame around
    // whichever is up — the timeline collapses to its ruler and cache strip, and the
    // player takes the height back. The rest of the chrome stays put.
    // Greyed while a comparison stage is up: those stages hold the timeline
    // compact, so the row's only remaining job would be the one thing it is refused.
    menuBar_.addItem(view, "  Compact Timeline",
                     [this] { setCompactTimeline(!compactTimeline_); },
                     [this] { return !launcherVisible() && !compareStage(); },
                     nullptr, "TAB", [this] { return compactTimeline_; });
    menuBar_.addSeparator(view);
    // The five things the player can be showing, as one radio group: the source
    // view plus the four stages. Each row is a destination rather than a toggle,
    // exactly like the F1–F5 keys it mirrors (see onKeyDown); re-picking the row
    // you are on does nothing, whereas Tab and L stay toggles.
    menuBar_.addItem(view, "  Source",
                     [this] {
                         setPlayerStage(PlayerStage::Frame);
                         openSourceViewAtPlayhead();
                     }, onStage, nullptr, "F1",
                     [this] { return sourceViewActive(); });
    // Timeline is also the way out of a scratch view: the marked row while you are
    // looking at one is "Source" or "Layout", so this is the labelled way back, and
    // it drops the view exactly as Backspace does - playhead and zoom included.
    menuBar_.addItem(view, "  Timeline",
                     [this] {
                         dropScratchView();
                         setPlayerStage(PlayerStage::Frame);
                     }, onStage, nullptr, "F2",
                     [this] { return stage_ == PlayerStage::Frame && !sourceViewActive(); });
    // Layout carries two keys: the row's own F-key switch, and the older L toggle
    // that has no row of its own, listed so the toggle stays discoverable.
    // Both off while a source view is up: there is one clip to show, which is what
    // the frame is already doing (the keys say so too, see setPlayerStage).
    auto onStageNoSource = [this] { return !launcherVisible() && !sourceViewActive(); };
    menuBar_.addItem(view, "  Overview", [this] { setPlayerStage(PlayerStage::Overview); },
                     onStageNoSource, nullptr, "F3", [this] { return gridView(); });
    menuBar_.addItem(view, "  Layout", [this] { setPlayerStage(PlayerStage::Layout); },
                     onStageNoSource, nullptr, "F4", [this] { return layoutView(); });
    // The Layout's other half: the same clips, one at a time, Up/Down rotating
    // which is on top.
    menuBar_.addItem(view, "  Stack", [this] { setPlayerStage(PlayerStage::Stack); },
                     onStageNoSource, nullptr, "F5", [this] { return stackView(); });
    menuBar_.addSeparator(view);
    menuBar_.addItem(view, "  Source Inspector",
                     [this] { inspectorOpen_ = !inspectorOpen_; }, onStage,
                     nullptr, "I", [this] { return inspectorOpen_; });
    menuBar_.addItem(view, "  Pixel Inspector",
                     [this] { pixelInspectorOpen_ = !pixelInspectorOpen_; }, onStage,
                     nullptr, "P", [this] { return pixelInspectorOpen_; });
    menuBar_.addSeparator(view);

    // Burn-in over the picture: file name and the playhead readout, on the main
    // player and the review monitor. The switch is a row of its own because it is
    // the thing you reach for; how it looks is set once, so it sits one level down.
    menuBar_.addItem(view, "  Show Overlay",
                     [this] { frameOverlay_ = !frameOverlay_; writePrefs(); }, onStage,
                     nullptr, "", [this] { return frameOverlay_; });
    {
        int opts = menuBar_.addSubmenu(view, "  Overlay Options");
        menuBar_.addItem(opts, "  Alignment", nullptr, [] { return false; }); // section header
        menuBar_.addItem(opts, "    Top",
                         [this] { frameOverlayBottom_ = false; writePrefs(); }, onStage,
                         nullptr, "", [this] { return !frameOverlayBottom_; });
        menuBar_.addItem(opts, "    Bottom",
                         [this] { frameOverlayBottom_ = true; writePrefs(); }, onStage,
                         nullptr, "", [this] { return frameOverlayBottom_; });
        menuBar_.addSeparator(opts);
        menuBar_.addItem(opts, "  Size", nullptr, [] { return false; }); // section header
        for (int i = 0; i < (int)std::size(kOverlaySizes); ++i)
            menuBar_.addItem(opts, std::string("    ") + kOverlaySizes[i].name,
                             [this, i] {
                                 overlaySize_ = i;
                                 reloadOverlayFont();
                                 reloadReviewOverlayFont();
                                 writePrefs();
                             }, onStage,
                             nullptr, "", [this, i] { return overlaySize_ == i; });
        menuBar_.addSeparator(opts);
        menuBar_.addItem(opts, "  Color", nullptr, [] { return false; }); // section header
        for (int i = 0; i < (int)std::size(kOverlayColors); ++i)
            menuBar_.addItem(opts, std::string("    ") + kOverlayColors[i].name,
                             [this, i] { overlayColor_ = i; writePrefs(); }, onStage,
                             nullptr, "", [this, i] { return overlayColor_ == i; });
    }
    menuBar_.addSeparator(view);
    {
        // One entry per icon-strip toggle, in strip order, each acting exactly like
        // a click on its glyph: the panes are mutually exclusive, and re-picking
        // the open one closes it (see toggleLeftPanel).
        struct PanelEntry { const char* label; LeftPanel panel; };
        static const PanelEntry kPanelEntries[] = {
            { "Project Explorer", LeftPanel::ProjectExplorer },
            { "Clip Source",      LeftPanel::ClipSource },
            { "Color Grading",    LeftPanel::Grade },
            { "Tech Check",       LeftPanel::Tech },
            { "Draw",             LeftPanel::Draw },
            { "Sync Review",      LeftPanel::Sync },
            { "Settings",         LeftPanel::Settings },
        };
        int panels = menuBar_.addSubmenu(view, "  Panels");
        for (const auto& pe : kPanelEntries) {
            const char* label = pe.label;
            const LeftPanel panel = pe.panel;
            menuBar_.addItem(panels, std::string("  ") + label,
                             [this, panel] { toggleLeftPanel(panel); }, onStage,
                             nullptr, "", [this, panel] { return leftPanelOpen(panel); });
        }
    }
    menuBar_.addSeparator(view);

    // Time Format / Frame Numbering: the two radio pairs from the Settings panel's
    // Timeline group. Both write their member and persist through writePrefs(), so
    // the panel's segmented toggle and these rows can never disagree.
    menuBar_.addItem(view, "  Time Format", nullptr, [] { return false; }); // section header
    menuBar_.addItem(view, "    Timecode",
                     [this] { timeFormatFrames_ = false; writePrefs(); }, nullptr,
                     nullptr, "", [this] { return !timeFormatFrames_; });
    menuBar_.addItem(view, "    Frames",
                     [this] { timeFormatFrames_ = true; writePrefs(); }, nullptr,
                     nullptr, "", [this] { return timeFormatFrames_; });
    menuBar_.addSeparator(view);
    menuBar_.addItem(view, "  Frame Numbering", nullptr, [] { return false; }); // section header
    menuBar_.addItem(view, "    Global",
                     [this] { frameNumberingClip_ = false; writePrefs(); }, nullptr,
                     nullptr, "", [this] { return !frameNumberingClip_; });
    menuBar_.addItem(view, "    Clip",
                     [this] { frameNumberingClip_ = true; writePrefs(); }, nullptr,
                     nullptr, "", [this] { return frameNumberingClip_; });
    menuBar_.addSeparator(view);
    menuBar_.addItem(view, "  Timeline Preview Thumbnail",
                     [this] {
                         showFramePreview_ = !showFramePreview_;
                         if (!showFramePreview_)
                             clearFramePreview();
                         writePrefs();
                     }, nullptr,
                     nullptr, "", [this] { return showFramePreview_; });
    // Waveform inside the video clip under the playhead. Only that clip's range
    // is decoded, and only while stopped, so turning it on costs one short read
    // per clip you park on rather than a scan of every visible master.
    menuBar_.addItem(view, "  Clip Waveform",
                     [this] {
                         clipWaveform_ = !clipWaveform_;
                         writePrefs();
                     }, nullptr,
                     nullptr, "", [this] { return clipWaveform_; });

    // The sequence view filter lives in the top toolbar (a left-aligned "Sequence"
    // button whose label shows the selected sequence); see renderSequenceMenu().

    // OCIO enable now lives in the Settings panel (PROJECT SETTINGS → Color
    // Management), persisted per-project. The Display / View / File Colorspace
    // pickers remain available as top-toolbar buttons (see App_Letterbox.cpp).

    // External output: NDI and the review monitor are mutually exclusive, so both
    // live in one radio-style menu. Items are rebuilt from the backend/display set
    // by updateOutputMenu() whenever it changes (hotplug); the active-selection
    // mark is evaluated live per frame by each item's `checked` predicate.
    outputMenuIdx_ = menuBar_.addMenu("Output");
    {
        auto t0 = std::chrono::steady_clock::now();
        enumerateAudioDevices(); // once at startup; no runtime hotplug
        if (jplayDebugLogging())
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "  [startup] enumerateAudioDevices: %.1f ms (%zu devices)",
                        std::chrono::duration<double, std::milli>(
                            std::chrono::steady_clock::now() - t0).count(),
                        audioDevices_.size());
    }
    populateOutputMenu();

    int help = menuBar_.addMenu("Help");
    menuBar_.addItem(help, "Keyboard Shortcuts",
                     [this] { helpOpen_ = true; aboutOpen_ = false; },
                     nullptr, nullptr, "H");
    menuBar_.addItem(help, "About jplay", [this] { aboutOpen_ = true; helpOpen_ = false; });
}

void App::populateOutputMenu() {
    if (outputMenuIdx_ < 0) return;
    menuBar_.clearItems(outputMenuIdx_);

    // HDR sits at the top: the enable toggle (effective on the next launch — the
    // renderer's output colorspace is fixed at creation) and, only while enabled,
    // the paper-white reference slider (live, but greyed until the pipeline is up).
    menuBar_.addItem(outputMenuIdx_, "  Enable HDR Output",
        [this] {
            hdrOutput_ = !hdrOutput_;
            writePrefs();
            if (hdrOutput_)
                setStatus("HDR output enabled, please restart.", 3000);
            else
                setStatus("HDR output disabled, please restart.", 3000);
            populateOutputMenu(); // show/hide the ref-white slider below
        },
        nullptr, nullptr, "", [this] { return hdrOutput_; });
    if (hdrOutput_) {
        menuBar_.addSlider(outputMenuIdx_, "  Ref White: 000 nits",
            kHdrRefMinNits, kHdrRefMaxNits,
            [this] { return hdrRefWhiteNits_; },
            [this](float v) { hdrRefWhiteNits_ = std::round(v); },
            [this] { writePrefs(); },
            [this] { return hdrPipeline_; },
            [this]() -> std::string {
                char b[48];
                SDL_snprintf(b, sizeof(b), "  Ref White: %d nits", (int)(hdrRefWhiteNits_ + 0.5f));
                return b;
            });
    }

    // Video section: external backends (NDI, ...) and the review monitor, all
    // mutually exclusive. An unavailable backend (runtime missing) is shown greyed.
    menuBar_.addSeparator(outputMenuIdx_);
    menuBar_.addItem(outputMenuIdx_, "  Video", nullptr, [] { return false; }); // section header
    const auto& bes = output_.backends();
    for (const auto& b : bes) {
        if (!b.available) {
            menuBar_.addItem(outputMenuIdx_, "  " + b.label + " (unavailable)",
                             nullptr, [] { return false; });
            continue;
        }
        std::string id = b.id, label = b.label;
        menuBar_.addItem(outputMenuIdx_, "  " + label,
            [this, id] { reviewEnabled_ = false; output_.select(id); },
            nullptr, nullptr, "",
            [this, id] { return !reviewEnabled_ && output_.activeId() == id; });
        // What the device is actually sending, under the entry that selects it: the
        // colorimetry of the last frame out plus the receiver count. The stream's
        // colorimetry is not a setting — it follows the frame (see hdrProgramTransfer_),
        // so a PQ or HLG stream drops to Rec.709 the moment the timeline reaches a
        // video clip, and this is the only place that says so. Live via labelFn: the
        // item SET is rebuilt only on hotplug (updateOutputMenu), never on selection.
        // The static label is the widest form the labelFn can return, because MenuBar
        // sizes the dropdown from it.
        menuBar_.addItem(outputMenuIdx_, "      HLG Rec.2020 16-bit, 00 receivers",
                         nullptr, [] { return false; },
            [this, id]() -> std::string {
                if (reviewEnabled_ || output_.activeId() != id)
                    return "      not sending";
                return "      " + output_.statusLine();
            });
    }

    // Review monitor: offered only when a second display is connected (with a
    // single display there's nowhere to put it). Needs the GL readback path.
    int dispCount = 0;
    SDL_DisplayID* disps = SDL_GetDisplays(&dispCount);
    if (dispCount > 1) {
        bool canRead = output_.canRead();
        SDL_DisplayID guiDisp = SDL_GetDisplayForWindow(window_);
        for (int i = 0; i < dispCount; ++i) {
            SDL_DisplayID id = disps[i];
            SDL_Rect bounds{};
            SDL_GetDisplayBounds(id, &bounds);
            const char* nm = SDL_GetDisplayName(id);
            std::string label = "Review: " + std::string(nm ? nm : "Display") + "  " +
                                std::to_string(bounds.w) + "x" + std::to_string(bounds.h);
            if (id == guiDisp)
                label += "  (main)";
            if (!canRead) {
                menuBar_.addItem(outputMenuIdx_, "  " + label, nullptr, [] { return false; });
                continue;
            }
            menuBar_.addItem(outputMenuIdx_, "  " + label,
                [this, id] {
                    output_.select("");
                    if (reviewWindow_ && reviewDisplay_ != id)
                        closeReviewWindow(); // rebuild on the newly-chosen monitor
                    reviewDisplay_ = id;
                    reviewEnabled_ = true;
                },
                nullptr, nullptr, "",
                [this, id] { return reviewEnabled_ && reviewDisplay_ == id; });
        }
    }
    SDL_free(disps);

    // Audio section: output device selection. The AudioEngine reopens its stream
    // on the chosen device; "System Default" follows whatever SDL picks.
    menuBar_.addSeparator(outputMenuIdx_);
    menuBar_.addItem(outputMenuIdx_, "  Audio", nullptr, [] { return false; }); // section header
    menuBar_.addItem(outputMenuIdx_, "  System Default",
        [this] {
            audioDeviceId_ = SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK;
            if (audio_) audio_->setDevice(audioDeviceId_);
        },
        nullptr, nullptr, "",
        [this] { return audioDeviceId_ == SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK; });
    for (const auto& d : audioDevices_) {
        SDL_AudioDeviceID id = d.id;
        std::string name = d.name;
        menuBar_.addItem(outputMenuIdx_, "  " + name,
            [this, id] {
                audioDeviceId_ = id;
                if (audio_) audio_->setDevice(id);
            },
            nullptr, nullptr, "", [this, id] { return audioDeviceId_ == id; });
    }

    // Audio scrubbing: a playback behaviour rather than a device choice, so it sits
    // below the device list under its own rule. Mirrors the Settings panel's Audio
    // group — same member, same writePrefs() — so the two rows always agree.
    menuBar_.addSeparator(outputMenuIdx_);
    menuBar_.addItem(outputMenuIdx_, "  Audio Scrubbing",
        [this] { audioScrub_ = !audioScrub_; writePrefs(); },
        nullptr, nullptr, "", [this] { return audioScrub_; });
}

void App::enumerateAudioDevices() {
    audioDevices_.clear();
    int count = 0;
    SDL_AudioDeviceID* ids = SDL_GetAudioPlaybackDevices(&count);
    if (!ids)
        return;
    for (int i = 0; i < count; ++i) {
        const char* nm = SDL_GetAudioDeviceName(ids[i]);
        audioDevices_.push_back({ ids[i], nm ? nm : "Audio device" });
    }
    SDL_free(ids);
}

void App::updateOutputMenu() {
    if (outputMenuIdx_ < 0 || menuBar_.isOpen())
        return; // never rebuild items out from under an open menu
    // Signature over the item SET only (backends + availability + canRead +
    // connected displays). The active-selection mark is live via `checked`, so
    // it must NOT enter the signature or the menu would rebuild every frame.
    size_t sig = 1469598103934665603ull;
    auto mix = [&sig](size_t v) { sig = (sig ^ v) * 1099511628211ull; };
    for (const auto& b : output_.backends()) {
        mix(std::hash<std::string>{}(b.id));
        mix(b.available ? 1u : 2u);
    }
    mix(output_.canRead() ? 3u : 4u);
    int dispCount = 0;
    SDL_DisplayID* disps = SDL_GetDisplays(&dispCount);
    mix((size_t)dispCount);
    for (int i = 0; i < dispCount; ++i) {
        mix((size_t)disps[i]);
        SDL_Rect b{};
        SDL_GetDisplayBounds(disps[i], &b);
        mix((size_t)b.w * 8192u + (size_t)b.h);
    }
    SDL_free(disps);
    if (sig == outputMenuSig_)
        return;
    outputMenuSig_ = sig;
    populateOutputMenu();
}

void App::populateOcioViewSubmenu() {
    if (ocioViewSubmenuIdx_ < 0) return;
    menuBar_.clearItems(ocioViewSubmenuIdx_);
    for (const auto& v : ocio_.views(ocio_.activeDisplay())) {
        menuBar_.addItem(ocioViewSubmenuIdx_, "  " + v,
            [this, v] {
                ocio_.setView(v);
                timeline_.ocioView = v; // persisted per-project on save
                hasTexture_ = false;
            },
            nullptr, nullptr, "", [this, v] { return ocio_.activeView() == v; });
    }
}

SDL_HitTestResult SDLCALL App::hitTest(SDL_Window* win, const SDL_Point* area, void* data) {
    App* self = static_cast<App*>(data);

    // The hit-test point is in window coordinates; our layout is in logical units.
    // Convert so high-DPI windows resize/drag correctly. SDL can call this while the
    // window is being created, before the renderer exists.
    int ww = 0, wh = 0;
    SDL_GetWindowSize(win, &ww, &wh); // window coordinates, same space as `area`
    float x = (float)area->x, y = (float)area->y;
    float w = (float)ww, h = (float)wh;
    if (self->renderer_) {
        SDL_RenderCoordinatesFromWindow(self->renderer_, x, y, &x, &y);
        SDL_RenderCoordinatesFromWindow(self->renderer_, w, h, &w, &h);
    }

    // Cinema mode: the window is fullscreen and carries no chrome, so there is
    // nothing to drag or resize by — every point is client area over the image.
    if (self->cinemaMode_)
        return SDL_HITTEST_NORMAL;

    // Resize edges take priority at the very border, even over the control
    // buttons — otherwise the top-right resize corner is swallowed by the close
    // button (this matches native Windows edge-resize behavior).
    const float border = 6.0f * dpiScale; // resize margin, in logical units
    if ((SDL_GetWindowFlags(win) & SDL_WINDOW_MAXIMIZED) == 0) {
        bool left = x < border, right = x > w - border;
        bool top = y < border, bottom = y > h - border;
        if (top && left) return SDL_HITTEST_RESIZE_TOPLEFT;
        if (top && right) return SDL_HITTEST_RESIZE_TOPRIGHT;
        if (bottom && left) return SDL_HITTEST_RESIZE_BOTTOMLEFT;
        if (bottom && right) return SDL_HITTEST_RESIZE_BOTTOMRIGHT;
        if (left) return SDL_HITTEST_RESIZE_LEFT;
        if (right) return SDL_HITTEST_RESIZE_RIGHT;
        if (top) return SDL_HITTEST_RESIZE_TOP;
        if (bottom) return SDL_HITTEST_RESIZE_BOTTOM;
    }

    // Control buttons, the app icon and menu titles stay clickable everywhere
    // else (a DRAGGABLE region would swallow the press into a window drag).
    const SDL_FRect icon = self->appIconRect();
    if (self->titleBar_.pointOverButton(x, y) || self->menuBar_.pointOverTitle(x, y) ||
        (x >= icon.x && x < icon.x + icon.w && y >= icon.y && y < icon.y + icon.h))
        return SDL_HITTEST_NORMAL;

    // The title bar's empty area is a drag handle; so is the top toolbar's empty
    // area (anything not over its sequence / OCIO / letterbox buttons).
    bool draggable = y < self->titleBar_.height();
    if (!draggable && self->topBarRect_.h > 0.0f &&
        y >= self->topBarRect_.y && y < self->topBarRect_.y + self->topBarRect_.h &&
        x >= self->topBarRect_.x && x < self->topBarRect_.x + self->topBarRect_.w) {
        auto over = [&](const SDL_FRect& r) {
            return x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h;
        };
        draggable = !over(self->sequenceBtnRect_) && !over(self->letterboxBtnRect_) &&
                    !over(self->ocioDisplayBtnRect_) && !over(self->ocioViewBtnRect_) &&
                    !over(self->ocioLookBtnRect_) && !over(self->ocioInputCsBtnRect_) &&
                    !over(self->proxyBtnRect_) &&
                    !over(self->openProjBtnRect_) &&
                    !over(self->openSeqBtnRect_);
    }
    // Which strip this landed on decides whether a swallowed press counts toward
    // the double-click-to-maximize gesture: the title bar's, not the toolbar's.
    self->titleDragHit_ = draggable && y < self->titleBar_.height();

    if (draggable) {
        // While a menu/popup is open, hand this press back as a client click
        // (NORMAL) so the app's outside-click handlers dismiss it. A press on a
        // DRAGGABLE (non-client) region is swallowed by the window drag and never
        // reaches the app, and detecting it inside the hit test is unreliable on
        // Wayland — so the empty bar only drags once nothing is open.
        if (self->menuBar_.isOpen() || self->anyTopBarPopupOpen() ||
            self->anyContextMenuOpen())
            return SDL_HITTEST_NORMAL;
        return SDL_HITTEST_DRAGGABLE;
    }

    return SDL_HITTEST_NORMAL;
}

void App::setStatus(const std::string& msg, Uint64 durationMs) {
    status_ = msg;
    statusUntil_ = SDL_GetTicks() + durationMs;
    statusWarn_ = false;
}

void App::setStatusWarn(const std::string& msg, Uint64 durationMs) {
    setStatus(msg, durationMs);
    statusWarn_ = true;
}

// ---------------------------------------------------------------- events

void App::onKeyDown(const SDL_KeyboardEvent& k) {
    const bool ctrl = (k.mod & SDL_KMOD_CTRL) != 0;
    const bool shift = (k.mod & SDL_KMOD_SHIFT) != 0;
    const bool alt = (k.mod & SDL_KMOD_ALT) != 0;

    // Spectator in a sync session: the host drives transport, so swallow the
    // playback / navigation keys (other shortcuts still work locally).
    if (transportLocked()) {
        switch (k.key) {
        case SDLK_SPACE:
            spectatorLocked("CONTROL PLAYBACK");
            return;
        case SDLK_UP: case SDLK_DOWN:
            // Only the Ctrl form moves the playhead now; the plain arrows drive
            // whichever left panel is open, which a spectator is free to use.
            if (!ctrl)
                break;
            spectatorLocked("CHANGE FRAME");
            return;
        case SDLK_LEFT: case SDLK_RIGHT:
        case SDLK_HOME: case SDLK_END:
        case SDLK_PAGEUP: case SDLK_PAGEDOWN:
        // F1 too: a source view parks the playhead on a matched frame, which the
        // host's next event would pull straight back out from under the view.
        case SDLK_F1:
            spectatorLocked("CHANGE FRAME");
            return;
        case SDLK_E: case SDLK_COMMA: case SDLK_PERIOD:
            spectatorLocked("CHANGE EXPOSURE");
            return;
        default: break;
        }
    }

    switch (k.key) {
    case SDLK_TAB:
        // Collapse the timeline to its ruler, or put the tracks back. The Overview
        // this key used to toggle keeps F3 (and its View row).
        setCompactTimeline(!compactTimeline_);
        break;
    case SDLK_BACKSPACE:
        // Back to the previous sequence / project scope, or out of a source view.
        // Inline edits (renames, filter fields) consume their own keys in
        // handleEvent, so this only fires when nothing is being typed into.
        if (ctrl || alt)
            break;
        goBackView();
        break;
    // The F row picks what the player shows, each key naming one destination
    // rather than toggling: pressing the one you are already on is a no-op, so a
    // hand on F3 can't fall out of the Overview. Tab and L stay toggles.
    case SDLK_F1:
        // Match-frame into the source of the clip under the playhead. The Frame
        // stage comes with it: a source view is one clip, which the Overview and
        // Layout have nothing to lay out.
        setPlayerStage(PlayerStage::Frame);
        openSourceViewAtPlayhead();
        break;
    case SDLK_F2:
        // Also the way out of a scratch view: from a source or a layout, the
        // timeline it was opened from is the destination, so drop the view the way
        // Backspace would (restoring its playhead and zoom) before naming the stage.
        dropScratchView();
        setPlayerStage(PlayerStage::Frame);
        break;
    case SDLK_F3:
        setPlayerStage(PlayerStage::Overview);
        break;
    case SDLK_F4:
        setPlayerStage(PlayerStage::Layout);
        break;
    case SDLK_F5:
        // The same comparison set as F4, stacked instead of tiled: from the Layout
        // it is a change of presentation, from anywhere else it stands the set up.
        setPlayerStage(PlayerStage::Stack);
        break;
    case SDLK_F11:
        setCinemaMode(!cinemaMode_);
        break;
    case SDLK_SPACE:
        togglePlay();
        break;
    case SDLK_LEFT:
        playing_ = false;
        setPlayhead(timeline_.playhead - (shift ? 10 : 1));
        break;
    case SDLK_RIGHT:
        playing_ = false;
        setPlayhead(timeline_.playhead + (shift ? 10 : 1));
        break;
    case SDLK_HOME:
        setPlayhead(timeline_.inPoint);
        break;
    case SDLK_END:
        setPlayhead(timeline_.effectiveOut());
        break;
    case SDLK_UP:
    case SDLK_DOWN: {
        // Ctrl walks the clip boundaries — what the bare arrows used to do. On
        // their own they belong to the Stack stage, or to the open left panel's
        // list: the SOURCES bin, or the Clip Source version picker. With none of
        // those showing they do nothing rather than falling back to the playhead,
        // so the key means one thing at a time. The stage wins over the panels: it
        // is the thing being looked at, and the panels stay reachable by mouse.
        const int dir = k.key == SDLK_UP ? -1 : 1;
        if (ctrl)
            jumpClip(dir);
        else if (stackView())
            cycleStack(dir);
        else if (projectExplorerOpen_ && peActiveTab_ == PeTabSources)
            stepBinSelection(dir);
        else if (clipSourceOpen_ && !k.repeat)
            // No repeat: each step is a real media swap behind a Python query, and
            // a held key would queue one per tick.
            stepClipSourceVersion(dir);
        break;
    }
    case SDLK_PAGEUP:
    case SDLK_PAGEDOWN: {
        // The page keys review clip by clip: the jump marks the clip it lands on
        // as the playback range (as X does), and Shift widens that range a clip at
        // a time on each side, PgUp out and PgDn back in. Up is forward here — the
        // page keys walk the program the way it plays, not the way the timeline is
        // drawn. The arrows above stay plain navigation, no range change.
        const int dir = k.key == SDLK_PAGEUP ? 1 : -1;
        if (shift) {
            expandClipRange(dir);
            break;
        }
        // From no range at all, the first press marks the clip already under the
        // playhead rather than moving off it: starting a clip-by-clip review keeps
        // you where you are, and the press after that steps on.
        const Clip* cur = (timeline_.inPoint == 0 && timeline_.outPoint < 0)
                              ? getTopMostClipAtFrame(timeline_.playhead)
                              : nullptr;
        if (cur)
            markClipRange(*cur);
        else
            jumpClip(dir, /*markRange=*/true);
        break;
    }
    case SDLK_H:
        helpOpen_ = !helpOpen_; // toggle the keyboard-shortcuts overlay
        aboutOpen_ = false;     // the two overlays share the centre of the window
        break;
    case SDLK_I:
        inspectorOpen_ = !inspectorOpen_;
        break;
    case SDLK_P:
        pixelInspectorOpen_ = !pixelInspectorOpen_;
        break;
    case SDLK_COMMA:
    case SDLK_PERIOD:
        // Exposure in thirds of a stop, whole stops with Shift. Held-key repeat
        // walks it, which is the point.
        if (ctrl || alt)
            break;
        exposureNudge((k.key == SDLK_COMMA ? -1.0f : 1.0f) * (shift ? 1.0f : 1.0f / 3.0f));
        break;
    case SDLK_E:
        // E on its own has no immediate effect: held it turns a left-drag over the
        // frame into the exposure/gamma virtual slider, and released without one it
        // toggles the bypass (see onKeyUp). Repeats are the key still being held.
        if (ctrl || alt || k.repeat)
            break;
        expHeld_ = true;
        expScrubbed_ = false;
        break;
    case SDLK_R:
    case SDLK_G:
    case SDLK_B: {
        // Isolate one colour channel (shown as grayscale). These share the
        // tech-check mode slot, so the active one pressed again returns to RGB.
        if (ctrl)
            break;
        TechMode m = k.key == SDLK_R ? TechMode::Red
                   : k.key == SDLK_G ? TechMode::Green
                                     : TechMode::Blue;
        const char* name = k.key == SDLK_R ? "RED" : (k.key == SDLK_G ? "GREEN" : "BLUE");
        setTechMode(m);
        setStatus(techMode_ == m ? std::string(name) + " CHANNEL" : "RGB");
        break;
    }
    case SDLK_LEFTBRACKET: // set / clear in point
        if (shift) {
            timeline_.inPoint = 0;
            setStatus("IN POINT CLEARED");
        } else {
            timeline_.inPoint = timeline_.playhead;
            if (timeline_.outPoint >= 0 && timeline_.outPoint < timeline_.inPoint)
                timeline_.outPoint = -1;
            setStatus("IN POINT: " + std::to_string(timeline_.inPoint));
        }
        break;
    case SDLK_RIGHTBRACKET: // set / clear out point
        if (shift) {
            timeline_.outPoint = -1;
            setStatus("OUT POINT CLEARED");
        } else {
            timeline_.outPoint = timeline_.playhead;
            if (timeline_.inPoint > timeline_.outPoint)
                timeline_.inPoint = 0;
            setStatus("OUT POINT: " + std::to_string(timeline_.outPoint));
        }
        break;
    case SDLK_X:
        // "Mark clip": playback range = the clip under the playhead. Marking is
        // idempotent, the way every NLE has it: Shift+X clears (as backslash
        // does), so pressing X twice re-marks rather than unmarking.
        if (!shift) {
            // The same clip Shift+F focuses: topmost visible under the playhead,
            // with the selection as the fallback for a playhead sitting in a gap.
            const Clip* c = getTopMostClipAtFrame(timeline_.playhead);
            if (!c)
                c = timeline_.findClipById(selectedClipId_);
            if (!c) {
                setStatus("NO CLIP TO MARK", 2000);
                break;
            }
            // The playhead is pulled into the range there (the gap fallback above
            // is the case that can land outside it).
            markClipRange(*c);
            break;
        }
        [[fallthrough]];
    case SDLK_BACKSLASH: // clear the in/out range back to the full timeline
        if (timeline_.inPoint != 0 || timeline_.outPoint >= 0) {
            timeline_.inPoint = 0;
            timeline_.outPoint = -1;
            setStatus("IN/OUT CLEARED");
        }
        break;
    case SDLK_O:
        if (ctrl)
            openProjectDialog();
        break;
    case SDLK_ESCAPE:
        if (cinemaMode_) {
            setCinemaMode(false);
            break;
        }
        if (curveMode()) {
            exitCurveMode();
            break;
        }
        break;
    case SDLK_Q:
        requestQuit();
        break;
    case SDLK_S:
        if (ctrl && shift)
            saveProjectDialog();
        else if (ctrl)
            saveProjectQuick();
        else {
            snapPlayhead_ = !snapPlayhead_; // same toggle as the magnet button
            writePrefs();
            setStatus(snapPlayhead_ ? "SNAP ON" : "SNAP OFF");
        }
        break;
    case SDLK_N:
        // Ctrl-qualified: unmodified N used to clear the whole timeline on a
        // stray keypress, with no confirmation and no undo.
        if (ctrl)
            requestNewProject();
        break;
    case SDLK_W:
        if (ctrl)
            requestCloseProject();
        break;
    case SDLK_F: {
        // Alt+F fits the sequence under the playhead: the scope between the whole
        // timeline (F) and the clip (Shift+F). With one sequence in view it would
        // land exactly where F does, so it says so rather than doing nothing.
        if (alt) {
            int64_t a = 0, b = 0;
            int si = -1;
            if (!playheadSeqRange(a, b, si)) {
                setStatus("NO SEQUENCE TO FIT", 2000);
                break;
            }
            fitRange(a, b);
            // Remembered like the sequence bar's double-click, so a following
            // double-click on that span reads as "already fit" and zooms back out.
            zoomFitSeqIdx_ = si;
            zoomFitShotId_ = -1;
            break;
        }
        // Shift+F zooms in on the clip under the playhead, wherever the cursor is:
        // on a crowded timeline the clip being reviewed is rarely the selected one,
        // so the playhead decides and selection is only the fallback.
        if (shift) {
            // An in/out range is an explicit statement of what is being reviewed,
            // so it outranks the clip under the playhead.
            if (timeline_.inPoint != 0 || timeline_.outPoint >= 0) {
                fitRange(timeline_.inPoint, timeline_.effectiveOut() + 1);
                zoomFitSeqIdx_ = zoomFitShotId_ = -1;
                break;
            }
            const Clip* c = getTopMostClipAtFrame(timeline_.playhead);
            if (!c)
                c = timeline_.findClipById(selectedClipId_);
            if (!c) {
                setStatus("NO CLIP TO FIT", 2000);
                break;
            }
            fitRange(c->timelineStart, c->end());
            zoomFitSeqIdx_ = zoomFitShotId_ = -1;
            break;
        }
        // Unmodified F fits whichever view the cursor is over: the frame if it's
        // over the video, otherwise the timeline. It only ever fits — the same
        // press over the same panel always lands the same way, so it stays the way
        // back out from a Shift+F zoom.
        float mx = 0.0f, my = 0.0f;
        uiMouse(mx, my);
        if (inPlayerView(mx, my)) {
            if (transportLocked()) { spectatorLocked("ZOOM/PAN"); break; } // host drives the frame view
            fitFrameView();
        } else {
            fitToFilteredSequence(); // fit the viewed sequence, or All when unfiltered
            zoomFitSeqIdx_ = zoomFitShotId_ = -1;
        }
        break;
    }
    case SDLK_1:
    case SDLK_2:
    case SDLK_3:
    case SDLK_4: {
        // Frame at exactly n:1 (n device pixels per image pixel), anchored on the
        // cursor when it's over the video and on the player center otherwise.
        if (transportLocked()) { spectatorLocked("ZOOM/PAN"); break; } // host drives the frame view
        float mx = 0.0f, my = 0.0f;
        uiMouse(mx, my);
        if (!inPlayerView(mx, my)) {
            mx = playerRect_.x + playerRect_.w * 0.5f;
            my = playerRect_.y + playerRect_.h * 0.5f;
        }
        zoomFramePixelScale(mx, my, (int)(k.key - SDLK_1) + 1);
        break;
    }
    case SDLK_DELETE:
        // A source row and a timeline clip can both be selected; whichever the user
        // touched last owns the key (binSelectionActive_). The bin only claims it
        // while its panel is open, so a selection left behind a closed panel cannot
        // swallow the keypress.
        if (binSelectionActive_ && projectExplorerOpen_ && !selectedSourcePaths_.empty())
            removeSelectedSource(/*alwaysConfirm=*/true);
        else if (selectedTransitionId_ >= 0)
            deleteSelectedTransition();
        else if (selectedGapTrack_ >= 0)
            deleteSelectedGap();
        else
            deleteSelectedClip();
        break;
    case SDLK_D: { // disable/enable a clip (hidden clips let a lower track show through)
        // Ctrl+D is Premiere's "apply default transition": a dissolve on the cut
        // nearest the playhead.
        if (ctrl) {
            addDissolveAtPlayhead();
            break;
        }
        // Prefer the selected clip; with none selected, act on the topmost clip
        // under the playhead.
        int id = selectedClipId_;
        if (id < 0)
            // Include hidden clips so the fallback can re-enable a disabled one
            // sitting on the top track under the playhead.
            if (const Clip* top = getTopMostClipAtFrame(timeline_.playhead, /*includeHidden=*/true))
                id = top->id;
        if (Clip* c = timeline_.findClipById(id)) {
            c->hidden = !c->hidden;
        }
        break;
    }
    case SDLK_C:
        if (ctrl) copySelectedClips();
        // Razor tool, Premiere's binding - but not while compact, where the tool
        // buttons are hidden and there are no track rows to cut in.
        else if (!compactTimeline_) setTimelineTool(TimelineTool::Razor);
        break;
    case SDLK_V:
        if (ctrl) pasteClips();
        // Bare V selects the cursor tool (Premiere's binding); the sequence
        // view-filter popup it used to open moved onto Shift+V, which was already
        // opening it too - the case did not test shift.
        else if (shift) openSequenceMenu();
        else setTimelineTool(TimelineTool::Cursor);
        break;
    case SDLK_Z:
        if (ctrl && shift) redo();
        else if (ctrl) undo();
        break;
    case SDLK_Y:
        if (ctrl) redo();
        break;
    default:
        break;
    }
}

// Only E is watched on release: it is the one key whose meaning depends on what
// happened while it was down. A hold that drove a scrub has already had its
// effect, so the tap case — press and release with no drag — is the bypass toggle.
void App::onKeyUp(const SDL_KeyboardEvent& k) {
    if (k.key != SDLK_E)
        return;
    const bool wasHeld = expHeld_;
    expHeld_ = false;
    expDragging_ = false;
    if (wasHeld && !expScrubbed_)
        exposureToggleBypass();
    expScrubbed_ = false;
}

// Longest gap between the two presses of a title-bar double-click, and the floor
// below which two of them are one press reported twice rather than two clicks.
static constexpr Uint64 kTitleDoubleClickMs = 400;
static constexpr Uint64 kTitlePressDedupMs = 40;

// The resize-handle hit zone straddles each open side panel's right edge,
// kOverviewHandleHitW/2 into either side, and runs the panel's full height.
App::PanelEdge App::panelResizeEdgeAt(float mx, float my) const {
    if (my < titleBar_.height() || my >= panelsBottom_)
        return PanelEdge::None;
    const float half = kOverviewHandleHitW * 0.5f;
    if (projectExplorerOpen_ && std::abs(mx - (kSidePanelW + peW_)) <= half)
        return PanelEdge::ProjectExplorer;
    if (clipSourceOpen_ && std::abs(mx - (kSidePanelW + clipSourceW_)) <= half)
        return PanelEdge::ClipSource;
    return PanelEdge::None;
}

void App::handleEvent(SDL_Event& e) {
    // A monitor being unplugged: drop the review target if it was that display,
    // so syncReviewWindow tears the window down instead of leaving it orphaned.
    if (e.type == SDL_EVENT_DISPLAY_REMOVED && e.display.displayID == reviewDisplay_) {
        reviewDisplay_ = 0;
        return;
    }
    // Events targeted at the review window: it is a passive display, so ignore its
    // input; honour only a close request (treat it as turning the feature off).
    if (reviewWindow_ && SDL_GetWindowFromEvent(&e) == reviewWindow_) {
        if (e.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) {
            reviewEnabled_ = false;
            closeReviewWindow();
        }
        return;
    }

    // The window moved to a display with a different DPI (or that display's scale
    // was changed): re-rasterize the fonts for it. Skipped when the UI Scale
    // preference pins the scale. The layout itself needs nothing — it is in logical
    // units.
    if (e.type == SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED && uiScalePref_ <= 0.0f)
        setUiScale(SDL_GetWindowDisplayScale(window_));

#ifndef _WIN32
    // Double-click the title bar's empty area to maximize / restore. That area is
    // DRAGGABLE, so the press is swallowed by the window drag and never reaches
    // us as a mouse event; its only trace is SDL_EVENT_WINDOW_HIT_TEST, which the
    // X11 backend emits per swallowed press. Two of those inside the double-click
    // window are the gesture. A press that turned into a real drag moves the
    // window, which drops the pending first click.
    //
    // The dedup floor is not paranoia: XInput2 reports one physical click twice,
    // once for the master pointer and once for the device itself, and SDL runs
    // the hit-test branch ahead of the duplicate filtering it applies to ordinary
    // mouse events. The two land in the same event batch, far closer together
    // than a human can click twice.
    //
    // Windows is excluded on purpose: there the OS already maximizes on a caption
    // double-click, and it fires this event for plain hovering too, which would
    // maximize by accident.
    if (e.type == SDL_EVENT_WINDOW_HIT_TEST && titleDragHit_) {
        const Uint64 now = SDL_GetTicks();
        const Uint64 since = now - titleDragClickMs_;
        if (titleDragClickMs_ && since < kTitlePressDedupMs) {
            // One press, reported twice — not a click of its own.
        } else if (titleDragClickMs_ && since <= kTitleDoubleClickMs) {
            titleBar_.toggleMaximize();
            titleDragClickMs_ = now; // absorbs this press's duplicate
        } else {
            titleDragClickMs_ = now;
        }
        return;
    }
    if (e.type == SDL_EVENT_WINDOW_MOVED)
        titleDragClickMs_ = 0;
#endif

    // Converts drop coordinates into the logical space as well, so they must not be
    // run through SDL_RenderCoordinatesFromWindow again: a second pass would divide
    // by the UI scale twice and pull every drop point toward the top-left.
    SDL_ConvertEventToRenderCoordinates(renderer_, &e);

    bool wantQuit = false;
    if (titleBar_.handleEvent(e, &wantQuit)) {
        if (wantQuit) requestQuit();
        return;
    }
    // Message dialog is the top-most modal: it blocks everything below it.
    if (msgDialogOpen_) {
        handleDialogEvent(e);
        return;
    }
    // Export dialog blocks all other input while open.
    if (exportDialog_.isOpen()) {
        exportDialog_.handleEvent(e, window_);
        return;
    }
    // Missing Source modal blocks all other input while open.
    if (relocateModalOpen_) {
        handleRelocateModalEvent(e);
        return;
    }
    // Once visible (task ran past the show delay), the dialog is modal: only
    // Cancel/Esc get through. Before then the app stays interactive.
    if (progressVisible()) {
        handleProgressEvent(e);
        return;
    }
    // Title-bar app icon: its window menu, and the icon's own click/double-click.
    if (appIconHandleEvent(e))
        return;
    // The clip right-click popup is top-most: let it consume events first (it
    // only claims them while open).
    if (clipMenu_.handleEvent(e))
        return;
    if (clipToolboxMenu_.handleEvent(e))
        return;
    // The zoom-fit button drops its menu on hover, so the popup sits above a still
    // live button: this opens/closes it with the cursor and gets a press on the
    // button out from under the popup, both before fitMenu_ sees the event.
    fitHoverEvent(e);
    if (fitMenu_.handleEvent(e))
        return;
    if (trackMenu_.handleEvent(e))
        return;
    if (peSortMenu_.handleEvent(e))
        return;
    if (peMediaMenu_.handleEvent(e))
        return;
    if (peSeqColorMenu_.handleEvent(e))
        return;
    if (menuBar_.handleEvent(e)) {
        // Opening/interacting with the menu bar dismisses the top-toolbar popups
        // (OCIO, sequence, letterbox) so they don't linger under the dropdown.
        if (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN)
            closeTopBarPopups();
        return;
    }
    // Sequence view-filter popup: consumes events while open.
    if (sequenceMenuOpen_ && sequenceMenuHandleEvent(e))
        return;
    // Proxy popup: consumes events while open.
    if (proxyMenuOpen_ && proxyMenuHandleEvent(e))
        return;
    // OCIO Display popup: consumes events while open.
    if (ocioDisplayMenuOpen_ && ocioDisplayMenuHandleEvent(e))
        return;
    // OCIO View Transform popup: consumes events while open.
    if (ocioViewMenuOpen_ && ocioViewMenuHandleEvent(e))
        return;
    // OCIO Look popup: consumes events while open.
    if (ocioLookMenuOpen_ && ocioLookMenuHandleEvent(e))
        return;
    // OCIO File Colorspace popup: consumes events while open.
    if (ocioInputCsMenuOpen_ && ocioInputCsMenuHandleEvent(e))
        return;
    // Letterbox popup (aspect-ratio / opacity): consumes events while open.
    if (letterboxMenuOpen_ && letterboxMenuHandleEvent(e))
        return;
    if (settingsOpen_ && settingsFpsHandleEvent(e))
        return;
    if (settingsOpen_ && settingsUiScaleHandleEvent(e))
        return;
    if (settingsOpen_ && settingsCacheHandleEvent(e))
        return;
    if (techOpen_ && techNitHandleEvent(e))
        return;
    if (projectExplorerOpen_ && projectTreeHandleEvent(e))
        return;
    // Inline track rename (timeline gutter): captures typing while it is open. The
    // click that ends it is not consumed, so it still lands where it was aimed.
    if (trackNameEdit_ >= 0 && trackNameEditHandleEvent(e))
        return;
    if (sessionPanelOpen_ && sessionHandleEvent(e))
        return;
    switch (e.type) {
    case SDL_EVENT_QUIT:
        requestQuit();
        break;
    case SDL_EVENT_WINDOW_FOCUS_GAINED:
        // Stamped so the click that re-activated the window can be told apart from
        // a click made while we already had focus (see playerClickActivating_).
        if (e.window.windowID == SDL_GetWindowID(window_))
            focusGainedMs_ = SDL_GetTicks();
        break;
    case SDL_EVENT_WINDOW_FOCUS_LOST:
        // A key released while another window has focus never reaches us, so let go
        // of the exposure hold here rather than leaving E latched down.
        if (e.window.windowID == SDL_GetWindowID(window_))
            expHeld_ = false;
        break;
    case SDL_EVENT_WINDOW_DISPLAY_CHANGED:
        // The main window moved to another display. Match the swapchain composition to
        // the new display: recreate as SDR when it can't present the HDR composition, or
        // re-enable HDR (scRGB) when it can. SDL's "gpu" renderer fixes its composition
        // at creation, so switching means a renderer rebuild (see rebuildRenderer_).
        if (hdrPipeline_ && gpuDevice_ &&
            e.window.windowID == SDL_GetWindowID(window_)) {
            // An external sink owns HDR while it runs, so the main window stays sRGB
            // no matter which display it lands on.
            bool want = windowCanPresentHdr_(window_, gpuDevice_) && !externalSinkActive();
            if (hdrActive_ != want)
                rebuildRenderer_(want);
        }
        break;
    case SDL_EVENT_WINDOW_LEAVE_FULLSCREEN:
        // The window can be pulled out of fullscreen without us asking (a window
        // manager shortcut, a display going away). Cinema mode leaves with it rather
        // than stranding a windowed jplay with no chrome and no way back to it.
        if (e.window.windowID == SDL_GetWindowID(window_))
            setCinemaMode(false);
        break;
    case SDL_EVENT_KEY_DOWN:
        onKeyDown(e.key);
        break;
    case SDL_EVENT_KEY_UP:
        onKeyUp(e.key);
        break;
    case SDL_EVENT_DROP_BEGIN:
        dropFirstFile_ = true; // start a new drop batch
        playerDropOnFrame_ = false;
        playerDropAction_ = -1;
        break;
    case SDL_EVENT_DROP_POSITION:
        updateFileHover(e.drop.x, e.drop.y); // SDL gives position but not the filename yet
        break;
    case SDL_EVENT_DROP_COMPLETE:
        fileHoverActive_ = false;
        playerDropActive_ = false;
        playerDropHover_ = -1;
        break;
    case SDL_EVENT_DROP_TEXT:
        // Not a file but a payload only Python can make sense of — a row dragged
        // out of a web app carries a URL naming it, not a path. onDropText hands
        // it to the site's resolver and brings in whatever media comes back.
        if (e.drop.data)
            onDropText(e.drop.data, e.drop.x, e.drop.y);
        fileHoverActive_ = false;
        playerDropActive_ = false;
        playerDropHover_ = -1;
        break;
    case SDL_EVENT_DROP_FILE:
        if (e.drop.data) {
            std::string path = e.drop.data;
            // A dropped project document replaces the open one, so it goes
            // through the same unsaved-changes gate as File > Open.
            if (hasExtension(path, ".jpproj")) {
                confirmDiscard("Open Project", [this, path] { loadProject(path); });
            } else if (hasExtension(path, ".otio")) {
                confirmDiscard("Import OTIO", [this, path] { loadOtio(path); });
            } else {
                // First item of the batch fixes the target from the drop point —
                // the chooser box over the frame, else a track/frame that
                // subsequent items (and folder contents) lay end-to-end after.
                const bool first = dropFirstFile_;
                if (dropFirstFile_) {
                    playerDropOnFrame_ = inPlayerView(e.drop.x, e.drop.y)
                                      && playerDropChooserApplies();
                    playerDropAction_ = playerDropBoxAt(e.drop.x, e.drop.y);
                    resolveDropTarget(e.drop.x, e.drop.y, dropInsertTrack_,
                                      dropInsertFrame_);
                    dropFirstFile_ = false;
                }
                std::error_code ec;
                const bool dir = fs::is_directory(path, ec);
                if (playerDropOnFrame_ && !dir) {
                    // A folder is not a single clip, so it falls through to the
                    // normal add below even when dropped on the chooser. Released
                    // between the boxes, the drop falls back to the source view.
                    applyPlayerDrop(playerDropAction_ >= 0 ? playerDropAction_ : 0,
                                    path, first);
                } else if (dir) {
                    addMediaFolder(path);
                } else {
                    int64_t end = addMediaFileAt(path, dropInsertTrack_,
                                                 dropInsertFrame_);
                    if (end >= 0)
                        dropInsertFrame_ = end; // advance for the next item
                }
            }
        }
        fileHoverActive_ = false;
        playerDropActive_ = false;
        playerDropHover_ = -1;
        break;
    case SDL_EVENT_MOUSE_BUTTON_DOWN: {
        float mx = e.button.x, my = e.button.y;
        // Shortcuts / About overlays: while one is open, any click anywhere
        // dismisses it and is consumed (so it can't also act on whatever is
        // underneath).
        if (helpOpen_ || aboutOpen_) {
            helpOpen_ = false;
            aboutOpen_ = false;
            break;
        }
        // Resize handles: intercept before the boundary check so the hit zone
        // straddles the edge.
        if (e.button.button == SDL_BUTTON_LEFT) {
            const PanelEdge edge = panelResizeEdgeAt(mx, my);
            if (edge == PanelEdge::ProjectExplorer) {
                peResizing_ = true;
                break;
            }
            if (edge == PanelEdge::ClipSource) {
                clipSourceResizing_ = true;
                break;
            }
            // The timeline's top edge, dragged to make the timeline shorter or
            // taller (the player and left panels above it follow, since their
            // heights are derived from the timeline's). A double-click hands the
            // height back to the automatic one.
            if (overTimelineEdge(mx, my)) {
                if (e.button.clicks >= 2)
                    tlUserH_ = 0.0f;
                else
                    tlResizing_ = true;
                break;
            }
        }
        // The inspector overlay is informational: only its close (X) button takes a
        // click, everything else passes through to the frame beneath it.
        if (inspectorVisible() && e.button.button == SDL_BUTTON_LEFT
            && inRect(inspectorCloseRect_, mx, my)) {
            inspectorOpen_ = false;
            break;
        }
        // Left panels capture all clicks within their strip (below the title bar,
        // above the full-width timeline): directory toggle, ProjectExplorer +/-
        // buttons, and row selection.
        if (mx < panelsRight_ && my >= titleBar_.height() && my < panelsBottom_) {
            if (e.button.button == SDL_BUTTON_LEFT) {
                if (inRect(dirButtonRect_, mx, my)) {
                    gradeOpen_ = false;
                    techOpen_ = false;
                    settingsOpen_ = false;
                    pencilMode_ = false;
                    sessionPanelOpen_ = false;
                    closeClipSource();
                    projectExplorerOpen_ = !projectExplorerOpen_;
                    if (projectExplorerOpen_) refreshExplorerOrder(); // re-sort shots on open
                } else if (inRect(gradeButtonRect_, mx, my)) {
                    if (gradeOpen_) {
                        gradeOpen_ = false;
                    } else {
                        projectExplorerOpen_ = false;
                        techOpen_ = false;
                        settingsOpen_ = false;
                        pencilMode_ = false;
                        sessionPanelOpen_ = false;
                        closeClipSource();
                        gradeOpen_ = true;
                    }
                } else if (inRect(techButtonRect_, mx, my)) {
                    if (techOpen_) {
                        techOpen_ = false;
                    } else {
                        projectExplorerOpen_ = false;
                        gradeOpen_ = false;
                        settingsOpen_ = false;
                        pencilMode_ = false;
                        sessionPanelOpen_ = false;
                        closeClipSource();
                        techOpen_ = true;
                    }
                } else if (inRect(clipSourceButtonRect_, mx, my)) {
                    if (clipSourceOpen_) {
                        closeClipSource();
                    } else {
                        projectExplorerOpen_ = false;
                        gradeOpen_ = false;
                        techOpen_ = false;
                        settingsOpen_ = false;
                        pencilMode_ = false;
                        sessionPanelOpen_ = false;
                        clipSourceOpen_ = true;
                        clipSourceDirty_ = true;  // describe on the next render
                        clipSourceScroll_ = 0.0f;
                    }
                } else if (inRect(settingsButtonRect_, mx, my)) {
                    if (settingsOpen_) {
                        settingsOpen_ = false;
                    } else {
                        projectExplorerOpen_ = false;
                        gradeOpen_ = false;
                        techOpen_ = false;
                        pencilMode_ = false;
                        sessionPanelOpen_ = false;
                        closeClipSource();
                        settingsOpen_ = true;
                    }
                } else if (inRect(pencilBtnRect_, mx, my)) {
                    if (pencilMode_) {
                        pencilMode_ = false;
                    } else {
                        projectExplorerOpen_ = false;
                        gradeOpen_ = false;
                        techOpen_ = false;
                        settingsOpen_ = false;
                        sessionPanelOpen_ = false;
                        closeClipSource();
                        pencilMode_ = true; // freehand markup; opens the draw-tool pane
                    }
                } else if (inRect(sessionButtonRect_, mx, my)) {
                    // Mutually-exclusive left pane (like grade / tech / settings).
                    if (sessionPanelOpen_) {
                        sessionPanelOpen_ = false;
                        SDL_StopTextInput(window_);
                    } else {
                        projectExplorerOpen_ = false;
                        gradeOpen_ = false;
                        techOpen_ = false;
                        settingsOpen_ = false;
                        pencilMode_ = false;
                        closeClipSource();
                        sessionPanelOpen_ = true;
                    }
                } else if (clipSourceOpen_) {
                    clipSourceHandleEvent(e); // picker option rows
                } else if (pencilMode_) {
                    drawPanelHandlePress(mx, my); // Clear / size / hue widgets
                } else if (settingsOpen_) {
                    settingsHandleEvent(e); // Time Format toggle (FPS dropdown handled earlier)
                } else if (gradeOpen_) {
                    gradeHandleEvent(e); // panel-internal widgets
                } else if (techOpen_) {
                    techHandleEvent(e); // panel-internal mode pills
                } else if (projectExplorerOpen_ && inRect(sourceInfoCloseRect_, mx, my)) {
                    // X on the MEDIA info sub-panel: close it.
                    inspectMediaPath_.clear();
                    sourceInfoScroll_ = 0.0f;
                    sourceInfoRect_ = {};
                    sourceInfoCloseRect_ = {};
                } else if (projectExplorerOpen_ && inRect(peAddRect_, mx, my)) {
                    addMediaViaBrowser();
                } else if (projectExplorerOpen_ && inRect(peRemoveRect_, mx, my)) {
                    removeSelectedSource();
                } else if (projectExplorerOpen_) {
                    bool onRow = false;
                    for (int i = 0; i < (int)explorerRows_.size(); ++i)
                        if (inRect(explorerRows_[i].rect, mx, my)) {
                            // Double-click shows the source on its own in a
                            // throwaway source view, leaving the cut alone; the
                            // first click already selected the row.
                            if (e.button.clicks >= 2)
                                openSourceView(explorerRows_[i].path);
                            else
                                pressExplorerRow(i, SDL_GetModState());
                            onRow = true;
                            break;
                        }
                    // Empty space in the bin's scrolling band: drop the selection.
                    // Everything else in the panel (tabs, headers, scrollbar, the
                    // buttons above) was consumed before this point, so a press
                    // that reaches here inside the band really is on nothing.
                    if (!onRow && inRect(peSourceBand_, mx, my) &&
                        !selectedSourcePaths_.empty()) {
                        selectedSourcePaths_.clear();
                        selectionAnchorPath_.clear();
                    }
                }
            } else if (e.button.button == SDL_BUTTON_RIGHT && projectExplorerOpen_) {
                // Right-click a source row: copy path / filename, reveal in the
                // system file browser.
                for (int i = 0; i < (int)explorerRows_.size(); ++i)
                    if (inRect(explorerRows_[i].rect, mx, my)) {
                        openBinContextMenu(i, mx, my);
                        break;
                    }
            }
            break; // clicks in the panels never reach the player / timeline
        }
        // Empty player shows the launcher: clickable recent/shared/sync rows,
        // then the CREATE PROJECT buttons. Hit rects are absolute, so the order
        // tested here is independent of the on-screen column order.
        if (launcherVisible() && e.button.button == SDL_BUTTON_LEFT) {
            if (inRect(createEmptyBtn_, mx, my))   { requestNewProject(); break; }
            if (inRect(createFromDirBtn_, mx, my)) { createFromDirDialog(); break; }
            if (inRect(openProjectBtn_, mx, my))   { openProjectDialog(); break; }
            if (inRect(importMediaBtn_, mx, my))   { addMediaViaBrowser(); break; }
            // Tab strip over the left column: switch which list it shows. Each tab
            // keeps its own scroll, so coming back lands where it was left. A tab
            // whose scan has nothing yet is faded and swallows the click.
            bool switchedTab = false;
            for (const auto& tab : launcherTabs_)
                if (tab.rect.w > 0 && inRect(tab.rect, mx, my)) {
                    if (tab.enabled)
                        launcherTab_ = tab.tab;
                    switchedTab = true; // a faded tab still swallows the click
                    break;
                }
            if (switchedTab)
                break;
            // The active list's scrollbar takes the press ahead of its rows — it is
            // drawn over the strip the rows were laid out clear of, so nothing is
            // shadowed. Only the tab that drew a bar has one to hit.
            if (recentSb_.press(mx, my, recentScroll_, dpiScale))
                break;
            if (sharedSb_.press(mx, my, sharedScroll_, dpiScale))
                break;
            // SYNC SESSION tab: click a discovered host to join it.
            bool joinedSync = false;
            for (const auto& row : launcherSyncRows_)
                if (inRect(row.rect, mx, my)) {
                    joinHost(row.ip, row.port);
                    joinedSync = true;
                    break;
                }
            if (joinedSync)
                break;
            // SHARED PROJECTS tab: click a studio show to bring in its project.
            bool openedShared = false;
            for (const auto& sp : sharedProjects_)
                if (sp.rect.w > 0 && inRect(sp.rect, mx, my)) {
                    openSharedProject(sp.path);
                    openedShared = true;
                    break;
                }
            if (openedShared)
                break;
            bool handled = false;
            for (const auto& t : recent_) {
                if (t.rect.w <= 0)
                    continue;
                if (t.closeRect.w > 0 && inRect(t.closeRect, mx, my)) {
                    UserData::removeRecent(t.path); // drop from saved recents
                    recentDirty_ = true;            // rebuild tiles next render
                    handled = true;
                    break;
                }
                if (inRect(t.rect, mx, my)) {
                    openRecentProject(t.path);
                    handled = true;
                    break;
                }
            }
            if (handled)
                break;
        }
        // Transport buttons live in the info bar (above the ruler).
        if (e.button.button == SDL_BUTTON_LEFT) {
            if (inRect(magnetBtnRect_, mx, my)) {
                snapPlayhead_ = !snapPlayhead_;
                writePrefs();
                break;
            }
            // Tool radio pair: each button selects its mode rather than toggling,
            // so clicking the active one is a no-op. Not persisted (see
            // timelineTool_), hence no writePrefs here.
            if (inRect(cursorToolBtnRect_, mx, my)) { setTimelineTool(TimelineTool::Cursor); break; }
            if (inRect(razorToolBtnRect_, mx, my))  { setTimelineTool(TimelineTool::Razor);  break; }
            // The two "Open …" shortcuts: same actions as the Sequence popup's rows.
            if (inRect(openProjBtnRect_, mx, my)) {
                openResolvedProject(openProjPath_, openProjName_);
                break;
            }
            if (inRect(openSeqBtnRect_, mx, my)) { showInSequence(); break; }
            if (inRect(sequenceBtnRect_, mx, my)) { openSequenceMenu(); break; }
            if (inRect(ocioDisplayBtnRect_, mx, my)) { openOcioDisplayMenu(); break; }
            if (inRect(ocioViewBtnRect_, mx, my)) { openOcioViewMenu(); break; }
            if (inRect(ocioLookBtnRect_, mx, my)) { openOcioLookMenu(); break; }
            if (inRect(ocioInputCsBtnRect_, mx, my)) { openOcioInputCsMenu(); break; }
            if (inRect(proxyBtnRect_, mx, my)) { openProxyMenu(); break; }
            if (inRect(letterboxBtnRect_, mx, my)) { openLetterboxMenu(); break; }
            if (inRect(prevClipBtnRect_, mx, my)) { jumpClip(-1); break; }
            if (inRect(playBtnRect_, mx, my))     { togglePlay(); break; }
            if (inRect(nextClipBtnRect_, mx, my)) { jumpClip(1);  break; }
            if (inRect(volumeBtnRect_, mx, my))   { toggleMute(); break; }
            if (inRect(volumeSliderRect_, mx, my)) {
                volumeDragging_ = true;
                setVolume((mx - volumeSliderRect_.x) / volumeSliderRect_.w);
                break;
            }
            // Zoom-fit button, right of "Snap": hover-only — it drops the fit list
            // (see fitHoverEvent) and takes no click of its own. Its press is still
            // swallowed rather than falling through to whatever is behind it.
            if (inRect(fitBtnRect_, mx, my))
                break;
        }
        // Grid view: the scrollbar takes the press ahead of the tiles — it is drawn
        // over the strip the tiles were laid out clear of, so nothing is shadowed.
        if (e.button.button == SDL_BUTTON_LEFT && gridView() &&
            gridSb_.press(mx, my, gridScroll_, dpiScale))
            break;
        // Grid view: a left-click on a tile jumps the playhead to that clip (so it
        // becomes the live-playing tile); double-click also marks the playback range
        // to it and Shift+click extends the range to it (see gridClickClip). The
        // click never reaches scrub / pan.
        if (e.button.button == SDL_BUTTON_LEFT && gridView() && inPlayerView(mx, my)) {
            for (const auto& cell : gridCells_)
                if (inRect(cell.rect, mx, my)) {
                    if (const Clip* c = clipById(cell.clipId))
                        gridClickClip(*c, e.button.clicks >= 2,
                                      (SDL_GetModState() & SDL_KMOD_SHIFT) != 0);
                    break;
                }
            break;
        }
        // E held: the left button drives the exposure/gamma virtual slider instead
        // of the jog — and ahead of the pencil, so checking exposure doesn't mean
        // leaving the draw tool. The key going up ends it even if the button is
        // still down.
        if (e.button.button == SDL_BUTTON_LEFT && expHeld_ && !gridView()
            && !launcherVisible() && inPlayerView(mx, my)) {
            if (transportLocked()) { spectatorLocked("CHANGE EXPOSURE"); break; } // host drives the exposure
            expDragging_ = true;
            expScrubbed_ = true; // this hold is a scrub, so its release isn't a tap
            expScrubDx_ = expScrubDy_ = 0.0f;
            expScrubGain0_ = grade_.gain;
            expScrubGamma0_ = grade_.gamma;
            break;
        }
        // Pencil mode: left-press over the frame starts a freehand stroke.
        if (e.button.button == SDL_BUTTON_LEFT && pencilMode_ && inPlayerView(mx, my)
            && texW_ > 0 && texH_ > 0) {
            annotBeginStroke(mx, my);
            break;
        }
        // Left press over the frame arms both frame gestures: a drag jogs the
        // playhead (MOTION), a release in place toggles play/pause (BUTTON_UP).
        // Nothing happens yet — which gesture it is isn't known until the mouse
        // either moves or comes back up.
        if (e.button.button == SDL_BUTTON_LEFT && !gridView() && !launcherVisible()
            && inPlayerView(mx, my)) {
            if (transportLocked()) { spectatorLocked("CONTROL PLAYBACK"); break; } // host drives the transport
            playerClickArmed_ = true;
            // SDL_HINT_MOUSE_FOCUS_CLICKTHROUGH (see init) lets the click that raises
            // an unfocused window also reach the frame underneath, which otherwise
            // reads as click-to-play. Treat such a press as activation only: it still
            // arms the jog (a deliberate drag), but its release won't toggle playback.
            // The window may already carry INPUT_FOCUS by the time the press is
            // delivered, so a just-arrived FOCUS_GAINED counts too.
            playerClickActivating_ =
                !(SDL_GetWindowFlags(window_) & SDL_WINDOW_INPUT_FOCUS)
                || (SDL_GetTicks() - focusGainedMs_) < 200;
            playerScrubDx_ = 0.0f;
            break;
        }
        // Grid view: middle-drag scrolls the grid rather than panning the frame —
        // there is no zoomed frame behind the grid to pan.
        if (e.button.button == SDL_BUTTON_MIDDLE && gridView() && inPlayerView(mx, my)) {
            gridPanning_ = true;
            break;
        }
        // Middle-drag in the player area pans the (zoomed) frame.
        if (e.button.button == SDL_BUTTON_MIDDLE && inPlayerView(mx, my)) {
            if (transportLocked()) { spectatorLocked("ZOOM/PAN"); break; } // host drives the frame view
            framePanning_ = true;
            break;
        }
        // Right-click over the frame: the clip menu for the clip on screen, the
        // same one the timeline shows for that clip. It drops downward from the
        // cursor here — the frame's empty space is below the click, not above it.
        // Not in grid view, where the frame under the cursor isn't the playhead's
        // clip.
        if (e.button.button == SDL_BUTTON_RIGHT && !gridView() && inPlayerView(mx, my)) {
            if (const Clip* c = getTopMostClipAtFrame(timeline_.playhead))
                openClipRightClickMenu(*c, mx, my, /*growDown=*/true);
            break;
        }
        if (my < rulerRect_.y)
            break; // player / info bar — not interactive here
        if (e.button.button == SDL_BUTTON_MIDDLE) {
            panning_ = true;
            break;
        }
        if (e.button.button == SDL_BUTTON_RIGHT) {
            handleTimelineRightClick(mx, my);
            break;
        }
        if (e.button.button != SDL_BUTTON_LEFT)
            break;

        // The track stack's scrollbar lies over the right end of its rows, so it
        // gets the press first. False when there is no bar (the stack fits) or the
        // press missed it, leaving the click to the row underneath.
        if (trackSb_.press(mx, my, trackScroll_, dpiScale))
            break;

        // A click in the timeline leaves the media-bin selection and its info
        // sub-panel alone: moving onto another clip shouldn't take either away.
        // (The press already cancelled any pending reveal.)

        int n = std::max(trackCount(), 1);
        if (mx < headerX_) {
            // Left gutter: the row's burger, which drops its menu. The trailing
            // placeholder track has no button (see renderTimeline), so it isn't
            // clickable.
            bool onMenuBtn = false;
            for (int t = 0; t < n; ++t) {
                if (t == n - 1 && timeline_.trackEmpty(t)) continue;
                if (!inRect(trackMenuRect(t), mx, my)) continue;
                // On the row expanded into a curve lane the button is drawn as a
                // graph and means "leave", not "drop the row's menu".
                if (t == curveTrack_)
                    exitCurveMode();
                else
                    openTrackMenu(t);
                onMenuBtn = true;
                break;
            }
            // Anywhere else on a label arms a reorder drag, or on a double-click
            // opens the inline rename. Arming is only that -- not started: under
            // the motion threshold the press stays a plain click, which is what
            // keeps the cross above reachable.
            // Reordering is off while a row is expanded: the other rows have no
            // height to drop onto, and the stack is about to be restored anyway.
            // Both are off under a scratch view too, and for the same reason its
            // row menu is (see openTrackMenu): these rows are the view's, while a
            // rename or a reorder would land on the project's stack.
            if (!onMenuBtn && !curveMode() && !scratchActive())
                for (int t = 0; t < trackReorderRows(); ++t) {
                    if (!inRect(trackHeaderRect(t), mx, my)) continue;
                    if (e.button.clicks >= 2) {
                        beginTrackNameEdit(t); // also disarms this click's first press
                        break;
                    }
                    trackDragFrom_ = t;
                    trackDragPressY_ = my;
                    trackDragGrabDY_ = my - trackRowY(t);
                    trackDropIns_ = -1;
                    break;
                }
            break; // gutter clicks never scrub
        }
        if (seqBarRect_.h > 0 && inRect(seqBarRect_, mx, my)) {
            if (e.button.clicks < 2) {
                // Single click scrubs, just like the ruler.
                if (transportLocked()) { spectatorLocked("CHANGE FRAME"); break; } // host controls the playhead
                scrubbing_ = true;
                playing_ = false;
                setPlayhead(scrubFrame(mx));
                break;
            }
            // Double-click zoom-fits the sequence span under the cursor (moving
            // the playhead to its start only if the playhead is outside the span),
            // then a second double-click while already fit zooms back out to the
            // whole visible content. When the view is scoped (to a single sequence
            // or to a project), double-click instead returns to "All". This does not
            // change the focus scope, only the zoom.
            if (!viewAll() && timeline_.sequences.size() > 1) {
                scopeToAll();
                break;
            }
            for (int si = 0; si < (int)timeline_.sequences.size(); ++si) {
                int64_t a = 0, b = 0;
                if (!timeline_.sequenceSpan(timeline_.sequences[si], a, b)) continue;
                if (mx >= (float)frameToX((double)a) && mx <= (float)frameToX((double)b)) {
                    if (zoomFitSeqIdx_ == si) {
                        fitToFilteredSequence();
                        zoomFitSeqIdx_ = -1;
                    } else {
                        fitRange(a, b);
                        if (timeline_.playhead < a || timeline_.playhead >= b)
                            setPlayhead(a); // only when the playhead is outside this span
                        zoomFitSeqIdx_ = si;
                        zoomFitShotId_ = -1;
                    }
                    break;
                }
            }
            break;
        }
        if (shotBarRect_.h > 0 && inRect(shotBarRect_, mx, my)) {
            if (e.button.clicks < 2) {
                // Single click scrubs, just like the ruler.
                if (transportLocked()) { spectatorLocked("CHANGE FRAME"); break; } // host controls the playhead
                scrubbing_ = true;
                playing_ = false;
                setPlayhead(scrubFrame(mx));
                break;
            }
            // Double-click zoom-fits the shot's cut range under the cursor (moving
            // the playhead to its start only if the playhead is outside the range),
            // then a second double-click while already fit zooms back out to the
            // whole visible content. This does not change the focus scope, only
            // the zoom.
            for (int i = 0; i < (int)timeline_.shots.size(); ++i) {
                const Shot& s = timeline_.shots[i];
                float x0 = (float)frameToX((double)s.timelineStart);
                float x1 = (float)frameToX((double)s.end());
                if (mx >= x0 && mx <= x1) {
                    if (zoomFitShotId_ == s.id) {
                        fitToFilteredSequence();
                        zoomFitShotId_ = -1;
                    } else {
                        fitRange(s.timelineStart, s.end());
                        if (timeline_.playhead < s.timelineStart || timeline_.playhead >= s.end())
                            setPlayhead(s.timelineStart); // only when the playhead is outside this range
                        zoomFitShotId_ = s.id;
                        zoomFitSeqIdx_ = -1;
                    }
                    break;
                }
            }
            break;
        }
        if (my < tracksTop_) {
            // Ruler / cache strip: scrub, or paint the playback range when shift
            // is held. Shift is already the range modifier here (shift+double-click
            // on a clip extends the in/out range), and the range isn't the playhead,
            // so a spectator may set it even while the transport is locked.
            if (SDL_GetModState() & SDL_KMOD_SHIFT) {
                rangeDragging_ = true;
                rangeDragMoved_ = false;
                rangeDragAnchor_ = std::max<int64_t>(0, scrubFrame(mx));
                break;
            }
            if (transportLocked()) { spectatorLocked("CHANGE FRAME"); break; } // host controls the playhead
            scrubbing_ = true;
            playing_ = false;
            setPlayhead(scrubFrame(mx));
            break;
        }
        if (my < tracksViewBottom()) {
            // The curve lane owns every press inside it — including the misses, so
            // a stray click cannot start a clip drag on the row being edited.
            if (curveLaneMouseDown(mx, my, /*rightButton*/ false, e.button.clicks))
                break;
            // Razor: the press is a cut, and nothing else. It takes over ahead of
            // the trim, fade, dissolve and selection paths below - the tool is
            // modal, so a press inside a row can only ever mean "cut here". The
            // hover pass already resolved which clip and which frame; a press over
            // no clip (or where no legal cut exists) is simply swallowed.
            if (razorMode()) {
                if (razorClipId_ >= 0)
                    splitClipAt(razorClipId_, razorCutFrame_);
                break;
            }
            // Track rows don't scrub (scrubbing lives on the ruler above). A press
            // on a clip selects it and arms a pending drag, which only becomes a
            // real drag once the cursor moves past a threshold (see MOTION). A
            // press on empty track space clears the selection.
            int track = trackFromY(my);
            // A press on a clip's left/right edge starts a trim drag (single click
            // only; a double-click still falls through to the in/out toggle below).
            int trimEdge = 0;
            if (e.button.clicks < 2) {
                // Fade dots sit on the clip's top edge, inside the trim zones at the
                // sides, so they get the press first — otherwise a corner dot would
                // be unreachable behind its own clip's trim handle.
                int fadeEdge = 0;
                if (Clip* fadeClip = fadeHandleAt(track, mx, my, fadeEdge)) {
                    beginClipFade(*fadeClip, fadeEdge);
                    break;
                }
                if (Clip* edgeClip = clipEdgeAt(track, mx, trimEdge)) {
                    beginClipTrim(*edgeClip, trimEdge);
                    break;
                }
                // A dissolve box sits on top of the two clips it joins, so it takes
                // the press next: an edge resizes that side, the body selects. The
                // clip-edge test above still runs first, which keeps the trim
                // handles on the cut underneath the box reachable.
                int transEdge = 0;
                if (Transition* te = transitionEdgeAt(track, mx, transEdge)) {
                    beginTransitionResize(*te, transEdge);
                    break;
                }
                if (Transition* tb = transitionAt(track, (int64_t)std::llround(xToFrame(mx)))) {
                    clearClipSelection();
                    selectedGapTrack_ = -1;
                    selectedTransitionId_ = tb->id;
                    break;
                }
            }
            const bool shiftHeld = (SDL_GetModState() & SDL_KMOD_SHIFT) != 0;
            if (Clip* hit = clipAtTrack(track, (int64_t)std::llround(xToFrame(mx)))) {
                selectedGapTrack_ = -1; // selecting a clip drops any gap selection
                // A clip whose source is missing (open failed) is red: clicking its
                // body opens the Missing Source modal to relocate it.
                if (auto pm = timeline_.findMediaById(hit->mediaId); pm && pm->openFailed()) {
                    selectClipSingle(hit->id);
                    openRelocateModal(*pm);
                    break;
                }
                if (e.button.clicks >= 2) {
                    if (shiftHeld && timeline_.outPoint >= 0) {
                        // Shift+double-click extends the existing range to include
                        // this clip (union of current range and the clip span).
                        timeline_.inPoint = std::min(timeline_.inPoint, hit->timelineStart);
                        timeline_.outPoint = std::max(timeline_.outPoint, hit->end() - 1);
                    } else if (timeline_.inPoint == hit->timelineStart && timeline_.outPoint == hit->end() - 1) {
                        timeline_.inPoint = 0;
                        timeline_.outPoint = -1;
                    } else {
                        timeline_.inPoint = hit->timelineStart;
                        timeline_.outPoint = hit->end() - 1;
                    }
                } else if (shiftHeld) {
                    // Shift+click toggles this clip's membership in the selection.
                    // It's a pure selection gesture: no drag is armed.
                    toggleClipSelection(hit->id);
                    pendingDragClipId_ = -1;
                    dragPressWasSelected_ = false;
                } else if (isClipSelected(hit->id)) {
                    // Press on an already-selected clip arms a drag of the whole
                    // selection; a release in place collapses/deselects (BUTTON_UP).
                    pendingDragClipId_ = hit->id;
                    dragPressX_ = mx;
                    dragPressY_ = my;
                    dragPressWasSelected_ = true;
                } else {
                    selectClipSingle(hit->id);
                    pendingDragClipId_ = hit->id;
                    dragPressX_ = mx;
                    dragPressY_ = my;
                    dragPressWasSelected_ = false;
                }
            } else if (!shiftHeld) {
                // Empty track space: select the gap under the cursor (if it sits
                // between two clips), else clear both selections. A Shift+click on
                // empty space leaves the current selection untouched.
                clearClipSelection();
                int64_t gs, ge;
                if (gapAtTrack(track, (int64_t)std::llround(xToFrame(mx)), gs, ge)) {
                    selectedGapTrack_ = track;
                    selectedGapStart_ = gs;
                    selectedGapEnd_ = ge;
                } else {
                    selectedGapTrack_ = -1;
                }
            }
        }
        break;
    }
    case SDL_EVENT_MOUSE_BUTTON_UP:
        if (e.button.button == SDL_BUTTON_LEFT) {
            if (annotDrawing_) {
                // Push the completed stroke into the session: the host broadcasts
                // the frame's combined set; a spectator sends its stroke to the host.
                if (syncSession_.role() == syncreview::Role::Host)
                    broadcastAnnotations();
                else if (syncSession_.role() == syncreview::Role::Spectator)
                    sendStrokeToHost();
                flushAnnotBuffer(); // persist the completed stroke onto its clip
            }
            annotDrawing_ = false; // finalize any in-progress pencil stroke
            drawDragSize_ = false;
            drawDragWheel_ = false;
            peResizing_ = false;
            clipSourceResizing_ = false;
            tlResizing_ = false;
            trackSb_.dragging = false;
            if (volumeDragging_) {
                volumeDragging_ = false;
                writePrefs(); // persist the level once, on release
            }
            if (gradeDragSlider_ >= 0 || gradeDragWheel_ >= 0 || gradeDragCurvePt_ >= 0)
                gradeHandleEvent(e); // release any active grade-widget drag
            if (trackDragging_ && trackDropIns_ >= 0)
                reorderTrack(trackDragFrom_, trackDropIns_);
            trackDragFrom_ = -1;
            trackDragging_ = false;
            trackDropIns_ = -1;
            if (curveLaneMouseUp()) {
                // handled: the lane recorded its own undo step
            } else if (resizingTransition_) {
                commitTransitionResize();
            } else if (fadingClip_) {
                commitClipFade();
            } else if (trimmingClip_) {
                commitClipTrim();
            } else if (draggingClip_) {
                // Alt held at release duplicates instead of moving (the modifier
                // is read now, not at grab time). Ctrl is the ripple-mode
                // modifier, so it can't also mean duplicate.
                commitClipDrag((SDL_GetModState() & SDL_KMOD_ALT) != 0);
            } else if (dragPressWasSelected_ && pendingDragClipId_ >= 0) {
                // Click-in-place on an already-selected clip (no drag): a multi-
                // selection collapses to just the clicked clip; a single selection
                // toggles off (deselect).
                if (selectedClipIds_.size() > 1)
                    selectClipSingle(pendingDragClipId_);
                else
                    clearClipSelection();
            }
            // Media-bin (or Clip Source) drag: commit the drop, or treat an
            // in-place release as the click the press was armed as.
            if (binDragging_) {
                commitBinDrag(e.button.x, e.button.y);
            } else if (binDragRow_ >= 0) {
                if (binDragReselect_)
                    clickExplorerRow(binDragRow_, SDL_KMOD_NONE); // collapse selection to the pressed row
            } else if (clipSourceDragArmed_) {
                commitClipSourcePick();
            }
            binDragRow_ = -1;
            binDragging_ = false;
            binDragReselect_ = false;
            binDragLabel_.clear();
            binDragPickerClipId_ = -1;
            clipSourceDragArmed_ = false;
            pendingDragClipId_ = -1; // press-release in place: was just a scrub
            dragPressWasSelected_ = false;
            scrubbing_ = false;
            if (rangeDragging_) {
                // A shift+click that never moved clears the range rather than
                // marking a one-frame loop; a real drag has already written it.
                if (!rangeDragMoved_) {
                    timeline_.inPoint = 0;
                    timeline_.outPoint = -1;
                    setStatus("IN/OUT CLEARED");
                } else {
                    setStatus("RANGE: " + std::to_string(timeline_.inPoint) + " - " +
                              std::to_string(timeline_.outPoint));
                }
                rangeDragging_ = false;
            }
            // Frame gesture: a press-release in place (never promoted to a jog)
            // is a click on the picture, which toggles playback. A release that
            // wandered off the frame is not a click on it, so it does nothing.
            if (playerClickArmed_ && !playerScrubbing_ && !playerClickActivating_
                && inPlayerView(e.button.x, e.button.y))
                togglePlay();
            playerClickArmed_ = false;
            playerClickActivating_ = false;
            playerScrubbing_ = false;
        }
        if (e.button.button == SDL_BUTTON_LEFT) {
            expDragging_ = false; // E may still be held: a second drag starts fresh
            gridSb_.dragging = false;
            peSourceSb_.dragging = false;
            peSeqSb_.dragging = false;
            ocioMenuSb_.dragging = false;
            sequenceMenuSb_.dragging = false;
            recentSb_.dragging = false;
            sharedSb_.dragging = false;
        }
        if (e.button.button == SDL_BUTTON_MIDDLE) { panning_ = false; framePanning_ = false; gridPanning_ = false; }
        break;
    case SDL_EVENT_MOUSE_MOTION: {
        float mx = e.motion.x, my = e.motion.y;
        // Resize drags: move only, no other motion processing.
        if (peResizing_) {
            peUserW_ = std::clamp(mx - kSidePanelW, 140.0f, winW_ * 0.5f);
            break;
        }
        if (clipSourceResizing_) {
            clipSourceUserW_ = std::clamp(mx - kSidePanelW, 140.0f, winW_ * 0.5f);
            break;
        }
        // Timeline top edge: the height is whatever the cursor leaves below it,
        // held to the same limits computeLayout applies, so a drag past either end
        // cannot bank a height that springs the timeline open again later.
        if (tlResizing_) {
            const float h = std::clamp(winH_ - my, tlMinH_, tlMaxH_);
            // Dragged back to the top of its range: hand the height back to the
            // automatic one, so the timeline goes on following its content rather
            // than staying pinned to what the stack happens to be right now.
            tlUserH_ = h >= tlMaxH_ - 0.5f ? 0.0f : h;
            break;
        }
        // Active grade-widget drag (slider / wheel / curve point) takes priority.
        if (gradeOpen_ && (gradeDragSlider_ >= 0 || gradeDragWheel_ >= 0 || gradeDragCurvePt_ >= 0)) {
            gradeHandleEvent(e);
            break;
        }
        // Active draw-tool drag (size slider / color wheel) updates from anywhere.
        if (drawDragSize_ || drawDragWheel_) {
            drawPanelHandlePress(mx, my);
            break;
        }
        // Extending a pencil stroke.
        if (annotDrawing_) {
            annotAppendPoint(mx, my);
            break;
        }
        // Dragging a grabbed scrollbar (Overview grid, source bin, sequence tree
        // or a launcher list): it follows the cursor anywhere, even off the bar,
        // which is what makes a grab feel held rather than tracked.
        if (gridSb_.dragging) {
            gridSb_.drag(my, gridScroll_, dpiScale);
            break;
        }
        if (peSourceSb_.dragging) {
            peSourceSb_.drag(my, peSourceScroll_, dpiScale);
            break;
        }
        if (peSeqSb_.dragging) {
            peSeqSb_.drag(my, peSeqScroll_, dpiScale);
            break;
        }
        if (recentSb_.dragging) {
            recentSb_.drag(my, recentScroll_, dpiScale);
            break;
        }
        if (sharedSb_.dragging) {
            sharedSb_.drag(my, sharedScroll_, dpiScale);
            break;
        }
        if (trackSb_.dragging) {
            trackSb_.drag(my, trackScroll_, dpiScale); // clamped in computeLayout
            break;
        }
        // Update resize handle hover states.
        const PanelEdge hoverEdge = panelResizeEdgeAt(mx, my);
        peResizeHovered_ = hoverEdge == PanelEdge::ProjectExplorer;
        clipSourceResizeHovered_ = hoverEdge == PanelEdge::ClipSource;
        tlResizeHovered_ = hoverEdge == PanelEdge::None && overTimelineEdge(mx, my);
        // Track reorder: promote the armed label press to a drag, then follow the
        // cursor. Takes over motion — the gutter has nothing else to hover.
        if (trackDragFrom_ >= 0) {
            const float kDragThreshold = 4.0f;
            if (!trackDragging_ && std::abs(my - trackDragPressY_) > kDragThreshold)
                trackDragging_ = true;
            if (trackDragging_) {
                trackDragGhostY_ = my - trackDragGrabDY_;
                int ins = trackDropBoundaryAt(my);
                // The two boundaries touching the grabbed row put it back where it
                // came from; those read as "no drop", so nothing lights up.
                trackDropIns_ = (ins == trackDragFrom_ || ins == trackDragFrom_ + 1) ? -1 : ins;
                break;
            }
        }
        // Active edge trim: resize the clip live; nothing else processes motion.
        if (trimmingClip_) {
            updateClipTrim(mx);
            break;
        }
        // Same for an active dissolve resize or fade drag.
        if (resizingTransition_) {
            updateTransitionResize(mx);
            break;
        }
        if (fadingClip_) {
            updateClipFade(mx);
            break;
        }
        // Same for a curve point being dragged in the lane.
        if (curveDragClipId_ >= 0) {
            curveLaneMouseMotion(mx, my);
            break;
        }
        // Media-bin → timeline drag: arm on a source-row press, promote to a live
        // drag past the threshold, then preview the drop target. Takes over motion.
        if (binDragRow_ >= 0) {
            const float kDragThreshold = 4.0f;
            if (!binDragging_ &&
                (std::abs(mx - binDragPressX_) > kDragThreshold ||
                 std::abs(my - binDragPressY_) > kDragThreshold)) {
                binDragging_ = true;
                binDragReselect_ = false; // it's a drag, not a click
                binDragPaths_ = selectedSourcePaths_.empty()
                                    ? std::vector<std::string>{ explorerRows_[binDragRow_].path }
                                    : selectedSourcePaths_;
                binDragLabel_.clear(); // the card names the source file
                binDragPickerClipId_ = -1; // no picker clip to align under
            }
        }
        // Clip Source commit row (Version) → the same drag. Its press was armed
        // instead of committed precisely so it could become this.
        if (clipSourceDragArmed_) {
            const float kDragThreshold = 4.0f;
            if (std::abs(mx - clipSourceDragPressX_) > kDragThreshold ||
                std::abs(my - clipSourceDragPressY_) > kDragThreshold)
                beginClipSourceDrag(); // drops the arm if the pick has no media
        }
        if (binDragging_) {
            updateBinDrag(mx, my);
            break;
        }
        // Commit pending clip click to a drag once the mouse moves far enough.
        if (pendingDragClipId_ >= 0 && !draggingClip_) {
            const float kDragThreshold = 4.0f;
            if (std::abs(mx - dragPressX_) > kDragThreshold ||
                std::abs(my - dragPressY_) > kDragThreshold) {
                if (Clip* c = clipById(pendingDragClipId_)) {
                    scrubbing_ = false; // a drag takes over from the scrub
                    beginClipDrag(*c, dragPressX_);
                    updateClipDrag(mx, my);
                }
                pendingDragClipId_ = -1;
            }
        }
        if (draggingClip_)
            updateClipDrag(mx, my);
        else if (scrubbing_)
            setPlayhead(scrubFrame(mx));
        else if (rangeDragging_) {
            // Shift+drag on the ruler: the anchor is the in point, the cursor the
            // out point (which is the last playable frame, so it's inclusive).
            // Nothing is written until the cursor leaves the anchor frame, which
            // is what lets a release in place read as "clear" instead.
            int64_t f = std::max<int64_t>(0, scrubFrame(mx));
            if (f != rangeDragAnchor_)
                rangeDragMoved_ = true;
            if (rangeDragMoved_) {
                timeline_.inPoint  = std::min(rangeDragAnchor_, f);
                timeline_.outPoint = std::max(rangeDragAnchor_, f);
            }
        } else if (gridPanning_) {
            // Grab and drag: the tiles follow the hand, so dragging up walks
            // further down the grid.
            gridScroll_ -= e.motion.yrel; // clamped in renderGridView
        } else if (expDragging_) {
            expScrubDx_ += e.motion.xrel;
            expScrubDy_ += e.motion.yrel;
            exposureScrub(expScrubDx_, expScrubDy_);
        } else if (playerClickArmed_) {
            // Fixed pixels-per-frame in logical units, so the jog feels the same
            // whatever the zoom or the clip length. One frame's travel is also
            // what separates a jog from a click, so a hand that barely moves on
            // a click still reads as a click.
            const float kPxPerFrame = 6.0f;
            playerScrubDx_ += e.motion.xrel;
            if (!playerScrubbing_ && std::abs(playerScrubDx_) >= kPxPerFrame) {
                // Promote to a jog: pause so the scrub isn't fighting playback,
                // and anchor on the frame playback actually reached.
                playing_ = false;
                playerScrubbing_ = true;
                playerScrubStart_ = timeline_.playhead;
            }
            if (playerScrubbing_)
                setPlayhead(playerScrubStart_
                            + (int64_t)std::llround(playerScrubDx_ / kPxPerFrame));
        } else if (framePanning_) {
            framePanX_ += e.motion.xrel;
            framePanY_ += e.motion.yrel;
        } else if (panning_) {
            viewStart_ -= e.motion.xrel * framesPerPx_;
            zoomFitSeqIdx_ = zoomFitShotId_ = -1; // view no longer matches a fit range
        }
        if (volumeDragging_)
            setVolume((mx - volumeSliderRect_.x) / volumeSliderRect_.w);
        // Transport button hover (info bar).
        hoveredTransport_ = inRect(prevClipBtnRect_, mx, my) ? 0
                          : inRect(playBtnRect_, mx, my)     ? 1
                          : inRect(nextClipBtnRect_, mx, my) ? 2
                          : -1;
        hoveredVolumeBtn_ = inRect(volumeBtnRect_, mx, my);
        hoveredVolumeSlider_ = volumeDragging_ || inRect(volumeSliderRect_, mx, my);
        hoveredMagnet_ = inRect(magnetBtnRect_, mx, my);
        hoveredCursorTool_ = inRect(cursorToolBtnRect_, mx, my);
        hoveredRazorTool_ = inRect(razorToolBtnRect_, mx, my);
        // A disabled "Fit" hovers too — the menu it drops has the other fits in it.
        hoveredFit_ = inRect(fitBtnRect_, mx, my);
        hoveredLetterbox_ = inRect(letterboxBtnRect_, mx, my);
        hoveredProxy_ = inRect(proxyBtnRect_, mx, my);
        hoveredOcioDisplay_ = inRect(ocioDisplayBtnRect_, mx, my);
        hoveredOcioView_ = inRect(ocioViewBtnRect_, mx, my);
        hoveredOcioLook_ = inRect(ocioLookBtnRect_, mx, my);
        hoveredOcioInputCs_ = inRect(ocioInputCsBtnRect_, mx, my);
        hoveredSequence_ = inRect(sequenceBtnRect_, mx, my);
        hoveredOpenProj_ = inRect(openProjBtnRect_, mx, my);
        hoveredOpenSeq_ = inRect(openSeqBtnRect_, mx, my);
        hoveredDrawClear_ = drawPanelRect_.w > 0.0f && inRect(drawClearRect_, mx, my);
        // Track-header burger (left gutter), never on the trailing placeholder
        // row, which draws none.
        hoveredTrackMenu_ = -1;
        if (mx < headerX_ && my >= tracksTop_ && my < tracksViewBottom()) {
            for (int t = 0, tn = std::max(trackCount(), 1); t < tn; ++t) {
                if (t == tn - 1 && timeline_.trackEmpty(t)) continue;
                if (inRect(trackMenuRect(t), mx, my)) { hoveredTrackMenu_ = t; break; }
            }
        }
        // Lane hover: which point (or the line) the cursor is on, for the
        // highlight and the ghost point. No-op when the mode is off.
        if (curveMode())
            curveLaneMouseMotion(mx, my);
        pencilOverFrame_ = pencilMode_ && inPlayerView(mx, my);
        // Ruler hover: preview where a click would drop the playhead. Matches
        // the scrub-clickable zone (ruler + cache strip + seq/shot bars).
        tlHoverActive_ = mx >= headerX_ && my >= rulerRect_.y && my < tracksTop_
                       && !scrubbing_ && !rangeDragging_ && !draggingClip_
                       && !trimmingClip_ && !panning_;
        tlHoverX_ = mx;
        updateFramePreview(); // resolve + decode the hovered-frame thumbnail (throttled)
        // Update hover state for clips and gaps.
        hoveredClipId_ = -1;
        hoverGapTrack_ = -1;
        trimHover_ = false;
        razorClipId_ = -1;
        if (razorMode()) {
            // The razor owns the whole row: no edge, fade or gap probe runs, since
            // every one of them would claim a press the razor is about to use. Only
            // the clip under the cursor matters, and hoveredClipId_ is still set so
            // the clip keeps drawing as itself (the dense-column sweep in drawFrame
            // flattens undecorated clips, and hover is one of the decorations).
            if (mx >= headerX_ && my >= tracksTop_ && my < tracksViewBottom())
                if (Clip* hit = clipAtTrack(trackFromY(my),
                                            (int64_t)std::llround(xToFrame(mx)))) {
                    hoveredClipId_ = hit->id;
                    razorCutFrame_ = razorFrameFor(*hit, mx);
                    // A one-frame clip has nowhere to cut: razorFrameFor clamps to
                    // the only legal frame, which is its start, so offer no line.
                    if (razorCutFrame_ > hit->timelineStart &&
                        razorCutFrame_ < hit->end())
                        razorClipId_ = hit->id;
                }
            break;
        }
        if (mx >= headerX_ && my >= tracksTop_ && my < tracksViewBottom()) {
            int track = trackFromY(my);
            int hoverEdge = 0;
            int transHoverEdge = 0, fadeHoverEdge = 0;
            if (Clip* fadeClip = fadeHandleAt(track, mx, my, fadeHoverEdge)) {
                trimHover_ = true; // a fade dot drags horizontally, like an edge
                hoveredClipId_ = fadeClip->id;
            } else if (Clip* edgeClip = clipEdgeAt(track, mx, hoverEdge)) {
                trimHover_ = true;
                hoveredClipId_ = edgeClip->id;
            } else if (transitionEdgeAt(track, mx, transHoverEdge)) {
                trimHover_ = true; // a dissolve edge resizes like a clip edge
            } else if (Clip* hit = clipAtTrack(track, (int64_t)std::llround(xToFrame(mx)))) {
                hoveredClipId_ = hit->id;
            } else {
                int64_t gs, ge;
                if (gapAtTrack(track, (int64_t)std::llround(xToFrame(mx)), gs, ge)) {
                    hoverGapTrack_ = track;
                    hoverGapStart_ = gs;
                    hoverGapEnd_ = ge;
                }
            }
        }
        break;
    }
    case SDL_EVENT_MOUSE_WHEEL:
        if (settingsOpen_ && e.wheel.mouse_x < panelsRight_ &&
                   e.wheel.mouse_x >= kSidePanelW && e.wheel.mouse_y < panelsBottom_ &&
                   e.wheel.y != 0.0f) {
            settingsScroll_ -= e.wheel.y * 48.0f; // clamped in renderSettingsPanel
            if (settingsScroll_ < 0.0f)
                settingsScroll_ = 0.0f;
        } else if (gradeOpen_ && e.wheel.mouse_x < panelsRight_ &&
                   e.wheel.mouse_x >= kSidePanelW && e.wheel.mouse_y < panelsBottom_ && e.wheel.y != 0.0f) {
            gradeScroll_ -= e.wheel.y * 48.0f; // clamped in renderGradePanel
            if (gradeScroll_ < 0.0f)
                gradeScroll_ = 0.0f;
        } else if (clipSourceOpen_ && e.wheel.mouse_x < panelsRight_ &&
                   e.wheel.mouse_x >= kSidePanelW && e.wheel.mouse_y < panelsBottom_ &&
                   e.wheel.y != 0.0f) {
            clipSourceScroll_ -= e.wheel.y * 48.0f; // clamped in renderClipSourcePanel
            if (clipSourceScroll_ < 0.0f)
                clipSourceScroll_ = 0.0f;
        } else if (projectExplorerOpen_ && peActiveTab_ == PeTabSources &&
                   e.wheel.y != 0.0f &&
                   inRect(sourceInfoRect_, e.wheel.mouse_x, e.wheel.mouse_y)) {
            sourceInfoScroll_ -= e.wheel.y * 48.0f; // clamped in renderSourceInfoPanel
            if (sourceInfoScroll_ < 0.0f)
                sourceInfoScroll_ = 0.0f;
        } else if (projectExplorerOpen_ && peActiveTab_ == PeTabSources &&
                   e.wheel.mouse_x < panelsRight_ && e.wheel.mouse_x >= kSidePanelW &&
                   e.wheel.mouse_y < panelsBottom_ && e.wheel.y != 0.0f) {
            peSourceScroll_ -= e.wheel.y * 48.0f; // upper clamp in renderProjectExplorer
            if (peSourceScroll_ < 0.0f)
                peSourceScroll_ = 0.0f;
        } else if (projectExplorerOpen_ && peActiveTab_ == PeTabSequences &&
                   e.wheel.mouse_x < panelsRight_ && e.wheel.mouse_x >= kSidePanelW &&
                   e.wheel.mouse_y < panelsBottom_ && e.wheel.y != 0.0f) {
            peSeqScroll_ -= e.wheel.y * 48.0f; // upper clamp in renderProjectExplorer
            if (peSeqScroll_ < 0.0f)
                peSeqScroll_ = 0.0f;
        } else if (inspectorVisible() && e.wheel.y != 0.0f &&
                   inRect(inspectorRect_, e.wheel.mouse_x, e.wheel.mouse_y)) {
            inspectorScroll_ -= e.wheel.y * 48.0f; // clamped in renderInspector
            if (inspectorScroll_ < 0.0f)
                inspectorScroll_ = 0.0f;
        } else if (launcherVisible() && e.wheel.y != 0.0f &&
                   inRect(launcherSyncListRect_, e.wheel.mouse_x, e.wheel.mouse_y)) {
            launcherSyncScroll_ -= e.wheel.y * 48.0f; // clamped in renderMainPanels
            if (launcherSyncScroll_ < 0.0f)
                launcherSyncScroll_ = 0.0f;
        } else if (launcherVisible() && e.wheel.y != 0.0f &&
                   inRect(sharedListRect_, e.wheel.mouse_x, e.wheel.mouse_y)) {
            sharedScroll_ -= e.wheel.y * 48.0f; // clamped in renderMainPanels
            if (sharedScroll_ < 0.0f)
                sharedScroll_ = 0.0f;
        } else if (launcherVisible() && e.wheel.y != 0.0f &&
                   inRect(recentListRect_, e.wheel.mouse_x, e.wheel.mouse_y)) {
            recentScroll_ -= e.wheel.y * 48.0f; // clamped in renderMainPanels
            if (recentScroll_ < 0.0f)
                recentScroll_ = 0.0f;
        } else if (gridView() && inPlayerView(e.wheel.mouse_x, e.wheel.mouse_y) && e.wheel.y != 0.0f) {
            // Overview: the wheel scrolls the grid and ctrl+wheel resizes its tiles.
            // Neither touches the timeline — the grid holds the whole scope, so
            // there is nothing about the timeline's window left for it to follow.
            if (SDL_GetModState() & SDL_KMOD_CTRL)
                gridSetThumbH(gridThumbH_ * std::pow(1.15f, (float)e.wheel.y));
            else
                gridScroll_ -= e.wheel.y * gridThumbH_ * 0.75f; // clamped in renderGridView
        } else if (inPlayerView(e.wheel.mouse_x, e.wheel.mouse_y) && e.wheel.y != 0.0f) {
            if (transportLocked()) // host drives the frame view; spectators mirror it
                spectatorLocked("ZOOM/PAN");
            else
                zoomFrameAt(e.wheel.mouse_x, e.wheel.mouse_y, std::pow(1.25f, (float)e.wheel.y));
        } else if (e.wheel.mouse_y >= tracksTop_ && e.wheel.mouse_y < tracksViewBottom()
                   && tracksMaxScroll() > 0.0f && e.wheel.y != 0.0f) {
            // Over the track rows with more tracks than fit, the wheel scrolls the
            // stack. With everything already on screen there is nothing to scroll,
            // so it keeps zooming the view as it does over the ruler and the bars.
            trackScroll_ -= e.wheel.y * kTrackH * 0.5f; // clamped in computeLayout
        } else if (e.wheel.mouse_y >= tlRect_.y && e.wheel.y != 0.0f) {
            zoomAt(e.wheel.mouse_x, std::pow(1.0 / 1.25, (double)e.wheel.y));
        }
        break;
    default:
        break;
    }
}

// ---------------------------------------------------------------- update

void App::update() {
    Uint64 now = SDL_GetTicks();
    double dt = (double)(now - lastTickMs_) / 1000.0;
    lastTickMs_ = now;

    // Clip-drop mode follows Ctrl live: tapping the key mid-drag must re-place the
    // drop (ripple jumps a landing that straddles a neighbour forward, overwrite
    // does not), so re-run the drag at the current cursor when the mode flips.
    if (draggingClip_) {
        DropMode want = rippleModifierHeld() ? DropMode::Ripple : DropMode::Overwrite;
        if (want != dragDropMode_) {
            dragDropMode_ = want;
            float mx = 0.0f, my = 0.0f;
            uiMouse(mx, my);
            updateClipDrag(mx, my);
        }
    } else {
        dragDropMode_ = DropMode::Overwrite;
    }

    // Visible playback rate (info-bar debug readout). Counts distinct frames that
    // actually reached the screen over the last ~1s (timestamps pushed in
    // renderPlayer). The rate is derived from frame spacing rather than the render
    // cadence, so steady playback reads a stable project fps; a held/dropped frame
    // shows up as a gap and an in-progress stall sags the value promptly.
    if (!playing_) {
        fpsVisible_ = 0.0;
        fpsShown_ = 0.0;
        visibleFrameTimes_.clear();
        fpsWindowFrom_ = 0;
    } else {
        const Uint64 windowMs = 1000;
        if (fpsWindowFrom_ == 0) fpsWindowFrom_ = now;
        while (visibleFrameTimes_.size() > 1 && visibleFrameTimes_.front() + windowMs < now)
            visibleFrameTimes_.pop_front();
        const size_t n = visibleFrameTimes_.size();
        if (n >= 2) {
            double spanMs = (double)(visibleFrameTimes_.back() - visibleFrameTimes_.front());
            double expMs = spanMs / (double)(n - 1);            // avg interval among recent frames
            double gapMs = (double)(now - visibleFrameTimes_.back()); // wait since last shown frame
            // Dead time at the *start* of the window, counted the same way as the
            // wait at the end. Without it a stall that has just cleared is invisible
            // -- its frameless stretch holds no timestamps, so trimming leaves only
            // the handful since the stall and the rate is read off those alone. That
            // reported a full 24 fps a beat after a 500 ms hitch, and after a stall
            // longer than the window (n back down to 2) the render cadence itself:
            // ~59 fps on 24 fps media. The window never reaches back past the
            // start of playback, or the first second after Play would read as one
            // long stall and ramp up from ~1 fps instead of the rate straight away.
            Uint64 winStart = (now > windowMs) ? now - windowMs : 0;
            if (winStart < fpsWindowFrom_) winStart = fpsWindowFrom_;
            double leadMs = (double)visibleFrameTimes_.front() - (double)winStart;
            double denomMs = spanMs + std::max(0.0, gapMs - expMs)   // only count an abnormal wait
                                    + std::max(0.0, leadMs - expMs);
            fpsVisible_ = denomMs > 0.0 ? (double)(n - 1) * 1000.0 / denomMs : 0.0;
        } else {
            fpsVisible_ = 0.0;
        }
        // Snap to the project rate when within tolerance, then hold the drawn
        // value until the measurement moves a full tenth away from it: an
        // estimate hovering on a tenth boundary would otherwise alternate
        // 23.9 / 24.0 every frame. Real drops move further than that and show.
        double fpsTarget = fpsVisible_;
        if (timeline_.fps > 0.0 && std::fabs(fpsTarget - timeline_.fps) < 0.3)
            fpsTarget = timeline_.fps;
        if (std::fabs(fpsTarget - fpsShown_) >= 0.1)
            fpsShown_ = std::round(fpsTarget * 10.0) / 10.0;
    }

    updateOutputMenu();   // rebuild the Output menu when displays/backends change (hotplug)

    if (!playing_ || timeline_.length() <= 0 || timeline_.fps <= 0.0) {
        updateAudio(); // stopped: pause the audio device (or feed scrub grains)
        return;
    }

    playbackStalled_ = false;
    playAcc_ += dt;
    const double spf = 1.0 / timeline_.fps;
    while (playAcc_ >= spf) {
        playAcc_ -= spf;
        int64_t lo = 0, hi = 0;
        playbackRange(lo, hi); // in/out range, confined to the scoped sequence
        int64_t next = timeline_.playhead + 1;
        if (next > hi || next < lo)
            next = lo; // loop the in/out range

        // Hold (instead of skipping ahead) while the next frame is still loading.
        const Clip* c = getTopMostClipAtFrame(next);
        if (c && !c->mediaId.empty()) {
            auto pm = timeline_.findMediaById(c->mediaId);
            if (pm && !pm->openFailed()) {
                CacheKey key{ c->mediaId, c->sourceOffset + (next - c->timelineStart) };
                if (!cache_->has(key)) {
                    playAcc_ = 0.0;
                    playbackStalled_ = true;
                    break;
                }
            }
        }
        timeline_.playhead = next;
        playDir_ = 1;
    }
    followPlayhead();
    updateAudio();
}

// Bake `c`'s volume curve into the linear-gain envelope the mixer consumes,
// covering kGainAheadSec of file time from the playhead. The mixer queues ~200 ms
// ahead, so the envelope has to reach past the playhead; a couple of seconds is
// far more than that and still only a few hundred floats a frame.
//
// Sampled in FILE time (the item's own clock) rather than timeline time, which is
// what lets the engine index it by a channel's decode position and land each gain
// value on the samples it belongs to. dB -> linear happens here so the engine
// stays a mixer and knows nothing about curves.
static void bakeClipGain(const Clip& c, int64_t srcFrame, double fps,
                         AudioEngine::FeedItem& item) {
    if (c.volume.isDefault() || fps <= 0.0)
        return; // unity: leave the envelope empty so the mixer skips it entirely
    constexpr double kGainAheadSec = 2.0;
    constexpr double kGainHz = 100.0;
    const int n = (int)(kGainAheadSec * kGainHz) + 1;
    item.gainStartSec = (double)srcFrame / fps;
    item.gainHz = kGainHz;
    item.gain.resize((size_t)n);
    for (int i = 0; i < n; ++i) {
        const double sec = item.gainStartSec + (double)i / kGainHz;
        item.gain[(size_t)i] = dbToGain(c.volume.valueAt((int64_t)std::floor(sec * fps)));
    }
}

// Slave the audio output to the video-master playhead. Gathers everything
// audible at the playhead — the program video clip's embedded audio plus every
// (non-hidden) audio clip covering the frame — and hands the set to the
// AudioEngine mixer, which (re)cues on discontinuities and pauses when video
// holds for I/O. Gaps resolve to silence.
void App::updateAudio() {
    if (!audio_)
        return;
    bool haveTimeline = timeline_.length() > 0 && timeline_.fps > 0.0;
    bool active = playing_ && haveTimeline;
    // Scrubbing feed: only while stopped, dragging the ruler, and the feature is on.
    bool scrub = audioScrub_ && scrubbing_ && !playing_ && haveTimeline;
    std::vector<AudioEngine::FeedItem> items;
    if (active || scrub) {
        // Embedded audio of the program (topmost video) clip. id -1 keeps this
        // channel's identity across clip boundaries so audio sliced from one
        // continuous file plays through seamlessly.
        const Clip* c = getTopMostClipAtFrame(timeline_.playhead);
        if (c && !c->mediaId.empty()) {
            auto pm = timeline_.findMediaById(c->mediaId);
            if (pm && pm->type() == ClipType::Video && !pm->openFailed()) {
                int64_t srcFrame = c->sourceOffset + (timeline_.playhead - c->timelineStart);
                double mfps = pm->info().fps > 0.0 ? pm->info().fps : timeline_.fps;
                if (mfps > 0.0)
                    items.push_back({ -1, pm->resolvedPath(), (double)srcFrame / mfps });
            }
        }
        // Every audio clip under the playhead (all audio tracks mix). Audio clip
        // frames map by the project fps.
        double fps = timeline_.fps > 0.0 ? timeline_.fps : 24.0;
        auto feedAudioClip = [&](const Clip& ac) {
            if (!ac.audio || timeline_.clipDisabled(ac))
                return;
            if (timeline_.playhead < ac.timelineStart || timeline_.playhead >= ac.end())
                return;
            auto pm = timeline_.findMediaById(ac.mediaId);
            if (!pm || pm->openFailed())
                return;
            int64_t srcFrame = ac.sourceOffset + (timeline_.playhead - ac.timelineStart);
            AudioEngine::FeedItem it{ ac.id, pm->path(), (double)srcFrame / fps };
            bakeClipGain(ac, srcFrame, fps, it);
            items.push_back(std::move(it));
        };
        forEachViewClip(feedAudioClip);
    }
    audio_->update(active, scrub, playbackStalled_, items);
}

void App::submitCacheRequests() {
    if (!timeline_.hasClips())
        return;
    // A project named on the command line loads during init(), before run() has
    // started the interpreter, so the proxy mode it should open in is not known
    // yet (adoptDefaultProxyMode in App_ProxyMenu.cpp). Decoding now reads the
    // representation the project is about to stop using, and applyProxyMode's
    // cache_->clear() then throws every one of those frames away -- including the
    // playhead's, which is the read furthest along when the flush lands. Waiting
    // for the mode costs only frames that were going to be discarded anyway.
    if (pendingProxyDefault_)
        return;
    // The active cache range: the in/out loop range when the user has set one,
    // else the whole timeline. inPoint/effectiveOut() collapse to [0, length()-1]
    // when no custom range is set, so [lo, hi] is general. Requests outside this
    // range are dropped so a custom range caches only its own frames.
    bool customRange = timeline_.inPoint != 0 || timeline_.outPoint >= 0;
    int64_t lo = std::min(timeline_.inPoint, timeline_.effectiveOut());
    int64_t hi = std::max(timeline_.inPoint, timeline_.effectiveOut());
    int64_t span = hi - lo + 1;
    // Keep decoding ahead until either the budget is full or the lead cap is
    // reached. The budget side is sized to how many frames actually fit (mean
    // resident frame size) so we don't over-request and thrash the tail. This
    // holds while playing too: too short a window caps the cushion at its own
    // length however fast the decoders run, so playback never banks the head start
    // it needs to ride out a slow stretch. Filling forward instead flushes the
    // frames behind the playhead as it goes (they drop out of the wanted set) and
    // buys as deep a lead as the budget allows, up to the cap.
    // kPrefetchAhead is the floor for before an average exists, and only then.
    size_t avg = cache_->avgFrameBytes();
    int64_t fit = avg ? (int64_t)(cache_->bytesMax() / avg) : 256; // bootstrap until an avg exists
    // On a comparison stage a timeline frame costs one decode per row — the Layout
    // shows them all at once and the Stack has to be able to rotate to any of them
    // without a decode — so the budget holds that many fewer of them. Without this
    // the fill over-requests by the row count and thrashes its own tail. (A
    // dissolve is left alone: it simply reaches less far in wall-clock, which is
    // cheaper than special-casing it.)
    if (compareStage()) {
        std::vector<const Clip*> tiles;
        layoutClipsAt(timeline_.playhead, tiles);
        int64_t live = 0;
        for (const Clip* t : tiles)
            if (t)
                ++live; // an empty row costs no decode
        fit /= std::max<int64_t>(1, live);
    }
    int64_t aheadCap = std::max<int64_t>(kPrefetchAhead, timeline_.length());
    // Bounded by lead time as well as by the budget: see kPrefetchLeadSeconds.
    int64_t leadCap = (int64_t)std::llround(kPrefetchLeadSeconds * std::max(1.0, timeline_.fps));
    // Look-behind is spent out of the same budget as the look-ahead -- both are in
    // the wanted set -- so it has to be sized by the budget too. A fixed 16 frames
    // against a budget that holds fewer than that is churn on its own account,
    // however small the forward window is made. A quarter of what fits, capped at
    // the old fixed count, leaves the rest to the look-ahead, which is what
    // playback actually rides on.
    int behind = (int)std::clamp<int64_t>(fit / 4, 0, kPrefetchBehind);
    // kPrefetchAhead is the *bootstrap* floor only. Until an average frame size
    // exists there is no budget to size the window by, so we ask for a fixed
    // window and let the first decodes establish one; after that `fit` has to win
    // even when it lands below kPrefetchAhead. Flooring at 64 regardless is what
    // made 4K EXR review thrash: at ~81 MB a frame the budget holds far fewer than
    // 64, so every frame that landed evicted one the window still wanted and the
    // pool re-decoded it forever -- churn that never converges, and that ran even
    // while paused.
    int64_t aheadFloor = avg ? 1 : kPrefetchAhead;
    int ahead = (int)std::clamp<int64_t>(std::min<int64_t>(fit - behind - 1, leadCap),
                                        aheadFloor, aheadCap);
    // The forward window wraps around the active range: as it passes the
    // out-point it folds back to the in-point. During playback this keeps the
    // loop seamless (without it, a range larger than the cache evicts the
    // in-point while playing forward and playback stalls at the out-point). While
    // paused with a custom range set, wrapping fills the whole range instead of a
    // plain forward window, so scrubbing anywhere in the range is instant.
    bool wrap = span > 1 && timeline_.playhead >= lo && timeline_.playhead <= hi &&
                (playing_ || customRange);
    // A window that laps the range only re-asks for frames it already asked for
    // this tick — deduped inside the cache, but not before a clip lookup per
    // frame. With a budget-sized window that lap is the common case on a short
    // loop range, and this runs every frame of playback.
    if (wrap)
        ahead = (int)std::min<int64_t>(ahead, span - 1);

    // Everything above is cheap; everything below costs a clip lookup and a cache
    // request per frame of the window. Nothing about the wanted set is tracked
    // per-edit, so this hashes what the requests are derived from -- the same
    // "derived, not tracked" bargain as projectSignature(), and for the same
    // reason: no edit path can silently go unnoticed. An unchanged signature means
    // the standing wanted set still describes the right frames, so the epoch is
    // left in force and the workers keep draining what they already have.
    // Rebuilding the signature is one pass over the view's clips; the loops it
    // guards are hundreds of times that, and while paused they change nothing at
    // all.
    uint64_t sig = 1469598103934665603ull;
    mixSig(sig, (uint64_t)timeline_.playhead);
    mixSig(sig, (uint64_t)lo);
    mixSig(sig, (uint64_t)hi);
    mixSig(sig, (uint64_t)ahead);
    mixSig(sig, (uint64_t)behind);
    mixSig(sig, wrap ? 1u : 0u);
    mixSig(sig, compareStage() ? 1u : 0u);
    mixSig(sig, cache_->generation()); // a flushed cache has to be refilled
    for (int si : viewSeqIndices())
        mixSig(sig, (uint64_t)si);
    forEachViewClip([&](const Clip& c) {
        mixSig(sig, (uint64_t)c.id);
        mixSig(sig, (uint64_t)c.track);
        mixSig(sig, (uint64_t)c.timelineStart);
        mixSig(sig, (uint64_t)c.duration);
        mixSig(sig, (uint64_t)c.sourceOffset);
        mixSig(sig, (uint64_t)c.fadeInFrames);
        mixSig(sig, (uint64_t)c.fadeOutFrames);
        mixSig(sig, (c.audio || timeline_.clipDisabled(c)) ? 1u : 0u);
        mixSig(sig, (uint64_t)std::hash<std::string>()(c.mediaId));
    });
    // Every sequence, not just the viewed ones: transitions are few, and being
    // over-inclusive here costs a resubmit that changes nothing.
    for (const auto& sq : timeline_.sequences)
        for (const auto& t : sq.transitions) {
            mixSig(sig, (uint64_t)t.aClipId);
            mixSig(sig, (uint64_t)t.bClipId);
            mixSig(sig, (uint64_t)t.inFrames);
            mixSig(sig, (uint64_t)t.outFrames);
        }
    const double nowMs = (double)SDL_GetTicks();
    if (sig == cacheReqSig_ && nowMs - cacheReqAtMs_ < kPrefetchRefreshMs)
        return;
    cacheReqSig_ = sig;
    cacheReqAtMs_ = nowMs;
    cache_->beginRequests();

    // A frame inside a dissolve needs both halves resident to composite, so it
    // costs two requests instead of one. The forward window is unchanged, which
    // means it reaches half as far in wall-clock across a span — spans are a second
    // or so, so that is cheaper than special-casing the prefetch budget.
    // The Layout stage shows every track at once, so every one of them has to be
    // resident — not just the program's one-or-two layers. All tiles of a frame go
    // in at the same priority, so no tile starves behind another's look-ahead.
    // Scratch buffers hoisted out of the loop: issueFrame runs once per prefetched
    // frame and would otherwise allocate on each.
    std::vector<const Clip*> tileClips;
    auto issueFrame = [&](int64_t f, int prio) {
        auto request = [&](const Clip* c, int64_t sf) {
            if (!c || c->mediaId.empty())
                return;
            auto pm = timeline_.findMediaById(c->mediaId);
            if (!pm || pm->openFailed())
                return;
            cache_->request(CacheKey{ c->mediaId, sf }, pm, sf, prio);
        };
        ProgramSource ps = programSourceAt(f);
        request(ps.a, ps.aSrc);
        request(ps.b, ps.bSrc);
        // The tracks below the program are wanted on both comparison stages: on the
        // Layout they are on screen, and on the Stack any of them is one Up/Down
        // away, which has to land on a frame that is already there. ps.a is the
        // program and was asked for above; it keeps its dissolve halves either way,
        // so a cut on the top track still composites while the stage is up.
        if (compareStage()) {
            layoutClipsAt(f, tileClips);
            for (const Clip* tc : tileClips)
                if (tc && tc != ps.a) // null: that row has no clip at this frame
                    request(tc, tc->sourceOffset + (f - tc->timelineStart));
        }
    };
    auto requestFrame = [&](int64_t f, int prio) {
        if (f < lo || f > hi)
            return;
        issueFrame(f, prio);
    };
    // Prefetch is always forward-biased: a large look-ahead plus a small
    // look-behind. We deliberately do NOT follow scrub direction. Playback only
    // ever moves forward, so flipping to a large backward window while scrubbing
    // back would evict the forward cache (which playback depends on) just to hold
    // frames we are scrubbing away from. The small behind window still gives
    // reverse scrubbing some smoothness without trashing the forward cache.
    // The playhead itself bypasses the range filter: the indicator can be parked
    // outside the in/out range (scrubbing, arrow keys), and the player has to show
    // that frame rather than hold the last in-range one. Prefetch stays confined,
    // so a frame outside the range costs one decode and evicts nothing extra.
    issueFrame(timeline_.playhead, 0);
    for (int d = 1; d <= ahead; ++d) {
        int64_t f = timeline_.playhead + (int64_t)d;
        if (wrap && f > hi)
            f = lo + (f - lo) % span; // fold the look-ahead back into [lo, hi]
        requestFrame(f, d);
    }
    for (int d = 1; d <= behind; ++d)
        requestFrame(timeline_.playhead - (int64_t)d, ahead + d);
    cache_->endRequests();
}

// ---------------------------------------------------------------- rendering

// (Re)rasterize the UI fonts at the current device scale. destroy() first: load()
// over a live font leaks it and leaves the string cache holding texture handles that
// belong to the previous renderer.
void App::reloadUiFonts() {
    const char* base = SDL_GetBasePath();
    std::string fontPath = std::string(base ? base : "") + "Inter-Regular.ttf";
    textFont_.destroy();
    headerFont_.destroy();
    if (!textFont_.load(renderer_, fontPath.c_str(), 10.0f, uiScale_))
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "TextFont: falling back to debug font");
    headerFont_.load(renderer_, fontPath.c_str(), 15.0f, uiScale_);
    reloadOverlayFont();
}

// The burn-in font, opened at the size the View > Overlay Options > Size choice
// asks for. Drawing never stretches it, so this is the only place its size is set:
// the Size rows and a UI-scale change both come back through here.
void App::reloadOverlayFont() {
    const char* base = SDL_GetBasePath();
    std::string fontPath = std::string(base ? base : "") + "Inter-Regular.ttf";
    overlayFont_.destroy();
    overlayFont_.load(renderer_, fontPath.c_str(), overlayFontPt(), uiScale_);
}

// In window coordinates, which are device pixels on Windows: keep the smallest
// window the same 640x360 of *layout* at every scale, so raising the UI Scale never
// leaves a window that can be shrunk until the chrome has nowhere to go.
void App::applyWindowMinimumSize() {
    const float density = SDL_GetWindowPixelDensity(window_);
    const float toWindow = uiScale_ / (density > 0.0f ? density : 1.0f);
    SDL_SetWindowMinimumSize(window_, (int)std::lround(640 * toWindow),
                             (int)std::lround(360 * toWindow));
}

// The window moved to a display with a different DPI, or the UI Scale preference
// changed. The layout needs no work — it is in logical units and computeLayout()
// picks up the new scale on the next frame — but the glyph atlases are rasterized
// for a specific density, so they have to be rebuilt or text would end up soft (or
// crisp but wrongly sized).
void App::setUiScale(float scale) {
    if (scale <= 0.0f || std::fabs(scale - uiScale_) < 0.001f)
        return;
    uiScale_ = scale;
    reloadUiFonts();
    applyWindowMinimumSize();
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "UI scale changed: %.2f", uiScale_);
}

// Cinema mode: the window goes fullscreen and every band of chrome steps out of
// the way, leaving the program image — plus whichever frame overlays were already
// on — filling the display. F11 toggles it, Esc leaves it, and so does the row at
// the top of the View menu.
//
// The docked panes are closed rather than merely left undrawn: their hit regions —
// the resize edges, the row clicks — are driven by the open flags and not by what
// render() painted, so a pane left open behind the image would go on swallowing
// clicks aimed at the frame. Whichever one it was is reopened on the way out.
void App::setCinemaMode(bool on) {
    if (on == cinemaMode_ || (on && launcherVisible()))
        return;
    cinemaMode_ = on;
    if (on) {
        static const LeftPanel kPanes[] = {
            LeftPanel::ProjectExplorer, LeftPanel::ClipSource, LeftPanel::Grade,
            LeftPanel::Tech, LeftPanel::Draw, LeftPanel::Sync, LeftPanel::Settings,
        };
        cinemaPanel_ = -1;
        for (LeftPanel p : kPanes) {
            if (leftPanelOpen(p)) { cinemaPanel_ = (int)p; break; }
        }
        if (cinemaPanel_ >= 0)
            toggleLeftPanel((LeftPanel)cinemaPanel_); // a pane's own entry closes it
        cinemaInspector_ = inspectorOpen_;
        inspectorOpen_ = false;
        closeTopBarPopups(); // anchored to a toolbar that is about to be gone
    } else {
        if (cinemaPanel_ >= 0)
            toggleLeftPanel((LeftPanel)cinemaPanel_);
        cinemaPanel_ = -1;
        inspectorOpen_ = cinemaInspector_;
    }
    // SDL3 fullscreens against the display the window is already on, and with no
    // mode set it takes the desktop's — so no resolution switch, and nothing that
    // would move the window to another display and force a renderer rebuild.
    SDL_SetWindowFullscreen(window_, on);
}

// Compact timeline (TAB, or View ▸ Compact Timeline): the timeline keeps its info
// bar, ruler and cache strip — enough to scrub, transport and read the cache — and
// drops the sequence bar, the shot bar and the track stack, handing that height to
// the player. computeLayout() does all of it; the collapsed bands take their hit
// regions with them, since those are bounded by the rects it zeroes.
void App::setCompactTimeline(bool on, bool closePanes, bool persist) {
    if (on == compactTimeline_)
        return;
    // The comparison stages run compact: the image is the whole point of them, and
    // the track stack under it is one row per tile / per stack entry — nothing the
    // stage doesn't already say. So TAB can't put the tracks back from in there
    // (the View row is greyed for the same reason); leave the stage and the key
    // works again.
    if (!on && compareStage()) {
        setStatusWarn(stackView() ? "STACK: TIMELINE STAYS COMPACT (F5 TO LEAVE)"
                                  : "LAYOUT: TIMELINE STAYS COMPACT (F4 OR L TO LEAVE)",
                      2000);
        return;
    }
    compactTimeline_ = on;
    // A drag in flight was sizing a timeline that is about to stop being resizable.
    tlResizing_ = false;
    // Compact hides the tool buttons, so a razor left armed here would be a mode
    // with no way out but the keyboard. Drop back to the cursor on the way in.
    // An inline track rename goes the same way: its row (and the gutter the field
    // sits in) is about to collapse, leaving the field floating over the ruler.
    if (on) {
        setTimelineTool(TimelineTool::Cursor);
        commitTrackNameEdit();
    }
    if (on && closePanes) {
        // Compact is a request for the image, so the docked pane beside it goes as
        // well. Unlike cinema mode there is nothing to put back on the way out: the
        // panes stay a click away, and one opened while compact stays open.
        for (LeftPanel p : { LeftPanel::ProjectExplorer, LeftPanel::ClipSource,
                             LeftPanel::Grade, LeftPanel::Tech, LeftPanel::Draw,
                             LeftPanel::Sync, LeftPanel::Settings }) {
            if (leftPanelOpen(p)) {
                toggleLeftPanel(p); // a pane's own entry closes it
                break;              // they are mutually exclusive: that was the one
            }
        }
    }
    setStatus(on ? "COMPACT TIMELINE" : "FULL TIMELINE", 1500);
    // Only a state the user asked for is worth remembering across launches: a
    // comparison stage holding the timeline compact is the stage's doing, and
    // leaving one is not a request to launch full either.
    if (persist)
        writePrefs();
}

// The way out of a comparison stage. Entering one forced compact (setPlayerStage),
// and while in there TAB cannot undo it, so the exit is the only place that can put
// the tracks back - and only for a user who was not compact to begin with, whose
// TAB state the stage borrowed. Called from both paths off a stage: the ones that
// name the stage they want, and the ones that just drop the scratch sequence.
void App::restoreCompactAfterCompare() {
    if (compareStage() || compactBeforeCompare_)
        return;
    setCompactTimeline(false, /*closePanes=*/false, /*persist=*/false);
}

// SDL_GetMouseState reports window coordinates, but everything they get tested
// against is laid out in logical units — the two differ whenever uiScale_ != 1.
// (Event coordinates arrive already converted, in handleEvent.)
void App::uiMouse(float& x, float& y) const {
    SDL_GetMouseState(&x, &y);
    SDL_RenderCoordinatesFromWindow(renderer_, x, y, &x, &y);
}

void App::computeLayout() {
    // Everything below lays out in logical units; the renderer scales them to the
    // window's real pixels. Recomputed every frame so a resize, a renderer rebuild
    // (HDR toggle) or a move to another display all heal themselves here: the
    // logical size tracks the window so resizing reveals more content rather than
    // magnifying what is there.
    int pxW = 0, pxH = 0;
    SDL_GetRenderOutputSize(renderer_, &pxW, &pxH); // always device pixels
    const int logW = (int)std::max(1L, std::lround(pxW / uiScale_));
    const int logH = (int)std::max(1L, std::lround(pxH / uiScale_));
    int curW = 0, curH = 0;
    SDL_RendererLogicalPresentation curMode = SDL_LOGICAL_PRESENTATION_DISABLED;
    SDL_GetRenderLogicalPresentation(renderer_, &curW, &curH, &curMode);
    if (curW != logW || curH != logH || curMode != SDL_LOGICAL_PRESENTATION_STRETCH) {
        // STRETCH, not LETTERBOX: the logical size is derived from the real one, so
        // the aspect already matches to within the half-pixel the rounding above can
        // introduce. Letterboxing that would show as a clear-colored hairline.
        SDL_SetRenderLogicalPresentation(renderer_, logW, logH,
                                         SDL_LOGICAL_PRESENTATION_STRETCH);
    }
    winW_ = (float)logW;
    winH_ = (float)logH;
    // The scale the drawing primitives snap hairlines against (PixelSnap.h). Taken
    // from the logical size actually in force rather than from uiScale_: the
    // rounding above can leave the two a hair apart, and it is this ratio that maps
    // a logical coordinate onto a device pixel.
    gDeviceScale = logW > 0 ? (float)pxW / (float)logW : 1.0f;
    titleBar_.layout(winW_);
    // Cinema mode zeroes every band of chrome from here down, which is what leaves
    // playerRect_ covering the whole window; render() skips drawing them to match.
    float topH = cinemaMode_ ? 0.0f : titleBar_.height();
    // Menu lives inside the title bar, shifted right to clear the app icon.
    menuBar_.layout(winW_, 0.0f, topH, kMenuLeftInset, dpiScale);

    // Left panels reserve a strip on the left; the player and timeline begin at
    // their right edge so the panels run the full height under the title bar.
    // All left-dock panels share one width (the tech-check value) so switching
    // between them never shifts the player/timeline. peW_ and clipSourceW_ stay
    // user-resizable, each keeping its own dragged width.
    const float leftPanelW = std::clamp(std::round(240.0f * dpiScale), 210.0f, 320.0f);
    peW_ = peUserW_ > 0.0f
        ? std::clamp(peUserW_, 140.0f, winW_ * 0.5f)
        : leftPanelW;
    gradeW_ = leftPanelW;
    techW_ = leftPanelW;
    settingsW_ = leftPanelW;
    sessionW_ = leftPanelW;
    clipSourceW_ = clipSourceUserW_ > 0.0f
        ? std::clamp(clipSourceUserW_, 140.0f, winW_ * 0.5f)
        : leftPanelW;

    bool emptyProject = launcherVisible();
    // The launcher and cinema mode collapse the same set of bands, for opposite
    // reasons: one has no project to show them for, the other wants them gone.
    const bool noChrome = emptyProject || cinemaMode_;
    float leftX = noChrome ? 0.0f
                  : kSidePanelW + (projectExplorerOpen_ ? peW_
                                   : (gradeOpen_ ? gradeW_
                                      : (techOpen_ ? techW_
                                         : (clipSourceOpen_ ? clipSourceW_
                                            : (settingsOpen_ ? settingsW_
                                               : (sessionPanelOpen_ ? sessionW_
                                                  : (pencilMode_ ? leftPanelW : 0.0f)))))));
    panelsRight_ = leftX;
    float rightX = winW_; // no right dock; the inspector overlays the frame

    ensureTrailingEmptyTrack();
    // Curve mode's lane height, resolved before anything asks trackRowH so the
    // whole layout sees one value. curveLaneHeight() measures the stack the
    // normal way (it must not consult trackRowH, which is already in curve mode),
    // and a mode whose track has gone away leaves by itself.
    if (curveTrack_ >= 0) {
        if (curveTrack_ >= trackCount() ||
            timeline_.trackKind(curveTrack_) == Timeline::TrackKind::Empty)
            exitCurveMode();
        else
            curveLaneH_ = curveLaneHeight();
    }
    // The sequence and shot bars (between the cache strip and the tracks) only
    // take vertical space when the project actually has sequences / shots.
    // The sequence bar is shown whenever the project has more than one sequence,
    // in both the All view (a span per sequence) and the focused view (the single
    // focused span, whose hover affordance is the exit-to-All icon).
    // Compact mode drops both bars along with the track stack below them, leaving
    // the info bar, ruler and cache strip.
    float seqH  = (!compactTimeline_ && timeline_.sequences.size() > 1) ? kSeqBarH : 0.0f;
    // An unnamed shot draws a blank block, so a project whose shots are all
    // nameless leaves the bar reading as an empty strip: drop it as well.
    const bool namedShots = std::any_of(timeline_.shots.begin(), timeline_.shots.end(),
                                        [](const Shot& s) { return !s.name.empty(); });
    float shotH = (compactTimeline_ || !namedShots) ? 0.0f : kShotBarH;
    // ... and gains the flattened clip band in their place, the cache strip's twin:
    // with the rows gone it is the only reading of where the clips and cuts are.
    float clipStripH = compactTimeline_ ? kCacheStripH : 0.0f;
    // Timeline height: the fixed bands, plus as much of the track stack as it is
    // allowed to show. Two limits bound it — it never takes more than
    // kTimelineMaxFrac of the window (so a deep stack cannot squeeze the player
    // away), and never more than its own content needs (so there is no dead strip
    // under the last row). Whatever is left over scrolls; see trackScroll_.
    const float tlFixedH = kInfoH + kRulerH + clipStripH + kCacheStripH + seqH + shotH + kTimelinePad;
    const float tlOneRowH = tlFixedH + kTrackH * 0.5f; // bands + the shortest row
    // Compact: the bands are the whole timeline, so the height is fixed at them and
    // the drag limits collapse onto it — there is nothing left to resize, and
    // tlUserH_ is left untouched so the dragged height comes back on the way out.
    tlMaxH_ = compactTimeline_
            ? tlFixedH
            : std::min(tlFixedH + tracksTotalH(), std::max(winH_ * kTimelineMaxFrac, tlOneRowH));
    tlMinH_ = compactTimeline_ ? tlFixedH : std::min(tlOneRowH, tlMaxH_);
    float tlH = noChrome ? 0.0f
              : (compactTimeline_ ? tlFixedH
                 : (tlUserH_ > 0.0f ? std::clamp(tlUserH_, tlMinH_, tlMaxH_) : tlMaxH_));
    // The timeline always spans the full window width along the bottom; only the
    // player is inset by the left panels. panelsBottom_ marks the timeline's top
    // so the panels (icon strip + open panel) stop there.
    panelsBottom_ = winH_ - tlH;
    tlRect_ = { 0.0f, panelsBottom_, winW_, tlH };
    // Top-toolbar button widths, measured here — before the bar's own rect — because
    // the bar wraps to a second row once its two groups no longer fit side by side,
    // and the wrapped height is what pushes the video frame down. Measuring once also
    // keeps the wrap test and the placement below reading the same numbers, so they
    // cannot drift apart. The wrap depends only on the bar's width and the labels,
    // never on its height, so there is no feedback loop between the decision and its
    // consequence — no oscillation as the window is resized.
    // A zero width means the button is not on offer this frame; placement turns that
    // into an empty rect, which is how renderTopBar and the hit tests read "absent".
    const float tbBtnH = 18.0f * dpiScale;
    const float tbGap  = 6.0f * dpiScale;
    float wSeq = 0.0f, wOpenProj = 0.0f, wOpenSeq = 0.0f;
    float wLetterbox = 0.0f, wProxy = 0.0f, wDisp = 0.0f, wView = 0.0f, wLook = 0.0f, wInputCs = 0.0f;
    bool topBarTwoRow = false;
    if (!noChrome) {
        // The two "Open …" shortcuts are only there when something is actually there
        // to open. Resolved here rather than in the render pass because the button
        // labels are what size them; cache-only while playing, so this costs a map
        // lookup.
        resolveOpenTargets(!playing_);
        // Sequence view filter. Its label is the scoped sequence or project name (or
        // "All"), so the button width tracks the current selection, with a generous
        // minimum so short names still read as a dropdown.
        wSeq = std::max(iconTextBtnW(sequenceViewLabel(), tbBtnH, true), 200.0f * dpiScale);
        if (!openProjName_.empty())
            wOpenProj = iconTextBtnW("Open Project: " + openProjName_, tbBtnH);
        if (!openSeqName_.empty())
            wOpenSeq = iconTextBtnW("Open Sequence: " + openSeqName_, tbBtnH);
        // Letterbox is icon-only (see renderTopBar), so its width is the glyph square
        // plus the strip the caret needs — the same terms iconTextBtnW would give an
        // empty label, without asking the font to measure one.
        wLetterbox = tbBtnH + (10.0f + kTopBarCaretW) * dpiScale;
        // Off entirely unless Settings > Enable Proxy is on (see applyProxyMode).
        if (proxyEnabled_)
            wProxy = iconTextBtnW(proxyModeLabel(), tbBtnH, true);
        // The OCIO pickers are hidden when color management is set to sRGB.
        if (ocio_.isReady() && ocio_.isEnabled()) {
            wDisp = iconTextBtnW(ocio_.activeDisplay().empty() ? "Display"
                                                              : ocio_.activeDisplay(), tbBtnH, true);
            wView = iconTextBtnW(ocio_.activeView().empty() ? "View"
                                                            : ocio_.activeView(), tbBtnH, true);
            // Look button only when the config offers a real choice (>1 named Look);
            // with none or a single Look there is nothing to pick between.
            if (ocio_.looks().size() > 1)
                wLook = iconTextBtnW(ocio_.activeLook().empty() ? "Look"
                                                                : ocio_.activeLook(), tbBtnH, true);
            wInputCs = iconTextBtnW(ocioInputCsLabel(), tbBtnH, true);
        }
        // Each group's total, gaps included, exactly as placed below — the left group
        // trails a gap after every button, the right group leads one before every
        // button past the rightmost. Both groups run flush to the bar's edges.
        auto plus = [&](float w) { return w > 0.0f ? w + tbGap : 0.0f; };
        const float tbLeftW  = wSeq + tbGap + plus(wOpenProj) + plus(wOpenSeq);
        const float tbRightW = wLetterbox + plus(wProxy)
                             + plus(wInputCs) + plus(wLook) + plus(wView) + plus(wDisp);
        topBarTwoRow = tbLeftW + tbRightW > rightX - leftX;
    }
    // Top toolbar: a thin bar between the title bar and the video frame, spanning
    // the frame width (right of the left panels, which run full height). The frame
    // begins below it. Hidden (zero height) while the launcher fills the player, and
    // twice as tall when the buttons had to wrap onto a second row.
    float topBarH = noChrome ? 0.0f : kTopBarH * (topBarTwoRow ? 2.0f : 1.0f);
    topBarRect_ = { leftX, topH, rightX - leftX, topBarH };
    float frameTop = topH + topBarH; // top of the video frame (below the title bar + top toolbar)
    // Draw-tool panel: a mutually-exclusive left pane (like Overview / Grade), so it
    // sits flush against the icon strip and the frame begins at its right edge. Its
    // width is already folded into leftX above; anchor it at kSidePanelW here.
    float drawPanelW = (pencilMode_ && !noChrome) ? leftPanelW : 0.0f;
    // Top-aligned with the other left panels (explorer/grade/tech): from just below
    // the title bar down to the timeline, running alongside the top toolbar — not
    // below it like the video frame.
    drawPanelRect_ = { kSidePanelW, topH, drawPanelW, panelsBottom_ - topH };
    playerRect_ = { leftX, frameTop, rightX - leftX, winH_ - tlH - frameTop };
    infoRect_ = { 0.0f, tlRect_.y, winW_, kInfoH };
    {
        const float bw = 24.0f * dpiScale, bh = 18.0f * dpiScale, gap = 6.0f * dpiScale;
        // One button-height band down the middle of the info bar; the two groups
        // below each cut their buttons off their own copy of it.
        const SDL_FRect band = centerV(infoRect_, bh);

        // Left group: the snap toggle, an icon+label button off a 2px lead-in so it
        // is not flush against the window edge; its draw code is in App_Timeline.cpp
        // (see App::drawFrame's info bar) and sizes from the rect placed here.
        SDL_FRect left = band;
        gapLeft(left, 2.0f * dpiScale);
        magnetBtnRect_ = cutLeft(left, iconTextBtnW("Snap", bh));
        // The zoom-fit button sits immediately right of it; layoutFitButtons (called
        // at the end of this function, once the zoom it reads is settled) fills this
        // slot, or leaves it empty when there is nothing to fit. The two read as one group, so the gap
        // between them is the same 2px as the lead-in. The slot is reserved whether
        // or not there is anything to fit, so the buttons after it never shift when
        // media arrives.
        gapLeft(left, 2.0f * dpiScale);
        fitBarBand_ = cutLeft(left, iconTextBtnW(kFitBtnLabel, bh));
        // Tool-mode pair: square icon-only buttons (no label, so a plain
        // button-height square each), touching so they read as one radio group
        // rather than two independent toggles. Set apart from the two view controls
        // by a wider gap, since choosing a tool is a different kind of thing.
        // Gone in compact mode: there are no track rows to point a tool at, so the
        // empty rects retire the hit tests and the drawing alike.
        if (compactTimeline_) {
            cursorToolBtnRect_ = {};
            razorToolBtnRect_  = {};
        } else {
            gapLeft(left, 14.0f * dpiScale);
            cursorToolBtnRect_ = cutLeft(left, bh);
            razorToolBtnRect_  = cutLeft(left, bh);
        }

        // Transport controls: prev-clip / play-pause / next-clip, centered.
        SDL_FRect mid = band;
        gapLeft(mid, (band.w - (bw * 3.0f + gap * 2.0f)) * 0.5f);
        prevClipBtnRect_ = cutLeft(mid, bw); gapLeft(mid, gap);
        playBtnRect_     = cutLeft(mid, bw); gapLeft(mid, gap);
        nextClipBtnRect_ = cutLeft(mid, bw);

        // Right group: master volume, hugging the right edge of the bar — speaker
        // first, then its slider. Cut right-to-left, so the slider is the outermost
        // slice. The band is one button tall, so the slider's hit region is that
        // tall too and the thin track it draws inside stays easy to grab.
        SDL_FRect right = band;
        gapRight(right, 12.0f * dpiScale);
        volumeSliderRect_ = cutRight(right, 64.0f * dpiScale);
        gapRight(right, gap);
        volumeBtnRect_ = cutRight(right, bw);
    }
    // Top-toolbar buttons (icon + label), vertically centered in their row. Sharing
    // one row, the Sequence group is left-aligned and the color group right-aligned;
    // wrapped, they take a row each and both align left. All of them open a downward popup,
    // which anchors itself off the button rect — so a second-row button needs no
    // special handling.
    if (noChrome) {
        ocioDisplayBtnRect_ = {}; // no toolbar while the launcher is up, or in cinema mode
        ocioViewBtnRect_ = {};
        ocioLookBtnRect_ = {};
        ocioInputCsBtnRect_ = {};
        proxyBtnRect_ = {};
        letterboxBtnRect_ = {};
        sequenceBtnRect_ = {};
        openProjBtnRect_ = {};
        openSeqBtnRect_ = {};
    } else {
        // One row per group when the bar wrapped, both groups sharing the single row
        // when it did not. Row 1 always carries the Sequence group, so the view
        // filter stays put as the window narrows and only the color controls drop.
        SDL_FRect rows = topBarRect_;
        SDL_FRect row1 = cutTop(rows, kTopBarH);
        SDL_FRect row2 = topBarTwoRow ? cutTop(rows, kTopBarH) : row1;
        // Left group, in order: the Sequence view filter, then the two "Open …"
        // shortcut buttons. Widths come from the measurement pass above; a zero
        // width is a button that is not on offer, so it consumes nothing here.
        SDL_FRect left = centerV(row1, tbBtnH);
        auto nextLeft = [&](float w) -> SDL_FRect {
            if (w <= 0.0f)
                return SDL_FRect{};
            SDL_FRect r = cutLeft(left, w);
            gapLeft(left, tbGap);
            return r;
        };
        sequenceBtnRect_ = nextLeft(wSeq);
        openProjBtnRect_ = nextLeft(wOpenProj);
        openSeqBtnRect_ = nextLeft(wOpenSeq);
        // Color group, reading left-to-right as Display, View, Look, File
        // Colorspace, Letterbox. On a shared row it hugs the right edge, so it is
        // placed from there inward — Letterbox first (rightmost), the OCIO pickers
        // to its left. On its own wrapped row it is left-aligned instead, under the
        // Sequence group, so the bar reads as two flush columns rather than one row
        // pushed to the far edge.
        SDL_FRect color = centerV(row2, tbBtnH);
        if (topBarTwoRow) {
            auto nextColor = [&](float w) -> SDL_FRect {
                if (w <= 0.0f)
                    return SDL_FRect{};
                SDL_FRect r = cutLeft(color, w);
                gapLeft(color, tbGap);
                return r;
            };
            ocioDisplayBtnRect_ = nextColor(wDisp);
            ocioViewBtnRect_ = nextColor(wView);
            ocioLookBtnRect_ = nextColor(wLook);
            ocioInputCsBtnRect_ = nextColor(wInputCs);
            proxyBtnRect_ = nextColor(wProxy);
            letterboxBtnRect_ = nextColor(wLetterbox);
        } else {
            letterboxBtnRect_ = cutRight(color, wLetterbox);
            auto nextColor = [&](float w) -> SDL_FRect {
                if (w <= 0.0f)
                    return SDL_FRect{};
                gapRight(color, tbGap);
                return cutRight(color, w);
            };
            proxyBtnRect_ = nextColor(wProxy);
            ocioInputCsBtnRect_ = nextColor(wInputCs);
            ocioLookBtnRect_ = nextColor(wLook);
            ocioViewBtnRect_ = nextColor(wView);
            ocioDisplayBtnRect_ = nextColor(wDisp);
        }
    }
    // The timeline's fixed bands, stacked below the info bar, with the track rows
    // starting where the last of them ends. Cut off an unbounded band rather than
    // tlRect_ itself: on an empty project tlH collapses to zero, and cuts clamp,
    // so bounding the stack there would flatten every band onto the same y.
    SDL_FRect bands = { 0.0f, infoRect_.y + kInfoH, winW_, kUnbounded };
    rulerRect_      = cutTop(bands, kRulerH);
    clipStripRect_  = cutTop(bands, clipStripH); // zero-height outside compact mode
    cacheStripRect_ = cutTop(bands, kCacheStripH);
    seqBarRect_     = cutTop(bands, seqH);   // zero-height with one sequence
    shotBarRect_    = cutTop(bands, shotH);  // zero-height with no shots
    tracksTop_ = bands.y;
    // What is left of the timeline below the bands is the track viewport; the pad
    // at the very bottom is not part of it. Anything past it scrolls.
    tracksViewH_ = std::max(0.0f, tlRect_.y + tlH - kTimelinePad - tracksTop_);
    trackScroll_ = std::clamp(trackScroll_, 0.0f, tracksMaxScroll());
    // Prefix-sum the (variable) row heights once, so trackRowY() is an O(1)
    // lookup in the per-clip render path (see App.h::trackRowTop_). The scroll is
    // folded in here, which is what makes every caller of trackRowY() scroll with
    // the stack without knowing about it.
    {
        int nt = std::max(trackCount(), 1);
        // Heights first, and derived with the cache emptied: trackRowH() reads
        // trackRowH_, so leaving last frame's answer in place would just copy it
        // forward and a row that changed type would never resize.
        std::vector<float> heights((size_t)nt);
        trackRowH_.clear();
        for (int t = 0; t < nt; ++t) heights[(size_t)t] = trackRowH(t);
        trackRowH_ = std::move(heights);
        float y = tracksTop_ - trackScroll_;
        trackRowTop_.assign(nt + 1, y);
        for (int t = 0; t < nt; ++t) { trackRowTop_[t] = y; y += trackRowH_[(size_t)t]; }
        trackRowTop_[nt] = y;
    }
    // Compact mode collapses the track-label gutter (headerW), so the ruler,
    // cache strip and flattened clip band start at the window's left edge.
    headerX_ = tlRect_.x + headerW();
    contentW_ = winW_ - headerW();

    // Keep the fitted content stretched to the timeline width: when the content
    // area changes width (window resize, panel toggle), rescale the zoom so the
    // same frame range stays in view instead of revealing more frames on the
    // right. viewStart_ (the left-edge frame) is left anchored.
    if (viewInitialized_ && viewContentW_ > 0.0f && contentW_ > 0.0f
        && std::fabs(contentW_ - viewContentW_) > 0.5f) {
        framesPerPx_ *= (double)viewContentW_ / (double)contentW_;
    }
    viewContentW_ = contentW_;

    // Left icon strip: one column of toggles, anchored just below the title
    // bar and stacked in the order listed here. The column ends at the timeline,
    // not at the window bottom: a short window would otherwise carry on stacking
    // toggles down across the timeline's info bar. Once the
    // room runs out the remaining toggles are skipped outright rather than
    // squashed — a zero rect draws no glyph and hit-tests false, so the button
    // simply is not there at that window height.
    //
    // A button spans the full strip width and runs 4px taller than the square its
    // glyph is sized to, so the hover/active fill reads as a band across the strip.
    // Those 4px come out of the gap below, leaving the column pitch unchanged.
    // The glyph square is kNavIconSide, not derived from the strip width, so the
    // strip can be narrowed (its side padding is what shrinks) without the icons
    // following it down.
    const float pad = 8.0f * dpiScale; // top inset and the gap between buttons
    const float btnSide = kNavIconSide * dpiScale;
    const float btnH = btnSide + 4.0f * dpiScale;
    SDL_FRect strip = { 0.0f, topH + pad, kSidePanelW, panelsBottom_ - topH - pad };
    auto nextStripBtn = [&]() -> SDL_FRect {
        if (strip.h < btnH)
            return SDL_FRect{}; // no whole button's worth of room left
        SDL_FRect r = cutTop(strip, btnH);
        gapTop(strip, pad - 4.0f * dpiScale);
        return r;
    };
    dirButtonRect_ = nextStripBtn();
    // Clip Source and the project explorer are both media-oriented panels, so
    // their toggles sit together at the top.
    clipSourceButtonRect_ = nextStripBtn();
    gradeButtonRect_ = nextStripBtn();
    techButtonRect_ = nextStripBtn();
    pencilBtnRect_ = nextStripBtn();      // freehand markup
    sessionButtonRect_ = nextStripBtn();  // sync-review session
    settingsButtonRect_ = nextStripBtn(); // cog
    if (projectExplorerOpen_) {
        const float btn = 18.0f;
        float by = topH + pad + 22.0f; // below the "PROJECT" header line
        peAddRect_ = { kSidePanelW + pad, by, btn, btn };
        peRemoveRect_ = { kSidePanelW + pad + btn + 6.0f, by, btn, btn };
    } else {
        peAddRect_ = peRemoveRect_ = SDL_FRect{};
    }

    if (!viewInitialized_ && timeline_.hasClips())
        fitView();

    // A project/otio loaded before any layout (e.g. passed on the command line)
    // fit against contentW_==0; re-fit now that the real width is known.
    if (pendingFit_ && contentW_ > 0.0f) {
        fitToFilteredSequence();
        pendingFit_ = false;
    }

    // Last: the fit buttons key off the zoom this function may just have changed,
    // and off the info-bar band it left free for them above.
    layoutFitButtons();
}

void App::drawText(float x, float y, SDL_Color c, const std::string& s) {
    textFont_.draw(renderer_, x, y, c, s.c_str());
}

float App::iconTextBtnW(const std::string& label, float h, bool caret) const {
    return h + textFont_.measure(renderer_, label.c_str())
             + (caret ? 10.0f + kTopBarCaretW : 10.0f) * dpiScale;
}

// A closed picker menu cancels its in-flight query: bump the id so the pending
// completion drops its (now stale) result. The background call still finishes.
void App::cancelPickerIfClosed() {
    if (clipMenu_.isOpen())
        return;
    if (menuCascade_.loading) {
        ++menuCascade_.queryId;
        menuCascade_.loading = false;
    }
    // Decoration deliberately leaves queryId alone, so a close is the only thing
    // that stops it: without this a menu dismissed a second after opening would
    // leave the site's lookup grinding through a hundred versions for nothing.
    // (The panel's close bumps its queryId, which pollPickerDecorations reads.)
    cancelPickerDecorate(menuCascade_);
}

// Animated overlay over an open picker view while its click's Python query
// resolves, so it reads as busy rather than frozen. The loop keeps running
// (the query is off-thread), so the dots cycle: "LOADING ." -> ".." -> "...".
void App::renderPickerLoading(const PickerCascade& c, const SDL_FRect& b) {
    if (!c.loading)
        return;
    if (b.w <= 0.0f || b.h <= 0.0f)
        return;
    // Translucent dim over the still-visible columns (which are frozen — the motion
    // handler stops tracking hover while loading, so they don't react to the cursor).
    SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_BLEND);
    setColor(renderer_, SDL_Color{ 17, 18, 21, 210 });    // dim the columns underneath
    jplay::fillRect(renderer_, &b);
    setColor(renderer_, SDL_Color{ 150, 165, 190, 255 }); // matches the hovered column outline
    jplay::drawRect(renderer_, &b);
    const int dots = 1 + (int)((SDL_GetTicks() / 400) % 3); // 1..3, ~2.5 Hz cycle
    const std::string msg = "LOADING " + std::string(dots, '.');
    // Center on the fixed 3-dot width so the label doesn't jitter as dots change.
    float tw = textFont_.measure(renderer_, "LOADING ...");
    float th = textFont_.lineHeight();
    textFont_.draw(renderer_, b.x + (b.w - tw) * 0.5f, b.y + (b.h - th) * 0.5f,
                   SDL_Color{ 255, 230, 150, 255 }, msg.c_str());
}

std::string App::fitText(const std::string& s, float maxW) {
    if (maxW <= 0.0f)
        return std::string();
    if (textFont_.measure(renderer_, s.c_str()) <= maxW)
        return s;
    // Binary search the longest fitting prefix.
    size_t lo = 0, hi = s.size();
    while (lo < hi) {
        size_t mid = (lo + hi + 1) / 2;
        if (textFont_.measure(renderer_, s.substr(0, mid).c_str()) <= maxW)
            lo = mid;
        else
            hi = mid - 1;
    }
    return s.substr(0, lo);
}

std::vector<std::string> App::wrapText(const std::string& s, float maxW) {
    std::vector<std::string> lines;
    if (maxW <= 0.0f || s.empty())
        return lines;
    std::string line;
    for (size_t i = 0; i <= s.size();) {
        size_t sp = s.find(' ', i);
        size_t end = (sp == std::string::npos) ? s.size() : sp;
        std::string word = s.substr(i, end - i);
        std::string cand = line.empty() ? word : line + " " + word;
        // Only break once the line has something on it; a lone over-wide word
        // stays put rather than looping forever on an empty line.
        if (!line.empty() && textFont_.measure(renderer_, cand.c_str()) > maxW) {
            lines.push_back(line);
            line = word;
        } else {
            line = cand;
        }
        if (sp == std::string::npos)
            break;
        i = sp + 1;
    }
    if (!line.empty())
        lines.push_back(line);
    return lines;
}

void App::render() {
    // Invalidate the per-frame playhead-clip memo; panel renderers below share it.
    // Cleared on the way out as well: outside a render pass the clip set is *not*
    // immutable (a drop that adds a clip reallocates Sequence::clips), so a memo
    // left armed here would hand the next off-pass reader - computeLayout() at the
    // top of the following drawFrame, or controlStateJson() - a dangling Clip*.
    playheadClipValid_ = false;
    struct MemoGuard { bool& v; ~MemoGuard() { v = false; } } memoGuard{ playheadClipValid_ };
    setColor(renderer_, kBlack);
    SDL_RenderClear(renderer_);
    renderPlayer();
    renderTechOverlay();              // false-color / clipping legend over the stage
    renderPixelInspector();           // pixel probe at the bottom left of the frame
    renderInfoOverlay();
    // While the launcher shows, the timeline (and its transport) and the left
    // side panel are hidden; only the recent/create launcher fills the player.
    // Cinema mode drops the same block, and the title bar below it: the program
    // image and whichever frame overlays were already on, and nothing else.
    if (!launcherVisible() && !cinemaMode_) {
        renderTopBar();               // top toolbar (Letterbox button) above the frame
        renderTimeline();
        renderSidePanel();            // left icon strip + directory toggle
        renderProjectExplorer();      // expanded source list, when open
        renderGradePanel();           // color grading tools, when open
        renderTechPanel();            // tech-check mode pills, when open
        updateClipSourceData();  // re-describe the pickers when the active media changed
        renderClipSourcePanel(); // stacked naming-config pickers, when open
        renderSettingsPanel();        // project / app settings, when open
        renderDrawPanel();            // draw-tool column, when pencil mode is on
        renderInspector();            // right-side inspector panel, when open
        renderSessionPanel();         // right-side SESSION panel (sync review), when open
        renderIconStripTooltips();    // icon-strip hover labels; over the open panel
        renderTopBarTooltips();       // top-toolbar hover labels; over the frame below the bar
        renderFramePreview();         // hovered-frame thumbnail above the ruler; on top of panels
    }
    if (!cinemaMode_) {
        titleBar_.render(renderer_);  // bar background + title + window controls
        drawAppIcon();                // app icon in the title bar, left of the menu
        menuBar_.render(renderer_);   // menu titles/dropdowns drawn into the bar
    }
    exportDialog_.render(renderer_, &textFont_, winW_, winH_); // modal overlay last
    renderRelocateModal();                                     // (mutually exclusive with export)
    clipMenu_.render(renderer_);                               // clip right-click popup, top-most
    renderPickerLoading(menuCascade_, clipMenu_.tableBounds()); // LOADING over a just-clicked picker column
    clipToolboxMenu_.render(renderer_);                        // info-bar toolbox popup, top-most
    fitMenu_.render(renderer_);                                // info-bar zoom-fit popup, top-most
    trackMenu_.render(renderer_);                              // track-header burger popup, top-most
    peSortMenu_.render(renderer_);                             // SOURCES bin sort-order popup, top-most
    peMediaMenu_.render(renderer_);                            // SOURCES bin right-click popup, top-most
    peSeqColorMenu_.render(renderer_);                         // sequence color palette popup, top-most
    appIconMenu_.render(renderer_);                            // title-bar icon window menu, top-most
    renderLetterboxMenu();                                     // letterbox aspect/opacity popup, top-most
    renderProxyMenu();                                         // proxy-mode popup, top-most
    renderOcioDisplayMenu();                                   // OCIO display-space popup, top-most
    renderOcioViewMenu();                                      // OCIO view-transform popup, top-most
    renderOcioLookMenu();                                      // OCIO look popup, top-most
    renderOcioInputCsMenu();                                   // OCIO file-colorspace popup, top-most
    renderSequenceMenu();                                      // sequence view-filter popup, top-most
    renderHelpPanel();                                         // keyboard-shortcuts overlay, top-most
    renderAboutPanel();                                        // version / build-info overlay, top-most
    renderProgressOverlay();                                   // background-task progress, top-most modal
    renderDialog();                                            // confirm/warn message dialog, over everything
    renderPlayerDropBoxes();                                   // view/replace/add chooser while dragging over the frame
    renderBinDragGhost();                                      // dragged source card, follows the cursor
    SDL_RenderPresent(renderer_);

    // Hand HDR to whichever sink owns it now (main window vs. external output). Runs
    // before syncReviewWindow so the main window is already sRGB by the time a review
    // monitor claims the HDR swapchain.
    syncHdrSink_();

    // Review-monitor window (second display): reconcile it with the current
    // toggle/selection, then draw + present it. Its present carries vsync, so it
    // paces the loop while active (the GUI present above runs unthrottled).
    syncReviewWindow();
    if (reviewActive())
        renderReviewWindow();
}

// Build the title-bar icon texture from the executable's embedded icon. Decoded
// at 2x the display size so the down-scaled result stays crisp; failure just
// leaves iconTex_ null and the menu starts flush left.
void App::loadAppIcon() {
    std::vector<uint8_t> rgba;
    int w = 0, h = 0;
    if (!loadAppIconRGBA((int)(kIconSize * 2.0f), rgba, w, h) || w <= 0 || h <= 0)
        return;
    iconTex_ = SDL_CreateTexture(renderer_, SDL_PIXELFORMAT_RGBA32,
                                 SDL_TEXTUREACCESS_STATIC, w, h);
    if (!iconTex_)
        return;
    SDL_SetTextureScaleMode(iconTex_, SDL_SCALEMODE_LINEAR);
    SDL_SetTextureBlendMode(iconTex_, SDL_BLENDMODE_BLEND);
    SDL_UpdateTexture(iconTex_, nullptr, rgba.data(), w * 4);
}

// Draw the app icon in the slot kMenuLeftInset reserves on the left of the title
// bar (before the first menu title). Centered vertically in the bar, at the
// fixed display size; a square icon fills the kIconSize box exactly. No-op if the
// icon failed to load (the menu then simply starts at the same inset).
void App::drawAppIcon() {
    if (!iconTex_)
        return;
    SDL_FRect dst = appIconRect();
    SDL_RenderTexture(renderer_, iconTex_, nullptr, &dst);
}

// The icon's box: where drawAppIcon paints it, and the region the hit test keeps
// clickable so a press there opens the window menu instead of dragging the window.
SDL_FRect App::appIconRect() const {
    float topH = titleBar_.height();
    return { kIconPadX, (topH - kIconSize) * 0.5f, kIconSize, kIconSize };
}

// The borderless window has no native system menu, so the icon carries one:
// minimize, maximize/restore and close, the same three actions as the buttons on
// the right of the bar.
void App::openAppIconMenu() {
    bool maximized = (SDL_GetWindowFlags(window_) & SDL_WINDOW_MAXIMIZED) != 0;
    std::vector<ContextMenu::Item> items;
    auto add = [&](const char* label, std::function<void()> action) {
        ContextMenu::Item it;
        it.label = label;
        it.action = std::move(action);
        items.push_back(std::move(it));
    };
    add("Minimize", [this] { SDL_MinimizeWindow(window_); });
    if (maximized)
        add("Restore", [this] { SDL_RestoreWindow(window_); });
    else
        add("Maximize", [this] { SDL_MaximizeWindow(window_); });
    add("Close", [this] { requestQuit(); });
    // ContextMenu anchors its bottom edge at the given y and grows upward; offset
    // by the list's height so it lands just below the icon instead.
    const SDL_FRect icon = appIconRect();
    const float listH = (float)items.size() * 20.0f; // ContextMenu::kRowH
    menuBar_.close(); // only one title-bar menu at a time
    appIconMenu_.open(icon.x, icon.y + icon.h + listH, winW_, winH_, std::move(items));
}

// Clicks on the icon: one opens (or dismisses) the window menu, two close the
// window — the same pair of gestures a native system menu offers. Taken before
// the popup itself sees the event, so the second click of a double click quits
// rather than being eaten as a dismissing click outside the open menu.
bool App::appIconHandleEvent(const SDL_Event& e) {
    if (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN && e.button.button == SDL_BUTTON_LEFT) {
        const SDL_FRect r = appIconRect();
        if (e.button.x >= r.x && e.button.x < r.x + r.w &&
            e.button.y >= r.y && e.button.y < r.y + r.h) {
            if (e.button.clicks >= 2) {
                appIconMenu_.close();
                requestQuit();
            } else if (appIconMenu_.isOpen()) {
                appIconMenu_.close(); // second click on the icon dismisses it
            } else {
                openAppIconMenu();
            }
            return true;
        }
        // A click on a top-level menu title while the window menu is open: close
        // it here and let the event through, so the title opens its menu on that
        // same click instead of being swallowed as a dismissing click outside.
        if (appIconMenu_.isOpen() && menuBar_.pointOverTitle(e.button.x, e.button.y)) {
            appIconMenu_.close();
            return false;
        }
    }
    return appIconMenu_.handleEvent(e);
}

// One full tick: advance state, then lay out and draw. Shared by the main loop
// and the resize watch so a frame drawn mid-resize is identical to a normal one.
void App::drawFrame(bool isLiveMovingOrResizing) {
    if (!renderer_)
        return;

    // Re-entrancy guard. Creating/fullscreening the review window pumps OS events
    // synchronously, which fire the resize event-watch (onWatchEvent -> drawFrame).
    // Without this, that nested draw re-enters syncReviewWindow before reviewWindow_
    // is assigned and recursively opens dozens of stacked review windows. The
    // legitimate modal-resize repaint never hits this: there the main loop is
    // blocked in SDL_PollEvent, so drawFrame is not already on the stack.
    if (inDrawFrame_)
        return;
    inDrawFrame_ = true;
    struct Guard { bool& f; ~Guard() { f = false; } } guard{ inDrawFrame_ };

    computeLayout();

    // Index the view's clips for the length of this pass. Everything downstream
    // that asks "which clip is at frame f" — the playback hold in update(), the
    // audio feed, the prefetch's per-frame programSourceAt, the ruler readouts —
    // then answers by binary search instead of scanning every clip in the view.
    // Torn down on the way out: between passes an edit can reallocate the vectors
    // it points into (see buildClipIndex).
    buildClipIndex();
    struct IndexGuard { bool& f; ~IndexGuard() { f = false; } } indexGuard{ clipIndexValid_ };

    update();

    if (tlResizing_ || tlResizeHovered_) {
        if (cursorNSResize_) SDL_SetCursor(cursorNSResize_);
    } else if (peResizing_ || peResizeHovered_ || clipSourceResizing_ || clipSourceResizeHovered_
        || trimmingClip_ || trimHover_) {
        if (cursorEWResize_) SDL_SetCursor(cursorEWResize_);
    } else if (draggingClip_) {
        // Clip drag: the cursor names the drop mode — move arrow for overwrite,
        // insertion bar for the Ctrl-held ripple.
        SDL_Cursor* c = dragDropMode_ == DropMode::Ripple ? cursorIBeam_ : cursorMove_;
        if (c) SDL_SetCursor(c);
    } else if (pencilOverFrame_) {
        if (cursorCrosshair_) SDL_SetCursor(cursorCrosshair_);
    } else if (razorMode() && razorOverTracks()) {
        // SDL has no razor cursor; the crosshair is the closest thing, and it puts
        // a vertical line under the cut the same way the pencil's does.
        if (cursorCrosshair_) SDL_SetCursor(cursorCrosshair_);
    } else {
        if (cursorDefault_) SDL_SetCursor(cursorDefault_);
    }

    if (!isLiveMovingOrResizing) {
        submitCacheRequests();
    }

    render();
}

// Called synchronously for every event, including from inside Windows' modal
// resize/move loop where the main run() loop is blocked in SDL_PollEvent. On a
// live size change we redraw immediately so the content tracks the drag instead
// of freezing on the last frame until the mouse is released.
bool SDLCALL App::onWatchEvent(void* userdata, SDL_Event* e) {
    App* self = static_cast<App*>(userdata);

    if (e->type != SDL_EVENT_WINDOW_RESIZED &&
        e->type != SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED &&
        e->type != SDL_EVENT_WINDOW_EXPOSED)
        return true;

    // Windows pumps EXPOSED events continuously through the whole modal drag
    // loop, flagged data1==1 ("live-resize"), even when the window is only being
    // *moved* — where the content is unchanged. Repainting on those adds work
    // inside the OS drag loop and makes the window lag the cursor. So for a live
    // expose, only repaint if the pixel size actually changed from the last
    // frame (a real resize). A normal expose (data1==0, e.g. being uncovered)
    // always repaints. RESIZED / PIXEL_SIZE_CHANGED only fire on real size
    // changes, so they always repaint.
    if (e->type == SDL_EVENT_WINDOW_EXPOSED && e->window.data1 == 1) {
        int w = 0, h = 0;
        SDL_GetRenderOutputSize(self->renderer_, &w, &h);
        // winW_/winH_ are logical; compare in the same units computeLayout derives.
        if (std::lround(w / self->uiScale_) == (long)self->winW_ &&
            std::lround(h / self->uiScale_) == (long)self->winH_)
            return true; // pure move (or idle modal loop): skip the redraw
    }

    self->drawFrame(true); // skips cache prefetch while dragging
    return true;
}

void App::run() {
    // The interpreter and its proxy-path resolver hook are started from init(),
    // early enough to overlap the renderer (see spawnPythonStartup there).
    bool firstFrameLogged = false;
    while (!quit_) {
        SDL_Event e;
        while (SDL_PollEvent(&e))
            handleEvent(e);
        // Command-line media loaded during init() before Python was ready: once
        // the interpreter is up, run the query_audio pairing once for those clips.
        if (!pendingCmdlineAudioClipIds_.empty() && jplayPythonReady()) {
            std::vector<int> ids;
            ids.swap(pendingCmdlineAudioClipIds_);
            if (attachAudioToSeq_)
                findAndAttachAudioAll(&ids);
        }
        // Same deal for a command-line .otio: its media were imported untagged
        // because the naming-convention query had no interpreter to run on yet.
        if (pendingPathValueTag_ && jplayPythonReady())
            tagMediaPathValues();
        // And for the proxy mode a project with no selection of its own should have
        // opened in: list_proxy_modes needs the same interpreter (App_ProxyMenu.cpp).
        if (pendingProxyDefault_ && jplayPythonReady())
            applyPendingProxyDefault();
        cancelPickerIfClosed();      // a closed picker panel cancels its in-flight query
        clipRangeCloseIfMenuClosed(); // a closed clip popup ends its range edit
        processPendingDialogs();
        work_.setPaused(playing_);   // hold background tasks while media plays
        work_.drainCompletions();    // run finished tasks' callbacks on this thread
        pickerWork_.drainCompletions(); // apply finished picker queries on this thread
        pollPickerDecorations();     // apply whatever the labels/colors pass has merged so far
        drainSync();                 // apply host events / emit changes (sync review)
        drainControl();              // apply commands from the local control channel
        resolveMissingWithRules();   // auto-relocate any newly-missing clip via learned rules
        pollProgress();              // finalize a finished background progress task on this thread
        drawFrame(false);
        if (!firstFrameLogged) {
            firstFrameLogged = true;
            // The window was created hidden so the renderer's window reconfigure
            // (which destroys and recreates the native window) stays invisible.
            // Reveal it now that it has real content in it.
            SDL_ShowWindow(window_);
            if (jplayDebugLogging())
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                            "  [startup] first frame presented, window shown: %.1f ms after launch",
                            std::chrono::duration<double, std::milli>(
                                std::chrono::steady_clock::now() - launchTime_).count());
        }
    }

    // Fast exit: the frame-cache and thumbnail workers can be blocked inside a
    // slow, uninterruptible decode (readFrame), so joining them in ~App() stalls
    // shutdown for up to one frame-decode per worker. Nothing they hold needs a
    // graceful flush — the frame cache is throwaway, thumbnails are written
    // atomically, and settings/recents are already persisted at mutation time.
    // So hide the window (feels instant) and let the OS reclaim everything.
    if (window_) SDL_HideWindow(window_);
    std::_Exit(0);
}

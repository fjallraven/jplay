#pragma once

#include "MediaSource.h" // SourceColorTags

#include <OpenColorIO/OpenColorIO.h>
#include <Imath/half.h>

#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace OCIO = OCIO_NAMESPACE;

// Manages the active OpenColorIO config, selection state (display / view / look),
// and the transforms that carry a decoded frame from the colour space its media is
// read in to display-referred output at review time.
//
// Every source is colour-managed, whatever it decoded from: the media colour space
// is resolved per media (colorSpaceForMedia) rather than chosen once for the
// session, and the pipeline is split at the linear working space so an exposure
// adjustment means the same thing on an EXR and on a ProRes (see Transform).
class OcioManager {
public:
    // Load config from $OCIO; fall back to the OCIO built-in CG config when the
    // variable is unset. Returns false (and leaves isReady() == false) only if
    // both paths throw — e.g. $OCIO points to a non-existent file.
    //
    // When $OCIO is unset, `samplePath` (a representative source media path) is
    // matched against the [ocio] source_regex in jplay_preferences.conf; on a
    // match the [ocio] ocio template is expanded and set as $OCIO before the
    // load. Pass an empty string to skip that step (built-in config is used).
    bool init(const std::string& samplePath = "");
    bool isReady() const { return !!config_; }

    // The $OCIO resolution step of init(), split out because it writes the process
    // environment and so must not run concurrently with anything reading it. init()
    // calls it itself, so callers normally ignore it; call it on the main thread
    // first when init() is being handed to a worker, which leaves the worker's own
    // call a no-op (it finds $OCIO already set, or the same path still unmatched)
    // and so keeps the write off that thread. A no-op once $OCIO is set.
    void resolveEnv(const std::string& samplePath);

    // True when the active config is the built-in CG fallback (loaded because
    // $OCIO was unset and no preferences [ocio] rule matched the sample path).
    // Lets the app retry init() once a real source path becomes available.
    bool usingBuiltinConfig() const { return usingBuiltin_; }

    // Make the config that `mediaPath` resolves to (via the [ocio] preferences
    // rules) the active one, loading it on first use and caching it thereafter.
    // Returns true only when the active config actually changed, so the caller can
    // force a re-render; false covers every no-op — same config, no rule match, an
    // unreadable config, or an externally-set $OCIO.
    //
    // $OCIO set outside the app pins one config for the whole session (it is a
    // deliberate override of exactly this per-source resolution), so only
    // preferences-derived configs vary per media.
    //
    // Each config carries its own display / view / look / input colorspace. Those
    // names rarely mean the same thing in two shows' configs, so a switch stores the
    // outgoing selection and restores the incoming one — or takes the new config's
    // own defaults on first use. No attempt is made to match selections across
    // configs by name.
    bool setActiveConfigForPath(const std::string& mediaPath);

    // Set the OCIO context variables for `meta`, a media's naming-convention
    // metadata (Media::meta()). The variables and their templates come from the
    // [ocio] context entry in jplay_preferences.conf ("SCENE={scene},SHOT={shot}"),
    // each {key} naming one metadata key. A variable whose template names a key the
    // media carries no value for is not set at all, leaving the config's own default
    // (or an externally set one) in place rather than a half-formed value.
    //
    // Returns true only when the resolved variables actually changed, so the caller
    // can force a re-render; the context feeds every processor built afterwards, and
    // a change bumps version() the same way a display/view change does. Like
    // setActiveConfigForPath this is called per rendered frame from the main thread,
    // and is a no-op when no context entry is configured.
    bool setContextForMedia(const std::map<std::string, std::string>& meta);

    // A job that builds, off the main thread, the transform the media at `mediaPath`
    // with metadata `meta` will need — its context variables resolved against the
    // active config. Loading a shot's LUTs costs tens of milliseconds cold (more from
    // network storage) and OCIO caches processors and LUT files per config, keyed by
    // content, so warming the clip ahead of the playhead turns the rebuild at the cut
    // into a cache hit rather than a stall in the render loop.
    //
    // The returned job owns copies of everything it reads — the config handle, the
    // selection, the resolved variables — so it races nothing the main thread may
    // change under it, and OCIO's caches are internally locked, so several may run at
    // once. Call this on the main thread; run the job anywhere.
    //
    // Empty when there is nothing worth warming: no context entry configured, values
    // the active transform already uses, values already warmed, or a media whose
    // config differs from the active one (that switch is not warmed).
    std::function<void()> warmJobForMedia(const std::string& mediaPath,
                                          const std::map<std::string, std::string>& meta);

    // ── Input colour space per media ──────────────────────────────────────
    // The colour space a media's pixels should be interpreted in when the user has
    // not named one explicitly. Resolved against the active config, in order:
    //
    //  1. The config's file rules (Config::getColorSpaceFromFilepath) — the same
    //     mechanism Nuke, Hiero and RV resolve a file's colour space with. A show
    //     config that has been set up at all answers here, so it wins outright.
    //  2. The container's own colour tags, for a file the rules matched only by
    //     their catch-all default. A stream tagged smpte2084 is not sRGB, and a
    //     config that says nothing about the file's *name* still almost always
    //     carries a colour space that means PQ.
    //  3. A fallback by media kind: the scene_linear role for scene-referred float
    //     (EXR), a conventional sRGB texture space for everything else.
    //
    // Returns "" only when there is no config at all. Cheap enough to call per
    // frame — the caller (Media) caches the answer regardless.
    std::string colorSpaceForMedia(const std::string& path, const SourceColorTags& tags) const;

    // Lists for populating menus.
    std::vector<std::string> displays() const;
    std::vector<std::string> views(const std::string& display) const;
    std::vector<std::string> looks() const;        // named Looks in the config
    std::vector<std::string> colorSpaces() const;  // all color spaces in the config

    const std::string& activeDisplay() const { return display_; }
    const std::string& activeView()    const { return view_; }
    const std::string& activeLook()    const { return look_; }

    // ── SDR companion transform ───────────────────────────────────────────
    // When an HDR output monitor owns the selected display (e.g. Rec.2100-PQ), the
    // application window is on an SDR swapchain and those code values are wrong
    // there. It renders through this companion instead: the config's SDR display,
    // same look and input space, so the GUI shows the SDR rendering of the same
    // show look rather than a bare sRGB encode of scene-linear (which has no tone
    // scale and clips everything above 1.0).
    //
    // Display: the config's sRGB display, falling back to its default display.
    // View: the selected view when this display also offers it (Un-tone-mapped,
    // Raw, per-shot views), else the display's own default — the config author
    // already picked the sensible SDR rendering, so we never name-match "HDR 1000
    // nits" down to an SDR sibling by hand.
    std::string sdrDisplay() const;
    // Copied out, not returned by reference: the startup warm writes it (see displayMtx_).
    std::string activeSdrDisplay() const { // valid after sdrTransformFor()
        std::lock_guard<std::mutex> lk(displayMtx_);
        return sdrDisplay_;
    }
    const std::string& activeSdrView()    const { return sdrView_; }

    // ── Preferred view ────────────────────────────────────────────────────
    // The view a config's defaults are overridden with when it first becomes
    // active, applied only when that config's default display actually offers a
    // view by this name — names rarely carry across two shows' configs, and a view
    // the display does not list builds no processor at all. Empty = no preference:
    // every config opens on its own default view.
    //
    // Set from the project on load (Timeline::ocioView) and by every explicit view
    // pick; passing "" clears the preference without disturbing the active view.
    // Applied both here (a project whose config is already active gets no
    // activation to hook onto) and at every later config activation.
    void setPreferredView(const std::string& v);

    // Whether the OCIO display transform is the active colour pipeline at all;
    // false leaves the built-in scene-linear -> sRGB fallback in charge. Starts
    // false and is set at launch from the preferences default (prefersOcio), then
    // by each project's saved mode and by the Settings toggle. Enabling it does not
    // load a config — that is init(), which the app calls once this turns on.
    bool isEnabled() const { return enabled_; }
    void setEnabled(bool e) { enabled_ = e; }

    // The colour pipeline named by [color_management] color_pipeline in
    // jplay_preferences.conf: true only for "ocio". Everything else — "srgb", an
    // unknown value, a missing key, no preferences file — is false, so a config is
    // never opened unless something explicitly asks for one.
    static bool prefersOcio();

    // The key identifying the active config (its path; "" for the built-in
    // fallback). A resolved input colour space is only meaningful against the
    // config it came from, so callers that cache one store this alongside it.
    const std::string& activeConfigPath() const { return activeConfigPath_; }

    // The resolved [ocio] context variables currently in force (see
    // setContextForMedia). They are part of the identity of a rendering — a config
    // with per-shot LUTs renders the same colour space differently under different
    // variables — so a consumer caching an image it produced keys that cache on
    // these alongside the config and the selection.
    const std::map<std::string, std::string>& contextVars() const { return contextVars_; }

    // The linear working space — the scene_linear role resolved to a concrete
    // colour space name. Empty for a config declaring no such role, in which case
    // the pipeline degenerates to one transform straight from the media space and
    // exposure falls back to acting there.
    const std::string& workingColorSpace() const { return workingCS_; }

    // ── The display pipeline, in two halves ───────────────────────────────
    // Split at the linear working space so an exposure adjustment has somewhere to
    // live that means the same thing whatever the source:
    //
    //   media space --toWorking--> working --x exp2(EV)--> --toDisplay--> display
    //
    // toWorking is the media colour space converted to the working one. It varies
    // per media and is cheap — usually a matrix and a curve — so it is built per
    // input space and cached; it is null when the media is already in the working
    // space, which the caller reads as identity and skips.
    //
    // toDisplay carries everything expensive a config has (the tone scale, a shot
    // LUT) and depends on nothing about the media, so a single build is shared by
    // every source on the timeline: a cut between an EXR and a ProRes rebuilds only
    // the cheap half, and a dissolve joining them costs one extra input transform
    // rather than a second full chain.
    //
    // `version` is a monotonic token covering both halves, for renderers caching a
    // compiled program. It changes whenever either half does — including when the
    // display, view, look, config or context variables change, which discard the
    // whole cache.
    struct Transform {
        OCIO::ConstProcessorRcPtr toWorking; // null = media already in the working space
        OCIO::ConstProcessorRcPtr toDisplay; // null = no valid display transform
        int version = -1;
        bool valid() const { return !!toDisplay; }
    };
    Transform transformFor(const std::string& inputCS) const;

    // Build the display half now instead of on the first transformFor(). It is the
    // expensive half by far -- one config->getProcessor() of the display+view chain,
    // hundreds of ms on a LUT-heavy show config -- and it depends on nothing but the
    // config, display, view and look: no media, no frame. Called from init() on a
    // worker so it runs while the renderer is being created, rather than landing on
    // the first drawn frame with the decoded image already waiting on it. Nothing
    // here is internally locked, so the caller owns the manager until that worker
    // is joined.
    void warmDisplay() const;

    // The SDR companion of transformFor (see the section further down) — the same
    // input half, the SDR display rendering for the second.
    Transform sdrTransformFor(const std::string& inputCS) const;

    // A CPU display transform for one input colour space. Self-contained: it owns
    // its processors, its exposure and its scratch buffer, so a copy can be handed
    // to a worker — the export encoder — and used there without touching the
    // manager or the main thread. Build it on the main thread; use one instance
    // from one thread at a time.
    class CpuTransform {
    public:
        bool valid() const { return !!toDisplay_; }
        // `f` -> `dst`, width * height * 4 display-referred bytes. Reads the
        // highest-precision buffer the frame carries.
        void apply(const Frame& f, uint8_t* dst) const;
        // `f` -> `dst`, width * height * 3 floats in the linear working space — the
        // input half and the exposure only, for a consumer that wants the working
        // image rather than a display rendering (the EXR export writer).
        void applyToWorking(const Frame& f, float* dst) const;

    private:
        friend class OcioManager;
        OCIO::ConstCPUProcessorRcPtr toWorking_, toDisplay_;
        float gain_ = 1.0f;
        mutable std::vector<float> scratch_;
    };
    CpuTransform cpuTransformFor(const std::string& inputCS, float exposureEV) const;

    // Setters mark the cached CPU processor dirty so it is rebuilt on next apply().
    void setDisplay(const std::string& d);
    // Picking a view is a standing preference, not a one-off: it is remembered as
    // the preferred view (below) so a cut into a config never seen before opens on
    // the same rendering rather than snapping to that config's default.
    void setView(const std::string& v) { view_ = v; preferredView_ = v; markDirty_(); }
    void setLook(const std::string& l) { look_ = l; markDirty_(); }

    // Monotonic token of the current display half, bumped whenever the active
    // transform changes. Every Transform::version is stamped from the same
    // sequence, so a renderer fed first one and then another always sees a
    // changed token.
    int version() const {
        std::lock_guard<std::mutex> lk(displayMtx_);
        return version_;
    }

private:
    OCIO::ConstConfigRcPtr config_;
    std::string display_, view_, look_;
    bool enabled_ = false; // see isEnabled/prefersOcio: OCIO is opt-in
    bool usingBuiltin_ = false; // active config is the built-in CG fallback

    // ── Per-source configs (see setActiveConfigForPath) ───────────────────
    // Loaded configs, keyed by config path ("" = the built-in fallback). A failed
    // load is cached as null so a bad path is not re-opened on every switch.
    std::map<std::string, OCIO::ConstConfigRcPtr> configCache_;
    struct Selection { std::string display, view, look; };
    std::map<std::string, Selection> selections_; // per config path; saved on switch away
    std::string activeConfigPath_;                // key of config_ in the two maps above
    std::map<std::string, std::string> contextVars_; // resolved [ocio] context variables
    std::string warmedKey_;  // transform (config + selection + variables) last warmed
    std::string lastResolvedMedia_;   // short-circuits the regex when the clip is unchanged
    bool envProbed_ = false;          // envPreset_ has been determined
    bool envPreset_ = false;          // $OCIO was set before we ever wrote it

    std::string preferredView_; // see setPreferredView; "" = use each config's default

    std::string workingCS_; // resolved scene_linear role; "" when the config has none

    // The display halves, built once per selection and shared by every input space,
    // plus the per-input-space transforms assembled from them. Every entry is
    // discarded together by markDirty_(): they are all built against one config,
    // one selection and one set of context variables.
    // Guards this block, and only this block. The startup warm (warmDisplay, run on
    // a worker from App::init while the renderer is built) is the one place any of it
    // is touched off the main thread; everything else here -- the config, the
    // selection, the context variables -- is the main thread's alone, and init keeps
    // off all of it until that worker is joined. The rebuild holds the lock for its
    // whole few hundred ms, so a main-thread caller that does reach a guarded method
    // while it runs waits it out rather than racing it.
    mutable std::mutex displayMtx_;
    mutable bool displayDirty_ = true;
    mutable OCIO::ConstProcessorRcPtr displayProc_;    // working -> selected display/view
    mutable OCIO::ConstProcessorRcPtr sdrDisplayProc_; // working -> SDR companion display
    mutable std::map<std::string, Transform> transforms_, sdrTransforms_;
    mutable std::map<std::string, OCIO::ConstProcessorRcPtr> inputProcs_; // shared input halves

    mutable int versionSeq_ = 0; // monotonic; stamps every Transform::version
    mutable int version_ = 0;    // token of the current displayProc_
    mutable std::string sdrDisplay_, sdrView_; // resolved in rebuildDisplay_()

    void markDirty_() const {
        std::lock_guard<std::mutex> lk(displayMtx_);
        displayDirty_ = true;
        transforms_.clear();
        sdrTransforms_.clear();
        inputProcs_.clear();
    }
    void applyConfigDefaults_(); // display/view/look/working space from config_ own defaults

    // Point view_ at preferredView_ when the active display offers a view by that
    // name. True when the selection actually changed (the caller marks dirty).
    bool applyPreferredView_();
    void rebuildDisplay_() const;
    // The media colour space -> the working space. Null for an identity conversion.
    OCIO::ConstProcessorRcPtr inputProcessor_(const std::string& inputCS) const;
    // (working -> display, view) with the look, as a GPU/CPU-ready processor.
    OCIO::ConstProcessorRcPtr buildDisplayProcessor_(const std::string& display,
                                                     const std::string& view) const;
};

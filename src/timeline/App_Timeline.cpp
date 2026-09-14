// Timeline translation unit: view <-> pixel math, the timeline/track rendering,
// track add/remove, and clip drag + external-file drop placement. These are all
// App members; they live here (rather than in App.cpp) only to keep that file a
// manageable size. Shared layout constants and helpers come from AppInternal.h.

#include "App.h"
#include "AppInternal.h"
#include "AudioEngine.h" // master volume: App::setVolume / toggleMute
#include "AudioSource.h"
#include "MediaSource.h"
#include "ExrSource.h"
#include "ImageSeq.h"
#include "Layout.h"
#include "MediaScan.h"
#include "Preferences.h"
#include "PythonBridge.h"
#include "PythonStartup.h" // jplayPythonReady, for the deferred metadata tagging
#include "SkinColors.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <climits>
#include <cmath>
#include <filesystem>
#include <map>
#include <thread>

using namespace jplay;

// ---- timeline palette
// Static colors used across renderTimeline(). Colors that are computed per state
// (track-row striping, clip fills, the hovered-transport tint) stay inline at
// their draw sites. A few entries are drawn at varying alpha — the constant
// carries the base RGB and the alpha is passed at the call site.
namespace {
// How often the cache strip's resident-frame index is rebuilt (ms).
constexpr double kCacheStripRefreshMs = 100.0;


// Panel / strip backgrounds. The timeline panel, the transport info bar and the
// left label gutter all use the shared jplay::kPanelBg so they read as one panel
// with the nav strip beside them.
constexpr SDL_Color kRulerBg     { 16, 13, 13, 255 }; // ruler (#292120)
constexpr SDL_Color kCacheStripBg{ 22, 23, 25, 255 }; // frame-cache strip
constexpr SDL_Color kSeqBarBg    { 28, 29, 32, 255 }; // sequence bar
constexpr SDL_Color kShotBarBg   { 24, 26, 30, 255 }; // shot bar
constexpr SDL_Color kRowLine     { 20, 21, 23, 255 }; // separator between track rows
constexpr SDL_Color kResizeHandle{ 100, 120, 180, 200 }; // hovered/active top-edge resize strip

// Text
constexpr SDL_Color kMutedText  { 150, 152, 158, 255 }; // gutter labels, fps, ruler numbers
constexpr SDL_Color kReadoutText{ 200, 200, 205, 255 }; // clip frame/time readout
constexpr SDL_Color kIconLight  { 210, 213, 220, 255 }; // transport glyphs
constexpr SDL_Color kBarLabel   { 210, 214, 222, 255 }; // sequence / shot names
constexpr SDL_Color kClipName   { 225, 228, 235, 255 }; // clip primary label
constexpr SDL_Color kRazorLine  { 255, 205, 70, 255 };  // razor cut line + its frame numbers
constexpr SDL_Color kClipDept   { 180, 190, 210, 255 }; // clip department label

// Track headers
constexpr SDL_Color kTrackLabel     { 215, 218, 225, 255 }; // populated track
constexpr SDL_Color kTrackLabelEmpty{ 110, 112, 118, 255 }; // empty track
constexpr SDL_Color kTrackLabelSpare{  70,  72,  78, 255 }; // trailing placeholder track
constexpr SDL_Color kTrackLabelOff  { 100, 102, 108, 255 }; // disabled track
constexpr SDL_Color kTrackBtn       { 150, 154, 163, 255 }; // header button, idle
constexpr SDL_Color kTrackBtnOff    {  95,  97, 103, 255 }; // header button, track disabled
constexpr SDL_Color kTrackBtnHot    { 225, 228, 235, 255 }; // header button, hovered

// Ruler ticks + in/out range (range fill drawn at low alpha)
constexpr SDL_Color kTickMinor { 64, 66, 72, 255 };
constexpr SDL_Color kTickMajor { 90, 92, 98, 255 };
constexpr SDL_Color kInOutRange{ 150, 150, 150, 255 };

// Cache strip: resident frames
constexpr SDL_Color kCacheResident{ 70, 200, 90, 255 };

// Sequence / shot bars (the *Live variant is lifted while under the playhead).
// Sequence spans are filled with the sequence's own bgColor, so they have no
// constant here.
constexpr SDL_Color kShotBlock   { 24, 26, 28, 255 }; // #181a1c
constexpr SDL_Color kShotBlockLive{ 36, 38, 42, 255 }; // #24262a
// Shot blocks outline lighter than their fill; the shared kBarBorder (black) is
// darker than both the block and the strip behind it, so it reads as no edge.
constexpr SDL_Color kShotBlockBorder{ 70, 74, 82, 255 };
constexpr SDL_Color kBarBorder   {  0,  0,  0, 110 };

// Clip fill by state (the *Live variant is used while the clip is under the playhead)
constexpr SDL_Color kClipNormal     {  12,  26,  41, 255 }; // video / image sequence, not under playhead (#0c1a29)
constexpr SDL_Color kClipLive       {  29,  48,  69, 255 }; // video / image sequence under playhead (#1d3045)
constexpr SDL_Color kClipNormalAudio{  58,  46,  86, 255 }; // audio clip, not under playhead
constexpr SDL_Color kClipAudioLive  {  72,  58, 104, 255 }; // audio clip under playhead
constexpr SDL_Color kClipWave       { 190, 175, 235, 190 }; // waveform inside audio clips
constexpr SDL_Color kClipWaveVideo  { 205, 195, 240,  70 }; // ... and behind a video clip's name rows
constexpr SDL_Color kClipMissing    { 120,  34,  34, 255 }; // source not found
constexpr SDL_Color kClipStrip      {  38, 84, 132, 255 }; // compact-mode flattened band: the row colours
constexpr SDL_Color kClipStripAudio { 104,  86, 150, 255 }; // lifted, since a 5px line of #0c1a29 against
                                                            // black reads as nothing at all
constexpr SDL_Color kClipStripDim     {  22, 48,  76, 255 }; // ... and the pair dimmed, for a clip the
constexpr SDL_Color kClipStripAudioDim{  60, 50,  87, 255 }; // playhead is not on: the lit pair marks it
constexpr SDL_Color kClipMissingLive{ 144,  34,  34, 255 }; // ... under playhead

// Clip outline (drawn at the clip's own alpha)
constexpr SDL_Color kClipBorder{ 120, 125, 135, 255 };

// Gap boxes
constexpr SDL_Color kGapSelected{ 175, 181, 191, 255 };
constexpr SDL_Color kGapHover   { 120, 124, 132, 255 };

// Drag / drop / displace overlays
constexpr SDL_Color kDisplaceFill  { 40, 70, 150, 120 };
constexpr SDL_Color kDisplaceBorder{ 90, 130, 220, 255 }; // also the overlapping drop preview
constexpr SDL_Color kDisplaceArrow { 200, 220, 255, 255 };
constexpr SDL_Color kDropFree      { 90, 200, 120, 255 };  // free-space drop preview
constexpr SDL_Color kFileDrop      { 90, 180, 220, 255 };  // external file-drag placeholder

// Frame indicator (also the cross-sequence reject box; faint variant for the
// hover preview). Drawn at varying alpha at the call sites.
constexpr SDL_Color kPlayhead{ 235, 70, 70, 255 };

// Transport button border
constexpr SDL_Color kTransportBorder{ 78, 80, 88, 255 };

// Clip fade ramps: a wedge over the clip's head/tail and the grab dot at its apex.
constexpr SDL_Color kFadeWedge { 232, 238, 250, 70 };  // very light, so the clip reads through
constexpr SDL_Color kFadeLine  { 232, 238, 250, 210 };
constexpr SDL_Color kFadeDot   { 245, 238, 214, 255 };

// Transition (dissolve) box straddling a cut, drawn over the two clips it joins.
constexpr SDL_Color kTransFill  {  86,  96, 122, 150 }; // translucent: both clips read through
constexpr SDL_Color kTransBorder{ 168, 178, 200, 255 };
constexpr SDL_Color kTransGlyph { 208, 216, 232, 220 }; // the diagonal split

// Dotted-border accents shared by clip selection and clip hover.
constexpr SDL_Color kSelectCream{ 245, 238, 214, 255 }; // light cream: selected clip
constexpr SDL_Color kHoverCream { 198, 184, 140, 255 }; // darker cream: hovered item

inline void setColor(SDL_Renderer* r, SDL_Color c) {
    SDL_SetRenderDrawColor(r, c.r, c.g, c.b, c.a);
}

// Template for the second row of a clip's label, from [timeline]
// clip_metadata_label in jplay_preferences.conf: {key} placeholders naming
// naming-config picker keys ("{department}", "{department} {version}", ...).
// Resolved once — the preferences file is itself parsed once and cached — so the
// per-clip cost each frame is the substitution alone, with no naming-convention
// parsing and no Python involved (the values were resolved into Media's metadata
// when the media was added; see tagMediaPathValues).
const std::string& clipMetadataTemplate() {
    static const std::string tmpl = [] {
        std::string t = Preferences::get("timeline", "clip_metadata_label");
        return t.empty() ? std::string("{department}") : t; // no prefs file / no key
    }();
    return tmpl;
}

// Expand `tmpl` against one media's picker metadata. A {key} the media carries no
// value for contributes nothing, and the runs of whitespace that leaves collapse
// to one space, so "{department} {version}" yields "compositing" rather than
// "compositing " on unversioned media. An unknown key resolves empty for the same
// reason a known-but-unset one does: metaValue() returns "" for both.
std::string expandClipMetadata(const std::string& tmpl, const Media& m) {
    std::string out;
    out.reserve(tmpl.size());
    for (size_t i = 0; i < tmpl.size(); ++i) {
        if (tmpl[i] == '{') {
            size_t close = tmpl.find('}', i);
            if (close != std::string::npos) {
                out += m.metaValue(tmpl.substr(i + 1, close - i - 1));
                i = close;
                continue;
            }
        }
        out += tmpl[i];
    }
    // Collapse the gaps left by placeholders that resolved to nothing.
    std::string packed;
    packed.reserve(out.size());
    for (char c : out) {
        const bool space = (c == ' ' || c == '\t');
        if (space && (packed.empty() || packed.back() == ' '))
            continue;
        packed += space ? ' ' : c;
    }
    while (!packed.empty() && packed.back() == ' ')
        packed.pop_back();
    return packed;
}

} // namespace

// Pixel half-width of a clip's left/right trim handle hit zone.
static constexpr float kTrimHandleW = 6.0f;
// Fade grab dot: half-width of its hit zone, and the height of the band along the
// clip's top edge that counts as a fade grab rather than a clip press.
static constexpr float kFadeHandleW = 6.0f;
static constexpr float kFadeHandleH = 7.0f;

// ---------------------------------------------------------------- view math

// The gutter is only there to label track rows; compact mode has none, so it
// collapses and the bands above it run to the window's left edge.
float App::headerW() const {
    return compactTimeline_ ? 0.0f : kHeaderW;
}

double App::frameToX(double frame) const {
    return headerX_ + (frame - viewStart_) / framesPerPx_;
}

double App::xToFrame(double x) const {
    return viewStart_ + (x - headerX_) * framesPerPx_;
}

void App::fitView() {
    double len = (double)std::max<int64_t>(timeline_.length(), 1);
    double w = std::max(contentW_ > 0 ? contentW_ : winW_ - headerW(), 100.0f);
    framesPerPx_ = len / (double)(w - 20.0);
    viewStart_ = -10.0 * framesPerPx_;
    viewInitialized_ = true;
}

void App::fitRange(int64_t a, int64_t b) {
    double len = (double)std::max(b - a, (int64_t)1);
    double w = std::max(contentW_ > 0 ? contentW_ : winW_ - headerW(), 100.0f);
    framesPerPx_ = len / (double)(w - 20.0);
    viewStart_ = (double)a - 10.0 * framesPerPx_;
    viewInitialized_ = true;
}

// Fitting respects the scope, matching the fit sites elsewhere: only the All view
// pulls back to the whole concatenated timeline.
void App::fitScope() {
    int64_t a = 0, b = 0;
    if (viewSpan(a, b))
        fitRange(a, b);
    else
        fitView();
}

// The framesPerPx fitScope() would land on. Mirrors fitRange/fitView's maths so a
// caller can tell whether a zoom-out step would take the view past fit.
double App::fitScopeFpp() const {
    int64_t a = 0, b = 0;
    if (viewSpan(a, b))
        return fitRangeFpp(a, b);
    return fitRangeFpp(0, std::max<int64_t>(timeline_.length(), 1));
}

// As fitRange, for a caller that only wants to know where it would land.
double App::fitRangeFpp(int64_t a, int64_t b) const {
    double len = (double)std::max(b - a, (int64_t)1);
    double w = std::max(contentW_ > 0 ? contentW_ : winW_ - headerW(), 100.0f);
    return len / (w - 20.0);
}

// True when the view already sits exactly where fitRange(a, b) would put it. The
// fit buttons use this to grey themselves out: a fit that would leave the view
// where it already is has nothing to do.
bool App::viewFitsRange(int64_t a, int64_t b) const {
    double fpp = fitRangeFpp(a, b);
    if (fpp <= 0.0)
        return false;
    if (std::fabs(framesPerPx_ - fpp) > fpp * 1e-3)
        return false;
    return std::fabs(viewStart_ - ((double)a - 10.0 * fpp)) < fpp * 0.5; // under half a pixel
}

// The sequence under the playhead as a fit range. Empty-handed unless the view
// actually holds more than one sequence: with a single one in view this fit lands
// exactly where the whole-timeline fit does. Shared by the fit menu's "Sequence"
// row and by Alt+F.
bool App::playheadSeqRange(int64_t& a, int64_t& b, int& seqIdx) const {
    seqIdx = timeline_.seqIndexAtFrame(timeline_.playhead);
    std::vector<int> idxs = viewSeqIndices();
    return idxs.size() > 1 && std::find(idxs.begin(), idxs.end(), seqIdx) != idxs.end()
           && timeline_.sequenceSpan(timeline_.sequences[seqIdx], a, b);
}

// ---------------------------------------------------------- fit-view button
// The zoom-fit control in the info bar, right of the snap toggle: a "Zoom To"
// button that is nothing but the way to the list — hovering it drops the four
// fits above it. Four labelled rows in a popup say what each fit does far better
// than four buttons on the bar could, and cost a fifth of the width to say it;
// hovering is how they are reached, so the bar spends no width on a caret either,
// and the button takes no click of its own.
//
// The label (App::kFitBtnLabel) is shared by the width measurement and the
// drawing, so the two can't drift apart.
//
// The fits are recomputed every frame from computeLayout; a fit whose range is
// what the view already shows reads as disabled, which greys its row.

void App::layoutFitButtons() {
    fitBtns_.clear();
    fitBtnRect_ = SDL_FRect{};
    if (fitBarBand_.w <= 0.0f || contentW_ <= 0.0f || !timeline_.hasClips())
        return;

    // `ok` is the fit's own precondition (something to fit at all); on top of
    // that a fit is only live when it would actually move the view.
    auto add = [&](FitBtnKind kind, const char* label, const char* shortcut,
                   int64_t a, int64_t b, bool ok, int seqIdx = -1) {
        bool enabled = ok && b > a && !viewFitsRange(a, b);
        fitBtns_.push_back({ kind, label, shortcut, a, b, seqIdx, enabled });
    };

    // Whole timeline: the scoped sequence/project span, or the entire concatenated
    // timeline when nothing is scoped — mirroring fitToFilteredSequence(), which is
    // what the F key and this button both run.
    {
        int64_t a = 0, b = 0;
        if (viewAll() || !viewSpan(a, b)) {
            a = 0;
            b = std::max<int64_t>(timeline_.length(), 1);
        }
        add(FitBtnKind::Scope, "Timeline", "F", a, b, true);
    }

    // The sequence under the playhead.
    {
        int64_t a = 0, b = 0;
        int si = -1;
        bool ok = playheadSeqRange(a, b, si);
        add(FitBtnKind::Sequence, "Sequence", "Alt+F", a, b, ok, si);
    }

    // Playback range: live once an in/out has actually been marked — the same test
    // Shift+F makes before it prefers the range over the clip under the playhead.
    add(FitBtnKind::Range, "Range", "Shift+F",
        timeline_.inPoint, timeline_.effectiveOut() + 1,
        timeline_.inPoint != 0 || timeline_.outPoint >= 0);

    // The current clip: the one under the playhead, with the selection as the
    // fallback, so the button and Shift+F agree on what "current" means.
    {
        const Clip* clip = getTopMostClipAtFrame(timeline_.playhead);
        if (!clip)
            clip = timeline_.findClipById(selectedClipId_);
        add(FitBtnKind::Clip, "Clip", "Shift+F",
            clip ? clip->timelineStart : 0, clip ? clip->end() : 0, clip != nullptr);
    }

    // The slot computeLayout reserved right of the snap toggle, which it sized from
    // this same label - so the button fills it exactly.
    fitBtnRect_ = fitBarBand_;
}

// The hover popup: every fit, labelled, with its keyboard shortcut. A fit that
// would leave the view where it already is is listed greyed rather than dropped,
// so the list reads the same from one frame to the next.
void App::openFitMenu() {
    if (fitBtns_.empty())
        return;
    std::vector<ContextMenu::Item> items;
    for (const FitBtn& b : fitBtns_) {
        ContextMenu::Item it;
        it.label = b.label;
        it.shortcut = b.shortcut;
        it.disabled = !b.enabled;
        if (b.enabled)
            it.action = [this, b] { applyFitButton(b); };
        items.push_back(std::move(it));
    }
    fitMenu_.open(fitBtnRect_.x, fitBtnRect_.y, winW_, winH_, std::move(items));
}

// The popup is opened by hover rather than by a click. Called from handleEvent
// before fitMenu_ sees the event, since fitMenu_ consumes motion while open.
void App::fitHoverEvent(const SDL_Event& e) {
    if (e.type == SDL_EVENT_MOUSE_MOTION) {
        const float mx = e.motion.x, my = e.motion.y;
        if (fitBtnRect_.w > 0.0f && inRect(fitBtnRect_, mx, my)) {
            if (!fitMenu_.isOpen())
                openFitMenu();
            return;
        }
        // Off the button: the popup stays up while the cursor is inside it (that is
        // how a row gets picked — it grows upward, so the trip there never leaves
        // the two) and drops as soon as it is in neither. listBounds() is empty
        // until the popup's first render, which can only be while the cursor is
        // still on the button, so an empty box means "keep it".
        if (fitMenu_.isOpen()) {
            SDL_FRect rect = fitMenu_.listBounds();
            if (rect.w > 0.0f && !inRect(rect, mx, my))
                fitMenu_.close();
        }
        return;
    }
}

// Apply one button's fit. Scope goes through fitToFilteredSequence() rather than
// its cached range so there is a single definition of "fit the whole timeline".
void App::applyFitButton(const FitBtn& b) {
    if (b.kind == FitBtnKind::Scope) {
        fitToFilteredSequence();
        zoomFitSeqIdx_ = zoomFitShotId_ = -1;
        return;
    }
    fitRange(b.a, b.b);
    // As with the sequence bar's double-click, a sequence fit is remembered so a
    // following double-click on that span reads as "already fit" and zooms back out.
    zoomFitSeqIdx_ = (b.kind == FitBtnKind::Sequence) ? b.seqIdx : -1;
    zoomFitShotId_ = -1;
}

// Drawn with the rest of the info bar; the hovered half's tooltip is dropped
// below it by the shared tooltip block at the end of renderTimeline.
void App::drawFitButtons() {
    if (fitBtns_.empty())
        return;
    constexpr SDL_Color kFitHot{ 235, 238, 245, 255 };
    auto face = [&](const SDL_FRect& r, bool hov) {
        setColor(renderer_, hov ? kUiBtnBgHover : kUiBtnBg);
        jplay::fillRect(renderer_, &r);
        setColor(renderer_, hov ? kUiBtnBorderHover : kUiBtnBorder);
        jplay::drawRect(renderer_, &r);
    };
    // The magnify-plus glyph (ICON_MDI_MAGNIFY_PLUS, U+F034B) + "Zoom To", the
    // same shape as the "Snap" button beside it. Never greys: it runs no fit of
    // its own, and there is always a list to show.
    {
        const bool hov = hoveredFit_;
        face(fitBtnRect_, hov);
        SDL_Color fg = hov ? kFitHot : kIconLight;
        SDL_FRect inner = fitBtnRect_;
        SDL_FRect well = cutLeft(inner, fitBtnRect_.h);
        icons_.drawGlyph(renderer_, 0xF034B, well, fg, 0.2f);
        SDL_FRect text = centerV(inner, textFont_.lineHeight());
        drawText(text.x, text.y, fg, kFitBtnLabel);
    }
}

void App::zoomAt(float mouseX, double factor) {
    zoomFitSeqIdx_ = zoomFitShotId_ = -1; // view no longer matches a fit range
    double anchor = xToFrame(mouseX);
    double minFpp = 1.0 / 200.0; // max zoom: 200 px per frame
    double maxFpp = std::max(1.0, (double)timeline_.length()) / 50.0;
    framesPerPx_ = std::clamp(framesPerPx_ * factor, minFpp, maxFpp);
    viewStart_ = anchor - (mouseX - headerX_) * framesPerPx_;
}

// Zoom holding `anchorFrame` at a fixed fraction across the content width, for
// callers outside the timeline that have a frame to keep rather than an x.
void App::zoomAtFrame(double anchorFrame, double screenFrac, double factor) {
    zoomFitSeqIdx_ = zoomFitShotId_ = -1; // view no longer matches a fit range
    double w = std::max(contentW_ > 0 ? contentW_ : winW_ - headerW(), 100.0f);
    double minFpp = 1.0 / 200.0; // max zoom: 200 px per frame
    double maxFpp = std::max(1.0, (double)timeline_.length()) / 50.0;
    framesPerPx_ = std::clamp(framesPerPx_ * factor, minFpp, maxFpp);
    viewStart_ = anchorFrame - screenFrac * w * framesPerPx_;
}

int64_t App::scrubFrame(double x) const {
    int64_t f = (int64_t)std::llround(xToFrame(x));
    // Confine to the scoped sequence, matching setPlayhead: a click in front of it
    // would land on another sequence's frames, which read negative here.
    int64_t sa = 0, sb = 0;
    if (scopeRange(sa, sb))
        f = std::clamp(f, sa, std::max(sa, sb - 1));
    if (!snapPlayhead_)
        return f;
    // Snap to the nearest clip start within a fixed screen-pixel radius, so the
    // magnet feels the same at any zoom. Only clips in the scoped sequence(s) are
    // considered when a filter is active, matching jumpClip.
    const double kSnapPx = 8.0;
    int64_t best = f;
    double bestPx = kSnapPx;
    auto consider = [&](int64_t start) {
        double px = std::fabs(frameToX((double)start) - x);
        if (px < bestPx) { bestPx = px; best = start; }
    };
    forEachViewClip([&](const Clip& c) { consider(c.timelineStart); });
    return best;
}

void App::setPlayhead(int64_t f) {
    int64_t last = std::max<int64_t>(timeline_.length() - 1, 0);
    int64_t lo = 0;
    int64_t a = 0, b = 0;
    if (scopeRange(a, b)) {
        // Confine the frame indicator to the scoped sequence: frames in front of it
        // belong to another sequence (the player shows black there) and read as
        // negative in the scope-relative numbering. A scope with no clips yet is a
        // single point at its insert point — one past the end of the timeline, but
        // valid to sit on, so the indicator marks where its first clip will land.
        lo = a;
        last = std::clamp(last, a, std::max(a, b - 1));
    }
    f = std::clamp<int64_t>(f, lo, std::max(lo, last));
    if (f != timeline_.playhead)
        playDir_ = f >= timeline_.playhead ? 1 : -1;
    timeline_.playhead = f;
}

void App::togglePlay() {
    if (transportLocked()) { spectatorLocked("CONTROL PLAYBACK"); return; } // host controls transport
    playing_ = !playing_;
    playAcc_ = 0.0;
    if (playing_) {
        playDir_ = 1;
        // If the playhead sits outside the active in/out range, snap it to the
        // range start so playback resumes inside the loop rather than stalling
        // on a frame that is never cached / advanced.
        int64_t lo = 0, hi = 0;
        playbackRange(lo, hi);
        if (timeline_.playhead < lo || timeline_.playhead > hi)
            setPlayhead(lo);
    }
}

// Master volume, set from the info-bar slider. Dragging the slider also lifts a
// mute: the level the user just picked is what they want to hear. Not persisted
// here — writePrefs() runs on the drag release (see the MOUSE_BUTTON_UP handler),
// so a drag does not rewrite settings.conf on every motion tick.
void App::setVolume(float v) {
    volume_ = std::clamp(v, 0.0f, 1.0f);
    muted_ = false;
    if (audio_)
        audio_->setVolume(volume_);
}

void App::toggleMute() {
    muted_ = !muted_;
    if (audio_)
        audio_->setVolume(muted_ ? 0.0f : volume_);
    writePrefs();
}

void App::jumpClip(int dir, bool markRange) {
    if (transportLocked()) { spectatorLocked("CHANGE FRAME"); return; } // host controls transport
    // Land on the first frame of the previous / next clip (by timeline start),
    // clamped to the timeline bounds when there is none in that direction.
    // When a filter is active, only clips in the scoped sequence(s) are considered.
    playing_ = false;
    int64_t last = std::max<int64_t>(timeline_.length() - 1, 0);
    int64_t target = dir < 0 ? 0 : last;
    auto check = [&](const Clip& c) {
        if (c.audio)
            return; // prev/next clip navigates the video program only
        if (dir < 0 && c.timelineStart < timeline_.playhead)
            target = std::max(target, c.timelineStart);
        if (dir > 0 && c.timelineStart > timeline_.playhead)
            target = std::min(target, c.timelineStart);
    };
    forEachViewClip(check);
    setPlayhead(target);
    // PgUp/PgDn double as "review this clip": the range follows the jump so
    // playback loops what we landed on. The clip is picked after the jump, the
    // same way X does it (topmost at the playhead); landing in a gap — only
    // possible when the clamp above ran out of clips — leaves the range alone.
    if (markRange)
        if (const Clip* clip = getTopMostClipAtFrame(timeline_.playhead))
            markClipRange(*clip);
}

void App::markClipRange(const Clip& c) {
    timeline_.inPoint  = c.timelineStart;
    timeline_.outPoint = c.end() - 1; // outPoint is the last playable frame
    rangeAnchorClipId_ = c.id;
    rangeExpand_ = 0;                 // Shift+PgUp/PgDn grows out from here
    // Only the range's own frames are cached (see submitCacheRequests), so a
    // playhead left outside it would sit on a frame that never decodes.
    if (timeline_.playhead < timeline_.inPoint || timeline_.playhead > timeline_.outPoint)
        setPlayhead(timeline_.inPoint);
    setStatus("RANGE: " + std::to_string(timeline_.inPoint) + " - " +
              std::to_string(timeline_.outPoint));
}

// Grow (dir>0) or shrink (dir<0) the playback range by one clip on each side of
// the anchor clip, so a review of one clip widens symmetrically into its
// neighbours and back again. The anchor and the count are the whole state — the
// range is recomputed from them on every press rather than nudged — so the walk
// out and back lands on exactly the same edges each time.
void App::expandClipRange(int dir) {
    if (transportLocked()) { spectatorLocked("CHANGE FRAME"); return; } // host controls transport
    // The clips Shift walks are the ones PgUp/PgDn navigates: the video program
    // of the scoped sequence(s), in timeline order.
    std::vector<const Clip*> clips;
    forEachViewClip([&](const Clip& c) {
        if (!c.audio)
            clips.push_back(&c);
    });
    std::sort(clips.begin(), clips.end(), [](const Clip* a, const Clip* b) {
        if (a->timelineStart != b->timelineStart)
            return a->timelineStart < b->timelineStart;
        return a->track < b->track;
    });
    if (clips.empty()) {
        setStatus("NO CLIP TO MARK", 2000);
        return;
    }

    // The span of clips[ai] plus n clips on each side of it. Clips can overlap
    // across tracks, so both edges are taken over the whole run of them rather
    // than off its two ends.
    auto rangeFor = [&](int ai, int n, int64_t& lo, int64_t& hi) {
        const int a = std::max(0, ai - n);
        const int b = std::min((int)clips.size() - 1, ai + n);
        lo = clips[a]->timelineStart;
        hi = clips[a]->end() - 1;
        for (int i = a; i <= b; ++i) {
            lo = std::min(lo, clips[i]->timelineStart);
            hi = std::max(hi, clips[i]->end() - 1);
        }
    };

    // The anchor holds only as long as it still describes the range on screen: a
    // range set some other way ([ ], a clip double-click, cleared) re-anchors on
    // the clip under the playhead, which is also what makes Shift+PgDn work
    // without a plain PgUp/PgDn or X first.
    int ai = -1;
    for (int i = 0; i < (int)clips.size(); ++i)
        if (clips[i]->id == rangeAnchorClipId_) { ai = i; break; }
    if (ai >= 0) {
        int64_t lo = 0, hi = 0;
        rangeFor(ai, rangeExpand_, lo, hi);
        if (timeline_.inPoint != lo || timeline_.effectiveOut() != hi)
            ai = -1;
    }
    if (ai < 0) {
        const Clip* clip = getTopMostClipAtFrame(timeline_.playhead);
        if (!clip)
            clip = timeline_.findClipById(selectedClipId_); // same fallback X uses in a gap
        for (int i = 0; clip && i < (int)clips.size(); ++i)
            if (clips[i]->id == clip->id) { ai = i; break; }
        if (ai < 0) {
            setStatus("NO CLIP TO MARK", 2000);
            return;
        }
        rangeAnchorClipId_ = clips[ai]->id;
        rangeExpand_ = 0;
    }

    // Saturate exactly at full coverage, so contracting takes as many presses as
    // expanding took rather than winding down a count nothing on screen reflects.
    const int maxN = std::max(ai, (int)clips.size() - 1 - ai);
    rangeExpand_ = std::clamp(rangeExpand_ + dir, 0, maxN);
    int64_t lo = 0, hi = 0;
    rangeFor(ai, rangeExpand_, lo, hi);
    timeline_.inPoint = lo;
    timeline_.outPoint = hi;
    // Contracting can leave the playhead outside; park it on the nearest edge so
    // it stays on a frame the cache is filling (see markClipRange).
    if (timeline_.playhead < lo)
        setPlayhead(lo);
    else if (timeline_.playhead > hi)
        setPlayhead(hi);

    const int count = std::min((int)clips.size() - 1, ai + rangeExpand_) -
                      std::max(0, ai - rangeExpand_) + 1;
    setStatus("RANGE: " + std::to_string(count) + (count == 1 ? " CLIP  " : " CLIPS  ") +
              std::to_string(lo) + " - " + std::to_string(hi));
}

void App::followPlayhead() {
    double x = frameToX((double)timeline_.playhead);
    if (x < headerX_ || x > headerX_ + contentW_)
        viewStart_ = (double)timeline_.playhead - 0.1 * contentW_ * framesPerPx_;
}

// The track header's burger, drawn as three bars rather than an icon-font glyph:
// the MDI sheet is rasterised at 64pt, so a 10px draw is a 6x bilinear
// minification and comes out soft. Three 1px rects on whole logical units land
// exactly on the pixel grid at 100% UI scale (and scale cleanly above it).
static void drawBurger(SDL_Renderer* r, const SDL_FRect& box, SDL_Color c) {
    constexpr float kPitch = 3.0f;                    // bar top to bar top
    const float x = std::round(box.x), w = std::round(box.w);
    const float y = std::round(box.y + (box.h - (2.0f * kPitch + 1.0f)) * 0.5f);
    setColor(r, c);
    for (int i = 0; i < 3; ++i) {
        SDL_FRect bar{ x, y + i * kPitch, w, 1.0f };
        jplay::fillRect(r, &bar);
    }
}

// ---------------------------------------------------------------- timeline render

void App::renderTimeline() {
    // Panel background
    setColor(renderer_, kPanelBg);
    jplay::fillRect(renderer_, &tlRect_);

    int64_t length = timeline_.length();

    // Sequence filter + display offset. When the view is scoped (to one sequence or
    // to a project), frame labels are shown relative to the scope's start (so its
    // first clip reads as "0").
    const std::vector<int> viewSeqs = viewSeqIndices();
    auto seqInView = [&](int si) {
        return std::find(viewSeqs.begin(), viewSeqs.end(), si) != viewSeqs.end();
    };
    int64_t dispOffset = 0;    // absolute frame that maps to display "0"
    int64_t seqEndFrame = length; // absolute end frame for the display total
    {
        // A sequence with no clips yet (just added) has no span to measure, so
        // scopeRange reports its packed insert point — where its first clip lands.
        // Zeroing the numbering there keeps a new sequence reading from frame 0 and
        // stops the ruler renumbering the moment that first clip arrives.
        int64_t a = 0, b = 0;
        if (scopeRange(a, b)) {
            dispOffset = a;
            seqEndFrame = b;
        }
    }

    // Format a display-relative frame count as either a raw number or HH:MM:SS:FF.
    // When the total project is under 1 hour the leading "HH:" is omitted.
    // `length` rather than timeline_.length(): the latter is a scan of every clip
    // in the project, and the ruler calls this once per tick label.
    auto fmtFrame = [this, length](int64_t f) -> std::string {
        if (showAsFrames())
            return std::to_string(f);
        double fps = timeline_.fps > 0.0 ? timeline_.fps : 24.0;
        int64_t totalSec = (int64_t)(f / fps);
        int h = (int)(totalSec / 3600);
        int m = (int)((totalSec % 3600) / 60);
        int s = (int)(totalSec % 60);
        int fr = (int)(f % (int64_t)std::llround(fps));
        char buf[32];
        bool shortFmt = length < (int64_t)(3600.0 * fps);
        if (shortFmt)
            SDL_snprintf(buf, sizeof(buf), "%02d:%02d:%02d", m, s, fr);
        else
            SDL_snprintf(buf, sizeof(buf), "%02d:%02d:%02d:%02d", h, m, s, fr);
        return buf;
    };

    // Clip-local frame/time at an absolute timeline frame as a bare string (no
    // prefix), or "-" when no clip sits there. In Frames mode this is the source
    // frame number (e.g. 1001-based); in Timecode mode it is clip-relative.
    auto fmtClipAt = [this, &fmtFrame](int64_t absFrame) -> std::string {
        const Clip* pc = getTopMostClipAtFrame(absFrame);
        if (!pc)
            return "-";
        int64_t base = 0;
        if (auto pm = timeline_.findMediaById(pc->mediaId); pm && pm->isOpen()) {
            std::string err;
            if (auto src = pm->ensureOpen(err))
                base = src->firstFrameNumber();
        }
        int64_t clipPos = pc->sourceOffset + (absFrame - pc->timelineStart);
        if (showAsFrames())
            return std::to_string(base + clipPos);
        return fmtFrame(clipPos);
    };

    // ---- info bar
    {
        setColor(renderer_, kPanelBg);
        jplay::fillRect(renderer_, &infoRect_);

        // This bar holds the transport buttons (centered): prev clip, play/pause,
        // next clip. Icons are drawn rather than text labels.
        const SDL_Color ic = kIconLight;
        drawTransportButton(prevClipBtnRect_, hoveredTransport_ == 0);
        drawTransportButton(playBtnRect_,     hoveredTransport_ == 1);
        drawTransportButton(nextClipBtnRect_, hoveredTransport_ == 2);

        // Prev clip (|<): a vertical bar plus a left-pointing triangle.
        {
            const SDL_FRect& rect = prevClipBtnRect_;
            float cx = rect.x + rect.w * 0.5f, cy = rect.y + rect.h * 0.5f;
            float tw = rect.w * 0.22f, th = rect.h * 0.30f, barW = 2.5f;
            SDL_FRect bar = { cx - tw - barW - 2.0f, cy - th, barW, th * 2 };
            SDL_SetRenderDrawColor(renderer_, ic.r, ic.g, ic.b, ic.a);
            jplay::fillRect(renderer_, &bar);
            fillTriangle(cx - tw, cy, cx + tw, cy - th, cx + tw, cy + th, ic);
        }

        // Play (right triangle) / Pause (two bars), depending on transport state.
        {
            const SDL_FRect& rect = playBtnRect_;
            float cx = rect.x + rect.w * 0.5f, cy = rect.y + rect.h * 0.5f;
            if (playing_) {
                float bw = rect.w * 0.16f, bh = rect.h * 0.52f, gap = bw * 0.85f;
                SDL_SetRenderDrawColor(renderer_, ic.r, ic.g, ic.b, ic.a);
                SDL_FRect b1 = { cx - gap - bw, cy - bh * 0.5f, bw, bh };
                SDL_FRect b2 = { cx + gap,      cy - bh * 0.5f, bw, bh };
                jplay::fillRect(renderer_, &b1);
                jplay::fillRect(renderer_, &b2);
            } else {
                float tw = rect.w * 0.22f, th = rect.h * 0.30f;
                fillTriangle(cx - tw, cy - th, cx - tw, cy + th, cx + tw, cy, ic);
            }
        }

        // Next clip (>|): a right-pointing triangle plus a vertical bar.
        {
            const SDL_FRect& rect = nextClipBtnRect_;
            float cx = rect.x + rect.w * 0.5f, cy = rect.y + rect.h * 0.5f;
            float tw = rect.w * 0.22f, th = rect.h * 0.30f, barW = 2.5f;
            fillTriangle(cx - tw, cy - th, cx - tw, cy + th, cx + tw, cy, ic);
            SDL_FRect bar = { cx + tw + 2.0f, cy - th, barW, th * 2 };
            SDL_SetRenderDrawColor(renderer_, ic.r, ic.g, ic.b, ic.a);
            jplay::fillRect(renderer_, &bar);
        }

        // Master volume, at the right end of the bar: a speaker button that
        // toggles mute, then the level slider. The glyph reports the state -
        // ICON_MDI_VOLUME_OFF when muted or at zero, else ICON_MDI_VOLUME_LOW /
        // ICON_MDI_VOLUME_MEDIUM / ICON_MDI_VOLUME_HIGH by level.
        {
            constexpr SDL_Color kIconMuted{ 138, 142, 150, 255 };
            drawTransportButton(volumeBtnRect_, hoveredVolumeBtn_);
            const float v = muted_ ? 0.0f : volume_;
            const uint32_t glyph = v <= 0.0f   ? 0xF0581   // ICON_MDI_VOLUME_OFF
                                 : v < 0.34f   ? 0xF057F   // ICON_MDI_VOLUME_LOW
                                 : v < 0.67f   ? 0xF0580   // ICON_MDI_VOLUME_MEDIUM
                                               : 0xF057E;  // ICON_MDI_VOLUME_HIGH
            icons_.drawGlyph(renderer_, glyph, volumeBtnRect_,
                             muted_ ? kIconMuted : ic, 0.22f);

            // Slider: a thin track down the middle of the hit region, filled to
            // the level, with a knob at the level. Muted draws the fill dimmed but
            // keeps it at the stored level, so unmuting reads as "back to there".
            SDL_FRect track = centerV(volumeSliderRect_, 4.0f * dpiScale);
            setColor(renderer_, SDL_Color{ 42, 45, 52, 255 });
            jplay::fillRect(renderer_, &track);
            float hx = track.x + std::clamp(volume_, 0.0f, 1.0f) * track.w;
            SDL_FRect fill = { track.x, track.y, hx - track.x, track.h };
            setColor(renderer_, muted_ ? SDL_Color{ 70, 78, 92, 255 }
                                       : SDL_Color{ 90, 120, 180, 255 });
            jplay::fillRect(renderer_, &fill);
            setColor(renderer_, kTransportBorder);
            jplay::drawRect(renderer_, &track);
            SDL_FRect knob = { hx - 2.0f * dpiScale, track.y - 3.0f * dpiScale,
                               4.0f * dpiScale, track.h + 6.0f * dpiScale };
            setColor(renderer_, hoveredVolumeSlider_ ? kIconLight : kIconMuted);
            jplay::fillRect(renderer_, &knob);
        }

        // Snap-to-clip toggle (ICON_MDI_MAGNET, U+F0347 + "Snap"): a square icon well
        // cut off the left edge, label centered in what is left, the geometry
        // computeLayout sized this rect for with iconTextBtnW. When enabled, scrubbing
        // snaps the playhead to clip start frames; the button reads highlighted while
        // active.
        {
            bool on = snapPlayhead_;
            bool hov = hoveredMagnet_;
            SDL_Color bg = on ? (hov ? colors().uibtnOnHover : colors().uibtnOnBg)
                              : (hov ? kUiBtnBgHover : kUiBtnBg);
            setColor(renderer_, bg);
            jplay::fillRect(renderer_, &magnetBtnRect_);
            setColor(renderer_, on ? colors().uibtnOnBorder : kUiBtnBorder);
            jplay::drawRect(renderer_, &magnetBtnRect_);
            SDL_Color fg = on ? colors().uibtnOnText
                              : (hov ? SDL_Color{ 235, 238, 245, 255 } : kIconLight);
            SDL_FRect inner = magnetBtnRect_;
            SDL_FRect well = cutLeft(inner, magnetBtnRect_.h);
            icons_.drawGlyph(renderer_, 0xF0347, well, fg, 0.2f);
            SDL_FRect text = centerV(inner, textFont_.lineHeight());
            drawText(text.x, text.y, fg, "Snap");
        }
        // Tool-mode pair, right of the snap toggle: cursor (the default) and the
        // razor. Same toggle art as the button above, but as a radio couple - the
        // active one is the highlighted one, and clicking either selects it rather
        // than flipping it. Icon-only; the tooltip names them.
        {
            auto drawTool = [&](const SDL_FRect& r, bool on, bool hov, uint32_t glyph) {
                SDL_Color bg = on ? (hov ? colors().uibtnOnHover : colors().uibtnOnBg)
                                  : (hov ? kUiBtnBgHover : kUiBtnBg);
                setColor(renderer_, bg);
                jplay::fillRect(renderer_, &r);
                setColor(renderer_, on ? colors().uibtnOnBorder : kUiBtnBorder);
                jplay::drawRect(renderer_, &r);
                SDL_Color fg = on ? colors().uibtnOnText
                                  : (hov ? SDL_Color{ 235, 238, 245, 255 } : kIconLight);
                icons_.drawGlyph(renderer_, glyph, r, fg, 0.2f);
            };
            // Empty rects in compact mode (computeLayout drops the pair), which is
            // the signal to draw nothing rather than a degenerate square.
            if (cursorToolBtnRect_.w > 0.0f) {
                drawTool(cursorToolBtnRect_, !razorMode(), hoveredCursorTool_,
                         0xF01C0); // ICON_MDI_CURSOR_DEFAULT
                drawTool(razorToolBtnRect_, razorMode(), hoveredRazorTool_,
                         0xF1997); // ICON_MDI_RAZOR_DOUBLE_EDGE
            }
        }
        // Zoom-fit button, right of the snap toggle.
        // computeLayout placed it; layoutFitButtons decided which fits are live.
        drawFitButtons();

        // Current clip's frame/time, left-aligned in the info bar. Always the
        // clip-local value (no prefix), rendered slightly larger for emphasis.
        const SDL_Color readout = kReadoutText;
        std::string label = fmtClipAt(timeline_.playhead);
        {
            const float scale = 1.3f;
            const float lh = textFont_.lineHeight() * scale;
            const float lx = infoRect_.x + 12.0f;
            const float ty = infoRect_.y + (infoRect_.h - lh) * 0.5f;
            //textFont_.draw(renderer_, lx, ty, readout, label.c_str(), scale);
            drawText(infoRect_.x + infoRect_.w, ty, kMutedText, label.c_str());
        }
    }

    int n = std::max(trackCount(), 1);
    // The bottom of the *viewport*, not of the stack: with more tracks than fit,
    // the rows below carry on past it and are scrolled to (see trackScroll_).
    float tracksBottom = tracksViewBottom();
    SDL_Rect contentClip = { (int)headerX_, (int)rulerRect_.y,
                             (int)std::ceil(contentW_), (int)std::ceil(tracksBottom - rulerRect_.y) };
    // Anything belonging to a row is clipped to the track band alone, so a row
    // scrolled past the top draws over neither the shot bar nor the gutter's
    // labels. Two of them: the full-width one takes in the label gutter (row fills
    // and headers), the content one starts after it (clips and the boxes over them).
    const SDL_Rect trackClip = { (int)tlRect_.x, (int)tracksTop_,
                                 (int)std::ceil(tlRect_.w), (int)std::ceil(tracksViewH_) };
    const SDL_Rect trackContentClip = { (int)headerX_, (int)tracksTop_,
                                        (int)std::ceil(contentW_), (int)std::ceil(tracksViewH_) };

    // ---- left gutter (track labels live here)
    {
        SDL_FRect gutter = { tlRect_.x, rulerRect_.y, headerW(), tracksBottom - rulerRect_.y };
        setColor(renderer_, kPanelBg);
        jplay::fillRect(renderer_, &gutter);
        if (seqBarRect_.h > 0)
            drawText(tlRect_.x + 8, seqBarRect_.y + (seqBarRect_.h - textFont_.lineHeight()) * 0.5f,
                     kMutedText, "SEQUENCE");
        if (shotBarRect_.h > 0)
            drawText(tlRect_.x + 8, shotBarRect_.y + (shotBarRect_.h - textFont_.lineHeight()) * 0.5f,
                     kMutedText, "SHOT");
    }

    // ---- ruler + cache-strip backgrounds (content side only)
    SDL_FRect rulerBg = { headerX_, rulerRect_.y, contentW_, kRulerH };
    setColor(renderer_, kRulerBg);
    jplay::fillRect(renderer_, &rulerBg);
    SDL_FRect cacheBg = { headerX_, cacheStripRect_.y, contentW_, kCacheStripH };
    setColor(renderer_, kCacheStripBg);
    jplay::fillRect(renderer_, &cacheBg);
    if (clipStripRect_.h > 0.0f) { // compact mode only; the clips go on it below
        SDL_FRect clipBg = { headerX_, clipStripRect_.y, contentW_, clipStripRect_.h };
        setColor(renderer_, kCacheStripBg);
        jplay::fillRect(renderer_, &clipBg);
    }

    // ---- track row backgrounds (single stack, top to bottom). Audio rows are
    // tinted a touch cooler than video rows so the two kinds read apart.
    SDL_SetRenderClipRect(renderer_, &trackClip); // rows + headers scroll inside the band
    for (int t = 0; t < n; ++t) {
        SDL_FRect row = { headerX_, trackRowY(t), contentW_, trackRowH(t) };
        if (row.h <= 0.0f)
            continue; // collapsed by curve mode: no fill, and no stray row line
        bool odd = (t & 1) != 0;
        if (timeline_.trackKind(t) == Timeline::TrackKind::Audio)
            SDL_SetRenderDrawColor(renderer_, odd ? 28 : 32, odd ? 30 : 34, odd ? 31 : 35, 255);
        else
            SDL_SetRenderDrawColor(renderer_, odd ? 30 : 34, odd ? 31 : 35, odd ? 34 : 38, 255);
        jplay::fillRect(renderer_, &row);
        setColor(renderer_, kRowLine);
        jplay::drawLine(renderer_, headerX_, row.y, headerX_ + contentW_, row.y);
    }

    // ---- track headers: label (faded if empty or the row is off) + the burger
    // that drops the row's menu. Hover only brightens the glyph, so at rest the
    // button doesn't pull the eye away from the label.
    auto drawTrackHeader = [&](int t, bool spare) {
        const SDL_FRect hr = trackHeaderRect(t);
        if (hr.h <= 0.0f)
            return; // collapsed by curve mode
        const bool empty = timeline_.trackEmpty(t);
        const bool off = timeline_.trackDisabled(t);
        SDL_Color col;
        if (empty && spare)
            col = kTrackLabelSpare;         // trailing placeholder track: dark grey
        else if (empty)
            col = kTrackLabelEmpty;
        else
            col = off ? kTrackLabelOff : kTrackLabel;
        // The row being renamed draws no label: the edit field covers it. On the
        // expanded curve lane the label sits near the top rather than halfway down
        // a very tall row, which is also where trackMenuRect puts the button.
        if (t != trackNameEdit_) {
            const float labelH = t == curveTrack_ ? std::min(hr.h, kTrackH) : hr.h;
            drawText(hr.x + 8, hr.y + (labelH - 8) * 0.5f, col, trackLabel(t).c_str());
        }

        // No menu button on the trailing placeholder track (it's the always-empty
        // spare row, removed automatically rather than by the user).
        if (empty && spare)
            return;
        const bool hot = hoveredTrackMenu_ == t;
        const SDL_Color btn = hot ? kTrackBtnHot : (off ? kTrackBtnOff : kTrackBtn);
        // On the expanded row the button is the way out of curve mode, so it says
        // so: the burger becomes a collapse box, and clicking it leaves instead of
        // dropping the row's menu (see the press handler in App.cpp). No padding —
        // the button is only 10px, and the default inset would leave too little
        // glyph to read.
        if (t == curveTrack_) {
            // A logical pixel wider and taller than the hit box: the box glyph
            // reads a touch small against the burger at the shared 10px, and
            // growing only the draw rect leaves the hit box (and the other rows'
            // burgers) alone. Whole units, so the outline stays a hairline.
            SDL_FRect gr = trackMenuRect(t);
            gr.w += 1.0f;
            gr.h += 1.0f;
            icons_.drawGlyph(renderer_, 0xF06F2, gr, btn, 0.0f); // ICON_MDI_MINUS_BOX_OUTLINE
        }
        else
            drawBurger(renderer_, trackMenuRect(t), btn);
    };
    for (int t = 0; t < n; ++t)
        drawTrackHeader(t, t == n - 1);
    if (trackNameEdit_ >= 0) {
        trackNameField_.setRect(trackNameFieldRect(trackNameEdit_)); // follow the row
        trackNameField_.render(renderer_, &textFont_);
    }

    // Everything below is positioned by frame and clipped to the content area.
    SDL_SetRenderClipRect(renderer_, &contentClip);

    // ---- ruler ticks + in/out highlight
    if (length > 0) {
        static const int64_t steps[] = { 1, 2, 5, 10, 25, 50, 100, 250, 500, 1000, 2500, 5000, 10000, 25000, 50000, 100000 };
        double minTickPx = showAsFrames() ? 64.0 : 100.0; // timecodes are wider
        int64_t step = steps[0];
        for (int64_t s : steps) {
            if ((double)s / framesPerPx_ >= minTickPx) { step = s; break; }
            step = s;
        }
        // Tick grid runs on the display numbering, not on absolute frames, so a
        // scoped sequence gets a tick exactly on its own 0 (and round numbers after
        // it) instead of absolute multiples of `step` reading as 19 / 69 / 119.
        int64_t first = dispOffset
                      + (int64_t)std::floor((xToFrame(headerX_) - (double)dispOffset) / (double)step) * step;
        int64_t lastVisible = (int64_t)std::ceil(xToFrame(headerX_ + contentW_));
        // Markings live at the very top of the ruler: short tick marks hanging
        // from the top edge, with the frame/time numbers just beneath them.
        float markTop = rulerRect_.y + 2.0f * dpiScale;
        float tickLen = 5.0f * dpiScale;
        float minorLen = 3.0f * dpiScale;
        float numY = markTop + tickLen + 1.0f;

        // The numbers fade out as they approach the red playhead label so the
        // two never collide, vanishing entirely directly beneath it. The faded
        // hover indicator gets the same treatment when it's showing.
        float phx = (float)frameToX((double)timeline_.playhead);
        auto phLabel = frameNumberingClip_ ? fmtClipAt(timeline_.playhead)
                                           : fmtFrame(timeline_.playhead - dispOffset);
        float phHalf = textFont_.measure(renderer_, phLabel.c_str()) * 0.5f;
        float fadeOuter = phHalf + 7.0f * dpiScale;

        // Mirror the hover-preview placement (see below) so ruler numbers also
        // clear the faded hover label.
        bool hoverShown = false;
        float hvx = 0.0f, hvHalf = 0.0f, hvOuter = 0.0f;
        if (tlHoverActive_) {
            int64_t hf = std::clamp<int64_t>(scrubFrame(tlHoverX_),
                                             dispOffset,
                                             std::max<int64_t>(seqEndFrame - 1, dispOffset));
            if (hf != timeline_.playhead) {
                hoverShown = true;
                hvx = (float)frameToX((double)hf);
                auto hvLabel = frameNumberingClip_ ? fmtClipAt(hf)
                                                   : fmtFrame(hf - dispOffset);
                hvHalf = textFont_.measure(renderer_, hvLabel.c_str()) * 0.5f;
                hvOuter = hvHalf + 7.0f * dpiScale;
            }
        }

        // Nothing in front of the display zero gets marked: those frames are outside
        // the scope, and labelling them would print negative numbers.
        for (int64_t f = std::max<int64_t>(first, dispOffset); f <= lastVisible; f += step) {
            float x = (float)frameToX((double)f);
            // 9 minor subdivisions between this major tick and the next.
            setColor(renderer_, kTickMinor);
            for (int i = 1; i < 10; ++i) {
                float mx = (float)frameToX((double)f + (double)step * i / 10.0);
                jplay::drawLine(renderer_, mx, markTop, mx, markTop + minorLen);
            }
            setColor(renderer_, kTickMajor);
            jplay::drawLine(renderer_, x, markTop, x, markTop + tickLen);
            auto label = frameNumberingClip_ ? fmtClipAt(f) : fmtFrame(f - dispOffset);
            float tw = textFont_.measure(renderer_, label.c_str());
            float dx = std::fabs(x - phx);
            float fade = std::clamp((dx - phHalf) / std::max(fadeOuter - phHalf, 1.0f),
                                    0.0f, 1.0f);
            if (hoverShown) {
                float hdx = std::fabs(x - hvx);
                float hfade = std::clamp((hdx - hvHalf) / std::max(hvOuter - hvHalf, 1.0f),
                                         0.0f, 1.0f);
                fade = std::min(fade, hfade);
            }
            if (fade > 0.01f) {
                Uint8 a = (Uint8)std::lround(255.0 * fade);
                // Centred on its tick, but never past the left edge of the content
                // area: compact mode puts that edge at the window's, where half a
                // number would otherwise be clipped away.
                drawText(std::max(x - tw * 0.5f, headerX_ + 1.0f), numY,
                         { kMutedText.r, kMutedText.g, kMutedText.b, a }, label);
            }
        }

        int64_t rangeLo = 0, rangeHi = 0;
        playbackRange(rangeLo, rangeHi); // confined to the scope, so never in front of it
        float ix = (float)frameToX((double)rangeLo);
        float ox = (float)frameToX((double)rangeHi + 1.0);
        SDL_FRect range = { ix, rulerRect_.y, std::max(ox - ix, 1.0f), kRulerH };
        SDL_SetRenderDrawColor(renderer_, kInOutRange.r, kInOutRange.g, kInOutRange.b, 40);
        jplay::fillRect(renderer_, &range);
        setColor(renderer_, kInOutRange);
        jplay::drawLine(renderer_, ix, rulerRect_.y, ix, rulerRect_.y + kRulerH - 1);
        jplay::drawLine(renderer_, ox, rulerRect_.y, ox, rulerRect_.y + kRulerH - 1);
    }

    // The media pool is a map keyed by a string id, and every loop below asks it
    // for the same entry over and over — a cut list built off one source is
    // thousands of clips of one media. Remembering the last answer turns a map
    // traversal plus a shared_ptr refcount pair per clip into a string compare.
    // The pointer is safe for the length of this pass: nothing here edits the
    // pool, and Media entries are owned by the timeline, not by the clips.
    const std::string* memoMediaId = nullptr;
    Media* memoMedia = nullptr;
    auto mediaOf = [&](const std::string& id) -> Media* {
        if (!memoMediaId || *memoMediaId != id) {
            auto pm = timeline_.findMediaById(id);
            memoMedia = pm.get();
            memoMediaId = &id;
        }
        return memoMedia;
    };

    // ---- cache strip: green where frames are resident
    {
        // Asked per pixel column rather than per resident frame. The strip is only
        // contentW_ pixels wide, but the cache holds a frame per source frame it
        // has read (thousands once it fills) and the timeline holds a clip per cut,
        // so sweeping every frame against every clip - what this did - costs the
        // product of the two every frame, and grows as the cache fills. Bucketing
        // the resident frames by media turns "is any frame under this column
        // resident" into a binary search, and the work becomes the strip's own
        // width instead.
        // Rebuilt on a timer, not every frame: the strip is a coarse progress
        // indicator, while a rebuild takes the cache lock and copies a key per
        // resident frame (thousands once the cache fills) into a fresh map. At 60 Hz
        // that was the UI thread's second contention point on that lock.
        const double nowMs = (double)SDL_GetTicks();
        if (cacheStripAtMs_ < 0.0 || nowMs - cacheStripAtMs_ >= kCacheStripRefreshMs) {
            cacheStripAtMs_ = nowMs;
            std::vector<CacheKey> keys;
            cache_->snapshotKeys(keys);
            cacheStripIndex_.clear();
            for (const auto& k : keys)
                cacheStripIndex_[k.media].push_back(k.frame);
            for (auto& kv : cacheStripIndex_)
                std::sort(kv.second.begin(), kv.second.end());
        }
        const std::map<std::string, std::vector<int64_t>>& resident = cacheStripIndex_;

        setColor(renderer_, kCacheResident);
        const float stripRight = (float)(headerX_ + contentW_);
        // Same memo bargain as mediaOf above, against the residency map: a cut
        // list off one source asks for the same key thousands of times over.
        const std::string* memoResId = nullptr;
        const std::vector<int64_t>* memoRes = nullptr;
        // Rightmost column already painted green. A clip that falls entirely
        // inside it can only repaint what is there, whatever its own residency
        // says, so it is skipped before the per-column search.
        int lastGreen = INT_MIN;
        timeline_.forEachClip([&](const Clip& c) {
            if (!memoResId || *memoResId != c.mediaId) {
                auto it = resident.find(c.mediaId);
                memoRes = (it == resident.end()) ? nullptr : &it->second;
                memoResId = &c.mediaId;
            }
            if (!memoRes)
                return; // nothing of this media is cached
            const std::vector<int64_t>& frames = *memoRes;
            float cx0 = (float)frameToX((double)c.timelineStart);
            float cx1 = (float)frameToX((double)c.end());
            if (cx1 < headerX_ || cx0 > stripRight)
                return;
            int px0 = (int)std::floor(std::max(cx0, (float)headerX_));
            int px1 = (int)std::ceil(std::min(cx1, stripRight));
            if (px1 - px0 <= 1 && px0 <= lastGreen)
                return; // that column is green already
            // Merge adjacent resident columns into one rect: at a zoom where a
            // frame is many pixels wide that is one fill for the frame, not one
            // per column.
            int runStart = -1;
            auto flushRun = [&](int endX) {
                if (runStart < 0)
                    return;
                SDL_FRect rect = { (float)runStart, cacheStripRect_.y + 1,
                                   (float)(endX - runStart), kCacheStripH - 2 };
                jplay::fillRect(renderer_, &rect);
                lastGreen = std::max(lastGreen, endX - 1);
                runStart = -1;
            };
            for (int x = px0; x < px1; ++x) {
                // Source frames under this column, clamped to the clip's own span:
                // at a zoomed-out view one column covers many frames, and the strip
                // has always shown a column green when any of them is resident.
                int64_t t0 = std::max(c.timelineStart,
                                      (int64_t)std::floor(xToFrame((double)x)));
                int64_t t1 = std::min(c.end(),
                                      (int64_t)std::ceil(xToFrame((double)x + 1.0)));
                if (t1 <= t0)
                    t1 = std::min(c.end(), t0 + 1);
                bool hit = false;
                if (t1 > t0) {
                    int64_t a = c.sourceOffset + (t0 - c.timelineStart);
                    int64_t b = c.sourceOffset + (t1 - c.timelineStart);
                    auto lo = std::lower_bound(frames.begin(), frames.end(), a);
                    hit = lo != frames.end() && *lo < b;
                }
                if (hit) {
                    if (runStart < 0)
                        runStart = x;
                } else {
                    flushRun(x);
                }
            }
            flushRun(px1);
        });
    }

    // Pixel columns of the content area, the unit the bars and the clip rows
    // flatten themselves into once their spans fall under a pixel each (see the
    // shot bar and the clip bucket pass below).
    const int colCount = std::max(1, (int)std::ceil(contentW_) + 2);
    const int colBase  = (int)std::floor((float)headerX_);
    // Spans at or under this width are bucketed rather than drawn as themselves.
    // Below it a block is all border anyway (jplay::drawRect fills a rect too
    // narrow to hold two strokes), so there is nothing left to lose.
    constexpr float kDenseMaxW = 3.0f;

    // ---- clip strip: every clip in view flattened onto one band, so the cuts are
    // still readable with the track stack collapsed (compact mode). Each clip fills
    // its span in a lifted version of its row colour, with the clip border down both
    // edges - so two clips that abut meet as a pair of border lines, which is what
    // reads as the cut.
    if (clipStripRect_.h > 0.0f) {
        const float yTop = clipStripRect_.y;
        const float yBot = clipStripRect_.y + clipStripRect_.h - 1.0f;
        // frameToX of the clip's bounds, false when the span is off screen entirely
        // (the content clip rect trims what is only partly on).
        auto span = [&](const Clip& c, float& x0, float& x1) {
            x0 = (float)frameToX((double)c.timelineStart);
            x1 = (float)frameToX((double)c.end());
            return x1 >= headerX_ && x0 <= winW_;
        };
        // Zoomed out far enough thousands of clips land under a pixel each, and the
        // fill and both border lines are then the same column or two: the strip comes
        // out a flat smear with no cut left in it, for a fill and a line per clip. So
        // narrow spans are bucketed by column instead - the way the shot bar and the
        // clip rows below flatten - and the strip goes out as run-merged fills with a
        // 1px tick per cut, at most one per column however many clips land there.
        // Anything wider still draws as itself, so a legible zoom is untouched.
        // Kind codes, ordered so the std::max below resolves a column two clips
        // share: video over audio, the precedence the stacked rows would give it,
        // and within a kind the clip under the playhead over one that is not.
        constexpr uint8_t kAudioDim = 1, kAudioLive = 2, kVideoDim = 3, kVideoLive = 4;
        std::vector<uint8_t> stripCol((size_t)colCount, 0); // 0 none, else a kind above
        std::vector<uint8_t> stripCut((size_t)colCount, 0);
        struct WideSpan { float x0, x1; uint8_t kind; }; // span kept, not recomputed
        std::vector<WideSpan> stripWide;
        forEachViewClip([&](const Clip& c) {
            float x0 = 0.0f, x1 = 0.0f;
            if (!span(c, x0, x1))
                return;
            // The same "under the frame indicator" test the track rows use to pick
            // their live fill. Here it is the only thing saying which clip is
            // playing, the rows that would otherwise say it being collapsed.
            const bool live = timeline_.playhead >= c.timelineStart &&
                              timeline_.playhead < c.end();
            const uint8_t kind = c.audio ? (live ? kAudioLive : kAudioDim)
                                         : (live ? kVideoLive : kVideoDim);
            if (x1 - x0 > kDenseMaxW) {
                stripWide.push_back({ x0, x1, kind });
                return;
            }
            const int head = (int)std::floor(x0) - colBase; // the cut, before clamping
            int a = std::clamp(head, 0, colCount);
            int b = std::clamp(std::max(head + 1, (int)std::ceil(x1) - colBase), 0, colCount);
            for (int x = a; x < b; ++x)
                stripCol[(size_t)x] = std::max(stripCol[(size_t)x], kind);
            // Only where the cut actually falls on screen: a clip running in from the
            // left has its head off the content area, and clamping that to column 0
            // would draw a cut that is not there.
            if (head >= 0 && head < colCount)
                stripCut[(size_t)head] = 1;
        });
        // Audio first, so a video clip over the same frames is the fill that reads -
        // as it would if the flattened rows were stacked in track order, and dim
        // before live so the playing clip survives a shared column either way. Each
        // kind lays down its bucketed runs and then its wide clips, so a dense row
        // and a wide one over the same frames still resolve in that order too.
        for (uint8_t kind = kAudioDim; kind <= kVideoLive; ++kind) {
            setColor(renderer_, kind == kAudioDim  ? kClipStripAudioDim
                              : kind == kAudioLive ? kClipStripAudio
                              : kind == kVideoDim  ? kClipStripDim
                                                   : kClipStrip);
            int runStart = -1;
            auto flushFill = [&](int endX) {
                if (runStart < 0)
                    return;
                SDL_FRect rect = { (float)(colBase + runStart), yTop,
                                   (float)(endX - runStart), clipStripRect_.h };
                SDL_RenderFillRect(renderer_, &rect); // already column-aligned
                runStart = -1;
            };
            for (int x = 0; x < colCount; ++x) {
                if (stripCol[(size_t)x] == kind) {
                    if (runStart < 0)
                        runStart = x;
                } else {
                    flushFill(x);
                }
            }
            flushFill(colCount);
            for (const WideSpan& w : stripWide) {
                if (w.kind != kind)
                    continue;
                SDL_FRect rect = { w.x0, yTop, std::max(w.x1 - w.x0, 1.0f), clipStripRect_.h };
                jplay::fillRect(renderer_, &rect);
            }
        }
        // Borders last, over every fill, as the single sweep below used to be.
        setColor(renderer_, kClipBorder);
        int tickStart = -1;
        auto flushTicks = [&](int endX) {
            if (tickStart < 0)
                return;
            SDL_FRect rect = { (float)(colBase + tickStart), yTop,
                               (float)(endX - tickStart), yBot - yTop + 1.0f };
            SDL_RenderFillRect(renderer_, &rect);
            tickStart = -1;
        };
        for (int x = 0; x < colCount; ++x) {
            if (stripCut[(size_t)x]) {
                if (tickStart < 0)
                    tickStart = x;
            } else {
                flushTicks(x);
            }
        }
        flushTicks(colCount);
        for (const WideSpan& w : stripWide) {
            jplay::drawLine(renderer_, w.x0, yTop, w.x0, yBot);
            if (w.x1 - w.x0 > 2.0f) // narrower and the two lines would be the clip
                jplay::drawLine(renderer_, w.x1 - 1.0f, yTop, w.x1 - 1.0f, yBot);
        }
    }

    // A sequence's or shot's name, centered on the part of its span that is
    // actually on screen — a span can start far to the left of the content area,
    // and centering on the whole thing would push the label out of view. Trimmed
    // to that visible width, so a span too narrow for any of its name shows none.
    auto drawBarLabel = [&](const SDL_FRect& bar, const std::string& name) {
        const float visX0 = std::max(bar.x, headerX_);
        const float visX1 = std::min(bar.x + bar.w, headerX_ + contentW_);
        SDL_FRect vis = { visX0, bar.y, visX1 - visX0, bar.h };
        std::string fitted = fitText(name, vis.w - 6.0f);
        if (fitted.empty())
            return;
        SDL_FRect slot = center(vis, textFont_.measure(renderer_, fitted.c_str()),
                                textFont_.lineHeight());
        drawText(slot.x, slot.y, kBarLabel, fitted);
    };

    // ---- sequence bar: one colored span per sequence, with a centered black
    // label. The span covers the timeline range of the sequence's member clips.
    if (seqBarRect_.h > 0) {
        SDL_FRect seqBg = { headerX_, seqBarRect_.y, contentW_, seqBarRect_.h };
        setColor(renderer_, kSeqBarBg);
        jplay::fillRect(renderer_, &seqBg);
        for (int si = 0; si < (int)timeline_.sequences.size(); ++si) {
            if (!seqInView(si)) continue;
            const auto& s = timeline_.sequences[si];
            int64_t a = 0, b = 0;
            if (!timeline_.sequenceSpan(s, a, b))
                continue;
            float x0 = (float)frameToX((double)a);
            float x1 = (float)frameToX((double)b);
            if (x1 < headerX_ || x0 > winW_)
                continue;
            SDL_FRect bar = { x0, seqBarRect_.y + 1, std::max(x1 - x0, 2.0f), seqBarRect_.h - 2 };
            bool live = timeline_.playhead >= a && timeline_.playhead < b; // under frame indicator
            // Per-sequence background color, lifted while under the playhead.
            setColor(renderer_, seqBgSdlColor(s.bgColor, live ? kSeqBgLift : 1.0f));
            jplay::fillRect(renderer_, &bar);
            setColor(renderer_, kBarBorder);
            jplay::drawRect(renderer_, &bar);
            drawBarLabel(bar, s.name);
        }
    }

    // ---- shot bar: one colored block per shot, with centered name label.
    // Each shot has an explicit timeline position (unlike sequences which derive
    // theirs from member clip positions).
    if (shotBarRect_.h > 0) {
        SDL_FRect shotBg = { headerX_, shotBarRect_.y, contentW_, shotBarRect_.h };
        setColor(renderer_, kShotBarBg);
        jplay::fillRect(renderer_, &shotBg);
        // The shot bar flattens the same way the clip rows do: at a zoom where a
        // shot covers about a pixel, thousands of blocks resolve to one band of
        // colour, so they are bucketed by column and emitted as run-merged fills
        // with a 1px edge tick per shot. Wider shots draw as blocks with names.
        // 1 = shot, 2 = shot under the playhead; 0 = no shot in that column.
        std::vector<uint8_t> shotCol((size_t)colCount, 0);
        std::vector<uint8_t> shotEdge((size_t)colCount, 0);
        const float barY = shotBarRect_.y + 1;
        const float barH = shotBarRect_.h - 2;
        for (int si = 0; si < (int)timeline_.shots.size(); ++si) {
            const auto& s = timeline_.shots[si];
            if (!viewAll()) {
                // Scoped: only shots owned by an in-view sequence. The All view
                // draws every shot, including any owned by no sequence.
                bool inSeq = false;
                for (int si : viewSeqs) {
                    const auto& seq = timeline_.sequences[si];
                    if (std::find(seq.shotIds.begin(), seq.shotIds.end(), s.id)
                        != seq.shotIds.end()) { inSeq = true; break; }
                }
                if (!inSeq) continue;
            }
            float x0 = (float)frameToX((double)s.timelineStart);
            float x1 = (float)frameToX((double)s.end());
            if (x1 < headerX_ || x0 > winW_)
                continue;
            bool live = timeline_.playhead >= s.timelineStart && timeline_.playhead < s.end(); // under frame indicator
            if (x1 - x0 <= kDenseMaxW) {
                const int head = (int)std::floor(x0) - colBase; // the shot's edge, unclamped
                int a = std::clamp(head, 0, colCount);
                int b = std::clamp(std::max(head + 1, (int)std::ceil(x1) - colBase), 0, colCount);
                for (int x = a; x < b; ++x)
                    shotCol[(size_t)x] = live ? 2 : 1;
                // Only mark the edge where it actually falls on screen (see the
                // clip bucket pass below for the same clamp).
                if (head >= 0 && head < colCount)
                    shotEdge[(size_t)head] = 1;
                continue;
            }
            SDL_FRect bar = { x0, barY, std::max(x1 - x0, 2.0f), barH };
            if (live) // dark deep grey, lifted while under the playhead
                setColor(renderer_, kShotBlockLive);
            else
                setColor(renderer_, kShotBlock); // dark deep grey: all shots
            jplay::fillRect(renderer_, &bar);
            setColor(renderer_, kShotBlockBorder);
            jplay::drawRect(renderer_, &bar);
            drawBarLabel(bar, s.name);
        }
        // Run-merge the bucketed columns, then their edge ticks. Already
        // column-aligned, so these go straight to SDL rather than through the
        // snapping wrappers, which would only round them to where they are.
        int runStart = -1;
        uint8_t runKind = 0;
        auto flushShots = [&](int endX) {
            if (runStart < 0)
                return;
            setColor(renderer_, runKind == 2 ? kShotBlockLive : kShotBlock);
            SDL_FRect rect = { (float)(colBase + runStart), barY,
                               (float)(endX - runStart), barH };
            SDL_RenderFillRect(renderer_, &rect);
            runStart = -1;
        };
        for (int x = 0; x < colCount; ++x) {
            if (shotCol[(size_t)x] != runKind) {
                flushShots(x);
                runKind = shotCol[(size_t)x];
                if (runKind)
                    runStart = x;
            }
        }
        flushShots(colCount);
        // Merged like the fills above: where every column is a shot edge the ticks
        // are a solid band, and one rect draws it.
        setColor(renderer_, kShotBlockBorder);
        int tickStart = -1;
        auto flushTicks = [&](int endX) {
            if (tickStart < 0)
                return;
            SDL_FRect rect = { (float)(colBase + tickStart), barY,
                               (float)(endX - tickStart), barH };
            SDL_RenderFillRect(renderer_, &rect);
            tickStart = -1;
        };
        for (int x = 0; x < colCount; ++x) {
            if (shotEdge[(size_t)x]) {
                if (tickStart < 0)
                    tickStart = x;
            } else {
                flushTicks(x);
            }
        }
        flushTicks(colCount);
    }

    // The vertical band every clip-sized box on `track` sits in: the track's row
    // inset top and bottom, by less on the half-height audio rows. Only the band
    // is shared — each caller supplies its own width, since these boxes are all
    // positioned by frame rather than cut off a parent.
    auto clipBand = [&](int track, bool audio) { return clipBandRect(track, audio); };

    // Where a clip sits and what colour it takes. Shared by the bucket pass and
    // the per-clip draw below, so the two can never disagree about either.
    // `media` is a bare pointer on purpose: the pool owns it for the length of
    // this pass, and a shared_ptr here would put back the refcount pair the memo
    // above exists to remove.
    struct ClipVisual {
        int64_t start = 0, dur = 0, src = 0; // trim preview applied
        float x0 = 0.0f, x1 = 0.0f;
        SDL_FRect band{};
        SDL_Color fill{};
        Uint8 alpha = 255;
        Uint32 fillKey = 0;                  // fill + alpha, packed, never 0
        Media* media = nullptr;
        int64_t fadeIn = 0, fadeOut = 0;
        bool trimmed = false, missing = false;
        bool onScreen = false, decorated = false;
    };
    auto clipVisual = [&](const Clip& c) {
        ClipVisual v;
        // While trimming a clip, preview its proposed geometry instead of the
        // committed values so the edge tracks the cursor live.
        v.trimmed = trimmingClip_ && c.id == trimClipId_;
        v.start = v.trimmed ? trimNewStart_ : c.timelineStart;
        v.dur   = v.trimmed ? trimNewDuration_ : c.duration;
        v.src   = v.trimmed ? trimNewSourceOffset_ : c.sourceOffset;
        v.x0 = (float)frameToX((double)v.start);
        v.x1 = (float)frameToX((double)(v.start + v.dur));
        v.onScreen = !(v.x1 < headerX_ || v.x0 > winW_);
        if (!v.onScreen)
            return v;
        // Audio clips render at the (half-height) audio-row size; video full size.
        v.band = clipBand(c.track, c.audio);
        v.alpha = (draggingClip_ && isClipSelected(c.id)) ? 110 : 255; // dim dragged sources
        if (timeline_.clipDisabled(c))
            v.alpha = 60; // disabled clip or row: transparent (a lower track shows through)
        v.media = mediaOf(c.mediaId);
        // "missing" once an open has been attempted and failed; not-yet-opened
        // media render as present (their file exists per the freshness check).
        v.missing = !v.media || v.media->openFailed();
        // Highlight the fill while the clip sits under the frame indicator.
        const bool current = (timeline_.playhead >= v.start && timeline_.playhead < v.start + v.dur);
        v.fill = c.audio ? kClipNormalAudio : kClipNormal; // not under playhead
        if (v.missing)
            v.fill = current ? kClipMissingLive : kClipMissing;
        else if (current)
            v.fill = c.audio ? kClipAudioLive : kClipLive;
        v.fillKey = ((Uint32)v.fill.r << 24) | ((Uint32)v.fill.g << 16) |
                    ((Uint32)v.fill.b << 8) | (Uint32)v.alpha;
        Timeline::clipFades(c, v.fadeIn, v.fadeOut);
        // Anything a flat column of colour cannot express. A fade is the one
        // decoration that still shows at a sub-pixel width — its grab dot is a
        // fixed 4px — so a clip carrying one is never flattened.
        v.decorated = v.fadeIn > 0 || v.fadeOut > 0 || v.trimmed ||
                      c.id == hoveredClipId_ || isClipSelected(c.id);
        return v;
    };

    // Zoomed far enough out a clip covers about a pixel, and a row is then fully
    // described by a colour per column: drawing it clip by clip is thousands of
    // overlapping 2px rects (the border alone fills one at that width), which
    // resolve to a flat band carrying no cut information at all. So clips that
    // narrow are bucketed into their row's columns instead, and the row is drawn
    // as run-merged fills with a 1px tick per cut — at most one per column,
    // however many clips land there. Everything wider, and everything carrying a
    // decoration, still draws as itself.
    // With the track stack collapsed to nothing - compact mode, where the flattened
    // clip strip above stands in for the rows - no row has any height, so every clip
    // here would bucket into a zero-height band and every wide one would be clipped
    // away. Skipping the sweep saves walking the whole view to draw nothing.
    const bool tracksOnScreen = tracksViewH_ > 0.0f;
    const int denseRows = tracksOnScreen ? std::max(n, 0) * 2 : 0; // [track * 2 + audio]
    std::vector<Uint32> colFill((size_t)denseRows * colCount, 0); // 0 = unpainted
    std::vector<const Clip*> colOwner((size_t)denseRows * colCount, nullptr);
    std::vector<uint8_t> colCut((size_t)denseRows * colCount, 0);
    std::vector<const Clip*> wideClips;

    // ---- clips, one row per track (filtered to the selected sequence if active).
    // From here to the hover preview below, everything hangs off a row, so the
    // narrower track band applies.
    SDL_SetRenderClipRect(renderer_, &trackContentClip);

    // Pass one: sort every clip into a column bucket or the per-clip list. Drawing
    // is deferred so the flattened rows land before the wide clips that sit beside
    // them, keeping the paint order the single loop used to have.
    auto bucketClip = [&](const Clip& c) {
        if (c.track < 0 || c.track >= n)
            return;
        if (trackRowH(c.track) <= 0.0f)
            return; // collapsed by curve mode: only the edited row is on screen
        ClipVisual v = clipVisual(c);
        if (!v.onScreen)
            return;
        if (v.x1 - v.x0 > kDenseMaxW || v.decorated) {
            wideClips.push_back(&c);
            return;
        }
        const int row = c.track * 2 + (c.audio ? 1 : 0);
        const int head = (int)std::floor(v.x0) - colBase; // the cut, before clamping
        int a = std::clamp(head, 0, colCount);
        int b = std::clamp(std::max(head + 1, (int)std::ceil(v.x1) - colBase), 0, colCount);
        const size_t base = (size_t)row * colCount;
        for (int x = a; x < b; ++x) {
            colFill[base + x] = v.fillKey;
            colOwner[base + x] = &c;
        }
        // The clip's head edge is the cut. Only when it is actually on screen — a
        // clip running in from the left has its cut off the left of the content
        // area, and clamping it to column 0 would draw a cut that isn't there.
        if (head >= 0 && head < colCount)
            colCut[base + head] = 1;
    };
    if (tracksOnScreen)
        forEachViewClip(bucketClip); // indexes the column buffers, so only when sized

    // Pass two: emit the bucketed rows. Equal-coloured columns merge into one
    // fill, so a row of thousands of cuts costs a handful of rects plus its ticks.
    // The rects are already column-aligned, so they go straight to SDL rather than
    // through the snapping wrappers, which would only round them to where they are.
    for (int row = 0; row < denseRows; ++row) {
        const size_t base = (size_t)row * colCount;
        const bool audio = (row & 1) != 0;
        const SDL_FRect band = clipBand(row / 2, audio);
        if (band.h <= 0.0f)
            continue;
        int runStart = -1;
        Uint32 runKey = 0;
        auto flushRun = [&](int endX) {
            if (runStart < 0)
                return;
            SDL_SetRenderDrawColor(renderer_, (Uint8)(runKey >> 24), (Uint8)(runKey >> 16),
                                   (Uint8)(runKey >> 8), (Uint8)runKey);
            SDL_FRect rect = { (float)(colBase + runStart), band.y,
                               (float)(endX - runStart), band.h };
            SDL_RenderFillRect(renderer_, &rect);
            runStart = -1;
        };
        for (int x = 0; x < colCount; ++x) {
            const Uint32 k = colFill[base + x];
            if (k != runKey) {
                flushRun(x);
                runKey = k;
                if (k)
                    runStart = x;
            }
        }
        flushRun(colCount);
        // Cut ticks over the fills, at the clip's own alpha so a disabled row's
        // cuts stay as faint as the clips they divide. Merged the same way the
        // fills are: zoomed out far enough that every column carries a cut, the
        // ticks *are* a solid band, and drawing it as one costs one rect.
        int tickStart = -1;
        Uint8 tickAlpha = 0;
        auto flushTicks = [&](int endX) {
            if (tickStart < 0)
                return;
            SDL_SetRenderDrawColor(renderer_, kClipBorder.r, kClipBorder.g, kClipBorder.b, tickAlpha);
            SDL_FRect rect = { (float)(colBase + tickStart), band.y,
                               (float)(endX - tickStart), band.h };
            SDL_RenderFillRect(renderer_, &rect);
            tickStart = -1;
        };
        for (int x = 0; x < colCount; ++x) {
            const Uint8 a = colCut[base + x] ? (Uint8)colFill[base + x] : 0;
            if (a != tickAlpha) {
                flushTicks(x);
                tickAlpha = a;
                if (a)
                    tickStart = x;
            }
        }
        flushTicks(colCount);
        // Audio rows keep their waveform, read off the column buckets: the
        // envelope was always a peak per column, so driving it by column costs the
        // row's width instead of one pass per clip in it.
        if (!audio)
            continue;
        const double wfFps = timeline_.fps > 0.0 ? timeline_.fps : 24.0;
        const float midY = band.y + band.h * 0.5f;
        const float maxAmp = band.h * 0.5f - 1.0f;
        const WaveformCache::Peaks* pk = nullptr;
        const Clip* pkClip = nullptr;
        for (int x = 0; x < colCount; ++x) {
            const Clip* clip = colOwner[base + x];
            if (!clip)
                continue;
            if (clip != pkClip) {
                pkClip = clip;
                Media* pm = mediaOf(clip->mediaId);
                pk = (pm && !pm->openFailed()) ? waveforms_.find(pm->path()) : nullptr;
                if (pm && !pm->openFailed())
                    waveforms_.ensure(pm->path(), work_);
            }
            if (!pk || !pk->hasAudio || pk->bins.empty())
                continue;
            const float px = (float)(colBase + x);
            int64_t frame = (int64_t)std::floor(xToFrame((double)px + 0.5));
            double sec = (double)(clip->sourceOffset + (frame - clip->timelineStart)) / wfFps
                       - pk->startSec;
            if (sec < 0.0)
                continue;
            size_t bin = (size_t)(sec * pk->binsPerSec);
            if (bin >= pk->bins.size())
                continue;
            float amp = std::max(pk->bins[bin] * maxAmp, 0.5f);
            SDL_SetRenderDrawColor(renderer_, kClipWave.r, kClipWave.g, kClipWave.b,
                                   (Uint8)(kClipWave.a * (Uint8)colFill[base + x] / 255));
            jplay::drawLine(renderer_, px, midY - amp, px, midY + amp);
        }
    }

    auto drawClip = [&](const Clip& c) {
        ClipVisual v = clipVisual(c);
        if (!v.onScreen)
            return;
        const bool beingTrimmed = v.trimmed;
        const int64_t cStart = v.start, cDur = v.dur, cSrc = v.src;
        const float x0 = v.x0, x1 = v.x1;
        const SDL_FRect band = v.band;
        const Uint8 a = v.alpha;
        Media* pm = v.media;
        const bool missing = v.missing;
        const SDL_Color fill = v.fill;
        const int64_t fadeIn = v.fadeIn, fadeOut = v.fadeOut;
        SDL_FRect rect = { x0 + 1, band.y, std::max(x1 - x0 - 2, 2.0f), band.h };

        SDL_SetRenderDrawColor(renderer_, fill.r, fill.g, fill.b, a);
        jplay::fillRect(renderer_, &rect);
        SDL_SetRenderDrawColor(renderer_, kClipBorder.r, kClipBorder.g, kClipBorder.b, a);
        jplay::drawRect(renderer_, &rect);

        // Audio clips draw their waveform as the clip's content: a per-column
        // peak envelope centered in the row, decoded in the background
        // (WaveformCache); nothing draws until the peaks are ready.
        //
        // A video clip can show the same thing for its embedded track, but only
        // under "View > Clip Waveform" and only for the clip under the playhead:
        // that audio is demuxed out of the video stream, so each envelope costs a
        // read of the clip's range of the file, and asking for one per visible clip
        // is not a trade worth making. Only the in/out range is decoded, which is
        // all this row can show anyway.
        const WaveformCache::Peaks* pk = nullptr;
        double wfFps = timeline_.fps > 0.0 ? timeline_.fps : 24.0;
        SDL_Color wfCol = kClipWave;
        if (pm && !missing) {
            if (c.audio) {
                waveforms_.ensure(pm->path(), work_);
                pk = waveforms_.find(pm->path());
            } else if (clipWaveform_ && pm->type() == ClipType::Video &&
                       playheadClip() && playheadClip()->id == c.id) {
                // sourceOffset counts in the file's own frames, so the range is in
                // the media's rate - the same conversion App::updateAudio uses to
                // cue the mixer. Committed values, not the trim preview: a drag
                // must not spawn an envelope per pixel it passes through.
                double mfps = pm->info().fps > 0.0 ? pm->info().fps : wfFps;
                double startSec = (double)c.sourceOffset / mfps;
                double durSec   = (double)c.duration / mfps;
                // Only a stopped playhead asks for a decode - mid-playback that
                // read would contend with frame decoding for the same disk. An
                // envelope already in the cache keeps drawing while playing: that
                // is the same per-column loop the audio rows run regardless.
                if (!playing_)
                    waveforms_.ensure(pm->resolvedPath(), work_, startSec, durSec);
                pk = waveforms_.find(pm->resolvedPath(), startSec, durSec);
                wfFps = mfps;
                wfCol = kClipWaveVideo; // fainter: the name rows sit on top of it
            }
        }
        if (pk && pk->hasAudio && !pk->bins.empty()) {
            float midY   = rect.y + rect.h * 0.5f;
            float maxAmp = rect.h * 0.5f - 1.0f;
            int px0 = (int)std::floor(std::max((double)rect.x, (double)headerX_));
            int px1 = (int)std::ceil(std::min((double)(rect.x + rect.w),
                                              (double)(headerX_ + contentW_)));
            SDL_SetRenderDrawColor(renderer_, wfCol.r, wfCol.g, wfCol.b,
                                   (Uint8)(wfCol.a * a / 255));
            for (int x = px0; x < px1; ++x) {
                int64_t frame = (int64_t)std::floor(xToFrame((double)x + 0.5));
                double sec = (double)(cSrc + (frame - cStart)) / wfFps - pk->startSec;
                if (sec < 0.0)
                    continue;
                size_t bin = (size_t)(sec * pk->binsPerSec);
                if (bin >= pk->bins.size())
                    continue;
                float amp = std::max(pk->bins[bin] * maxAmp, 0.5f);
                jplay::drawLine(renderer_, (float)x, midY - amp, (float)x, midY + amp);
            }
        }
        // A clip narrower than the text inset has nothing legible to show -
        // fitText returns empty for a non-positive width - so the name/metadata
        // pair costs a path parse, a metadata expansion and a run of font
        // measurements per clip and draws nothing. Zoomed out to thousands of
        // cuts that is the whole clip loop's cost, paid every frame.
        // The razored clip shows the cut instead of its name (drawn below), so the
        // name/metadata pair is skipped outright rather than drawn and covered.
        if (rect.w > 8.0f && c.id != razorClipId_) {
            std::string baseName = hashSeqStem(
                std::filesystem::path(pm ? pm->path() : std::string()).stem().string(),
                pm && pm->type() == ClipType::ImageSequence);
            // Audio clips carry the file name alone; the metadata row is video-only.
            std::string metaRow = (pm && !c.audio) ? expandClipMetadata(clipMetadataTemplate(), *pm)
                                                   : std::string();
            // A missing source shows its filename over a "click to relocate" prompt
            // in place of the usual name/metadata pair.
            if (missing) {
                baseName = "Missing " +
                           std::filesystem::path(pm ? pm->path() : std::string()).filename().string();
                metaRow = "Click to relocate source";
            }
            // While trimming, replace the name/metadata pair with the live source frame
            // range (e.g. "5-100") so the user sees which footage they're keeping.
            if (beingTrimmed) {
                int64_t base = 0;
                if (pm && pm->isOpen()) {
                    std::string err;
                    if (auto src = pm->ensureOpen(err))
                        base = src->firstFrameNumber();
                }
                baseName = std::to_string(base + cSrc) + "-" + std::to_string(base + cSrc + cDur - 1);
                metaRow.clear();
            }
            float lineH = textFont_.lineHeight();
            std::string row1 = fitText(baseName, rect.w - 8.0f);
            std::string row2 = metaRow.empty() ? std::string() : fitText(metaRow, rect.w - 8.0f);
            if (!row2.empty()) {
                float gap = 2.0f;
                float startY = rect.y + (rect.h - lineH * 2.0f - gap) * 0.5f;
                if (!row1.empty()) {
                    float tw = textFont_.measure(renderer_, row1.c_str());
                    drawText(rect.x + (rect.w - tw) * 0.5f, startY, { kClipName.r, kClipName.g, kClipName.b, a }, row1);
                }
                float tw2 = textFont_.measure(renderer_, row2.c_str());
                drawText(rect.x + (rect.w - tw2) * 0.5f, startY + lineH + gap, { kClipDept.r, kClipDept.g, kClipDept.b, a }, row2);
            } else if (!row1.empty()) {
                float tw = textFont_.measure(renderer_, row1.c_str());
                drawText(rect.x + (rect.w - tw) * 0.5f, rect.y + (rect.h - lineH) * 0.5f, { kClipName.r, kClipName.g, kClipName.b, a }, row1);
            }
        }

        // Razor: the cut this click would make. A line down the clip, with the
        // source frame each side would end up on in place of the label - left is
        // the last frame the head keeps, right the first the tail starts on, so the
        // pair reads as two consecutive footage frames. Source numbering, like the
        // trim readout above, and from the same base.
        if (c.id == razorClipId_) {
            const float cutX = (float)frameToX((double)razorCutFrame_);
            SDL_SetRenderDrawColor(renderer_, kRazorLine.r, kRazorLine.g, kRazorLine.b, a);
            jplay::drawLine(renderer_, cutX, rect.y, cutX, rect.y + rect.h - 1.0f);
            int64_t base = 0;
            if (pm && pm->isOpen()) {
                std::string err;
                if (auto src = pm->ensureOpen(err))
                    base = src->firstFrameNumber();
            }
            // Frames mode gives the source frame number; Timecode mode the
            // clip-local time, which is the same split fmtClipAt makes for the
            // info bar - the two readouts should never disagree.
            auto fmtCut = [&](int64_t absFrame) {
                int64_t clipPos = cSrc + (absFrame - cStart);
                return showAsFrames() ? std::to_string(base + clipPos) : fmtFrame(clipPos);
            };
            std::string lhs = fmtCut(razorCutFrame_ - 1);
            std::string rhs = fmtCut(razorCutFrame_);
            const float lineH = textFont_.lineHeight();
            const float ty = rect.y + (rect.h - lineH) * 0.5f;
            const float pad = 3.0f * dpiScale;
            // Each number is dropped when its own side is too narrow to hold it;
            // the line itself always draws, so a thin clip still shows the cut.
            float lw = textFont_.measure(renderer_, lhs.c_str());
            if (cutX - lw - pad >= rect.x)
                drawText(cutX - lw - pad, ty, { kRazorLine.r, kRazorLine.g, kRazorLine.b, a }, lhs);
            float rw = textFont_.measure(renderer_, rhs.c_str());
            if (cutX + pad + rw <= rect.x + rect.w)
                drawText(cutX + pad, ty, { kRazorLine.r, kRazorLine.g, kRazorLine.b, a }, rhs);
        }

        // Fade ramps: a wedge from the clip's bottom corner up to the ramp's apex,
        // with a grab dot on top. Drawn inside the clip (not as a separate box) so
        // it reads as a property of the clip, which is what it is.
        if (!c.audio) {
            int64_t fin = fadeIn, fout = fadeOut;
            // Preview the dragged length live, as clip trim and dissolve resize do.
            if (fadingClip_ && c.id == fadeClipId_) {
                if (fadeEdge_ == 0) fin = fadeNewFrames_;
                else                fout = fadeNewFrames_;
            }
            const float yTop = rect.y, yBot = rect.y + rect.h - 1.0f;
            auto wedge = [&](int64_t frames, bool head) {
                if (frames <= 0)
                    return;
                float apexX = head ? (float)frameToX((double)(cStart + frames))
                                   : (float)frameToX((double)(cStart + cDur - frames));
                float baseX = head ? rect.x : rect.x + rect.w - 1.0f;
                apexX = std::clamp(apexX, rect.x, rect.x + rect.w - 1.0f);
                // Horizontal slices under the ramp line: a filled triangle without
                // needing a geometry path (the renderer draws rects and lines).
                setColor(renderer_, kFadeWedge);
                const int rows = (int)std::max(rect.h, 1.0f);
                for (int i = 0; i < rows; ++i) {
                    // Fraction along the ramp line, which runs from the base's
                    // bottom corner up to the apex on the top edge: the widest
                    // slice is the top one, tapering to nothing at the base.
                    float f = 1.0f - (float)i / (float)rows;
                    float x = baseX + (apexX - baseX) * f;
                    SDL_FRect row{ std::min(x, baseX), yTop + (float)i,
                                   std::abs(x - baseX), 1.0f };
                    if (row.w > 0.0f)
                        jplay::fillRect(renderer_, &row);
                }
                setColor(renderer_, kFadeLine);
                jplay::drawLine(renderer_, baseX, yBot, apexX, yTop);
                SDL_FRect dot{ apexX - 2.0f, yTop, 4.0f, 4.0f };
                setColor(renderer_, kFadeDot);
                jplay::fillRect(renderer_, &dot);
            };
            wedge(fin, true);
            wedge(fout, false);
        }

        // Selection: light-cream dotted border around every selected clip.
        // Hover (when not selected): a darker cream dotted border.
        if (isClipSelected(c.id))
            drawDottedRect(rect, kSelectCream);
        else if (c.id == hoveredClipId_)
            drawDottedRect(rect, kHoverCream);
    };
    for (const Clip* c : wideClips)
        drawClip(*c);

    // The curve lane sits on top of the clips it edits (their waveforms are its
    // backdrop) and under the playhead, which has to stay readable over it.
    if (curveMode())
        renderCurveLane();

    // ---- transitions: a translucent box straddling the cut, with the diagonal
    // split every NLE draws for a dissolve. Drawn after the clips so it sits on
    // top of both, which is also the hit-test order (see the timeline mouse press).
    auto drawTransition = [&](const Sequence& s, const Transition& t) {
        Timeline::TransitionSpan sp;
        if (!timeline_.resolveTransition(s, t, sp))
            return; // stale: nothing to draw (pruneTransitions drops it on the next edit)
        if (sp.a->track < 0 || sp.a->track >= n)
            return;
        // Preview the dragged geometry live, as clip trim does.
        int64_t tIn = t.inFrames, tOut = t.outFrames;
        if (resizingTransition_ && t.id == resizeTransId_) {
            tIn = resizeTransNewIn_;
            tOut = resizeTransNewOut_;
        }
        float x0 = (float)frameToX((double)(sp.cut - tIn));
        float x1 = (float)frameToX((double)(sp.cut + tOut));
        if (x1 < headerX_ || x0 > winW_)
            return;
        SDL_FRect band = clipBand(sp.a->track, false);
        SDL_FRect rect = { x0, band.y, std::max(x1 - x0, 2.0f), band.h };
        setColor(renderer_, kTransFill);
        jplay::fillRect(renderer_, &rect);
        setColor(renderer_, kTransBorder);
        jplay::drawRect(renderer_, &rect);
        // Bottom-left to top-right: the outgoing clip's weight falling away.
        setColor(renderer_, kTransGlyph);
        jplay::drawLine(renderer_, rect.x, rect.y + rect.h - 1.0f, rect.x + rect.w - 1.0f, rect.y);
        if (t.id == selectedTransitionId_)
            drawDottedRect(rect, kSelectCream);
    };
    for (int si : viewSeqIndices()) {
        const Sequence& seq = timeline_.sequences[si];
        for (const auto& t : seq.transitions)
            drawTransition(seq, t);
    }

    // ---- gap box over empty space between two clips: gray dotted on hover, a
    // brighter box with faint fill when selected (Delete closes it).
    auto drawGapBox = [&](int track, int64_t s, int64_t e, SDL_Color col, bool fill) {
        float x0 = (float)frameToX((double)s);
        float x1 = (float)frameToX((double)e);
        if (x1 < headerX_ || x0 > winW_)
            return;
        bool audio = timeline_.trackKind(track) == Timeline::TrackKind::Audio;
        SDL_FRect band = clipBand(track, audio);
        SDL_FRect rect = { x0 + 1, band.y, std::max(x1 - x0 - 2, 2.0f), band.h };
        if (fill) {
            SDL_SetRenderDrawColor(renderer_, col.r, col.g, col.b, 40);
            jplay::fillRect(renderer_, &rect);
        }
        drawDottedRect(rect, col);
    };
    if (selectedGapTrack_ >= 0)
        drawGapBox(selectedGapTrack_, selectedGapStart_, selectedGapEnd_, kGapSelected, true);
    if (hoverGapTrack_ >= 0 &&
        !(hoverGapTrack_ == selectedGapTrack_ && hoverGapStart_ == selectedGapStart_))
        drawGapBox(hoverGapTrack_, hoverGapStart_, hoverGapEnd_, kGapHover, false);

    // Highlight neighbours that a proposed span [ns,ne) on `track` would displace:
    // a dark-blue fill, dotted border and a symbol over the overlap. Downstream
    // neighbours (pushed forward) get a double-headed arrow. In `overwrite` mode
    // (clip drag), earlier neighbours whose tail is trimmed and neighbours fully
    // covered by the span get a scissors glyph instead. Shared by clip trim and
    // clip drag (both applied on release).
    auto drawDisplaceOverlay = [&](int track, int64_t ns, int64_t ne, int excludeId,
                                   bool overwrite = false) {
        bool audio = timeline_.trackKind(track) == Timeline::TrackKind::Audio;
        auto markOverlap = [&](const Clip& k) {
            // Never mark the dragged clips themselves as displaced (a group drag
            // has several selected clips that shouldn't flag one another).
            if (k.id == excludeId || k.track != track ||
                (draggingClip_ && isClipSelected(k.id)))
                return;
            int64_t a = std::max(ns, k.timelineStart);
            int64_t b = std::min(ne, k.end());
            if (b <= a)
                return;
            float x0 = (float)frameToX((double)a);
            float x1 = (float)frameToX((double)b);
            SDL_FRect band = clipBand(track, audio);
            SDL_FRect rect = { x0, band.y, std::max(x1 - x0, 1.0f), band.h };
            setColor(renderer_, kDisplaceFill);
            jplay::fillRect(renderer_, &rect);
            drawDottedRect(rect, kDisplaceBorder);
            // In overwrite mode nothing ripples: every overlapping clip
            // is trimmed (tail or head), split, or removed — so it always gets
            // scissors. Only ripple mode pushes neighbours (the arrow below).
            bool trimmed = overwrite;
            if (trimmed) {
                float side = std::min(rect.h, rect.w);
                if (side > 6.0f) {
                    float cx = rect.x + rect.w * 0.5f, cy = rect.y + rect.h * 0.5f;
                    SDL_FRect gr = { cx - side * 0.5f, cy - side * 0.5f, side, side };
                    icons_.drawGlyph(renderer_, 0xF0190 /* ICON_MDI_CONTENT_CUT */, gr, kDisplaceArrow);
                }
                return;
            }
            // Double-headed arrow centered in the overlap: this neighbour is
            // being displaced (pushed forward on release, back if you reverse).
            float half = std::min(rect.w * 0.5f - 4.0f, 14.0f);
            if (half > 4.0f) {
                const SDL_Color ac = kDisplaceArrow;
                float cy = rect.y + rect.h * 0.5f;
                float cx = rect.x + rect.w * 0.5f;
                float lx = cx - half, rx = cx + half;
                const float aw = 6.0f, ah = 5.0f; // arrowhead length / half-height
                SDL_SetRenderDrawColor(renderer_, ac.r, ac.g, ac.b, ac.a);
                jplay::drawLine(renderer_, lx, cy, rx, cy);
                fillTriangle(lx, cy, lx + aw, cy - ah, lx + aw, cy + ah, ac);
                fillTriangle(rx, cy, rx - aw, cy - ah, rx - aw, cy + ah, ac);
            }
        };
        forEachViewClip(markOverlap);
    };

    // ---- trim overlap: while trimming, flag the neighbours that ripple forward.
    if (trimmingClip_) {
        if (const Clip* tc = timeline_.findClipById(trimClipId_))
            drawDisplaceOverlay(tc->track, trimNewStart_,
                                trimNewStart_ + trimNewDuration_, trimClipId_);
    }

    // ---- drop preview: dotted box at the would-be drop location. Green in free
    // space; dark blue (with arrows on the clips it displaces) when it overlaps.
    if (draggingClip_ && dragRejected_) {
        // Drop is over another sequence: a clip can't cross sequences. Mark the
        // whole offending sequence region with a red dotted box (no drop preview).
        auto regs = timeline_.seqRegions();
        if (dragRejectSeq_ >= 0 && dragRejectSeq_ < (int)regs.size()) {
            const auto& reg = regs[dragRejectSeq_];
            float x0 = (float)frameToX((double)reg.start);
            float x1 = (float)frameToX((double)reg.end);
            SDL_FRect box = { x0 + 1, tracksTop_ + 2, std::max(x1 - x0 - 2, 4.0f),
                              tracksBottom - tracksTop_ - 4 };
            const SDL_Color red = kPlayhead;
            SDL_SetRenderDrawColor(renderer_, red.r, red.g, red.b, 32);
            jplay::fillRect(renderer_, &box);
            drawDottedRect(box, red);
            const char* msg = "Cannot move clip across sequences";
            std::string label = fitText(msg, box.w - 8.0f);
            if (!label.empty()) {
                float tw = textFont_.measure(renderer_, label.c_str());
                drawText(box.x + (box.w - tw) * 0.5f,
                         box.y + (box.h - textFont_.lineHeight()) * 0.5f, red, label);
            }
        }
    } else if (draggingClip_ && !dragOrig_.empty()) {
        // Group drop preview, offset by the same (dTrack, dFrame) delta derived
        // from the primary. Overwrite mode draws a translucent ghost box per
        // dragged clip (where it lands); ripple mode draws a vertical insertion
        // bar at each landing's cut point (where the track splits and everything
        // downstream shifts right).
        const DragClipOrig* prim = nullptr;
        for (const auto& o : dragOrig_)
            if (o.id == dragClipId_) prim = &o;
        if (prim) {
            const int dTrack = dragTargetTrack_ - prim->track;
            const int64_t dFrame = dragTargetStart_ - prim->start;
            const bool ripple = dragDropMode_ == DropMode::Ripple;
            // Red when the drop would land on a track of the wrong type (rejected),
            // dark blue when it displaces neighbours, green in free space.
            const SDL_Color col = dragBadType_ ? kPlayhead
                                : dragOverlaps_ ? kDisplaceBorder : kDropFree;
            for (const auto& o : dragOrig_) {
                int64_t ns = o.start + dFrame;
                int nt = o.track + dTrack;
                float x0 = (float)frameToX((double)ns);
                SDL_FRect band = clipBand(nt, o.audio);
                if (ripple) {
                    // Insertion bar: a 2px column at the cut frame, capped with a
                    // triangle at each end so it reads as an insert, not a playhead.
                    SDL_FRect bar = { x0 - 1.0f, band.y, 2.0f, band.h };
                    setColor(renderer_, col);
                    jplay::fillRect(renderer_, &bar);
                    const float cx = x0, cap = 4.0f;
                    fillTriangle(cx - cap, bar.y, cx + cap, bar.y, cx, bar.y + cap, col);
                    float by = bar.y + bar.h;
                    fillTriangle(cx - cap, by, cx + cap, by, cx, by - cap, col);
                    continue;
                }
                float x1 = (float)frameToX((double)(ns + o.duration));
                SDL_FRect box = { x0 + 1, band.y, std::max(x1 - x0 - 2, 2.0f), band.h };
                SDL_SetRenderDrawColor(renderer_, col.r, col.g, col.b, 70);
                jplay::fillRect(renderer_, &box);
                setColor(renderer_, col);
                jplay::drawRect(renderer_, &box);
            }
            if (dragOverlaps_ && !dragBadType_)
                for (const auto& o : dragOrig_)
                    drawDisplaceOverlay(o.track + dTrack, o.start + dFrame,
                                        o.start + dFrame + o.duration, o.id, !ripple);
        }
    }

    // ---- external file drag: fixed-width placeholder (true length is unknown
    // until the drop). Existing clips will ripple aside where it lands.
    if (fileHoverActive_) {
        float x0 = (float)frameToX((double)fileHoverStart_);
        bool audio = timeline_.trackKind(fileHoverTrack_) == Timeline::TrackKind::Audio;
        SDL_FRect band = clipBand(fileHoverTrack_, audio);
        // A known landing span (an aligned Clip Source drop) draws the box at
        // the real width, matching the clip it lines up with; otherwise the length
        // is a guess until the drop, hence the fixed width.
        float boxW = kHoverBoxPx;
        if (fileHoverSpan_ > 0) {
            float x1 = (float)frameToX((double)(fileHoverStart_ + fileHoverSpan_));
            boxW = std::max(x1 - x0 - 2.0f, 2.0f);
        }
        SDL_FRect box = { x0 + 1, band.y, boxW, band.h };
        SDL_Color col = kFileDrop;
        SDL_SetRenderDrawColor(renderer_, col.r, col.g, col.b, 40);
        jplay::fillRect(renderer_, &box);
        drawDottedRect(box, col);
        drawText(box.x + 5, box.y + (box.h - 8) * 0.5f, col, "DROP HERE");
    }

    SDL_SetRenderClipRect(renderer_, &contentClip); // back to the ruler-to-tracks band

    // ---- hover preview: faint playhead handle where a click would land
    if (tlHoverActive_ && length > 0) {
        int64_t hf = std::clamp<int64_t>(scrubFrame(tlHoverX_),
                                         dispOffset,
                                         std::max<int64_t>(seqEndFrame - 1, dispOffset));
        if (hf != timeline_.playhead) {
            const SDL_Color faint = { kPlayhead.r, kPlayhead.g, kPlayhead.b, 180 };
            float x = (float)frameToX((double)hf);
            float triHalfW = 6.0f * dpiScale;
            float triH = 7.0f * dpiScale;
            float baseY = rulerRect_.y + 2.0f * dpiScale;
            float tipY = baseY + triH;
            fillTriangle(x - triHalfW, baseY, x + triHalfW, baseY, x, tipY, faint);
            auto cur = frameNumberingClip_ ? fmtClipAt(hf) : fmtFrame(hf - dispOffset);
            float tw = textFont_.measure(renderer_, cur.c_str());
            drawText(x - tw * 0.5f, tipY + 1.0f, faint, cur);
        }
    }

    // ---- playhead
    {
        const SDL_Color red = kPlayhead;
        float x = (float)frameToX((double)timeline_.playhead);

        // Downward-facing triangle handle pinned to the top of the ruler, tip
        // pointing down toward the tracks.
        float triHalfW = 6.0f * dpiScale;
        float triH = 7.0f * dpiScale;
        float baseY = rulerRect_.y + 2.0f * dpiScale;
        float tipY = baseY + triH;
        fillTriangle(x - triHalfW, baseY, x + triHalfW, baseY, x, tipY, red);

        // Current frame/time label centered on the playhead, just below the
        // triangle. Follows the Frame Numbering setting: Global -> timeline-
        // relative, Clip -> the clip's local value.
        auto cur = frameNumberingClip_ ? fmtClipAt(timeline_.playhead)
                                       : fmtFrame(timeline_.playhead - dispOffset);
        float tw = textFont_.measure(renderer_, cur.c_str());
        float labelY = tipY + 1.0f;
        drawText(x - tw * 0.5f, labelY, red, cur);

        // Red line starts just below the green frame-cache strip so it cuts
        // through neither the label nor the cache line, then runs down through
        // the tracks.
        float lineTop = cacheStripRect_.y + kCacheStripH;
        SDL_SetRenderDrawColor(renderer_, red.r, red.g, red.b, red.a);
        jplay::drawLine(renderer_, x, lineTop, x, tracksBottom - 1);
    }

    SDL_SetRenderClipRect(renderer_, nullptr);

    // ---- track stack scrollbar: only there when the stack outgrows the viewport,
    // hugging the right edge of the track band. Grabbable, so the draw records its
    // geometry for the next frame's press (see ScrollbarDrag).
    {
        const SDL_FRect sbView = { tlRect_.x, tracksTop_, tlRect_.w, tracksViewH_ };
        drawScrollbar(renderer_, sbView, tracksTotalH(), trackScroll_, dpiScale, true, trackSb_);
    }

    // ---- timeline resize handle: a brighter strip along the top edge while the
    // cursor is on it or dragging it, matching the side panels' edge affordance.
    if (tlResizing_ || tlResizeHovered_) {
        SDL_FRect strip = { tlRect_.x, tlRect_.y, tlRect_.w, 2.0f };
        setColor(renderer_, kResizeHandle);
        jplay::fillRect(renderer_, &strip);
    }

    // ---- track reorder drag: a faint ghost of the grabbed row under the cursor,
    // and a marker at every boundary the row could be dropped at (the one the
    // release would use drawn bright, the rest faint). Clipped to the track band
    // rather than to the content area — the gesture starts in the gutter and the
    // row spans it — which also keeps a boundary belonging to a scrolled-away row
    // from being drawn up over the bars or the player.
    if (trackDragging_ && trackDragFrom_ >= 0) {
        SDL_SetRenderClipRect(renderer_, &trackClip);
        const float x0 = tlRect_.x, x1 = headerX_ + contentW_;
        const int rows = trackReorderRows();
        for (int i = 0; i <= rows; ++i) {
            float y = trackRowY(i);
            bool active = i == trackDropIns_;
            SDL_Color color = active ? SDL_Color{ 235, 238, 245, 255 }
                                 : SDL_Color{ 120, 124, 132, 140 };
            setColor(renderer_, color);
            jplay::drawLine(renderer_, x0, y, x1 - 1, y);
            if (active) // 2px, so the live target reads as a line rather than a rule
                jplay::drawLine(renderer_, x0, y - 1, x1 - 1, y - 1);
            // End caps mark the boundary as a drop target even where a row line
            // already runs along it.
            jplay::drawLine(renderer_, x0, y - 3, x0, y + 3);
            jplay::drawLine(renderer_, x1 - 1, y - 3, x1 - 1, y + 3);
        }
        float gh = trackRowH(trackDragFrom_);
        SDL_FRect ghost = { x0, std::clamp(trackDragGhostY_, tracksTop_, tracksBottom - gh),
                            x1 - x0, gh };
        SDL_SetRenderDrawColor(renderer_, 150, 160, 185, 40);
        jplay::fillRect(renderer_, &ghost);
        setColor(renderer_, { 190, 200, 220, 150 });
        jplay::drawRect(renderer_, &ghost);
        SDL_SetRenderClipRect(renderer_, nullptr);
    }

    // ---- visible playback rate (debug), in the window's bottom-right corner.
    // Only while playing: stopped it reads nothing useful, and the corner goes
    // back to being plain timeline. Drawn unclipped over the tracks, with a
    // translucent backing so it stays legible over whatever clip is under it.
    // fpsShown_ is already snapped/held (see update()), so the readout stays put
    // during steady playback instead of flickering between adjacent tenths.
    // Compact mode has no spare timeline in that corner — the bands fill the whole
    // strip — so there the readout lifts to just above the timeline and sits in the
    // player's bottom-right corner instead of covering the cache strip.
    if (playing_) {
        char fpsBuf[16];
        SDL_snprintf(fpsBuf, sizeof(fpsBuf), "%.1f fps", fpsShown_);
        const float pad = 5.0f * dpiScale;
        float tw = textFont_.measure(renderer_, fpsBuf);
        const float bottom = compactTimeline_ ? tlRect_.y : winH_;
        SDL_FRect box = { winW_ - tw - pad * 2.0f,
                          bottom - textFont_.lineHeight() - pad * 2.0f,
                          tw + pad * 2.0f, textFont_.lineHeight() + pad * 2.0f };
        SDL_SetRenderDrawColor(renderer_, 20, 21, 24, 190);
        jplay::fillRect(renderer_, &box);
        drawText(box.x + pad, box.y + pad, kMutedText, fpsBuf);
    }

    // ---- info-bar button tooltips: a small label dropped below the hovered
    // button. Drawn last (unclipped, on top) so it overlays the ruler beneath.
    {
        auto drawTooltip = [&](const SDL_FRect& btn, const char* text) {
            const float pad = 5.0f * dpiScale;
            float tw = textFont_.measure(renderer_, text);
            float bw = tw + pad * 2.0f;
            float bh = textFont_.lineHeight() + pad * 2.0f;
            float bx = std::clamp(btn.x + (btn.w - bw) * 0.5f, 4.0f, winW_ - bw - 4.0f);
            float by = btn.y + btn.h + 4.0f * dpiScale;
            SDL_FRect box = { bx, by, bw, bh };
            SDL_SetRenderDrawColor(renderer_, 20, 21, 24, 240);
            jplay::fillRect(renderer_, &box);
            setColor(renderer_, { 80, 82, 90, 255 });
            jplay::drawRect(renderer_, &box);
            drawText(bx + pad, by + pad, { 235, 238, 245, 255 }, text);
        };
        if (hoveredTransport_ == 0)      drawTooltip(prevClipBtnRect_, "Previous clip");
        else if (hoveredTransport_ == 1) drawTooltip(playBtnRect_, playing_ ? "Pause" : "Play");
        else if (hoveredTransport_ == 2) drawTooltip(nextClipBtnRect_, "Next clip");
        else if (hoveredVolumeBtn_)      drawTooltip(volumeBtnRect_, muted_ ? "Unmute" : "Mute");
        else if (hoveredVolumeSlider_) {
            char vb[24];
            SDL_snprintf(vb, sizeof(vb), "Volume %.0f%%", std::clamp(volume_, 0.0f, 1.0f) * 100.0f);
            drawTooltip(volumeSliderRect_, vb);
        }
        if (hoveredMagnet_)      drawTooltip(magnetBtnRect_,
                                             snapPlayhead_ ? "Snap to clips: on" : "Snap to clips: off");
        if (hoveredCursorTool_)  drawTooltip(cursorToolBtnRect_, "Cursor tool (V)");
        if (hoveredRazorTool_)   drawTooltip(razorToolBtnRect_, "Razor tool (C)");
    }
}

// ---------------------------------------------------------------- tracks

float App::trackRowH(int track) const {
    // Curve mode collapses every row but the edited one, which takes the whole
    // stack's height. That is what makes the lane the only thing under the shot
    // bar without any of the drawing or hit testing needing a second layout: a
    // zero-height row has no band, no header and no clip boxes.
    if (curveTrack_ >= 0)
        return track == curveTrack_ ? curveLaneH_ : 0.0f;
    // Fast path: the per-frame cache built in computeLayout, alongside the row
    // tops it was summed into (see App.h::trackRowH_).
    if (track >= 0 && track < (int)trackRowH_.size())
        return trackRowH_[(size_t)track];
    int n = std::max(trackCount(), 1);
    Timeline::TrackKind k = timeline_.trackKind(track);
    // The single trailing empty row is the half-height "spare" placeholder.
    if (track == n - 1 && k == Timeline::TrackKind::Empty)
        return kTrackH * 0.5f;
    // Audio tracks are half height; video (and empty mid-stack) rows full height.
    return (k == Timeline::TrackKind::Audio) ? kAudioTrackH : kTrackH;
}

float App::trackRowY(int track) const {
    // Fast path: read the per-frame cache built in computeLayout.
    if (!trackRowTop_.empty()) {
        int i = std::clamp(track, 0, (int)trackRowTop_.size() - 1);
        return trackRowTop_[i];
    }
    // Fallback before the first layout: sum the (variable) row heights directly.
    float y = tracksTop_ - trackScroll_;
    for (int t = 0; t < track; ++t)
        y += trackRowH(t);
    return y;
}

float App::tracksTotalH() const {
    int n = std::max(trackCount(), 1);
    float h = 0.0f;
    for (int t = 0; t < n; ++t)
        h += trackRowH(t);
    return h;
}

SDL_FRect App::clipBandRect(int track, bool audio) const {
    // In curve mode the row is the expanded lane, so the band has to follow the
    // real row height rather than the type's nominal one.
    const float h = curveTrack_ >= 0 ? trackRowH(track)
                                     : (audio ? kAudioTrackH : kTrackH);
    SDL_FRect row = { headerX_, trackRowY(track), contentW_, h };
    return inset(row, 0.0f, audio ? 1.0f : 3.0f);
}

// A track name is a plain label: letters, digits, space and underscore only.
// Anything else is dropped, whether it was typed or pasted into the field.
static std::string filterTrackName(const char* s) {
    std::string out;
    for (; *s; ++s) {
        char c = *s;
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
            c == ' ' || c == '_')
            out.push_back(c);
    }
    return out;
}

// A track's derived name follows its type, counted per type top-to-bottom: video
// tracks are "Video 1/2/...", audio tracks "Audio 1/2/...". Empty tracks have no
// type yet, so they show a generic "Track N". A renamed row shows its own name.
std::string App::trackLabel(int track) const {
    if (const std::string& custom = timeline_.trackName(track); !custom.empty())
        return custom;
    // Derived names are numbered within their kind, so the count has to walk the
    // rows above this one -- renamed rows included, which keeps the numbering of
    // the rest stable when one row is given a name.
    int vCount = 0, aCount = 0;
    for (int t = 0; t <= track; ++t) {
        switch (timeline_.trackKind(t)) {
        case Timeline::TrackKind::Video: ++vCount; break;
        case Timeline::TrackKind::Audio: ++aCount; break;
        default: break;
        }
    }
    char name[32];
    switch (timeline_.trackKind(track)) {
    case Timeline::TrackKind::Video: SDL_snprintf(name, sizeof(name), "Video %d", vCount); break;
    case Timeline::TrackKind::Audio: SDL_snprintf(name, sizeof(name), "Audio %d", aCount); break;
    default:                         SDL_snprintf(name, sizeof(name), "Track %d", track + 1); break;
    }
    return name;
}

// Where the rename field sits in a track header: centered in the row and stopping
// short of the burger, so the menu stays reachable while the field is up.
SDL_FRect App::trackNameFieldRect(int track) const {
    SDL_FRect hr = trackHeaderRect(track);
    float h = std::min(hr.h - 4.0f, 18.0f);
    return { hr.x + 3.0f, hr.y + (hr.h - h) * 0.5f, headerW() - 24.0f, h };
}

void App::beginTrackNameEdit(int track) {
    // Any armed reorder drag belongs to the first click of this double-click.
    trackDragFrom_ = -1;
    trackDragging_ = false;
    trackDropIns_ = -1;
    trackNameEdit_ = track;
    trackNameField_.setRect(trackNameFieldRect(track));
    trackNameField_.setText(trackLabel(track));
    trackNameField_.setFocus(true);
    SDL_StartTextInput(window_);
}

void App::commitTrackNameEdit() {
    int track = trackNameEdit_;
    cancelTrackNameEdit();
    if (track < 0)
        return;
    // Ctrl-V goes straight into the field, so the paste is filtered here rather
    // than only on the way in.
    std::string name = filterTrackName(trackNameField_.text().c_str());
    // Trim the ends: leading/trailing blanks would read as an empty label.
    size_t a = name.find_first_not_of(' ');
    size_t b = name.find_last_not_of(' ');
    name = (a == std::string::npos) ? std::string() : name.substr(a, b - a + 1);
    // Clear first, so trackLabel() below is the row's derived name: typing that
    // back (or emptying the field) means the row has no custom name at all.
    timeline_.setTrackName(track, std::string());
    if (!name.empty() && name != trackLabel(track))
        timeline_.setTrackName(track, name);
    hostSnapshotDirty_ = true; // re-push the project to spectators if hosting
    setStatus(timeline_.trackName(track).empty() ? "CLEARED TRACK NAME"
                                                : "RENAMED TRACK TO " + name);
}

void App::cancelTrackNameEdit() {
    if (trackNameEdit_ < 0)
        return;
    trackNameEdit_ = -1;
    trackNameField_.setFocus(false);
    SDL_StopTextInput(window_);
}

bool App::trackNameEditHandleEvent(const SDL_Event& e) {
    if (trackNameEdit_ < 0)
        return false;
    // The row can go away under the field (a track removed, or the spare row
    // dropped by ensureTrailingEmptyTrack); drop the edit rather than rename a
    // row that is no longer the one that was double-clicked.
    if (trackNameEdit_ >= std::max(trackCount(), 1)) {
        cancelTrackNameEdit();
        return false;
    }
    if (e.type == SDL_EVENT_KEY_DOWN) {
        if (e.key.key == SDLK_RETURN || e.key.key == SDLK_KP_ENTER) { commitTrackNameEdit(); return true; }
        if (e.key.key == SDLK_ESCAPE) { cancelTrackNameEdit(); return true; }
        trackNameField_.handleEvent(e); // backspace / arrows / home / end / ctrl-A
        return true;
    }
    if (e.type == SDL_EVENT_TEXT_INPUT) {
        std::string kept = filterTrackName(e.text.text);
        if (!kept.empty()) {
            SDL_Event filtered = e;
            filtered.text.text = kept.c_str();
            trackNameField_.handleEvent(filtered);
        }
        return true;
    }
    if (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN) {
        // Inside the field: selection/cursor placement. Outside: commit, and let
        // the click go on to do whatever it was aimed at.
        if (inRect(trackNameField_.rect(), e.button.x, e.button.y)) {
            trackNameField_.handleEvent(e);
            return true;
        }
        commitTrackNameEdit();
        return false;
    }
    if (e.type == SDL_EVENT_MOUSE_MOTION || e.type == SDL_EVENT_MOUSE_BUTTON_UP) {
        // Keep drag-selection inside the field alive; other motion is not ours.
        return trackNameField_.handleEvent(e);
    }
    return false;
}

SDL_FRect App::trackHeaderRect(int track) const {
    return SDL_FRect{ tlRect_.x, trackRowY(track), headerW(), trackRowH(track) };
}

// The header's burger: a small square against the right edge of the gutter,
// centred on the row's label rather than on the row itself. The label is placed
// against a nominal 8px band (see renderTimeline) and so sits a little below the
// row's middle; matching it keeps the two on one line whatever the row height.
static constexpr float kTrackBtnSz = 10.0f;

SDL_FRect App::trackMenuRect(int track) const {
    // A row collapsed by curve mode has no button. Answering with an empty rect
    // rather than leaving each caller to check keeps the draw, the hover and the
    // press agreeing: inRect is false on a zero-size box, so a hidden row cannot
    // hand out a phantom hit box near the top of the lane's gutter.
    if (trackRowH(track) <= 0.0f)
        return SDL_FRect{};
    // Centred on the row's label, except on the expanded curve lane: there the
    // label is pinned to the top of a tall row (see drawTrackHeader), and a
    // button floating in the middle of all that empty gutter reads as unrelated.
    const float rowH = track == curveTrack_ ? std::min(trackRowH(track), kTrackH)
                                            : trackRowH(track);
    const float labelY = trackRowY(track) + (rowH - 8.0f) * 0.5f;
    // Whole logical units: the bars are 1px hairlines, so a half-unit origin
    // would smear them across two device pixels (and the hit box should match
    // what is drawn).
    return SDL_FRect{ std::round(headerX_ - kTrackBtnSz - 4.0f),
                      std::round(labelY + (textFont_.lineHeight() - kTrackBtnSz) * 0.5f),
                      kTrackBtnSz, kTrackBtnSz };
}

int App::trackFromY(float y) const {
    int n = std::max(trackCount(), 1);
    // Rows start at trackRowY(0) rather than at tracksTop_: with the stack scrolled
    // the first row's top sits above the viewport.
    float top = trackRowY(0);
    if (y < top)
        return 0;
    for (int t = 0; t < n; ++t) {
        float h = trackRowH(t);
        if (y < top + h)
            return t;
        top += h;
    }
    return n - 1;
}

void App::drawDottedRect(const SDL_FRect& r, SDL_Color c) {
    SDL_SetRenderDrawColor(renderer_, c.r, c.g, c.b, c.a);
    const float dash = 5.0f, gap = 4.0f, step = dash + gap;
    for (float x = r.x; x < r.x + r.w; x += step) {
        float x2 = std::min(x + dash, r.x + r.w);
        jplay::drawLine(renderer_, x, r.y, x2, r.y);
        jplay::drawLine(renderer_, x, r.y + r.h, x2, r.y + r.h);
    }
    for (float y = r.y; y < r.y + r.h; y += step) {
        float y2 = std::min(y + dash, r.y + r.h);
        jplay::drawLine(renderer_, r.x, y, r.x, y2);
        jplay::drawLine(renderer_, r.x + r.w, y, r.x + r.w, y2);
    }
}

void App::drawTransportButton(const SDL_FRect& r, bool hovered) {
    SDL_SetRenderDrawColor(renderer_, hovered ? 60 : 44, hovered ? 63 : 46, hovered ? 72 : 52, 255);
    jplay::fillRect(renderer_, &r);
    setColor(renderer_, kTransportBorder);
    jplay::drawRect(renderer_, &r);
}

void App::fillTriangle(float ax, float ay, float bx, float by, float cx, float cy, SDL_Color c) {
    SDL_FColor fc{ c.r / 255.0f, c.g / 255.0f, c.b / 255.0f, c.a / 255.0f };
    SDL_Vertex v[3] = {
        { { ax, ay }, fc, { 0, 0 } },
        { { bx, by }, fc, { 0, 0 } },
        { { cx, cy }, fc, { 0, 0 } },
    };
    SDL_RenderGeometry(renderer_, nullptr, v, 3, nullptr, 0);
}

void App::ensureTrailingEmptyTrack() {
    // Invariant: exactly one empty trailing track, so there is always a free
    // row to drop onto and no more. Grow if the bottom row has clips; shrink
    // while the bottom two rows are both empty (keeping at least one track).
    //
    // On the stack that is actually on screen: while a scratch view is up that is
    // its rows, not the project's (see trackCount). Both the count and the
    // emptiness test follow the view — timeline_.trackCount is a saved field, so
    // growing it to cover a view's clips would write the view into the file and the
    // dirty signature.
    int& count = mutableTrackCount();
    while (!timeline_.trackEmpty(count - 1))
        count++;
    while (count > 1
           && timeline_.trackEmpty(count - 1)
           && timeline_.trackEmpty(count - 2))
        count--;
    if (scratchActive())
        return; // track names and off states belong to the project's stack
    // A name (or an off state) on a row the shrink dropped must not come back if
    // the stack regrows.
    if ((int)timeline_.trackNames.size() > timeline_.trackCount)
        timeline_.trackNames.resize((size_t)timeline_.trackCount);
    if ((int)timeline_.disabledTracks.size() > timeline_.trackCount)
        timeline_.disabledTracks.resize((size_t)timeline_.trackCount);
}

// The trailing spare row is not part of the reorderable stack: it must stay at
// the bottom (ensureTrailingEmptyTrack puts it there), so it neither moves nor
// takes a drop. Everything above it does, which leaves rows 0..n-2 draggable and
// insertion boundaries 0..n-1 (boundary i sits at the top of row i).
int App::trackReorderRows() const {
    return std::max(trackCount(), 1) - 1;
}

int App::trackDropBoundaryAt(float y) const {
    int rows = trackReorderRows();
    int best = 0;
    float bestD = std::fabs(y - trackRowY(0));
    for (int i = 1; i <= rows; ++i) {
        float d = std::fabs(y - trackRowY(i));
        if (d < bestD) { bestD = d; best = i; }
    }
    return best;
}

void App::reorderTrack(int from, int ins) {
    // `ins` is a boundary; the row index it lands on shifts down by one once
    // `from` has been lifted out of the stack above it.
    int to = ins > from ? ins - 1 : ins;
    if (to == from)
        return;
    ContentSnapshot before = captureContent();
    // A track is just its clips' row number, so the move is the permutation that
    // takes `from` to `to` and slides everything in between the other way.
    for (auto& s : timeline_.sequences)
        for (auto& c : s.clips) {
            if (c.track == from)                                 c.track = to;
            else if (from < to && c.track > from && c.track <= to) c.track--;
            else if (from > to && c.track >= to && c.track < from) c.track++;
        }
    // Names and off states travel with their row: lift the moved one out and
    // re-insert it at the destination, which slides the rows in between exactly as
    // the clips above did.
    if (!timeline_.trackNames.empty()) {
        auto& names = timeline_.trackNames;
        int need = std::max(from, to) + 1;
        if ((int)names.size() < need)
            names.resize((size_t)need);
        std::string moved = std::move(names[(size_t)from]);
        names.erase(names.begin() + from);
        names.insert(names.begin() + to, std::move(moved));
    }
    if (!timeline_.disabledTracks.empty()) {
        auto& off = timeline_.disabledTracks;
        int need = std::max(from, to) + 1;
        if ((int)off.size() < need)
            off.resize((size_t)need, 0);
        uint8_t moved = off[(size_t)from];
        off.erase(off.begin() + from);
        off.insert(off.begin() + to, moved);
    }
    // A gap is remembered by row, and the rows just moved, so a kept selection
    // would put Delete on the wrong track.
    hoverGapTrack_ = -1;
    selectedGapTrack_ = -1;
    // The move can leave the row above the spare empty too; the invariant then
    // drops one row, which is the same tidy-up any other edit gets.
    ensureTrailingEmptyTrack();
    pushContentUndo("MOVE TRACK", before);
    setStatus("MOVED TRACK " + std::to_string(from + 1) + " TO " + std::to_string(to + 1));
}

// The burger's popup: the row's enable/disable toggle and its remove. Grows
// upward from the button, as the timeline's other popups do.
void App::openTrackMenu(int track) {
    // The rows under a scratch view are that view's, not the project's (see
    // trackCount): both entries here would act on the project's stack at the same
    // index, which is a different row entirely — and removing one of those from
    // inside a view that isn't part of the project is an edit nobody asked for.
    if (scratchActive()) {
        setStatusWarn("TRACKS BELONG TO THE TIMELINE: F2 OR BACKSPACE TO RETURN TO IT", 3000);
        return;
    }
    std::vector<ContextMenu::Item> items;
    ContextMenu::Item toggle;
    toggle.label = timeline_.trackDisabled(track) ? "Enable Track" : "Disable Track";
    toggle.action = [this, track] { toggleTrackDisabled(track); };
    items.push_back(std::move(toggle));
    ContextMenu::Item remove;
    remove.label = "Remove Track";
    remove.action = [this, track] { requestRemoveTrack(track); };
    items.push_back(std::move(remove));
    // Row-wide clip actions. Both are pointless on an empty row, and the mute only
    // has something to act on where the row actually sounds, so each earns its way
    // in — an entry that can only no-op is worse than no entry.
    const std::vector<int> audioIds = trackAudioClipIds(track);
    if (!timeline_.trackEmpty(track) || !audioIds.empty()) {
        ContextMenu::Item sep;
        sep.separator = true;
        items.push_back(std::move(sep));
    }
    if (!timeline_.trackEmpty(track)) {
        ContextMenu::Item selectAll;
        selectAll.label = "Select All Clips";
        selectAll.action = [this, track] { selectTrackClips(track); };
        items.push_back(std::move(selectAll));
    }
    if (!audioIds.empty()) {
        // Label says what the entry does next, as the enable/disable toggle above
        // does: muted only once every clip it covers is silent.
        const bool allMuted = std::all_of(audioIds.begin(), audioIds.end(), [this](int id) {
            const Clip* clip = timeline_.findClipById(id);
            return clip && clip->hidden;
        });
        ContextMenu::Item mute;
        mute.label = allMuted ? "Unmute Audio" : "Mute Audio";
        mute.action = [this, track] { toggleTrackAudioMuted(track); };
        items.push_back(std::move(mute));
    }
    // Parameter curves for the whole row: the lane draws every clip on it, so the
    // row's burger is the way in. Audio only, since volume is the only parameter
    // bound to the lane so far.
    if (timeline_.trackKind(track) == Timeline::TrackKind::Audio && !curveMode()) {
        ContextMenu::Item sep;
        sep.separator = true;
        items.push_back(std::move(sep));
        ContextMenu::Item curve;
        curve.label = "Curves";
        curve.action = [this, track] { enterCurveMode(track, CurveParam::Volume); };
        items.push_back(std::move(curve));
    }
    const SDL_FRect rect = trackMenuRect(track);
    trackMenu_.open(rect.x, rect.y, winW_, winH_, std::move(items));
}

// Switch a whole row off (its clips stop compositing and stop feeding audio, and
// the row draws faint) or back on. Per-clip hidden flags are left alone, so
// re-enabling the row restores exactly what it showed before.
void App::toggleTrackDisabled(int track) {
    bool off = !timeline_.trackDisabled(track);
    timeline_.setTrackDisabled(track, off);
    hostSnapshotDirty_ = true; // re-push the project to spectators if hosting
    setStatus((off ? "DISABLED TRACK " : "ENABLED TRACK ") + std::to_string(track + 1));
}

// The row's whole content as a selection, so a move/copy/delete can act on it.
// Linked audio comes along the way it does for a click (see selectClipSingle), so
// dragging a picture row's worth of clips keeps their sound attached.
//
// The row as the *view* draws it, not as the project stores it: under a scope the
// rows on screen hold one sequence's clips (or one project's), and the clips the
// other sequences keep at the same track index are not on screen at all. Taking
// those in would put clips the user cannot see in the selection, and every action
// that reads it — a source switch from the picker menu most visibly — would then
// reach into sequences nobody selected anything in.
void App::selectTrackClips(int track) {
    std::vector<int> ids;
    forEachViewClip([&](const Clip& c) {
        if (c.track == track)
            ids.push_back(c.id);
    });
    if (ids.empty()) {
        clearClipSelection();
        return;
    }
    addLinkedFollowers(ids);
    selectedClipIds_ = std::move(ids);
    selectedClipId_ = selectedClipIds_.front(); // primary: the row's first clip
    selectedTransitionId_ = -1; // clip and dissolve selection are mutually exclusive
    binSelectionActive_ = false; // Delete goes back to the timeline
    setStatus("SELECTED " + std::to_string(selectedClipIds_.size()) +
              " CLIPS ON TRACK " + std::to_string(track + 1));
}

// The audio a row sounds: an audio row's clips are it, a video row's is whatever
// is linked to them. The link is asymmetric, so this is the only direction that
// needs walking — an audio clip never has picture following it.
std::vector<int> App::trackAudioClipIds(int track) const {
    std::vector<int> ids;
    timeline_.forEachTrackClip([&](const Clip& c) {
        if (c.track != track)
            return;
        if (c.audio) {
            ids.push_back(c.id);
            return;
        }
        for (int f : timeline_.linkFollowers(c.id))
            if (const Clip* clip = timeline_.findClipById(f))
                if (clip->audio)
                    ids.push_back(clip->id);
    });
    return ids;
}

// Mute by hiding the audio clips themselves (updateAudio feeds only non-hidden
// ones) rather than by switching the row off: a video row keeps compositing, and
// the audio's own row keeps showing where its clips are. A video clip's *embedded*
// audio is a separate feed keyed to the program clip, so this doesn't reach it —
// silencing that would take per-track mute state the timeline doesn't have. One
// audible clip left means the row still sounds, so the toggle mutes; only when
// every one is silent does it unmute them all.
void App::toggleTrackAudioMuted(int track) {
    const std::vector<int> ids = trackAudioClipIds(track);
    if (ids.empty())
        return;
    const bool mute = std::any_of(ids.begin(), ids.end(), [this](int id) {
        const Clip* clip = timeline_.findClipById(id);
        return clip && !clip->hidden;
    });
    for (int id : ids)
        if (Clip* c = clipById(id))
            c->hidden = mute;
    hostSnapshotDirty_ = true; // re-push the project to spectators if hosting
    setStatus((mute ? "MUTED AUDIO ON TRACK " : "UNMUTED AUDIO ON TRACK ") +
              std::to_string(track + 1));
}

void App::requestRemoveTrack(int track) {
    if (timeline_.trackCount <= 1) {
        setStatus("AT LEAST ONE TRACK REQUIRED", 4000);
        return;
    }
    if (timeline_.trackEmpty(track)) {
        removeTrack(track);
        return;
    }
    // Non-empty: confirm with a modal warning. Enter picks Abort, as Esc does —
    // a reflex Return must not delete a track's worth of clips.
    showDialog("Remove Track", "Track is not Empty",
               { { "Abort", nullptr },
                 { "Delete", [this, track] { removeTrack(track); } } },
               /*escIdx*/ 0, /*enterIdx*/ 0);
}

void App::removeTrack(int track) {
    if (draggingClip_) { draggingClip_ = false; dragClipId_ = -1; }
    if (trimmingClip_) { trimmingClip_ = false; trimClipId_ = -1; }
    for (auto& s : timeline_.sequences) {
        s.clips.erase(std::remove_if(s.clips.begin(), s.clips.end(),
                                     [track](const Clip& c) { return c.track == track; }),
                      s.clips.end());
        for (auto& c : s.clips)
            if (c.track > track)
                c.track--;
    }
    if (track < (int)timeline_.trackNames.size())
        timeline_.trackNames.erase(timeline_.trackNames.begin() + track);
    if (track < (int)timeline_.disabledTracks.size())
        timeline_.disabledTracks.erase(timeline_.disabledTracks.begin() + track);
    timeline_.trackCount = std::max(timeline_.trackCount - 1, 1);
    timeline_.repackSequences();
    timeline_.clampPlayhead();
    pruneTransitions(); // the removed track took its clips, and their dissolves
    // Removing a track deletes clips and renumbers the rows below it, which would
    // make recorded move/delete positions point at the wrong place. Drop history.
    undoStack_.clear();
    setStatus("REMOVED TRACK " + std::to_string(track + 1));
}

// ---------------------------------------------------------------- clip drag

Clip* App::clipById(int id) {
    return timeline_.findClipById(id);
}

Clip* App::clipAtTrack(int track, int64_t frame) {
    for (auto& s : timeline_.sequences)
        for (auto& c : s.clips)
            if (c.track == track &&
                frame >= c.timelineStart && frame < c.end())
                return &c;
    return nullptr;
}

// Only meaningful for empty space (callers check clipAtTrack first): the gap is
// the span between the nearest clip ending at/before `frame` and the nearest one
// starting after it. Needs a clip on both sides, else it's leading/trailing space.
bool App::gapAtTrack(int track, int64_t frame, int64_t& gapStart, int64_t& gapEnd) const {
    int64_t prevEnd = 0, nextStart = 0;
    bool havePrev = false, haveNext = false;
    timeline_.forEachClip([&](const Clip& c) {
        if (c.track != track) return;
        if (c.end() <= frame && (!havePrev || c.end() > prevEnd)) { prevEnd = c.end(); havePrev = true; }
        if (c.timelineStart > frame && (!haveNext || c.timelineStart < nextStart)) { nextStart = c.timelineStart; haveNext = true; }
    });
    if (!havePrev || !haveNext)
        return false;
    gapStart = prevEnd;
    gapEnd = nextStart;
    return true;
}

int64_t App::snapDragStart(double desiredStart, int64_t duration, int excludeId) const {
    double startF = desiredStart;
    double endF = desiredStart + (double)duration;
    double threshold = 8.0 * framesPerPx_; // 8 px snap radius
    double best = threshold;
    double snapped = desiredStart;
    auto consider = [&](double target) {
        if (std::fabs(startF - target) < best) { best = std::fabs(startF - target); snapped = target; }
        if (std::fabs(endF - target) < best) { best = std::fabs(endF - target); snapped = target - (double)duration; }
    };
    consider((double)timeline_.playhead);
    timeline_.forEachClip([&](const Clip& c) {
        if (c.id == excludeId) return;
        consider((double)c.timelineStart);
        consider((double)c.end());
    });
    int64_t s = (int64_t)std::llround(snapped);
    return std::max<int64_t>(s, 0);
}

bool App::rippleModifierHeld() {
    return (SDL_GetModState() & SDL_KMOD_CTRL) != 0;
}

void App::beginClipDrag(Clip& c, float mouseX) {
    draggingClip_ = true;
    dragDropMode_ = rippleModifierHeld() ? DropMode::Ripple : DropMode::Overwrite;
    dragClipId_ = c.id;
    playing_ = false;
    dragGrabOffset_ = xToFrame(mouseX) - (double)c.timelineStart;
    dragTargetTrack_ = c.track;
    dragTargetStart_ = c.timelineStart;
    dragOverlaps_ = false;
    // Capture the originals of the whole selection so the group moves as one.
    // The grabbed clip (c) is always a member; guard in case the set is empty.
    dragOrig_.clear();
    for (int id : selectedClipIds_)
        if (Clip* clip = clipById(id))
            dragOrig_.push_back({ clip->id, clip->track, clip->audio, clip->timelineStart, clip->duration });
    if (dragOrig_.empty())
        dragOrig_.push_back({ c.id, c.track, c.audio, c.timelineStart, c.duration });
}

void App::updateClipDrag(float mouseX, float mouseY) {
    if (dragOrig_.empty())
        return;
    // Primary clip's captured original anchors the delta.
    const DragClipOrig* prim = nullptr;
    for (const auto& o : dragOrig_)
        if (o.id == dragClipId_) prim = &o;
    if (!prim)
        return;

    // The cursor's target row in the single track stack.
    int track = trackFromY(mouseY);
    double desired = xToFrame(mouseX) - dragGrabOffset_;
    int64_t start = snapDragStart(desired, prim->duration, prim->id);

    // Raw delta from the primary, then clamp it so every dragged clip stays on a
    // valid track and no earlier than frame 0.
    int dTrack = track - prim->track;
    int64_t dFrame = start - prim->start;
    const int n  = std::max(trackCount(), 1);
    int64_t minS = dragOrig_.front().start;
    int lo = INT_MIN, hi = INT_MAX; // allowed dTrack range
    for (const auto& o : dragOrig_) {
        lo = std::max(lo, -o.track);
        hi = std::min(hi, (n - 1) - o.track);
        minS = std::min(minS, o.start);
    }
    dTrack = std::clamp(dTrack, lo, hi);
    if (minS + dFrame < 0)
        dFrame = -minS;
    dragTargetTrack_ = prim->track + dTrack;
    dragTargetStart_ = prim->start + dFrame;

    // Ripple drop: a dragged clip must never land *inside* an existing clip. If a
    // clip's landing straddles a non-dragged neighbour, jump the whole group
    // forward so the dropped clip starts at that neighbour's end; the clips to the
    // right then ripple forward on release. (Overwrite mode carves the neighbour
    // under the drop instead, so it is left alone here.)
    if (dragDropMode_ == DropMode::Ripple) {
        std::vector<int> dragged;
        for (const auto& o : dragOrig_) dragged.push_back(o.id);
        int64_t fwd = 0;
        for (const auto& o : dragOrig_) {
            int nt = o.track + dTrack;
            int64_t ns = o.start + dFrame;
            timeline_.forEachClip([&](const Clip& c) {
                if (c.track != nt) return;
                if (std::find(dragged.begin(), dragged.end(), c.id) != dragged.end()) return;
                if (c.timelineStart < ns && ns < c.end())
                    fwd = std::max(fwd, c.end() - ns);
            });
        }
        if (fwd > 0) {
            dFrame += fwd;
            dragTargetStart_ = prim->start + dFrame;
        }
    }

    // Overlap + cross-sequence + track-type checks span the whole group; dragged
    // clips are excluded so they never flag one another.
    std::vector<int> sel;
    for (const auto& o : dragOrig_) sel.push_back(o.id);
    auto overlapExcept = [&](int trk, int64_t s, int64_t dur) {
        int64_t e = s + dur;
        bool found = false;
        timeline_.forEachClip([&](const Clip& c) {
            if (found || c.track != trk) return;
            if (std::find(sel.begin(), sel.end(), c.id) != sel.end()) return;
            if (s < c.end() && e > c.timelineStart) found = true;
        });
        return found;
    };
    // The (non-dragged) kind resident on a track, or Empty if none.
    auto residentKind = [&](int trk) {
        Timeline::TrackKind k = Timeline::TrackKind::Empty;
        timeline_.forEachClip([&](const Clip& c) {
            if (c.track != trk) return;
            if (std::find(sel.begin(), sel.end(), c.id) != sel.end()) return;
            k = c.audio ? Timeline::TrackKind::Audio : Timeline::TrackKind::Video;
        });
        return k;
    };
    dragOverlaps_ = false;
    dragRejected_ = false;
    dragRejectSeq_ = -1;
    dragBadType_ = false;
    const bool multiSeq = timeline_.sequences.size() > 1;
    std::map<int, bool> dropKind; // target track -> kind (audio) it receives from the drag
    for (const auto& o : dragOrig_) {
        int nt = o.track + dTrack;
        int64_t ns = o.start + dFrame;
        if (overlapExcept(nt, ns, o.duration))
            dragOverlaps_ = true;
        // Track type: a clip may only land on an empty track or one already of its
        // own kind. Two dragged clips of different kinds can't share a target row.
        Timeline::TrackKind want = o.audio ? Timeline::TrackKind::Audio : Timeline::TrackKind::Video;
        Timeline::TrackKind res = residentKind(nt);
        if (res != Timeline::TrackKind::Empty && res != want)
            dragBadType_ = true;
        auto it = dropKind.find(nt);
        if (it == dropKind.end()) dropKind[nt] = o.audio;
        else if (it->second != o.audio) dragBadType_ = true;
        // Cross-sequence guard: a clip may only be rearranged within its own
        // sequence. If any clip lands in a foreign region, reject the whole move.
        if (multiSeq) {
            int ownSeq = timeline_.seqIndexOfClip(o.id);
            int tgtSeq = timeline_.seqIndexAtFrame(ns);
            if (tgtSeq >= 0 && tgtSeq != ownSeq) {
                dragRejected_ = true;
                dragRejectSeq_ = tgtSeq;
            }
        }
    }
}

void App::commitClipDrag(bool duplicate) {
    if (draggingClip_ && (dragRejected_ || dragBadType_)) {
        // Drop targeted another sequence, or a track of the wrong type: reject and
        // leave the clips put.
        setStatus(dragBadType_ ? "CANNOT MIX VIDEO AND AUDIO ON ONE TRACK"
                               : "CANNOT MOVE A CLIP BETWEEN SEQUENCES", 3000);
        draggingClip_ = false;
        dragClipId_ = -1;
        dragRejected_ = false;
        dragRejectSeq_ = -1;
        dragBadType_ = false;
        dragOrig_.clear();
        return;
    }
    if (draggingClip_ && !dragOrig_.empty()) {
        // Delta from the primary's captured original to its clamped target.
        const DragClipOrig* prim = nullptr;
        for (const auto& o : dragOrig_)
            if (o.id == dragClipId_) prim = &o;
        const int dTrack = prim ? dragTargetTrack_ - prim->track : 0;
        const int64_t dFrame = prim ? dragTargetStart_ - prim->start : 0;
        const int n = std::max(trackCount(), 1);

        if (duplicate || dTrack != 0 || dFrame != 0) {
            // Snapshot the whole clip/shot state: the overwrite drop can trim a
            // neighbour's duration and remove covered clips, neither of which a
            // position-only snapshot can restore. Undo/redo reverse as one step.
            auto beforeSeqs = timeline_.sequences;
            auto beforeShots = timeline_.shots;

            std::vector<int> selBefore; // originals (valid after undo)
            for (const auto& o : dragOrig_) selBefore.push_back(o.id);
            std::vector<int> selAfter;  // moved clips / new copies (valid after redo)

            if (duplicate) {
                // Leave the originals put; drop a copy of each at its delta. All
                // copies are excluded from the overwrite so they don't cut each
                // other, only the non-dragged clips they land on.
                std::map<int, int> copyOf; // original clip id -> its copy's id
                for (const auto& o : dragOrig_) {
                    Clip* src = clipById(o.id);
                    if (!src) continue;
                    Clip copy = *src;
                    copy.id = nextClipId_++;
                    copy.track = std::clamp(o.track + dTrack, 0, n - 1);
                    copy.timelineStart = o.start + dFrame;
                    copy.shotId = -1;
                    copyOf[o.id] = copy.id;
                    if (Sequence* seq = timeline_.sequenceOfClipMut(o.id))
                        seq->clips.push_back(copy);
                    else
                        activeSequence().clips.push_back(copy);
                    selAfter.push_back(copy.id);
                }
                // Re-point the copies' links at each other. A copied audio clip
                // whose picture clip was not part of the drag is left unlinked
                // rather than quietly following the original's video.
                for (int id : selAfter)
                    if (Clip* c = clipById(id); c && c->linkedTo != 0) {
                        auto it = copyOf.find(c->linkedTo);
                        c->linkedTo = (it != copyOf.end()) ? it->second : 0;
                        if (c->linkedTo == 0)
                            c->linkOffset = 0;
                    }
                for (int id : selAfter)
                    if (Clip* c = clipById(id))
                        applyDrop(c->track, c->timelineStart, c->duration, selAfter);
            } else {
                // Move each clip by the delta, then overwrite non-dragged clips
                // around each new span (all dragged clips excluded).
                for (const auto& o : dragOrig_) {
                    if (Clip* clip = clipById(o.id)) {
                        clip->track = std::clamp(o.track + dTrack, 0, n - 1);
                        clip->timelineStart = o.start + dFrame;
                    }
                    selAfter.push_back(o.id);
                }
                for (int id : selAfter)
                    if (Clip* c = clipById(id))
                        applyDrop(c->track, c->timelineStart, c->duration, selAfter);
            }

            // A follower dragged without its parent has been slipped deliberately:
            // record the new relationship. Everything else keeps its offset, so the
            // resync below only has to undo what the drop's ripple displaced.
            for (int id : selAfter)
                if (Clip* k = clipById(id))
                    if (const Clip* p = timeline_.linkParent(*k);
                        p && std::find(selAfter.begin(), selAfter.end(), p->id) == selAfter.end())
                        k->linkOffset = k->timelineStart - p->timelineStart;
            resyncLinkedClips();
            syncShotsToClips();
            timeline_.repackSequences();
            timeline_.clampPlayhead();
            // The drop can trim or remove the clips a dissolve is anchored to, and
            // moving a clip away from its neighbour dissolves nothing any more.
            pruneTransitions();
            selectedClipIds_ = selAfter;
            selectedClipId_ = selAfter.empty() ? -1 : selAfter.back();
            auto afterSeqs = timeline_.sequences;
            auto afterShots = timeline_.shots;
            const bool multi = dragOrig_.size() > 1;
            std::string desc = duplicate ? (multi ? "DUPLICATE CLIPS" : "DUPLICATE CLIP")
                                         : (multi ? "MOVE CLIPS" : "MOVE CLIP");
            undoStack_.push({
                desc,
                [this, beforeSeqs, beforeShots, selBefore] {
                    timeline_.sequences = beforeSeqs; timeline_.shots = beforeShots;
                    timeline_.clampPlayhead();
                    selectedClipIds_ = selBefore;
                    selectedClipId_ = selBefore.empty() ? -1 : selBefore.back();
                },
                [this, afterSeqs, afterShots, selAfter] {
                    timeline_.sequences = afterSeqs; timeline_.shots = afterShots;
                    timeline_.clampPlayhead();
                    selectedClipIds_ = selAfter;
                    selectedClipId_ = selAfter.empty() ? -1 : selAfter.back();
                },
            });
        }
        if (panelOpen(kPanelProjectExplorer))
            refreshExplorerOrder(); // the drag moved the clip's shot: re-sort the tree
    }
    draggingClip_ = false;
    dragClipId_ = -1;
    dragRejected_ = false;
    dragRejectSeq_ = -1;
    dragOrig_.clear();
}

// ---------------------------------------------------------------- clip trim

Clip* App::clipEdgeAt(int track, float x, int& edge) {
    Clip* best = nullptr;
    float bestDist = kTrimHandleW;
    auto consider = [&](Clip& c) {
        if (c.track != track) return;
        float xL = (float)frameToX((double)c.timelineStart);
        float xR = (float)frameToX((double)c.end());
        // The handle zones sit just *inside* the clip ([xL, xL+W] on the left,
        // [xR-W, xR] on the right), never spilling into a neighbour. So when two
        // clips touch, the cursor's side of the shared edge picks the right clip.
        if (x < xL || x > xR) return;
        float dL = x - xL; // inward distance from the left edge
        float dR = xR - x; // inward distance from the right edge
        if (dL <= bestDist) { bestDist = dL; best = &c; edge = 0; }
        if (dR <= bestDist) { bestDist = dR; best = &c; edge = 1; }
    };
    for (auto& s : timeline_.sequences)
        for (auto& c : s.clips)
            consider(c);
    return best;
}

void App::beginClipTrim(Clip& c, int edge) {
    trimmingClip_ = true;
    trimClipId_ = c.id;
    trimEdge_ = edge;
    trimNewStart_ = c.timelineStart;
    trimNewDuration_ = c.duration;
    trimNewSourceOffset_ = c.sourceOffset;
    selectClipSingle(c.id);
    selectedGapTrack_ = -1;
    playing_ = false;
}

int64_t App::snapTrimFrame(double x, int track) const {
    int64_t f = (int64_t)std::llround(xToFrame(x));
    if (!snapPlayhead_)
        return f;
    // Fixed screen-pixel radius, so the magnet feels the same at any zoom (as in
    // scrubFrame). Only the row directly above is considered: an audio clip sits
    // under the video it belongs to, so its cuts are the points a trim wants to
    // land on — the pair gets back in sync after a slip. The dragged clip's own
    // row is excluded, so an edge never snaps to itself or its neighbours.
    const double kSnapPx = 8.0;
    int64_t best = f;
    double bestPx = kSnapPx;
    auto consider = [&](int64_t frame) {
        double px = std::fabs(frameToX((double)frame) - x);
        if (px < bestPx) { bestPx = px; best = frame; }
    };
    consider(timeline_.playhead);
    forEachViewClip([&](const Clip& c) {
        if (c.track != track - 1) return;
        consider(c.timelineStart);
        consider(c.end());
    });
    return best;
}

void App::updateClipTrim(float mouseX) {
    Clip* clip = clipById(trimClipId_);
    if (!clip)
        return;
    int64_t srcFrames = timeline_.clipSourceFrames(*clip);
    int64_t f = snapTrimFrame(mouseX, clip->track);
    if (trimEdge_ == 1) {
        // Right edge: start + sourceOffset fixed, duration follows the cursor.
        // Capped at 1 frame and at the frames remaining in the source.
        int64_t maxDur = std::max<int64_t>(srcFrames - clip->sourceOffset, 1);
        int64_t dur = std::clamp<int64_t>(f - clip->timelineStart, 1, maxDur);
        trimNewStart_ = clip->timelineStart;
        trimNewSourceOffset_ = clip->sourceOffset;
        trimNewDuration_ = dur;
    } else {
        // Left edge: right edge (end) fixed; start and sourceOffset move together.
        // Can't extend past source frame 0 (sourceOffset >= 0) or below timeline 0,
        // and can't cross the right edge (duration >= 1).
        int64_t fixedEnd = clip->end();
        int64_t minStart = std::max<int64_t>(clip->timelineStart - clip->sourceOffset, 0);
        int64_t maxStart = fixedEnd - 1;
        int64_t start = std::clamp<int64_t>(f, minStart, maxStart);
        trimNewStart_ = start;
        trimNewSourceOffset_ = clip->sourceOffset + (start - clip->timelineStart);
        trimNewDuration_ = fixedEnd - start;
    }
}

void App::commitClipTrim() {
    if (trimmingClip_) {
        if (Clip* clip = clipById(trimClipId_)) {
            const int64_t oldStart = clip->timelineStart, oldDur = clip->duration, oldSrc = clip->sourceOffset;
            const int64_t newStart = trimNewStart_, newDur = trimNewDuration_, newSrc = trimNewSourceOffset_;
            if (oldStart != newStart || oldDur != newDur || oldSrc != newSrc) {
                const int id = clip->id;
                const int track = clip->track;
                // Snapshot all positions (a right-edge trim can grow the sequence
                // and shift later sequences via repack), so undo/redo restore the
                // whole layout plus the trimmed clip's duration/source range. The
                // linked audio's range rides along in the geometry snapshot.
                std::vector<int> geomIds{ id };
                addLinkedFollowers(geomIds);
                PositionSnapshot before = capturePositions();
                std::vector<ClipGeom> geomBefore = captureClipGeom(geomIds);
                clip->timelineStart = newStart;
                clip->duration = newDur;
                clip->sourceOffset = newSrc;
                trimLinkedFollowers(id, newSrc - oldSrc, newDur - oldDur);
                // Trimming a follower's own head slips it against its parent: record
                // the new relationship instead of letting the link go quietly wrong.
                if (const Clip* p = timeline_.linkParent(*clip))
                    clip->linkOffset = clip->timelineStart - p->timelineStart;
                // Confine the ripple to this clip's own sequence: a left-edge
                // extend must never push clips in the sequence in front (repack
                // re-abuts the sequences afterwards regardless).
                if (Sequence* seq = timeline_.sequenceOfClipMut(id))
                    rippleMakeRoomInSeq(*seq, track, newStart, newDur, id);
                resyncLinkedClips(); // the ripple moved one track: re-abut the pairs
                timeline_.repackSequences();
                timeline_.clampPlayhead();
                // A trim moves an edit point and can eat the handle a dissolve on
                // it was spending; settle transitions before snapshotting `after`.
                pruneTransitions();
                PositionSnapshot after = capturePositions();
                std::vector<ClipGeom> geomAfter = captureClipGeom(geomIds);
                undoStack_.push({
                    "TRIM CLIP",
                    [this, id, before, geomBefore] {
                        restorePositions(before);
                        restoreClipGeom(geomBefore);
                        selectClipSingle(id);
                    },
                    [this, id, after, geomAfter] {
                        restorePositions(after);
                        restoreClipGeom(geomAfter);
                        selectClipSingle(id);
                    },
                });
            }
        }
    }
    trimmingClip_ = false;
    trimClipId_ = -1;
}

// ------------------------------------------------------------- transitions

Transition* App::transitionAt(int track, int64_t frame) {
    for (auto& s : timeline_.sequences)
        for (auto& t : s.transitions) {
            Timeline::TransitionSpan sp;
            if (!timeline_.resolveTransition(s, t, sp))
                continue;
            if (sp.a->track == track && frame >= sp.start && frame < sp.end)
                return &t;
        }
    return nullptr;
}

void App::pruneTransitions() {
    for (auto& s : timeline_.sequences) {
        for (auto& t : s.transitions) {
            Timeline::TransitionSpan sp;
            if (!timeline_.resolveTransition(s, t, sp)) {
                t.inFrames = t.outFrames = 0; // stale: marked for the erase below
                continue;
            }
            // A trim can eat into a handle the dissolve was spending, or shorten a
            // clip the span lies inside. Give back only what is no longer there.
            int64_t maxIn = 0, maxOut = 0;
            timeline_.transitionLimits(*sp.a, *sp.b, maxIn, maxOut);
            t.inFrames  = std::min(t.inFrames, maxIn);
            t.outFrames = std::min(t.outFrames, maxOut);
        }
        s.transitions.erase(std::remove_if(s.transitions.begin(), s.transitions.end(),
                                           [](const Transition& t) { return t.duration() <= 0; }),
                            s.transitions.end());
    }
    if (selectedTransitionId_ >= 0) {
        bool alive = false;
        for (const auto& s : timeline_.sequences)
            for (const auto& t : s.transitions)
                if (t.id == selectedTransitionId_) alive = true;
        if (!alive)
            selectedTransitionId_ = -1;
    }
}

void App::adoptLoadedTransitions() {
    // The incoming timeline's ids have nothing to do with the outgoing one's, so a
    // held selection could land on an unrelated dissolve that happens to reuse the
    // number. pruneTransitions only clears a selection whose id is *gone*.
    selectedTransitionId_ = -1;
    nextTransitionId_ = 1;
    for (const auto& s : timeline_.sequences)
        for (const auto& t : s.transitions)
            nextTransitionId_ = std::max(nextTransitionId_, t.id + 1);
    pruneTransitions();
}

void App::addDissolveAtClipOut(int aClipId) {
    Sequence* seq = timeline_.sequenceOfClipMut(aClipId);
    if (!seq)
        return;
    Clip* clip = nullptr;
    for (auto& c : seq->clips)
        if (c.id == aClipId) clip = &c;
    if (!clip || clip->audio) {
        setStatus("NO DISSOLVE: VIDEO CLIPS ONLY", 3000);
        return;
    }
    // The incoming clip is whichever clip on the same track starts exactly where
    // this one ends. Anything else — a gap, an overlap-free end of track — has no
    // cut to put a dissolve on.
    Clip* b = nullptr;
    for (auto& c : seq->clips)
        if (c.id != clip->id && c.track == clip->track && !c.audio && c.timelineStart == clip->end())
            b = &c;
    if (!b) {
        setStatus("NO DISSOLVE: NO ADJACENT CLIP AT THE OUT POINT", 4000);
        return;
    }
    for (const auto& t : seq->transitions)
        if (t.aClipId == clip->id && t.bClipId == b->id) {
            setStatus("DISSOLVE ALREADY ON THIS CUT", 3000);
            return;
        }
    // The default: one second, split evenly across the cut, then clamped to
    // whatever handle each side actually has.
    const int64_t want = std::max<int64_t>((int64_t)std::llround(timeline_.fps > 0.0 ? timeline_.fps : 24.0), 2);
    int64_t maxIn = 0, maxOut = 0;
    timeline_.transitionLimits(*clip, *b, maxIn, maxOut);
    Transition transition;
    transition.inFrames  = std::min(want / 2, maxIn);
    transition.outFrames = std::min(want - want / 2, maxOut);
    if (transition.duration() <= 0) {
        // Both clips are cut to their full source extent, so there is no material
        // to dissolve through. Handle-based transitions cannot invent it.
        setStatus("NO DISSOLVE: NO HANDLES ON EITHER SIDE OF THE CUT", 5000);
        return;
    }
    ContentSnapshot before = captureContent();
    transition.id = nextTransitionId_++;
    transition.aClipId = clip->id;
    transition.bClipId = b->id;
    seq->transitions.push_back(transition);
    clearClipSelection();          // clears selectedTransitionId_, so claim it after
    selectedGapTrack_ = -1;
    selectedTransitionId_ = transition.id;
    pushContentUndo("ADD DISSOLVE", before);
    setStatus("DISSOLVE " + std::to_string(transition.duration()) + "F", 2000);
}

void App::addDissolveAtPlayhead() {
    // The cut nearest the playhead on the playhead's own track: the clip under it
    // supplies the track, and whichever of its two edges is closer supplies the
    // outgoing clip (its own out point, or the previous clip's).
    const Clip* here = getTopMostClipAtFrame(timeline_.playhead);
    if (!here) {
        setStatus("NO DISSOLVE: NO CLIP UNDER THE PLAYHEAD", 3000);
        return;
    }
    const int64_t toStart = timeline_.playhead - here->timelineStart;
    const int64_t toEnd   = here->end() - timeline_.playhead;
    if (toStart < toEnd) {
        // Nearer this clip's in point: the cut belongs to the clip in front of it.
        const Sequence* seq = timeline_.sequenceOfClip(here->id);
        if (seq)
            for (const auto& c : seq->clips)
                if (c.track == here->track && !c.audio && c.end() == here->timelineStart) {
                    addDissolveAtClipOut(c.id);
                    return;
                }
        setStatus("NO DISSOLVE: NO ADJACENT CLIP AT THE CUT", 4000);
        return;
    }
    addDissolveAtClipOut(here->id);
}

void App::deleteSelectedTransition() {
    if (selectedTransitionId_ < 0)
        return;
    ContentSnapshot before = captureContent();
    const int id = selectedTransitionId_;
    for (auto& s : timeline_.sequences)
        s.transitions.erase(std::remove_if(s.transitions.begin(), s.transitions.end(),
                                           [id](const Transition& t) { return t.id == id; }),
                            s.transitions.end());
    selectedTransitionId_ = -1;
    pushContentUndo("DELETE DISSOLVE", before);
    setStatus("DISSOLVE REMOVED", 2000);
}

// ----------------------------------------------------------------- razor

void App::setTimelineTool(TimelineTool t) {
    if (timelineTool_ == t)
        return;
    timelineTool_ = t;
    razorClipId_ = -1; // stale until the next motion resolves a cut under the cursor
    setStatus(t == TimelineTool::Razor ? "RAZOR TOOL" : "CURSOR TOOL", 2000);
}

bool App::razorOverTracks() const {
    float mx = 0.0f, my = 0.0f;
    uiHoverMouse(mx, my);
    return mx >= headerX_ && my >= tracksTop_ && my < tracksViewBottom();
}

int64_t App::razorFrameFor(const Clip& c, float x) const {
    // A one-frame clip has no interior, so there is no legal cut: hand back its
    // start, which the caller reads as "offer nothing".
    if (c.duration < 2)
        return c.timelineStart;
    int64_t f = (int64_t)std::llround(xToFrame((double)x));
    if (snapPlayhead_) {
        // Same fixed screen-pixel radius as the trim magnet, so both feel alike at
        // any zoom. The rows either side are the targets - lining a cut up with the
        // one above or below is the reason to snap at all - plus the playhead. The
        // clip's own row offers nothing: its neighbours' edges all fall outside the
        // clamp below, and its own edges are that clamp.
        const double kSnapPx = 8.0;
        int64_t best = f;
        double bestPx = kSnapPx;
        auto consider = [&](int64_t frame) {
            double px = std::fabs(frameToX((double)frame) - (double)x);
            if (px < bestPx) { bestPx = px; best = frame; }
        };
        consider(timeline_.playhead);
        forEachViewClip([&](const Clip& o) {
            if (o.track != c.track - 1 && o.track != c.track + 1)
                return;
            consider(o.timelineStart);
            consider(o.end());
        });
        f = best;
    }
    // Inside the clip, never on an edge: a cut there would leave a zero-length piece.
    return std::clamp<int64_t>(f, c.timelineStart + 1, c.end() - 1);
}

void App::splitClipAt(int clipId, int64_t frame) {
    const Clip* hit = clipById(clipId);
    if (!hit)
        return;
    // The whole audio-follows-video group is cut in one go, so a split video clip
    // never leaves its audio whole underneath. Normalise to the parent first - the
    // razor may just as well have landed on the follower.
    int parentId = clipId;
    if (const Clip* clip = timeline_.linkParent(*hit))
        parentId = clip->id;
    std::vector<int> group{ parentId };
    for (int fid : timeline_.linkFollowers(parentId))
        group.push_back(fid);
    // Only members the cut actually falls inside are touched: a follower slipped
    // clear of this frame keeps its single piece.
    std::vector<int> cutIds;
    for (int id : group)
        if (const Clip* c = clipById(id))
            if (frame > c->timelineStart && frame < c->end())
                cutIds.push_back(id);
    if (cutIds.empty())
        return;

    ContentSnapshot before = captureContent();
    // Ids are handed out up front so a follower's tail can be pointed at the
    // parent's tail rather than at the head it was carved from.
    std::map<int, int> tailIds;
    for (int id : cutIds)
        tailIds[id] = nextClipId_++;
    // Built by value, added after the loop: pushing onto a sequence's clip vector
    // reallocates it, which would dangle every Clip* still held here.
    std::vector<std::pair<int, Clip>> tails; // (head clip id -> its new tail)
    for (int id : cutIds) {
        Clip* clip = clipById(id);
        if (!clip)
            continue;
        const int64_t adv = frame - clip->timelineStart;
        Clip tail = *clip;
        tail.id            = tailIds[id];
        tail.sourceOffset += adv;
        tail.duration      = clip->end() - frame;
        tail.timelineStart = frame;
        tail.shotId        = -1;  // the head goes on owning the shot (below)
        tail.fadeInFrames  = 0;   // the head keeps the head ramp, the tail the tail one
        if (auto it = tailIds.find(clip->linkedTo); it != tailIds.end())
            tail.linkedTo = it->second; // follow the tail parent, not the head
        clip->duration       = adv;
        clip->fadeOutFrames  = 0;
        // The shot is deliberately left alone, so its bar still spans both pieces:
        // a razor cuts the edit, not the shot the edit came from. The shot stays on
        // the head, since a shot belongs to exactly one clip and syncShotsToClips
        // anchors its bar to that clip's start - which the head, unlike the tail,
        // never moved.
        tails.push_back({ id, tail });
    }
    for (auto& t : tails)
        if (Sequence* seq = timeline_.sequenceOfClipMut(t.first))
            seq->clips.push_back(t.second);
    pruneTransitions(); // a dissolve straddling the new cut has lost the handle it spent
    pushContentUndo("CUT CLIP", before);
    hostSnapshotDirty_ = true; // re-push the project to spectators if hosting
    setStatus(cutIds.size() > 1 ? "CUT (LINKED)" : "CUT", 2000);
}

// ------------------------------------------------------------- clip fades

void App::setClipFade(int clipId, int edge, int64_t frames) {
    Clip* clip = clipById(clipId);
    if (!clip)
        return;
    const int64_t want = std::clamp<int64_t>(frames, 0, Timeline::fadeLimit(*clip));
    int64_t& target = (edge == 0) ? clip->fadeInFrames : clip->fadeOutFrames;
    if (target == want)
        return;
    // Fades are plain clip fields, so a ContentSnapshot restores them and there is
    // nothing to re-anchor or prune — the whole point of putting them on the clip.
    ContentSnapshot before = captureContent();
    target = want;
    pushContentUndo(want > 0 ? "SET FADE" : "REMOVE FADE", before);
}

void App::addFadeToSelection(int edge) {
    std::vector<int> ids = selectedClipIds_;
    if (ids.empty())
        if (const Clip* top = getTopMostClipAtFrame(timeline_.playhead))
            ids.push_back(top->id);
    // One undo step for the whole selection, so setClipFade's per-clip snapshot
    // would be wrong here — clamp and write directly, then push once.
    // Default length when this edge has nothing below it: 1 s, i.e. a fade to black.
    const int64_t deflt = std::max<int64_t>(
        (int64_t)std::llround(timeline_.fps > 0.0 ? timeline_.fps : 24.0), 2);
    ContentSnapshot before = captureContent();
    int applied = 0, tooShort = 0;
    for (int id : ids) {
        Clip* clip = clipById(id);
        if (!clip || clip->audio)
            continue;
        const int64_t cap = Timeline::fadeLimit(*clip);
        if (cap <= 0) { ++tooShort; continue; }
        // Where a lower track shows through, ramp exactly across that run so the
        // blend covers the whole overlap; elsewhere fall back to the default.
        const int64_t cover = fadeCoverageFrames(*clip, edge);
        int64_t& target = (edge == 0) ? clip->fadeInFrames : clip->fadeOutFrames;
        target = std::min(cover > 0 ? cover : deflt, cap);
        ++applied;
    }
    if (applied == 0) {
        setStatus(tooShort > 0 ? "CLIP TOO SHORT FOR A FADE" : "NO CLIP SELECTED", 3000);
        return;
    }
    pushContentUndo(edge == 0 ? "FADE IN" : "FADE OUT", before);
    setStatus(std::string(edge == 0 ? "FADE IN " : "FADE OUT ") +
              std::to_string(applied) + (applied == 1 ? " CLIP" : " CLIPS"), 2000);
}

Clip* App::fadeHandleAt(int track, float x, float y, int& edge) {
    // The dot lives on the clip's top edge; only that band is a fade grab, so the
    // rest of the clip still selects/drags and the side trim zones still trim.
    Clip* best = nullptr;
    float bestDist = kFadeHandleW;
    for (auto& s : timeline_.sequences)
        for (auto& c : s.clips) {
            if (c.track != track || c.audio)
                continue;
            SDL_FRect band = clipBandRect(c.track, false);
            // Strictly the band's own top edge. Without the lower bound the gutter
            // above the row counts too, which would put this row's dots in the row
            // above's space.
            if (y < band.y || y > band.y + kFadeHandleH)
                continue;
            int64_t fin = 0, fout = 0;
            Timeline::clipFades(c, fin, fout);
            // Apex of each ramp; a clip with no fade yet offers the dot in its
            // corner, so the same drag creates one.
            const float xIn  = (float)frameToX((double)(c.timelineStart + fin));
            const float xOut = (float)frameToX((double)(c.end() - fout));
            const float xL = (float)frameToX((double)c.timelineStart);
            const float xR = (float)frameToX((double)c.end());
            if (x < xL - kFadeHandleW || x > xR + kFadeHandleW)
                continue;
            if (std::abs(x - xIn) <= bestDist)  { bestDist = std::abs(x - xIn);  best = &c; edge = 0; }
            if (std::abs(x - xOut) <= bestDist) { bestDist = std::abs(x - xOut); best = &c; edge = 1; }
        }
    return best;
}

void App::beginClipFade(Clip& c, int edge) {
    fadingClip_ = true;
    fadeClipId_ = c.id;
    fadeEdge_ = edge;
    int64_t fin = 0, fout = 0;
    Timeline::clipFades(c, fin, fout);
    fadeNewFrames_ = (edge == 0) ? fin : fout;
    selectClipSingle(c.id);
    selectedGapTrack_ = -1;
    playing_ = false;
}

void App::updateClipFade(float mouseX) {
    Clip* clip = clipById(fadeClipId_);
    if (!clip)
        return;
    const int64_t f = (int64_t)std::llround(xToFrame(mouseX));
    const int64_t cap = Timeline::fadeLimit(*clip);
    fadeNewFrames_ = (fadeEdge_ == 0) ? std::clamp<int64_t>(f - clip->timelineStart, 0, cap)
                                      : std::clamp<int64_t>(clip->end() - f, 0, cap);
}

void App::commitClipFade() {
    if (fadingClip_)
        setClipFade(fadeClipId_, fadeEdge_, fadeNewFrames_); // no-op when unchanged
    fadingClip_ = false;
    fadeClipId_ = -1;
}

int64_t App::fadeCoverageFrames(const Clip& c, int edge) const {
    if (c.audio || c.track < 0 || c.track >= trackCount())
        return 0;
    // The cap is what keeps the two runs disjoint: a clip lying entirely over a
    // lower one gets a head run and a tail run that meet in the middle, rather
    // than one run spanning the whole clip that no single fade could match.
    const int64_t cap = Timeline::fadeLimit(c);
    if (cap <= 0)
        return 0;
    // Walk the clips below one at a time instead of frame by frame. getClipBelow is
    // the compositor's own lookup, so butted lower clips extend the run exactly as
    // far as the blend would actually reach.
    int64_t frames = 0;
    int64_t f = (edge == 0) ? c.timelineStart : c.end() - 1;
    while (frames < cap) {
        const Clip* clip = getClipBelow(f, c.track);
        if (!clip)
            break;
        const int64_t adv = std::min(edge == 0 ? clip->end() - f : f - clip->timelineStart + 1,
                                     cap - frames);
        if (adv <= 0)
            break;
        frames += adv;
        f += (edge == 0) ? adv : -adv;
    }
    return frames;
}

Transition* App::transitionEdgeAt(int track, float x, int& edge) {
    Transition* best = nullptr;
    float bestDist = kTrimHandleW;
    for (auto& s : timeline_.sequences)
        for (auto& t : s.transitions) {
            Timeline::TransitionSpan sp;
            if (!timeline_.resolveTransition(s, t, sp) || sp.a->track != track)
                continue;
            float xL = (float)frameToX((double)sp.start);
            float xR = (float)frameToX((double)sp.end);
            if (x < xL || x > xR)
                continue;
            // Handle zones sit inside the box, as clip trim handles do, so the
            // cursor's side of an edge picks the side it will resize.
            float dL = x - xL, dR = xR - x;
            if (dL <= bestDist) { bestDist = dL; best = &t; edge = 0; }
            if (dR <= bestDist) { bestDist = dR; best = &t; edge = 1; }
        }
    return best;
}

void App::beginTransitionResize(const Transition& t, int edge) {
    resizingTransition_ = true;
    resizeTransId_ = t.id;
    resizeTransEdge_ = edge;
    resizeTransNewIn_ = t.inFrames;
    resizeTransNewOut_ = t.outFrames;
    clearClipSelection();          // clears selectedTransitionId_, so claim it after
    selectedGapTrack_ = -1;
    selectedTransitionId_ = t.id;
    playing_ = false;
}

void App::updateTransitionResize(float mouseX) {
    for (auto& s : timeline_.sequences)
        for (auto& t : s.transitions) {
            if (t.id != resizeTransId_)
                continue;
            Timeline::TransitionSpan sp;
            if (!timeline_.resolveTransition(s, t, sp))
                return;
            int64_t maxIn = 0, maxOut = 0;
            timeline_.transitionLimits(*sp.a, *sp.b, maxIn, maxOut);
            const int64_t f = (int64_t)std::llround(xToFrame(mouseX));
            // Each edge moves its own side of the cut. The other side stays put, so
            // dragging turns a centred dissolve into an off-centre one — there is
            // no separate alignment state.
            if (resizeTransEdge_ == 0)
                resizeTransNewIn_ = std::clamp<int64_t>(sp.cut - f, 0, maxIn);
            else
                resizeTransNewOut_ = std::clamp<int64_t>(f - sp.cut, 0, maxOut);
            return;
        }
}

void App::commitTransitionResize() {
    if (resizingTransition_) {
        // Snapshot first and mutate through a plain lookup: pruneTransitions can
        // erase the entry (dragging both edges onto the cut collapses the box,
        // which reads as a delete rather than a zero-width dissolve), so nothing
        // may hold a reference into the vector across that call.
        ContentSnapshot before = captureContent();
        bool changed = false;
        for (auto& s : timeline_.sequences)
            for (auto& t : s.transitions)
                if (t.id == resizeTransId_ &&
                    (t.inFrames != resizeTransNewIn_ || t.outFrames != resizeTransNewOut_)) {
                    t.inFrames = resizeTransNewIn_;
                    t.outFrames = resizeTransNewOut_;
                    changed = true;
                }
        if (changed) {
            pruneTransitions();
            pushContentUndo("RESIZE DISSOLVE", before);
        }
    }
    resizingTransition_ = false;
    resizeTransId_ = -1;
}

bool App::isClipSelected(int id) const {
    return std::find(selectedClipIds_.begin(), selectedClipIds_.end(), id) != selectedClipIds_.end();
}

// Grows `ids` in place, so the index loop keeps walking newly appended entries —
// a follower's own followers come along too, without recursion.
void App::addLinkedFollowers(std::vector<int>& ids) const {
    for (size_t i = 0; i < ids.size(); ++i)
        for (int f : timeline_.linkFollowers(ids[i]))
            if (std::find(ids.begin(), ids.end(), f) == ids.end())
                ids.push_back(f);
}

// Selecting a clip selects the audio linked to it. Expanding here rather than at
// each gesture is what gets moving, copying and deleting a linked pair for free:
// every one of those acts on selectedClipIds_. The link is asymmetric, so picking
// the audio alone stays a selection of one.
void App::selectClipSingle(int id) {
    selectedClipIds_ = { id };
    addLinkedFollowers(selectedClipIds_);
    selectedClipId_ = id;
    selectedTransitionId_ = -1; // clip and dissolve selection are mutually exclusive
    binSelectionActive_ = false; // Delete goes back to the timeline
}

void App::toggleClipSelection(int id) {
    std::vector<int> group{ id }; // the clicked clip and the audio that follows it
    addLinkedFollowers(group);
    auto inGroup = [&](int k) { return std::find(group.begin(), group.end(), k) != group.end(); };
    if (isClipSelected(id)) {
        // Removing a clip removes the linked audio that came in with it.
        selectedClipIds_.erase(std::remove_if(selectedClipIds_.begin(), selectedClipIds_.end(), inGroup),
                               selectedClipIds_.end());
        selectedClipId_ = selectedClipIds_.empty() ? -1 : selectedClipIds_.back();
    } else {
        for (int k : group)
            if (!isClipSelected(k))
                selectedClipIds_.push_back(k);
        selectedClipId_ = id; // clicked clip becomes the primary
    }
    selectedTransitionId_ = -1;
    binSelectionActive_ = false; // Delete goes back to the timeline
}

// ------------------------------------------------------------- audio/video link

// Carry a trim of `parentId` onto the audio linked to it: the follower's start is
// re-derived from linkOffset (a head trim moved the parent) and the same source /
// duration deltas are applied, clamped to what the audio can supply. An audio file
// shorter than the picture therefore ends up shorter rather than referencing frames
// it doesn't have.
void App::trimLinkedFollowers(int parentId, int64_t dSrc, int64_t dDur) {
    for (int fid : timeline_.linkFollowers(parentId)) {
        const Clip* clip = timeline_.findClipById(parentId);
        Clip* f = clipById(fid);
        if (!clip || !f)
            continue;
        const int64_t srcFrames = timeline_.clipSourceFrames(*f);
        f->timelineStart = std::max<int64_t>(clip->timelineStart + f->linkOffset, 0);
        f->sourceOffset  = std::clamp<int64_t>(f->sourceOffset + dSrc, 0,
                                              std::max<int64_t>(srcFrames - 1, 0));
        f->duration      = std::clamp<int64_t>(f->duration + dDur, 1,
                                              std::max<int64_t>(srcFrames - f->sourceOffset, 1));
    }
}

// Put every follower back at parent.timelineStart + linkOffset. A ripple, a
// close-gap or an overwrite drop moves clips one track at a time, so the picture
// can slide out from under its audio; one sweep before the repack restores the
// pairs, which keeps every edit path from having to know links exist. Idempotent.
void App::resyncLinkedClips() {
    timeline_.forEachClipMut([&](Clip& c) {
        if (c.linkedTo == 0)
            return;
        if (const Clip* clip = timeline_.linkParent(c))
            c.timelineStart = std::max<int64_t>(clip->timelineStart + c.linkOffset, 0);
    });
}

void App::linkSelectedClips() {
    const Clip* parent = nullptr;
    std::vector<int> followers;
    for (int id : selectedClipIds_) {
        const Clip* clip = timeline_.findClipById(id);
        if (!clip)
            continue;
        if (clip->audio) {
            followers.push_back(id);
        } else if (!parent) {
            parent = clip;
        } else {
            setStatus("LINK NEEDS EXACTLY ONE VIDEO CLIP", 4000);
            return;
        }
    }
    if (!parent || followers.empty()) {
        setStatus("SELECT ONE VIDEO CLIP AND THE AUDIO TO LINK TO IT", 4000);
        return;
    }
    // A link only holds inside one sequence: the follower's position is derived
    // from the parent's, and repackSequences shifts whole sequences at a time.
    const int seq = timeline_.seqIndexOfClip(parent->id);
    const int parentId = parent->id;
    const int64_t parentStart = parent->timelineStart;
    ContentSnapshot before = captureContent();
    int linked = 0;
    for (int id : followers) {
        if (timeline_.seqIndexOfClip(id) != seq)
            continue;
        Clip* clip = clipById(id);
        clip->linkedTo = parentId;
        clip->linkOffset = clip->timelineStart - parentStart;
        ++linked;
    }
    if (linked == 0) {
        setStatus("CANNOT LINK ACROSS SEQUENCES", 4000);
        return; // nothing was written: the guard above skipped every candidate
    }
    pushContentUndo("LINK AUDIO", before);
    setStatus(linked == 1 ? "LINKED AUDIO"
                          : "LINKED " + std::to_string(linked) + " AUDIO CLIPS");
}

void App::unlinkSelectedClips() {
    // Clear the selected followers, plus the followers of any selected parent —
    // right-clicking the video clip is the natural way to break its link.
    std::vector<int> ids;
    auto want = [&](int id) {
        if (std::find(ids.begin(), ids.end(), id) == ids.end())
            ids.push_back(id);
    };
    for (int id : selectedClipIds_) {
        if (const Clip* clip = timeline_.findClipById(id); clip && timeline_.linkParent(*clip))
            want(id);
        for (int f : timeline_.linkFollowers(id))
            want(f);
    }
    if (ids.empty()) {
        setStatus("NOTHING LINKED", 3000);
        return;
    }
    ContentSnapshot before = captureContent();
    for (int id : ids)
        if (Clip* c = clipById(id)) { c->linkedTo = 0; c->linkOffset = 0; }
    pushContentUndo("UNLINK AUDIO", before);
    setStatus(ids.size() == 1 ? "UNLINKED AUDIO"
                              : "UNLINKED " + std::to_string(ids.size()) + " AUDIO CLIPS");
}

bool App::selectionCanLink() const {
    const Clip* parent = nullptr;
    int videos = 0;
    for (int id : selectedClipIds_)
        if (const Clip* c = timeline_.findClipById(id); c && !c->audio) {
            ++videos;
            parent = c;
        }
    if (videos != 1)
        return false;
    const int seq = timeline_.seqIndexOfClip(parent->id);
    for (int id : selectedClipIds_)
        if (const Clip* c = timeline_.findClipById(id);
            c && c->audio && c->linkedTo != parent->id && timeline_.seqIndexOfClip(id) == seq)
            return true;
    return false;
}

bool App::selectionHasLink() const {
    for (int id : selectedClipIds_) {
        if (const Clip* clip = timeline_.findClipById(id); clip && timeline_.linkParent(*clip))
            return true;
        if (!timeline_.linkFollowers(id).empty())
            return true;
    }
    return false;
}

// Also drops any dissolve selection, so Delete is never ambiguous. The two sites
// that select a dissolve call this first and claim the id afterwards.
void App::clearClipSelection() {
    selectedClipIds_.clear();
    selectedClipId_ = -1;
    selectedTransitionId_ = -1;
    // Every dissolve / gap selection clears the clip selection first, so this one
    // line covers those gestures too.
    binSelectionActive_ = false; // Delete goes back to the timeline
}

void App::copySelectedClips() {
    std::vector<Clip> buf;
    for (int id : selectedClipIds_)
        if (const Clip* clip = timeline_.findClipById(id))
            buf.push_back(*clip);
    if (buf.empty()) {
        setStatus("NO CLIP SELECTED");
        return;
    }
    // Rebase on the earliest start so paste can anchor the group at the playhead,
    // and order top-down so the topmost clip claims its track first.
    int64_t base = buf.front().timelineStart;
    for (const auto& clip : buf)
        base = std::min(base, clip.timelineStart);
    for (auto& clip : buf)
        clip.timelineStart -= base;
    std::sort(buf.begin(), buf.end(), [](const Clip& a, const Clip& b) {
        return a.track != b.track ? a.track < b.track : a.timelineStart < b.timelineStart;
    });
    clipboard_ = std::move(buf);
    setStatus(clipboard_.size() == 1 ? "COPIED CLIP"
                                     : "COPIED " + std::to_string(clipboard_.size()) + " CLIPS");
}

int App::firstFreeTrackFor(bool audio, int64_t start, int64_t duration) const {
    const int n = std::max(trackCount(), 1);
    const Timeline::TrackKind want = audio ? Timeline::TrackKind::Audio : Timeline::TrackKind::Video;
    for (int t = 0; t < n; ++t) {
        // A track holds one type only; an empty row adopts whatever lands on it.
        Timeline::TrackKind k = timeline_.trackKind(t);
        if (k != Timeline::TrackKind::Empty && k != want)
            continue;
        if (!timeline_.trackHasOverlap(t, start, duration, -1))
            return t;
    }
    return n; // nothing fits: the caller needs a new row
}

void App::pasteClips() {
    if (clipboard_.empty()) {
        setStatus("CLIPBOARD IS EMPTY");
        return;
    }
    ensureDefaultSequence();

    // Route the paste to a sequence the same way a drop does: the filtered one
    // when viewing a single sequence, else the sequence the playhead is in.
    int targetIdx = -1;
    if (int fsi = filteredSeqIdx(); fsi >= 0)
        targetIdx = fsi;
    else if (int si = timeline_.seqIndexAtFrame(timeline_.playhead); si >= 0)
        targetIdx = si;
    if (targetIdx < 0)
        targetIdx = activeSequenceIdx_;
    // Confine placement to that sequence's region, so a paste can never land in
    // (and ripple) a neighbouring sequence.
    const int64_t at = std::max(timeline_.playhead, timeline_.seqRegions()[targetIdx].start);

    auto beforeSeqs = timeline_.sequences;
    auto beforeShots = timeline_.shots;
    const int beforeTracks = timeline_.trackCount;

    std::vector<int> newIds;
    std::map<int, int> copyOf; // clipboard clip id -> pasted clip id
    int skipped = 0;
    int firstTrack = 0;
    for (const Clip& src : clipboard_) {
        if (!timeline_.findMediaById(src.mediaId)) { ++skipped; continue; } // media gone
        Clip clip = src;
        clip.id = nextClipId_++;
        copyOf[src.id] = clip.id;
        clip.timelineStart = at + src.timelineStart;
        clip.shotId = -1; // a copy is not part of the original's shot
        int t = firstFreeTrackFor(clip.audio, clip.timelineStart, clip.duration);
        if (t >= timeline_.trackCount)
            timeline_.trackCount = t + 1; // clip fits nowhere: add a track for it
        clip.track = t;
        if (newIds.empty())
            firstTrack = t;
        newIds.push_back(clip.id);
        timeline_.sequences[targetIdx].clips.push_back(std::move(clip));
    }
    if (newIds.empty()) {
        timeline_.trackCount = beforeTracks;
        setStatus("CANNOT PASTE: SOURCE MEDIA IS MISSING", 4000);
        return;
    }
    // Links point within the pasted set (the clipboard holds copies, so the ids in
    // linkedTo are the ones that were copied). A pasted audio clip whose picture
    // clip isn't in the paste is left unlinked rather than following the original.
    for (int id : newIds)
        if (Clip* c = clipById(id); c && c->linkedTo != 0) {
            auto it = copyOf.find(c->linkedTo);
            c->linkedTo = (it != copyOf.end()) ? it->second : 0;
            if (c->linkedTo == 0)
                c->linkOffset = 0;
        }

    timeline_.repackSequences();
    ensureTrailingEmptyTrack();
    timeline_.clampPlayhead();
    selectedClipIds_ = newIds;
    selectedClipId_ = newIds.back();
    selectedGapTrack_ = -1;

    auto afterSeqs = timeline_.sequences;
    auto afterShots = timeline_.shots;
    const int afterTracks = timeline_.trackCount;
    undoStack_.push({
        newIds.size() == 1 ? "PASTE CLIP" : "PASTE " + std::to_string(newIds.size()) + " CLIPS",
        [this, beforeSeqs, beforeShots, beforeTracks] {
            timeline_.sequences = beforeSeqs; timeline_.shots = beforeShots;
            timeline_.trackCount = beforeTracks;
            timeline_.clampPlayhead();
            clearClipSelection();
        },
        [this, afterSeqs, afterShots, afterTracks, newIds] {
            timeline_.sequences = afterSeqs; timeline_.shots = afterShots;
            timeline_.trackCount = afterTracks;
            timeline_.clampPlayhead();
            selectedClipIds_ = newIds;
            selectedClipId_ = newIds.back();
        },
    });

    if (panelOpen(kPanelProjectExplorer))
        refreshExplorerOrder();
    std::string msg = newIds.size() == 1
                          ? "PASTED CLIP ON TRACK " + std::to_string(firstTrack + 1)
                          : "PASTED " + std::to_string(newIds.size()) + " CLIPS";
    if (skipped > 0)
        msg += " (" + std::to_string(skipped) + " SKIPPED: MEDIA MISSING)";
    setStatus(msg, skipped > 0 ? 4000 : 2000);
}

void App::deleteSelectedClip() {
    if (selectedClipIds_.empty())
        return;
    // A video clip takes its linked audio with it. Selecting in the timeline
    // already pulls followers in, but Delete is reachable from selections built
    // elsewhere (a restore, a paste), so expand again here.
    std::vector<int> ids = selectedClipIds_;
    addLinkedFollowers(ids);

    // Label: the file name for a single clip, else a count.
    std::string label;
    if (ids.size() == 1) {
        std::string path;
        if (Clip* clip = clipById(ids[0]))
            if (auto pm = timeline_.findMediaById(clip->mediaId)) path = pm->path();
        label = fileLabel(path);
    }

    // Removal isn't positional (it can trim/cover neighbours indirectly via
    // repack), so snapshot the whole clip/shot state before and after the erase;
    // undo/redo restore it in one step.
    auto beforeSeqs = timeline_.sequences;
    auto beforeShots = timeline_.shots;
    int removed = 0;
    for (int id : ids) {
        if (Sequence* seq = timeline_.sequenceOfClipMut(id)) {
            auto it = std::find_if(seq->clips.begin(), seq->clips.end(),
                                   [id](const Clip& c) { return c.id == id; });
            if (it != seq->clips.end()) { seq->clips.erase(it); ++removed; }
        }
        // Cancel any in-flight drag/trim referencing this clip.
        if (draggingClip_ && dragClipId_ == id) { draggingClip_ = false; dragClipId_ = -1; }
        if (trimmingClip_ && trimClipId_ == id) { trimmingClip_ = false; trimClipId_ = -1; }
        if (pendingDragClipId_ == id) pendingDragClipId_ = -1;
    }
    if (removed == 0) { clearClipSelection(); return; }
    timeline_.repackSequences();
    timeline_.clampPlayhead();
    pruneTransitions(); // a dissolve whose clip is gone goes with it
    auto afterSeqs = timeline_.sequences;
    auto afterShots = timeline_.shots;
    std::string desc = ids.size() == 1 ? ("DELETE " + label)
                                        : ("DELETE " + std::to_string(removed) + " CLIPS");
    undoStack_.push({
        desc,
        [this, beforeSeqs, beforeShots, ids] {
            timeline_.sequences = beforeSeqs; timeline_.shots = beforeShots;
            timeline_.clampPlayhead();
            selectedClipIds_ = ids;
            selectedClipId_ = ids.empty() ? -1 : ids.back();
        },
        [this, afterSeqs, afterShots] {
            timeline_.sequences = afterSeqs; timeline_.shots = afterShots;
            timeline_.clampPlayhead();
            clearClipSelection();
        },
    });

    clearClipSelection();
    timeline_.clampPlayhead();
    setStatus(ids.size() == 1 ? ("DELETED " + label)
                              : ("DELETED " + std::to_string(removed) + " CLIPS"));
}

void App::deleteSelectedGap() {
    if (selectedGapTrack_ < 0)
        return;
    const int64_t shift = selectedGapEnd_ - selectedGapStart_;
    const int64_t gapEnd = selectedGapEnd_;
    selectedGapTrack_ = -1;
    if (shift <= 0)
        return;
    // Global ripple: every clip starting at/after the gap slides back to close
    // it; shots follow. Snapshotted so it undoes in one step.
    PositionSnapshot before = capturePositions();
    timeline_.forEachClipMut([&](Clip& c) {
        if (c.timelineStart >= gapEnd) c.timelineStart -= shift;
    });
    resyncLinkedClips(); // a pair straddling gapEnd would otherwise be parted
    syncShotsToClips();
    timeline_.repackSequences();
    timeline_.clampPlayhead();
    pruneTransitions(); // closing a gap can make two clips adjacent, or part two
    PositionSnapshot after = capturePositions();
    undoStack_.push({
        "CLOSE GAP",
        [this, before] { restorePositions(before); },
        [this, after]  { restorePositions(after); },
    });
    setStatus("CLOSED GAP");
}

// ---------------------------------------------------------------- undo / redo

void App::undo() {
    std::string label = undoStack_.undo();
    setStatus(label.empty() ? "NOTHING TO UNDO" : "UNDO " + label);
}

void App::redo() {
    std::string label = undoStack_.redo();
    setStatus(label.empty() ? "NOTHING TO REDO" : "REDO " + label);
}

// Map a cursor position to a drop target (track row + snapped start frame). The
// cursor marks the clip's left edge; the real length is unknown until drop, so a
// fixed-width placeholder is used for snapping the (estimated) end.
void App::dropTargetAt(float x, float y, int& track, int64_t& start) const {
    track = trackFromY(y);
    int64_t placeholderDur = (int64_t)std::llround(kHoverBoxPx * framesPerPx_);
    start = snapDragStart(xToFrame(x), placeholderDur, -1);
}

// Decide where a dropped item lands. Over the timeline track area the cursor
// picks the row and snapped start frame (matching the dotted preview box).
// Anywhere else (notably the frame view) the mouse position is ignored and the
// item lands at the frame indicator (playhead) — track -1 means "auto-pick".
void App::resolveDropTarget(float x, float y, int& track, int64_t& start) const {
    if (overTrackArea(x, y)) {
        dropTargetAt(x, y, track, start);
    } else {
        track = -1;
        start = timeline_.playhead;
    }
}

bool App::overTrackArea(float x, float y) const {
    return x >= headerX_ && y >= tracksTop_ && y < tracksViewBottom();
}

// The timeline's top edge, as a grabbable band straddling it (kTimelineEdgeHitH/2
// into the player above and into the info bar below — the bar's buttons are
// centred in it, so those few pixels are its own margin). Nothing to grab while
// the launcher is up: there is no timeline then.
bool App::overTimelineEdge(float mx, float my) const {
    // No timeline, no edge to drag: both states park tlRect_ at the bottom of the
    // window, where the hit zone would otherwise straddle the foot of the image.
    // Compact is a third: the timeline is exactly its bands, so there is no height
    // to trade with the player until the tracks are back.
    if (launcherVisible() || cinemaMode_ || compactTimeline_)
        return false;
    return mx >= tlRect_.x && mx < tlRect_.x + tlRect_.w
        && std::abs(my - tlRect_.y) <= kTimelineEdgeHitH * 0.5f;
}

void App::updateFileHover(float x, float y) {
    // Over the video frame the drop-action chooser is what previews the drop.
    playerDropActive_ = inPlayerView(x, y) && playerDropChooserApplies();
    playerDropHover_ = playerDropActive_ ? playerDropBoxAt(x, y) : -1;
    if (!overTrackArea(x, y)) {
        fileHoverActive_ = false; // only preview (and snap to cursor) over the track area
        return;
    }
    dropTargetAt(x, y, fileHoverTrack_, fileHoverStart_);
    fileHoverSpan_ = 0; // an OS file drag never knows its length up front
    fileHoverActive_ = true;
}

// ---------------------------------------------------------------- frame preview

namespace {
// Box-averaged downscale of a frame to `outW` px wide, height derived from the
// source's display aspect (its pixel aspect applied, so an anamorphic frame comes
// out in its true shape). Fills `out` (outW*oh*4) and reports the dims.
void downscaleToWidth(const Frame& src, int outW,
                      std::vector<uint8_t>& out, int& ow, int& oh) {
    const int sw = src.width, sh = src.height;
    if (sw <= 0 || sh <= 0) {
        out.clear(); ow = oh = 0; return;
    }
    const int dw = std::max(1, outW);
    const double dispW = sw * (src.pixelAspect > 0.0f ? src.pixelAspect : 1.0f);
    const int dh = std::max(1, (int)std::lround((double)dw * sh / dispW));
    renderFrameScaled(src, dw, dh, out);
    if (out.empty()) { ow = oh = 0; return; }
    ow = dw; oh = dh;
}
} // namespace

// Resolve the frame under the ruler cursor and make sure its thumbnail is (being)
// built. Throttled to at most one resolve per 5px of cursor travel so a fast drag
// across the ruler doesn't decode every intermediate frame. Called on mouse move.
void App::updateFramePreview() {
    if (!showFramePreview_ || playing_ || !tlHoverActive_) {
        previewHasKey_ = false;
        previewClipId_ = -1;
        previewResolvedOnce_ = false; // re-enter resolves immediately
        return;
    }
    if (previewResolvedOnce_ && std::fabs(tlHoverX_ - previewLastResolveX_) < 5.0f)
        return;
    previewLastResolveX_ = tlHoverX_;
    previewResolvedOnce_ = true;

    int64_t length = timeline_.length();
    if (length <= 0) { previewHasKey_ = false; previewClipId_ = -1; return; }
    int64_t hf = std::clamp<int64_t>(scrubFrame(tlHoverX_),
                                     0, length - 1);
    const Clip* clip = getTopMostClipAtFrame(hf);
    if (!clip || clip->mediaId.empty()) { previewHasKey_ = false; previewClipId_ = -1; return; }
    auto media = timeline_.findMediaById(clip->mediaId);
    if (!media || media->openFailed()) { previewHasKey_ = false; previewClipId_ = -1; return; }

    int64_t sf = clip->sourceOffset + (hf - clip->timelineStart);
    CacheKey key{ clip->mediaId, sf };
    previewKey_ = key;
    previewClipId_ = clip->id;
    previewHasKey_ = true;
    // The frame we're about to have is also a usable Overview / SOURCES thumbnail
    // for this clip, so pass those keys along for the gap-fill write-through.
    ensurePreviewThumb(key, media, sf,
                       clip->audio ? std::string() : clipThumbKey(*clip),
                       clip->audio ? std::string() : sourceThumbKey(media.get()));
}

// Ensure a thumbnail exists for `key`: touch it if already cached, lift it from
// the resident full-res FrameCache when available (free — no decode), otherwise
// decode off the main thread and store it on completion.
void App::ensurePreviewThumb(const CacheKey& key, std::shared_ptr<Media> media,
                             int64_t srcFrame, const std::string& clipKey,
                             const std::string& srcKey) {
    if (auto it = previewCache_.find(key); it != previewCache_.end()) {
        auto lit = std::find(previewLru_.begin(), previewLru_.end(), key);
        if (lit != previewLru_.end()) previewLru_.erase(lit);
        previewLru_.push_back(key); // mark most-recently-used
        return;
    }
    if (previewInflight_.count(key))
        return; // decode already pending

    // Fast path: the display cache already holds this exact frame at full res.
    if (FramePtr f = cache_->get(key); f && f->width > 0 && f->height > 0) {
        PreviewThumb t;
        downscaleToWidth(*f, kPreviewW, t.rgba, t.w, t.h);
        if (t.w > 0) storePreview(key, std::move(t));
        fillThumbsFromPreviewFrame(f, clipKey, srcKey);
        return;
    }

    // Slow-network guard: once a read was found to be slow we no longer decode on
    // demand — previews then only appear for frames the playback range already
    // cached (served by the fast path above). A blocking network read can't be
    // interrupted mid-flight, so we also cap ourselves to one outstanding decode:
    // that keeps a single slow read from becoming a pile-up across the worker pool.
    if (previewSlowAccess_.load(std::memory_order_relaxed))
        return;
    if (!previewInflight_.empty())
        return; // one on-demand decode at a time

    // Slow path: decode on a worker thread, downscale, publish on the main thread.
    // The full-res frame is also handed back so playback can reuse it (see below).
    previewInflight_.insert(key);
    auto out = std::make_shared<PreviewThumb>();
    auto full = std::make_shared<FramePtr>();
    const uint64_t gen = previewGen_;
    work_.submit(
        [this, media, srcFrame, out, full](const std::atomic<bool>& stop) {
            if (stop) return;
            std::string err;
            std::shared_ptr<MediaSource> src = media->ensureOpen(err);
            if (!src || stop) return;
            const int64_t slate = media->slateOffset(); // proxy slate: not part of the shot
            int64_t count = src->frameCount() - slate;
            int64_t f = srcFrame;
            if (count > 0)      f = std::clamp<int64_t>(f, 0, count - 1);
            else if (f < 0)     f = 0;
            auto t0 = std::chrono::steady_clock::now();
            FramePtr frame = src->readFrame(f + slate);
            auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                          std::chrono::steady_clock::now() - t0).count();
            if (ms > kPreviewSlowMs)
                previewSlowAccess_.store(true, std::memory_order_relaxed);
            if (!frame || stop) return;
            downscaleToWidth(*frame, kPreviewW, out->rgba, out->w, out->h);
            *full = std::move(frame); // publish the full-res frame to the cache
        },
        [this, key, gen, out, full, clipKey, srcKey]() {
            previewInflight_.erase(key);
            if (gen != previewGen_) return;  // project changed while decoding
            if (*full) cache_->put(key, *full); // reuse the decode for playback
            if (out->w > 0 && out->h > 0)
                storePreview(key, std::move(*out));
            fillThumbsFromPreviewFrame(*full, clipKey, srcKey);
        });
}

// Write-through for the hover preview: reuse a frame it already has to produce
// the Overview (per-clip) and SOURCES (per-source) thumbnail JPEGs for the clip
// under the cursor. Gap-fill only — an existing thumbnail (normally the clip's
// middle frame) is never replaced, so the grid and bin don't shift around as you
// scrub; this just means they're already populated the first time you open them.
// The 512x512 fit + JPEG encode runs on a worker thread off the full-res frame,
// so these files are indistinguishable from the background pass's output.
void App::fillThumbsFromPreviewFrame(FramePtr frame, const std::string& clipKey,
                                     const std::string& srcKey) {
    if (!frame)
        return;
    std::vector<std::string> keys;
    for (const std::string& k : { clipKey, srcKey })
        if (!k.empty() && previewThumbWritten_.insert(k).second)
            keys.push_back(k);
    if (keys.empty())
        return;

    std::string projPath = projectHasPath_ ? projectPath_ : std::string();
    work_.submit([frame, keys, projPath](const std::atomic<bool>& stop) {
        const std::string dir = ThumbnailCache::resolveDir(projPath);
        for (const std::string& k : keys) {
            if (stop) return;
            ThumbnailCache::writeIfMissing(dir, k, *frame);
        }
    });
}

void App::storePreview(const CacheKey& key, PreviewThumb&& thumb) {
    previewCache_[key] = std::move(thumb);
    if (auto lit = std::find(previewLru_.begin(), previewLru_.end(), key);
            lit != previewLru_.end())
        previewLru_.erase(lit);
    previewLru_.push_back(key);
    while (previewLru_.size() > kPreviewMax) {
        CacheKey evict = previewLru_.front();
        previewLru_.erase(previewLru_.begin());
        previewCache_.erase(evict);
        if (previewDisplayedValid_ && previewDisplayedKey_ == evict)
            previewDisplayedValid_ = false;
    }
}

void App::clearFramePreview() {
    previewGen_++;              // any in-flight decode result is now discarded
    previewCache_.clear();
    previewLru_.clear();
    previewInflight_.clear();
    previewHasKey_ = false;
    previewClipId_ = -1;
    previewResolvedOnce_ = false;
    previewDisplayedValid_ = false;
    previewDisplayedClipId_ = -1;
    previewSlowAccess_.store(false, std::memory_order_relaxed); // re-probe new media
    previewThumbWritten_.clear(); // thumbnail dir/keys change with the project
    waveforms_.clear(); // media set changed: re-decode envelopes on demand
}

// Draw the resolved-frame thumbnail as a small floating panel just above the
// ruler, centered on the cursor. No-op until the thumbnail is ready.
void App::renderFramePreview() {
    if (!showFramePreview_ || playing_ || !tlHoverActive_)
        return;

    // Promote the freshly-resolved frame to the display texture as soon as its
    // thumbnail is ready. Until then keep showing whatever is already up, so a
    // fast scrub doesn't flicker/close between frames — the panel stays open and
    // follows the cursor, and the image just lags until the new decode lands.
    if (previewHasKey_) {
        if (auto it = previewCache_.find(previewKey_);
                it != previewCache_.end() && it->second.w > 0 && it->second.h > 0) {
            const PreviewThumb& t = it->second;
            if (!previewTex_ || previewTexW_ != t.w || previewTexH_ != t.h) {
                if (previewTex_) SDL_DestroyTexture(previewTex_);
                previewTex_ = SDL_CreateTexture(renderer_, SDL_PIXELFORMAT_RGBA32,
                                                SDL_TEXTUREACCESS_STREAMING, t.w, t.h);
                SDL_SetTextureScaleMode(previewTex_, SDL_SCALEMODE_LINEAR);
                previewTexW_ = t.w; previewTexH_ = t.h;
                previewDisplayedValid_ = false;
            }
            if (previewTex_ && !(previewDisplayedValid_ && previewDisplayedKey_ == previewKey_)) {
                SDL_UpdateTexture(previewTex_, nullptr, t.rgba.data(), t.w * 4);
                previewDisplayedKey_ = previewKey_;
                previewDisplayedClipId_ = previewClipId_;
                previewDisplayedValid_ = true;
            }
        }
    }

    // Crossing into a different clip closes the panel immediately: the "keep the
    // last frame up" lag above is only wanted for a fast scrub within one clip, so
    // that we never briefly show the previous clip's frame over a new clip's range.
    if (previewDisplayedValid_ && previewDisplayedClipId_ != previewClipId_)
        previewDisplayedValid_ = false;

    if (!previewTex_ || !previewDisplayedValid_)
        return; // nothing ready to show yet

    const float pad = 3.0f;
    const float gap = 6.0f; // clearance above the hover indicator triangle
    const float halfW = previewTexW_ * 0.5f;
    // Center the thumbnail over the (possibly snapped) hover indicator, not the
    // raw cursor, so it tracks the frame it actually shows.
    float hoverX = (float)frameToX((double)scrubFrame(tlHoverX_));
    float cx = std::clamp(hoverX, headerX_ + halfW,
                          std::max(headerX_ + halfW, headerX_ + contentW_ - halfW));
    SDL_FRect img = { cx - halfW, rulerRect_.y - gap - (float)previewTexH_,
                      (float)previewTexW_, (float)previewTexH_ };
    // Keep the panel from riding up past the top of the player stage.
    float minY = playerRect_.y + pad;
    if (img.y < minY) img.y = minY;

    SDL_FRect bg = { img.x - pad, img.y - pad, img.w + pad * 2, img.h + pad * 2 };
    SDL_SetRenderDrawColor(renderer_, 20, 21, 24, 235);
    jplay::fillRect(renderer_, &bg);
    SDL_SetRenderDrawColor(renderer_, 90, 92, 100, 255);
    jplay::drawRect(renderer_, &bg);
    SDL_RenderTexture(renderer_, previewTex_, nullptr, &img);
}

// ---------------------------------------------------------------- media add
// Adding media to the timeline (single file, dropped folder, placed clip), the
// default-sequence bootstrap, ripple-make-room, shot<->clip position sync, and
// the undo position snapshot/restore. Moved from App.cpp.

bool App::addMediaFile(const std::string& path) {
    // Default placement (command-line args, fallback): auto-pick a track of the
    // file's kind (or an empty one), appended at that track's end.
    bool audio = isAudioPath(path);
    int track = firstTrackForKind(audio);
    return addMediaFileAt(path, track, timeline_.trackEnd(track)) >= 0;
}

// Add every supported item in a dropped folder (top level only), laid end-to-end
// from the current drop insertion point in natural filename order.
void App::addMediaFolder(const std::string& dir) {
    std::vector<std::string> media = MediaScan::collectDirectoryMedia(dir);
    if (media.empty()) {
        setStatus("NO MEDIA IN " + fileLabel(dir), 5000);
        return;
    }
    int added = 0;
    for (const std::string& path : media) {
        // Bulk folder add: skip per-file audio queries (interactive/command-line
        // single adds get the query; see addMediaFileAt).
        int64_t end = addMediaFileAt(path, dropInsertTrack_, dropInsertFrame_, /*queryAudio=*/false);
        if (end >= 0) {
            dropInsertFrame_ = end; // advance for the next item
            ++added;
        }
    }
    setStatus("ADDED " + std::to_string(added) + " ITEM(S) FROM " + fileLabel(dir));
}

// A text drop rather than a file one: dragging a row out of a web app hands over
// a URL naming that row, not a path on disk, so nothing here can resolve it. The
// site's resolve_drop_text callback is asked instead and answers with the media
// to bring in (see jplayResolveDropText); a payload no site claims is dropped on
// the floor, exactly as before this understood text drops at all.
void App::onDropText(const std::string& text, float x, float y) {
    if (!jplayPythonReady())
        return;
    // The payload is the only record of what was dragged in, and it is far too
    // long for the progress dialog, so it goes to the log instead.
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "[drop] text payload: %s", text.c_str());
    resolveDropTarget(x, y, dropInsertTrack_, dropInsertFrame_);

    // What the media would be decoded as, so the callback can hand back the
    // representation already on screen instead of one we would swap on arrival.
    const std::string mode = proxyEnabled_ ? timeline_.proxyMode : std::string();

    struct DropQuery {
        DropResolution res;
        bool claimed = false;
        bool cancelled = false;
    };
    auto q = std::make_shared<DropQuery>();

    beginProgress("Resolving drop", [text, mode, q](ProgressReporter& pr) {
        pr.update(-1.f, "Looking up dropped item");
        q->claimed = jplayResolveDropText(text, mode, q->res, &pr);
        // The callback reports progress per batch of versions and stops at the
        // first one it starts after Cancel, so what came back is a partial answer
        // to a question the user withdrew -- not media to add.
        q->cancelled = pr.cancelled();
    }, [this, q] {
        if (q->cancelled) {
            setStatus("DROP CANCELLED");
            return;
        }
        if (!q->claimed)
            return; // not ours: no status line, the drop simply did nothing
        if (q->res.paths.empty()) {
            setStatusWarn("DROP RESOLVED TO NO MEDIA");
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "[drop] resolve_drop_text claimed the payload but resolved no media");
            return;
        }
        // A named answer is a cut in its own right, so it
        // gets a fresh sequence and starts at frame 0 on an auto-picked track
        // rather than landing wherever the cursor happened to be.
        const bool asSequence = !q->res.sequence.empty();
        if (asSequence) {
            addSequence(); // creates it, makes it active and scopes the view to it
            activeSequence().name = q->res.sequence;
            dropInsertTrack_ = -1;
            dropInsertFrame_ = 0;
        }
        int added = 0;
        for (const std::string& path : q->res.paths) {
            // Bulk add: skip the per-file audio query, like addMediaFolder.
            int64_t end = addMediaFileAt(path, dropInsertTrack_, dropInsertFrame_,
                                         /*queryAudio=*/false);
            if (end >= 0) {
                dropInsertFrame_ = end; // advance for the next item
                ++added;
            }
        }
        if (added == 0) {
            setStatusWarn("COULD NOT OPEN ANY OF " +
                          std::to_string(q->res.paths.size()) + " DROPPED ITEM(S)", 8000);
            return;
        }
        if (asSequence) {
            fitScope(); // the new sequence, not the whole concatenated timeline
            setStatus("ADDED " + std::to_string(added) + " ITEM(S) AS \"" +
                      q->res.sequence + "\"");
        } else {
            setStatus("ADDED " + std::to_string(added) + " ITEM(S)");
        }
    });
}

// Shift clips on a track to the right so [start, start+duration) is free,
// pushing the clip under the insertion point (and rippling any that follow).
Sequence& App::ensureDefaultSequence() {
    if (timeline_.sequences.empty()) {
        Sequence seq;
        seq.id   = nextSeqId_++;
        seq.name = "Default Sequence";
        timeline_.sequences.push_back(std::move(seq));
        activeSequenceIdx_ = 0;
    }
    return timeline_.sequences[activeSequenceIdx_];
}

// Push the gathered clips forward so none overlaps [start, start+duration),
// cascading the displacement down the track. Shared by the global and
// per-sequence ripple entry points.
static void rippleClips(std::vector<Clip*>& on, int64_t start, int64_t duration) {
    std::sort(on.begin(), on.end(),
              [](const Clip* a, const Clip* b) { return a->timelineStart < b->timelineStart; });
    int64_t frontier = start + duration;
    for (Clip* clip : on) {
        if (clip->end() <= start)
            continue;
        if (clip->timelineStart < frontier)
            clip->timelineStart = frontier;
        frontier = clip->end();
    }
}

void App::rippleMakeRoomInSeq(Sequence& s, int track, int64_t start, int64_t duration, int excludeId) {
    std::vector<Clip*> on;
    for (auto& c : s.clips)
        if (c.track == track && c.id != excludeId) on.push_back(&c);
    rippleClips(on, start, duration);
}

void App::applyRippleDrop(int track, int64_t start, int64_t duration, const std::vector<int>& excludeIds) {
    auto excluded = [&](int id) {
        return std::find(excludeIds.begin(), excludeIds.end(), id) != excludeIds.end();
    };
    // Push every non-dragged clip at or after the drop start forward so the span
    // is clear. Clips earlier than the drop stay put; nothing is trimmed or
    // removed. Shots follow their clips via syncShotsToClips after the drop.
    std::vector<Clip*> on;
    timeline_.forEachClipMut([&](Clip& c) {
        if (c.track == track && !excluded(c.id) && c.timelineStart >= start)
            on.push_back(&c);
    });
    rippleClips(on, start, duration);
}

void App::applyOverwriteDrop(int track, int64_t start, int64_t duration,
                             const std::vector<int>& excludeIds) {
    const int64_t S = start, E = start + duration;
    auto excluded = [&](int id) {
        return std::find(excludeIds.begin(), excludeIds.end(), id) != excludeIds.end();
    };
    // Remove a shot everywhere it is referenced (the global pool and every
    // sequence's ordered shotIds list).
    auto dropShot = [&](int shotId) {
        if (shotId < 0) return;
        timeline_.shots.erase(
            std::remove_if(timeline_.shots.begin(), timeline_.shots.end(),
                           [shotId](const Shot& s) { return s.id == shotId; }),
            timeline_.shots.end());
        for (auto& seq : timeline_.sequences)
            seq.shotIds.erase(std::remove(seq.shotIds.begin(), seq.shotIds.end(), shotId),
                              seq.shotIds.end());
    };
    // Trim, split or remove every non-dragged clip the span overlaps; nothing
    // ripples. Collect removals and split-off tails by value so the vector edits
    // below can't invalidate pointers we hold during iteration.
    std::vector<int> removeIds, removeShotIds;
    std::vector<std::pair<int, Clip>> newTails; // (head clip id -> tail clip to add)
    timeline_.forEachClipMut([&](Clip& c) {
        if (c.track != track || excluded(c.id))
            return;
        if (c.end() <= S || c.timelineStart >= E)
            return; // no overlap with the drop span
        if (c.timelineStart < S && c.end() > E) {
            // Clip spans the whole drop: split it. Keep the head up to
            // S; hand the tail from E onward to a new clip (its own shot dropped,
            // like a duplicate). Build the tail from c's original range first.
            Clip tail = c;
            tail.id = nextClipId_++;
            int64_t adv = E - c.timelineStart;
            tail.sourceOffset += adv;
            tail.duration      = c.end() - E;
            tail.timelineStart = E;
            tail.shotId        = -1;
            newTails.push_back({ c.id, tail });
            c.duration = S - c.timelineStart;      // head keeps [start, S)
        } else if (c.timelineStart < S) {
            c.duration = S - c.timelineStart;      // earlier/straddling: trim its out-point
        } else if (c.end() <= E) {
            removeIds.push_back(c.id);             // wholly covered: remove clip + its shot
            removeShotIds.push_back(c.shotId);
            return;
        } else {
            int64_t adv = E - c.timelineStart;     // head overlap: advance the in-point
            c.sourceOffset += adv;
            c.duration     -= adv;
            c.timelineStart = E;
        }
        // Shrink the surviving clip's shot bar to match (start is realigned by
        // syncShotsToClips; the cut in/out source range is left intact).
        if (c.shotId >= 0)
            if (Shot* sh = timeline_.findShotById(c.shotId)) {
                sh->timelineStart = c.timelineStart;
                sh->duration = c.duration;
            }
    });
    for (int id : removeIds)
        if (Sequence* s = timeline_.sequenceOfClipMut(id)) {
            auto it = std::find_if(s->clips.begin(), s->clips.end(),
                                   [id](const Clip& c) { return c.id == id; });
            if (it != s->clips.end()) s->clips.erase(it);
        }
    for (int sid : removeShotIds) dropShot(sid);
    // Add the split-off tails last: the head clip they were carved from still
    // exists, so it locates the owning sequence.
    for (auto& t : newTails)
        if (Sequence* s = timeline_.sequenceOfClipMut(t.first))
            s->clips.push_back(t.second);
}

void App::applyDrop(int track, int64_t start, int64_t duration, const std::vector<int>& excludeIds) {
    if (dragDropMode_ == DropMode::Ripple)
        applyRippleDrop(track, start, duration, excludeIds);
    else
        applyOverwriteDrop(track, start, duration, excludeIds);
}

void App::syncShotsToClips() {
    // Each clip owns one shot whose bar mirrors the clip's timeline position.
    // After a clip moves (or ripples a neighbour) realign the shot's start;
    // duration and cut in/out are deliberately untouched.
    timeline_.forEachClipMut([&](Clip& c) {
        if (c.shotId < 0) return;
        if (Shot* shot = timeline_.findShotById(c.shotId))
            shot->timelineStart = c.timelineStart;
    });
}

App::PositionSnapshot App::capturePositions() const {
    PositionSnapshot s;
    timeline_.forEachClip([&](const Clip& c) { s.clips.push_back({ c.id, c.track, c.timelineStart }); });
    for (const auto& sh : timeline_.shots) s.shots.push_back({ sh.id, sh.timelineStart });
    for (const auto& seq : timeline_.sequences) s.transitions.push_back(seq.transitions);
    return s;
}

void App::restorePositions(const PositionSnapshot& s) {
    for (const auto& cp : s.clips)
        if (Clip* clip = clipById(cp.id)) { clip->track = cp.track; clip->timelineStart = cp.start; }
    for (const auto& sp : s.shots)
        if (Shot* sh = timeline_.findShotById(sp.id)) sh->timelineStart = sp.start;
    // Only when the sequence list still lines up; edits that add or remove
    // sequences use a ContentSnapshot, which carries transitions of its own.
    if (s.transitions.size() == timeline_.sequences.size())
        for (size_t i = 0; i < timeline_.sequences.size(); ++i)
            timeline_.sequences[i].transitions = s.transitions[i];
    timeline_.clampPlayhead();
}

std::vector<App::ClipGeom> App::captureClipGeom(const std::vector<int>& ids) const {
    std::vector<ClipGeom> g;
    for (int id : ids)
        if (const Clip* clip = timeline_.findClipById(id))
            g.push_back({ clip->id, clip->timelineStart, clip->duration, clip->sourceOffset, clip->linkOffset });
    return g;
}

void App::restoreClipGeom(const std::vector<ClipGeom>& g) {
    for (const auto& e : g)
        if (Clip* clip = clipById(e.id)) {
            clip->timelineStart = e.start;
            clip->duration      = e.duration;
            clip->sourceOffset  = e.sourceOffset;
            clip->linkOffset    = e.linkOffset;
        }
}

App::ContentSnapshot App::captureContent() const {
    ContentSnapshot s;
    s.sequences  = timeline_.sequences;
    s.shots      = timeline_.shots;
    s.trackCount = timeline_.trackCount;
    s.trackNames = timeline_.trackNames;
    s.disabledTracks = timeline_.disabledTracks;
    return s;
}

void App::restoreContent(const ContentSnapshot& s) {
    timeline_.sequences  = s.sequences;
    // A snapshot taken while a scratch view was up holds its sequence too. Undoing
    // back into a view that is still open must keep it — that is where the edit
    // being undone happened — but one taken during a view that has since closed
    // would otherwise put a dead scratch sequence back into the timeline, where
    // nothing would ever drop it again (see dropScratchView).
    timeline_.sequences.erase(
        std::remove_if(timeline_.sequences.begin(), timeline_.sequences.end(),
                       [this](const Sequence& q) {
                           return q.temporary && q.id != scratchSeqId_;
                       }),
        timeline_.sequences.end());
    timeline_.shots      = s.shots;
    timeline_.trackCount = s.trackCount;
    timeline_.trackNames = s.trackNames;
    timeline_.disabledTracks = s.disabledTracks;
    timeline_.clampPlayhead();
    // Clips the restore removed cannot stay selected.
    selectedClipIds_.erase(std::remove_if(selectedClipIds_.begin(), selectedClipIds_.end(),
                                          [this](int id) { return clipById(id) == nullptr; }),
                           selectedClipIds_.end());
    selectedClipId_ = selectedClipIds_.empty() ? -1 : selectedClipIds_.back();
    pruneTransitions();        // also drops a selectedTransitionId_ the restore removed
    infoClipId_ = -2;          // force the info overlay to rebuild
    hostSnapshotDirty_ = true; // re-push the project to spectators if hosting
}

void App::pushContentUndo(const std::string& label, const ContentSnapshot& before) {
    ContentSnapshot after = captureContent();
    undoStack_.push({
        label,
        [this, before] { restoreContent(before); },
        [this, after]  { restoreContent(after); },
    });
}

int App::firstTrackForKind(bool audio) const {
    int n = std::max(trackCount(), 1);
    Timeline::TrackKind want = audio ? Timeline::TrackKind::Audio : Timeline::TrackKind::Video;
    for (int t = 0; t < n; ++t)
        if (timeline_.trackKind(t) == want)
            return t;
    for (int t = 0; t < n; ++t)
        if (timeline_.trackKind(t) == Timeline::TrackKind::Empty)
            return t;
    return n - 1;
}

ClipType App::mediaTypeForPath(const std::string& path) {
    return isAudioPath(path) ? ClipType::Audio
         : ImageSeq::isSequencePath(path) ? ClipType::ImageSequence : ClipType::Video;
}

std::shared_ptr<Media> App::ensureMedia(const std::string& path, ClipType type) {
    if (auto existing = timeline_.findMediaByPath(type, path))
        return existing;

    auto media = std::make_shared<Media>(type, path);
    std::string err;
    if (type == ClipType::Audio) {
        // No frame decoder: probe the audio stream directly for its duration
        // and turn it into a frame count at the project fps.
        auto asrc = AudioSource::open(path, err);
        if (!asrc) {
            setStatus("FAILED TO OPEN " + fileLabel(path) + ": " + err, 5000);
            return nullptr;
        }
        double fps = timeline_.fps > 0.0 ? timeline_.fps : 24.0;
        MediaInfo ai;
        ai.frameCount = std::max<int64_t>((int64_t)std::llround(asrc->durationSeconds() * fps), 1);
        ai.freshHash = Media::computeFreshHash(type, path);
        media->setInfo(ai);
    } else if (!media->ensureOpen(err)) {
        setStatus("FAILED TO OPEN " + fileLabel(path) + ": " + err, 5000);
        return nullptr;
    } else {
        media->refreshMetadata();
    }
    {
        // Use the resolved concrete frame (not a "####"/"%04d" pattern path),
        // so naming-convention parsing sees a real frame number.
        std::map<std::string, std::string> vals;
        // A command-line source is added from init(), before the interpreter is up,
        // so the query has nothing to run on: defer it to the deferred pass in run()
        // rather than leave the clip's metadata label empty for the session.
        if (!jplayGetPathValues(media->resolvedPath(), vals) && !jplayPythonReady())
            pendingPathValueTag_ = true;
        for (auto& kv : vals)
            media->setMetaValue(kv.first, std::move(kv.second));
    }
    timeline_.media[media->id()] = media;
    return media;
}

int64_t App::addMediaFileAt(const std::string& path, int track, int64_t start, bool queryAudio,
                            int64_t srcIn, int64_t srcDur, bool keepView) {
    ClipType type = mediaTypeForPath(path);
    const bool isAudio = (type == ClipType::Audio);

    // Resolve the target track up front so a wrong-type drop is rejected before
    // the media is even opened. A negative track auto-picks a suitable row; an
    // explicit track must be empty or already of the file's kind.
    const int n = std::max(trackCount(), 1);
    if (track < 0) {
        track = firstTrackForKind(isAudio);
    } else {
        track = std::clamp(track, 0, n - 1);
        Timeline::TrackKind want = isAudio ? Timeline::TrackKind::Audio : Timeline::TrackKind::Video;
        Timeline::TrackKind k = timeline_.trackKind(track);
        if (k != Timeline::TrackKind::Empty && k != want) {
            setStatus(isAudio ? "AUDIO CLIPS NEED AN AUDIO OR EMPTY TRACK"
                              : "VIDEO CLIPS NEED A VIDEO OR EMPTY TRACK", 4000);
            return -1;
        }
    }

    auto media = ensureMedia(path, type);
    if (!media)
        return -1;
    MediaInfo info = media->info();
    int64_t frameCount = std::max<int64_t>(info.frameCount, 1);

    // Caller-pinned source range (an aligned Clip Source drop inherits the
    // in/out of the clip it lands under), clamped to what this source has: a
    // shorter version yields a shorter clip rather than an out-of-range one.
    int64_t sourceOffset = 0;
    if (srcIn >= 0 && srcDur > 0) {
        sourceOffset = std::clamp<int64_t>(srcIn, 0, frameCount - 1);
        frameCount = std::clamp<int64_t>(srcDur, 1, frameCount - sourceOffset);
    }

    bool wasEmpty = !timeline_.hasClips();
    ensureDefaultSequence();
    start = std::max<int64_t>(start, 0);

    // Route the new clip to a sequence: the filtered one when viewing a single
    // sequence, else the sequence whose region the drop landed in (All view).
    int targetIdx = -1;
    if (int fsi = filteredSeqIdx(); fsi >= 0)
        targetIdx = fsi;
    else if (int si = timeline_.seqIndexAtFrame(start); si >= 0)
        targetIdx = si;
    if (targetIdx < 0)
        targetIdx = activeSequenceIdx_; // ensureDefaultSequence guarantees one exists
    Sequence& target = timeline_.sequences[targetIdx];

    // Confine placement to the target sequence's region: clamp the drop frame so
    // it can't land in (and ripple) another sequence. An empty sequence is a
    // zero-width point at its packed location, so the clip starts exactly there.
    auto regs = timeline_.seqRegions();
    // The first clip of an empty sequence is pinned to that sequence's start (its
    // own frame 0) rather than to wherever the cursor was: a drop to the right of
    // the insert point would otherwise open a leading gap inside the sequence.
    start = target.clips.empty() ? regs[targetIdx].start
                                 : std::max<int64_t>(start, regs[targetIdx].start);

    // Make room within the target sequence only, so the drop never opens a gap in
    // a neighbouring sequence.
    rippleMakeRoomInSeq(target, track, start, frameCount);

    Clip clip;
    clip.id          = nextClipId_++;
    clip.mediaId     = media->id();
    clip.track       = track;
    clip.audio       = isAudio;
    clip.timelineStart = start;
    clip.duration    = frameCount;
    clip.sourceOffset = sourceOffset;
    const int clipId = clip.id;
    target.clips.push_back(std::move(clip));

    // Offer to pair aligned audio with a freshly added image sequence: the naming
    // config's query_audio callback returns an audio path + source-in offset, or
    // None to skip. The audio clip mirrors this clip's timeline in/out range.
    if (queryAudio && attachAudioToSeq_ && type == ClipType::ImageSequence) {
        std::string audioPath;
        int64_t audioOffset = 0;
        if (jplayQueryAudio(media->resolvedPath(), audioPath, audioOffset))
            addPairedAudio(target, track, clipId, start, frameCount, audioPath, audioOffset);
    }

    resyncLinkedClips(); // the ripple above moved one track only
    timeline_.repackSequences();
    // The ripple above pushes the clips after the insert point along, so a clip
    // dropped across a cut parts the pair a dissolve was anchored to.
    pruneTransitions();

    if (wasEmpty && info.fps > 0.0)
        timeline_.fps = info.fps;

    if (contentW_ <= 0.0f) {
        // Layout isn't sized yet (command-line media added during init, before the
        // first computeLayout). Defer the fit: leaving the view uninitialised lets
        // computeLayout fit once the content width is known.
        viewInitialized_ = false;
    } else if (wasEmpty) {
        // Initial media: fit the zoom to exactly this clip, even if it was dropped
        // at a non-zero frame (so we don't leave empty space before it).
        fitRange(start, start + frameCount);
        viewInitialized_ = true;
    } else if (keepView) {
        // The caller aimed this drop at a spot on screen: leave the user's zoom and
        // pan exactly where they were.
    } else if (!viewAll()) {
        // Keep the view confined to the current scope (a sequence or a project);
        // only the All view fits the whole concatenated timeline. Without this a
        // drop into a scoped sequence would zoom out to the full project and the
        // ruler's scope-relative "0" would no longer line up with viewStart_.
        int64_t a = 0, b = 0;
        if (viewSpan(a, b)) {
            fitRange(a, b);
            viewInitialized_ = true;
        } else {
            fitView();
        }
    } else {
        fitView();
    }
    setStatus("ADDED " + fileLabel(path) + " (" + std::to_string(frameCount) + " FRAMES)");
    reinitOcioForFirstSource(); // first added source may resolve a preferences-based config
    hostSnapshotDirty_ = true; // re-push the project to spectators if hosting
    return start + frameCount;
}

void App::addPairedAudio(Sequence& seq, int videoTrack, int videoClipId,
                         int64_t start, int64_t dur,
                         const std::string& audioPath, int64_t sourceOffset,
                         double probedDurationSec) {
    // Load (or reuse) the audio media, probing its stream for a frame count the
    // same way the audio branch of addMediaFileAt does.
    auto media = timeline_.findMediaByPath(ClipType::Audio, audioPath);
    if (!media) {
        double durationSec = probedDurationSec;
        if (durationSec < 0.0) {
            // Single-clip path: no off-thread probe available, open here.
            std::string err;
            auto asrc = AudioSource::open(audioPath, err);
            if (!asrc) {
                setStatus("PAIRED AUDIO FAILED: " + fileLabel(audioPath) + ": " + err, 5000);
                return;
            }
            durationSec = asrc->durationSeconds();
        }
        media = std::make_shared<Media>(ClipType::Audio, audioPath);
        double fps = timeline_.fps > 0.0 ? timeline_.fps : 24.0;
        MediaInfo ai;
        ai.frameCount = std::max<int64_t>((int64_t)std::llround(durationSec * fps), 1);
        ai.freshHash = Media::computeFreshHash(ClipType::Audio, audioPath);
        media->setInfo(ai);
        timeline_.media[media->id()] = media;
    }

    // Resolve the audio track: prefer the row directly below the EXR clip, else
    // the first free audio track, else a fresh audio track at the bottom.
    auto usable = [&](int t) {
        Timeline::TrackKind k = timeline_.trackKind(t);
        return (k == Timeline::TrackKind::Empty || k == Timeline::TrackKind::Audio)
            && !timeline_.trackHasOverlap(t, start, dur, -1);
    };
    int audioTrack = videoTrack + 1;
    if (audioTrack >= timeline_.trackCount)
        timeline_.trackCount = audioTrack + 1; // grow so the row directly below exists
    if (!usable(audioTrack)) {
        audioTrack = -1;
        for (int t = 0; t < timeline_.trackCount; ++t)
            if (timeline_.trackKind(t) == Timeline::TrackKind::Audio
                && !timeline_.trackHasOverlap(t, start, dur, -1)) {
                audioTrack = t;
                break;
            }
        if (audioTrack < 0) {
            audioTrack = timeline_.trackCount;   // append a fresh audio track
            timeline_.trackCount = audioTrack + 1;
        }
    }

    Clip clip;
    clip.id            = nextClipId_++;
    clip.mediaId       = media->id();
    clip.track         = audioTrack;
    clip.audio         = true;
    clip.timelineStart = start;          // same in/out range as the EXR clip
    clip.duration      = dur;
    clip.sourceOffset  = sourceOffset;   // callback offset: audio source-in, in frames
    clip.linkedTo      = videoClipId;    // follows the picture it was paired with...
    clip.linkOffset    = 0;              // ...starting on the same frame
    seq.clips.push_back(std::move(clip));
}

void App::findAndAttachAudio(int clipId) {
    const Clip* clip = timeline_.findClipById(clipId);
    if (!clip)
        return;
    auto media = timeline_.findMediaById(clip->mediaId);
    if (!media || media->type() != ClipType::ImageSequence)
        return;
    const int owner = timeline_.seqIndexOfClip(clipId);
    if (owner < 0)
        return;

    const int track      = clip->track;
    const int64_t start  = clip->timelineStart;
    const int64_t dur    = clip->duration;

    std::string audioPath;
    int64_t audioOffset = 0;
    if (jplayQueryAudio(media->resolvedPath(), audioPath, audioOffset)) {
        addPairedAudio(timeline_.sequences[owner], track, clipId, start, dur, audioPath, audioOffset);
        timeline_.repackSequences();
        hostSnapshotDirty_ = true; // re-push the project to spectators if hosting
        setStatus("ATTACHED AUDIO " + fileLabel(audioPath));
    } else {
        setStatus("NO AUDIO FOUND FOR " + fileLabel(media->resolvedPath()), 4000);
    }
}

void App::findAndAttachAudioAll(const std::vector<int>* onlyClipIds) {
    // Snapshot the active sequence's clips to consider before attaching, since
    // addPairedAudio mutates seq.clips. Skip any that already has audio under it.
    struct Target { int owner; int track; int clipId; int64_t start; int64_t dur; std::string path; };
    auto targets = std::make_shared<std::vector<Target>>();
    const Sequence& seq = activeSequence();
    for (const Clip& clip : seq.clips) {
        if (clip.audio)
            continue;
        if (onlyClipIds &&
            std::find(onlyClipIds->begin(), onlyClipIds->end(), clip.id) == onlyClipIds->end())
            continue;
        auto media = timeline_.findMediaById(clip.mediaId);
        if (!media || media->type() != ClipType::ImageSequence)
            continue;
        bool hasAudio = false;
        for (const Clip& o : seq.clips)
            if (o.audio && clip.timelineStart < o.end() && clip.end() > o.timelineStart) {
                hasAudio = true;
                break;
            }
        if (hasAudio)
            continue;
        targets->push_back({ activeSequenceIdx_, clip.track, clip.id, clip.timelineStart, clip.duration,
                             media->resolvedPath() });
    }

    if (targets->empty()) {
        if (!onlyClipIds) // stay quiet for the deferred command-line pass
            setStatus("NO CLIPS NEED AUDIO", 4000);
        return;
    }

    // The audio queries call into Python (network lookups), so run them on the
    // progress worker. Each resolved (path, offset) is stashed alongside its
    // target so the completion can build the audio clips on the main thread
    // (addPairedAudio mutates the timeline, which the render loop reads).
    struct Resolved { Target target; std::string audioPath; int64_t audioOffset; double durationSec; };
    auto candidates = std::make_shared<std::vector<Resolved>>(); // derived, pre-probe
    auto resolved   = std::make_shared<std::vector<Resolved>>(); // openable, with duration
    beginProgress("Find and Attach Audio",
        [targets, candidates, resolved](ProgressReporter& pr) {
            // Phase 1: derive candidate audio paths. Pure string work in Python
            // (~microseconds each, no filesystem access), so this stays a serial
            // loop under the GIL.
            candidates->reserve(targets->size());
            for (size_t i = 0; i < targets->size(); ++i) {
                if (pr.cancelled())
                    break;
                const Target& t = (*targets)[i];
                pr.update(0.5f * (float)i / (float)targets->size(),
                          "Resolving audio paths (" + std::to_string(i + 1) + "/" +
                          std::to_string(targets->size()) + ")…");
                std::string audioPath;
                int64_t audioOffset = 0;
                if (jplayDeriveAudio(t.path, audioPath, audioOffset))
                    candidates->push_back({ t, std::move(audioPath), audioOffset, -1.0 });
            }

            // Phase 2: probe the candidates in parallel — AudioSource::open reads
            // the container header (one network round-trip, the expensive part),
            // which both confirms the file exists and yields its duration. Opening
            // here on the worker pool overlaps the waits and keeps them off the
            // main thread, where 166 serial opens froze the UI for ~1.3 s. Each
            // opens its own FFmpeg context, so concurrent opens are safe.
            //
            // A candidate that will not open is handed back to the naming
            // convention with allowScan, which may answer with a near-miss sibling
            // it found on disk (a quicktime published under a different name); that
            // second answer is opened the same way. The scan is the round-trip
            // phase 1 refuses to make, so it happens here, only for the misses, and
            // spread over the same threads.
            const size_t n = candidates->size();
            std::vector<char> ok(n, 0);
            std::atomic<size_t> next{ 0 };
            std::atomic<size_t> done{ 0 };
            const size_t nthreads = std::min<size_t>(n, 16);
            auto worker = [&] {
                for (;;) {
                    if (pr.cancelled())
                        return;
                    const size_t i = next.fetch_add(1, std::memory_order_relaxed);
                    if (i >= n)
                        return;
                    std::string err;
                    auto asrc = AudioSource::open((*candidates)[i].audioPath, err);
                    if (!asrc) {
                        std::string alt;
                        int64_t altOffset = 0;
                        if (jplayDeriveAudio((*candidates)[i].target.path, alt, altOffset,
                                             /*allowScan=*/true) &&
                            alt != (*candidates)[i].audioPath) {
                            asrc = AudioSource::open(alt, err);
                            if (asrc) {
                                (*candidates)[i].audioPath   = std::move(alt);
                                (*candidates)[i].audioOffset = altOffset;
                            }
                        }
                    }
                    if (asrc) {
                        (*candidates)[i].durationSec = asrc->durationSeconds();
                        ok[i] = 1;
                    }
                    done.fetch_add(1, std::memory_order_relaxed);
                }
            };
            std::vector<std::thread> pool;
            pool.reserve(nthreads);
            for (size_t t = 0; t < nthreads; ++t)
                pool.emplace_back(worker);
            while (done.load(std::memory_order_relaxed) < n && !pr.cancelled()) {
                const size_t d = done.load(std::memory_order_relaxed);
                pr.update(0.5f + 0.5f * (float)d / (float)(n ? n : 1),
                          "Probing audio files (" + std::to_string(d) + "/" +
                          std::to_string(n) + ")…");
                std::this_thread::sleep_for(std::chrono::milliseconds(30));
            }
            for (std::thread& th : pool)
                th.join();

            for (size_t i = 0; i < n; ++i)
                if (ok[i])
                    resolved->push_back(std::move((*candidates)[i]));
            pr.update(1.0f);
        },
        [this, resolved] {
            int attached = 0;
            for (const Resolved& r : *resolved) {
                addPairedAudio(timeline_.sequences[r.target.owner], r.target.track,
                               r.target.clipId, r.target.start, r.target.dur,
                               r.audioPath, r.audioOffset, r.durationSec);
                ++attached;
            }
            if (attached > 0) {
                timeline_.repackSequences();
                hostSnapshotDirty_ = true; // re-push the project to spectators if hosting
                setStatus("ATTACHED AUDIO TO " + std::to_string(attached) + " CLIP(S)");
            } else if (progress_.cancelled()) {
                setStatus("FIND AND ATTACH AUDIO: CANCELLED", 3000);
            } else {
                setStatus("NO AUDIO FOUND FOR ANY CLIP", 4000);
            }
        });
}

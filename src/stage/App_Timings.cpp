// Playback Timings translation unit: the floating chart over the frame (toggled
// with 'T') that shows, one stacked bar per frame that reached the screen, where
// that frame spent its time from the file to the display. The samples are
// gathered by renderPlayer (display stages) and the frame-cache workers (read
// stages, see ReadTiming); this file keeps the ring and draws it. A frame shown
// again from the cache on a later lap keeps its read figures, drawn dimmed: they
// describe the frame, not work done for this showing.

#include "App.h"
#include "AppInternal.h"
#include "Layout.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <iterator>
#include <vector>

using namespace jplay;

namespace {

// The stages, bottom of the stack to top, in the fixed categorical order. Checked
// for colour-vision separation and contrast against the dark panel. The last is
// not work: the wait for the previous swap (TimingSample::vsyncMs), in neutral
// grey rather than a hue, and out of the stack until its key is clicked.
constexpr int kStages = 6;
constexpr int kDisplayStage = 3;
constexpr int kVsyncStage = 5;
const SDL_Color kStageColor[kStages] = {
    { 0x39, 0x87, 0xe5, 255 }, // wait IO  - blue
    { 0xd9, 0x59, 0x26, 255 }, // read IO  - orange
    { 0x19, 0x9e, 0x70, 255 }, // decode   - aqua
    { 0xc9, 0x85, 0x00, 255 }, // OCIO     - yellow
    { 0xd5, 0x51, 0x81, 255 }, // output   - magenta
    { 0x6e, 0x72, 0x7e, 255 }, // vsync    - grey
};
const char* const kStageLabel[kStages] = { "Wait IO", "Read IO", "Decode", "OCIO", "Output", "Vsync wait" };
// The display stage's key for a frame shown without the OCIO transform: the same
// span, which is then the composite and the plain upload.
const char* const kPlainDisplayLabel = "Upload";

const SDL_Color kTitleText = { 160, 170, 200, 255 };
const SDL_Color kMutedText = { 130, 135, 148, 255 };
const SDL_Color kValueText = { 215, 218, 225, 255 };
const SDL_Color kGridLine = { 62, 64, 72, 255 };
const SDL_Color kNoteText = { 104, 108, 120, 255 }; // a lane title's explanation
const SDL_Color kRateLine = { 235, 238, 245, 255 };
const SDL_Color kOffRateLine = { 222, 82, 82, 255 }; // the rate line off the project rate
// The capacity and the buffer: neutral ink rather than a hue, so neither
// reads as one of the stages. Identity comes from the lane and the labels.
const SDL_Color kSustainLine = { 160, 168, 190, 255 };

// The legend's line keys after the stages, and each one's widest value.
constexpr int kRates = 3;
const char* const kRateLabel[kRates] = { "Shown", "Capacity", "Ahead" };
const char* const kRateWidest[kRates] = { "000.0, late", "0000 (display)", "10 s+" };

// The Y axis is milliseconds. Its top is snapped to a step of this ladder so it
// does not twitch with every sample (a stall past the last runs off the top), and
// each top has its own round gridline spacing.
const double kScaleMs[] = { 25.0, 50.0, 100.0, 200.0, 400.0, 1000.0, 2000.0, 5000.0, 10000.0 };
const double kGridStepMs[] = { 5.0, 10.0, 20.0, 50.0, 100.0, 200.0, 500.0, 1000.0, 2000.0 };

} // namespace

void App::toggleTimingsPanel() {
    timingsOpen_ = !timingsOpen_;
    // Nothing was timed while it was shut, so what is left in the ring predates a
    // gap it cannot show. Start over.
    if (timingsOpen_)
        clearTimingSamples();
}

void App::clearTimingSamples() {
    timingHead_ = 0;
    timingCount_ = 0;
    timingPendingValid_ = false;
    // Results still in flight are read and dropped; nothing is committed to them.
    for (TimingGpuSlot& slot : timingGpuSlots_)
        slot.committed = false;
    timingLastShownNs_ = 0;
}

namespace {

// The GL entry points the tick timer uses, resolved once. The GL renderer's
// context is 3.2 compatibility; GL_TIME_ELAPSED needs 3.3 or ARB_timer_query.
constexpr unsigned kGlTimeElapsed = 0x88BF;
constexpr unsigned kGlQueryResult = 0x8866;
constexpr unsigned kGlQueryResultAvailable = 0x8867;
struct TickGl {
    void (*genQueries)(int, unsigned*) = nullptr;
    void (*beginQuery)(unsigned, unsigned) = nullptr;
    void (*endQuery)(unsigned) = nullptr;
    void (*getQueryObjectui64v)(unsigned, unsigned, Uint64*) = nullptr;
    void (*getIntegerv)(unsigned, int*) = nullptr;
};
const TickGl& tickGl() {
    static const TickGl gl = [] {
        TickGl g;
        g.genQueries = (void (*)(int, unsigned*))SDL_GL_GetProcAddress("glGenQueries");
        g.beginQuery = (void (*)(unsigned, unsigned))SDL_GL_GetProcAddress("glBeginQuery");
        g.endQuery = (void (*)(unsigned))SDL_GL_GetProcAddress("glEndQuery");
        g.getQueryObjectui64v =
            (void (*)(unsigned, unsigned, Uint64*))SDL_GL_GetProcAddress("glGetQueryObjectui64v");
        g.getIntegerv = (void (*)(unsigned, int*))SDL_GL_GetProcAddress("glGetIntegerv");
        return g;
    }();
    return gl;
}

} // namespace

// Each UI tick's GPU time comes from a GL_TIME_ELAPSED query around it, and the
// review monitor's draw from one on its own context; the tick's GPU time is the
// sum, as both windows' work queues on the GPU. The queries sit in a small ring of
// slots and each result is collected on a later tick once the driver reports it
// available, a tick or two on. Nothing here waits for the GPU: an earlier version
// drained it with glFinish every tick, which took away the frame of lead the CPU
// normally has on it, so any hiccup in the driver or compositor missed a vsync --
// the panel itself made playback drop to 23.4 fps on 24 while it was open. GL path
// only; a review monitor not on OpenGL has no timer.
//
// The tick's CPU clock starts here, after the previous present. Where the driver
// blocks in the present for the swap, that leaves the wait out. Where it returns
// at once and blocks at the next GL call instead, the display path times that
// call and takes it back out of its stage and the UI tick (timingSyncWaitMs).
void App::timingTickBegin() {
    const bool review = reviewActive();
    const bool gl = !hdrPipeline_ && (!review || (mainGlContext_ && reviewGlContext_));
    const TickGl& g = tickGl();
    if (mainGlContext_)
        SDL_GL_MakeCurrent(window_, mainGlContext_);
    if (timingGpuActive_) { // a tick that never reached the present: its result is dropped
        g.endQuery(kGlTimeElapsed);
        timingGpuActive_ = false;
        TimingGpuSlot& slot = timingGpuSlots_[(size_t)timingGpuCur_];
        slot.mainPending = true;
        slot.committed = false;
    }
    timingGpuCur_ = -1;
    timingCollectGpu();
    if (gl && timingGpuSupport_ < 0) {
        int major = 0, minor = 0;
        if (g.getIntegerv) {
            g.getIntegerv(0x821B /*GL_MAJOR_VERSION*/, &major);
            g.getIntegerv(0x821C /*GL_MINOR_VERSION*/, &minor);
        }
        const bool api = g.genQueries && g.beginQuery && g.endQuery && g.getQueryObjectui64v;
        timingGpuSupport_ = api && (major > 3 || (major == 3 && minor >= 3) ||
                                    SDL_GL_ExtensionSupported("GL_ARB_timer_query")) ? 1 : 0;
    }
    if (gl && timingGpuSupport_ == 1) {
        // A free slot: both its results read. With none (the GPU some ticks
        // behind) this tick goes untimed rather than wait.
        for (int i = 0; i < kTimingGpuSlots; ++i) {
            TimingGpuSlot& slot = timingGpuSlots_[(size_t)i];
            if (slot.mainPending || slot.reviewPending)
                continue;
            if (slot.mainQuery == 0)
                g.genQueries(1, &slot.mainQuery);
            if (slot.mainQuery == 0)
                break;
            slot = TimingGpuSlot{ slot.mainQuery, slot.reviewQuery };
            g.beginQuery(kGlTimeElapsed, slot.mainQuery);
            timingGpuActive_ = true;
            timingGpuCur_ = i;
            break;
        }
    }
    timingTickStartNs_ = SDL_GetTicksNS();
}

// Read whatever results have arrived, without waiting for the rest, and give a
// slot whose results are all in to the sample its tick committed.
void App::timingCollectGpu() {
    const TickGl& g = tickGl();
    auto ready = [&](unsigned q, Uint64& ns) {
        Uint64 avail = 0;
        g.getQueryObjectui64v(q, kGlQueryResultAvailable, &avail);
        if (!avail)
            return false;
        g.getQueryObjectui64v(q, kGlQueryResult, &ns);
        return true;
    };
    bool anyReview = false, anyMain = false;
    for (const TimingGpuSlot& slot : timingGpuSlots_) {
        anyReview |= slot.reviewPending;
        anyMain |= slot.mainPending;
    }
    if (anyReview && reviewGlContext_) {
        SDL_GL_MakeCurrent(reviewWindow_, reviewGlContext_);
        for (TimingGpuSlot& slot : timingGpuSlots_) {
            Uint64 ns = 0;
            if (slot.reviewPending && ready(slot.reviewQuery, ns)) {
                slot.ns += ns;
                slot.reviewPending = false;
            }
        }
    }
    if (mainGlContext_)
        SDL_GL_MakeCurrent(window_, mainGlContext_);
    for (TimingGpuSlot& slot : timingGpuSlots_) {
        Uint64 ns = 0;
        if (anyMain && slot.mainPending && ready(slot.mainQuery, ns)) {
            slot.ns += ns;
            slot.mainPending = false;
        }
        if (!slot.committed || slot.mainPending || slot.reviewPending)
            continue;
        slot.committed = false;
        TimingSample& s = timingSamples_[(size_t)slot.sample];
        if (timingCount_ > 0 && s.frame == slot.frame)
            s.gpuMs = (float)slot.ns * 1e-6f;
    }
}

void App::timingTickEnd() {
    if (timingPendingValid_) // less the swap wait, which is idle (see timingSyncWaitMs)
        timingPending_.uiMs = std::max(0.0f, (float)(SDL_GetTicksNS() - timingTickStartNs_) * 1e-6f -
                                                 timingPending_.vsyncMs);
    if (!timingGpuActive_)
        return;
    // SDL holds this tick's draws in its queue until the present; submit them so
    // the query spans them.
    SDL_FlushRenderer(renderer_);
    if (mainGlContext_) // with nothing queued the flush leaves the context as it was
        SDL_GL_MakeCurrent(window_, mainGlContext_);
    tickGl().endQuery(kGlTimeElapsed);
    timingGpuActive_ = false;
    timingGpuSlots_[(size_t)timingGpuCur_].mainPending = true; // committed with its sample
}

// The review monitor's draw, for a frame with a sample pending: its CPU time up to
// the present is charged to the sample's Output stage and UI tick, and its GPU time
// goes to a query on its own context, in the tick's slot. The present is left out:
// with the review monitor holding vsync, that is the wait.
void App::timingReviewBegin() {
    if (!timingPendingValid_)
        return;
    timingReviewStartNs_ = SDL_GetTicksNS();
    if (hdrPipeline_ || !mainGlContext_ || !reviewGlContext_ || timingGpuSupport_ != 1 || timingGpuCur_ < 0)
        return;
    const TickGl& g = tickGl();
    TimingGpuSlot& slot = timingGpuSlots_[(size_t)timingGpuCur_];
    SDL_GL_MakeCurrent(reviewWindow_, reviewGlContext_);
    if (slot.reviewQuery == 0)
        g.genQueries(1, &slot.reviewQuery); // query objects are not shared between contexts
    if (slot.reviewQuery != 0) {
        g.beginQuery(kGlTimeElapsed, slot.reviewQuery);
        timingReviewGpuActive_ = true;
    }
}

void App::timingReviewEnd() {
    if (!timingPendingValid_)
        return;
    // Submit the queued draws, so both the CPU time and the query span them.
    SDL_FlushRenderer(reviewRenderer_);
    if (timingReviewGpuActive_) {
        SDL_GL_MakeCurrent(reviewWindow_, reviewGlContext_);
        tickGl().endQuery(kGlTimeElapsed);
        timingReviewGpuActive_ = false;
        timingGpuSlots_[(size_t)timingGpuCur_].reviewPending = true;
    }
    const float ms = (float)(SDL_GetTicksNS() - timingReviewStartNs_) * 1e-6f;
    timingPending_.outputMs += ms;
    timingPending_.uiMs += ms;
}

// The first GL call of a tick that needs the driver's attention is where, with
// vsync on and a present that returns at once, this thread waits out the previous
// frame's swap: most of a refresh, and no work at all. The display path makes
// that call itself just before its upload (SDL_UpdateTexture would otherwise take
// the wait inside it), so the wait is measured and kept out of the display stage
// and the UI tick (TimingSample::vsyncMs). The OCIO GPU pass measures its own, at
// the same point in its sequence (OcioGpu::lastSyncWaitMs). GL path only.
double App::timingSyncWaitMs() {
    const TickGl& g = tickGl();
    if (hdrPipeline_ || !mainGlContext_ || !g.getIntegerv)
        return 0.0;
    const Uint64 t0 = SDL_GetTicksNS();
    SDL_FlushRenderer(renderer_);
    int fbo = 0;
    g.getIntegerv(0x8CA6 /*GL_FRAMEBUFFER_BINDING*/, &fbo);
    return (double)(SDL_GetTicksNS() - t0) * 1e-6;
}

SDL_GLContext App::glContextOf_(SDL_Renderer* r) {
    // SDL's GL renderer makes the context it creates current, and the caller asks
    // straight after creating r.
    const char* name = r ? SDL_GetRendererName(r) : nullptr;
    return name && std::strcmp(name, "opengl") == 0 ? SDL_GL_GetCurrentContext() : nullptr;
}

// Reads run on the cache's worker pool in parallel with each other and with the UI
// thread, so the read side sustains workers / read time; the display side is
// bounded by the UI tick's CPU or GPU time, whichever is longer, since the two run
// side by side as well. The slower side is the estimate.
//
// A read timed with the pool mostly idle (steady playback reads one frame per
// frame shown) did not share the disk and memory bandwidth; run all workers at
// once, each would be slower than that. So the read side is never costed below
// what the pool has really managed with every worker busy
// (FrameCache::fillRateCeiling), and a read slower than even that shows as itself.
double App::readCostMs(double readMs, double readsPerFrame) {
    const int workers = std::max(1, cache_->threadCount());
    const double ceiling = cache_->fillRateCeiling();
    const double floorMs = ceiling > 0.0 ? readsPerFrame * 1000.0 / ceiling : 0.0;
    return std::max(readMs / workers, floorMs);
}

void App::commitTimingSample() {
    timingPendingValid_ = false;
    TimingSample s = timingPending_;
    const Uint64 now = SDL_GetTicksNS();
    // Only playback has an interval. A step or a scrub a second after the last
    // frame would chart the pause, not the pipeline.
    if (playing_ && timingLastShownNs_ != 0 && now - timingLastShownNs_ < 1000000000ull)
        s.intervalMs = (float)(now - timingLastShownNs_) * 1e-6f;
    timingLastShownNs_ = now;
    if (playing_) {
        s.playing = true;
        s.infoFps = (float)fpsShown_;
        s.aheadSec = (float)timingBufferedAheadSec();
    }
    timingSamples_[(size_t)timingHead_] = s;
    if (timingGpuCur_ >= 0) { // its GPU time arrives a tick or two on (timingCollectGpu)
        TimingGpuSlot& slot = timingGpuSlots_[(size_t)timingGpuCur_];
        slot.committed = true;
        slot.sample = timingHead_;
        slot.frame = s.frame;
    }
    timingHead_ = (timingHead_ + 1) % kTimingSamples;
    timingCount_ = std::min(timingCount_ + 1, kTimingSamples);
}

double App::timingBufferedAheadSec() {
    if (timeline_.fps <= 0.0)
        return -1.0;
    int64_t lo = 0, hi = 0;
    playbackRange(lo, hi);
    if (hi < lo)
        return -1.0;
    // Play order from the frame after the playhead, wrapping the loop range as
    // update() does. A frame with nothing to read (a gap, a missing file) holds
    // nothing up, so it stands in as resident. A whole loop resident is as far
    // ahead as it gets: it never needs another read.
    const int64_t span = hi - lo + 1;
    const int64_t want = std::min<int64_t>(span, (int64_t)std::ceil(kTimingAheadCapSec * timeline_.fps));
    std::vector<CacheKey> keys;
    std::vector<int64_t> at; // each key's place in play order
    keys.reserve((size_t)want);
    at.reserve((size_t)want);
    int64_t f = timeline_.playhead;
    for (int64_t i = 0; i < want; ++i) {
        f = (f + 1 > hi || f + 1 < lo) ? lo : f + 1;
        const Clip* clip = getTopMostClipAtFrame(f);
        if (!clip || clip->mediaId.empty())
            continue;
        auto pm = timeline_.findMediaById(clip->mediaId);
        if (!pm || pm->openFailed())
            continue;
        keys.push_back({ clip->mediaId, clip->sourceOffset + (f - clip->timelineStart) });
        at.push_back(i);
    }
    const int run = cache_->residentRun(keys);
    if (run == (int)keys.size())
        return want == span ? kTimingAheadCapSec : (double)want / timeline_.fps;
    return (double)at[(size_t)run] / timeline_.fps;
}

void App::renderTimingsPanel() {
    if (!timingsOpen_)
        return;
    const SDL_FRect& pr = playerRect_;
    const float margin = 20.0f;
    const float pad = 10.0f * dpiScale;
    const float lineH = textFont_.lineHeight();
    const float closeBtn = 14.0f;
    const double fps = timeline_.fps;
    const int workers = std::max(1, cache_->threadCount());
    // Contracted, a fixed size in the top-right corner; expanded, the whole frame
    // at the same margin. Anchored top-right either way, so the two buttons stay put.
    const float fullW = pr.w - margin * 2.0f;
    const float fullH = pr.h - margin * 2.0f;
    // Contracted, at least as wide as the legend row at its widest (every value at
    // its widest), so the whole row fits and the panel does not breathe with it.
    const float swatchW = std::round(lineH * 0.6f);
    const float valueW = textFont_.measure(renderer_, "000.0 ms");
    float legendW = 0.0f;
    for (int k = 0; k < kStages; ++k) {
        float labelW = textFont_.measure(renderer_, kStageLabel[k]);
        if (k == kDisplayStage)
            labelW = std::max(labelW, textFont_.measure(renderer_, kPlainDisplayLabel));
        legendW += swatchW + 4.0f + labelW + 4.0f + valueW + 12.0f;
    }
    for (int k = 0; k < kRates; ++k)
        legendW += swatchW + 4.0f + textFont_.measure(renderer_, kRateLabel[k]) + 4.0f +
                   textFont_.measure(renderer_, kRateWidest[k]) + 12.0f;
    const float contractedW = std::max(560.0f * dpiScale, legendW + pad * 2.0f);
    const float panelW = timingsExpanded_ ? fullW : std::min(fullW, contractedW);
    const float panelH = timingsExpanded_ ? fullH : std::min(fullH, 400.0f * dpiScale);
    timingsStageKeyRect_ = {}; // set again only if the legend is drawn
    if (panelW < 200.0f || panelH < 200.0f) {
        timingsRect_ = timingsCloseRect_ = timingsExpandRect_ = {};
        return;
    }
    timingsRect_ = { std::round(pr.x + pr.w - margin - panelW), std::round(pr.y + margin),
                     std::round(panelW), std::round(panelH) };

    SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(renderer_, kPanelBg.r, kPanelBg.g, kPanelBg.b, 224);
    jplay::fillRect(renderer_, &timingsRect_);
    SDL_SetRenderDrawColor(renderer_, 80, 82, 90, 220);
    jplay::drawRect(renderer_, &timingsRect_);

    SDL_FRect body = inset(timingsRect_, pad, pad);

    auto stageValues = [](const TimingSample& s, float v[kStages]) {
        v[0] = s.waitIoMs;
        v[1] = s.readIoMs;
        v[2] = s.decodeMs;
        v[3] = s.ocioMs;
        v[4] = s.outputMs;
        v[kVsyncStage] = s.vsyncMs;
    };
    // The work lane's stack: the stages less any toggled out at the legend.
    static_assert(std::tuple_size_v<decltype(timingsStageHidden_)> == kStages);
    auto stageHidden = [&](int k) { return timingsStageHidden_[(size_t)k]; };
    auto shownWorkMs = [&](const TimingSample& s) {
        float v[kStages];
        stageValues(s, v);
        double t = 0.0;
        for (int k = 0; k < kStages; ++k)
            if (!stageHidden(k))
                t += v[k];
        return t;
    };
    // Samples oldest first: the ring's i-th entry counting from the oldest.
    auto sampleAt = [&](int i) -> const TimingSample& {
        const int oldest = (timingHead_ - timingCount_ + kTimingSamples) % kTimingSamples;
        return timingSamples_[(size_t)((oldest + i) % kTimingSamples)];
    };
    // The shown rate at each frame: what the info bar's readout said while it was
    // up (TimingSample::infoFps), so hovering a frame reads what the viewer saw
    // there. A frame late on its refresh or a skip is marked on the line rather
    // than dipping it: its catch-up keeps the rate, which the readout reflects.
    // 0 where there is none (paused, a step, just after Play).
    auto shownFps = [&](const TimingSample& s) -> double { return s.playing ? s.infoFps : 0.0; };
    auto onRate = [&](double v) { return fps <= 0.0 || std::fabs(v - fps) < 0.05; };
    auto missed = [](const TimingSample& s) { return s.lateRefreshes > 0 || s.skippedFrames > 0; };
    // The rate the pipeline could keep up at, as of sample i: the slower of the
    // read side and the display side, averaged over the kTimingRateWindow played
    // samples up to it. The read side sustains workers / read time (see
    // readCostMs), the display side 1 / the UI tick's CPU or GPU time; they run
    // side by side, so they are not added. Frames shown again from the cache read
    // nothing, so the read side is costed per frame shown: the reads there were,
    // over every frame. A per-frame figure would cost every fresh frame as if all
    // of them needed a read, and drop well under a rate playback is holding.
    //
    // A video's reads run one at a time (TimingSample::serial), so they are costed
    // at their own time, with no division by the workers. A window that cuts from
    // a sequence to a video holds both kinds; each is costed per frame shown, and
    // the two added, as they followed each other. A dissolve between two videos is
    // costed as one serial read of both halves, though each has its own worker:
    // the estimate errs low there.
    auto capacityAt = [&](int i, bool& readLimited) -> double {
        double readSum = 0.0, serialSum = 0.0, dispSum = 0.0;
        int n = 0, reads = 0;
        for (int j = i; j >= 0 && j > i - kTimingRateWindow; --j) {
            const TimingSample& s = sampleAt(j);
            if (!s.playing)
                break;
            dispSum += std::max(s.uiMs, s.gpuMs);
            if (s.hasRead && s.fresh) {
                if (s.serial) {
                    serialSum += s.readMs();
                } else {
                    readSum += s.readMs();
                    ++reads;
                }
            }
            ++n;
        }
        if (n == 0)
            return 0.0;
        const double readMs = (reads > 0 ? readCostMs(readSum / n, (double)reads / n) : 0.0) + serialSum / n;
        const double dispMs = dispSum / n;
        readLimited = readMs > dispMs;
        const double cost = std::max(readMs, dispMs);
        return cost > 0.0 ? 1000.0 / cost : 0.0;
    };
    auto aheadText = [&](float sec, char* buf, size_t n) {
        if (sec < 0.0f)
            SDL_snprintf(buf, n, "\xe2\x80\x94");
        else if (sec >= (float)kTimingAheadCapSec - 1e-3f)
            SDL_snprintf(buf, n, "%g s+", kTimingAheadCapSec);
        else
            SDL_snprintf(buf, n, "%.1f s", sec);
    };

    // ---- header ----
    SDL_FRect row = cutTop(body, lineH);
    drawText(row.x, row.y, kTitleText, "PLAYBACK TIMINGS");
    {
        timingsCloseRect_ = { row.x + row.w - closeBtn, row.y + (lineH - closeBtn) * 0.5f,
                              closeBtn, closeBtn };
        float mx = 0.0f, my = 0.0f;
        uiHoverMouse(mx, my);
        auto button = [&](const SDL_FRect& r, uint32_t glyph) {
            const bool hov = inRect(r, mx, my);
            icons_.drawGlyph(renderer_, glyph, r,
                             hov ? SDL_Color{ 235, 238, 245, 255 } : SDL_Color{ 140, 145, 156, 255 });
        };
        button(timingsCloseRect_, iconCp(ICON_MDI_CLOSE));
        timingsExpandRect_ = { timingsCloseRect_.x - 6.0f * dpiScale - closeBtn, timingsCloseRect_.y,
                               closeBtn, closeBtn };
        button(timingsExpandRect_, timingsExpanded_ ? iconCp(ICON_MDI_ARROW_COLLAPSE)
                                                    : iconCp(ICON_MDI_ARROW_EXPAND));
    }
    gapTop(body, 4.0f * dpiScale);

    // ---- summary: shown rate, what the pipeline could sustain, and the buffer ----
    // Capacity as capacityAt has it for the newest sample; headroom is that over
    // the project rate.
    {
        row = cutTop(body, lineH);
        if (timingCount_ == 0) {
            drawText(row.x, row.y, kMutedText, "Waiting for frames...");
        } else {
            const TimingSample& newest = sampleAt(timingCount_ - 1);
            std::string text = "Shown ";
            char buf[160];
            const double shown = shownFps(newest);
            if (shown > 0.0)
                SDL_snprintf(buf, sizeof(buf), "%.1f fps", shown);
            else
                SDL_snprintf(buf, sizeof(buf), "\xe2\x80\x94 fps");
            text += buf;
            buf[0] = '\0';
            // Frames in the chart that missed their refresh, or were passed over:
            // each one is a red mark on the rate line below.
            int late = 0, skipped = 0;
            for (int i = 0; i < timingCount_; ++i) {
                const TimingSample& s = sampleAt(i);
                late += s.lateRefreshes > 0 ? 1 : 0;
                skipped += s.skippedFrames;
            }
            if (late > 0 || skipped > 0)
                SDL_snprintf(buf, sizeof(buf), " (%d late, %d skipped in view)", late, skipped);
            text += buf;
            bool readLimited = false;
            const double est = capacityAt(timingCount_ - 1, readLimited);
            if (est > 0.0) {
                if (fps > 0.0)
                    SDL_snprintf(buf, sizeof(buf), "   Capacity %.0f fps, %.1f\xc3\x97 headroom (%s-limited, %d workers",
                                 est, est / fps, readLimited ? "read" : "display", workers);
                else
                    SDL_snprintf(buf, sizeof(buf), "   Capacity %.0f fps (%s-limited, %d workers",
                                 est, readLimited ? "read" : "display", workers);
                text += buf;
                // The pool's measured ceiling, or that it has not been seen full yet
                // and the read side is the optimistic workers / read time.
                const double ceiling = cache_->fillRateCeiling();
                if (ceiling > 0.0)
                    SDL_snprintf(buf, sizeof(buf), ", fill %.0f fps)", ceiling);
                else
                    SDL_snprintf(buf, sizeof(buf), ", fill not measured)");
                text += buf;
            }
            if (newest.aheadSec >= 0.0f) {
                char a[32];
                aheadText(newest.aheadSec, a, sizeof(a));
                text += std::string("   Buffered ") + a + " ahead";
            }
            drawText(row.x, row.y, kValueText, text);
        }
    }
    gapTop(body, 6.0f * dpiScale);

    // ---- geometry: three lanes over one frame axis ----
    // Rate, work and buffer are three different measures (fps, ms, seconds), so
    // each has its own lane and its own axis rather than sharing one; the frames
    // line up across all three. The legend and the frame numbers go under them.
    SDL_FRect legend = cutBottom(body, lineH);
    gapBottom(body, 4.0f * dpiScale);
    SDL_FRect xLabels = cutBottom(body, lineH);
    const float axisW = textFont_.measure(renderer_, "10000 ms") + 6.0f;
    const float rightW = textFont_.measure(renderer_, "000.0 fps") + 6.0f;
    const float titleH = lineH + 2.0f * dpiScale;
    const float laneGap = 6.0f * dpiScale;
    const float lanesH = body.h - 3.0f * (titleH + lineH * 0.5f) - 2.0f * laneGap;
    if (lanesH < 90.0f || body.w - axisW - rightW < 60.0f)
        return;
    const float cx = body.x + axisW;
    const float cw = body.w - axisW - rightW;
    SDL_FRect lanes[3];
    const char* const laneTitle[3] = { "FRAME RATE", "WORK PER FRAME", "BUFFERED AHEAD" };
    char laneNote[3][128];
    SDL_snprintf(laneNote[0], sizeof(laneNote[0]),
                 "playback speed vs. what loading could sustain");
    // A video loads one frame at a time (TimingSample::serial): there a bar past
    // the frame interval is a frame that cannot keep up.
    if (timingCount_ > 0 && sampleAt(timingCount_ - 1).serial)
        SDL_snprintf(laneNote[1], sizeof(laneNote[1]),
                     "time to load each frame; a video loads one at a time");
    else
        SDL_snprintf(laneNote[1], sizeof(laneNote[1]),
                     "time to load each frame; %d load at once, so tall bars are not late frames",
                     workers);
    SDL_snprintf(laneNote[2], sizeof(laneNote[2]),
                 "frames loaded and waiting; at zero playback stalls");
    {
        const float share[3] = { 0.36f, 0.42f, 0.22f };
        float y = body.y;
        for (int l = 0; l < 3; ++l) {
            const float tx = body.x;
            drawText(tx, y, kMutedText, laneTitle[l]);
            const float nx = tx + textFont_.measure(renderer_, laneTitle[l]) + 8.0f;
            if (nx + textFont_.measure(renderer_, laneNote[l]) <= body.x + body.w)
                drawText(nx, y, kNoteText, laneNote[l]);
            y += titleH + lineH * 0.5f; // the top gridline's label sits half above it
            const float h = std::round(lanesH * share[l]);
            lanes[l] = { cx, y, cw, h };
            y += h + laneGap;
        }
    }
    const SDL_FRect& rateLane = lanes[0];
    const SDL_FRect& workLane = lanes[1];
    const SDL_FRect& aheadLane = lanes[2];

    // A fixed number of slots, filled from the right: the newest sample is always
    // at the right edge and the history scrolls left.
    const float slotW = cw / (float)kTimingSamples;
    const float barW = std::max(1.0f, slotW - (slotW >= 3.0f ? 1.0f : 0.0f));
    const float x0 = cx + cw - slotW * (float)timingCount_;
    auto slotMid = [&](int i) { return x0 + slotW * ((float)i + 0.5f); };

    // The sample read out below: the one under the cursor, else the newest. The
    // legend and every lane's right-hand label follow it.
    int shownIdx = timingCount_ - 1;
    bool hovering = false;
    if (!playing_) {
        float mx = 0.0f, my = 0.0f;
        uiHoverMouse(mx, my);
        if (timingCount_ > 0 && mx >= x0 && mx < cx + cw && my >= rateLane.y &&
            my < aheadLane.y + aheadLane.h) {
            shownIdx = std::clamp((int)((mx - x0) / slotW), 0, timingCount_ - 1);
            hovering = true;
        }
    }
    const TimingSample* readout = timingCount_ > 0 ? &sampleAt(shownIdx) : nullptr;

    auto gridLine = [&](const SDL_FRect& lane, float y, const char* label) {
        SDL_SetRenderDrawColor(renderer_, kGridLine.r, kGridLine.g, kGridLine.b, 255);
        SDL_RenderLine(renderer_, lane.x, y, lane.x + lane.w, y);
        if (label)
            drawText(body.x, y - lineH * 0.5f, kMutedText, label);
    };
    // A 2 px polyline through per-frame points, broken wherever a frame has none
    // and recoloured where `colorAt` changes, carrying the last point across so
    // the line stays joined.
    auto drawSeries = [&](auto valueAt, auto yOf, auto colorAt) {
        std::vector<SDL_FPoint> run;
        run.reserve((size_t)timingCount_);
        SDL_Color runColor{};
        auto flush = [&] {
            if (run.size() < 2)
                return;
            SDL_SetRenderDrawColor(renderer_, runColor.r, runColor.g, runColor.b, runColor.a);
            SDL_RenderLines(renderer_, run.data(), (int)run.size());
            for (SDL_FPoint& p : run)
                p.y -= 1.0f;
            SDL_RenderLines(renderer_, run.data(), (int)run.size());
            for (SDL_FPoint& p : run)
                p.y += 1.0f;
        };
        for (int i = 0; i < timingCount_; ++i) {
            const double v = valueAt(sampleAt(i));
            if (v <= 0.0) {
                flush();
                run.clear();
                continue;
            }
            const SDL_FPoint p = { slotMid(i), std::round(yOf(v)) };
            const SDL_Color c = colorAt(v);
            if (!run.empty() && (c.r != runColor.r || c.g != runColor.g || c.b != runColor.b)) {
                flush();
                const SDL_FPoint last = run.back();
                run.clear();
                run.push_back(last);
            }
            runColor = c;
            run.push_back(p);
        }
        flush();
    };
    // A lane's right-hand readout, kept inside the lane and clear of `avoidY`.
    auto rightLabel = [&](const SDL_FRect& lane, float y, SDL_Color c, const char* t, float avoidY = -1e9f) {
        float ty = y - lineH * 0.5f;
        if (std::fabs(ty - (avoidY - lineH * 0.5f)) < lineH)
            ty = avoidY - lineH * 1.5f;
        ty = std::clamp(ty, lane.y - lineH * 0.5f, lane.y + lane.h - lineH * 0.5f);
        drawText(lane.x + lane.w + 6.0f, ty, c, t);
    };

    // ---- lane 1: frame rate ----
    // In multiples of the project rate on a doubling scale, from half of it (a
    // frame held for two reads there) up past the highest capacity, so the
    // project rate is one fixed gridline and headroom reads off as 2x, 4x, 8x.
    // The shown rate sits on 1x, white while it holds the project's, red where it
    // does not; a frame that missed its refresh or was skipped is a red dot on it.
    // The capacity runs dashed beside it: an estimate, not something that played,
    // so it is muted, and falling under 1x is not marked as a fault on its own.
    std::vector<double> capacity((size_t)timingCount_, 0.0);
    std::vector<char> capacityRead((size_t)timingCount_, 0);
    for (int i = 0; i < timingCount_; ++i) {
        bool r = false;
        capacity[(size_t)i] = capacityAt(i, r);
        capacityRead[(size_t)i] = r ? 1 : 0;
    }
    {
        const SDL_FRect& L = rateLane;
        const float bottom = L.y + L.h;
        double maxMul = 4.0;
        for (int i = 0; i < timingCount_; ++i)
            if (fps > 0.0)
                maxMul = std::max(maxMul, capacity[(size_t)i] / fps);
        const int topPow = std::clamp((int)std::ceil(std::log2(maxMul) - 1e-9), 2, 6);
        auto yOf = [&](double v) {
            const double t = fps > 0.0 ? (std::log2(v / fps) + 1.0) / (topPow + 1.0) : 0.0;
            return bottom - (float)std::clamp(t, 0.0, 1.0) * L.h;
        };
        gridLine(L, std::round(bottom), nullptr);
        for (int p = 0; p <= topPow && fps > 0.0; ++p) {
            char l[16];
            SDL_snprintf(l, sizeof(l), "%d\xc3\x97", 1 << p);
            gridLine(L, std::round(yOf(fps * (1 << p))), l);
        }
        if (fps > 0.0) {
            // Headroom: the band between the project rate and a capacity above it.
            std::vector<SDL_FRect> band;
            for (int i = 0; i < timingCount_; ++i) {
                const double v = capacity[(size_t)i];
                if (v <= fps)
                    continue;
                const float y1 = yOf(fps), y2 = yOf(v);
                band.push_back({ x0 + slotW * (float)i, y2, slotW, y1 - y2 });
            }
            SDL_SetRenderDrawColor(renderer_, kSustainLine.r, kSustainLine.g, kSustainLine.b, 38);
            if (!band.empty())
                SDL_RenderFillRects(renderer_, band.data(), (int)band.size());
            // The capacity, dashed: 6 px on, 4 off along x, broken where a sample
            // has none.
            SDL_SetRenderDrawColor(renderer_, kSustainLine.r, kSustainLine.g, kSustainLine.b, 200);
            for (int i = 1; i < timingCount_; ++i) {
                const double a = capacity[(size_t)i - 1], b = capacity[(size_t)i];
                if (a <= 0.0 || b <= 0.0)
                    continue;
                const float xa = slotMid(i - 1), xb = slotMid(i);
                const float ya = yOf(a), yb = yOf(b);
                for (float x = xa; x < xb; x += 1.0f) {
                    if (std::fmod(x - cx, 10.0f) >= 6.0f)
                        continue;
                    const float xe = std::min(x + 1.0f, xb);
                    const float t0 = (x - xa) / (xb - xa), t1 = (xe - xa) / (xb - xa);
                    SDL_RenderLine(renderer_, x, std::round(ya + (yb - ya) * t0), xe,
                                   std::round(ya + (yb - ya) * t1));
                }
            }
            drawSeries(shownFps, yOf,
                       [&](double v) { return onRate(v) ? kRateLine : kOffRateLine; });
            // A frame that missed its refresh or was skipped: a dot on the line.
            SDL_SetRenderDrawColor(renderer_, kOffRateLine.r, kOffRateLine.g, kOffRateLine.b, 255);
            for (int i = 0; i < timingCount_; ++i) {
                const TimingSample& s = sampleAt(i);
                const double v = shownFps(s);
                if (v <= 0.0 || !missed(s))
                    continue;
                const SDL_FRect dot = { slotMid(i) - 2.5f, std::round(yOf(v)) - 3.0f, 5.0f, 5.0f };
                jplay::fillRect(renderer_, &dot);
            }
        }
        if (readout && fps > 0.0) {
            const double v = shownFps(*readout);
            if (v > 0.0) {
                const float shownY = std::round(yOf(v));
                const SDL_Color& c = onRate(v) && !missed(*readout) ? kRateLine : kOffRateLine;
                if (hovering) { // mark the point being read out
                    SDL_SetRenderDrawColor(renderer_, c.r, c.g, c.b, c.a);
                    const SDL_FRect dot = { slotMid(shownIdx) - 3.0f, shownY - 3.5f, 6.0f, 6.0f };
                    jplay::fillRect(renderer_, &dot);
                }
                char buf[32];
                SDL_snprintf(buf, sizeof(buf), "%.1f fps", v);
                rightLabel(L, shownY, kValueText, buf);
            }
        }
    }

    // ---- lane 2: work per frame ----
    // Where each frame spent its time, stacked, in ms. The top is snapped to a step
    // of the ladder so it does not twitch with every sample (a stall past the last
    // runs off the top), each with its own round gridline spacing.
    {
        const SDL_FRect& L = workLane;
        const float bottom = L.y + L.h;
        double peak = 0.0;
        for (int i = 0; i < timingCount_; ++i) {
            const TimingSample& s = sampleAt(i);
            peak = std::max(peak, shownWorkMs(s));
        }
        size_t scale = std::size(kScaleMs) - 1;
        for (size_t i = 0; i < std::size(kScaleMs); ++i)
            if (peak <= kScaleMs[i]) {
                scale = i;
                break;
            }
        const double yMax = kScaleMs[scale];
        auto yOf = [&](double ms) { return bottom - (float)std::min(ms / yMax, 1.0) * L.h; };
        for (double ms = kGridStepMs[scale]; ms <= yMax + 1e-6; ms += kGridStepMs[scale]) {
            char l[32];
            SDL_snprintf(l, sizeof(l), "%g ms", ms);
            gridLine(L, std::round(yOf(ms)), l);
        }
        gridLine(L, bottom, nullptr);

        // Read stages of a frame shown again from the cache go in their own, dimmed set.
        std::vector<SDL_FRect> rects[kStages], dimRects[kStages];
        for (auto& v : rects)
            v.reserve((size_t)timingCount_);
        for (int i = 0; i < timingCount_; ++i) {
            const TimingSample& s = sampleAt(i);
            float v[kStages];
            stageValues(s, v);
            const float x = x0 + slotW * (float)i;
            double acc = 0.0;
            for (int k = 0; k < kStages; ++k) {
                if (v[k] <= 0.0f || stageHidden(k))
                    continue;
                const float yTop = yOf(acc + v[k]);
                const float yBot = yOf(acc);
                acc += v[k];
                if (yBot - yTop >= 0.5f)
                    (k < 3 && !s.fresh ? dimRects : rects)[k].push_back({ x, yTop, barW, yBot - yTop });
            }
        }
        for (int k = 0; k < kStages; ++k) {
            const SDL_Color& c = kStageColor[k];
            if (!rects[k].empty()) {
                SDL_SetRenderDrawColor(renderer_, c.r, c.g, c.b, 255);
                SDL_RenderFillRects(renderer_, rects[k].data(), (int)rects[k].size());
            }
            if (!dimRects[k].empty()) {
                SDL_SetRenderDrawColor(renderer_, c.r, c.g, c.b, 80);
                SDL_RenderFillRects(renderer_, dimRects[k].data(), (int)dimRects[k].size());
            }
        }
        if (readout) {
            const double total = shownWorkMs(*readout);
            if (total > 0.0) {
                char buf[32];
                SDL_snprintf(buf, sizeof(buf), "%.1f ms", total);
                rightLabel(L, std::round(yOf(total)), kMutedText, buf);
            }
        }
    }

    // ---- lane 3: buffered ahead ----
    // Seconds of playback already in the cache after each frame shown, as an area
    // from zero. Flat along the top when the reads keep well ahead; a run of slow
    // reads shows as it sinking, and a frame is held only where it reaches zero.
    {
        const SDL_FRect& L = aheadLane;
        const float bottom = L.y + L.h;
        const double kAheadScale[] = { 1.0, 2.0, 5.0, kTimingAheadCapSec };
        const double kAheadStep[] = { 0.5, 1.0, 1.0, 5.0 };
        double peak = 0.0;
        for (int i = 0; i < timingCount_; ++i)
            peak = std::max(peak, (double)sampleAt(i).aheadSec);
        size_t scale = std::size(kAheadScale) - 1;
        for (size_t i = 0; i < std::size(kAheadScale); ++i)
            if (peak <= kAheadScale[i] + 1e-6) {
                scale = i;
                break;
            }
        const double yMax = kAheadScale[scale];
        auto yOf = [&](double sec) { return bottom - (float)std::clamp(sec / yMax, 0.0, 1.0) * L.h; };
        for (double sec = kAheadStep[scale]; sec <= yMax + 1e-6; sec += kAheadStep[scale]) {
            char l[32];
            SDL_snprintf(l, sizeof(l), "%g s", sec);
            gridLine(L, std::round(yOf(sec)), l);
        }
        gridLine(L, bottom, nullptr);
        std::vector<SDL_FRect> cols;
        cols.reserve((size_t)timingCount_);
        for (int i = 0; i < timingCount_; ++i) {
            const float a = sampleAt(i).aheadSec;
            if (a <= 0.0f)
                continue;
            const float y = yOf(a);
            cols.push_back({ x0 + slotW * (float)i, y, slotW, bottom - y });
        }
        if (!cols.empty()) {
            SDL_SetRenderDrawColor(renderer_, kSustainLine.r, kSustainLine.g, kSustainLine.b, 70);
            SDL_RenderFillRects(renderer_, cols.data(), (int)cols.size());
        }
        drawSeries([](const TimingSample& s) { return (double)s.aheadSec; }, yOf,
                   [](double) { return kSustainLine; });
        if (readout && readout->aheadSec >= 0.0f) {
            char buf[32];
            aheadText(readout->aheadSec, buf, sizeof(buf));
            rightLabel(L, std::round(yOf(readout->aheadSec)), kValueText, buf);
        }
    }

    // ---- hover: outline the sample the legend is reading, through every lane ----
    if (hovering) {
        const float x = x0 + slotW * (float)shownIdx;
        SDL_SetRenderDrawColor(renderer_, 235, 238, 245, 160);
        for (const SDL_FRect& L : lanes) {
            const SDL_FRect hl = { x - 1.0f, L.y, barW + 2.0f, L.h };
            jplay::drawRect(renderer_, &hl);
        }
    }

    // Frame numbers under the lanes: the oldest, the newest, and the one read out,
    // which also says when its read figures are from an earlier lap.
    auto frameLabel = [&](int i) {
        const TimingSample& s = sampleAt(i);
        std::string t = std::to_string(s.frame);
        if (i == shownIdx && s.hasRead && !s.fresh)
            t += " (cached)";
        return t;
    };
    if (timingCount_ > 0) {
        auto label = [&](int i, bool alignRight) {
            const std::string t = frameLabel(i);
            const float w = textFont_.measure(renderer_, t.c_str());
            float x = alignRight ? x0 + slotW * (float)(i + 1) - w : x0 + slotW * (float)i;
            x = std::clamp(x, cx, cx + cw - w);
            drawText(x, xLabels.y, kMutedText, t);
            return SDL_FRect{ x, xLabels.y, w, lineH };
        };
        const SDL_FRect a = label(0, false);
        const SDL_FRect b = label(timingCount_ - 1, true);
        if (shownIdx != timingCount_ - 1 && shownIdx != 0) {
            const std::string t = frameLabel(shownIdx);
            const float w = textFont_.measure(renderer_, t.c_str());
            const float x = std::clamp(slotMid(shownIdx) - w * 0.5f, cx, cx + cw - w);
            if (x > a.x + a.w + 4.0f && x + w < b.x - 4.0f)
                drawText(x, xLabels.y, kValueText, t);
        }
    }

    // ---- legend: swatch, stage and its value for the sample read out ----
    {
        const TimingSample* s = readout;
        float v[kStages] = {};
        if (s)
            stageValues(*s, v);
        const float sw = swatchW;
        float x = legend.x;
        char buf[48];
        float mx = 0.0f, my = 0.0f;
        uiHoverMouse(mx, my);
        // A chunk-by-chunk OpenEXR read is inside the Decode figure (see
        // ReadTiming::ioSplit): Read IO has no key of its own then, and Decode is
        // keyed as both.
        const bool ioInDecode = s && s->hasRead && !s->ioSplit;
        for (int k = 0; k < kStages; ++k) {
            if (k == 1 && ioInDecode)
                continue;
            // Read stages of a frame with no read figures (a hover-preview frame,
            // a still) show a dash.
            if (!s || (k < 3 && !s->hasRead))
                SDL_snprintf(buf, sizeof(buf), "\xe2\x80\x94");
            else
                SDL_snprintf(buf, sizeof(buf), "%.1f ms", v[k]);
            const char* label = k == 2 && ioInDecode ? "Read IO & Decode"
                              : k == kDisplayStage && s && !s->ocio ? kPlainDisplayLabel
                                                                     : kStageLabel[k];
            // Each key is a toggle (see timingsStageHidden_): a stage toggled out
            // of the work lane is keyed faint, and the key under the cursor lights
            // up, on a faint plate, to show it takes a click.
            const float labelW = textFont_.measure(renderer_, label);
            const float valW = textFont_.measure(renderer_, buf);
            const SDL_FRect key = { x, legend.y, sw + 4.0f + labelW + 4.0f + valW, lineH };
            timingsStageKeyRect_[(size_t)k] = key;
            const bool hidden = stageHidden(k);
            const bool hot = inRect(key, mx, my);
            if (hot) {
                const SDL_FRect plate = { key.x - 3.0f, key.y, key.w + 6.0f, key.h };
                SDL_SetRenderDrawColor(renderer_, 255, 255, 255, 28);
                jplay::fillRect(renderer_, &plate);
            }
            const SDL_FRect swatch = { x, legend.y + (lineH - sw) * 0.5f, sw, sw };
            SDL_SetRenderDrawColor(renderer_, kStageColor[k].r, kStageColor[k].g, kStageColor[k].b,
                                   hidden ? (hot ? 130 : 60) : 255);
            jplay::fillRect(renderer_, &swatch);
            x += sw + 4.0f;
            drawText(x, legend.y, hidden ? (hot ? kMutedText : kGridLine) : (hot ? kValueText : kMutedText),
                     label);
            x += labelW + 4.0f;
            drawText(x, legend.y, hidden ? (hot ? kMutedText : kGridLine) : (hot ? kRateLine : kValueText), buf);
            x += valW + 12.0f;
        }
        // The lines' keys, each a short stroke in the line's own colour.
        for (int k = 0; k < kRates; ++k) {
            const float need = sw + 4.0f + textFont_.measure(renderer_, kRateLabel[k]) + 4.0f +
                               textFont_.measure(renderer_, kRateWidest[k]);
            if (x + need > legend.x + legend.w + 0.5f)
                break;
            const SDL_Color& c = k == 0 ? kRateLine : kSustainLine;
            if (k == 1) { // the capacity is dashed: so is its key
                SDL_SetRenderDrawColor(renderer_, c.r, c.g, c.b, 200);
                const float ly = std::round(legend.y + lineH * 0.5f);
                SDL_RenderLine(renderer_, x, ly, x + sw * 0.4f, ly);
                SDL_RenderLine(renderer_, x + sw * 0.65f, ly, x + sw, ly);
            } else if (k == 2) { // the buffer is an area: its key is a swatch
                const SDL_FRect swatch = { x, legend.y + (lineH - sw) * 0.5f, sw, sw };
                SDL_SetRenderDrawColor(renderer_, c.r, c.g, c.b, 110);
                jplay::fillRect(renderer_, &swatch);
            } else {
                SDL_SetRenderDrawColor(renderer_, c.r, c.g, c.b, c.a);
                const float ly = std::round(legend.y + lineH * 0.5f);
                SDL_RenderLine(renderer_, x, ly, x + sw, ly);
                SDL_RenderLine(renderer_, x, ly - 1.0f, x + sw, ly - 1.0f);
            }
            x += sw + 4.0f;
            drawText(x, legend.y, kMutedText, kRateLabel[k]);
            x += textFont_.measure(renderer_, kRateLabel[k]) + 4.0f;
            SDL_snprintf(buf, sizeof(buf), "\xe2\x80\x94");
            if (s && k == 0 && shownFps(*s) > 0.0)
                SDL_snprintf(buf, sizeof(buf), missed(*s) ? "%.1f, late" : "%.1f", shownFps(*s));
            else if (s && k == 1 && capacity[(size_t)shownIdx] > 0.0)
                SDL_snprintf(buf, sizeof(buf), "%.0f (%s)", capacity[(size_t)shownIdx],
                             capacityRead[(size_t)shownIdx] ? "read" : "display");
            else if (s && k == 2)
                aheadText(s->aheadSec, buf, sizeof(buf));
            drawText(x, legend.y, kValueText, buf);
            x += textFont_.measure(renderer_, buf) + 12.0f;
        }
    }
}

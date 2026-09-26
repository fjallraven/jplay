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
#include <iterator>
#include <vector>

using namespace jplay;

namespace {

// The five stages, bottom of the stack to top, in the fixed categorical order.
// Checked for colour-vision separation and contrast against the dark panel.
constexpr int kStages = 5;
const SDL_Color kStageColor[kStages] = {
    { 0x39, 0x87, 0xe5, 255 }, // wait IO  - blue
    { 0xd9, 0x59, 0x26, 255 }, // read IO  - orange
    { 0x19, 0x9e, 0x70, 255 }, // EXR      - aqua
    { 0xc9, 0x85, 0x00, 255 }, // OCIO     - yellow
    { 0xd5, 0x51, 0x81, 255 }, // output   - magenta
};
const char* const kStageLabel[kStages] = { "Wait IO", "Read IO", "EXR", "OCIO", "Output" };

const SDL_Color kTitleText = { 160, 170, 200, 255 };
const SDL_Color kMutedText = { 130, 135, 148, 255 };
const SDL_Color kValueText = { 215, 218, 225, 255 };
const SDL_Color kGridLine = { 62, 64, 72, 255 };
const SDL_Color kNoteText = { 104, 108, 120, 255 }; // a lane title's explanation
const SDL_Color kRateLine = { 235, 238, 245, 255 };
const SDL_Color kOffRateLine = { 222, 82, 82, 255 }; // the rate line off the project rate
// The sustainable rate and the buffer: neutral ink rather than a hue, so neither
// reads as one of the stages. Identity comes from the lane and the labels.
const SDL_Color kSustainLine = { 160, 168, 190, 255 };

// The legend's line keys after the stages, and each one's widest value.
constexpr int kRates = 3;
const char* const kRateLabel[kRates] = { "Shown", "Sustainable", "Ahead" };
const char* const kRateWidest[kRates] = { "000.0", "0000 (display)", "10 s+" };

// The Y axis is milliseconds. Its top is snapped to a step of this ladder so it
// does not twitch with every sample (a stall past the last runs off the top), and
// each top has its own round gridline spacing.
const double kScaleMs[] = { 25.0, 50.0, 100.0, 200.0, 400.0 };
const double kGridStepMs[] = { 5.0, 10.0, 20.0, 50.0, 100.0 };

// A measured rate as the info bar's fps readout shows it (see App::update): the
// project rate itself when within 0.3 fps of it, otherwise rounded to a tenth.
// The readout also holds its value until it moves a full tenth; a per-frame chart
// has no "previous value" to hold, so that part is not repeated here.
double snappedFps(double fps, double projectFps) {
    if (projectFps > 0.0 && std::fabs(fps - projectFps) < 0.3)
        return projectFps;
    return std::round(fps * 10.0) / 10.0;
}

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
    timingLastShownNs_ = 0;
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
    // The shown rate at this frame (see TimingSample::rateMs). A single interval
    // zigzags with the refresh cadence (24 fps on 60 Hz holds frames for 2 and 3
    // refreshes in turn, 33 and 50 ms), which reads as trouble when it is not;
    // averaged over an even run the cadence cancels, and what is left moves only
    // for a frame really held or dropped. Only over a full window, unbroken: just
    // after Play a part-filled one holds an odd count of 2- and 3-refresh
    // intervals as often as an even one, so the line would zigzag either side of
    // the rate (7 of them, 18 refreshes, read 23.3 fps) with every frame on time.
    if (s.intervalMs > 0.0f && timingCount_ >= kTimingRateWindow - 1) {
        double sum = s.intervalMs;
        int n = 1;
        for (; n < kTimingRateWindow; ++n) {
            const float in = timingSamples_[(size_t)((timingHead_ - n + kTimingSamples) % kTimingSamples)].intervalMs;
            if (in <= 0.0f)
                break;
            sum += in;
        }
        if (n == kTimingRateWindow)
            s.rateMs = (float)(sum / n);
    }
    if (playing_) {
        const int workers = std::max(1, cache_->threadCount());
        const double readCost = s.hasRead && s.fresh ? s.readMs() / workers : 0.0;
        const double cost = std::max(readCost, (double)s.uiMs);
        if (cost > 0.0) {
            s.sustainFps = (float)(1000.0 / cost);
            s.readLimited = readCost > s.uiMs;
        }
        s.aheadSec = (float)timingBufferedAheadSec();
    }
    timingSamples_[(size_t)timingHead_] = s;
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
    for (const char* label : kStageLabel)
        legendW += swatchW + 4.0f + textFont_.measure(renderer_, label) + 4.0f + valueW + 12.0f;
    for (int k = 0; k < kRates; ++k)
        legendW += swatchW + 4.0f + textFont_.measure(renderer_, kRateLabel[k]) + 4.0f +
                   textFont_.measure(renderer_, kRateWidest[k]) + 12.0f;
    const float contractedW = std::max(560.0f * dpiScale, legendW + pad * 2.0f);
    const float panelW = timingsExpanded_ ? fullW : std::min(fullW, contractedW);
    const float panelH = timingsExpanded_ ? fullH : std::min(fullH, 400.0f * dpiScale);
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
        v[2] = s.exrMs;
        v[3] = s.ocioMs;
        v[4] = s.outputMs;
    };
    // Samples oldest first: the ring's i-th entry counting from the oldest.
    auto sampleAt = [&](int i) -> const TimingSample& {
        const int oldest = (timingHead_ - timingCount_ + kTimingSamples) % kTimingSamples;
        return timingSamples_[(size_t)((oldest + i) % kTimingSamples)];
    };
    // The shown rate at each frame (see commitTimingSample), snapped as the info
    // bar snaps it, so a rate that reads as the project's there sits exactly on it
    // here too, flat rather than a pixel either side. 0 where there is none
    // (paused, or just after Play).
    auto shownFps = [&](const TimingSample& s) {
        return s.rateMs > 0.0f ? snappedFps(1000.0 / s.rateMs, fps) : 0.0;
    };
    auto onRate = [&](double v) { return fps <= 0.0 || std::fabs(v - fps) < 1e-6; };
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
    // Reads run on the cache's worker pool in parallel with each other and with the
    // UI thread, so the stages are not added: the read side sustains workers / read
    // time, the display side 1 / UI tick, and the slower of the two is the estimate.
    // Averaged over the last second or so of samples; headroom is that over the
    // project rate.
    {
        const int recent = std::min(timingCount_, kTimingRateWindow);
        double readSum = 0.0, dispSum = 0.0;
        int reads = 0;
        for (int i = timingCount_ - recent; i < timingCount_; ++i) {
            const TimingSample& s = sampleAt(i);
            dispSum += s.uiMs;
            if (s.hasRead && s.fresh) {
                readSum += s.readMs();
                ++reads;
            }
        }
        row = cutTop(body, lineH);
        if (recent == 0) {
            drawText(row.x, row.y, kMutedText, "Waiting for frames...");
        } else {
            // Frames shown again from the cache read nothing, so the read side is
            // costed per frame shown: the reads there were, over every frame.
            const double readMsPerFrame = reads > 0 ? readSum / recent / workers : 0.0;
            const double dispMsPerFrame = dispSum / recent;
            const bool readLimited = readMsPerFrame > dispMsPerFrame;
            const double costMs = std::max(readMsPerFrame, dispMsPerFrame);
            const TimingSample& newest = sampleAt(timingCount_ - 1);
            std::string text = "Shown ";
            char buf[160];
            const double shown = shownFps(newest);
            if (shown > 0.0)
                SDL_snprintf(buf, sizeof(buf), "%.1f fps", shown);
            else
                SDL_snprintf(buf, sizeof(buf), "\xe2\x80\x94 fps");
            text += buf;
            if (costMs > 0.0) {
                const double est = 1000.0 / costMs;
                if (fps > 0.0)
                    SDL_snprintf(buf, sizeof(buf), "   Sustainable %.0f fps, %.0f\xc3\x97 headroom (%s-limited, %d workers)",
                                 est, est / fps, readLimited ? "read" : "display", workers);
                else
                    SDL_snprintf(buf, sizeof(buf), "   Sustainable %.0f fps (%s-limited, %d workers)",
                                 est, readLimited ? "read" : "display", workers);
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
    const float axisW = textFont_.measure(renderer_, "400 ms") + 6.0f;
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
                 "shown, and what each frame's work could sustain; the gap is the headroom");
    SDL_snprintf(laneNote[1], sizeof(laneNote[1]),
                 "reads run %d at once, ahead of the playhead: a tall stack is not a late frame",
                 workers);
    SDL_snprintf(laneNote[2], sizeof(laneNote[2]),
                 "frames already read past the one shown; at zero a frame is held");
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
    {
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
    // frame held for two reads there) up past the highest sustainable rate, so the
    // project rate is one fixed gridline and headroom reads off as 2x, 4x, 8x.
    // The shown rate sits on 1x, white while it holds the project's, red where it
    // does not (where the info bar's readout would read something else). Above it
    // runs each frame's sustainable rate; the band between the two is filled, and
    // turns red where a frame could not have been kept up with.
    {
        const SDL_FRect& L = rateLane;
        const float bottom = L.y + L.h;
        double maxMul = 4.0;
        for (int i = 0; i < timingCount_; ++i)
            if (fps > 0.0)
                maxMul = std::max(maxMul, (double)sampleAt(i).sustainFps / fps);
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
            std::vector<SDL_FRect> band, shortfall;
            for (int i = 0; i < timingCount_; ++i) {
                const float v = sampleAt(i).sustainFps;
                if (v <= 0.0f)
                    continue;
                const float y1 = yOf(fps), y2 = yOf(v);
                const float x = x0 + slotW * (float)i;
                if (y2 < y1)
                    band.push_back({ x, y2, slotW, y1 - y2 });
                else if (y2 > y1)
                    shortfall.push_back({ x, y1, slotW, y2 - y1 });
            }
            SDL_SetRenderDrawColor(renderer_, kSustainLine.r, kSustainLine.g, kSustainLine.b, 38);
            if (!band.empty())
                SDL_RenderFillRects(renderer_, band.data(), (int)band.size());
            SDL_SetRenderDrawColor(renderer_, kOffRateLine.r, kOffRateLine.g, kOffRateLine.b, 90);
            if (!shortfall.empty())
                SDL_RenderFillRects(renderer_, shortfall.data(), (int)shortfall.size());
            drawSeries([](const TimingSample& s) { return (double)s.sustainFps; }, yOf,
                       [](double) { return kSustainLine; });
            drawSeries(shownFps, yOf,
                       [&](double v) { return onRate(v) ? kRateLine : kOffRateLine; });
        }
        if (readout && fps > 0.0) {
            char buf[32];
            float shownY = -1e9f;
            const double v = shownFps(*readout);
            if (v > 0.0) {
                shownY = std::round(yOf(v));
                const SDL_Color& c = onRate(v) ? kRateLine : kOffRateLine;
                if (hovering) { // mark the point being read out
                    SDL_SetRenderDrawColor(renderer_, c.r, c.g, c.b, c.a);
                    const SDL_FRect dot = { slotMid(shownIdx) - 3.0f, shownY - 3.5f, 6.0f, 6.0f };
                    jplay::fillRect(renderer_, &dot);
                }
                SDL_snprintf(buf, sizeof(buf), "%.1f fps", v);
                rightLabel(L, shownY, kValueText, buf);
            }
            if (readout->sustainFps > 0.0f) {
                SDL_snprintf(buf, sizeof(buf), "%.0f fps", readout->sustainFps);
                rightLabel(L, std::round(yOf(readout->sustainFps)), kMutedText, buf, shownY);
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
            peak = std::max(peak, (double)(s.readMs() + s.displayMs()));
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
                if (v[k] <= 0.0f)
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
            const double total = readout->readMs() + readout->displayMs();
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
        for (int k = 0; k < kStages; ++k) {
            const SDL_FRect swatch = { x, legend.y + (lineH - sw) * 0.5f, sw, sw };
            SDL_SetRenderDrawColor(renderer_, kStageColor[k].r, kStageColor[k].g, kStageColor[k].b, 255);
            jplay::fillRect(renderer_, &swatch);
            x += sw + 4.0f;
            drawText(x, legend.y, kMutedText, kStageLabel[k]);
            x += textFont_.measure(renderer_, kStageLabel[k]) + 4.0f;
            // Read stages of a frame with no read figures (a hover-preview frame,
            // a video) show a dash; OpenEXR's read is inside the EXR figure (see
            // ReadTiming::ioSplit).
            if (!s || (k < 3 && !s->hasRead))
                SDL_snprintf(buf, sizeof(buf), "\xe2\x80\x94");
            else if (k == 1 && !s->ioSplit)
                SDL_snprintf(buf, sizeof(buf), "in EXR");
            else
                SDL_snprintf(buf, sizeof(buf), "%.1f ms", v[k]);
            drawText(x, legend.y, kValueText, buf);
            x += textFont_.measure(renderer_, buf) + 12.0f;
        }
        // The lines' keys, each a short stroke in the line's own colour.
        for (int k = 0; k < kRates; ++k) {
            const float need = sw + 4.0f + textFont_.measure(renderer_, kRateLabel[k]) + 4.0f +
                               textFont_.measure(renderer_, kRateWidest[k]);
            if (x + need > legend.x + legend.w + 0.5f)
                break;
            const SDL_Color& c = k == 0 ? kRateLine : kSustainLine;
            if (k == 2) { // the buffer is an area: its key is a swatch
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
                SDL_snprintf(buf, sizeof(buf), "%.1f", shownFps(*s));
            else if (s && k == 1 && s->sustainFps > 0.0f)
                SDL_snprintf(buf, sizeof(buf), "%.0f (%s)", s->sustainFps, s->readLimited ? "read" : "display");
            else if (s && k == 2)
                aheadText(s->aheadSec, buf, sizeof(buf));
            drawText(x, legend.y, kValueText, buf);
            x += textFont_.measure(renderer_, buf) + 12.0f;
        }
    }
}

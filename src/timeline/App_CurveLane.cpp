// Curve lane: one timeline track expanded into a full-height editing surface
// where every clip on it carries a parameter curve drawn over its own content.
//
// The mode is deliberately parameter-agnostic. Everything below works on the
// Curve that clipCurve() hands back, mapped through curveRange() and labelled by
// curveValueLabel(); nothing here knows the values are decibels. Adding, say, a
// video opacity lane means one more CurveParam case in those three functions —
// the drawing, the hit testing and the drag are already generic.
//
// The lane's geometry comes from the ordinary track layout: curve mode collapses
// every other row to zero height (App::trackRowH) and gives this one the whole
// stack's height, so clipBandRect already returns the tall band and no second
// layout pass exists to disagree with the first.

#include "App.h"
#include "AppInternal.h"
#include "Layout.h"
#include "TopBarStyle.h" // setColor

#include <algorithm>
#include <cmath>

using namespace jplay;

namespace {

// Lane chrome. Kept local like the rest of the timeline's colours (the shared
// palette in SkinColors.h covers panel chrome, not the timeline).
constexpr SDL_Color kCurveLine     { 255, 214, 102, 235 }; // the curve itself
constexpr SDL_Color kCurvePoint    { 255, 232, 170, 255 }; // a breakpoint
constexpr SDL_Color kCurvePointHot { 255, 255, 255, 255 }; // ... hovered or dragged
constexpr SDL_Color kCurveStartBox { 130, 220, 255, 255 }; // the clip's translate handle
constexpr SDL_Color kCurveGrid     { 255, 255, 255,  26 }; // value gridline
constexpr SDL_Color kCurveGridRef  { 255, 255, 255,  64 }; // ... the unity reference
constexpr SDL_Color kCurveGridText { 150, 156, 166, 255 };
constexpr SDL_Color kCurveGhost    { 255, 232, 170, 120 }; // point the cursor would add

constexpr float kPointHalf    = 3.0f;  // half-size of a breakpoint square
constexpr float kStartHalf    = 4.5f;  // half-size of the start box (bigger: it grabs first)
constexpr float kGrabPx       = 7.0f;  // cursor distance that counts as "on" a point
constexpr float kLineGrabPx   = 6.0f;  // vertical distance that counts as "on" the line
constexpr float kPlotInsetY   = 8.0f;  // so a point at either range end stays inside the band
constexpr float kMinLaneH     = 180.0f;

} // namespace

// ------------------------------------------------------------- mode and binding

void App::enterCurveMode(int track, CurveParam param) {
    if (track < 0 || track >= trackCount())
        return;
    curveTrack_ = track;
    curveParam_ = param;
    curveLaneH_ = curveLaneHeight();
    curveDragClipId_ = -1;
    curveHoverClipId_ = -1;
    curveHoverPtIdx_ = -2;
    // A drag or trim armed on a row that is about to collapse would commit against
    // geometry that no longer exists.
    draggingClip_ = false;
    trimmingClip_ = false;
    fadingClip_ = false;
    selectedGapTrack_ = -1;
    setStatus("EDIT VOLUME: DRAG THE BOX TO OFFSET, CLICK THE LINE TO ADD A POINT", 5000);
}

void App::exitCurveMode() {
    curveTrack_ = -1;
    curveDragClipId_ = -1;
    curveHoverClipId_ = -1;
    curveHoverPtIdx_ = -2;
}

Curve& App::clipCurve(Clip& c) const {
    switch (curveParam_) {
    case CurveParam::Volume:
    default:
        return c.volume;
    }
}

const Curve& App::clipCurve(const Clip& c) const {
    return clipCurve(const_cast<Clip&>(c));
}

void App::curveRange(float& lo, float& hi) const {
    switch (curveParam_) {
    case CurveParam::Volume:
    default:
        lo = kVolMinDb;
        hi = kVolMaxDb;
        break;
    }
}

std::string App::curveValueLabel(float v) const {
    char buf[32];
    switch (curveParam_) {
    case CurveParam::Volume:
    default:
        if (v <= kVolMinDb)
            SDL_snprintf(buf, sizeof(buf), "-inf dB");
        else
            SDL_snprintf(buf, sizeof(buf), "%+.1f dB", v);
        break;
    }
    return buf;
}

// The stack's height measured the normal way. Cannot go through trackRowH: by the
// time this runs the mode is already on and that would answer with the lane's own
// height. Floored so a two-track project still gets a lane worth editing in.
float App::curveLaneHeight() const {
    const int n = std::max(trackCount(), 1);
    float h = 0.0f;
    for (int t = 0; t < n; ++t) {
        Timeline::TrackKind k = timeline_.trackKind(t);
        if (t == n - 1 && k == Timeline::TrackKind::Empty)
            h += kTrackH * 0.5f;
        else
            h += (k == Timeline::TrackKind::Audio) ? kAudioTrackH : kTrackH;
    }
    return std::max(h, kMinLaneH * dpiScale);
}

// ------------------------------------------------------------------- geometry

SDL_FRect App::curvePlotRect(const Clip& c) const {
    return inset(clipBandRect(c.track, c.audio), 0.0f, kPlotInsetY * dpiScale);
}

float App::curveValueToY(const SDL_FRect& plot, float v) const {
    float lo = 0.0f, hi = 1.0f;
    curveRange(lo, hi);
    const float f = hi > lo ? (v - lo) / (hi - lo) : 0.0f;
    return plot.y + plot.h * (1.0f - std::clamp(f, 0.0f, 1.0f));
}

float App::curveYToValue(const SDL_FRect& plot, float y) const {
    float lo = 0.0f, hi = 1.0f;
    curveRange(lo, hi);
    if (plot.h <= 0.0f)
        return lo;
    const float f = std::clamp(1.0f - (y - plot.y) / plot.h, 0.0f, 1.0f);
    return lo + (hi - lo) * f;
}

// Source frame under a pixel column for `c` — the curve's own time base.
static int64_t srcFrameAtX(const Clip& c, double frame) {
    return c.sourceOffset + (int64_t)std::llround(frame - (double)c.timelineStart);
}

// ---------------------------------------------------------------- hit testing

Clip* App::curveClipAt(float mx, float my, int& ptIdx) {
    ptIdx = -2;
    if (!curveMode())
        return nullptr;

    // Start boxes first, scanned across the row rather than looked up under the
    // cursor. A box is centred on its clip's cut-in, so its left half hangs over
    // frames the clip does not cover: resolving the clip by the frame under the
    // cursor would make that half dead, or worse hand the press to the clip
    // before it. Same reason fadeHandleAt scans instead of looking up.
    for (auto& sq : timeline_.sequences)
        for (auto& c : sq.clips) {
            if (c.track != curveTrack_)
                continue;
            const SDL_FRect plot = curvePlotRect(c);
            const float sx = (float)frameToX((double)c.timelineStart);
            const float sy = curveValueToY(plot, clipCurve(c).valueAt(c.sourceOffset));
            if (std::fabs(mx - sx) <= kStartHalf + 2.0f &&
                std::fabs(my - sy) <= kStartHalf + 2.0f) {
                ptIdx = -1;
                return &c;
            }
        }

    const int64_t f = (int64_t)std::floor(xToFrame((double)mx));
    Clip* hit = clipAtTrack(curveTrack_, f);
    if (!hit)
        return nullptr;
    const Curve& cv = clipCurve(*hit);
    const SDL_FRect plot = curvePlotRect(*hit);

    // Then the breakpoints inside the clip's trimmed range; points outside it are
    // kept but not drawn, so they must not be grabbable either.
    const int64_t srcLo = hit->sourceOffset;
    const int64_t srcHi = hit->sourceOffset + hit->duration;
    float best = kGrabPx;
    for (size_t i = 0; i < cv.pts.size(); ++i) {
        if (cv.pts[i].t < srcLo || cv.pts[i].t >= srcHi)
            continue;
        const float px = (float)frameToX((double)(hit->timelineStart + (cv.pts[i].t - srcLo)));
        const float py = curveValueToY(plot, cv.pts[i].v);
        const float d = std::max(std::fabs(mx - px), std::fabs(my - py));
        if (d <= best) {
            best = d;
            ptIdx = (int)i;
        }
    }
    return hit;
}

// ------------------------------------------------------------------ rendering

void App::renderCurveLane() {
    if (!curveMode())
        return;
    float lo = 0.0f, hi = 1.0f;
    curveRange(lo, hi);

    // ---- value gridlines across the whole lane, with the unity line picked out.
    // Drawn once for the row rather than per clip so they read as the lane's axis.
    {
        // Built from the same band the clips' plots come from, so a gridline and a
        // flat curve at that value land on exactly the same pixel row.
        const bool audioRow =
            timeline_.trackKind(curveTrack_) == Timeline::TrackKind::Audio;
        const SDL_FRect plot =
            inset(clipBandRect(curveTrack_, audioRow), 0.0f, kPlotInsetY * dpiScale);
        static const float kDbGrid[] = { 0.0f, -6.0f, -12.0f, -24.0f, -48.0f };
        for (float g : kDbGrid) {
            if (g > hi || g < lo)
                continue;
            const float y = std::round(curveValueToY(plot, g));
            setColor(renderer_, g == 0.0f ? kCurveGridRef : kCurveGrid);
            drawLine(renderer_, headerX_, y, headerX_ + contentW_, y);
            char lbl[16];
            SDL_snprintf(lbl, sizeof(lbl), "%.0f", g);
            drawText(headerX_ + 3.0f, y - textFont_.lineHeight() - 1.0f, kCurveGridText, lbl);
        }
    }

    // ---- one curve per clip on the row.
    auto drawOne = [&](const Clip& c) {
        if (c.track != curveTrack_)
            return;
        const float x0 = (float)frameToX((double)c.timelineStart);
        const float x1 = (float)frameToX((double)c.end());
        if (x1 < headerX_ || x0 > headerX_ + contentW_)
            return;
        const Curve& cv = clipCurve(c);
        const SDL_FRect plot = curvePlotRect(c);

        // The curve as a per-column polyline, evaluated with the same valueAt the
        // audio bake uses — so what is drawn is exactly what is heard, with no
        // separate tessellation to drift out of step with it.
        const int px0 = (int)std::floor(std::max(x0, headerX_));
        const int px1 = (int)std::ceil(std::min(x1, headerX_ + contentW_));
        setColor(renderer_, kCurveLine);
        float prevX = 0.0f, prevY = 0.0f;
        bool havePrev = false;
        for (int x = px0; x < px1; ++x) {
            const double frame = xToFrame((double)x + 0.5);
            const float y = curveValueToY(plot, cv.valueAt(srcFrameAtX(c, frame)));
            if (havePrev)
                drawLine(renderer_, prevX, prevY, (float)x, y);
            prevX = (float)x;
            prevY = y;
            havePrev = true;
        }

        // Breakpoints inside the trimmed range.
        const int64_t srcLo = c.sourceOffset;
        const int64_t srcHi = c.sourceOffset + c.duration;
        for (size_t i = 0; i < cv.pts.size(); ++i) {
            if (cv.pts[i].t < srcLo || cv.pts[i].t >= srcHi)
                continue;
            const float px = (float)frameToX((double)(c.timelineStart + (cv.pts[i].t - srcLo)));
            if (px < headerX_ || px > headerX_ + contentW_)
                continue;
            const float py = curveValueToY(plot, cv.pts[i].v);
            const bool hot = (curveDragClipId_ == c.id && curveDragPtIdx_ == (int)i) ||
                             (curveDragClipId_ < 0 && curveHoverClipId_ == c.id &&
                              curveHoverPtIdx_ == (int)i);
            SDL_FRect box{ px - kPointHalf, py - kPointHalf, kPointHalf * 2, kPointHalf * 2 };
            setColor(renderer_, hot ? kCurvePointHot : kCurvePoint);
            fillRect(renderer_, &box);
        }

        // The start box at the clip's cut-in: drags the whole curve up and down.
        // Hollow rather than filled so it stays distinct from a breakpoint sitting
        // in the same place.
        {
            const float sx = (float)frameToX((double)c.timelineStart);
            const float sy = curveValueToY(plot, cv.valueAt(c.sourceOffset));
            if (sx >= headerX_ - kStartHalf && sx <= headerX_ + contentW_) {
                const bool hot = (curveDragClipId_ == c.id && curveDragPtIdx_ == -1) ||
                                 (curveDragClipId_ < 0 && curveHoverClipId_ == c.id &&
                                  curveHoverPtIdx_ == -1);
                SDL_FRect box{ sx - kStartHalf, sy - kStartHalf, kStartHalf * 2, kStartHalf * 2 };
                setColor(renderer_, hot ? kCurvePointHot : kCurveStartBox);
                if (hot)
                    fillRect(renderer_, &box);
                else
                    drawRect(renderer_, &box);
            }
        }

        // Readout for the point being dragged, and the ghost point a click on the
        // line would add.
        if (curveDragClipId_ == c.id) {
            const float v = curveDragPtIdx_ >= 0 && curveDragPtIdx_ < (int)cv.pts.size()
                                ? cv.pts[(size_t)curveDragPtIdx_].v
                                : cv.valueAt(c.sourceOffset);
            const int64_t t = curveDragPtIdx_ >= 0 && curveDragPtIdx_ < (int)cv.pts.size()
                                  ? cv.pts[(size_t)curveDragPtIdx_].t
                                  : c.sourceOffset;
            const float lx = (float)frameToX((double)(c.timelineStart + (t - srcLo)));
            const std::string lbl = curveValueLabel(v);
            drawText(lx + 8.0f, curveValueToY(plot, v) - textFont_.lineHeight() - 4.0f,
                     kCurvePointHot, lbl);
        } else if (curveDragClipId_ < 0 && curveHoverClipId_ == c.id &&
                   curveHoverPtIdx_ == -2) {
            // Ghost of the point a click would add, right on the line — and only
            // within the same reach the click itself uses, so it never advertises
            // a gesture that would miss.
            const double frame = xToFrame((double)curveHoverX_);
            const float gy = curveValueToY(plot, cv.valueAt(srcFrameAtX(c, frame)));
            if (std::fabs(curveHoverY_ - gy) <= kLineGrabPx) {
                SDL_FRect box{ curveHoverX_ - kPointHalf, gy - kPointHalf,
                               kPointHalf * 2, kPointHalf * 2 };
                setColor(renderer_, kCurveGhost);
                drawRect(renderer_, &box);
            }
        }
    };
    forEachViewClip(drawOne);
}

// ----------------------------------------------------------------- interaction

bool App::curveLaneMouseDown(float mx, float my, bool rightButton, int clicks) {
    if (!curveMode())
        return false;
    const float rowY = trackRowY(curveTrack_);
    if (mx < headerX_ || my < rowY || my >= rowY + trackRowH(curveTrack_))
        return false;

    int ptIdx = -2;
    Clip* clip = curveClipAt(mx, my, ptIdx);
    if (!clip)
        return true; // empty lane space: swallow it rather than let a clip drag start

    // Right-click or double-click on a point removes it; the start box has no
    // point of its own to remove, so it resets the clip's curve instead.
    if (rightButton || clicks >= 2) {
        if (ptIdx == -2)
            return true;
        ContentSnapshot before = captureContent();
        Curve& cv = clipCurve(*clip);
        if (ptIdx >= 0 && ptIdx < (int)cv.pts.size()) {
            cv.pts.erase(cv.pts.begin() + ptIdx);
            pushContentUndo("REMOVE CURVE POINT", before);
        } else {
            cv = Curve{};
            pushContentUndo("RESET VOLUME", before);
        }
        curveHoverPtIdx_ = -2;
        return true;
    }

    curveDragBefore_ = captureContent();
    Curve& cv = clipCurve(*clip);
    const SDL_FRect plot = curvePlotRect(*clip);
    bool added = false;

    if (ptIdx == -2) {
        // On (or near) the line: add a point there and drag it straight away, so
        // one gesture both creates and places it. Too far from the line and the
        // press is just a miss — otherwise a click anywhere in the tall lane would
        // fling a point to the cursor's height.
        const int64_t src = srcFrameAtX(*clip, xToFrame((double)mx));
        if (std::fabs(my - curveValueToY(plot, cv.valueAt(src))) > kLineGrabPx)
            return true;
        ptIdx = (int)cv.addPoint(std::clamp(src, clip->sourceOffset,
                                            clip->sourceOffset + clip->duration - 1),
                                 cv.valueAt(src));
        added = true;
    }

    curveDragClipId_ = clip->id;
    curveDragPtIdx_ = ptIdx;
    curveDragChanged_ = added;
    // Grab offset, so a point does not jump to the cursor when picked up slightly
    // off-centre.
    const float grabbed = ptIdx >= 0 ? cv.pts[(size_t)ptIdx].v : cv.valueAt(clip->sourceOffset);
    curveDragGrabDv_ = curveYToValue(plot, my) - grabbed;
    selectClipSingle(clip->id);
    return true;
}

bool App::curveLaneMouseMotion(float mx, float my) {
    if (!curveMode())
        return false;

    if (curveDragClipId_ < 0) {
        // Hover only: remember what is under the cursor so the render pass can
        // light it up and offer the ghost point.
        int ptIdx = -2;
        const float rowY = trackRowY(curveTrack_);
        Clip* clip = (mx >= headerX_ && my >= rowY && my < rowY + trackRowH(curveTrack_))
                      ? curveClipAt(mx, my, ptIdx)
                      : nullptr;
        curveHoverClipId_ = clip ? clip->id : -1;
        curveHoverPtIdx_ = clip ? ptIdx : -2;
        curveHoverX_ = mx;
        curveHoverY_ = my;
        return clip != nullptr;
    }

    Clip* c = clipById(curveDragClipId_);
    if (!c) {
        curveDragClipId_ = -1;
        return false;
    }
    float lo = 0.0f, hi = 1.0f;
    curveRange(lo, hi);
    Curve& cv = clipCurve(*c);
    const SDL_FRect plot = curvePlotRect(*c);
    const float v = curveYToValue(plot, my) - curveDragGrabDv_;

    curveDragChanged_ = true;
    if (curveDragPtIdx_ < 0) {
        // The start box: a rigid vertical translate of the whole curve, which is
        // the same gesture whether the clip has points yet or not.
        cv.translate(std::clamp(v, lo, hi) - cv.valueAt(c->sourceOffset), lo, hi);
        return true;
    }
    if (curveDragPtIdx_ >= (int)cv.pts.size()) {
        curveDragClipId_ = -1;
        return false;
    }
    CurvePoint& p = cv.pts[(size_t)curveDragPtIdx_];
    p.v = std::clamp(v, lo, hi);
    // Horizontal move too, penned in by the neighbouring points and the clip's own
    // source range so the list stays sorted and the point stays reachable.
    int64_t t = srcFrameAtX(*c, xToFrame((double)mx));
    int64_t tLo = c->sourceOffset;
    int64_t tHi = c->sourceOffset + c->duration - 1;
    if (curveDragPtIdx_ > 0)
        tLo = std::max(tLo, cv.pts[(size_t)curveDragPtIdx_ - 1].t + 1);
    if (curveDragPtIdx_ + 1 < (int)cv.pts.size())
        tHi = std::min(tHi, cv.pts[(size_t)curveDragPtIdx_ + 1].t - 1);
    p.t = std::clamp(t, tLo, std::max(tLo, tHi));
    return true;
}

bool App::curveLaneMouseUp() {
    if (curveDragClipId_ < 0)
        return false;
    curveDragClipId_ = -1;
    curveDragPtIdx_ = -1;
    // One undo step for the whole gesture, matching how a clip trim or a fade drag
    // records itself — and none at all for a press that grabbed a point and let go
    // without moving it, which would otherwise stack up empty steps.
    if (curveDragChanged_) {
        pushContentUndo(curveParam_ == CurveParam::Volume ? "EDIT VOLUME" : "EDIT CURVE",
                        curveDragBefore_);
        curveDragChanged_ = false;
    }
    return true;
}

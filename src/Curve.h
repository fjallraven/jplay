#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

// A time-varying scalar parameter attached to a clip: a flat base value plus an
// optional list of breakpoints. Deliberately unitless — the owner decides what a
// value means (Clip::volume stores dB) and interpolation happens in those units,
// which is what keeps the type generic: a video parameter (opacity, exposure)
// adopts it unchanged and gets the right shape without the curve knowing anything.
//
// Point times are SOURCE frames, the same anchoring Clip::annotations uses, so a
// dip stays glued to the media it was drawn on through every move, trim and slip.
// Points outside the clip's trimmed range are kept rather than pruned (they just
// don't draw), so shortening a clip and lengthening it again restores the shape
// the user made — the same read-time philosophy as Timeline::clipFades.
struct CurvePoint {
    int64_t t = 0;    // source frame (sourceOffset space)
    float   v = 0.0f; // value in the owner's units
};

struct Curve {
    // Value where there are no points. Also what translate() moves along with
    // them, which is what lets the start-box drag be one code path whether the
    // curve is still flat or already shaped.
    float base = 0.0f;
    std::vector<CurvePoint> pts; // sorted by t; empty = a flat line at `base`

    // Smooth is monotone cubic Hermite (Fritsch-Carlson). Chosen over plain
    // Catmull-Rom because it cannot overshoot: the curve stays inside the range
    // its points span, so nothing has to clamp against the shape that was drawn.
    enum class Interp : uint8_t { Linear, Smooth };
    Interp interp = Interp::Smooth;

    bool isDefault() const { return pts.empty() && base == 0.0f; }

    // Value at `src`, flat-extrapolated outside the point range. The renderer and
    // the audio bake both call this, so what is drawn is what is heard.
    float valueAt(int64_t src) const {
        if (pts.empty())
            return base;
        if (src <= pts.front().t)
            return pts.front().v;
        if (src >= pts.back().t)
            return pts.back().v;
        // Segment [i, i+1] containing src.
        size_t i = 0;
        while (i + 2 < pts.size() && pts[i + 1].t <= src)
            ++i;
        const CurvePoint& a = pts[i];
        const CurvePoint& b = pts[i + 1];
        const double h = (double)(b.t - a.t);
        if (h <= 0.0)
            return b.v;
        const double u = (double)(src - a.t) / h;
        if (interp == Interp::Linear)
            return (float)(a.v + (b.v - a.v) * u);

        // Monotone cubic Hermite: tangents from the Fritsch-Carlson filter, so a
        // segment between two points never leaves the band those points span.
        const double d = ((double)b.v - (double)a.v) / h;
        const double m0 = tangent(i, d);
        const double m1 = tangent(i + 1, d);
        const double u2 = u * u, u3 = u2 * u;
        const double h00 =  2.0 * u3 - 3.0 * u2 + 1.0;
        const double h10 =        u3 - 2.0 * u2 + u;
        const double h01 = -2.0 * u3 + 3.0 * u2;
        const double h11 =        u3 -       u2;
        return (float)(h00 * a.v + h10 * h * m0 + h01 * b.v + h11 * h * m1);
    }

    // Rigid vertical move of the whole curve (the start box's drag), clamped so
    // no part of it leaves [lo, hi]. Clamping the offset rather than each value
    // keeps the shape intact when the drag runs into a limit.
    void translate(float dv, float lo, float hi) {
        float vmin = base, vmax = base;
        if (!pts.empty()) {
            vmin = vmax = pts.front().v;
            for (const auto& p : pts) {
                vmin = std::min(vmin, p.v);
                vmax = std::max(vmax, p.v);
            }
        }
        dv = std::clamp(dv, lo - vmin, hi - vmax);
        base = std::clamp(base + dv, lo, hi);
        for (auto& p : pts)
            p.v += dv;
    }

    // Insert a point, keeping pts sorted by t. Returns its index. A point already
    // at `t` is moved rather than duplicated — two points sharing a time would
    // make the segment lookup ambiguous.
    size_t addPoint(int64_t t, float v) {
        size_t i = 0;
        while (i < pts.size() && pts[i].t < t)
            ++i;
        if (i < pts.size() && pts[i].t == t) {
            pts[i].v = v;
            return i;
        }
        pts.insert(pts.begin() + (std::ptrdiff_t)i, CurvePoint{ t, v });
        return i;
    }

private:
    // Slope at point `i`; `d` is the secant of the segment the caller is in, used
    // for the end points which have only one neighbour. Secants of opposite sign
    // (a local extremum) give a flat tangent, which is what makes it monotone.
    double tangent(size_t i, double d) const {
        double dPrev = d, dNext = d;
        if (i > 0) {
            const double hp = (double)(pts[i].t - pts[i - 1].t);
            if (hp > 0.0) dPrev = ((double)pts[i].v - (double)pts[i - 1].v) / hp;
        }
        if (i + 1 < pts.size()) {
            const double hn = (double)(pts[i + 1].t - pts[i].t);
            if (hn > 0.0) dNext = ((double)pts[i + 1].v - (double)pts[i].v) / hn;
        }
        if (dPrev * dNext <= 0.0)
            return 0.0;
        return 2.0 * dPrev * dNext / (dPrev + dNext); // harmonic mean
    }
};

// ---------------------------------------------------------------- audio volume

// Range of the per-clip volume curve, in dB. The floor is treated as -inf: a
// point dragged to the bottom of the lane is silence, not a very quiet signal.
inline constexpr float kVolMinDb = -60.0f;
inline constexpr float kVolMaxDb =   6.0f;

inline float dbToGain(float db) {
    return db <= kVolMinDb ? 0.0f : std::pow(10.0f, db / 20.0f);
}

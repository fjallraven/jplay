// Color-grading state for the Color tools panel.
//
// A single global/session grade is applied as one GPU post-pass over the final
// display-referred image (after OCIO for EXR, or the raw RGBA for video). This
// header holds only data + pure math (curve baking, presets); the GPU pass lives
// in GradeGpu, the UI in App_Grade.cpp.
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

namespace grade {

// ─── Tone / RGB curve ────────────────────────────────────────────────────────
// Control points in normalized [0,1]x[0,1], kept sorted by x. Default identity is
// the two endpoints (0,0)-(1,1). Baked to a 256-entry LUT via monotone cubic.
struct Curve {
    struct Pt { float x, y; };
    std::vector<Pt> pts{ { 0.0f, 0.0f }, { 1.0f, 1.0f } };

    bool isIdentity() const {
        return pts.size() == 2 && pts[0].x == 0.0f && pts[0].y == 0.0f &&
               pts[1].x == 1.0f && pts[1].y == 1.0f;
    }
    void reset() { pts = { { 0.0f, 0.0f }, { 1.0f, 1.0f } }; }

    // Insert a point, keeping x-sorted; returns its index.
    int addPoint(float x, float y) {
        x = std::clamp(x, 0.0f, 1.0f);
        y = std::clamp(y, 0.0f, 1.0f);
        int i = 0;
        while (i < (int)pts.size() && pts[i].x < x) ++i;
        pts.insert(pts.begin() + i, { x, y });
        return i;
    }

    // Sample the spline at x in [0,1] using Catmull-Rom (clamped to be safe).
    float eval(float x) const {
        if (pts.empty()) return x;
        if (x <= pts.front().x) return pts.front().y;
        if (x >= pts.back().x) return pts.back().y;
        int i = 0;
        while (i + 1 < (int)pts.size() && pts[i + 1].x < x) ++i;
        const Pt& p1 = pts[i];
        const Pt& p2 = pts[i + 1];
        const Pt& p0 = pts[i > 0 ? i - 1 : i];
        const Pt& p3 = pts[i + 2 < (int)pts.size() ? i + 2 : i + 1];
        float t = (x - p1.x) / std::max(p2.x - p1.x, 1e-6f);
        float t2 = t * t, t3 = t2 * t;
        float y = 0.5f * ((2.0f * p1.y) + (-p0.y + p2.y) * t +
                          (2.0f * p0.y - 5.0f * p1.y + 4.0f * p2.y - p3.y) * t2 +
                          (-p0.y + 3.0f * p1.y - 3.0f * p2.y + p3.y) * t3);
        return std::clamp(y, 0.0f, 1.0f);
    }
};

// ─── 3-way wheel zone ────────────────────────────────────────────────────────
// Marker position in the wheel as (x,y) in [-1,1] (hue = angle, sat = radius),
// plus a luma/exposure offset for that tonal zone in [-1,1].
struct Wheel {
    float x = 0.0f, y = 0.0f, luma = 0.0f;
    bool isIdentity() const { return x == 0.0f && y == 0.0f && luma == 0.0f; }
    void reset() { x = y = luma = 0.0f; }
};

// ─── Full grade state ────────────────────────────────────────────────────────
// Slider ranges are documented inline; the UI clamps to these. Defaults are all
// "no-op" so a fresh grade is identity.
struct State {
    bool enabled = true; // master on/off for the whole grade

    // Basic correction
    float temperature = 0.0f; // -100..100  (cool blue .. warm amber)
    float tint = 0.0f;        // -100..100  (green .. magenta)
    float gain = 0.0f;        // -5..5 f-stops (scene-linear multiply before OCIO; Nuke-style gain)
    float gamma = 1.0f;       // 0.1..4 midtone power (display-referred)
    float saturation = 0.0f;  // -100..100

    // RGB & color curves
    Curve curveMaster, curveR, curveG, curveB;

    // 3-way color wheels
    Wheel shadowsWheel, midWheel, highWheel;

    bool curvesIdentity() const {
        return curveMaster.isIdentity() && curveR.isIdentity() &&
               curveG.isIdentity() && curveB.isIdentity();
    }
    bool wheelsIdentity() const {
        return shadowsWheel.isIdentity() && midWheel.isIdentity() && highWheel.isIdentity();
    }
    // Whether the grade actually changes pixels (lets renderPlayer skip the pass).
    bool active() const {
        if (!enabled) return false;
        return temperature || tint || gain || gamma != 1.0f ||
               saturation || !curvesIdentity() || !wheelsIdentity();
    }
};

// Bake one curve to a 256-entry [0,1] LUT.
inline void bakeCurve(const Curve& c, std::array<float, 256>& out) {
    for (int i = 0; i < 256; ++i)
        out[i] = c.eval(i / 255.0f);
}

} // namespace grade

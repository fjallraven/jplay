// Player translation unit: the video-frame view (fit/zoom/pan math), the frame
// render with OCIO / grade / tech-check GPU passes, and the media-info overlay.
// Split out of App.cpp; all are App members.

#include "App.h"
#include "AppInternal.h"
#include "ImageSeq.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <functional>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace jplay;

namespace {

// Map an OCIO display name (ACES-style, e.g. "Rec.2100-PQ - Display") to the
// output transfer + primaries the HDR pass must undo. PQ/ST2084 and HLG → true HDR
// range; wide-gamut primaries are converted to scRGB (Rec.709). Unknown names fall
// back to sRGB / Rec.709.
void hdrDisplayEncoding(const std::string& d,
                        HdrColorPass::Encoding& enc, HdrColorPass::Primaries& prim) {
    auto has = [&](const char* s) { return d.find(s) != std::string::npos; };
    if (has("PQ") || has("2084"))       enc = HdrColorPass::Encoding::PQ;
    else if (has("HLG"))                enc = HdrColorPass::Encoding::HLG;
    else if (has("1886"))               enc = HdrColorPass::Encoding::Gamma24;
    else if (has("Gamma 2.2"))          enc = HdrColorPass::Encoding::Gamma22;
    else if (has("P3-D65"))             enc = HdrColorPass::Encoding::Gamma26; // DCI P3-D65 display
    else                                enc = HdrColorPass::Encoding::sRGB;    // sRGB, Display P3
    if (has("2100") || has("2020"))     prim = HdrColorPass::Primaries::Rec2020;
    else if (has("P3"))                 prim = HdrColorPass::Primaries::P3D65;
    else                                prim = HdrColorPass::Primaries::Rec709;
}

// sRGB transfer tables for the 8-bit dissolve path. Lerping sRGB-encoded values
// directly is the classic "dark dissolve" — a 50% mix of black and white lands
// near 0.22 in light instead of 0.5 — so the 8-bit blend decodes to linear, mixes
// there, and re-encodes. 4096 reverse steps keep the quantisation error below one
// 8-bit code, including near black where the curve is steepest.
constexpr int kEncSteps = 4096;
struct SrgbTables {
    float   toLinear[256];
    uint8_t toSrgb[kEncSteps];
};
const SrgbTables& srgbTables() {
    static const SrgbTables t = [] {
        SrgbTables s{};
        for (int i = 0; i < 256; ++i) {
            float c = i / 255.0f;
            s.toLinear[i] = c <= 0.04045f ? c / 12.92f
                                          : std::pow((c + 0.055f) / 1.055f, 2.4f);
        }
        for (int i = 0; i < kEncSteps; ++i) {
            float l = (float)i / (float)(kEncSteps - 1);
            float e = l <= 0.0031308f ? l * 12.92f
                                      : 1.055f * std::pow(l, 1.0f / 2.4f) - 0.055f;
            s.toSrgb[i] = (uint8_t)std::lround(std::clamp(e, 0.0f, 1.0f) * 255.0f);
        }
        return s;
    }();
    return t;
}

// Fit `src` into a dstW x dstH frame: centered, cropped where it overflows, black
// where it falls short. No resampling — pixels keep their 1:1 size, which is what
// makes this exact and cheap, and it matches how ExrSource already composites a
// data window into display-window space.
//
// The black padding is not a cop-out: because the blend is a*(1-w) + b*w, a padded
// secondary dims the primary toward black precisely where it has no coverage, so a
// partly-overlapping lower layer degrades region by region into the fade-to-black
// case instead of failing as a whole. That only holds if the padding is *opaque*
// black — see the alpha pass at the end.
void conformFrame(const Frame& src, int dstW, int dstH, Frame& out) {
    const size_t n = (size_t)dstW * dstH;
    out.width = dstW;
    out.height = dstH;
    out.rgba.assign(n * 4, 0);
    const bool haveLinear = src.linearRgb.size() >= (size_t)src.width * src.height * 3;
    if (haveLinear)
        out.linearRgb.assign(n * 3, Imath::half(0.0f));
    else
        out.linearRgb.clear();

    // Offset of src's origin within dst. Negative means src is larger on that axis
    // and gets cropped; positive means it is smaller and the margin stays black.
    const int offX = (dstW - src.width) / 2;
    const int offY = (dstH - src.height) / 2;
    const int x0 = std::max(0, offX), x1 = std::min(dstW, offX + src.width);
    const int y0 = std::max(0, offY), y1 = std::min(dstH, offY + src.height);
    if (x1 <= x0 || y1 <= y0)
        return; // no overlap at all: dst stays fully black
    const int copyW = x1 - x0;
    for (int y = y0; y < y1; ++y) {
        const int sy = y - offY;
        std::memcpy(out.rgba.data() + ((size_t)y * dstW + x0) * 4,
                    src.rgba.data() + ((size_t)sy * src.width + (x0 - offX)) * 4,
                    (size_t)copyW * 4);
        if (haveLinear)
            std::memcpy(out.linearRgb.data() + ((size_t)y * dstW + x0) * 3,
                        src.linearRgb.data() + ((size_t)sy * src.width + (x0 - offX)) * 3,
                        (size_t)copyW * 3 * sizeof(Imath::half));
    }
    // Every pixel opaque, padding included. texture_ is an alpha-blended RGBA
    // texture (SDL defaults alpha-bearing formats to SDL_BLENDMODE_BLEND), so a
    // translucent pad would let the black backdrop through *on top of* the blend's
    // own falloff and square it: the outgoing clip would darken as a*(1-w)^2
    // instead of a*(1-w) wherever the incoming frame is smaller. Opaque black is
    // exactly the coverage the blend is meant to model. Runs after the copy loop,
    // which brings the source's own alpha in with the memcpy.
    for (size_t i = 0; i < n; ++i)
        out.rgba[i * 4 + 3] = 255;
}

// Scale a frame toward black in place: the fade a clip performs when there is no
// lower layer to dissolve into. Scene-linear when the frame carries it (so the
// fade happens before the display transform, like the dissolve), and through the
// sRGB tables otherwise so the ramp is linear in light rather than in code value.
void fadeFrame(Frame& f, float opacity) {
    const float k = std::clamp(opacity, 0.0f, 1.0f);
    const size_t n = (size_t)f.width * f.height;
    if (f.rgba.size() >= n * 4) {
        const SrgbTables& t = srgbTables();
        uint8_t* p = f.rgba.data();
        for (size_t i = 0; i < n; ++i, p += 4)
            for (int c = 0; c < 3; ++c) {
                float lin = t.toLinear[p[c]] * k;
                p[c] = t.toSrgb[(int)std::lround(std::clamp(lin, 0.0f, 1.0f) * (kEncSteps - 1))];
            }
    }
    if (f.linearRgb.size() >= n * 3)
        for (size_t i = 0; i < n * 3; ++i)
            f.linearRgb[i] = Imath::half((float)f.linearRgb[i] * k);
}

// The buffer a frame should be handed to the display transform through, and how it
// is laid out: the most precise one it carries. The integer formats are read as
// code values normalised to 0..1, which is what an integer-encoded colour space
// expects; scene-linear half values go through unscaled.
} // namespace

// The highest-precision buffer a decoded frame carries, and the OcioGpu format
// that describes it. Shared with App_Layout.cpp (a Layout tile feeds its own
// frames through the same display transform), hence a member rather than another
// file-local helper.
void App::frameInput(const Frame& f, const void*& pixels, OcioGpu::InputFormat& fmt) {
    const size_t n = (size_t)f.width * f.height;
    if (f.linearRgb.size() >= n * 3) {
        pixels = f.linearRgb.data();
        fmt = OcioGpu::InputFormat::SceneLinearHalf;
    } else if (f.rgba16.size() >= n * 4) {
        pixels = f.rgba16.data();
        fmt = OcioGpu::InputFormat::Rgba16;
    } else if (f.rgba.size() >= n * 4) {
        pixels = f.rgba.data();
        fmt = OcioGpu::InputFormat::Rgba8;
    } else {
        pixels = nullptr;
        fmt = OcioGpu::InputFormat::Rgba8;
    }
}

namespace {

// Cross-dissolve two decoded frames into `out`: out = a*(1-mix) + b*mix. Runs only
// on the frames a transition actually covers, so the per-pixel cost is confined to
// those. The blend happens on the frame data, upstream of the display transform,
// which is what makes it correct: the OCIO pass, the grade and the tech-check
// overlay then all see one ordinary frame and need to know nothing about cuts.
//
// Every buffer present on both sides is mixed. The 8-bit one always is (every
// source supplies it) and goes through the tables above; rgba16 takes the same
// treatment computed rather than tabulated; linearRgb is scene-linear already and
// mixes directly. A buffer only one side carries is dropped from the result, which
// leaves the 8-bit mix standing in for it.
//
// The two are assumed to share a colour space by the time they get here: the caller
// blends only when both halves are read in the same one, and otherwise brings each
// through its own display transform first (see renderPlayer's blend site).
//
// Requires both frames to agree on size; the caller runs a mismatched secondary
// through conformFrame first, so the primary always defines the canvas (which is
// also what texture_ is sized to). False only on a degenerate/empty frame.
bool blendFrames(const Frame& a, const Frame& b, float mix, Frame& out) {
    if (a.width != b.width || a.height != b.height || a.width <= 0 || a.height <= 0)
        return false;
    const size_t n = (size_t)a.width * a.height;
    if (a.rgba.size() < n * 4 || b.rgba.size() < n * 4)
        return false;
    const float w = std::clamp(mix, 0.0f, 1.0f);
    out.width = a.width;
    out.height = a.height;

    const SrgbTables& t = srgbTables();
    out.rgba.resize(n * 4);
    const uint8_t* pa = a.rgba.data();
    const uint8_t* pb = b.rgba.data();
    uint8_t* po = out.rgba.data();
    for (size_t i = 0; i < n; ++i, pa += 4, pb += 4, po += 4) {
        for (int k = 0; k < 3; ++k) {
            float lin = t.toLinear[pa[k]] * (1.0f - w) + t.toLinear[pb[k]] * w;
            int idx = (int)std::lround(std::clamp(lin, 0.0f, 1.0f) * (kEncSteps - 1));
            po[k] = t.toSrgb[idx];
        }
        po[3] = (uint8_t)std::lround(pa[3] * (1.0f - w) + pb[3] * w); // opacity, not light
    }

    // The 16-bit buffer, when both sides carry one. Same intent as the 8-bit mix
    // above — blend in light, not in code values — but computed rather than
    // tabulated: 65536 entries is not worth a table for a path only a dissolve
    // between two deep sources takes.
    if (a.rgba16.size() >= n * 4 && b.rgba16.size() >= n * 4) {
        out.rgba16.resize(n * 4);
        const uint16_t* qa = a.rgba16.data();
        const uint16_t* qb = b.rgba16.data();
        uint16_t* qo = out.rgba16.data();
        auto toLin = [](float c) {
            return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
        };
        auto toEnc = [](float c) {
            return c <= 0.0031308f ? c * 12.92f : 1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f;
        };
        for (size_t i = 0; i < n; ++i, qa += 4, qb += 4, qo += 4) {
            for (int k = 0; k < 3; ++k) {
                float lin = toLin(qa[k] * (1.0f / 65535.0f)) * (1.0f - w) +
                            toLin(qb[k] * (1.0f / 65535.0f)) * w;
                qo[k] = (uint16_t)std::lround(
                    std::clamp(toEnc(std::clamp(lin, 0.0f, 1.0f)), 0.0f, 1.0f) * 65535.0f);
            }
            qo[3] = (uint16_t)std::lround(qa[3] * (1.0f - w) + qb[3] * w);
        }
    } else {
        out.rgba16.clear();
    }

    if (a.linearRgb.size() >= n * 3 && b.linearRgb.size() >= n * 3) {
        out.linearRgb.resize(n * 3);
        const Imath::half* la = a.linearRgb.data();
        const Imath::half* lb = b.linearRgb.data();
        Imath::half* lo = out.linearRgb.data();
        for (size_t i = 0; i < n * 3; ++i)
            lo[i] = Imath::half((float)la[i] * (1.0f - w) + (float)lb[i] * w);
    } else {
        out.linearRgb.clear();
    }
    return true;
}

constexpr SDL_Color kBlack         {   0,   0,   0, 255 }; // player backdrop / frame clear
constexpr SDL_Color kLoadingBg     { 160,  40,  40, 220 }; // "LOADING" badge fill
constexpr SDL_Color kLoadingText   { 255, 255, 255, 255 }; // "LOADING" badge text
constexpr SDL_Color kStatusText    { 255, 210,  90, 255 }; // transient status message
constexpr SDL_Color kStatusWarnBg  { 150,  30,  30, 255 }; // warning status: badge fill
constexpr SDL_Color kStatusWarnText{ 255, 235, 235, 255 }; // warning status: text
constexpr SDL_Color kInfoPanelBg   {  16,  17,  20, 225 }; // media-info overlay panel fill
constexpr SDL_Color kInfoPanelEdge {  90, 130, 200, 255 }; // media-info overlay panel border
constexpr SDL_Color kInfoTitle     { 120, 180, 255, 255 }; // media-info overlay title
constexpr SDL_Color kInfoBody      { 215, 218, 225, 255 }; // media-info overlay body text

inline void setColor(SDL_Renderer* r, SDL_Color c) {
    SDL_SetRenderDrawColor(r, c.r, c.g, c.b, c.a);
}

// Append a soft-edged disc (opaque core + feathered rim) to the geometry buffers,
// in the given color. Used for a single-point pencil mark (a click with no drag).
void emitDot(std::vector<SDL_Vertex>& verts, std::vector<int>& idx,
             float cx, float cy, float hw, float feather,
             float cr, float cg, float cb) {
    const int K = 16;
    int c = (int)verts.size();
    SDL_Vertex ctr{}; ctr.position = { cx, cy }; ctr.color = { cr, cg, cb, 1 };
    verts.push_back(ctr);
    int ring1 = (int)verts.size();
    for (int i = 0; i <= K; ++i) {
        float ang = (float)i / K * 6.2831853f;
        SDL_Vertex v{}; v.position = { cx + std::cos(ang) * hw, cy + std::sin(ang) * hw };
        v.color = { cr, cg, cb, 1 }; verts.push_back(v);
    }
    int ring2 = (int)verts.size();
    for (int i = 0; i <= K; ++i) {
        float ang = (float)i / K * 6.2831853f, r = hw + feather;
        SDL_Vertex v{}; v.position = { cx + std::cos(ang) * r, cy + std::sin(ang) * r };
        v.color = { cr, cg, cb, 0 }; verts.push_back(v);
    }
    for (int i = 0; i < K; ++i) {
        idx.push_back(c);        idx.push_back(ring1 + i); idx.push_back(ring1 + i + 1);
        idx.push_back(ring1 + i); idx.push_back(ring2 + i); idx.push_back(ring2 + i + 1);
        idx.push_back(ring1 + i); idx.push_back(ring2 + i + 1); idx.push_back(ring1 + i + 1);
    }
}

// Clean up a projected stroke, in three stages: low-pass the sample positions,
// resample through an approximating cubic B-spline, blur the result. Pointer samples
// arrive at the OS polling rate, so a quick stroke lands them tens of pixels apart
// and the raw polyline reads as straight runs with visible corners; the spline curves
// between them, with the subdivision count following segment length in screen px so
// zooming in adds detail instead of magnifying facets. The spline approximates rather
// than interpolates, so the curve cuts inside the polyline instead of threading every
// sample: the stroke follows the gesture rather than the individual pointer reads.
void smoothStroke(std::vector<float>& px, std::vector<float>& py, std::vector<float>& phw) {
    size_t n = px.size();
    if (n < 3)
        return;

    // Low-pass the sample positions before interpolating them. Pointer coordinates
    // arrive quantised to the integer pixel grid, so a spline through them threads
    // that stair-step — and the hand's own tremor — straight into the stroke edge as
    // a visible ripple. Three passes of a 5-point Savitzky-Golay quadratic fit
    // average the noise out while keeping a deliberate corner a corner; a box or
    // binomial average strong enough to clean up the same wobble rounds the corners
    // off instead. The end samples stay put, so the stroke still starts and ends
    // exactly where the cursor did.
    {
        std::vector<float> ax(n), ay(n);
        for (int pass = 0; pass < 3; ++pass) {
            ax = px; ay = py;
            for (size_t i = 2; i + 2 < n; ++i) {
                px[i] = (-3.0f * ax[i - 2] + 12.0f * ax[i - 1] + 17.0f * ax[i]
                         + 12.0f * ax[i + 1] - 3.0f * ax[i + 2]) / 35.0f;
                py[i] = (-3.0f * ay[i - 2] + 12.0f * ay[i - 1] + 17.0f * ay[i]
                         + 12.0f * ay[i + 1] - 3.0f * ay[i + 2]) / 35.0f;
            }
            // Shoulders, where the 5-point window does not fit: 3-point average.
            for (size_t i : { (size_t)1, n - 2 }) {
                px[i] = 0.25f * ax[i - 1] + 0.5f * ax[i] + 0.25f * ax[i + 1];
                py[i] = 0.25f * ay[i - 1] + 0.5f * ay[i] + 0.25f * ay[i + 1];
            }
        }
    }

    // Resample onto a uniform cubic B-spline over the samples as control points.
    // Duplicating each end twice pins the curve to the first and last sample; every
    // interior point becomes a weighted average of four, which is what rounds off the
    // curvature breaks an interpolating spline leaves at each knot.
    {
        std::vector<float> cx, cy, chw;
        cx.reserve(n + 4); cy.reserve(n + 4); chw.reserve(n + 4);
        for (int k = 0; k < 2; ++k) { cx.push_back(px[0]); cy.push_back(py[0]); chw.push_back(phw[0]); }
        for (size_t i = 0; i < n; ++i) { cx.push_back(px[i]); cy.push_back(py[i]); chw.push_back(phw[i]); }
        for (int k = 0; k < 2; ++k) { cx.push_back(px[n - 1]); cy.push_back(py[n - 1]); chw.push_back(phw[n - 1]); }
        const size_t m = cx.size();
        const float step = 2.5f; // spacing between emitted samples (screen px)
        const int maxSub = 24;   // cap per span
        std::vector<float> ox, oy, ohw;
        ox.reserve(n * 6); oy.reserve(n * 6); ohw.reserve(n * 6);
        ox.push_back(px[0]); oy.push_back(py[0]); ohw.push_back(phw[0]);
        for (size_t i = 0; i + 3 < m; ++i) {
            auto eval = [&](float u, float& rx, float& ry, float& rh) {
                float u2 = u * u, u3 = u2 * u;
                float b0 = (1.0f - 3.0f * u + 3.0f * u2 - u3) / 6.0f;
                float b1 = (4.0f - 6.0f * u2 + 3.0f * u3) / 6.0f;
                float b2 = (1.0f + 3.0f * u + 3.0f * u2 - 3.0f * u3) / 6.0f;
                float b3 = u3 / 6.0f;
                rx = b0 * cx[i] + b1 * cx[i + 1] + b2 * cx[i + 2] + b3 * cx[i + 3];
                ry = b0 * cy[i] + b1 * cy[i + 1] + b2 * cy[i + 2] + b3 * cy[i + 3];
                rh = b0 * chw[i] + b1 * chw[i + 1] + b2 * chw[i + 2] + b3 * chw[i + 3];
            };
            // Subdivide by the span's own chord, not the control-point spacing: the
            // duplicated end control points make those two spans zero-length.
            float sx, sy, sh, ex, ey, eh;
            eval(0.0f, sx, sy, sh);
            eval(1.0f, ex, ey, eh);
            int sub = (int)std::ceil(std::hypot(ex - sx, ey - sy) / step);
            sub = std::clamp(sub, 1, maxSub);
            for (int k = 1; k <= sub; ++k) {
                float rx, ry, rh;
                eval((float)k / (float)sub, rx, ry, rh);
                ox.push_back(rx); oy.push_back(ry); ohw.push_back(rh);
            }
        }
        px = std::move(ox); py = std::move(oy); phw = std::move(ohw);
        n = px.size();
    }

    // Blur the curve. Doing this after the resample rather than on the raw samples is
    // what makes it usable: the spacing is now uniform, so the pass count is a fixed
    // radius in pixels and a flicked stroke gets the same treatment as a slow one.
    // Filtering the raw samples instead would smooth by a radius proportional to
    // cursor speed, which visibly shrinks fast loops while barely touching slow ones.
    if (n > 4) {
        std::vector<float> ax(n), ay(n);
        for (int pass = 0; pass < 8; ++pass) {
            ax = px; ay = py;
            for (size_t i = 1; i + 1 < n; ++i) {
                px[i] = 0.25f * ax[i - 1] + 0.5f * ax[i] + 0.25f * ax[i + 1];
                py[i] = 0.25f * ay[i - 1] + 0.5f * ay[i] + 0.25f * ay[i + 1];
            }
        }
    }
}

} // namespace

// The fit scale: what dispW x h is multiplied by at frameZoom_ == 1. Normally the
// whole image fits inside `view`. With Fit View on and a matte set, the *masked*
// region fits instead, so the bars land outside the view and the framing shows with
// no black edges. That region is centred in the image, so centring the image still
// centres it and only this scale has to change.
float App::fitScaleIn(const SDL_FRect& view, float dispW, float h) const {
    if (dispW <= 0.0f || h <= 0.0f)
        return 0.0f;
    float cw = dispW, ch = h;
    const double R = timeline_.letterboxRatio;
    if (letterboxFitView_ && R > 0.0) {
        const double ar = (double)dispW / (double)h;
        if (R > ar)
            ch = (float)((double)dispW / R); // top/bottom bars cropped away
        else if (R < ar)
            cw = (float)((double)h * R);     // left/right bars cropped away
    }
    return std::min(view.w / cw, view.h / ch);
}

// Fit dispW x h into `view`, aspect-preserving and centred, then scale by
// frameZoom_ and shift by the pan offset. One zoom/pan for every caller, which is
// what keeps the Layout tiles zoomed together: they differ only in `view`, and
// auto-packing gives them all the same cell size, so the pan (in pixels) means
// the same thing in each.
SDL_FRect App::fitImageIn(const SDL_FRect& view, float dispW, float h) const {
    float scale = fitScaleIn(view, dispW, h) * frameZoom_;
    float dw = dispW * scale, dh = h * scale;
    return { view.x + (view.w - dw) * 0.5f + framePanX_,
             view.y + (view.h - dh) * 0.5f + framePanY_, dw, dh };
}

// Where the player image lands on screen. Caller guarantees texW_/texH_ > 0.
SDL_FRect App::frameDstRect() const {
    return fitImageIn(playerRect_, texDispW(), (float)texH_);
}

// The area the program image is fitted into: the whole player area normally, its
// own tile's cell on the Layout stage.
SDL_FRect App::programViewRect() const {
    if (layoutView() && !layoutTiles_.empty())
        return layoutProgramCell();
    return playerRect_;
}

// Where the *program* image lands. Everything anchored to the program image goes
// through this so it follows the program into its tile.
SDL_FRect App::programDstRect() const {
    return fitImageIn(programViewRect(), texDispW(), (float)texH_);
}

// The area the zoom/pan is anchored in — the tile under the cursor on the Layout
// stage, so a zoom toward a point pins that point in the tile it was aimed at.
SDL_FRect App::frameViewRect(float cx, float cy) const {
    if (layoutView()) {
        int t = layoutTileAt(cx, cy);
        if (t >= 0)
            return layoutTiles_[t];
        if (!layoutTiles_.empty())
            return layoutProgramCell();
    }
    return playerRect_;
}

void App::fitFrameView() {
    frameZoom_ = 1.0f;
    framePanX_ = 0.0f;
    framePanY_ = 0.0f;
}

// Zoom the frame toward the cursor: keep whatever image point sits under (cx,cy)
// pinned there as the scale changes. Snaps back to centered once fully zoomed out.
void App::zoomFrameAt(float cx, float cy, float factor) {
    if (texW_ <= 0 || texH_ <= 0)
        return;
    const SDL_FRect view = frameViewRect(cx, cy);
    SDL_FRect before = fitImageIn(view, texDispW(), (float)texH_);
    float u = before.w > 0 ? (cx - before.x) / before.w : 0.5f;
    float v = before.h > 0 ? (cy - before.y) / before.h : 0.5f;
    frameZoom_ = std::clamp(frameZoom_ * factor, 1.0f, 32.0f);
    if (frameZoom_ <= 1.0f) {
        framePanX_ = 0.0f;
        framePanY_ = 0.0f;
        return;
    }
    float fit = fitScaleIn(view, texDispW(), (float)texH_);
    float scale = fit * frameZoom_;
    float dw = texDispW() * scale, dh = texH_ * scale;
    framePanX_ = cx - u * dw - (view.x + (view.w - dw) * 0.5f);
    framePanY_ = cy - v * dh - (view.y + (view.h - dh) * 0.5f);
}

// Scale the frame so one image pixel covers exactly `mult` device pixels, keeping
// the image point under (cx,cy) pinned there. frameDstRect works in logical units,
// so the target scale is mult/gDeviceScale rather than mult — on a HiDPI display (or
// a non-1.0 UI Scale preference) a logical unit is more than one real pixel. Zoom
// can't go below the fit floor, so an image smaller than the player area lands at
// fit instead, still upscaled.
void App::zoomFramePixelScale(float cx, float cy, int mult) {
    if (texW_ <= 0 || texH_ <= 0)
        return;
    const SDL_FRect view = frameViewRect(cx, cy);
    float fit = fitScaleIn(view, texDispW(), (float)texH_);
    if (fit <= 0.0f)
        return;
    float scale = gDeviceScale > 0.0f ? gDeviceScale : 1.0f;
    float target = (float)mult / (fit * scale);
    zoomFrameAt(cx, cy, target / frameZoom_);
    // The zoom clamp can refuse the target (small image → below fit, huge image in a
    // tiny player → past 32x); say what was actually reached instead of claiming n:1.
    if (std::fabs(frameZoom_ - target) <= target * 0.001f)
        setStatus("ZOOM " + std::to_string(mult) + ":1");
    else
        setStatus("ZOOM " + std::to_string((int)std::lround(frameZoom_ * fit * scale * 100.0f)) + "%");
}

// Encode the current frame view for the sync stream: the zoom factor plus the
// normalized image point (u,v in [0,1]) currently sitting at the player center.
// Expressing pan this way (rather than raw pixels) makes it window-size-agnostic
// so a spectator reproduces the host's framing on a differently sized display.
void App::frameViewCenter(float& zoom, float& u, float& v) const {
    zoom = frameZoom_;
    if (texW_ <= 0 || texH_ <= 0 || frameZoom_ <= 1.0f) { u = v = 0.5f; return; }
    // The program's own area, so a host on the Layout stage describes what its
    // program tile shows rather than a framing of the whole stage.
    const SDL_FRect view = programViewRect();
    // The same fit the pan was built against — with Fit View on that is the masked
    // region's, so u/v must be derived from it or the framing decodes wrong.
    float fit = fitScaleIn(view, texDispW(), (float)texH_);
    float dw = texDispW() * fit * frameZoom_, dh = texH_ * fit * frameZoom_;
    u = 0.5f - framePanX_ / dw;
    v = 0.5f - framePanY_ / dh;
}

// Apply a frame view received from the host (spectator side): set the zoom and
// derive the local pan that puts (u,v) back at this player's center.
void App::applyFrameView(float zoom, float u, float v) {
    frameZoom_ = std::clamp(zoom, 1.0f, 32.0f);
    if (texW_ <= 0 || texH_ <= 0 || frameZoom_ <= 1.0f) {
        framePanX_ = framePanY_ = 0.0f;
        return;
    }
    const SDL_FRect view = programViewRect();
    float fit = fitScaleIn(view, texDispW(), (float)texH_);
    float dw = texDispW() * fit * frameZoom_, dh = texH_ * fit * frameZoom_;
    framePanX_ = (0.5f - u) * dw;
    framePanY_ = (0.5f - v) * dh;
}

// True when (x,y) is over the video frame area. The inspector overlay does not
// carve anything out: it only claims clicks on its close button (handled in the
// event loop), so the frame stays live underneath it.
bool App::inPlayerView(float x, float y) const {
    return x >= playerRect_.x && x < playerRect_.x + playerRect_.w &&
           y >= playerRect_.y && y < playerRect_.y + playerRect_.h;
}

// ------------------------------------------------- drop-action chooser (frame)
// Boxes over the video frame, shown while media is dragged onto it (a bin row or
// an OS file drag). They name the things a drop on the frame can mean, so the
// gesture no longer implies just one of them.

// Which actions are on offer, in the order the boxes are laid out. The three that
// stand something new up out of the drag sit next to each other on one row (see
// playerDropBoxRects); the ones that edit the cut are stacked under them.
//
// The comparison stages offer the one that means anything there: every tile on the
// Layout and every entry of the Stack is a comparison image, so a dropped source
// becomes another one. Opening it would leave the stage the user just set up,
// "Replace Clip" would edit a copy that goes away with the stage, and the Create
// actions would throw it away outright.
int App::playerDropActions(int out[kPlayerDropBoxes]) const {
    if (compareStage()) {
        out[0] = 2;
        return 1;
    }
    // A multi-source drag has no single thing to open and nothing to replace with:
    // both of those actions consume one item and would ignore the rest. Only a bin
    // drag knows its count up front; an OS drag hands over the paths at the drop.
    const bool one = !(binDragging_ && binDragPaths_.size() > 1);
    // A source view is a throwaway one-clip sequence, so editing it is pointless:
    // the clip is the view and both cut edits would go away with it.
    const bool cut = !sourceViewActive();
    int n = 0;
    if (one)
        out[n++] = 0; // Open
    out[n++] = 3;     // Create Sequence  ]
    out[n++] = 4;     // Create Layout    ]- one row
    out[n++] = 5;     // Create Stack     ]
    if (one && cut)
        out[n++] = 1; // Replace Clip
    if (cut)
        out[n++] = 2; // Add Clip to Track
    return n;
}

// Lay the boxes out centred in the frame view, largest that comfortably fits.
// Returns the box count, or 0 when the chooser does not apply: with nothing on the
// timeline the launcher owns the frame (a drop there keeps the old "just show me
// this" behaviour), and a very small frame has no room for boxes this size.
int App::playerDropBoxRects(SDL_FRect out[kPlayerDropBoxes]) const {
    if (launcherVisible())
        return 0;
    int actions[kPlayerDropBoxes];
    const int n = playerDropActions(actions);
    // Row each box sits on: one per box, except the Create actions, which share one
    // whenever more than one of them is on offer. They are laid out consecutively
    // (see playerDropActions), so "the previous box is one too" is the whole test.
    auto isCreate = [](int a) { return a >= 3; };
    int boxRow[kPlayerDropBoxes];
    int rows = 0;
    for (int i = 0; i < n; ++i)
        boxRow[i] = (i > 0 && isCreate(actions[i]) && isCreate(actions[i - 1])) ? rows - 1
                                                                               : rows++;
    const float availW = playerRect_.w;

    const float gap  = 12.0f * dpiScale;
    const float boxH = std::clamp(playerRect_.h * 0.17f, 40.0f * dpiScale, 76.0f * dpiScale);
    const float totalH = boxH * rows + gap * (rows - 1);
    if (availW < 200.0f * dpiScale || totalH > playerRect_.h)
        return 0;

    const float boxW = std::min(availW * 0.72f, 560.0f * dpiScale);
    const float x = playerRect_.x + (availW - boxW) * 0.5f;
    const float top = playerRect_.y + (playerRect_.h - totalH) * 0.5f;
    for (int i = 0; i < n; ++i) {
        const int row = boxRow[i];
        // Boxes sharing a row split the width between them, gap included, so the
        // row lines up with the full-width ones above and below it.
        int inRow = 0, atInRow = 0;
        for (int j = 0; j < n; ++j) {
            if (boxRow[j] != row)
                continue;
            if (j == i)
                atInRow = inRow;
            ++inRow;
        }
        const float w = (boxW - gap * (inRow - 1)) / inRow;
        out[i] = SDL_FRect{ x + atInRow * (w + gap), top + row * (boxH + gap), w, boxH };
    }
    return n;
}

bool App::playerDropChooserApplies() const {
    SDL_FRect boxes[kPlayerDropBoxes];
    return playerDropBoxRects(boxes) > 0;
}

// "Replace Clip" needs a clip under the frame indicator to replace; the other two
// always apply. (An audio file has nothing to replace either, but a drag's kind is
// only known at drop time for a bin drag, so applyPlayerDrop rejects that.)
bool App::playerDropBoxEnabled(int action) const {
    return action != 1 || getTopMostClipAtFrame(timeline_.playhead) != nullptr;
}

int App::playerDropBoxAt(float x, float y) const {
    SDL_FRect boxes[kPlayerDropBoxes];
    int actions[kPlayerDropBoxes];
    const int n = playerDropBoxRects(boxes);
    if (!inPlayerView(x, y) || n <= 0)
        return -1;
    playerDropActions(actions);
    for (int i = 0; i < n; ++i)
        if (inRect(boxes[i], x, y) && playerDropBoxEnabled(actions[i]))
            return actions[i];
    return -1;
}

void App::renderPlayerDropBoxes() {
    SDL_FRect boxes[kPlayerDropBoxes];
    int actions[kPlayerDropBoxes];
    const int n = playerDropBoxRects(boxes);
    if (!playerDropActive_ || n <= 0)
        return;
    playerDropActions(actions);

    // The dragged file's name is only known for a bin drag: an OS drag does not
    // hand over the path until the drop itself, so the box degrades to "View".
    std::string viewLabel = "View";
    if (binDragging_ && !binDragPaths_.empty()) {
        const std::string& path = binDragPaths_.front();
        fs::path p(path);
        viewLabel += ": " + hashSeqStem(p.stem().string(),
                                        mediaTypeForPath(path) == ClipType::ImageSequence)
                   + p.extension().string();
    }
    // Indexed by action id, not by box order.
    const std::string labels[kPlayerDropBoxes] = {
        viewLabel, "Replace Clip",
        stackView()  ? "Add to Stack"
        : layoutView() ? "Add to Layout"
                       : "Add Clip to Track",
        "Create Sequence", "Create Layout", "Create Stack" };

    const float textScale = 1.5f; // the header font, stretched: these boxes are large
    for (int i = 0; i < n; ++i) {
        const int act  = actions[i];
        const bool on  = playerDropBoxEnabled(act);
        const bool hot = on && playerDropHover_ == act;
        SDL_SetRenderDrawColor(renderer_, hot ? 44 : 16, hot ? 60 : 16, hot ? 94 : 20,
                               hot ? 240 : 215);
        jplay::fillRect(renderer_, &boxes[i]);
        const SDL_Color border = !on  ? SDL_Color{ 64, 66, 74, 210 }
                               : hot  ? SDL_Color{ 150, 190, 255, 255 }
                                      : SDL_Color{ 96, 104, 124, 230 };
        SDL_SetRenderDrawColor(renderer_, border.r, border.g, border.b, border.a);
        jplay::drawRect(renderer_, &boxes[i]);
        if (hot) { // second stroke inside, so the hovered box reads as armed
            SDL_FRect in = { boxes[i].x + 1, boxes[i].y + 1, boxes[i].w - 2, boxes[i].h - 2 };
            jplay::drawRect(renderer_, &in);
        }

        // Trim the label until it fits the box (long sequence names in "View:").
        std::string label = labels[act];
        const float maxW = boxes[i].w - 24.0f * dpiScale;
        while (label.size() > 4 &&
               headerFont_.measure(renderer_, label.c_str()) * textScale > maxW)
            label = label.substr(0, label.size() - 4) + "...";

        const float tw = headerFont_.measure(renderer_, label.c_str()) * textScale;
        const float th = headerFont_.lineHeight() * textScale;
        const SDL_Color textCol = !on  ? SDL_Color{ 118, 122, 130, 220 }
                                : hot  ? SDL_Color{ 244, 248, 255, 255 }
                                       : SDL_Color{ 218, 224, 238, 240 };
        headerFont_.draw(renderer_, boxes[i].x + (boxes[i].w - tw) * 0.5f,
                         boxes[i].y + (boxes[i].h - th) * 0.5f, textCol, label.c_str(),
                         textScale);
    }
}

void App::applyPlayerDrop(int action, const std::string& path, bool first) {
    switch (action) {
    case 0:
        // Same shortcut a double-click on a source row performs: a throwaway source
        // view, which leaves the cut untouched. It holds one clip, so anything after
        // the leading item of a multi-item drag would just replace the view.
        if (first)
            openSourceView(path);
        break;
    case 1: {
        // Single-target action: it swaps the media behind one clip, so anything
        // after the leading item of a multi-item drag has nowhere to go.
        if (!first)
            break;
        if (isAudioPath(path)) {
            setStatus("CANNOT REPLACE A CLIP WITH AUDIO", 4000);
            break;
        }
        const Clip* top = getTopMostClipAtFrame(timeline_.playhead);
        Clip* clip = top ? clipById(top->id) : nullptr;
        if (!clip) {
            setStatus("NO CLIP UNDER THE FRAME INDICATOR", 4000);
            break;
        }
        // A replace also retimes the clip and shifts its followers, so the whole
        // clip/shot state is snapshotted rather than just positions.
        const std::string prevMediaId = clip->mediaId;
        ContentSnapshot before = captureContent();
        replaceClipMedia(*clip, path); // keeps the clip's position and in/out
        const bool swapped = clip->mediaId != prevMediaId; // replace bails on open failure
        timeline_.repackSequences();
        timeline_.clampPlayhead();
        if (swapped)
            pushContentUndo("REPLACE CLIP", before);
        hostSnapshotDirty_ = true;  // re-push the project to spectators if hosting
        if (gridView())
            startClipThumbnails(); // the switched clip needs a new thumbnail key
        break;
    }
    case 2: {
        ContentSnapshot before = captureContent();
        if (addMediaAlignedToCurrentClip(path))
            pushContentUndo("ADD CLIP", before);
        break;
    }
    case 3: {
        // A cut of the drag's own: the leading item makes the sequence and scopes
        // to it, the rest append after it. Unlike the two views above this edits the
        // project, so a scratch view up at the time is put away first: a real
        // sequence must not be pushed in behind one, which is always the last in the
        // vector and is what addSequence's scoping counts on.
        if (first) {
            dropScratchView();
            addSequence(); // creates it, makes it active and scopes to it
        }
        ContentSnapshot before = captureContent();
        const int track = firstTrackForKind(isAudioPath(path));
        if (addMediaFileAt(path, track, timeline_.trackEnd(track)) >= 0)
            pushContentUndo(first ? "CREATE SEQUENCE" : "ADD CLIP", before);
        break;
    }
    case 4:
    case 5: {
        // The comparison stages, filled from the drag rather than from the cut: the
        // same scratch sequence, so the cut is untouched and there is nothing to
        // undo. One row per item, since a row is a tile on the Layout and a step of
        // the rotation on the Stack. Action 4 lands on the Layout, 5 on the Stack;
        // everything up to the last line is the same for both.
        if (isAudioPath(path)) {
            setStatus(action == 5 ? "AUDIO HAS NO IMAGE TO STACK"
                                  : "AUDIO HAS NO TILE TO LAY OUT", 4000);
            break;
        }
        if (!layoutSeqActive()) // the leading item that opens stands the stage up
            beginScratchSequence(action == 5 ? "STACK" : "LAYOUT", ScratchKind::Layout);
        const int idx = (int)timeline_.sequences.size() - 1; // the scratch is last
        const int track = (int)timeline_.sequences[idx].clips.size();
        scratchTrackCount_ = track + 1; // the row this clip is about to take
        if (addMediaFileAt(path, track, timeline_.seqRegions()[idx].start,
                           /*queryAudio=*/false, /*srcIn=*/-1, /*srcDur=*/-1,
                           /*keepView=*/true) < 0) {
            scratchTrackCount_ = std::max(track, 1); // the row went unused
            if (track == 0)
                dropScratchView(); // nothing to lay out; addMediaFileAt set the status
            break;
        }
        // The scope was taken while the sequence was still empty, so the zoom it
        // chose has nothing to do with what is in it now.
        fitToFilteredSequence();
        setPlayhead(timeline_.seqRegions()[idx].start);
        // A no-op once we are on it, which is what makes the rest of a multi-item
        // drag land on the stage the leading item opened.
        setPlayerStage(action == 5 ? PlayerStage::Stack : PlayerStage::Layout);
        break;
    }
    default:
        break;
    }
}

// Add `path` starting on the same frame the clip under the indicator starts on,
// so the two line up frame-for-frame for an A/B comparison. It goes on the first
// row (top down) that can take its kind and has that span free — on the Layout
// stage, the first row that is empty outright, since there a row is a tile. When
// no row qualifies a new one is appended. A multi-item drag therefore stacks each
// item on its own row, all sharing the start frame.
//
// A Clip Source drag lines up further: it also inherits the reference clip's
// source in/out, so a version whose media covers more than the cut shows the same
// frames rather than its whole extent. A bin drag carries an unrelated source with
// no range to inherit, so it keeps taking the whole thing.
bool App::addMediaAlignedToCurrentClip(const std::string& path) {
    // A Clip Source drag names the clip the panel described; otherwise align to
    // whatever the indicator sits on. In a gap there is nothing to align to at all.
    const Clip* ref = binDragPickerClipId_ >= 0
                          ? timeline_.findClipById(binDragPickerClipId_) : nullptr;
    const bool inheritRange = ref != nullptr;
    if (!ref)
        ref = getTopMostClipAtFrame(timeline_.playhead);
    int64_t start = ref ? ref->timelineStart : timeline_.playhead;

    // The span decides which rows are free, so the media has to be open first.
    // addMediaFileAt reuses this pool entry rather than opening it again.
    auto media = ensureMedia(path, mediaTypeForPath(path));
    if (!media)
        return false; // ensureMedia set the status
    int64_t dur = std::max<int64_t>(media->info().frameCount, 1);
    int64_t srcIn = -1, srcDur = -1;
    if (inheritRange) {
        int64_t shift = 0;
        if (alignedSourceRange(*ref, *media, srcIn, srcDur, shift)) {
            start += shift;
            dur = srcDur;
        } else {
            srcIn = srcDur = -1; // no overlap: the whole source, as a bin drag gets
        }
    }

    const Timeline::TrackKind want = isAudioPath(path) ? Timeline::TrackKind::Audio
                                                       : Timeline::TrackKind::Video;
    const int n = std::max(trackCount(), 1);
    int track = -1;
    for (int t = 0; t < n && track < 0; ++t) {
        Timeline::TrackKind k = timeline_.trackKind(t);
        if (k != want && k != Timeline::TrackKind::Empty)
            continue;
        // On a comparison stage a row *is* a tile / a step of the stack, so it wants
        // a row of its own: a second clip on one would share that cell and only ever
        // be on screen for the frames the first does not cover. Elsewhere a free
        // span is enough.
        if (compareStage() ? timeline_.trackEmpty(t)
                           : !timeline_.trackHasOverlap(t, start, dur, -1))
            track = t;
    }
    if (track < 0) {
        track = n;                       // every row is taken across this span
        mutableTrackCount() = n + 1;     // ...so give the clip a new one, on the
                                         // stack that is on screen (a Layout view
                                         // grows its own rows, not the project's)
    }
    // it sets the status either way. keepView: the drop was aimed at the frame,
    // not at the timeline, so it has no business moving the user's zoom/pan.
    return addMediaFileAt(path, track, start, /*queryAudio=*/true, srcIn, srcDur,
                          /*keepView=*/true) >= 0;
}

// (Re)create the offscreen float target the HDR color pass renders into when an
// external output / review sink needs the transformed image read back.
bool App::ensureHdrProgramTex_(int w, int h) {
    if (w <= 0 || h <= 0) return false;
    if (hdrProgramTex_ && hdrProgW_ == w && hdrProgH_ == h) return true;
    if (hdrProgramTex_) { SDL_DestroyTexture(hdrProgramTex_); hdrProgramTex_ = nullptr; }
    SDL_PropertiesID tp = SDL_CreateProperties();
    SDL_SetNumberProperty(tp, SDL_PROP_TEXTURE_CREATE_FORMAT_NUMBER, SDL_PIXELFORMAT_RGBA64_FLOAT);
    SDL_SetNumberProperty(tp, SDL_PROP_TEXTURE_CREATE_ACCESS_NUMBER, SDL_TEXTUREACCESS_TARGET);
    SDL_SetNumberProperty(tp, SDL_PROP_TEXTURE_CREATE_WIDTH_NUMBER, w);
    SDL_SetNumberProperty(tp, SDL_PROP_TEXTURE_CREATE_HEIGHT_NUMBER, h);
    SDL_SetNumberProperty(tp, SDL_PROP_TEXTURE_CREATE_COLORSPACE_NUMBER, SDL_COLORSPACE_SRGB_LINEAR);
    hdrProgramTex_ = SDL_CreateTextureWithProperties(renderer_, tp);
    SDL_DestroyProperties(tp);
    if (hdrProgramTex_) {
        SDL_SetTextureScaleMode(hdrProgramTex_, SDL_SCALEMODE_LINEAR);
        hdrProgW_ = w; hdrProgH_ = h;
    }
    return hdrProgramTex_ != nullptr;
}

// Read the current render target (hdrProgramTex_) back to CPU for the sinks that need
// CPU pixels: an output device and an SDR review monitor. `sdr8` produces hdrReadback_
// (8-bit sRGB, highlights clipped); `half` produces hdrReadbackHalf_, the scRGB values
// as they stand, for a device that encodes HDR itself. An HDR review monitor never
// comes through here — it transforms the scene-linear source on its own device (see
// feedReviewSource_), which is what keeps this GPU->CPU stall off the HDR playback path.
// Returns false if the read failed.
bool App::readbackHdrProgram_(int w, int h, bool sdr8, bool half) {
    if (!sdr8 && !half)
        return false;
    SDL_Surface* s = SDL_RenderReadPixels(renderer_, nullptr);
    if (!s) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "HDR readback: SDL_RenderReadPixels failed: %s", SDL_GetError());
        return false;
    }
    bool ok = false;

    // The offscreen target is RGBA64_FLOAT, so read the halves in place; only an
    // unexpected format needs SDL_ConvertSurface, which costs a whole extra
    // frame-sized copy on every call.
    const bool halfSrc = (s->format == SDL_PIXELFORMAT_RGBA64_FLOAT);
    if (jplayDebugLogging() && (uint32_t)s->format != hdrDiagReadFmt_) {
        hdrDiagReadFmt_ = (uint32_t)s->format;
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "HDR readback: %dx%d target -> surface %s %dx%d, path=%s",
                    w, h, SDL_GetPixelFormatName(s->format), s->w, s->h,
                    halfSrc ? "half in place" : "convert to RGBA128F");
    }
    SDL_Surface* f = (halfSrc || s->format == SDL_PIXELFORMAT_RGBA128_FLOAT)
                         ? s : SDL_ConvertSurface(s, SDL_PIXELFORMAT_RGBA128_FLOAT);
    if (f && f->w >= w && f->h >= h) {
        if (sdr8) hdrReadback_.resize((size_t)w * h * 4);
        if (half) hdrReadbackHalf_.resize((size_t)w * h * 4);
        auto enc = [](float c) {
            c = std::clamp(c, 0.0f, 1.0f);
            float e = c <= 0.0031308f ? c * 12.92f : 1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f;
            return (uint8_t)std::lround(e * 255.0f);
        };
        const int pitchS = f->pitch / (int)(halfSrc ? sizeof(Imath::half) : sizeof(float));
        for (int y = 0; y < h; ++y) {
            const Imath::half* rowH = halfSrc
                ? (const Imath::half*)f->pixels + (size_t)y * pitchS : nullptr;
            const float* rowF = halfSrc
                ? nullptr : (const float*)f->pixels + (size_t)y * pitchS;
            uint8_t* d = sdr8 ? &hdrReadback_[(size_t)y * w * 4] : nullptr;
            Imath::half* dh = half ? &hdrReadbackHalf_[(size_t)y * w * 4] : nullptr;
            for (int x = 0; x < w; ++x) {
                const float r = halfSrc ? (float)rowH[x*4+0] : rowF[x*4+0];
                const float g = halfSrc ? (float)rowH[x*4+1] : rowF[x*4+1];
                const float b = halfSrc ? (float)rowH[x*4+2] : rowF[x*4+2];
                if (d) {
                    d[x*4+0] = enc(r);
                    d[x*4+1] = enc(g);
                    d[x*4+2] = enc(b);
                    d[x*4+3] = 255;
                }
                if (dh) {
                    dh[x*4+0] = r;
                    dh[x*4+1] = g;
                    dh[x*4+2] = b;
                    dh[x*4+3] = 1.0f;
                }
            }
        }
        ok = true;
    }
    if (f && f != s) SDL_DestroySurface(f);
    SDL_DestroySurface(s);
    return ok;
}

OutputTransfer App::hdrProgramTransfer_() const {
    // nitHeatmapShown_ replaces the display transform with a false-colour ramp, so its
    // output is not display-referred HDR at all — that goes out SDR like everything else.
    if (!hdrPipeline_ || !hdrFrameManaged_ || nitHeatmapShown_ ||
        !hdrColorPass_.hasState())
        return OutputTransfer::None;
    switch (hdrColorPass_.encoding()) {
    case HdrColorPass::Encoding::PQ:  return OutputTransfer::PQ;
    case HdrColorPass::Encoding::HLG: return OutputTransfer::HLG;
    default: return OutputTransfer::None; // an SDR display transform: 8-bit path
    }
}

// Seconds of look-ahead for the OCIO prewarm. A shot's LUTs cost tens of
// milliseconds to load cold — more from a show mount — and that load used to land
// on the main thread at the cut, as a visible pause. Two seconds is comfortably
// more than any such load and stays inside the frame cache's own forward window,
// so the transform is ready by the time the frames it applies to are.
static constexpr double kOcioWarmSeconds = 2.0;

// Build the transform the clip ~kOcioWarmSeconds ahead of the playhead will need,
// on a background thread, so switching to it is a cache hit. Called once per
// rendered frame: the lookup is one clipAt plus a map compare, and a job is only
// submitted when that clip resolves context variables not already live or warmed.
void App::warmOcioAhead() {
    const int64_t ahead = (int64_t)(timeline_.fps * kOcioWarmSeconds);
    const Clip* next = getTopMostClipAtFrame(timeline_.playhead + ahead);
    if (!next || next->mediaId.empty())
        return;
    auto media = timeline_.findMediaById(next->mediaId);
    if (!media || media->openFailed())
        return;
    // Copies: the job outlives this frame and must not hold Media pointers.
    std::function<void()> job = ocio_.warmJobForMedia(media->path(), media->meta());
    if (!job)
        return;
    ocioWarmWork_.submit([job](const std::atomic<bool>&) { job(); });
}

std::string App::mediaColorSpace(Media& m) {
    if (!ocio_.isReady())
        return {};
    // An explicit choice wins outright and needs no config to resolve against.
    std::string override_ = m.colorSpaceOverride();
    if (!override_.empty())
        return override_;

    // Otherwise the config answers, once. The answer belongs to the config that gave
    // it, so the cache reads back empty after a per-source config switch and this
    // resolves again against the new one.
    //
    // The container's colour tags come from the decoder, so a fully-informed answer
    // needs the source open — which it is by the time a frame of it is on screen,
    // but not necessarily when the toolbar asks for its label. Both answers are
    // cached, under keys that differ, so the provisional one costs no repeat work
    // and is still replaced by the informed one as soon as the source opens.
    const bool open = m.isOpen();
    SourceColorTags tags;
    if (open) {
        std::string err;
        if (auto src = m.ensureOpen(err))
            tags = src->colorTags();
    } else {
        // One tag is readable from the path alone: OpenEXR is scene-linear by
        // format, and ExrSequenceSource reports exactly that whatever the file
        // holds. Filling it in here is what keeps the unopened answer for an EXR
        // equal to the informed one - without it the sequence falls through to the
        // sRGB convention for untagged media, and whatever is keyed on that answer
        // (a thumbnail tile) bakes scene-linear values as sRGB code values, which
        // reads as a dark tile.
        tags.sceneLinear = ImageSeq::isExrPath(m.resolvedPath());
        // The same holds for the video flag, decided here by the very test
        // Media::ensureOpen() picks its decoder with: anything that is not an image
        // sequence is opened by VideoSource.
        tags.video = !ImageSeq::isSequencePath(m.resolvedPath());
    }
    // The resolved path is part of the key, not just the config: a proxy mode
    // substitutes the file actually decoded, and a published h264 standing in for
    // an EXR sequence is a different answer (its own tags, its own file rule).
    // Read after the ensureOpen above, which is where a mode switch reopens the
    // source and so is where resolvedPath() becomes the new file's.
    std::string key = ocio_.activeConfigPath() + "\n" + m.resolvedPath();
    if (!open)
        key += "\n<unopened>";
    if (std::string cached = m.resolvedColorSpace(key); !cached.empty())
        return cached;

    std::string cs = ocio_.colorSpaceForMedia(m.resolvedPath(), tags);
    if (!cs.empty())
        m.setResolvedColorSpace(key, cs);
    return cs;
}

std::shared_ptr<Media> App::playheadMedia() {
    const Clip* clip = playheadClip();
    if (!clip || clip->mediaId.empty())
        return nullptr;
    return timeline_.findMediaById(clip->mediaId);
}

std::string App::ocioInputCsLabel() {
    auto m = playheadMedia();
    if (!m)
        return "File Colorspace";
    if (!m->colorSpaceOverride().empty())
        return m->colorSpaceOverride();
    std::string cs = mediaColorSpace(*m);
    // The asterisk marks a space the config resolved rather than one the user
    // chose, so a wrong auto-assignment is visible without opening the menu.
    return cs.empty() ? "File Colorspace" : "* " + cs;
}

void App::renderPlayer() {
    // Black backdrop: a frame with no media on any track stays black.
    setColor(renderer_, kBlack);
    jplay::fillRect(renderer_, &playerRect_);

    // Read before the build below sets it: a cleared hasTexture_ is how every
    // change to the program's rendering inputs — the grade (markGradeDirty), the
    // tech mode, the OCIO display/view, the letterbox — asks for a re-render. The
    // Layout tiles render through the same grade and the same display transform, so
    // the one flag invalidates them too and they need no invalidation of their own.
    const bool renderInputsChanged = !hasTexture_;

    // Reconcile the pencil-markup buffer with the frame under the playhead before
    // anything reads or draws it (flushes the frame we just left, loads this one).
    syncAnnotBuffer();

    const Clip* clip = playheadClip();
    bool loading = false;
    auto playerMedia = clip && !clip->mediaId.empty() ? timeline_.findMediaById(clip->mediaId) : nullptr;
    bool missing = clip && playerMedia && playerMedia->openFailed();
    if (clip && playerMedia && !missing) {
        CacheKey key{ clip->mediaId, clip->sourceOffset + (timeline_.playhead - clip->timelineStart) };
        // Second half of a dissolve, when one covers this frame. programSourceAt's
        // `a` is the clip playheadClip() returned, so outside a transition this
        // resolves to nothing and the path below is what it always was.
        const ProgramSource ps = programSourceAt(timeline_.playhead);
        const Clip* bClip = (ps.a == clip) ? ps.b : nullptr;
        CacheKey keyB{};
        FramePtr fb;
        if (bClip) {
            auto bMedia = timeline_.findMediaById(bClip->mediaId);
            if (!bMedia || bMedia->openFailed()) {
                bClip = nullptr; // incoming source is missing: show the outgoing clip alone
            } else {
                keyB = CacheKey{ bClip->mediaId, ps.bSrc };
                fb = cache_->get(keyB);
            }
        }
        const float mix = bClip ? ps.mix : 0.0f;
        // A fade with nothing beneath it dims the clip toward black instead. Only
        // ever set when there is no second layer, so it never combines with mix.
        const float fade = bClip ? 1.0f : ps.fade;
        FramePtr frame = cache_->get(key);
        // A composite needs both halves resident. With only one, hold the previous
        // frame rather than popping to an unblended one mid-span — the same thing
        // the player already does when the single frame it needs isn't decoded yet.
        if (bClip && !fb)
            frame = nullptr;
        if (frame) {
            // Per-source OCIO config: sources from different shows resolve different
            // configs from the [ocio] preferences rules, so the config follows the
            // clip under the playhead — and a dissolve selects on the primary clip,
            // both halves then rendering through that one config. The OCIO context
            // variables ([ocio] context) follow the same clip, so a config with
            // per-shot LUTs resolves them for the source being shown.
            if (ocio_.isReady() && ocio_.isEnabled()) {
                const bool cfgChanged = ocio_.setActiveConfigForPath(playerMedia->path());
                const bool ctxChanged = ocio_.setContextForMedia(playerMedia->meta());
                if (cfgChanged || ctxChanged)
                    hasTexture_ = false; // different transform: this frame must re-render
                warmOcioAhead(); // the next clip's LUTs, loaded before its cut arrives
            }

            // Non-square pixels are corrected by stretching the texture at draw time
            // (see texDispW) rather than resampling the frame, so the frame's pixel
            // aspect has to follow it onto the texture. Set for every frame, not just
            // on a resize: cutting to same-sized media of a different pixel aspect
            // reuses the texture.
            texPa_ = frame->pixelAspect > 0.0f ? frame->pixelAspect : 1.0f;
            if (!texture_ || texW_ != frame->width || texH_ != frame->height) {
                if (texture_) SDL_DestroyTexture(texture_);
                if (hdrPipeline_) {
                    // HDR: a linear, extended-range float texture tagged SRGB_LINEAR so
                    // SDL treats its contents as already-linear (no sRGB decode) and lets
                    // values exceed 1.0 into the display's HDR headroom.
                    SDL_PropertiesID tp = SDL_CreateProperties();
                    SDL_SetNumberProperty(tp, SDL_PROP_TEXTURE_CREATE_FORMAT_NUMBER,
                                          SDL_PIXELFORMAT_RGBA64_FLOAT);
                    SDL_SetNumberProperty(tp, SDL_PROP_TEXTURE_CREATE_ACCESS_NUMBER,
                                          SDL_TEXTUREACCESS_STREAMING);
                    SDL_SetNumberProperty(tp, SDL_PROP_TEXTURE_CREATE_WIDTH_NUMBER, frame->width);
                    SDL_SetNumberProperty(tp, SDL_PROP_TEXTURE_CREATE_HEIGHT_NUMBER, frame->height);
                    SDL_SetNumberProperty(tp, SDL_PROP_TEXTURE_CREATE_COLORSPACE_NUMBER,
                                          SDL_COLORSPACE_SRGB_LINEAR);
                    texture_ = SDL_CreateTextureWithProperties(renderer_, tp);
                    SDL_DestroyProperties(tp);
                } else {
                    texture_ = SDL_CreateTexture(renderer_, SDL_PIXELFORMAT_RGBA32,
                                                 SDL_TEXTUREACCESS_STREAMING, frame->width, frame->height);
                }
                SDL_SetTextureScaleMode(texture_, SDL_SCALEMODE_LINEAR);
                texW_ = frame->width;
                texH_ = frame->height;
                hasTexture_ = false;
            }
            // The texture's identity is all three: a dissolve blends two frames, so
            // the primary key alone would report every later frame of a span as
            // already on screen.
            const bool upToDate = hasTexture_ && displayedKey_ == key &&
                                  displayedKey2_ == keyB && displayedMix_ == mix &&
                                  displayedFade_ == fade;
            if (texture_ && !upToDate) {
                // Count distinct frames reaching the screen (visible playback rate).
                // A re-render of the same frame (e.g. OCIO setting change) keeps the
                // same key and is not counted.
                bool newVisibleFrame = !(displayedKey_ == key);

                // The colour space each half is read in. Resolved before the
                // composite because a dissolve joining two different ones cannot be
                // blended first (see the mixed-space branch below).
                const bool ocioActive = ocio_.isReady() && ocio_.isEnabled();
                const std::string csA = ocioActive ? mediaColorSpace(*playerMedia) : std::string();
                std::string csB = csA;
                if (bClip && ocioActive) {
                    if (auto bMedia = timeline_.findMediaById(bClip->mediaId))
                        csB = mediaColorSpace(*bMedia);
                }
                // The composite has no single input space when the two halves
                // disagree. On the SDR path that sends each through its own
                // transform below; on the HDR path, where the transform happens at
                // draw time from one processor, it means the frame cannot be
                // colour-managed at all and is drawn as it decoded.
                const bool csMixed = bClip && ocioActive && csB != csA;

                // From here the pipeline works on `fr`: the decoded frame, or the
                // composite standing in for it. The primary defines the canvas, so a
                // secondary of another resolution is cropped/black-padded to fit
                // rather than refused.
                const Frame* fr = frame.get();
                if (bClip && fb) {
                    const Frame* pri = frame.get();
                    const Frame* sec = fb.get();
                    if (fb->width != frame->width || fb->height != frame->height) {
                        conformFrame(*fb, frame->width, frame->height, conformFrame_);
                        sec = &conformFrame_;
                    }
                    // A dissolve whose two halves are read in different colour
                    // spaces has no common space to mix in: blending an EXR against
                    // a ProRes, or two sources a config reads differently, would mix
                    // values that do not mean the same thing. When the spaces agree
                    // — the ordinary case, two clips of one format from one show —
                    // the blend happens here in that shared space and the result
                    // takes the display transform once, exactly as a single frame
                    // does.
                    //
                    // When they disagree, each half goes through its own transform
                    // to display-referred bytes first and the blend joins those.
                    // That runs on the CPU and is the slow path, but it is confined
                    // to the length of a mixed-format dissolve, and it is the same
                    // shape the code already used for the EXR/video case.
                    //
                    // Not on the HDR pipeline: it applies OCIO at draw time from the
                    // program texture (hdrColorPass_), and a CPU transform emits
                    // bytes in the display's own encoding — PQ for an HDR display,
                    // which that path would then wrongly sRGB-decode. A mixed
                    // dissolve there keeps the plain blend.
                    if (!hdrPipeline_ && csMixed) {
                        auto toDisplay = [&](const Frame& in, Frame& out,
                                             const std::string& cs) {
                            out.width = in.width;
                            out.height = in.height;
                            out.rgba.resize((size_t)in.width * in.height * 4);
                            out.rgba16.clear();
                            out.linearRgb.clear(); // display-referred now
                            ocio_.cpuTransformFor(cs, grade_.gain).apply(in, out.rgba.data());
                        };
                        toDisplay(*pri, ocioMixFrame_, csA);
                        toDisplay(*sec, ocioMixFrameB_, csB);
                        pri = &ocioMixFrame_;
                        sec = &ocioMixFrameB_;
                    }
                    if (blendFrames(*pri, *sec, mix, mixFrame_))
                        fr = &mixFrame_;
                } else if (fade < 1.0f) {
                    // Fade with no layer beneath: dim toward black. Copy first, since
                    // the cached frame is shared and must not be modified.
                    mixFrame_ = *frame;
                    fadeFrame(mixFrame_, fade);
                    fr = &mixFrame_;
                }

                if (hdrPipeline_) {
                    // HDR path: fill the float (SRGB_LINEAR) program texture with the
                    // frame in its OWN colour space — the colour pass transforms it at
                    // draw time, so nothing is decoded here. The texture is tagged
                    // SRGB_LINEAR only to stop SDL applying a transfer of its own; for
                    // an integer source the values are code values normalised to 0..1,
                    // which is what its OCIO input transform expects.
                    hdrFrameManaged_ = ocioActive && !csMixed;
                    // Luminance tech mode shows the nit heatmap. Scene-linear sources
                    // only: for anything else the pass would be reading code values as
                    // if they were luminance.
                    nitHeatmapShown_ = !fr->linearRgb.empty() &&
                                       techMode_ == TechMode::Luminance &&
                                       hdrColorPass_.hasNitState();
                    const size_t n = (size_t)fr->width * fr->height;
                    hdrPixels_.resize(n * 4);
                    Imath::half* d = hdrPixels_.data();
                    const Imath::half one(1.0f);
                    if (!fr->linearRgb.empty()) {
                        // Pure RGB->RGBA widening: 16-bit copies, no float round-trip.
                        // The gain is applied by the color pass shader (see
                        // HdrColorPass::begin) precisely so this stays cheap.
                        const Imath::half* s = fr->linearRgb.data();
                        for (size_t i = 0; i < n; ++i, s += 3, d += 4) {
                            d[0] = s[0];
                            d[1] = s[1];
                            d[2] = s[2];
                            d[3] = one;
                        }
                    } else {
                        // An integer source. Colour-managed, its code values go up
                        // untouched for the pass to interpret. Unmanaged, nothing
                        // downstream will decode them and the target is linear, so
                        // the display encoding has to come off here or the frame
                        // reads far too bright — sRGB, the assumption this path has
                        // always made for an unmanaged source.
                        auto srgbToLinear = [](float clip) {
                            return clip <= 0.04045f ? clip / 12.92f
                                                 : std::pow((clip + 0.055f) / 1.055f, 2.4f);
                        };
                        const bool decode = !hdrFrameManaged_;
                        if (fr->rgba16.size() >= n * 4) {
                            const uint16_t* s = fr->rgba16.data();
                            for (size_t i = 0; i < n; ++i, s += 4, d += 4) {
                                for (int k = 0; k < 3; ++k) {
                                    float v = s[k] * (1.0f / 65535.0f);
                                    d[k] = Imath::half(decode ? srgbToLinear(v) : v);
                                }
                                d[3] = one;
                            }
                        } else {
                            const uint8_t* s = fr->rgba.data();
                            for (size_t i = 0; i < n; ++i, s += 4, d += 4) {
                                for (int k = 0; k < 3; ++k) {
                                    float v = s[k] * (1.0f / 255.0f);
                                    d[k] = Imath::half(decode ? srgbToLinear(v) : v);
                                }
                                d[3] = one;
                            }
                        }
                    }
                    SDL_UpdateTexture(texture_, nullptr, hdrPixels_.data(),
                                      fr->width * 4 * (int)sizeof(Imath::half));
                    // Build/refresh the OCIO display-transform render state for EXR.
                    // The output encoding + primaries come from the active OCIO
                    // display name (ACES-style), so PQ HDR displays get true
                    // above-SDR-white highlights and wide-gamut primaries convert
                    // to scRGB (Rec.709).
                    if (hdrFrameManaged_) {
                        HdrColorPass::Encoding enc; HdrColorPass::Primaries prim;
                        hdrDisplayEncoding(ocio_.activeDisplay(), enc, prim);
                        OcioManager::Transform t = ocio_.transformFor(csA);
                        if (!t.valid())
                            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                                        "HDR: no OCIO transform for display \"%s\" view \"%s\" "
                                        "in \"%s\" — frame will be shown untransformed.",
                                        ocio_.activeDisplay().c_str(), ocio_.activeView().c_str(),
                                        csA.c_str());
                        if (reviewActive() && reviewHdr_) {
                            // The HDR monitor is the reference display: it gets the
                            // selected transform, on its own device (its pass tracks its
                            // own build state, so this is a no-op once built).
                            hdrColorPassReview_.setProcessors(t.toWorking, t.toDisplay,
                                                              t.version, enc, prim);
                            // The main window is SDR. Render it through the config's SDR
                            // companion so the GUI shows the SDR rendering of the same
                            // look; falling back to the reference transform is still
                            // better than nothing if the config yields no SDR display.
                            OcioManager::Transform sdr = ocio_.sdrTransformFor(csA);
                            if (sdr.valid()) {
                                HdrColorPass::Encoding senc; HdrColorPass::Primaries sprim;
                                hdrDisplayEncoding(ocio_.activeSdrDisplay(), senc, sprim);
                                hdrColorPass_.setProcessors(sdr.toWorking, sdr.toDisplay,
                                                            sdr.version, senc, sprim);
                            } else {
                                hdrColorPass_.setProcessors(t.toWorking, t.toDisplay,
                                                            t.version, enc, prim);
                            }
                        } else {
                            hdrColorPass_.setProcessors(t.toWorking, t.toDisplay,
                                                        t.version, enc, prim);
                        }
                    } else {
                        hdrColorPass_.setProcessors(nullptr, nullptr, -1,
                                                   HdrColorPass::Encoding::sRGB,
                                                   HdrColorPass::Primaries::Rec709);
                        if (reviewActive() && reviewHdr_)
                            hdrColorPassReview_.setProcessors(nullptr, nullptr, -1,
                                                             HdrColorPass::Encoding::sRGB,
                                                             HdrColorPass::Primaries::Rec709);
                    }
                    // Hand the scene-linear frame to the review device; it transforms
                    // there, so no readback of this renderer is needed for it.
                    if (reviewActive() && reviewHdr_) {
                        reviewSrcSceneLinear_ = hdrFrameManaged_;
                        feedReviewSource_(hdrPixels_.data(), fr->width, fr->height);
                    }
                } else {
                // Luminance mode: a true HDR nit heatmap of the scene-linear data,
                // computed BEFORE any display transform (and without the grade).
                // EXR-only — video carries no scene-linear values, so the mode is
                // inert there and we fall through to the normal display path.
                nitHeatmapShown_ = false;
                bool didNit = false;
                if (techMode_ == TechMode::Luminance && !fr->linearRgb.empty() &&
                    ocioGpu_.isReady()) {
                    SDL_PropertiesID tp = SDL_GetTextureProperties(texture_);
                    Sint64 texId = SDL_GetNumberProperty(tp, SDL_PROP_TEXTURE_OPENGL_TEXTURE_NUMBER, 0);
                    Sint64 texTarget = SDL_GetNumberProperty(tp, SDL_PROP_TEXTURE_OPENGL_TEXTURE_TARGET_NUMBER, 0);
                    if (texId != 0 && texTarget != 0)
                        didNit = ocioGpu_.renderNitHeatmap(fr->linearRgb.data(), fr->width, fr->height,
                                                           (unsigned)texId, (unsigned)texTarget,
                                                           grade_.gain, nitRef_);
                    nitHeatmapShown_ = didNit;
                }

                // Gain is a scene-linear operation (in stops), so it is applied by
                // the OCIO GPU pass (before the display transform) rather than in
                // the display-referred grade post-pass. This flag tracks that
                // so the post-pass doesn't apply it a second time on the 8-bit output.
                bool gainHandledUpstream = false;
                if (!didNit && ocioActive && !csMixed) {
                    // The display transform, for every source: the frame is read in
                    // its own colour space, brought to the working space, exposed,
                    // then rendered to the display. Prefer the GPU; fall back to the
                    // CPU path when it is unavailable.
                    const void* px = nullptr;
                    OcioGpu::InputFormat ifmt = OcioGpu::InputFormat::Rgba8;
                    frameInput(*fr, px, ifmt);

                    bool didGpu = false;
                    if (ocioGpu_.isReady() && px) {
                        OcioManager::Transform t = ocio_.transformFor(csA);
                        ocioGpu_.setProcessors(t.toWorking, t.toDisplay, t.version);
                        SDL_PropertiesID tp = SDL_GetTextureProperties(texture_);
                        Sint64 texId = SDL_GetNumberProperty(
                            tp, SDL_PROP_TEXTURE_OPENGL_TEXTURE_NUMBER, 0);
                        Sint64 texTarget = SDL_GetNumberProperty(
                            tp, SDL_PROP_TEXTURE_OPENGL_TEXTURE_TARGET_NUMBER, 0);
                        if (texId != 0 && texTarget != 0) {
                            didGpu = ocioGpu_.render(px, ifmt, fr->width, fr->height,
                                                     (unsigned)texId, (unsigned)texTarget,
                                                     grade_.gain);
                            gainHandledUpstream = didGpu;
                        }
                    }
                    if (!didGpu) {
                        ocioPixels_.resize((size_t)fr->width * fr->height * 4);
                        ocio_.cpuTransformFor(csA, grade_.gain).apply(*fr, ocioPixels_.data());
                        gainHandledUpstream = true; // the CPU transform exposed it too
                        SDL_UpdateTexture(texture_, nullptr, ocioPixels_.data(), fr->width * 4);
                    }
                } else if (!didNit) {
                    // Colour management off, or a mixed-space dissolve already
                    // transformed above: the frame's own 8-bit buffer stands.
                    gainHandledUpstream = csMixed;
                    SDL_UpdateTexture(texture_, nullptr, fr->rgba.data(), fr->width * 4);
                }
                // Color-grading + tech-check post-pass: grade the freshly-built
                // display texture in place on the GPU, then apply the tech-check
                // overlay. Works for both EXR (post-OCIO) and video. Luminance is a
                // pre-display pass (handled above / EXR-only), never routed here.
                int gradeTech = (techMode_ == TechMode::Luminance) ? 0 : (int)techMode_;
                if (!didNit && (grade_.active() || gradeTech != 0))
                    ensureGradeGpu(); // first graded frame builds the shaders
                if (!didNit && gradeGpu_.isReady() && (grade_.active() || gradeTech != 0)) {
                    SDL_PropertiesID gtp = SDL_GetTextureProperties(texture_);
                    Sint64 gId = SDL_GetNumberProperty(gtp, SDL_PROP_TEXTURE_OPENGL_TEXTURE_NUMBER, 0);
                    Sint64 gTarget = SDL_GetNumberProperty(gtp, SDL_PROP_TEXTURE_OPENGL_TEXTURE_TARGET_NUMBER, 0);
                    if (gId != 0 && gTarget != 0) {
                        // Gain already applied in scene-linear by the OCIO pass;
                        // zero it here so the display-space post-pass doesn't re-apply.
                        grade::State gs = grade_;
                        if (gainHandledUpstream)
                            gs.gain = 0.0f;
                        gradeGpu_.apply((unsigned)gId, (unsigned)gTarget, fr->width, fr->height,
                                        gs, gradeTech);
                    }
                }
                } // end else (non-HDR composite path)
                displayedKey_ = key;
                displayedKey2_ = keyB;
                displayedMix_ = mix;
                displayedFade_ = fade;
                hasTexture_ = true;
                // Push the freshly-composited program image (post OCIO/grade/tech)
                // to any active sink. A single GL readback feeds both the external
                // output device and the review-monitor window. GL path only — the
                // SDL_GPU pipeline reads its offscreen float target back further down.
                if (!hdrPipeline_ && (output_.active() || reviewActive())) {
                    const uint8_t* px = output_.readProgram(texture_, texW_, texH_);
                    if (px) {
                        // External output (NDI/SDI): bake the letterbox matte into the
                        // native-res frame so the received signal carries the bars. The
                        // review monitor overlays its bars in renderReviewWindow, so it
                        // gets the clean program image.
                        output_.submitFrame(letterboxForOutput(px, texW_, texH_),
                                            texW_, texH_, timeline_.fps); // no-op if no device
                        if (reviewActive())
                            feedReviewFrame(px, texW_, texH_);
                    }
                }
                if (newVisibleFrame)
                    visibleFrameTimes_.push_back(SDL_GetTicks());
            }
        } else {
            loading = true; // keep showing the previous texture while decoding
        }
    }

    // Whether a live program frame is on screen this frame (drives the review
    // window: a gap / launcher / missing source shows black there, like the GUI).
    programShown_ = clip && !missing && hasTexture_ && texture_ &&
                    texW_ > 0 && texH_ > 0 && !launcherVisible();

    // The burn-in's two strings, for every sink and every stage: the review window
    // shows the program frame even on the grid stage, where the player draws none.
    updateFrameOverlay();

    // Layout stage: resolve the rows on screen and where their cells fall before
    // anything is drawn. programDstRect() reads the program's cell out of this, so
    // the program image, its annotations and its letterbox all land in it.
    if (layoutView() && !launcherVisible()) {
        std::vector<const Clip*> tiles;
        layoutClipsAt(timeline_.playhead, tiles);
        layoutClipIds_.clear();
        for (const Clip* t : tiles)
            layoutClipIds_.push_back(t ? t->id : -1);
        // Which cell the program lands in. `c` is the top-most clip at the playhead
        // and the rows are its filter's, so it is one of them — but not necessarily
        // the first, since the rows above it may have no frame here.
        layoutProgramTile_ = 0;
        for (size_t i = 0; i < tiles.size(); ++i)
            if (tiles[i] == clip) {
                layoutProgramTile_ = (int)i;
                break;
            }
        // Cells are shaped by the program image, so the tiles match the media being
        // compared. Before the first frame lands there is no texture to ask, so the
        // program clip's cached metadata answers instead and the cells are right
        // from the very first render rather than reflowing once the decode arrives.
        float ar = (texW_ > 0 && texH_ > 0) ? texDispW() / (float)texH_ : 0.0f;
        if (ar <= 0.0f) {
            for (const Clip* clip : tiles) {
                auto media = clip ? timeline_.findMediaById(clip->mediaId) : nullptr;
                if (!media)
                    continue;
                MediaInfo info = media->info();
                if (info.width > 0 && info.height > 0) {
                    ar = (float)info.width *
                         (info.pixelAspect > 0.0f ? info.pixelAspect : 1.0f) /
                         (float)info.height;
                    break;
                }
            }
        }
        buildLayoutTiles((int)tiles.size(), ar);
    }

    if (gridView() && !launcherVisible()) {
        // Grid view: every clip as a tile; the clip under the playhead (c) plays
        // live in its tile (via texture_), the rest reuse the Overview thumbnails.
        renderGridView(clip && !missing ? clip : nullptr);
    } else {
        // Layout stage: the program shares the stage with the comparison tiles, so
        // its image, annotations and letterbox are confined to tile 0's cell — a
        // zoomed program must not spill into the tile beside it. Applied around the
        // on-screen draws rather than across this whole block: the HDR path renders
        // to an offscreen target midway through, and a clip rect meant for the
        // window has no business following it there.
        const bool tiled = layoutView() && !layoutTiles_.empty();
        auto clipToTile = [&](bool on) {
            if (!tiled)
                return;
            if (!on) {
                SDL_SetRenderClipRect(renderer_, nullptr);
                return;
            }
            const SDL_FRect& cell = layoutProgramCell();
            const SDL_Rect cr = { (int)cell.x, (int)cell.y, (int)cell.w, (int)cell.h };
            SDL_SetRenderClipRect(renderer_, &cr);
        };
        // Only draw the frame while the playhead is actually over a clip with a live
        // source; otherwise the backdrop (black) shows through. (A missing source must
        // not keep showing the previous clip's texture.)
        if (clip && !missing && hasTexture_ && texture_ && texW_ > 0 && texH_ > 0) {
            SDL_FRect dst = programDstRect();
            if (hdrPipeline_) {
                // Does the color pass have anything to apply? With colour management
                // off, and for a dissolve already transformed on the CPU, the frame is
                // drawn as-is — but it still travels the offscreen route below when a
                // sink needs a readback. The nit heatmap is independent of the display
                // transform, so it applies whether or not one is loaded.
                const bool colorPass = nitHeatmapShown_ ||
                                       (hdrFrameManaged_ && hdrColorPass_.hasState());
                // Draw texture_ into the current render target, through the active HDR
                // color pass (nit heatmap, or OCIO + display-space grade/tech) if any.
                auto drawProgram = [&](const SDL_FRect* dstRect) {
                    if (!colorPass) {
                        SDL_RenderTexture(renderer_, texture_, nullptr, dstRect);
                        return;
                    }
                    const float gain = std::exp2(grade_.gain); // scene-linear, pre-transform
                    if (nitHeatmapShown_) {
                        hdrColorPass_.beginNit(nitRef_, gain);
                    } else {
                        int gradeTech = (techMode_ == TechMode::Luminance) ? 0 : (int)techMode_;
                        hdrColorPass_.begin(hdrRefWhiteNits_, gain, grade_, gradeTech);
                    }
                    SDL_RenderTexture(renderer_, texture_, nullptr, dstRect);
                    hdrColorPass_.end();
                };
                // Only sinks that need CPU pixels force a readback. An output device that
                // encodes HDR itself (NDI: Rec.2020/PQ) takes the scRGB halves untouched
                // whenever the pass really produced an HDR rendering; every other sink —
                // an SDR review monitor, or an output device on any non-PQ frame — takes
                // the tonemapped 8-bit image. An HDR review monitor transforms on its own
                // device (feedReviewSource_ above), so it costs nothing here.
                const OutputTransfer outXfer =
                    (output_.active() && output_.wantsHdr()) ? hdrProgramTransfer_()
                                                            : OutputTransfer::None;
                const bool outHdr = outXfer != OutputTransfer::None;
                const bool wantSdr = (output_.active() && !outHdr) ||
                                     (reviewActive() && !reviewHdr_);
                // The pass emits linear (scRGB) values. An HDR (SRGB_LINEAR) main
                // swapchain wants exactly that, so we can draw straight to it. An SDR main
                // swapchain wants sRGB-encoded values, so we render into the offscreen
                // SRGB_LINEAR target and let SDL's texture blit do the linear->sRGB encode
                // (highlights clip). A readback sink also needs that offscreen.
                // The pixel inspector reads its DISP value off this target too — the
                // display image exists nowhere else on this pipeline — so it forces the
                // detour even when the swapchain could take the pass directly.
                const bool viaOffscreen = wantSdr || outHdr || !hdrActive_ ||
                                          pixelInspectorOpen_;
                // One line describing the whole HDR routing decision, logged only when it
                // changes. The OCIO display matters as much as the swapchain: an HDR
                // review monitor fed an "sRGB - Display" transform presents SDR-looking
                // pixels even though the swapchain is scRGB.
                if (jplayDebugLogging()) {
                    char cfg[512];
                    const bool fwd = mainForcedSdrOcio();
                    SDL_snprintf(cfg, sizeof(cfg),
                                 "mainHDR=%d reviewHDR=%d review=%d ndi=%d colorPass=%d "
                                 "sceneLinear=%d nitHeatmap=%d passState=%d reviewPass=%d "
                                 "offscreen=%d wantSdr=%d outHdr=%d ocio=%d/%d "
                                 "display=\"%s\" view=\"%s\" mainOcio=\"%s / %s\"%s",
                                 hdrActive_ ? 1 : 0, reviewHdr_ ? 1 : 0, reviewActive() ? 1 : 0,
                                 output_.active() ? 1 : 0, colorPass ? 1 : 0,
                                 hdrFrameManaged_ ? 1 : 0, nitHeatmapShown_ ? 1 : 0,
                                 hdrColorPass_.hasState() ? 1 : 0,
                                 hdrColorPassReview_.hasState() ? 1 : 0, viaOffscreen ? 1 : 0,
                                 wantSdr ? 1 : 0, outHdr ? 1 : 0,
                                 ocio_.isReady() ? 1 : 0, ocio_.isEnabled() ? 1 : 0,
                                 ocio_.activeDisplay().c_str(), ocio_.activeView().c_str(),
                                 fwd ? ocio_.activeSdrDisplay().c_str() : ocio_.activeDisplay().c_str(),
                                 fwd ? ocio_.activeSdrView().c_str() : ocio_.activeView().c_str(),
                                 fwd ? " (forced SDR: HDR monitor owns the reference)" : "");
                    if (hdrDiagSink_ != cfg) {
                        hdrDiagSink_ = cfg;
                        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "HDR route: %s", cfg);
                    }
                }
                if (viaOffscreen && ensureHdrProgramTex_(texW_, texH_)) {
                    SDL_SetRenderTarget(renderer_, hdrProgramTex_);
                    drawProgram(nullptr);
                    bool read = readbackHdrProgram_(texW_, texH_, wantSdr, outHdr);
                    SDL_SetRenderTarget(renderer_, nullptr);
                    clipToTile(true);
                    SDL_RenderTexture(renderer_, hdrProgramTex_, nullptr, &dst);
                    clipToTile(false);
                    if (read) {
                        // External output (NDI/SDI): bake the letterbox matte in, as on
                        // the SDR path — the received signal has to carry the bars.
                        if (outHdr && !hdrReadbackHalf_.empty())
                            output_.submitHdrFrame(
                                letterboxForOutput(hdrReadbackHalf_.data(), texW_, texH_),
                                texW_, texH_, timeline_.fps, outXfer, hdrRefWhiteNits_);
                        else if (output_.active() && !hdrReadback_.empty())
                            output_.submitFrame(letterboxForOutput(hdrReadback_.data(), texW_, texH_),
                                                texW_, texH_, timeline_.fps);
                        if (reviewActive() && !reviewHdr_ && !hdrReadback_.empty())
                            feedReviewFrame(hdrReadback_.data(), texW_, texH_);
                    }
                } else {
                    clipToTile(true);
                    drawProgram(&dst);
                    clipToTile(false);
                }
            } else {
                clipToTile(true);
                SDL_RenderTexture(renderer_, texture_, nullptr, &dst);
                clipToTile(false);
            }
        }

        // Pencil markup, on top of the frame image.
        clipToTile(true);
        renderAnnotations();

        // Letterbox matte, on top of the frame + annotations. Image-relative, so it
        // tracks zoom/pan and matches the native-res output/review framing. Never in
        // grid view (the grid branch above skips this) or over an empty backdrop.
        if (programShown_)
            drawLetterbox(renderer_, programDstRect());
        // Burn-in, last over the picture so the matte can't cover it.
        if (programShown_)
            drawFrameOverlay(renderer_, overlayFont_, programDstRect());
        clipToTile(false);

        // Missing source: a black frame with a centred "Missing <filename>" label.
        if (missing) {
            std::string label = "Missing " + fs::path(playerMedia->path()).filename().string();
            float tw = textFont_.measure(renderer_, label.c_str());
            float th = textFont_.lineHeight();
            const SDL_FRect& area = tiled ? layoutProgramCell() : playerRect_;
            drawText(area.x + (area.w - tw) * 0.5f,
                     area.y + (area.h - th) * 0.5f,
                     kStatusText, label);
        }

        // The comparison tiles, and the program tile's own label + border. Last, so
        // the chrome sits over every image.
        if (tiled) {
            renderLayoutTiles(renderInputsChanged);
            drawLayoutTileChrome(layoutProgramCell(), clip, layoutProgramTile_);
        }
    }

    if (launcherVisible()) {
        renderMainPanels();
    } else if (loading && !gridView()) {
        // A source the readers can't decode never leaves this branch, so name the
        // failure rather than showing LOADING for the rest of the session. The
        // reason is in the log (once per media, from the cache worker).
        const bool failed = clip && cache_->mediaFailed(clip->mediaId);
        const char* label = failed ? "FAILED TO DECODE SOURCE" : "LOADING";
        const float pad = 6.0f;
        float tw = textFont_.measure(renderer_, label);
        float th = textFont_.lineHeight();
        SDL_FRect badge = { playerRect_.x + 10, playerRect_.y + 10,
                            tw + pad * 2, th + pad };
        setColor(renderer_, failed ? kStatusWarnBg : kLoadingBg);
        jplay::fillRect(renderer_, &badge);
        drawText(badge.x + pad, badge.y + pad * 0.5f, kLoadingText, label);
    }

    // Pixel probe: sampled here, where the program texture and the layout are both
    // current, and drawn later by renderPixelInspector.
    samplePixelInspector();

    // Stack stage: the basename overlay a cycle armed, over the image and under the
    // status message — which is where a warning about the cycle itself belongs.
    renderStackOverlay();

    // Transient status message, top-left of the video frame.
    if (SDL_GetTicks() < statusUntil_ && !status_.empty()) {
        float tw = textFont_.measure(renderer_, status_.c_str());
        SDL_FRect bg = { playerRect_.x + 8, playerRect_.y + 8, tw + 12, 18 };
        const SDL_Color fill = statusWarn_ ? kStatusWarnBg : kBlack;
        SDL_SetRenderDrawColor(renderer_, fill.r, fill.g, fill.b, statusWarn_ ? 230 : 200);
        jplay::fillRect(renderer_, &bg);
        drawText(bg.x + 6, bg.y + 5, statusWarn_ ? kStatusWarnText : kStatusText, status_);
    }
}

// ---- burn-in over the program image (View > Overlay) ------------------------

// The two strings the burn-in shows, rebuilt once per rendered frame: the file
// under the playhead and the playhead readout. The readout follows the same
// Time Format / Frame Numbering preferences as the timeline ruler, so the two
// can never disagree; the formatting rules are the ruler's (see renderTimeline).
void App::updateFrameOverlay() {
    overlayFile_.clear();
    overlayTime_.clear();
    if (!frameOverlay_)
        return;

    const Clip* clip = playheadClip();
    if (!clip)
        return;
    auto media = clip->mediaId.empty() ? nullptr : timeline_.findMediaById(clip->mediaId);
    if (!media)
        return;

    const int64_t clipPos = clip->sourceOffset + (timeline_.playhead - clip->timelineStart);

    // An image sequence names the file for this frame rather than the whole
    // sequence: that is the point of a burn-in. Only when the decoder is already
    // open, so the overlay never forces one on the render thread.
    std::string file = media->path();
    int64_t base = 0;
    if (media->isOpen()) {
        std::string err;
        if (auto src = media->ensureOpen(err)) {
            base = src->firstFrameNumber();
            const auto& files = src->sequenceFiles();
            if (clipPos >= 0 && clipPos < (int64_t)files.size())
                file = files[(size_t)clipPos];
        }
    }
    overlayFile_ = fs::u8path(file).filename().u8string();

    const double fps = timeline_.fps > 0.0 ? timeline_.fps : 24.0;
    const int64_t length = timeline_.length();
    auto fmtTime = [&](int64_t f) {
        int64_t totalSec = (int64_t)(f / fps);
        char buf[32];
        int h = (int)(totalSec / 3600), mi = (int)((totalSec % 3600) / 60);
        int sec = (int)(totalSec % 60), fr = (int)(f % (int64_t)std::llround(fps));
        if (length < (int64_t)(3600.0 * fps)) // no "HH:" under an hour, as on the ruler
            SDL_snprintf(buf, sizeof(buf), "%02d:%02d:%02d", mi, sec, fr);
        else
            SDL_snprintf(buf, sizeof(buf), "%02d:%02d:%02d:%02d", h, mi, sec, fr);
        return std::string(buf);
    };

    if (frameNumberingClip_) {
        overlayTime_ = showAsFrames() ? std::to_string(base + clipPos) : fmtTime(clipPos);
    } else {
        // Global numbering is relative to the scoped view's start, like the ruler.
        int64_t dispOffset = 0, a = 0, b = 0;
        if (scopeRange(a, b))
            dispOffset = a;
        int64_t f = timeline_.playhead - dispOffset;
        overlayTime_ = showAsFrames() ? std::to_string(f) : fmtTime(f);
    }
}

void App::drawFrameOverlay(SDL_Renderer* r, const TextFont& font,
                           const SDL_FRect& content) const {
    if (!frameOverlay_ || overlayFile_.empty() || content.w <= 0.0f || content.h <= 0.0f)
        return;

    // Anchor to the picture, not the image rect: with a matte up, the top and
    // bottom edges of the image are black bars, and a burn-in belongs on the
    // picture inside them.
    SDL_FRect pic = content;
    SDL_FRect bars[2];
    if (letterboxBars(content, bars) == 2) {
        if (bars[0].w >= content.w - 0.5f) { // horizontal bars (top/bottom)
            pic.y += bars[0].h;
            pic.h -= bars[0].h * 2.0f;
        } else {                             // pillarbox (left/right)
            pic.x += bars[0].w;
            pic.w -= bars[0].w * 2.0f;
        }
    }
    if (pic.w <= 0.0f || pic.h <= 0.0f)
        return;

    // Drawn at the font's own rasterized size: stretching a cached glyph texture is
    // what made the text soft, so the Size choice reopens the font instead (see
    // reloadOverlayFont). The text sits straight on the picture with no backing
    // band; the margins are laid out off the line height, so they grow with it.
    const float lineH = font.lineHeight();
    const float pad = std::round(lineH * 0.35f);
    const float rowH = lineH + pad;
    const float ty = (frameOverlayBottom_ ? pic.y + pic.h - rowH : pic.y) + pad * 0.5f;
    const SDL_Color text = kOverlayColors[overlayColor_].c;

    font.draw(r, pic.x + pad, ty, text, overlayFile_.c_str());
    if (!overlayTime_.empty()) {
        float tw = font.measure(r, overlayTime_.c_str());
        font.draw(r, pic.x + pic.w - pad - tw, ty, text, overlayTime_.c_str());
    }
}

// Bar above the video frame. Left side names what is active under the playhead
// ("sequence | shot | clip"); right side carries the transport readout that used
// to sit in the timeline info bar (FRAME / IN / OUT / FPS / CACHE).
// Rebuild the overlay fields only when the clip under the playhead changes.
// Opening the decoder (for codec/EXR-header detail) happens here, on demand.
void App::updateInfoOverlay() {
    const Clip* clip = playheadClip();
    int id = clip ? clip->id : -1;
    if (id == infoClipId_)
        return;
    infoClipId_ = id;
    infoFields_.clear();
    if (!clip || clip->mediaId.empty())
        return;
    auto mptr = timeline_.findMediaById(clip->mediaId);
    if (!mptr)
        return;

    Media& media = *mptr;
    MediaInfo info = media.info();
    infoFields_.push_back({ "File", fs::path(media.path()).filename().string() });
    infoFields_.push_back({ "Type", media.type() == ClipType::ImageSequence
                                            ? ImageSeq::typeLabel(media.path()) : "Video" });
    if (info.width > 0 && info.height > 0)
        infoFields_.push_back({ "Resolution", std::to_string(info.width) + " x " + std::to_string(info.height) });
    infoFields_.push_back({ "Frames", std::to_string(std::max<int64_t>(info.frameCount, 1)) });
    if (info.fps > 0.0) {
        char b[32];
        SDL_snprintf(b, sizeof(b), "%.3f", info.fps);
        infoFields_.push_back({ "FPS", b });
    }

    // Source-specific detail (codec/audio, or EXR bit depth/channels/layers/views).
    std::string err;
    if (auto src = media.ensureOpen(err)) {
        for (const auto& f : src->describe())
            infoFields_.push_back(f);
    } else {
        infoFields_.push_back({ "Status", "unavailable: " + err });
    }
}

// --- Pencil annotation --------------------------------------------------------
// A round brush dragged along the cursor path. Strokes are stored in image
// coordinates (see App.h AnnotPt) so they stay glued to the frame under zoom/pan.
// Width is the brush diameter and follows cursor speed — slow draws thick, a fast
// flick draws thin — smoothed so it swells rather than steps. renderAnnotations()
// traces the outline of that brush: round caps, round corners, feathered edges.

// Clip shown at an absolute timeline frame + the media source frame it maps to.
Clip* App::clipAtSourceFrame(int64_t frame, int64_t& srcFrame) {
    const Clip* clip = getTopMostClipAtFrame(frame);
    if (!clip) {
        srcFrame = 0;
        return nullptr;
    }
    srcFrame = clip->sourceOffset + (frame - clip->timelineStart);
    return timeline_.findClipById(clip->id);
}

// Write the live buffer back into its owning clip, keyed by source frame. Empty
// buffers erase the entry so cleared frames don't linger in the project file.
void App::flushAnnotBuffer() {
    if (annotClipId_ < 0)
        return;
    Clip* clip = timeline_.findClipById(annotClipId_);
    if (!clip)
        return;
    if (annotStrokes_.empty())
        clip->annotations.erase(annotFrame_);
    else
        clip->annotations[annotFrame_] = annotStrokes_;
}

// Reconcile the live buffer with the frame under the playhead: when the displayed
// (clip, source frame) changes, flush the old frame's markup back into its clip
// and load the new frame's stored markup. Cheap no-op when nothing changed. Runs
// once per rendered frame (renderPlayer) so playback flushes each frame it leaves.
void App::syncAnnotBuffer() {
    int64_t sf = 0;
    Clip* clip = clipAtSourceFrame(timeline_.playhead, sf);
    int clipId = clip ? clip->id : -1;
    if (clipId == annotClipId_ && sf == annotFrame_)
        return;
    flushAnnotBuffer();
    annotClipId_ = clipId;
    annotFrame_ = sf;
    annotDrawing_ = false;
    annotStrokes_.clear();
    if (clip) {
        auto it = clip->annotations.find(sf);
        if (it != clip->annotations.end())
            annotStrokes_ = it->second;
    }
}

void App::annotBeginStroke(float sx, float sy) {
    SDL_FRect dst = programDstRect();
    if (dst.w <= 0.0f || texW_ <= 0)
        return;
    float scale = dst.w / texDispW();
    // Make sure the live buffer belongs to the frame under the playhead (flushing
    // and loading as needed) before appending to it.
    syncAnnotBuffer();
    annotStrokes_.emplace_back();
    // Capture the current pencil color; later wheel changes won't recolor this stroke.
    pencilColorRGB(annotStrokes_.back().r, annotStrokes_.back().g, annotStrokes_.back().b);
    // A press with the cursor still is the slow end of the speed ramp, so the brush
    // starts at full width; a click with no drag lands one round dab of that size.
    float hw0 = 3.4f * dpiScale * pencilSizeMul_;
    annotStrokes_.back().pts.push_back({ (sx - dst.x) / scale, (sy - dst.y) / scale, hw0 / scale });
    annotDrawing_ = true;
    annotLastSX_ = sx; annotLastSY_ = sy;
    annotLastT_ = SDL_GetTicks();
    annotSmoothHw_ = hw0;
}

void App::annotAppendPoint(float sx, float sy) {
    if (!annotDrawing_ || annotStrokes_.empty())
        return;
    SDL_FRect dst = programDstRect();
    if (dst.w <= 0.0f || texW_ <= 0)
        return;
    float scale = dst.w / texDispW();

    float dx = sx - annotLastSX_, dy = sy - annotLastSY_;
    float dist = std::sqrt(dx * dx + dy * dy);
    if (dist < 2.0f * dpiScale) // drop sub-step jitter; keeps the polyline clean
        return;

    uint64_t now = SDL_GetTicks();
    float dt = (float)(now - annotLastT_);
    if (dt < 1.0f) dt = 1.0f;
    float speed = dist / dt; // screen px per ms

    // Pressure sim: near-stationary cursor draws thick, a fast flick draws thin.
    const float maxHw = 3.4f * dpiScale;
    const float minHw = 1.0f * dpiScale;
    const float speedRef = 2.2f; // px/ms that maps down to minHw
    float t = std::clamp(speed / speedRef, 0.0f, 1.0f);
    float target = (maxHw - (maxHw - minHw) * t) * pencilSizeMul_; // slider scales the pressure sim
    annotSmoothHw_ += (target - annotSmoothHw_) * 0.35f; // ease, don't step

    annotStrokes_.back().pts.push_back({ (sx - dst.x) / scale, (sy - dst.y) / scale,
                                         annotSmoothHw_ / scale });
    annotLastSX_ = sx; annotLastSY_ = sy;
    annotLastT_ = now;
}

void App::renderAnnotations() {
    if (annotStrokes_.empty() || texW_ <= 0 || texH_ <= 0)
        return;
    SDL_FRect dst = programDstRect();
    if (dst.w <= 0.0f || dst.h <= 0.0f)
        return;

    std::vector<SDL_Vertex> verts;
    std::vector<int> idx;
    annotationGeometry(dst, verts, idx);
    if (!verts.empty())
        SDL_RenderGeometry(renderer_, nullptr, verts.data(), (int)verts.size(),
                           idx.data(), (int)idx.size());
}

// Tessellate the current pencil strokes into triangle geometry for `dst` (the
// on-screen rect the native-res frame is drawn into). Strokes are stored in image
// display coordinates (texDispW x texH_, so one uniform scale still maps them when
// the pixels are not square), so the same geometry reproduces at any dst / renderer
// — used both for the GUI player view and the review-monitor window. Screen-space
// widths (feather, tip taper) scale with the destination, matching zoom behaviour.
void App::annotationGeometry(const SDL_FRect& dst,
                             std::vector<SDL_Vertex>& verts,
                             std::vector<int>& idx) const {
    if (texW_ <= 0 || dst.w <= 0.0f)
        return;
    float scale = dst.w / texDispW();
    const float feather = 1.4f * dpiScale; // soft-edge band width (screen px)

    for (const auto& strokeObj : annotStrokes_) {
        const auto& stroke = strokeObj.pts;
        const float cr = strokeObj.r, cg = strokeObj.g, cb = strokeObj.b; // this stroke's captured color
        size_t n = stroke.size();
        if (n == 0)
            continue;
        // Project to screen; carry half-width back to screen px.
        std::vector<float> px(n), py(n), phw(n);
        for (size_t i = 0; i < n; ++i) {
            px[i] = dst.x + stroke[i].x * scale;
            py[i] = dst.y + stroke[i].y * scale;
            phw[i] = std::max(stroke[i].hw * scale, 0.4f * dpiScale);
        }
        if (n == 1) {
            emitDot(verts, idx, px[0], py[0], phw[0], feather, cr, cg, cb);
            continue;
        }
        // Curve between the pointer samples before tessellating.
        smoothStroke(px, py, phw);
        n = px.size();

        // Cumulative arc length: the window the width filter below averages over.
        std::vector<float> s(n, 0.0f);
        for (size_t i = 1; i < n; ++i)
            s[i] = s[i - 1] + std::hypot(px[i] - px[i - 1], py[i] - py[i - 1]);

        // Box-filter the half-width over a window in arc length. The pressure sim
        // reads cursor speed off millisecond-resolution event timing, and a corner
        // slows the cursor, so raw widths wobble sample to sample — which a thick
        // pencil shows as a chain of lumps. Filtering keeps the slow swell of the
        // pressure sim and drops the per-sample wobble.
        {
            const float win = 14.0f * dpiScale;
            std::vector<float> sm(n);
            size_t lo = 0, hi = 0;
            double acc = 0.0;
            for (size_t i = 0; i < n; ++i) {
                while (hi < n && s[hi] <= s[i] + win) acc += phw[hi++];
                while (s[lo] < s[i] - win)            acc -= phw[lo++];
                sm[i] = (float)(acc / (double)(hi - lo));
            }
            phw = std::move(sm);
        }

        // Four offset vertices per sample: outerL, coreL, coreR, outerR. The
        // outer pair carries alpha 0 (the feather); the core pair is opaque.
        // Offsets run along the angle bisector of the incoming and outgoing
        // directions, lengthened by 1/cos(theta/2) so the perpendicular width is
        // preserved through the turn. Past kMaxMiter the corner would spike, so it
        // is capped there and the sample gets a round dab instead — which is what
        // a round brush leaves at a corner anyway. Offsetting along one averaged
        // tangent instead (no miter, no dab) leaves a wedge gap on the outside of
        // every bend and folds the inside over itself.
        const float kMaxMiter = 1.15f;
        std::vector<size_t> dabs;   // samples whose corner needs the round join
        int base = (int)verts.size();
        for (size_t i = 0; i < n; ++i) {
            float ix, iy, gx, gy; // incoming / outgoing directions at this sample
            if (i == 0) { ix = px[1] - px[0];     iy = py[1] - py[0]; }
            else        { ix = px[i] - px[i - 1]; iy = py[i] - py[i - 1]; }
            if (i == n - 1) { gx = px[n - 1] - px[n - 2]; gy = py[n - 1] - py[n - 2]; }
            else            { gx = px[i + 1] - px[i];     gy = py[i + 1] - py[i]; }
            float il = std::hypot(ix, iy), gl = std::hypot(gx, gy);
            if (il < 1e-4f) { ix = gx; iy = gy; il = gl; }
            if (gl < 1e-4f) { gx = ix; gy = iy; gl = il; }
            if (il < 1e-4f) { ix = 1.0f; iy = 0.0f; il = 1.0f; gx = 1.0f; gy = 0.0f; gl = 1.0f; }
            ix /= il; iy /= il; gx /= gl; gy /= gl;
            float n1x = -iy, n1y = ix;            // left normal of the incoming leg
            float nx = n1x + -gy, ny = n1y + gx;  // + left normal of the outgoing leg
            float bl = std::hypot(nx, ny);
            float miter = 1.0f;
            if (bl < 1e-4f) {          // doubled back on itself: keep the incoming normal
                nx = n1x; ny = n1y;
                miter = kMaxMiter;
            } else {
                nx /= bl; ny /= bl;
                miter = 1.0f / std::max(nx * n1x + ny * n1y, 1e-3f); // 1/cos(theta/2)
            }
            if (miter > kMaxMiter) { miter = kMaxMiter; dabs.push_back(i); }

            float hw = phw[i] * miter;
            float outer = (phw[i] + feather) * miter;

            SDL_Vertex vOL{}, vCL{}, vCR{}, vOR{};
            vOL.position = { px[i] - nx * outer, py[i] - ny * outer }; vOL.color = { cr, cg, cb, 0 };
            vCL.position = { px[i] - nx * hw,    py[i] - ny * hw    }; vCL.color = { cr, cg, cb, 1 };
            vCR.position = { px[i] + nx * hw,    py[i] + ny * hw    }; vCR.color = { cr, cg, cb, 1 };
            vOR.position = { px[i] + nx * outer, py[i] + ny * outer }; vOR.color = { cr, cg, cb, 0 };
            verts.push_back(vOL); verts.push_back(vCL);
            verts.push_back(vCR); verts.push_back(vOR);
        }

        // Stitch consecutive samples: left feather, opaque core, right feather.
        for (size_t i = 0; i + 1 < n; ++i) {
            int a0 = base + (int)i * 4;
            int b0 = base + (int)(i + 1) * 4;
            auto quad = [&](int l0, int l1, int r0, int r1) {
                idx.push_back(l0); idx.push_back(l1); idx.push_back(r1);
                idx.push_back(l0); idx.push_back(r1); idx.push_back(r0);
            };
            quad(a0 + 0, a0 + 1, b0 + 0, b0 + 1);
            quad(a0 + 1, a0 + 2, b0 + 1, b0 + 2);
            quad(a0 + 2, a0 + 3, b0 + 2, b0 + 3);
        }

        // The dabs go last: emitDot appends its own vertices, so interleaving them
        // with the ribbon would break the base + i * 4 indexing above.
        for (size_t i : dabs)
            emitDot(verts, idx, px[i], py[i], phw[i], feather, cr, cg, cb);
        emitDot(verts, idx, px[0], py[0], phw[0], feather, cr, cg, cb);             // round caps
        emitDot(verts, idx, px[n - 1], py[n - 1], phw[n - 1], feather, cr, cg, cb);
    }
}

void App::renderInfoOverlay() {
    if (!showInfo_)
        return;
    updateInfoOverlay();

    const float pad = 12.0f;
    const float line = textFont_.lineHeight() + 3.0f;
    const size_t keyW = 14; // key column width, padded so values align
    const char* title = "MEDIA INFO  (i to close)";

    std::vector<std::string> lines;
    if (infoFields_.empty()) {
        lines.push_back("No clip under the playhead");
    } else {
        for (const auto& f : infoFields_) {
            std::string k = f.key;
            if (k.size() < keyW)
                k.append(keyW - k.size(), ' ');
            lines.push_back(k + f.value);
        }
    }

    float widestPx = textFont_.measure(renderer_, title);
    for (const auto& s : lines)
        widestPx = std::max(widestPx, textFont_.measure(renderer_, s.c_str()));

    SDL_FRect panel = { playerRect_.x + 20, playerRect_.y + 20,
                        pad * 2 + widestPx,
                        pad * 2 + line + 6 + line * (float)lines.size() };

    setColor(renderer_, kInfoPanelBg);
    jplay::fillRect(renderer_, &panel);
    setColor(renderer_, kInfoPanelEdge);
    jplay::drawRect(renderer_, &panel);

    float y = panel.y + pad;
    drawText(panel.x + pad, y, kInfoTitle, title);
    y += line + 6;
    for (const auto& s : lines) {
        drawText(panel.x + pad, y, kInfoBody, s);
        y += line;
    }
}

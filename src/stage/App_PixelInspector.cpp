// Pixel-inspector translation unit: the probe overlay in a bottom corner of the
// player stage. A nearest-neighbour magnifier around the cursor with a crosshair
// on the sampled pixel, and that pixel's value at three points of the player
// pipeline (see App::PixelProbe in App.h). Split out of App_Player.cpp; all are
// App members.
//
// The three stages are read where they actually exist rather than re-derived:
//   SRC  — the decoded frame out of the cache, the same buffer the display path
//          uploads (scene-linear halves for EXR, code values otherwise).
//   WORK — that one pixel through the OCIO input transform and the exposure, via
//          the same CpuTransform the export writer uses.
//   DISP — read back off the program texture, so it carries the display
//          transform, the grade and the tech mode exactly as the GPU produced
//          them, with nothing to drift out of step.

#include "App.h"
#include "AppInternal.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

using namespace jplay;

namespace {

// Overlay chrome. Player colours are file-local across these units rather than in
// the shared palette (see CLAUDE.md); these match the media-info overlay so the
// two read as one family.
constexpr SDL_Color kProbeBg   {  16,  17,  20, 232 }; // panel fill
constexpr SDL_Color kProbeEdge {  90, 130, 200, 255 }; // panel border
constexpr SDL_Color kProbeTitle  { 120, 180, 255, 255 }; // panel title
constexpr SDL_Color kProbeLabel  { 150, 155, 168, 255 }; // row labels (SRC / WORK / DISP)
constexpr SDL_Color kProbeValue  { 215, 218, 225, 255 }; // numbers
constexpr SDL_Color kProbeDim    { 110, 114, 124, 255 }; // footnotes, and a blank row's dashes
constexpr SDL_Color kProbeCross  { 255,  60,  60, 255 }; // magnifier crosshair
constexpr SDL_Color kProbeMagEdge   {  70,  74,  86, 255 }; // magnifier border
constexpr SDL_Color kProbeMagBg     {   8,   8,  10, 255 }; // magnifier fill past an image edge

inline void setColor(SDL_Renderer* r, SDL_Color c) {
    SDL_SetRenderDrawColor(r, c.r, c.g, c.b, c.a);
}

// The magnifier: an odd pixel count so one texel sits dead centre under the
// crosshair, at a whole zoom factor so texel edges land on pixel edges.
constexpr int   kMagPixels = 13;
constexpr int   kMagZoom   = 8;
constexpr float kMagSide   = (float)(kMagPixels * kMagZoom);
// The swatch beside it: the same square, showing the sampled pixel flat.
constexpr float kSwatchGap = 6.0f;

// Numbers change on every mouse move, and TextFont caches one texture per distinct
// string with no eviction — so the readout is drawn a character at a time. That
// caps the cache at the handful of single-glyph strings a number is made of, and
// summing per-character advances is what lets the columns line up whatever the
// font's metrics say (font.ttf is not guaranteed tabular).
float numWidth(SDL_Renderer* r, const TextFont& f, const std::string& s) {
    float w = 0.0f;
    char ch[2] = { 0, 0 };
    for (char c : s) {
        ch[0] = c;
        w += f.measure(r, ch);
    }
    return w;
}

void drawNum(SDL_Renderer* r, const TextFont& f, float x, float y, SDL_Color c,
             const std::string& s) {
    char ch[2] = { 0, 0 };
    for (char k : s) {
        ch[0] = k;
        f.draw(r, x, y, c, ch);
        x += f.measure(r, ch);
    }
}

std::string fmt(const char* spec, float v) {
    char b[32];
    SDL_snprintf(b, sizeof(b), spec, v);
    return b;
}

// Linear scRGB half -> the 8-bit sRGB code the SDR swapchain would show. The same
// encode readbackHdrProgram_ applies, kept here so the HDR path's DISP row means
// the same thing as the GL path's.
uint8_t encodeSrgb8(float c) {
    c = std::clamp(c, 0.0f, 1.0f);
    const float e = c <= 0.0031308f ? c * 12.92f
                                    : 1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f;
    return (uint8_t)std::lround(e * 255.0f);
}

} // namespace

// ── Sampling ────────────────────────────────────────────────────────────────

void App::samplePixelInspector() {
    if (!pixelInspectorOpen_) {
        probe_ = PixelProbe{};
        return;
    }
    float mx = 0.0f, my = 0.0f;
    uiHoverMouse(mx, my);
    // Cursor reaching the panel: send it to the other bottom corner rather than let it
    // sit over the pixels being probed. renderPixelInspector picks the corner up. Only
    // where the two corners don't overlap — in a player too narrow to hold the panel
    // twice the flip would land it back under the cursor and it would flicker.
    if (probePanelRect_.w > 0.0f && inRect(probePanelRect_, mx, my) &&
        probePanelRect_.w * 2.0f < programViewRect().w)
        probeRight_ = !probeRight_;

    probe_ = PixelProbe{};
    if (!programShown_ || gridView() || launcherVisible() || texW_ <= 0 || texH_ <= 0)
        return;

    // Cursor -> image pixel. programDstRect() folds in the fit, the zoom, the pan and
    // (on the Layout stage) tile 0's cell, so this holds at any framing; texPa_ undoes
    // the anamorphic stretch the draw applies, leaving a stored-pixel coordinate.
    const SDL_FRect view = programViewRect();
    if (!inRect(view, mx, my))
        return;
    const SDL_FRect dst = programDstRect();
    if (dst.w <= 0.0f || dst.h <= 0.0f)
        return;
    const float scale = dst.w / texDispW();
    if (scale <= 0.0f)
        return;
    const float pa = texPa_ > 0.0f ? texPa_ : 1.0f;
    const int px = (int)std::floor((mx - dst.x) / scale / pa);
    const int py = (int)std::floor((my - dst.y) / scale);
    if (px < 0 || py < 0 || px >= texW_ || py >= texH_)
        return;
    probe_.valid = true;
    probe_.x = px;
    probe_.y = py;

    const Clip* clip = playheadClip();
    if (!clip || clip->mediaId.empty())
        return;

    // ── SRC: the decoded frame, straight out of the cache ──
    // Mid-dissolve the cache holds the two halves and the blend only exists on the
    // GPU, so the source and working rows are the outgoing clip's and say so.
    const ProgramSource ps = programSourceAt(timeline_.playhead);
    probe_.dissolve = (ps.a == clip) && ps.b != nullptr;
    const CacheKey key{ clip->mediaId, clip->sourceOffset + (timeline_.playhead - clip->timelineStart) };
    FramePtr frame = cache_->get(key);
    if (!frame || frame->width != texW_ || frame->height != texH_)
        return;
    const size_t i = (size_t)py * frame->width + px;
    if (frame->linearRgb.size() >= (i + 1) * 3) {
        probe_.srcFloat = true;
        for (int k = 0; k < 3; ++k)
            probe_.src[k] = (float)frame->linearRgb[i * 3 + k];
    } else if (frame->rgba16.size() >= (i + 1) * 4) {
        probe_.srcMax = 65535.0f;
        for (int k = 0; k < 4; ++k)
            probe_.src[k] = (float)frame->rgba16[i * 4 + k];
    } else if (frame->rgba.size() >= (i + 1) * 4) {
        probe_.srcMax = 255.0f;
        for (int k = 0; k < 4; ++k)
            probe_.src[k] = (float)frame->rgba[i * 4 + k];
    } else {
        return;
    }
    probe_.srcValid = true;

    // ── WORK: the input transform and the exposure, on this one pixel ──
    // CpuTransform takes a Frame, so the pixel is wrapped in a 1x1 one — cheaper
    // than a special case, and it goes through exactly the chain the export writer
    // and the CPU display fallback use.
    if (ocio_.isReady() && ocio_.isEnabled()) {
        if (auto media = timeline_.findMediaById(clip->mediaId)) {
            const std::string cs = mediaColorSpace(*media);
            if (cs != probeXfCs_ || ocio_.version() != probeXfVersion_ ||
                grade_.gain != probeXfGain_) {
                probeXf_ = ocio_.cpuTransformFor(cs, grade_.gain);
                probeXfCs_ = cs;
                probeXfVersion_ = ocio_.version();
                probeXfGain_ = grade_.gain;
            }
            if (probeXf_.valid()) {
                Frame one;
                one.width = one.height = 1;
                if (probe_.srcFloat) {
                    one.linearRgb = { Imath::half(probe_.src[0]), Imath::half(probe_.src[1]),
                                      Imath::half(probe_.src[2]) };
                } else if (probe_.srcMax > 255.0f) {
                    one.rgba16 = { (uint16_t)probe_.src[0], (uint16_t)probe_.src[1],
                                   (uint16_t)probe_.src[2], (uint16_t)probe_.src[3] };
                } else {
                    one.rgba = { (uint8_t)probe_.src[0], (uint8_t)probe_.src[1],
                                 (uint8_t)probe_.src[2], (uint8_t)probe_.src[3] };
                }
                probeXf_.applyToWorking(one, probe_.work);
                probe_.workValid = true;
            }
        }
    }

    // ── DISP: one texel off the program texture ──
    if (hdrPipeline_) {
        // The HDR pipeline applies its display transform at draw time, so the only
        // place the display image exists at native resolution is the offscreen target
        // renderPlayer routes through — which it keeps doing while the inspector is
        // open precisely so this read has something to sample. Its values are linear
        // scRGB halves; encode them the way the SDR swapchain would.
        if (hdrProgramTex_ && hdrProgW_ >= texW_ && hdrProgH_ >= texH_) {
            SDL_Texture* prev = SDL_GetRenderTarget(renderer_);
            SDL_SetRenderTarget(renderer_, hdrProgramTex_);
            const SDL_Rect rect{ px, py, 1, 1 };
            if (SDL_Surface* s = SDL_RenderReadPixels(renderer_, &rect)) {
                const bool halfSrc = (s->format == SDL_PIXELFORMAT_RGBA64_FLOAT);
                SDL_Surface* cv = (halfSrc || s->format == SDL_PIXELFORMAT_RGBA128_FLOAT)
                                      ? s : SDL_ConvertSurface(s, SDL_PIXELFORMAT_RGBA128_FLOAT);
                if (cv) {
                    for (int k = 0; k < 3; ++k) {
                        const float v = halfSrc ? (float)((const Imath::half*)cv->pixels)[k]
                                                : ((const float*)cv->pixels)[k];
                        probe_.disp[k] = encodeSrgb8(v);
                    }
                    probe_.dispValid = true;
                    if (cv != s)
                        SDL_DestroySurface(cv);
                }
                SDL_DestroySurface(s);
            }
            SDL_SetRenderTarget(renderer_, prev);
        }
    } else if (const uint8_t* p = output_.readProgramRect(texture_, px, py, 1, 1)) {
        for (int k = 0; k < 3; ++k)
            probe_.disp[k] = p[k];
        probe_.dispValid = true;
    }
}

// ── Drawing ─────────────────────────────────────────────────────────────────

void App::renderPixelInspector() {
    probePanelRect_ = SDL_FRect{};
    if (!pixelInspectorOpen_ || gridView() || launcherVisible())
        return;

    const float pad  = 10.0f * dpiScale;
    const float line = textFont_.lineHeight() + 3.0f;
    const char* title = "PIXEL  (p to close)";

    // Rows, as label plus three right-aligned columns. An empty column string draws
    // a dash, so a stage with nothing to say keeps its row and its alignment.
    struct Row { const char* label; std::string col[3]; };
    Row rows[3] = { { "SRC" }, { "WORK" }, { "DISP" } };
    if (probe_.srcValid) {
        const char* spec = probe_.srcFloat ? "%.4f" : "%.0f";
        for (int k = 0; k < 3; ++k)
            rows[0].col[k] = fmt(spec, probe_.src[k]);
    }
    if (probe_.workValid)
        for (int k = 0; k < 3; ++k)
            rows[1].col[k] = fmt("%.4f", probe_.work[k]);
    if (probe_.dispValid)
        for (int k = 0; k < 3; ++k)
            rows[2].col[k] = fmt("%.0f", (float)probe_.disp[k]);

    const std::string xy = probe_.valid
                               ? "x " + std::to_string(probe_.x) + "   y " + std::to_string(probe_.y)
                               : std::string("x -   y -");

    const float bodyH = line + 6.0f          // title
                        + kMagSide + 6.0f    // magnifier and swatch
                        + line               // x / y
                        + line               // R / G / B header
                        + line * 3.0f;        // the three stages

    // A square, and the same square whatever the probe reads: nothing about the
    // panel's size follows the content, so it neither breathes as the coordinate
    // gains a digit nor changes shape over a cut. The side is set by whichever of
    // the fixed chrome needs most — the stacked body, the title, or the two boxes
    // side by side — and the number columns then divide up the width that leaves
    // rather than setting it, which is what keeps them compact enough to fit.
    const float labelW = numWidth(renderer_, textFont_, "WORK") + 10.0f * dpiScale;
    const int   cols   = 3;
    const float boxesW = kMagSide + kSwatchGap + kMagSide;
    const float side   = pad * 2.0f + std::max({ bodyH, boxesW,
                                                 textFont_.measure(renderer_, title) });
    const float colW   = (side - pad * 2.0f - labelW) / (float)cols;
    // A bottom corner of the player stage — whichever one the cursor has not chased it
    // out of. The stage, not the image: a frame narrower or shorter than the player
    // leaves pillar/letterbox the panel should be free to sit in, and a zoomed-past-
    // the-edges image should not carry it off the stage either.
    const SDL_FRect view = programViewRect();
    const float inset  = 20.0f * dpiScale;
    SDL_FRect panel = { probeRight_ ? view.x + view.w - inset - side : view.x + inset,
                        view.y + view.h - inset - side, side, side };
    panel.x = std::clamp(panel.x, view.x + 8.0f, std::max(view.x + 8.0f, view.x + view.w - panel.w - 8.0f));
    panel.y = std::clamp(panel.y, view.y + 8.0f, std::max(view.y + 8.0f, view.y + view.h - panel.h - 8.0f));
    probePanelRect_ = panel;

    setColor(renderer_, kProbeBg);
    jplay::fillRect(renderer_, &panel);
    setColor(renderer_, kProbeEdge);
    jplay::drawRect(renderer_, &panel);

    float y = panel.y + (side - bodyH) * 0.5f;
    drawText(panel.x + pad, y, kProbeTitle, title);
    y += line + 6.0f;

    // ── Magnifier ──
    const SDL_FRect mag = { panel.x + pad, y, kMagSide, kMagSide };
    setColor(renderer_, kProbeMagBg);
    jplay::fillRect(renderer_, &mag);
    // The display image at kMagZoom:1, nearest-neighbour, so the box shows texels
    // rather than the renderer's smoothing of them. Source pixels, not display ones —
    // an anamorphic frame reads square here, which is what a probe wants. Near an
    // image edge the source rect is clipped and the destination shrinks with it, so
    // the centre texel stays under the crosshair either way.
    SDL_Texture* src = hdrPipeline_ ? hdrProgramTex_ : texture_;
    if (probe_.valid && src) {
        const int half = kMagPixels / 2;
        const int sx0 = probe_.x - half, sy0 = probe_.y - half;
        const int cx0 = std::max(sx0, 0), cy0 = std::max(sy0, 0);
        const int cx1 = std::min(sx0 + kMagPixels, texW_);
        const int cy1 = std::min(sy0 + kMagPixels, texH_);
        if (cx1 > cx0 && cy1 > cy0) {
            const SDL_FRect rect = { (float)cx0, (float)cy0,
                                     (float)(cx1 - cx0), (float)(cy1 - cy0) };
            const SDL_FRect d = { mag.x + (float)(cx0 - sx0) * kMagZoom,
                                  mag.y + (float)(cy0 - sy0) * kMagZoom,
                                  (float)(cx1 - cx0) * kMagZoom,
                                  (float)(cy1 - cy0) * kMagZoom };
            SDL_ScaleMode prev = SDL_SCALEMODE_LINEAR;
            SDL_GetTextureScaleMode(src, &prev);
            SDL_SetTextureScaleMode(src, SDL_SCALEMODE_NEAREST);
            SDL_RenderTexture(renderer_, src, &rect, &d);
            SDL_SetTextureScaleMode(src, prev);
        }
    }
    // Crosshair: four arms stopping at the sampled texel, plus a box round it, so the
    // pixel being reported stays visible instead of being painted over.
    {
        const float k = (float)kMagZoom;
        const SDL_FRect ctr = { mag.x + (float)(kMagPixels / 2) * k,
                                mag.y + (float)(kMagPixels / 2) * k, k, k };
        setColor(renderer_, kProbeCross);
        jplay::drawRect(renderer_, &ctr);
    }
    setColor(renderer_, kProbeMagEdge);
    jplay::drawRect(renderer_, &mag);

    // ── Swatch ──
    // The DISP value as a flat patch: the sampled pixel as the screen shows it,
    // without the neighbours the magnifier surrounds it with. Off the image, or
    // before a readback lands, it reads as the magnifier's empty fill.
    const SDL_FRect swatch = { mag.x + kMagSide + kSwatchGap, mag.y, kMagSide, kMagSide };
    if (probe_.dispValid)
        SDL_SetRenderDrawColor(renderer_, probe_.disp[0], probe_.disp[1], probe_.disp[2], 255);
    else
        setColor(renderer_, kProbeMagBg);
    jplay::fillRect(renderer_, &swatch);
    setColor(renderer_, kProbeMagEdge);
    jplay::drawRect(renderer_, &swatch);
    y += kMagSide + 6.0f;

    // ── Coordinates, then one row per stage ──
    drawNum(renderer_, textFont_, panel.x + pad, y, probe_.valid ? kProbeValue : kProbeDim, xy);
    y += line;
    {
        static const char* kChan[3] = { "R", "G", "B" };
        for (int k = 0; k < cols; ++k) {
            const float w = textFont_.measure(renderer_, kChan[k]);
            drawText(panel.x + pad + labelW + colW * (k + 1) - w, y, kProbeDim, kChan[k]);
        }
        y += line;
    }
    for (const Row& r : rows) {
        drawText(panel.x + pad, y, kProbeLabel, r.label);
        for (int k = 0; k < cols; ++k) {
            const bool blank = r.col[k].empty();
            const std::string t = blank ? std::string("-") : r.col[k];
            const float w = numWidth(renderer_, textFont_, t);
            drawNum(renderer_, textFont_, panel.x + pad + labelW + colW * (k + 1) - w,
                    y, blank ? kProbeDim : kProbeValue, t);
        }
        y += line;
    }
}

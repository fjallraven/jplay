// Draw-tool (pencil) panel: a mutually-exclusive left pane (like Overview / Grade),
// shown while pencil mode is on. Holds a Clear button, a Pencil Size slider, and a
// hue wheel that sets the global markup color. Split out of App_Grade.cpp; the
// size/color it edits feed renderAnnotations. App members.

#include "App.h"
#include "AppInternal.h"
#include "Layout.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

using namespace jplay;

namespace {

constexpr SDL_Color kHeader  = { 160, 170, 200, 255 };
constexpr SDL_Color kLabel   = { 200, 205, 215, 255 };
constexpr SDL_Color kValue   = { 150, 190, 255, 255 };
constexpr SDL_Color kSection = { 120, 150, 210, 255 };

// Draw-tool pencil-size slider range (multiplier on the pressure-sim width).
constexpr float kPencilSizeMin = 0.25f;
constexpr float kPencilSizeMax = 4.0f;

inline void setCol(SDL_Renderer* r, int rr, int gg, int bb, int aa = 255) {
    SDL_SetRenderDrawColor(r, (Uint8)rr, (Uint8)gg, (Uint8)bb, (Uint8)aa);
}

// HSL(h deg, s, l) -> 0..1 RGB. Matches the reference wheel: at s=1, l runs from
// black (0) through the pure hue (0.5) to white (1).
void hsl(float h, float s, float l, float& R, float& G, float& B) {
    h = std::fmod(std::fmod(h, 360.0f) + 360.0f, 360.0f);
    float a = s * std::min(l, 1.0f - l);
    auto f = [&](float n) {
        float k = std::fmod(n + h / 30.0f, 12.0f);
        return l - a * std::max(-1.0f, std::min(std::min(k - 3.0f, 9.0f - k), 1.0f));
    };
    R = f(0.0f); G = f(8.0f); B = f(4.0f);
}

// Draw a filled HSL color disk: angle -> hue, radius -> lightness (white at the
// center, black at the outer edge), saturation fixed at 100%. The pure hues sit at
// mid-radius (lightness 0.5), so the mesh is subdivided radially as well as around
// so the GPU's linear interpolation tracks the non-linear white->hue->black ramp.
void drawColorWheel(SDL_Renderer* r, float cx, float cy, float outerR, float /*hueDeg*/) {
    const int N = 96;   // angular segments
    const int M = 12;   // radial bands

    // The mesh only depends on (cx, cy, outerR), which only change on resize/DPI
    // change, not per frame — cache it instead of rebuilding on every draw call.
    static std::vector<SDL_Vertex> verts;
    static std::vector<int> idx;
    static float lastCx = 0.0f, lastCy = 0.0f, lastOuterR = -1.0f;
    if (verts.empty() || cx != lastCx || cy != lastCy || outerR != lastOuterR) {
        auto vert = [&](int j, int i) {
            float f = (float)j / M;                    // 0 at center .. 1 at edge
            float ang = (float)i / N * 6.2831853f;
            float rad = f * outerR;
            float rr, gg, bb;
            hsl(ang * 57.29578f, 1.0f, 1.0f - f, rr, gg, bb);
            SDL_Vertex v{};
            v.position = { cx + std::cos(ang) * rad, cy + std::sin(ang) * rad };
            v.color = { rr, gg, bb, 1.0f };
            return v;
        };
        verts.clear();
        verts.reserve((M + 1) * (N + 1));
        for (int j = 0; j <= M; ++j)
            for (int i = 0; i <= N; ++i)
                verts.push_back(vert(j, i));

        if (idx.empty()) { // topology (N, M) never changes; build once for the process
            auto vi = [&](int j, int i) { return j * (N + 1) + i; };
            idx.reserve(M * N * 6);
            for (int j = 0; j < M; ++j) {
                for (int i = 0; i < N; ++i) {
                    int a = vi(j, i), b = vi(j, i + 1), c = vi(j + 1, i), d = vi(j + 1, i + 1);
                    idx.push_back(a); idx.push_back(b); idx.push_back(c);
                    idx.push_back(b); idx.push_back(d); idx.push_back(c);
                }
            }
        }
        lastCx = cx; lastCy = cy; lastOuterR = outerR;
    }
    SDL_RenderGeometry(r, nullptr, verts.data(), (int)verts.size(), idx.data(), (int)idx.size());
}

// A small white-over-black square marker (matches the prior hue-wheel marker).
void drawMarker(SDL_Renderer* r, float mx, float my) {
    setCol(r, 255, 255, 255);
    SDL_FRect mk = { mx - 4.0f, my - 4.0f, 8.0f, 8.0f };
    jplay::drawRect(r, &mk);
    setCol(r, 0, 0, 0);
    SDL_FRect mk2 = { mx - 3.0f, my - 3.0f, 6.0f, 6.0f };
    jplay::drawRect(r, &mk2);
}

} // namespace

// ─── Draw-tool panel ─────────────────────────────────────────────────────────
// A mutually-exclusive left pane (flush against the icon strip, frame begins at its
// right edge) while pencil mode is on. Holds a Clear button, a Pencil Size slider
// (a multiplier on the speed-based stroke width), and a hue
// wheel that sets the global markup color. Widget hit regions are registered here
// and consumed by drawPanelHandlePress on the next event (one-frame lag, as with
// the grade widgets). Size/color feed renderAnnotations.
void App::renderDrawPanel() {
    if (!pencilMode_ || drawPanelRect_.w <= 0.0f)
        return;
    const SDL_FRect& R = drawPanelRect_;
    setCol(renderer_, kPanelBg.r, kPanelBg.g, kPanelBg.b);
    jplay::fillRect(renderer_, &R);
    // Right-edge border (1px).
    setCol(renderer_, 60, 64, 74);
    jplay::drawLine(renderer_, R.x + R.w - 0.5f, R.y, R.x + R.w - 0.5f, R.y + R.h);

    const float pad = 10.0f * dpiScale;
    const float lineH = textFont_.lineHeight();
    SDL_FRect body = inset(R, pad, 0.0f);
    gapTop(body, 8.0f);

    SDL_FRect header = cutTop(body, lineH);
    drawText(header.x, header.y, kHeader, "DRAW");
    gapTop(body, 10.0f);

    // Clear button (eraser icon + label).
    drawClearRect_ = cutTop(body, 26.0f * dpiScale);
    gapTop(body, 14.0f);
    bool ch = hoveredDrawClear_;
    setCol(renderer_, ch ? 74 : 48, ch ? 52 : 40, ch ? 54 : 44);
    jplay::fillRect(renderer_, &drawClearRect_);
    setCol(renderer_, 118, 92, 96);
    jplay::drawRect(renderer_, &drawClearRect_);
    {
        SDL_FRect inner = drawClearRect_;
        gapLeft(inner, 6.0f);
        SDL_FRect ir = cutLeft(inner, drawClearRect_.h); // square icon well
        icons_.drawGlyph(renderer_, 0xF01FE /*ICON_MDI_ERASER*/, ir, kLabel);
        gapLeft(inner, 4.0f);
        const SDL_FRect lbl = centerV(inner, lineH);
        drawText(lbl.x, lbl.y, kLabel, "Clear");
    }

    // Pencil Size slider.
    {
        const float trackH = 6.0f * dpiScale;
        drawSizeRect_ = cutTop(body, lineH + 6.0f + trackH);
        gapTop(body, 16.0f);

        // Label left, value right-aligned in the same row; track underneath.
        SDL_FRect rows = drawSizeRect_;
        SDL_FRect head = cutTop(rows, lineH);
        char buf[16];
        std::snprintf(buf, sizeof(buf), "%.2fx", pencilSizeMul_);
        const SDL_FRect valueSlot = cutRight(head, textFont_.measure(renderer_, buf));
        drawText(head.x, head.y, kLabel, "Pencil Size");
        drawText(valueSlot.x, valueSlot.y, kValue, buf);

        gapTop(rows, 6.0f);
        SDL_FRect track = cutTop(rows, trackH);
        setCol(renderer_, 42, 45, 52);
        jplay::fillRect(renderer_, &track);
        float t = std::clamp((pencilSizeMul_ - kPencilSizeMin) / (kPencilSizeMax - kPencilSizeMin), 0.0f, 1.0f);
        float hx = track.x + t * track.w;
        SDL_FRect fill = { track.x, track.y, hx - track.x, track.h };
        setCol(renderer_, 90, 120, 180);
        jplay::fillRect(renderer_, &fill);
        setCol(renderer_, 70, 74, 84);
        jplay::drawRect(renderer_, &track);
        SDL_FRect knob = { hx - 3.0f, track.y - 3.0f, 6.0f, track.h + 6.0f };
        setCol(renderer_, 210, 214, 222);
        jplay::fillRect(renderer_, &knob);
    }

    // Color: filled HSL disk. Angle = hue, radius = lightness (white center -> black
    // edge, saturation 100%), plus a current-color swatch below.
    SDL_FRect sect = cutTop(body, lineH);
    drawText(sect.x, sect.y, kSection, "COLOR");
    gapTop(body, 6.0f);
    float rad = std::min(body.w * 0.42f, 64.0f * dpiScale);
    const SDL_FRect band = cutTop(body, rad * 2.0f);
    gapTop(body, 10.0f);
    drawHueRect_ = centerH(band, rad * 2.0f);
    float cx = drawHueRect_.x + rad;
    float cy = drawHueRect_.y + rad;
    drawColorWheel(renderer_, cx, cy, rad, pencilHue_);
    // Marker at the current color: radius from lightness (r = 1-light), angle = hue.
    {
        float ha = pencilHue_ * 0.017453293f;
        float rr = (1.0f - pencilLight_) * rad;
        drawMarker(renderer_, cx + std::cos(ha) * rr, cy + std::sin(ha) * rr);
    }

    float cr, cg, cb;
    pencilColorRGB(cr, cg, cb);
    SDL_FRect sw = cutTop(body, 16.0f * dpiScale);
    setCol(renderer_, (int)(cr * 255.0f), (int)(cg * 255.0f), (int)(cb * 255.0f));
    jplay::fillRect(renderer_, &sw);
    setCol(renderer_, 70, 74, 84);
    jplay::drawRect(renderer_, &sw);
}

// Current stroke color (0..1) straight from the wheel's HSL state (saturation 100%).
void App::pencilColorRGB(float& r, float& g, float& b) const {
    hsl(pencilHue_, 1.0f, pencilLight_, r, g, b);
}

// Press (or drag continuation) inside the draw panel. Returns true if consumed.
bool App::drawPanelHandlePress(float mx, float my) {
    auto applySize = [&] {
        float t = std::clamp((mx - drawSizeRect_.x) / drawSizeRect_.w, 0.0f, 1.0f);
        pencilSizeMul_ = kPencilSizeMin + t * (kPencilSizeMax - kPencilSizeMin);
    };
    float cx = drawHueRect_.x + drawHueRect_.w * 0.5f;
    float cy = drawHueRect_.y + drawHueRect_.h * 0.5f;
    float rad = drawHueRect_.w * 0.5f;
    // Angle picks hue; distance from center picks lightness (center = white,
    // edge = black). Distance is clamped so dragging past the rim pins to black.
    auto applyWheel = [&] {
        pencilHue_ = std::fmod(std::atan2(my - cy, mx - cx) * 57.29578f + 360.0f, 360.0f);
        float dist = std::hypot(mx - cx, my - cy);
        pencilLight_ = std::clamp(1.0f - dist / rad, 0.0f, 1.0f);
    };
    // Continuation of an active drag: update the grabbed widget from anywhere.
    if (drawDragSize_)  { applySize();  return true; }
    if (drawDragWheel_) { applyWheel(); return true; }
    // Fresh press: hit-test the widgets.
    if (inRect(drawClearRect_, mx, my)) { annotStrokes_.clear(); annotDrawing_ = false; flushAnnotBuffer(); broadcastClearStrokes(); return true; }
    if (inRect(drawSizeRect_, mx, my))  { drawDragSize_ = true; applySize(); return true; }
    if (rad > 0.0f && std::hypot(mx - cx, my - cy) <= rad) {
        drawDragWheel_ = true; applyWheel(); return true;
    }
    return false;
}

// Color grading panel: a left-expand pane (mutually exclusive with the
// ProjectExplorer / Overview) with a 6-tool top bar and an interactive control
// area for each tool. A single global/session grade (grade_) is applied as a GPU
// post-pass in renderPlayer; every edit here calls markGradeDirty() to force the
// player texture to rebuild and re-grade. App members, split out of App.cpp.
//
// Tool bar icons (named here so the font subsetter includes their glyphs):
//   ICON_MDI_WHITE_BALANCE_SUNNY  ICON_MDI_VECTOR_CURVE  ICON_MDI_CIRCLE_HALF_FULL

#include "App.h"
#include "AppInternal.h"
#include "Layout.h"
#include "SkinColors.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

using namespace jplay;

namespace {

const SDL_Color& kHeader  = jplay::colors().header;
const SDL_Color& kDim     = jplay::colors().labelOff;
const SDL_Color& kValue   = jplay::colors().value;
const SDL_Color& kSection = jplay::colors().section;
// This panel's label is brighter than the settings-panel --label, and matches the
// idle icon tint; the palette has no separate entry for it.
constexpr SDL_Color kLabel   = { 200, 205, 215, 255 };

inline void setCol(SDL_Renderer* r, int rr, int gg, int bb, int aa = 255) {
    SDL_SetRenderDrawColor(r, (Uint8)rr, (Uint8)gg, (Uint8)bb, (Uint8)aa);
}

// HSV(h deg, s, v) -> 0..255 RGB. Mirrors GradeGpu's hsv2rgb for UI swatches.
void hsv(float h, float s, float v, Uint8& R, Uint8& G, Uint8& B) {
    h = std::fmod(std::fmod(h, 360.0f) + 360.0f, 360.0f) / 60.0f;
    float c = v * s, x = c * (1.0f - std::fabs(std::fmod(h, 2.0f) - 1.0f)), m = v - c;
    float r = 0, g = 0, b = 0;
    if (h < 1) { r = c; g = x; } else if (h < 2) { r = x; g = c; }
    else if (h < 3) { g = c; b = x; } else if (h < 4) { g = x; b = c; }
    else if (h < 5) { r = x; b = c; } else { r = c; b = x; }
    R = (Uint8)std::lround((r + m) * 255.0f);
    G = (Uint8)std::lround((g + m) * 255.0f);
    B = (Uint8)std::lround((b + m) * 255.0f);
}

// Draw a hue wheel (gray center -> saturated rim) as a triangle fan.
void drawHueWheel(SDL_Renderer* r, float cx, float cy, float rad) {
    const int N = 48;
    std::vector<SDL_Vertex> verts;
    verts.reserve(N + 2);
    SDL_Vertex c{};
    c.position = { cx, cy };
    c.color = { 0.5f, 0.5f, 0.5f, 1.0f };
    verts.push_back(c);
    for (int i = 0; i <= N; ++i) {
        float a = (float)i / N * 6.2831853f;
        Uint8 R, G, B;
        // hue 0 at +x, increasing CCW; match marker mapping (atan2(y,x))
        hsv(a * 57.29578f, 1.0f, 1.0f, R, G, B);
        SDL_Vertex v{};
        v.position = { cx + std::cos(a) * rad, cy + std::sin(a) * rad };
        v.color = { R / 255.0f, G / 255.0f, B / 255.0f, 1.0f };
        verts.push_back(v);
    }
    std::vector<int> idx;
    idx.reserve(N * 3);
    for (int i = 1; i <= N; ++i) { idx.push_back(0); idx.push_back(i); idx.push_back(i + 1); }
    SDL_RenderGeometry(r, nullptr, verts.data(), (int)verts.size(),
                       idx.data(), (int)idx.size());
}

} // namespace

// ─── Shared slider widget ────────────────────────────────────────────────────
// Cuts a labelled horizontal slider (and the gap before the next control) off
// the top of `body`, draws it and registers its hit region. grad: 0 plain,
// 1 temp gradient, 2 tint gradient, 3 hue rainbow.
void App::gradeSlider(SDL_FRect& body, const char* label, float* value,
                      float lo, float hi, float def, int grad) {
    const float lineH = textFont_.lineHeight();
    const float trackH = 6.0f * dpiScale;

    const SDL_FRect block = cutTop(body, lineH + 6.0f + trackH);
    gapTop(body, 6.0f);

    // Label left, value right-aligned in the same row; track underneath.
    SDL_FRect rows = block;
    SDL_FRect head = cutTop(rows, lineH);
    char buf[32];
    if (lo < 0.0f) std::snprintf(buf, sizeof(buf), "%+.*f", (hi - lo) <= 12.0f ? 2 : 0, *value);
    else           std::snprintf(buf, sizeof(buf), "%.*f", (hi - lo) <= 12.0f ? 2 : 0, *value);
    const SDL_FRect valueSlot = cutRight(head, textFont_.measure(renderer_, buf));
    drawText(head.x, head.y, kLabel, label);
    drawText(valueSlot.x, valueSlot.y, kValue, buf);

    gapTop(rows, 6.0f);
    SDL_FRect track = cutTop(rows, trackH);
    if (grad == 1) { // temperature: cool blue -> warm amber
        const int S = 48;
        for (int i = 0; i < S; ++i) {
            float t = (float)i / (S - 1);
            SDL_FRect rect = { track.x + t * track.w, track.y, track.w / S + 1.0f, track.h };
            setCol(renderer_, (int)(70 + t * 160), (int)(130 + t * 40), (int)(200 - t * 110));
            jplay::fillRect(renderer_, &rect);
        }
    } else if (grad == 2) { // tint: green -> magenta
        const int S = 48;
        for (int i = 0; i < S; ++i) {
            float t = (float)i / (S - 1);
            SDL_FRect rect = { track.x + t * track.w, track.y, track.w / S + 1.0f, track.h };
            setCol(renderer_, (int)(120 + t * 80), (int)(200 - t * 90), (int)(120 + t * 80));
            jplay::fillRect(renderer_, &rect);
        }
    } else if (grad == 3) { // hue rainbow
        const int S = 60;
        for (int i = 0; i < S; ++i) {
            float t = (float)i / (S - 1);
            Uint8 R, G, B; hsv(t * 360.0f, 0.9f, 1.0f, R, G, B);
            SDL_FRect rect = { track.x + t * track.w, track.y, track.w / S + 1.0f, track.h };
            setCol(renderer_, R, G, B);
            jplay::fillRect(renderer_, &rect);
        }
    } else {
        // Plain track: the gradient variants above paint their own fill, so they
        // only take the outline below.
        setCol(renderer_, 52, 54, 62);
        jplay::fillRect(renderer_, &track);
    }
    {
        const SDL_Color& color = colors().border;
        setCol(renderer_, color.r, color.g, color.b);
        jplay::drawRect(renderer_, &track);
    }

    // Centre tick for bipolar sliders.
    if (lo < 0.0f && hi > 0.0f) {
        float zx = track.x + (0.0f - lo) / (hi - lo) * track.w;
        setCol(renderer_, 110, 114, 124);
        jplay::drawLine(renderer_, zx, track.y - 2.0f, zx, track.y + track.h + 2.0f);
    }

    float t = std::clamp((*value - lo) / (hi - lo), 0.0f, 1.0f);
    float hx = track.x + t * track.w;
    SDL_FRect handle = { hx - 3.0f, track.y - 3.0f, 6.0f, track.h + 6.0f };
    {
        const SDL_Color& color = colors().gsliderKnob;
        setCol(renderer_, color.r, color.g, color.b);
        jplay::fillRect(renderer_, &handle);
    }

    gradeSliders_.push_back({ value, lo, hi, def, block, grad, label });
}

// ─── Panel ───────────────────────────────────────────────────────────────────
void App::renderGradePanel() {
    if (!panelOpen(kPanelGrade))
        return;
    gradeSliders_.clear();
    gradeWheels_.clear();

    SDL_FRect panel = beginLeftPanel();

    const float pad = 10.0f * dpiScale;
    const float lineH = textFont_.lineHeight();
    SDL_FRect body = inset(panel, pad, 0.0f);
    gapTop(body, 8.0f);

    // Header: title, with the master enable checkbox and Reset cut off the right.
    // Both boxes are shorter than the row and sit at its top, as they always have.
    SDL_FRect header = cutTop(body, lineH);
    const char* resetTxt = "Reset";
    const float rtw = textFont_.measure(renderer_, resetTxt);
    const SDL_FRect enableSlot = cutRight(header, 16.0f);
    gapRight(header, 8.0f);
    const SDL_FRect resetSlot = cutRight(header, rtw + 10.0f);
    drawText(header.x, header.y, kHeader, "COLOR");

    gradeEnableRect_ = { enableSlot.x, enableSlot.y - 1.0f, 16.0f, 16.0f };
    setCol(renderer_, grade_.enabled ? 70 : 44, grade_.enabled ? 110 : 46, grade_.enabled ? 180 : 52);
    jplay::fillRect(renderer_, &gradeEnableRect_);
    setCol(renderer_, 90, 94, 104);
    jplay::drawRect(renderer_, &gradeEnableRect_);
    if (grade_.enabled) {
        const SDL_FRect& rect = gradeEnableRect_;
        setCol(renderer_, 235, 238, 245);
        jplay::drawLine(renderer_, rect.x + 3, rect.y + 8, rect.x + 6, rect.y + 11);
        jplay::drawLine(renderer_, rect.x + 6, rect.y + 11, rect.x + 12, rect.y + 4);
    }
    gradeResetRect_ = { resetSlot.x, resetSlot.y - 2.0f, resetSlot.w, 18.0f };
    setCol(renderer_, 48, 50, 58);
    jplay::fillRect(renderer_, &gradeResetRect_);
    setCol(renderer_, 80, 82, 90);
    jplay::drawRect(renderer_, &gradeResetRect_);
    drawText(gradeResetRect_.x + 5.0f, header.y, kDim, resetTxt);
    gapTop(body, 8.0f);

    // Top bar: three tool buttons, thirds of the row.
    static const uint32_t kToolIcon[3] = {
        0xF05A8, // white-balance-sunny  (Basic)
        0xF0559, // vector-curve         (Curves)
        0xF1396, // circle-half-full     (Wheels)
    };
    const float bh = 44.0f * dpiScale;
    SDL_FRect toolbar = cutTop(body, bh);
    const float bw = toolbar.w / 3.0f;
    for (int i = 0; i < 3; ++i) {
        SDL_FRect br = cutLeft(toolbar, bw);
        gapRight(br, 3.0f); // separation between buttons
        gradeToolRects_[i] = br;
        bool act = gradeTool_ == i;
        setCol(renderer_, act ? 55 : 40, act ? 78 : 42, act ? 130 : 48);
        jplay::fillRect(renderer_, &br);
        setCol(renderer_, act ? 110 : 70, act ? 140 : 72, act ? 200 : 80);
        jplay::drawRect(renderer_, &br);
        // drawGlyph derives its padding from the rect's WIDTH, so a wide-short
        // button starves the glyph vertically. Draw into a square region sized to
        // the button height (centred), matching the side-panel icon sizing.
        SDL_FRect ir = centerH(br, bh);
        icons_.drawGlyph(renderer_, kToolIcon[i], ir,
                         act ? SDL_Color{ 180, 210, 255, 255 } : SDL_Color{ 190, 195, 205, 255 },
                         0.18f);
    }
    gapTop(body, 8.0f);
    const SDL_FRect rule = cutTop(body, 1.0f);
    setCol(renderer_, 50, 53, 60);
    jplay::drawLine(renderer_, rule.x, rule.y, rule.x + rule.w, rule.y);
    gapTop(body, 7.0f);

    // Content area, clipped + scrollable. The clip rect spans the full panel
    // width; only the layout is inset.
    SDL_FRect view = body;
    gapBottom(view, 6.0f);
    SDL_Rect clip = { (int)panel.x, (int)view.y, (int)panel.w, (int)view.h };
    SDL_SetRenderClipRect(renderer_, &clip);
    const float contentTop = view.y - gradeScroll_;
    SDL_FRect content = { view.x, contentTop, view.w, kUnbounded };
    switch (gradeTool_) {
    case 0: renderGradeBasic(content); break;
    case 1: renderGradeCurves(content); break;
    case 2: renderGradeWheels(content); break;
    }
    SDL_SetRenderClipRect(renderer_, nullptr);

    gradeContentH_ = content.y - contentTop; // what the tool cut off, i.e. its height
    gradeScroll_ = std::clamp(gradeScroll_, 0.0f, std::max(0.0f, gradeContentH_ - view.h));
    drawScrollbar(renderer_, view, gradeContentH_, gradeScroll_, dpiScale, true);
}

// ─── Tool: Basic Correction ──────────────────────────────────────────────────
void App::renderGradeBasic(SDL_FRect& body) {
    auto sect = [&](const char* s) {
        SDL_FRect row = cutTop(body, textFont_.lineHeight());
        drawText(row.x, row.y, kSection, s);
        gapTop(body, 4.0f);
    };
    sect("EXPOSURE");
    gradeSlider(body, "Gain", &grade_.gain, -5, 5, 0);
    gradeSlider(body, "Gamma", &grade_.gamma, 0.1f, 4.0f, 1.0f);
    sect("COLOR");
    gradeSlider(body, "Temperature", &grade_.temperature, -100, 100, 0, 1);
    gradeSlider(body, "Tint", &grade_.tint, -100, 100, 0, 2);
    gradeSlider(body, "Saturation", &grade_.saturation, -100, 100, 0);
}

// ─── Tool: 3-Way Color Wheels ────────────────────────────────────────────────
void App::renderGradeWheels(SDL_FRect& body) {
    struct Z { const char* name; grade::Wheel* wheel; };
    Z zones[3] = { { "Shadows", &grade_.shadowsWheel },
                   { "Midtones", &grade_.midWheel },
                   { "Highlights", &grade_.highWheel } };
    float rad = std::min(body.w * 0.30f, 60.0f * dpiScale);
    for (int i = 0; i < 3; ++i) {
        SDL_FRect title = cutTop(body, textFont_.lineHeight());
        drawText(title.x, title.y, kSection, zones[i].name);
        gapTop(body, 4.0f);
        const SDL_FRect band = cutTop(body, rad * 2.0f);
        const SDL_FRect wheel = centerH(band, rad * 2.0f);
        float cx = wheel.x + rad;
        float cy = wheel.y + rad;
        drawHueWheel(renderer_, cx, cy, rad);
        // crosshair
        setCol(renderer_, 20, 20, 24, 160);
        jplay::drawLine(renderer_, cx - rad, cy, cx + rad, cy);
        jplay::drawLine(renderer_, cx, cy - rad, cx, cy + rad);
        // marker
        grade::Wheel* wh = zones[i].wheel;
        float mx = cx + wh->x * rad, my = cy + wh->y * rad;
        SDL_FRect mk = { mx - 4.0f, my - 4.0f, 8.0f, 8.0f };
        setCol(renderer_, 255, 255, 255);
        jplay::drawRect(renderer_, &mk);
        setCol(renderer_, 0, 0, 0);
        SDL_FRect mk2 = { mx - 3.0f, my - 3.0f, 6.0f, 6.0f };
        jplay::drawRect(renderer_, &mk2);
        gradeWheels_.push_back({ wh, wheel });
        gapTop(body, 6.0f);
        gradeSlider(body, "Luma", &wh->luma, -100, 100, 0);
        gapTop(body, 6.0f);
    }
}

// ─── Tool: RGB & Color Curves ────────────────────────────────────────────────
void App::renderGradeCurves(SDL_FRect& body) {
    // Channel tabs.
    static const char* kTabs[4] = { "Luma", "R", "G", "B" };
    static const SDL_Color kTabCol[4] = { { 210, 214, 222, 255 }, { 235, 110, 110, 255 },
                                          { 110, 220, 130, 255 }, { 110, 160, 240, 255 } };
    SDL_FRect tabs = cutTop(body, 22.0f * dpiScale);
    const float tabW = tabs.w / 4.0f;
    float mx = 0.0f, my = 0.0f;
    uiMouse(mx, my);
    for (int i = 0; i < 4; ++i) {
        SDL_FRect tr = cutLeft(tabs, tabW);
        gapRight(tr, 2.0f); // separation between tabs
        gradeCurveTabs_[i] = tr;
        const bool act = gradeCurveChannel_ == i;
        const bool hov = inRect(tr, mx, my);
        // Standard toggle-button art; the label keeps its channel colour so the
        // tab still reads as Luma/R/G/B in either state.
        drawButton(renderer_, &textFont_, tr, kTabs[i],
                   act ? (hov ? colors().uibtnOnHover : colors().uibtnOnBg)
                       : (hov ? kUiBtnBgHover : kUiBtnBg),
                   act ? colors().uibtnOnBorder
                       : (hov ? kUiBtnBorderHover : kUiBtnBorder),
                   kTabCol[i]);
    }
    gapTop(body, 8.0f);

    // Plot area (square-ish).
    const float plot = std::min(body.w, 240.0f * dpiScale);
    const SDL_FRect plotBand = cutTop(body, plot);
    const SDL_FRect rect = centerH(plotBand, plot);
    gradeCurveRect_ = rect;
    {
        const SDL_Color& cb = colors().curveBg;
        setCol(renderer_, cb.r, cb.g, cb.b);
        jplay::fillRect(renderer_, &rect);
    }

    computeGradeHistogram();
    // Histogram (translucent gray), per active channel.
    const std::array<float, 64>* h = &gradeHisto_;
    if (gradeCurveChannel_ == 1) h = &gradeHistoR_;
    else if (gradeCurveChannel_ == 2) h = &gradeHistoG_;
    else if (gradeCurveChannel_ == 3) h = &gradeHistoB_;
    SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_BLEND);
    for (int i = 0; i < 64; ++i) {
        float bh = (*h)[i] * rect.h;
        SDL_FRect bar = { rect.x + (float)i / 64.0f * rect.w, rect.y + rect.h - bh, rect.w / 64.0f + 1.0f, bh };
        setCol(renderer_, 120, 124, 132, 90);
        jplay::fillRect(renderer_, &bar);
    }
    SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_BLEND); // app-wide default; NONE here leaks into the timeline ruler's alpha fill

    // Grid + diagonal reference.
    setCol(renderer_, 44, 46, 54);
    for (int i = 1; i < 4; ++i) {
        float gx = rect.x + rect.w * i / 4.0f, gy = rect.y + rect.h * i / 4.0f;
        jplay::drawLine(renderer_, gx, rect.y, gx, rect.y + rect.h);
        jplay::drawLine(renderer_, rect.x, gy, rect.x + rect.w, gy);
    }
    setCol(renderer_, 60, 63, 72);
    jplay::drawLine(renderer_, rect.x, rect.y + rect.h, rect.x + rect.w, rect.y);
    setCol(renderer_, 80, 82, 90);
    jplay::drawRect(renderer_, &rect);

    // Active curve polyline.
    const grade::Curve* cv = &grade_.curveMaster;
    SDL_Color cc = kTabCol[gradeCurveChannel_];
    if (gradeCurveChannel_ == 1) cv = &grade_.curveR;
    else if (gradeCurveChannel_ == 2) cv = &grade_.curveG;
    else if (gradeCurveChannel_ == 3) cv = &grade_.curveB;
    setCol(renderer_, cc.r, cc.g, cc.b);
    const int SN = 64;
    float px = rect.x, py = rect.y + rect.h - cv->eval(0.0f) * rect.h;
    for (int i = 1; i <= SN; ++i) {
        float t = (float)i / SN;
        float vx = rect.x + t * rect.w;
        float vy = rect.y + rect.h - cv->eval(t) * rect.h;
        jplay::drawLine(renderer_, px, py, vx, vy);
        px = vx; py = vy;
    }
    // Control points.
    for (const auto& p : cv->pts) {
        float vx = rect.x + p.x * rect.w, vy = rect.y + rect.h - p.y * rect.h;
        SDL_FRect dot = { vx - 3.0f, vy - 3.0f, 6.0f, 6.0f };
        setCol(renderer_, 235, 238, 245);
        jplay::fillRect(renderer_, &dot);
    }
    gapTop(body, 6.0f);
    SDL_FRect hint = cutTop(body, textFont_.lineHeight());
    drawText(hint.x, hint.y, kDim, "Double-click to add a point, drag to shape");
    gapTop(body, 4.0f);
}

// ─── Histogram (display frame) ───────────────────────────────────────────────
void App::computeGradeHistogram() {
    const Clip* clip = playheadClip();
    if (!clip || clip->mediaId.empty()) return;
    CacheKey key{ clip->mediaId, clip->sourceOffset + (timeline_.playhead - clip->timelineStart) };
    if (key == gradeHistoKey_) return; // already computed for this frame
    FramePtr frame = cache_->get(key);
    if (!frame || frame->rgba.empty()) return;

    gradeHisto_.fill(0.0f);
    gradeHistoR_.fill(0.0f);
    gradeHistoG_.fill(0.0f);
    gradeHistoB_.fill(0.0f);
    const uint8_t* p = frame->rgba.data();
    size_t n = (size_t)frame->width * frame->height;
    size_t step = std::max<size_t>(1, n / 200000); // cap samples
    for (size_t i = 0; i < n; i += step) {
        const uint8_t* px = p + i * 4;
        int r = px[0], gg = px[1], b = px[2];
        int l = (r * 54 + gg * 183 + b * 19) >> 8; // ~Rec709 luma
        gradeHisto_[(l * 63) / 255] += 1.0f;
        gradeHistoR_[(r * 63) / 255] += 1.0f;
        gradeHistoG_[(gg * 63) / 255] += 1.0f;
        gradeHistoB_[(b * 63) / 255] += 1.0f;
    }
    auto norm = [](std::array<float, 64>& h) {
        float mx = 0.0f;
        for (float v : h) mx = std::max(mx, v);
        if (mx > 0.0f) for (float& v : h) v = std::sqrt(v / mx); // sqrt: lift small bins
    };
    norm(gradeHisto_); norm(gradeHistoR_); norm(gradeHistoG_); norm(gradeHistoB_);
    gradeHistoKey_ = key;
}

// ─── Event handling ──────────────────────────────────────────────────────────
bool App::gradeHandleEvent(const SDL_Event& e) {
    if (e.type == SDL_EVENT_MOUSE_BUTTON_UP) {
        gradeDragSlider_ = -1; gradeDragWheel_ = -1; gradeDragCurvePt_ = -1;
        return true;
    }
    if (e.type == SDL_EVENT_MOUSE_MOTION) {
        float mx = e.motion.x, my = e.motion.y;
        if (gradeDragSlider_ >= 0 && gradeDragSlider_ < (int)gradeSliders_.size()) {
            const GradeSlider& s = gradeSliders_[gradeDragSlider_];
            float t = std::clamp((mx - s.rect.x) / s.rect.w, 0.0f, 1.0f);
            *s.value = s.lo + t * (s.hi - s.lo);
            markGradeDirty();
            return true;
        }
        if (gradeDragWheel_ >= 0 && gradeDragWheel_ < (int)gradeWheels_.size()) {
            const GradeWheelHit& wh = gradeWheels_[gradeDragWheel_];
            float cx = wh.rect.x + wh.rect.w * 0.5f, cy = wh.rect.y + wh.rect.h * 0.5f;
            float rad = wh.rect.w * 0.5f;
            float nx = (mx - cx) / rad, ny = (my - cy) / rad;
            float len = std::sqrt(nx * nx + ny * ny);
            if (len > 1.0f) { nx /= len; ny /= len; }
            wh.wheel->x = nx; wh.wheel->y = ny;
            markGradeDirty();
            return true;
        }
        if (gradeDragCurvePt_ >= 0) {
            grade::Curve* cv = (gradeCurveChannel_ == 1) ? &grade_.curveR
                             : (gradeCurveChannel_ == 2) ? &grade_.curveG
                             : (gradeCurveChannel_ == 3) ? &grade_.curveB
                             : &grade_.curveMaster;
            const SDL_FRect& rect = gradeCurveRect_;
            int i = gradeDragCurvePt_;
            if (i >= 0 && i < (int)cv->pts.size()) {
                float nx = std::clamp((mx - rect.x) / rect.w, 0.0f, 1.0f);
                float ny = std::clamp(1.0f - (my - rect.y) / rect.h, 0.0f, 1.0f);
                if (i == 0) nx = 0.0f;
                else if (i == (int)cv->pts.size() - 1) nx = 1.0f;
                else nx = std::clamp(nx, cv->pts[i - 1].x + 0.001f, cv->pts[i + 1].x - 0.001f);
                cv->pts[i].x = nx; cv->pts[i].y = ny;
                markGradeDirty();
            }
            return true;
        }
        return false;
    }
    if (e.type != SDL_EVENT_MOUSE_BUTTON_DOWN || e.button.button != SDL_BUTTON_LEFT)
        return false;

    float mx = e.button.x, my = e.button.y;
    int clicks = e.button.clicks;

    // Header controls.
    if (inRect(gradeEnableRect_, mx, my)) { grade_.enabled = !grade_.enabled; markGradeDirty(); return true; }
    if (inRect(gradeResetRect_, mx, my)) { grade_ = grade::State{}; markGradeDirty(); return true; }

    // Tool bar.
    for (int i = 0; i < 3; ++i)
        if (inRect(gradeToolRects_[i], mx, my)) { gradeTool_ = i; gradeScroll_ = 0.0f; return true; }

    // Curve tabs + plot.
    if (gradeTool_ == 1) {
        for (int i = 0; i < 4; ++i)
            if (inRect(gradeCurveTabs_[i], mx, my)) { gradeCurveChannel_ = i; return true; }
        const SDL_FRect& rect = gradeCurveRect_;
        if (rect.w > 0.0f && inRect(rect, mx, my)) {
            grade::Curve* cv = (gradeCurveChannel_ == 1) ? &grade_.curveR
                             : (gradeCurveChannel_ == 2) ? &grade_.curveG
                             : (gradeCurveChannel_ == 3) ? &grade_.curveB
                             : &grade_.curveMaster;
            // Hit an existing point?
            int hit = -1;
            for (int i = 0; i < (int)cv->pts.size(); ++i) {
                float vx = rect.x + cv->pts[i].x * rect.w, vy = rect.y + rect.h - cv->pts[i].y * rect.h;
                if (std::fabs(mx - vx) <= 6.0f && std::fabs(my - vy) <= 6.0f) { hit = i; break; }
            }
            if (clicks >= 2) {
                if (hit > 0 && hit < (int)cv->pts.size() - 1) {
                    cv->pts.erase(cv->pts.begin() + hit); // double-click a midpoint removes it
                } else if (hit < 0) {
                    float nx = std::clamp((mx - rect.x) / rect.w, 0.0f, 1.0f);
                    float ny = std::clamp(1.0f - (my - rect.y) / rect.h, 0.0f, 1.0f);
                    gradeDragCurvePt_ = cv->addPoint(nx, ny);
                }
                markGradeDirty();
                return true;
            }
            if (hit >= 0) gradeDragCurvePt_ = hit;
            return true;
        }
    }

    // Sliders (incl. the per-wheel Luma sliders).
    for (int i = 0; i < (int)gradeSliders_.size(); ++i) {
        if (inRect(gradeSliders_[i].rect, mx, my)) {
            if (clicks >= 2) {
                *gradeSliders_[i].value = gradeSliders_[i].def;
            } else {
                gradeDragSlider_ = i;
                float t = std::clamp((mx - gradeSliders_[i].rect.x) / gradeSliders_[i].rect.w, 0.0f, 1.0f);
                *gradeSliders_[i].value = gradeSliders_[i].lo + t * (gradeSliders_[i].hi - gradeSliders_[i].lo);
            }
            markGradeDirty();
            return true;
        }
    }

    // Color wheels.
    for (int i = 0; i < (int)gradeWheels_.size(); ++i) {
        if (inRect(gradeWheels_[i].rect, mx, my)) {
            if (clicks >= 2) { gradeWheels_[i].wheel->x = 0.0f; gradeWheels_[i].wheel->y = 0.0f; }
            else {
                gradeDragWheel_ = i;
                const GradeWheelHit& wh = gradeWheels_[i];
                float cx = wh.rect.x + wh.rect.w * 0.5f, cy = wh.rect.y + wh.rect.h * 0.5f;
                float rad = wh.rect.w * 0.5f;
                float nx = (mx - cx) / rad, ny = (my - cy) / rad;
                float len = std::sqrt(nx * nx + ny * ny);
                if (len > 1.0f) { nx /= len; ny /= len; }
                wh.wheel->x = nx; wh.wheel->y = ny;
            }
            markGradeDirty();
            return true;
        }
    }
    return true; // swallow any other click inside the panel
}

// ─── Player exposure / gamma ─────────────────────────────────────────────────
// The same two values the Basic tool's Gain and Gamma sliders drive, reachable
// from the player without opening the panel: incremental keys, a hold-a-key-and-drag
// virtual slider, and a bypass for the A/B. Ranges match the sliders in
// renderGradeBasic so the panel and the player can't disagree.
namespace {
constexpr float kGainLo = -5.0f, kGainHi = 5.0f;
constexpr float kGammaLo = 0.1f, kGammaHi = 4.0f;
} // namespace

void App::exposureStatus() {
    char buf[96];
    if (grade_.gamma != 1.0f)
        std::snprintf(buf, sizeof buf, "EXPOSURE %+.2f EV   GAMMA %.2f",
                      grade_.gain, grade_.gamma);
    else
        std::snprintf(buf, sizeof buf, "EXPOSURE %+.2f EV", grade_.gain);
    setStatus(buf, 1500);
}

void App::exposureNudge(float stops) {
    grade_.gain = std::clamp(grade_.gain + stops, kGainLo, kGainHi);
    expBypassed_ = false; // the stashed gain is stale now
    markGradeDirty();
    exposureStatus();
}

// dx/dy are the total travel since the press, so the value tracks the cursor
// rather than accumulating rounding drift: dragging back to the start restores
// the value it started on. Exposure is linear in stops (100px per stop); gamma is
// multiplicative (250px per doubling) because its range isn't linear, and up is
// brighter in both axes.
void App::exposureScrub(float dx, float dy) {
    grade_.gain = std::clamp(expScrubGain0_ + dx * 0.01f, kGainLo, kGainHi);
    grade_.gamma = std::clamp(expScrubGamma0_ * std::exp2(-dy * 0.004f),
                              kGammaLo, kGammaHi);
    expBypassed_ = false;
    markGradeDirty();
    exposureStatus();
}

// Both values, not just the gain: one gesture sets the pair, so neutralizing half
// of it would leave an image that is neither the graded nor the untouched one.
void App::exposureToggleBypass() {
    if (expBypassed_) {
        grade_.gain = expBypassGain_;
        grade_.gamma = expBypassGamma_;
        expBypassed_ = false;
    } else if (grade_.gain != 0.0f || grade_.gamma != 1.0f) {
        expBypassGain_ = grade_.gain;
        expBypassGamma_ = grade_.gamma;
        grade_.gain = 0.0f;
        grade_.gamma = 1.0f;
        expBypassed_ = true;
    } else {
        exposureStatus(); // nothing to bypass; still say where we are
        return;
    }
    markGradeDirty();
    exposureStatus();
}

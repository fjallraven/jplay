// Tech-check panel: a left-expand pane with three mutually-exclusive diagnostic
// view modes (Luminance / Clipping Warning / Monochrome). Clipping and Monochrome
// are a display-referred GPU post-pass applied after the grade (see GradeGpu);
// Luminance is a scene-referred nit heatmap computed before the display transform
// (see OcioGpu::renderNitHeatmap, EXR-only). Modes persist when the panel is
// closed; Luminance and Clipping draw a legend on the stage. The R / G / B keys
// (App.cpp) reach the same slot with the three channel-isolation modes, which have
// no pill of their own.
// App members, split out of App.cpp.
//
// Pill icons (named here so the font subsetter includes their glyphs):
//   ICON_MDI_FIRE  ICON_MDI_SHIELD_ALERT  ICON_MDI_CONTRAST_CIRCLE

#include "App.h"
#include "AppInternal.h"
#include "Layout.h"

#include <algorithm>
#include <cmath>

using namespace jplay;

namespace {

constexpr SDL_Color kHeader = { 160, 170, 200, 255 };
constexpr SDL_Color kDim    = { 130, 134, 142, 255 };
constexpr SDL_Color kAmber  = { 235, 180, 90, 255 };
constexpr SDL_Color kCyan   = { 90, 215, 230, 255 };
constexpr SDL_Color kNeutral= { 205, 209, 217, 255 };

inline void setCol(SDL_Renderer* r, int rr, int gg, int bb, int aa = 255) {
    SDL_SetRenderDrawColor(r, (Uint8)rr, (Uint8)gg, (Uint8)bb, (Uint8)aa);
}

// Nit-heatmap colour ramp. These 8 control colours mirror heat() in the
// OcioGpu nit-map shader (kNitFS) so the on-stage scale matches the image.
const float kHeatRamp[8][3] = {
    { 0.25f, 0.00f, 0.35f }, // deep purple
    { 0.10f, 0.15f, 0.70f }, // deep blue
    { 0.00f, 0.55f, 0.55f }, // teal
    { 0.10f, 0.75f, 0.25f }, // emerald
    { 0.50f, 0.50f, 0.50f }, // medium grey
    { 0.90f, 0.75f, 0.10f }, // golden yellow
    { 1.00f, 0.50f, 0.00f }, // bright orange
    { 1.00f, 0.05f, 0.05f }, // neon red
};

// t in 0..1 -> heat colour (linear interpolation across kHeatRamp).
SDL_Color heatColor(float t) {
    t = std::clamp(t, 0.0f, 1.0f) * 7.0f;
    int i = (int)t;
    if (i > 6) i = 6;
    float f = t - (float)i;
    auto ch = [&](int k) {
        return (Uint8)std::lround(255.0f * (kHeatRamp[i][k] * (1.0f - f) + kHeatRamp[i + 1][k] * f));
    };
    return { ch(0), ch(1), ch(2), 255 };
}

// Log nit value -> bar position in 0..1, matching the shader (0.1..10000 nits).
inline float nitToT(float nits) {
    return (std::log10(std::max(nits, 1e-4f)) + 1.0f) / 5.0f;
}

} // namespace

void App::setTechMode(TechMode m) {
    techMode_ = (techMode_ == m) ? TechMode::None : m;
    markGradeDirty(); // force the player texture to rebuild + re-run the pass
}

// ─── Panel ───────────────────────────────────────────────────────────────────
void App::renderTechPanel() {
    if (!panelOpen(kPanelTech)) {
        if (nitRefFld_.focused()) {
            nitRefFld_.setFocus(false);
            SDL_StopTextInput(window_);
        }
        return;
    }
    float topH = titleBar_.height();
    float ex = kSidePanelW;
    SDL_FRect panel = { ex, topH, openPanelW(), panelsBottom_ - topH };
    setCol(renderer_, kPanelBg.r, kPanelBg.g, kPanelBg.b);
    jplay::fillRect(renderer_, &panel);

    // Right-edge border (1px).
    setCol(renderer_, 60, 64, 74);
    jplay::drawLine(renderer_, ex + openPanelW() - 0.5f, topH, ex + openPanelW() - 0.5f, panelsBottom_);

    const float pad = 10.0f * dpiScale;
    const float lineH = textFont_.lineHeight();
    SDL_FRect body = inset(panel, pad, 0.0f);
    gapTop(body, 8.0f);

    SDL_FRect header = cutTop(body, lineH);
    drawText(header.x, header.y, kHeader, "TECH CHECK");
    gapTop(body, 10.0f);

    struct Pill { const char* name; const char* desc; uint32_t icon; SDL_Color col; TechMode mode; };
    const Pill pills[3] = {
        { "Luminance (nits)", "HDR nit heatmap \xc2\xb7 EXR", 0xF0238, kAmber, TechMode::Luminance }, // fire
        { "Clipping Warning", "Crushed / blown pixels", 0xF0ECC, kCyan, TechMode::Clipping }, // shield-alert
        { "Monochrome", "Luminance checker", 0xF0197, kNeutral, TechMode::Monochrome },   // contrast-circle
    };
    const float pillH = 48.0f * dpiScale;
    const float iconW = 30.0f * dpiScale;
    for (int i = 0; i < 3; ++i) {
        SDL_FRect pr = cutTop(body, pillH);
        gapTop(body, 8.0f);
        techPillRects_[i] = pr;
        bool act = techMode_ == pills[i].mode;
        setCol(renderer_, act ? 52 : 42, act ? 56 : 44, act ? 66 : 50);
        jplay::fillRect(renderer_, &pr);
        setCol(renderer_, act ? pills[i].col.r : 80, act ? pills[i].col.g : 82,
               act ? pills[i].col.b : 90);
        jplay::drawRect(renderer_, &pr);
        if (act) { // accent stripe at the left edge
            SDL_FRect bar = { pr.x, pr.y, 3.0f, pr.h };
            setCol(renderer_, pills[i].col.r, pills[i].col.g, pills[i].col.b);
            jplay::fillRect(renderer_, &bar);
        }
        // Square icon well on the left, the two text lines in what is left over.
        // The well is sized off the text, not off pillH: a pillH-wide well eats a
        // third of the panel and pushes the description past the right edge.
        SDL_FRect inner = pr;
        gapLeft(inner, 6.0f);
        SDL_FRect well = cutLeft(inner, iconW);
        SDL_FRect ir = centerV(well, iconW);
        icons_.drawGlyph(renderer_, pills[i].icon, ir,
                         act ? pills[i].col : SDL_Color{ 175, 180, 190, 255 }, 0.12f);
        gapLeft(inner, 8.0f);
        gapTop(inner, 8.0f);
        SDL_FRect name = cutTop(inner, lineH);
        drawText(name.x, name.y, act ? pills[i].col : kNeutral, fitText(pills[i].name, name.w));
        gapTop(inner, 2.0f);
        SDL_FRect desc = cutTop(inner, lineH);
        drawText(desc.x, desc.y, kDim, fitText(pills[i].desc, desc.w));
    }

    gapTop(body, 4.0f);
    for (const std::string& ln : wrapText("Applied after the grade, on the final image.", body.w)) {
        SDL_FRect note = cutTop(body, lineH);
        drawText(note.x, note.y, kDim, ln);
    }
    gapTop(body, 20.0f);

    // HDR reference white (nits): scene-linear 1.0 maps to this many nits, which
    // sets the scale of the Luminance heatmap above.
    SDL_FRect nitHead = cutTop(body, lineH);
    drawText(nitHead.x, nitHead.y, kHeader, "HDR REF WHITE");
    gapTop(body, 8.0f);
    {
        const char* nitLabel = "1.0 =";
        SDL_FRect row = cutTop(body, 20.0f);
        SDL_FRect label = cutLeft(row, textFont_.measure(renderer_, nitLabel));
        gapLeft(row, 8.0f);
        nitRefFldBox_ = row; // the field takes the rest of the row
        nitRefFld_.setRect(nitRefFldBox_);
        if (!nitRefFld_.focused()) {
            char buf[32];
            SDL_snprintf(buf, sizeof(buf), "%g", nitRef_);
            nitRefFld_.setText(buf);
        }
        const SDL_FRect lt = centerV(label, lineH);
        drawText(lt.x, lt.y, kNeutral, nitLabel);
        nitRefFld_.render(renderer_, &textFont_);
    }
}

// ─── Event handling ──────────────────────────────────────────────────────────
bool App::techHandleEvent(const SDL_Event& e) {
    if (e.type != SDL_EVENT_MOUSE_BUTTON_DOWN || e.button.button != SDL_BUTTON_LEFT)
        return false;
    float mx = e.button.x, my = e.button.y;
    static const TechMode modes[3] = { TechMode::Luminance, TechMode::Clipping, TechMode::Monochrome };
    for (int i = 0; i < 3; ++i)
        if (inRect(techPillRects_[i], mx, my)) { setTechMode(modes[i]); return true; }
    return true; // swallow any other click inside the panel
}

// ─── HDR nit-reference editable field ───────────────────────────────────────
// Plain text field. On commit, parse nits, clamp to [1, 10000], persist, and
// mark the grade/tech texture dirty so the live Luminance heatmap re-renders.
bool App::techNitHandleEvent(const SDL_Event& e) {
    auto commit = [this]() {
        double v = SDL_atof(nitRefFld_.text().c_str());
        nitRef_ = (float)std::clamp(v, 1.0, 10000.0);
        nitRefFld_.setFocus(false);
        SDL_StopTextInput(window_);
        markGradeDirty();
        writePrefs();
    };
    switch (e.type) {
    case SDL_EVENT_MOUSE_BUTTON_DOWN: {
        if (e.button.button != SDL_BUTTON_LEFT)
            return false;
        float mx = e.button.x, my = e.button.y;
        if (inRect(nitRefFldBox_, mx, my)) {
            if (!nitRefFld_.focused()) {
                nitRefFld_.setFocus(true);
                SDL_StartTextInput(window_);
            }
            return true;
        }
        if (nitRefFld_.focused()) {
            commit(); // clicking away commits the typed value
            return false; // let the click also hit whatever it landed on
        }
        return false;
    }
    case SDL_EVENT_TEXT_INPUT:
        if (nitRefFld_.focused()) { nitRefFld_.handleEvent(e); return true; }
        return false;
    case SDL_EVENT_KEY_DOWN:
        if (nitRefFld_.focused()) {
            if (e.key.key == SDLK_RETURN || e.key.key == SDLK_KP_ENTER) {
                commit();
                return true;
            }
            if (e.key.key == SDLK_ESCAPE) {
                nitRefFld_.setFocus(false);
                SDL_StopTextInput(window_);
                return true;
            }
            nitRefFld_.handleEvent(e);
            return true;
        }
        return false;
    default:
        return false;
    }
}

// ─── On-stage legend ─────────────────────────────────────────────────────────
void App::renderTechOverlay() {
    if (techMode_ == TechMode::None || !timeline_.hasClips())
        return;
    const SDL_FRect& pr = playerRect_;
    if (pr.w < 80.0f || pr.h < 80.0f)
        return;

    SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_BLEND);

    if (techMode_ == TechMode::Luminance) {
        float barW = std::min(pr.w - 40.0f, 680.0f);
        float barH = 22.0f;
        float bx = pr.x + (pr.w - barW) * 0.5f;
        float by = pr.y + pr.h - barH - 30.0f;
        // backing panel (room for the title above and tick labels below)
        SDL_FRect bg = { bx - 8.0f, by - 18.0f, barW + 16.0f,
                         barH + 26.0f + textFont_.lineHeight() };
        setCol(renderer_, 12, 13, 16, 210);
        jplay::fillRect(renderer_, &bg);

        if (!nitHeatmapShown_) {
            // Luminance is scene-referred; a video source carries no HDR data.
            drawText(bx, by + (barH - textFont_.lineHeight()) * 0.5f, kAmber,
                     "LUMINANCE  \xc2\xb7  no HDR data on this source");
        } else {
            char title[64];
            SDL_snprintf(title, sizeof(title), "LUMINANCE  \xc2\xb7  nits  (1.0 = %g)", nitRef_);
            drawText(bx, by - 16.0f, kAmber, title);
            // Continuous log gradient: each column maps directly to the shader's t.
            int cols = (int)barW;
            for (int i = 0; i < cols; ++i) {
                SDL_Color hc = heatColor((float)i / barW);
                setCol(renderer_, hc.r, hc.g, hc.b);
                SDL_FRect strip = { bx + (float)i, by, 1.0f, barH };
                jplay::fillRect(renderer_, &strip);
            }
            // Log tick labels.
            struct Tick { float nits; const char* label; };
            const Tick ticks[6] = { { 0.1f, "0.1" }, { 1.0f, "1" }, { 10.0f, "10" },
                                    { 100.0f, "100" }, { 1000.0f, "1k" }, { 10000.0f, "10k" } };
            for (const Tick& t : ticks) {
                float tx = bx + nitToT(t.nits) * barW;
                setCol(renderer_, 235, 238, 245, 200);
                jplay::drawLine(renderer_, tx, by, tx, by + barH);
                float lw = textFont_.measure(renderer_, t.label);
                float lx = std::clamp(tx - lw * 0.5f, bx, bx + barW - lw);
                drawText(lx, by + barH + 1.0f, kNeutral, t.label);
            }
        }
    } else if (techMode_ == TechMode::Clipping) {
        // Pulsing dual-pill legend.
        float pulse = 0.5f + 0.5f * std::sin((float)SDL_GetTicks() * 0.006f);
        Uint8 a = (Uint8)(120 + 120 * pulse);
        struct LP { const char* txt; Uint8 r, g, b; };
        LP lps[2] = { { "CRUSHED  (shadows \xe2\x89\xa4 1)", 0, 230, 235 },
                      { "CLIPPED  (highlights \xe2\x89\xa5 254)", 255, 30, 40 } };
        float pillH = 24.0f, gap = 12.0f;
        float wA = textFont_.measure(renderer_, lps[0].txt) + 34.0f;
        float wB = textFont_.measure(renderer_, lps[1].txt) + 34.0f;
        float totalW = wA + wB + gap;
        float px = pr.x + (pr.w - totalW) * 0.5f;
        float py = pr.y + pr.h - pillH - 30.0f;
        float widths[2] = { wA, wB };
        for (int i = 0; i < 2; ++i) {
            SDL_FRect pill = { px, py, widths[i], pillH };
            setCol(renderer_, 12, 13, 16, 220);
            jplay::fillRect(renderer_, &pill);
            setCol(renderer_, lps[i].r, lps[i].g, lps[i].b, a);
            jplay::drawRect(renderer_, &pill);
            SDL_FRect dot = { px + 8.0f, py + (pillH - 10.0f) * 0.5f, 10.0f, 10.0f };
            setCol(renderer_, lps[i].r, lps[i].g, lps[i].b, a);
            jplay::fillRect(renderer_, &dot);
            drawText(px + 24.0f, py + (pillH - textFont_.lineHeight()) * 0.5f,
                     SDL_Color{ 235, 238, 245, 255 }, lps[i].txt);
            px += widths[i] + gap;
        }
    }

    SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_BLEND); // app-wide default; NONE here leaks into the timeline ruler's alpha fill
}

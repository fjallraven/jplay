#include "Splash.h"

#include <cmath>

namespace {

// fract(sin(dot(co, (12.9898, 78.233))) * 43758.5453) -- the classic GLSL hash,
// ported verbatim from the prototype shader.
inline float noise(float x, float y) {
    float v = std::sin(x * 12.9898f + y * 78.233f) * 43758.5453f;
    return v - std::floorf(v);
}

inline float clampf(float v, float lo, float hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

} // namespace

Splash::~Splash() {
    if (tex_) SDL_DestroyTexture(tex_);
}

// Rasterise the static logo (dark grid + bold "jplay" + play arrow + shortcut
// hint) into base_ using the SDL renderer + an offscreen target, then read it
// back to CPU. Done once; the per-frame shader pass only samples base_.
bool Splash::buildBase(SDL_Renderer* r) {
    SDL_Texture* target = SDL_CreateTexture(r, SDL_PIXELFORMAT_RGBA32,
                                            SDL_TEXTUREACCESS_TARGET, kW, kH);
    if (!target) return false;

    SDL_Texture* prevTarget = SDL_GetRenderTarget(r);
    SDL_SetRenderTarget(r, target);
    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);

    // Dark grid background (bg #06130e, lines #112b20).
    SDL_SetRenderDrawColor(r, 6, 19, 14, 255);
    SDL_RenderClear(r);
    SDL_SetRenderDrawColor(r, 17, 43, 32, 255);
    for (int x = 0; x < kW; x += 32)
        SDL_RenderLine(r, (float)x, 0.0f, (float)x, (float)kH);
    for (int y = 0; y < kH; y += 32)
        SDL_RenderLine(r, 0.0f, (float)y, (float)kW, (float)y);

    // Play arrow (yellow #ffdd00), to the right of the wordmark.
    const float ax = 470.0f, ay = 150.0f, ah = 60.0f, aw = 52.0f;
    SDL_FColor yellow{ 1.0f, 0.866f, 0.0f, 1.0f };
    SDL_Vertex tri[3] = {
        { { ax, ay }, yellow, { 0, 0 } },
        { { ax, ay + ah }, yellow, { 0, 0 } },
        { { ax + aw, ay + ah * 0.5f }, yellow, { 0, 0 } },
    };
    SDL_RenderGeometry(r, nullptr, tri, 3, nullptr, 0);

    // "jplay" wordmark: SDL's 8x8 debug font scaled up. Blocky, which suits the
    // CRT look. A 1px-ish dark offset fakes a stroke/shadow.
    const float ts = 8.0f;            // scale factor (char -> 64px)
    const float lx = 120.0f / ts;     // device x 120 in scaled coords
    const float ly = 150.0f / ts;     // device y 150 in scaled coords
    SDL_SetRenderScale(r, ts, ts);
    SDL_SetRenderDrawColor(r, 0, 0, 0, 255);
    SDL_RenderDebugText(r, lx + 0.18f, ly + 0.18f, "jplay");
    SDL_SetRenderDrawColor(r, 255, 119, 0, 255); // orange #ff7700
    SDL_RenderDebugText(r, lx, ly, "jplay");
    SDL_SetRenderScale(r, 1.0f, 1.0f);

    // Shortcut hint, baked in so it gets the same VHS treatment.
    //const char* hint = "DROP VIDEO OR EXR FILES HERE  -  CTRL+O LOAD  -  CTRL+S SAVE";
    //float hintW = (float)SDL_strlen(hint) * 8.0f;
    //SDL_SetRenderDrawColor(r, 90, 120, 105, 255);
    //SDL_RenderDebugText(r, (kW - hintW) * 0.5f, kH - 40.0f, hint);

    // Read the rasterised logo back into base_ (RGBA32).
    bool ok = false;
    if (SDL_Surface* surf = SDL_RenderReadPixels(r, nullptr)) {
        SDL_Surface* rgba = surf->format == SDL_PIXELFORMAT_RGBA32
                                ? surf
                                : SDL_ConvertSurface(surf, SDL_PIXELFORMAT_RGBA32);
        if (rgba) {
            base_.resize((size_t)kW * kH * 4);
            const uint8_t* src = (const uint8_t*)rgba->pixels;
            for (int y = 0; y < kH; ++y)
                SDL_memcpy(&base_[(size_t)y * kW * 4], src + (size_t)y * rgba->pitch,
                           (size_t)kW * 4);
            if (rgba != surf) SDL_DestroySurface(rgba);
            ok = true;
        }
        SDL_DestroySurface(surf);
    }

    SDL_SetRenderTarget(r, prevTarget);
    SDL_DestroyTexture(target);
    if (!ok) return false;

    tex_ = SDL_CreateTexture(r, SDL_PIXELFORMAT_RGBA32,
                             SDL_TEXTUREACCESS_STREAMING, kW, kH);
    if (!tex_) return false;
    SDL_SetTextureScaleMode(tex_, SDL_SCALEMODE_LINEAR);
    out_.resize((size_t)kW * kH * 4);
    return true;
}

void Splash::render(SDL_Renderer* r, const SDL_FRect& dst, double timeSeconds) {
    if (!ready_) {
        if (failed_) return;
        if (!buildBase(r)) { failed_ = true; return; }
        ready_ = true;
    }

    const float t = (float)timeSeconds;
    // Time-only terms hoisted out of the pixel loop.
    const float rOffset = 0.007f * std::sin(t * 0.5f);  // red channel split
    const float bOffset = -0.005f * std::cos(t * 0.8f); // blue channel split

    auto sampleChan = [&](float u, float v, int ch) -> float {
        u = clampf(u, 0.0f, 0.999999f);
        v = clampf(v, 0.0f, 0.999999f);
        int ix = (int)(u * kW);
        int iy = (int)(v * kH);
        return base_[((size_t)iy * kW + ix) * 4 + ch] * (1.0f / 255.0f);
    };

    for (int y = 0; y < kH; ++y) {
        float uvy = (y + 0.5f) / kH;
        // VHS tracking / jitter glitch: occasional horizontal bands shift.
        float glitch = std::sin(t * 1.5f + uvy * 3.0f) >= 0.92f ? 1.0f : 0.0f;
        float jitter = noise(t, uvy) * 2.0f - 1.0f;
        float scanline = std::sin(uvy * kH * 1.8f) * 0.12f;
        // Per-row horizontal shift: tracking jitter + subtle tape warp.
        float shift = jitter * 0.015f * glitch + std::sin(uvy * 15.0f + t * 3.0f) * 0.002f;
        uint8_t* row = &out_[(size_t)y * kW * 4];

        for (int x = 0; x < kW; ++x) {
            float uvx = (x + 0.5f) / kW + shift;

            // Chromatic aberration: split RGB sample positions.
            float cr = sampleChan(uvx + rOffset, uvy, 0);
            float cg = sampleChan(uvx, uvy, 1);
            float cb = sampleChan(uvx + bOffset, uvy, 2);

            // TV static / analog noise.
            float s = noise(uvx + t, uvy + t);
            cr = cr * 0.86f + s * 0.14f;
            cg = cg * 0.86f + s * 0.14f;
            cb = cb * 0.86f + s * 0.14f;

            // Scanlines.
            cr -= scanline; cg -= scanline; cb -= scanline;

            // CRT vignette (dark edges).
            float dx = std::fabs(uvx - 0.5f) * 1.1f;
            float dy = std::fabs(uvy - 0.5f) * 1.1f;
            float vig = clampf(1.0f - (dx * dx + dy * dy), 0.2f, 1.0f);
            cr *= vig; cg *= vig; cb *= vig;

            row[x * 4 + 0] = (uint8_t)(clampf(cr, 0.0f, 1.0f) * 255.0f);
            row[x * 4 + 1] = (uint8_t)(clampf(cg, 0.0f, 1.0f) * 255.0f);
            row[x * 4 + 2] = (uint8_t)(clampf(cb, 0.0f, 1.0f) * 255.0f);
            row[x * 4 + 3] = 255;
        }
    }

    SDL_UpdateTexture(tex_, nullptr, out_.data(), kW * 4);
    SDL_RenderTexture(r, tex_, nullptr, &dst);
}

#pragma once

#include <SDL3/SDL.h>

#include <cstdint>
#include <vector>

// Animated "jplay" splash shown in the player area when the timeline is empty.
//
// The original look was prototyped as a WebGL fragment shader (VHS tracking
// jitter, chromatic aberration, TV static, scanlines, CRT vignette). jplay
// renders through SDL's 2D renderer (no GL context), so the effect is ported to
// a per-pixel CPU pass: the static "jplay" logo + grid is rasterised once into a
// CPU buffer, then the shader math runs over it each frame into a streaming
// texture that SDL scales up into the player rect.
class Splash {
public:
    ~Splash();
    // Draw the animated splash filling dst. timeSeconds drives the animation.
    // Builds its resources lazily on first call (needs a live renderer).
    void render(SDL_Renderer* r, const SDL_FRect& dst, double timeSeconds);

private:
    // Internal effect resolution. Low-res on purpose: it suits the lo-fi VHS
    // aesthetic and keeps the per-frame CPU pass cheap; SDL upscales (linear)
    // into the player rect.
    static constexpr int kW = 640;
    static constexpr int kH = 360;

    bool buildBase(SDL_Renderer* r); // rasterise logo+grid -> base_, create out texture

    bool ready_ = false;
    bool failed_ = false;             // base build failed; don't retry forever
    SDL_Texture* tex_ = nullptr;      // streaming RGBA target uploaded each frame
    std::vector<uint8_t> base_;       // kW*kH*4, the un-distorted logo+grid (RGBA)
    std::vector<uint8_t> out_;        // kW*kH*4, scratch for the per-frame pass
};

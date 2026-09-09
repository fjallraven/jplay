#pragma once

#include <SDL3/SDL.h>

#include <string>
#include <unordered_map>

// Opaque; the implementation includes <SDL3_ttf/SDL_ttf.h>.
typedef struct TTF_Font TTF_Font;

// UI text font loaded from a .ttf file on disk. Renders each unique string to a
// white-on-transparent texture on first draw and caches it; color is applied
// per-draw via SDL_SetTextureColorMod. If loading fails the object stays inert
// and draw() is a no-op. draw() and measure() must be called on the render
// thread (they may create textures).
//
// All positions and metrics this class takes and returns are in *logical* units.
// Glyphs are rasterized at `density` times that size so they land on the device
// pixel grid 1:1 under the renderer's logical presentation — sharp on a high-DPI
// display without the layout above it changing at all.
class TextFont {
public:
    ~TextFont();

    // TTF_Init + open the font sized for `ptsize` logical points, rasterized at
    // `density` device pixels per logical unit. Returns false (and logs) on
    // failure. charWidth() and lineHeight() are set from font metrics on success.
    bool load(SDL_Renderer* renderer, const char* path, float ptsize,
              float density = 1.0f);

    // Release all cached textures + close the font + TTF_Quit. Idempotent; must
    // run while the renderer is still alive. The destructor also calls this.
    void destroy();

    // Draw text at logical position (x, y) — top-left corner — with color c.
    // Creates and caches a texture on first call for each unique string. scale
    // stretches the cached glyph texture (linear-filtered) for emphasis.
    void draw(SDL_Renderer* r, float x, float y, SDL_Color c,
              const char* text, float scale = 1.0f) const;

    // Width of text as rendered, in logical units. Creates and caches a texture on
    // first call.
    float measure(SDL_Renderer* r, const char* text) const;

    // Average advance width of a single character (measured from "M" on load;
    // usable without a renderer for layout estimates).
    float charWidth() const { return charW_; }

    // Recommended line height in logical units (from TTF_GetFontHeight on load).
    float lineHeight() const { return lineH_; }

    // Baseline offset from the top of a drawn line, and the height of a capital
    // above that baseline, both in logical units. Together they place the band the
    // letters actually occupy, which is what a marker drawn beside a label wants to
    // centre on rather than the taller line box (see MenuBar::drawCheck).
    float ascent() const { return ascent_; }
    float capHeight() const { return capH_; }

private:
    struct Entry {
        SDL_Texture* tex = nullptr;
        int w = 0, h = 0;
    };
    const Entry* entry(SDL_Renderer* r, const char* text) const;

    bool ttfInited_ = false;
    TTF_Font* font_ = nullptr;
    mutable std::unordered_map<std::string, Entry> cache_;
    float density_ = 1.0f; // device pixels per logical unit the glyphs are rasterized at
    float lineH_ = 8.0f;
    float ascent_ = 8.0f;
    float capH_ = 8.0f;
    float charW_ = 8.0f;
};

#pragma once

#include <SDL3/SDL.h>

#include <cstdint>
#include <unordered_map>

// Opaque; the implementation includes <SDL3_ttf/SDL_ttf.h>.
typedef struct TTF_Font TTF_Font;

// Embedded Material Design Icons webfont. Loaded once at a generous point size;
// each glyph we use is rendered to a white texture on first draw and cached, then
// tinted per-draw via SDL_SetTextureColorMod. If loading fails the object stays
// inert and draws nothing. drawGlyph must be called on the render thread (it may
// create a texture).
class IconFont {
public:
    ~IconFont();

    // TTF_Init + open the embedded font. Returns false (and logs) on failure,
    // leaving the object inert. Glyph textures are built lazily on first draw.
    bool load(SDL_Renderer* renderer);

    // Release all glyph textures + font and TTF_Quit. Idempotent; must run while
    // the renderer is still alive (it destroys textures). The destructor also
    // calls this as a safety net.
    void destroy();

    // Draw the MDI glyph `codepoint` centered inside r, tinted with c. padFrac is
    // the inset on each side as a fraction of r's width. No-op if the font failed
    // to load or the glyph can't be rendered.
    void drawGlyph(SDL_Renderer* renderer, uint32_t codepoint, const SDL_FRect& r,
                   SDL_Color c, float padFrac = 0.18f) const;

private:
    struct Glyph {
        SDL_Texture* tex = nullptr;
        int w = 0, h = 0;
        bool tried = false; // a render was attempted (tex may still be null on failure)
    };
    // Return the cached glyph for `cp`, rendering+caching it on first request.
    const Glyph* glyph(SDL_Renderer* renderer, uint32_t cp) const;

    bool ttfInited_ = false;
    TTF_Font* font_ = nullptr;
    mutable std::unordered_map<uint32_t, Glyph> glyphs_;
};

#pragma once

#include <SDL3/SDL.h>

#include "IconsMaterialDesignIcons.h"

#include <cstdint>
#include <unordered_map>

// Opaque; the implementation includes <SDL3_ttf/SDL_ttf.h>.
typedef struct TTF_Font TTF_Font;

// Compile-time UTF-8 -> codepoint, so an ICON_MDI_* macro can be handed
// straight to drawGlyph (which takes a codepoint, while the macros in
// IconsMaterialDesignIcons.h are UTF-8 string literals):
//
//     icons_.drawGlyph(renderer, iconCp(ICON_MDI_BRUSH), rect, col);
//
// Prefer this over a bare 0xF00E3. Two reasons, both about the embedded font:
// tools/gen_icon_font.py subsets the webfont down to the glyphs the source
// references and finds them by scanning for ICON_MDI_* identifiers, so naming
// the macro is what keeps the glyph in the build; and a mistyped macro is a
// compile error, where a mistyped codepoint is a button that silently draws
// nothing. (The tool also recognises raw 0xFxxxx literals as a safety net --
// see its docstring -- but nothing checks those for you.)
constexpr uint32_t iconCp(const char* utf8) {
    const unsigned char c0 = (unsigned char)utf8[0];
    if (c0 < 0x80)
        return c0;
    const uint32_t c1 = (uint32_t)((unsigned char)utf8[1] & 0x3F);
    if ((c0 & 0xE0) == 0xC0)
        return ((uint32_t)(c0 & 0x1F) << 6) | c1;
    const uint32_t c2 = (uint32_t)((unsigned char)utf8[2] & 0x3F);
    if ((c0 & 0xF0) == 0xE0)
        return ((uint32_t)(c0 & 0x0F) << 12) | (c1 << 6) | c2;
    const uint32_t c3 = (uint32_t)((unsigned char)utf8[3] & 0x3F);
    return ((uint32_t)(c0 & 0x07) << 18) | (c1 << 12) | (c2 << 6) | c3;
}

// The MDI glyphs live in supplementary private-use plane A, so every macro that
// matters here takes the 4-byte branch; the shorter ones are covered for the
// sake of being a correct UTF-8 decoder rather than because MDI needs them.
static_assert(iconCp(ICON_MDI_FILE_MULTIPLE) == 0xF0222, "iconCp: 4-byte decode");
static_assert(iconCp(ICON_MDI_COG) == 0xF0493, "iconCp: 4-byte decode");
static_assert(iconCp("A") == 0x41, "iconCp: ASCII decode");

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

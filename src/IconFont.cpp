#include "IconFont.h"

#include "icon_font_data.h"

#include <SDL3_ttf/SDL_ttf.h>

#include <algorithm>

IconFont::~IconFont() {
    destroy();
}

// Initialise SDL_ttf and open the embedded Material Design Icons webfont. The
// font is loaded once at a generous point size; glyph textures are scaled down
// at draw time so the same texture stays crisp at any button size. Failure just
// leaves the object inert and drawGlyph draws nothing.
bool IconFont::load(SDL_Renderer* /*renderer*/) {
    if (!TTF_Init()) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "TTF_Init failed: %s", SDL_GetError());
        return false;
    }
    ttfInited_ = true;

    // TTF_OpenFontIO with closeio=true hands ownership of the SDL_IOStream to the
    // font; the const-mem stream just points at our static array (never freed).
    SDL_IOStream* io = SDL_IOFromConstMem(g_mdi_font_ttf, (size_t)g_mdi_font_ttf_len);
    if (!io) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "SDL_IOFromConstMem failed: %s", SDL_GetError());
        return false;
    }
    font_ = TTF_OpenFontIO(io, true, 64.0f);
    if (!font_) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "TTF_OpenFontIO failed: %s", SDL_GetError());
        return false;
    }
    return true;
}

void IconFont::destroy() {
    for (auto& kv : glyphs_)
        if (kv.second.tex)
            SDL_DestroyTexture(kv.second.tex);
    glyphs_.clear();
    if (font_) {
        TTF_CloseFont(font_);
        font_ = nullptr;
    }
    if (ttfInited_) {
        TTF_Quit();
        ttfInited_ = false;
    }
}

const IconFont::Glyph* IconFont::glyph(SDL_Renderer* renderer, uint32_t cp) const {
    auto it = glyphs_.find(cp);
    if (it != glyphs_.end())
        return &it->second;

    Glyph g;
    g.tried = true;
    if (font_ && renderer) {
        // Lowest-level glyph path: returns the glyph's own bitmap white-on-clear,
        // bypassing the text shaper entirely. This SDL_ttf build has no HarfBuzz,
        // and its fallback shaper reports "zero width" for plane-15 PUA codepoints
        // (which all MDI glyphs are), so the UTF-8 string path can't be used here.
        TTF_ImageType itype = TTF_IMAGE_INVALID;
        SDL_Surface* s = TTF_GetGlyphImage(font_, cp, &itype);
        if (!s)
            s = TTF_RenderGlyph_Blended(font_, cp, SDL_Color{ 255, 255, 255, 255 });
        if (s) {
            g.tex = SDL_CreateTextureFromSurface(renderer, s);
            g.w = s->w;
            g.h = s->h;
            SDL_DestroySurface(s);
            if (g.tex)
                SDL_SetTextureScaleMode(g.tex, SDL_SCALEMODE_LINEAR);
        } else {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "glyph %u render failed: %s", cp,
                        SDL_GetError());
        }
    }
    auto res = glyphs_.emplace(cp, g);
    return &res.first->second;
}

void IconFont::drawGlyph(SDL_Renderer* renderer, uint32_t codepoint, const SDL_FRect& r,
                         SDL_Color c, float padFrac) const {
    const Glyph* g = glyph(renderer, codepoint);
    if (!g || !g->tex || g->w <= 0 || g->h <= 0)
        return;
    float pad = r.w * padFrac;
    float availW = r.w - 2.0f * pad, availH = r.h - 2.0f * pad;
    float scale = std::min(availW / g->w, availH / g->h);
    float dw = g->w * scale, dh = g->h * scale;
    SDL_FRect dst = { r.x + (r.w - dw) * 0.5f, r.y + (r.h - dh) * 0.5f, dw, dh };
    SDL_SetTextureColorMod(g->tex, c.r, c.g, c.b);
    SDL_SetTextureAlphaMod(g->tex, c.a);
    SDL_RenderTexture(renderer, g->tex, nullptr, &dst);
}

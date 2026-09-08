#include "TextFont.h"

#include <SDL3_ttf/SDL_ttf.h>

#include <cmath>

TextFont::~TextFont() {
    destroy();
}

bool TextFont::load(SDL_Renderer* /*renderer*/, const char* path, float ptsize,
                    float density) {
    if (!TTF_Init()) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "TTF_Init failed: %s", SDL_GetError());
        return false;
    }
    ttfInited_ = true;

    density_ = density > 0.0f ? density : 1.0f;
    font_ = TTF_OpenFont(path, ptsize * density_);
    if (!font_) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "TextFont: TTF_OpenFont(%s) failed: %s", path, SDL_GetError());
        return false;
    }

    // Metrics come back in the rasterized (device) size; the callers lay out in
    // logical units, so scale them back down. Hinting means these are not exactly
    // the 1x metrics times density, but the error stays under one device pixel
    // instead of compounding through a layout scaled by hand.
    lineH_ = (float)TTF_GetFontHeight(font_) / density_;
    ascent_ = (float)TTF_GetFontAscent(font_) / density_;
    // Cap height straight off the 'M' outline: the ascent includes accent and
    // line-gap room no plain label uses, so it is the wrong band to centre against.
    int capMaxY = 0;
    if (TTF_GetGlyphMetrics(font_, 'M', nullptr, nullptr, nullptr, &capMaxY, nullptr))
        capH_ = (float)capMaxY / density_;
    else
        capH_ = ascent_;

    // Measure "M" to get a representative single-character advance width for
    // layout estimates (used where a renderer isn't available, e.g. MenuBar::layout).
    SDL_Surface* s = TTF_RenderText_Blended(font_, "M", 0, SDL_Color{255, 255, 255, 255});
    if (s) {
        charW_ = (float)s->w / density_;
        SDL_DestroySurface(s);
    } else {
        charW_ = lineH_ * 0.65f; // safe fallback
    }
    return true;
}

void TextFont::destroy() {
    for (auto& kv : cache_)
        if (kv.second.tex)
            SDL_DestroyTexture(kv.second.tex);
    cache_.clear();
    if (font_) {
        TTF_CloseFont(font_);
        font_ = nullptr;
    }
    if (ttfInited_) {
        TTF_Quit();
        ttfInited_ = false;
    }
}

const TextFont::Entry* TextFont::entry(SDL_Renderer* r, const char* text) const {
    auto it = cache_.find(text);
    if (it != cache_.end())
        return &it->second;

    Entry e;
    if (font_ && r && text[0] != '\0') {
        SDL_Surface* s = TTF_RenderText_Blended(font_, text, 0,
                                                SDL_Color{255, 255, 255, 255});
        if (s) {
            e.tex = SDL_CreateTextureFromSurface(r, s);
            e.w   = s->w;
            e.h   = s->h;
            SDL_DestroySurface(s);
            if (e.tex)
                SDL_SetTextureScaleMode(e.tex, SDL_SCALEMODE_LINEAR);
        } else {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "TextFont: render \"%s\" failed: %s", text, SDL_GetError());
        }
    }
    return &cache_.emplace(std::string(text), e).first->second;
}

void TextFont::draw(SDL_Renderer* r, float x, float y, SDL_Color c,
                    const char* text, float scale) const {
    const Entry* e = entry(r, text);
    if (!e || !e->tex || e->w <= 0)
        return;
    // The glyph texture is rasterized at density_ device pixels per logical unit and
    // sampled with a linear filter, so it only stays crisp when it maps 1:1 onto the
    // pixel grid. Drawing it at 1/density_ of its texel size is exactly that mapping
    // once the renderer scales logical -> device. Snap the origin to whole device
    // pixels to keep it there; a fractional x/y (from centering terms that grow with
    // e.g. the media-bin size slider) otherwise makes the filter blend across texels
    // and the text goes soft.
    const float toLogical = 1.0f / density_;
    SDL_FRect dst = { std::round(x * density_) * toLogical,
                      std::round(y * density_) * toLogical,
                      e->w * scale * toLogical, e->h * scale * toLogical };
    SDL_SetTextureColorMod(e->tex, c.r, c.g, c.b);
    SDL_SetTextureAlphaMod(e->tex, c.a);
    SDL_RenderTexture(r, e->tex, nullptr, &dst);
}

float TextFont::measure(SDL_Renderer* r, const char* text) const {
    const Entry* e = entry(r, text);
    return e ? (float)e->w / density_ : 0.0f;
}

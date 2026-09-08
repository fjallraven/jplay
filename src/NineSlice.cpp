#include "NineSlice.h"

#include "PixelSnap.h"

#include <algorithm>
#include <cmath>

namespace {

// A border guide pixel "marks" a span when it is opaque and near-black.
inline bool isMark(Uint8 r, Uint8 g, Uint8 b, Uint8 a) {
    return a > 127 && r < 64 && g < 64 && b < 64;
}

// Read an ARGB8888 pixel. `surf` must be ARGB8888 and locked if needed.
inline Uint32 getPx(const SDL_Surface* surf, int x, int y) {
    const Uint8* row = (const Uint8*)surf->pixels + (size_t)y * surf->pitch;
    return ((const Uint32*)row)[x];
}

inline void splitARGB(Uint32 p, Uint8& r, Uint8& g, Uint8& b, Uint8& a) {
    a = (p >> 24) & 0xff;
    r = (p >> 16) & 0xff;
    g = (p >> 8) & 0xff;
    b = p & 0xff;
}

// Scan one edge of the border for the contiguous run of mark pixels and return
// it as [first,last] in content coordinates (0..len-1). Returns false when the
// edge carries no marks. `read(i)` fetches the border pixel for content index i.
template <typename ReadFn>
bool scanRun(int len, ReadFn read, int& first, int& last) {
    first = -1;
    last = -1;
    for (int i = 0; i < len; ++i) {
        Uint8 r, g, b, a;
        splitARGB(read(i), r, g, b, a);
        if (isMark(r, g, b, a)) {
            if (first < 0) first = i;
            last = i;
        }
    }
    return first >= 0;
}

} // namespace

bool NineSlice_LoadBMP(SDL_Renderer* r, const char* path, NineSlice& out) {
    NineSlice_Destroy(out);

    SDL_Surface* raw = SDL_LoadBMP(path);
    if (!raw) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "NineSlice: SDL_LoadBMP(%s) failed: %s",
                     path, SDL_GetError());
        return false;
    }
    // Normalize to ARGB8888 so pixel reads below are format-independent.
    SDL_Surface* surf = SDL_ConvertSurface(raw, SDL_PIXELFORMAT_ARGB8888);
    SDL_DestroySurface(raw);
    if (!surf) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "NineSlice: convert failed: %s", SDL_GetError());
        return false;
    }
    if (surf->w < 3 || surf->h < 3) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "NineSlice: %s is too small (%dx%d) for a guide border", path, surf->w, surf->h);
        SDL_DestroySurface(surf);
        return false;
    }

    SDL_LockSurface(surf);
    const int W = surf->w, H = surf->h;        // includes the 1px border
    const int cw = W - 2, ch = H - 2;          // content size

    // Stretch insets: top border encodes horizontal span, left border vertical.
    int hFirst, hLast, vFirst, vLast;
    bool haveH = scanRun(cw, [&](int i) { return getPx(surf, 1 + i, 0); }, hFirst, hLast);
    bool haveV = scanRun(ch, [&](int i) { return getPx(surf, 0, 1 + i); }, vFirst, vLast);

    // Content/padding insets: bottom border (horizontal), right border (vertical).
    int pcFirst, pcLast, prFirst, prLast;
    bool haveP = scanRun(cw, [&](int i) { return getPx(surf, 1 + i, H - 1); }, pcFirst, pcLast);
    bool havePr = scanRun(ch, [&](int i) { return getPx(surf, W - 1, 1 + i); }, prFirst, prLast);
    SDL_UnlockSurface(surf);

    out.w = cw;
    out.h = ch;
    out.left = haveH ? hFirst : cw / 3;
    out.right = haveH ? (cw - 1 - hLast) : cw / 3;
    out.top = haveV ? vFirst : ch / 3;
    out.bottom = haveV ? (ch - 1 - vLast) : ch / 3;
    out.padL = haveP ? pcFirst : out.left;
    out.padR = haveP ? (cw - 1 - pcLast) : out.right;
    out.padT = havePr ? prFirst : out.top;
    out.padB = havePr ? (ch - 1 - prLast) : out.bottom;

    // Strip the 1px border into a fresh surface, then upload as a texture.
    SDL_Surface* inner = SDL_CreateSurface(cw, ch, SDL_PIXELFORMAT_ARGB8888);
    if (!inner) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "NineSlice: inner surface alloc failed: %s",
                     SDL_GetError());
        SDL_DestroySurface(surf);
        return false;
    }
    SDL_Rect srcR{ 1, 1, cw, ch };
    SDL_Rect dstR{ 0, 0, cw, ch };
    SDL_BlitSurface(surf, &srcR, inner, &dstR);
    SDL_DestroySurface(surf);

    out.tex = SDL_CreateTextureFromSurface(r, inner);
    SDL_DestroySurface(inner);
    if (!out.tex) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "NineSlice: texture create failed: %s",
                     SDL_GetError());
        return false;
    }
    // Nearest keeps corners and 1px guide lines crisp; the center still looks fine.
    SDL_SetTextureScaleMode(out.tex, SDL_SCALEMODE_NEAREST);
    SDL_SetTextureBlendMode(out.tex, SDL_BLENDMODE_BLEND);
    return true;
}

NineSlice NineSlice_FromTexture(SDL_Texture* tex, int left, int right, int top, int bottom) {
    NineSlice ns;
    ns.tex = tex;
    float fw = 0, fh = 0;
    if (tex) SDL_GetTextureSize(tex, &fw, &fh);
    ns.w = (int)fw;
    ns.h = (int)fh;
    ns.left = left;
    ns.right = right;
    ns.top = top;
    ns.bottom = bottom;
    ns.padL = left;
    ns.padR = right;
    ns.padT = top;
    ns.padB = bottom;
    return ns;
}

namespace {

// Draw one piece of the slice. A positive stepX / stepY repeats the src cell at
// that logical size across dst, clipped to dst; a zero step stretches to fill.
void drawPiece(SDL_Renderer* r, SDL_Texture* tex, const SDL_FRect& src, const SDL_FRect& dst,
               float stepX, float stepY) {
    if (dst.w <= 0 || dst.h <= 0 || src.w <= 0 || src.h <= 0) return;
    if (stepX <= 0.0f && stepY <= 0.0f) {
        SDL_RenderTexture(r, tex, &src, &dst);
        return;
    }
    SDL_Rect prevClip;
    bool hadClip = SDL_RenderClipEnabled(r);
    if (hadClip) SDL_GetRenderClipRect(r, &prevClip);
    SDL_Rect clip{ (int)dst.x, (int)dst.y, (int)SDL_ceilf(dst.w), (int)SDL_ceilf(dst.h) };
    SDL_SetRenderClipRect(r, &clip);

    const float sx = stepX > 0.0f ? stepX : dst.w;
    const float sy = stepY > 0.0f ? stepY : dst.h;
    for (float y = dst.y; y < dst.y + dst.h; y += sy) {
        for (float x = dst.x; x < dst.x + dst.w; x += sx) {
            SDL_FRect d{ x, y, sx, sy };
            SDL_RenderTexture(r, tex, &src, &d);
        }
    }
    SDL_SetRenderClipRect(r, hadClip ? &prevClip : nullptr);
}

} // namespace

void NineSlice_Draw(SDL_Renderer* r, const NineSlice& ns, const SDL_FRect& dst, NineSliceMode mode) {
    if (!ns.tex || ns.w <= 0 || ns.h <= 0) return;

    // Device pixels per logical unit for the view being drawn into (1 for render
    // targets and the authoring tool, which have logical presentation disabled).
    const float s = jplay::snap::scaleFor(r);

    // Whole device pixels per source pixel. Flooring keeps the corners inside the
    // widget at fractional scales — at 150% a 6px corner stays 6 device pixels
    // rather than becoming a resampled 9 — and the center absorbs the remainder.
    const float m = std::max(1.0f, std::floor(s + 0.001f));

    // Everything below is computed in device pixels and converted back at the
    // point of the draw call. Snapping the box matters at *every* scale here: art
    // at a fractional offset is resampled, not merely nudged. Both edges round
    // from the same logical coordinates, so abutting surfaces still meet exactly.
    const float x0 = std::round(dst.x * s);
    const float y0 = std::round(dst.y * s);
    const float x1 = std::round((dst.x + dst.w) * s);
    const float y1 = std::round((dst.y + dst.h) * s);
    const float dw = x1 - x0, dh = y1 - y0;
    if (dw <= 0.0f || dh <= 0.0f) return;

    // Corner sizes can't exceed the destination; shrink proportionally if so.
    // That resamples the art, but a widget smaller than its own corners has no
    // better option, and it is what this did before.
    float l = ns.left * m, rt = ns.right * m, t = ns.top * m, b = ns.bottom * m;
    if (l + rt > dw && (l + rt) > 0.0f) {
        const float k = dw / (l + rt);
        l *= k;
        rt *= k;
    }
    if (t + b > dh && (t + b) > 0.0f) {
        const float k = dh / (t + b);
        t *= k;
        b *= k;
    }

    const float sl = (float)ns.left, sr = (float)ns.right, st = (float)ns.top, sb = (float)ns.bottom;
    const float scw = ns.w - sl - sr; // src center width
    const float sch = ns.h - st - sb; // src center height

    // src column/row offsets, in texture pixels
    const float sx0 = 0, sx1 = sl, sx2 = ns.w - sr;
    const float sy0 = 0, sy1 = st, sy2 = ns.h - sb;
    // dst column/row offsets, in device pixels
    const float dx0 = x0, dx1 = x0 + l, dx2 = x1 - rt;
    const float dy0 = y0, dy1 = y0 + t, dy2 = y1 - b;
    const float dcw = dw - l - rt; // dst center width
    const float dch = dh - t - b;  // dst center height

    const bool tile = (mode == NineSliceMode::Tile);
    const float inv = 1.0f / s;

    // Device pixels back to logical units for the render call. A tiled axis
    // repeats the cell at its whole-multiple size rather than stretching.
    auto piece = [&](const SDL_FRect& src, float px, float py, float pw, float ph,
                     bool tileX, bool tileY) {
        const SDL_FRect d{ px * inv, py * inv, pw * inv, ph * inv };
        drawPiece(r, ns.tex, src, d,
                  tileX ? src.w * m * inv : 0.0f,
                  tileY ? src.h * m * inv : 0.0f);
    };

    // Corners (never tiled, never stretched in their fixed axis).
    piece({ sx0, sy0, sl, st }, dx0, dy0, l,  t,  false, false);
    piece({ sx2, sy0, sr, st }, dx2, dy0, rt, t,  false, false);
    piece({ sx0, sy2, sl, sb }, dx0, dy2, l,  b,  false, false);
    piece({ sx2, sy2, sr, sb }, dx2, dy2, rt, b,  false, false);

    // Edges: fixed at the whole multiple on one axis, free on the other.
    piece({ sx1, sy0, scw, st }, dx1, dy0, dcw, t,   tile, false); // top
    piece({ sx1, sy2, scw, sb }, dx1, dy2, dcw, b,   tile, false); // bottom
    piece({ sx0, sy1, sl, sch }, dx0, dy1, l,   dch, false, tile); // left
    piece({ sx2, sy1, sr, sch }, dx2, dy1, rt,  dch, false, tile); // right

    // Center.
    piece({ sx1, sy1, scw, sch }, dx1, dy1, dcw, dch, tile, tile);
}

void NineSlice_Destroy(NineSlice& ns) {
    if (ns.tex) SDL_DestroyTexture(ns.tex);
    ns = NineSlice{};
}

// ----------------------------------------------------------------------------
// Template generation
// ----------------------------------------------------------------------------

namespace {

void fillRect(SDL_Renderer* r, float x, float y, float w, float h, SDL_Color c) {
    SDL_SetRenderDrawColor(r, c.r, c.g, c.b, c.a);
    SDL_FRect rc{ x, y, w, h };
    SDL_RenderFillRect(r, &rc);
}

} // namespace

bool NineSlice_WriteTemplate(const char* path, int w, int h,
                             int left, int right, int top, int bottom,
                             bool renderBorder) {
    if (w < 8 || h < 8) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "NineSlice: template size %dx%d too small", w, h);
        return false;
    }
    // Clamp insets so the center keeps at least 1px in each axis.
    left = std::clamp(left, 1, w - 2);
    right = std::clamp(right, 1, w - 1 - left);
    top = std::clamp(top, 1, h - 2);
    bottom = std::clamp(bottom, 1, h - 1 - top);

    const int W = w + 2, H = h + 2; // include the 1px guide border
    SDL_Surface* surf = SDL_CreateSurface(W, H, SDL_PIXELFORMAT_ARGB8888);
    if (!surf) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "NineSlice: surface alloc failed: %s", SDL_GetError());
        return false;
    }
    SDL_Renderer* r = SDL_CreateSoftwareRenderer(surf);
    if (!r) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "NineSlice: software renderer failed: %s", SDL_GetError());
        SDL_DestroySurface(surf);
        return false;
    }
    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_NONE);

    // Transparent everywhere (border stays transparent except for the marks).
    SDL_SetRenderDrawColor(r, 0, 0, 0, 0);
    SDL_RenderClear(r);

    const float ox = 1.0f, oy = 1.0f;          // content origin (inside border)
    const float fl = (float)left, fr = (float)right, ft = (float)top, fb = (float)bottom;
    const float cx0 = ox + fl, cx1 = ox + w - fr;
    const float cy0 = oy + ft, cy1 = oy + h - fb;

    if (renderBorder) {
        // Actual frame art: a solid-red interior that fades to dark red over
        // each side's inset distance as it approaches the outer edge. The fade
        // fraction for a pixel is the smallest of its four per-edge fractions,
        // so proximity to any edge dims it; the darkest points are the corners.
        const SDL_Color core{ 220, 40, 40, 255 };
        const SDL_Color edge{ 60, 16, 16, 255 };
        for (int yy = 0; yy < h; ++yy) {
            for (int xx = 0; xx < w; ++xx) {
                float fL = (float)xx / left, fR = (float)(w - 1 - xx) / right;
                float fT = (float)yy / top, fB = (float)(h - 1 - yy) / bottom;
                float t = std::clamp(std::min(std::min(fL, fR), std::min(fT, fB)), 0.0f, 1.0f);
                SDL_Color c{ (Uint8)(edge.r + (core.r - edge.r) * t),
                             (Uint8)(edge.g + (core.g - edge.g) * t),
                             (Uint8)(edge.b + (core.b - edge.b) * t), 255 };
                fillRect(r, ox + xx, oy + yy, 1, 1, c);
            }
        }
    } else {
        // Checkerboard so transparency / region extents read clearly.
        const int cell = 8;
        for (int yy = 0; yy < h; ++yy) {
            for (int xx = 0; xx < w; ++xx) {
                bool dark = (((xx / cell) + (yy / cell)) & 1) != 0;
                SDL_Color c = dark ? SDL_Color{ 60, 64, 72, 255 } : SDL_Color{ 84, 90, 100, 255 };
                fillRect(r, ox + xx, oy + yy, 1, 1, c);
            }
        }

        // Tint the 9 regions: corners blue, edges green, center amber (semi-transparent).
        SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
        const SDL_Color cCorner{ 70, 130, 220, 70 };
        const SDL_Color cEdge{ 70, 200, 130, 55 };
        const SDL_Color cCenter{ 230, 180, 60, 55 };
        // corners
        fillRect(r, ox, oy, fl, ft, cCorner);
        fillRect(r, cx1, oy, fr, ft, cCorner);
        fillRect(r, ox, cy1, fl, fb, cCorner);
        fillRect(r, cx1, cy1, fr, fb, cCorner);
        // edges
        fillRect(r, cx0, oy, cx1 - cx0, ft, cEdge);
        fillRect(r, cx0, cy1, cx1 - cx0, fb, cEdge);
        fillRect(r, ox, cy0, fl, cy1 - cy0, cEdge);
        fillRect(r, cx1, cy0, fr, cy1 - cy0, cEdge);
        // center
        fillRect(r, cx0, cy0, cx1 - cx0, cy1 - cy0, cCenter);

        // Guide lines along the cut positions + a content outline.
        SDL_SetRenderDrawColor(r, 255, 60, 200, 255);
        SDL_RenderLine(r, cx0, oy, cx0, oy + h - 1);
        SDL_RenderLine(r, cx1, oy, cx1, oy + h - 1);
        SDL_RenderLine(r, ox, cy0, ox + w - 1, cy0);
        SDL_RenderLine(r, ox, cy1, ox + w - 1, cy1);
        SDL_SetRenderDrawColor(r, 230, 230, 235, 255);
        SDL_FRect outline{ ox, oy, (float)w, (float)h };
        SDL_RenderRect(r, &outline);

        // Region labels (8px debug font). Skip any that don't fit.
        SDL_SetRenderDrawColor(r, 245, 245, 250, 255);
        auto label = [&](float rx, float ry, float rw, float rh, const char* s) {
            float tw = (float)SDL_strlen(s) * 8.0f;
            if (rw < tw + 2 || rh < 10) return;
            SDL_RenderDebugText(r, rx + (rw - tw) * 0.5f, ry + (rh - 8) * 0.5f, s);
        };
        label(ox, oy, fl, ft, "TL");
        label(cx1, oy, fr, ft, "TR");
        label(ox, cy1, fl, fb, "BL");
        label(cx1, cy1, fr, fb, "BR");
        label(cx0, oy, cx1 - cx0, ft, "TOP");
        label(cx0, cy1, cx1 - cx0, fb, "BOTTOM");
        label(ox, cy0, fl, cy1 - cy0, "L");
        label(cx1, cy0, fr, cy1 - cy0, "R");
        label(cx0, cy0, cx1 - cx0, cy1 - cy0, "CENTER");
    }

    // Guide-border marks LAST, with exact colors and no blending, so the parser
    // reads them back precisely. Top: horizontal stretch span. Left: vertical span.
    // Bottom/right mirror them as the content box.
    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_NONE);
    SDL_SetRenderDrawColor(r, 0, 0, 0, 255);
    SDL_RenderLine(r, ox + fl, 0, ox + w - 1 - fr, 0);                 // top
    SDL_RenderLine(r, 0, oy + ft, 0, oy + h - 1 - fb);                 // left
    SDL_RenderLine(r, ox + fl, (float)H - 1, ox + w - 1 - fr, (float)H - 1); // bottom (content)
    SDL_RenderLine(r, (float)W - 1, oy + ft, (float)W - 1, oy + h - 1 - fb); // right (content)

    SDL_RenderPresent(r);
    bool ok = SDL_SaveBMP(surf, path);
    if (!ok) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "NineSlice: SDL_SaveBMP(%s) failed: %s",
                     path, SDL_GetError());
    }
    SDL_DestroyRenderer(r);
    SDL_DestroySurface(surf);
    return ok;
}

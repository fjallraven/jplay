#pragma once

// Rect-cut layout helpers.
//
// Instead of walking an x/y cursor and adding widths and gaps by hand, a panel
// starts from its content rect and *cuts* slices off it. Each cut shrinks the
// rect it is given and returns the slice removed, so the code reads top-to-
// bottom as the rows the panel holds:
//
//     SDL_FRect body = inset(panel, pad, 0.0f);
//     gapTop(body, 8.0f);
//     SDL_FRect header = cutTop(body, lineH);
//     drawText(header.x, header.y, kHeader, "PROJECT SETTINGS");
//     gapTop(body, 12.0f);
//     SDL_FRect row = cutTop(body, rowH);       // full-width row
//     SDL_FRect label = cutLeft(row, labelW);   // split it left-to-right
//
// A row's own sub-layout uses the same cuts on the row rect, so a control never
// needs to know where it sits in the panel — only how big it is. Nothing is
// retained between frames: these are pure functions over SDL_FRect.
//
// Name the slice before using it, as above, rather than writing
// `drawText(body.x, cutTop(body, lineH).y, ...)`: the argument order there is
// unsequenced, and while cutTop happens to leave x alone, cutLeft would not.
//
// Cuts clamp to what is left, so over-cutting a full panel yields zero-size
// slices instead of negative dimensions (a negative w/h makes SDL_RenderFillRect
// draw nothing but makes hit-testing behave strangely). Scrollable content must
// therefore be laid out into a kUnbounded-tall rect — see below.

#include <SDL3/SDL.h>

#include <algorithm>

namespace jplay {

// Height for the rect that scrollable panel content lays itself out into.
// Because cuts clamp, content bounded by its viewport would have every row past
// the bottom edge silently collapsed to zero height — and its measured height
// capped, which breaks the scroll range. Give it an effectively unbounded rect
// instead, let the clip rect bound what is *visible*, and read the y advance
// back as the content height.
constexpr float kUnbounded = 1.0e6f;

// Clamp a requested cut to what the rect can give. Note the order: clamping
// against the extent first and against zero second means an already-degenerate
// rect (negative w/h, e.g. a panel measured on a window too short to hold it)
// yields a zero-size slice rather than a negative-size one.
inline float clampCut(float amount, float extent) {
    if (amount > extent) amount = extent;
    return amount < 0.0f ? 0.0f : amount;
}

// Remove the top `h` pixels of `r` and return them.
inline SDL_FRect cutTop(SDL_FRect& r, float h) {
    h = clampCut(h, r.h);
    SDL_FRect slice{ r.x, r.y, r.w, h };
    r.y += h;
    r.h -= h;
    return slice;
}

// Remove the bottom `h` pixels of `r` and return them.
inline SDL_FRect cutBottom(SDL_FRect& r, float h) {
    h = clampCut(h, r.h);
    r.h -= h;
    return SDL_FRect{ r.x, r.y + r.h, r.w, h };
}

// Remove the left `w` pixels of `r` and return them.
inline SDL_FRect cutLeft(SDL_FRect& r, float w) {
    w = clampCut(w, r.w);
    SDL_FRect slice{ r.x, r.y, w, r.h };
    r.x += w;
    r.w -= w;
    return slice;
}

// Remove the right `w` pixels of `r` and return them.
inline SDL_FRect cutRight(SDL_FRect& r, float w) {
    w = clampCut(w, r.w);
    r.w -= w;
    return SDL_FRect{ r.x + r.w, r.y, w, r.h };
}

// Drop pixels off an edge of `r` without using them — a named gap between rows
// or between the buttons of a toolbar group.
inline void gapTop   (SDL_FRect& r, float h) { cutTop(r, h); }
inline void gapBottom(SDL_FRect& r, float h) { cutBottom(r, h); }
inline void gapLeft  (SDL_FRect& r, float w) { cutLeft(r, w); }
inline void gapRight (SDL_FRect& r, float w) { cutRight(r, w); }

// `r` shrunk by dx on the left/right and dy on the top/bottom.
inline SDL_FRect inset(const SDL_FRect& r, float dx, float dy) {
    return { r.x + dx, r.y + dy, r.w - dx * 2.0f, r.h - dy * 2.0f };
}

// Sub-rects centered within `r` on one axis or both — for putting a control or a
// text baseline in the middle of a band larger than it. These do not consume
// anything; pass the row you already cut.
inline SDL_FRect centerV(const SDL_FRect& r, float h) {
    return { r.x, r.y + (r.h - h) * 0.5f, r.w, h };
}
inline SDL_FRect centerH(const SDL_FRect& r, float w) {
    return { r.x + (r.w - w) * 0.5f, r.y, w, r.h };
}
inline SDL_FRect center(const SDL_FRect& r, float w, float h) {
    return { r.x + (r.w - w) * 0.5f, r.y + (r.h - h) * 0.5f, w, h };
}

// ── Scrolling bands ─────────────────────────────────────────────────────────
// A scrolling list cuts its rows off a kUnbounded rect offset by the scroll
// (see above), then asks per row whether that row landed in the visible band.

// True when `r` overlaps `band` vertically — the test a row uses to decide
// whether it is worth drawing. Written once because the negated form ("skip
// when entirely above or entirely below") is easy to get wrong by hand.
inline bool visibleIn(const SDL_FRect& r, const SDL_FRect& band) {
    return r.y + r.h > band.y && r.y < band.y + band.h;
}

// The part of `r` inside `band` vertically, keeping r's own x and w: the rect
// form of visibleIn, for a hit region that must stop at the band edge instead
// of reaching out under the panel chrome. Zero height when they miss.
inline SDL_FRect clipV(const SDL_FRect& r, const SDL_FRect& band) {
    const float top = std::max(r.y, band.y);
    const float bot = std::min(r.y + r.h, band.y + band.h);
    return { r.x, top, r.w, std::max(0.0f, bot - top) };
}

// Scrollbar thumb inside `track`, for `contentH` of content scrolled down by
// `scroll`. `minH` is the shortest the thumb may become — a parameter because
// the panels disagree about it. Zero height when the content fits the track,
// which is also the "draw no scrollbar" case.
inline SDL_FRect scrollThumb(const SDL_FRect& track, float contentH, float scroll, float minH) {
    const float maxScroll = contentH - track.h;
    if (maxScroll <= 0.0f || contentH <= 0.0f)
        return { track.x, track.y, track.w, 0.0f };
    const float h = std::max(minH, track.h * track.h / contentH);
    return { track.x, track.y + (track.h - h) * (scroll / maxScroll), track.w, h };
}

} // namespace jplay

#pragma once

// Device-pixel snapping for the UI drawing primitives.
//
// Layout is expressed in logical units and SDL scales logical -> device pixels
// for us (SDL_SetRenderLogicalPresentation, set up in App::computeLayout). At an
// *integer* scale that is lossless: a coordinate at x.0 or x.5 lands on an exact
// device pixel boundary, so 200% is just 100% drawn twice as large and there is
// nothing to correct.
//
// At a *fractional* scale it is not, and Windows' 125% / 150% are the two most
// common display settings there are. SDL turns a one-logical-unit line into a
// quad `scale` device pixels thick at an unrounded position (SDL_RenderLines
// takes its geometry path whenever logical presentation is on), and the
// rasteriser samples pixel centers with no antialiasing — so whether that quad
// lights one device pixel or two depends on where it happens to fall. Panel
// dividers, 1px outlines and hairline separators then come out unevenly
// weighted across the window, some crisp and some doubled.
//
// These wrappers fix it the way Skia and the browsers do: round each edge onto
// the device pixel grid, and give strokes a whole number of device pixels so
// equal logical thicknesses always render equal. Rect fills keep gap-free
// tiling — both edges are snapped from the same logical coordinates, so
// abutting panels still share an exact boundary and never show a seam. Rect
// outlines and axis-aligned lines are treated as strokes. Non-axis-aligned
// lines (caret chevrons, grade curves, freehand markup) pass straight through:
// snapping would only distort them.
//
// Everything short-circuits to the raw SDL call at integer scale, so 1x and 2x
// output is byte-for-byte what it was before.
//
// Splash.cpp deliberately keeps the raw SDL calls: it draws into its own
// fixed-size render target, not window chrome. The scaleFor() guard below would
// no-op there anyway.

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>

namespace jplay {

// Device pixels per logical unit, mirrored from App::uiScale_ (see
// App::setUiScale). A free variable rather than a parameter because these
// wrappers stand in for SDL calls made from ~27 translation units, several of
// which (Widgets, Menu, Dropdown, ContextMenu) have no App reference at all.
inline float gDeviceScale = 1.0f;

namespace snap {

// Render targets and the review window keep logical presentation disabled, so
// they already draw 1:1 in their own pixels and must not be snapped against the
// main window's scale. SDL_GetRenderLogicalPresentation reads the *current*
// view, which is the render target's when one is bound.
inline float scaleFor(SDL_Renderer* r) {
    SDL_RendererLogicalPresentation mode = SDL_LOGICAL_PRESENTATION_DISABLED;
    SDL_GetRenderLogicalPresentation(r, nullptr, nullptr, &mode);
    return mode == SDL_LOGICAL_PRESENTATION_DISABLED ? 1.0f : gDeviceScale;
}

inline bool fractional(float s) {
    return std::fabs(s - std::round(s)) > 0.001f;
}

// Device thickness of a one-logical-unit stroke, never thinner than a pixel.
inline float strokePx(float s) {
    return std::max(1.0f, std::round(s));
}

} // namespace snap

// Drop-in for SDL_RenderFillRect.
inline bool fillRect(SDL_Renderer* r, const SDL_FRect* rect) {
    const float s = snap::scaleFor(r);
    if (!rect || !snap::fractional(s) || rect->w <= 0.0f || rect->h <= 0.0f)
        return SDL_RenderFillRect(r, rect); // null = whole target; empty draws nothing

    const float t = snap::strokePx(s);
    const float x0 = std::round(rect->x * s);
    const float y0 = std::round(rect->y * s);
    float x1 = std::round((rect->x + rect->w) * s);
    float y1 = std::round((rect->y + rect->h) * s);
    // Hairline fills — separators, 1px insets, the text caret — are strokes in
    // disguise: pin them to the stroke thickness so they stop alternating
    // between one and two pixels depending on where they land. Widening one by
    // a pixel only overlaps the panel behind it, which is invisible; leaving it
    // to round on its own is what looks wrong.
    if (x1 - x0 <= t) x1 = x0 + t;
    if (y1 - y0 <= t) y1 = y0 + t;

    const SDL_FRect out{ x0 / s, y0 / s, (x1 - x0) / s, (y1 - y0) / s };
    return SDL_RenderFillRect(r, &out);
}

// Drop-in for SDL_RenderRect. SDL strokes the outline just inside the rect
// (it outlines x..x+w-1 with quads that extend one scale unit right/down, so
// the border lands within the rect's own device footprint); the four snapped
// edges below keep that convention.
inline bool drawRect(SDL_Renderer* r, const SDL_FRect* rect) {
    const float s = snap::scaleFor(r);
    if (!rect || !snap::fractional(s) || rect->w <= 0.0f || rect->h <= 0.0f)
        return SDL_RenderRect(r, rect);

    const float t = snap::strokePx(s);
    const float x0 = std::round(rect->x * s);
    const float y0 = std::round(rect->y * s);
    const float x1 = std::round((rect->x + rect->w) * s);
    const float y1 = std::round((rect->y + rect->h) * s);

    // Too small to hold two borders: the strokes would overlap into a solid
    // block, which is what SDL's overlapping lines produce as well.
    if (x1 - x0 <= t * 2.0f || y1 - y0 <= t * 2.0f) {
        const SDL_FRect solid{ x0 / s, y0 / s, (x1 - x0) / s, (y1 - y0) / s };
        return SDL_RenderFillRect(r, &solid);
    }

    const SDL_FRect edges[4] = {
        { x0 / s, y0 / s, (x1 - x0) / s, t / s },        // top
        { x0 / s, (y1 - t) / s, (x1 - x0) / s, t / s },  // bottom
        { x0 / s, y0 / s, t / s, (y1 - y0) / s },        // left
        { (x1 - t) / s, y0 / s, t / s, (y1 - y0) / s },  // right
    };
    bool ok = true;
    for (const SDL_FRect& e : edges)
        ok = SDL_RenderFillRect(r, &e) && ok;
    return ok;
}

// Drop-in for SDL_RenderLine.
inline bool drawLine(SDL_Renderer* r, float x1, float y1, float x2, float y2) {
    const float s = snap::scaleFor(r);
    if (!snap::fractional(s) || (x1 != x2 && y1 != y2))
        return SDL_RenderLine(r, x1, y1, x2, y2); // diagonal: nothing to snap

    const float t = snap::strokePx(s);
    const float ax = std::round(std::min(x1, x2) * s);
    const float ay = std::round(std::min(y1, y2) * s);
    const float bx = std::round(std::max(x1, x2) * s);
    const float by = std::round(std::max(y1, y2) * s);
    // SDL's line includes its end point and strokes right/down from it, so the
    // span is the extent plus one stroke width.
    const SDL_FRect out{ ax / s, ay / s, (bx - ax + t) / s, (by - ay + t) / s };
    return SDL_RenderFillRect(r, &out);
}

} // namespace jplay

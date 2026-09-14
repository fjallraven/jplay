#pragma once

#include <SDL3/SDL.h>

// 9-slice (a.k.a. nine-patch) image support.
//
// A 9-slice image is divided into 9 regions by two vertical and two horizontal
// cut lines. When drawn at an arbitrary size the four corners stay 1:1, the four
// edges stretch (or tile) along a single axis, and the center fills the rest.
// This lets a single small bitmap skin a button, panel or tooltip at any size
// without the borders distorting.
//
// The cut lines (stretch insets) are encoded in a 1px guide border around the
// source BMP, in the spirit of Android's ".9.png":
//   - top border:    one opaque-black run marks the horizontal stretch span,
//                    giving the left/right corner widths.
//   - left border:   one opaque-black run marks the vertical stretch span,
//                    giving the top/bottom corner heights.
//   - bottom border: optional black run marking the horizontal content box.
//   - right border:  optional black run marking the vertical content box.
// The 1px border is stripped when the texture is built; the insets below are in
// the coordinate space of the stripped (content) image.
//
// There is one tileset, authored at 1x. NineSlice_Draw makes it look right at
// every display scale rather than shipping a pack per scale — see the note there.
//
// This module depends only on SDL3 and PixelSnap.h (header-only, SDL3 only) so it
// can be reused by the main application as well as the tools/nineslice tool.

struct NineSlice {
    SDL_Texture* tex = nullptr;
    int w = 0, h = 0;        // texture size, with the guide border already stripped
    int left = 0, right = 0; // fixed corner column widths (px)
    int top = 0, bottom = 0; // fixed corner row heights (px)
    // Content padding box: where a widget's label / child content belongs.
    // Falls back to the stretch insets when the source has no content guides.
    int padL = 0, padR = 0, padT = 0, padB = 0;
};

enum class NineSliceMode {
    Stretch, // edges + center scale to fill
    Tile     // edges + center repeat at native size
};

// Parse a ".9" BMP: read the guide border, strip it, upload the inner image as a
// texture and fill `out`. Logs and returns false on failure. On success the
// caller owns out.tex (release with NineSlice_Destroy).
bool NineSlice_LoadBMP(SDL_Renderer* r, const char* path, NineSlice& out);

// Build a NineSlice from an already-loaded texture and explicit insets (no border
// parsing). The returned slice references `tex`; the caller still owns it.
NineSlice NineSlice_FromTexture(SDL_Texture* tex, int left, int right, int top, int bottom);

// Draw `ns` to fill `dst` (in logical units). Corners are 1:1, edges/center scale
// or tile per `mode`. Degrades gracefully if dst is smaller than the combined
// corners.
//
// The art is authored at 1x but must stay crisp on a 125% / 150% / 200% display.
// Rather than resampling it by the display scale — which lands a 1px border on
// one device pixel or two depending where it falls, the uneven-hairline artefact
// PixelSnap.h exists to remove — each source pixel is blitted at a *whole* number
// of device pixels, and the stretchable center absorbs the remainder. That costs
// nothing, because a center is a flat run of one colour, and it means borders and
// corner arcs are pixel-exact at every scale. The only visible consequence is
// that a corner radius stays its authored size in device pixels rather than
// growing with the display.
void NineSlice_Draw(SDL_Renderer* r, const NineSlice& ns, const SDL_FRect& dst,
                    NineSliceMode mode = NineSliceMode::Stretch);

// Destroy the texture and reset the struct. Safe to call on a zeroed NineSlice.
void NineSlice_Destroy(NineSlice& ns);

// --- Template generation ----------------------------------------------------
//
// Write a "blueprint" ".9" BMP a designer can paint widget art over: a
// checkerboard-backed image whose guide border encodes the given stretch insets,
// with the 9 regions drawn as color-coded, labeled cells. `w`/`h` are the inner
// (content) size; the saved file is (w+2) x (h+2) including the 1px border.
// Insets are corner sizes in content-space (left+right < w, top+bottom < h).
// When `renderBorder` is true the content is filled with actual frame art
// instead of the blueprint: a solid-red interior that fades to dark red over
// each side's inset distance as it approaches the outer edges. The guide border
// is still written, so the result reloads as a valid 9-slice.
// Logs and returns false on failure.
bool NineSlice_WriteTemplate(const char* path, int w, int h,
                             int left, int right, int top, int bottom,
                             bool renderBorder = false);

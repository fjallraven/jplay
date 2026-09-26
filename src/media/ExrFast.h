#pragma once

// Fast path for the EXR a render farm writes most: an uncompressed scanline file
// whose R, G and B are half channels. There is nothing to decode in such a file,
// only 50 MB of planar B/G/R rows to become interleaved RGB pixels, and OpenEXR's
// general machinery -- one read call per scanline, a line buffer per chunk, a
// slice copy per channel -- spends 24 ms on that at 4K where the data movement
// itself is worth about 8. This reader parses the header, streams the pixel data
// in large blocks, and interleaves each row with SIMD straight into the frame's
// buffer. Everything it does not understand hands back to OpenEXR: the result is
// either bit-identical to what ExrSequenceSource's OpenEXR path produces or a
// `false` and no result at all.
//
// JPLAY_EXR_NOFAST=1 disables it, for A/B checks and for isolating a suspect file.

#include "MediaSource.h"

#include <string>

namespace exrfast {

// Whether the fast path is on (not disabled by JPLAY_EXR_NOFAST).
bool enabled();

// Read `path`'s part `part`, channels rgb[0], rgb[1], rgb[2], into `out` as
// width*height*3 halves in display-window space (pixels the data window does not
// cover are black), the layout Frame::linearRgb has. A name may repeat (one
// channel shown as grayscale); every one must exist. On success sets dispW/dispH
// and returns true. Returns false, with `out` in an unspecified state, for any
// file the fast path does not cover: compressed, tiled, deep, float or subsampled
// channels, a data window wider than the display window, a chunk layout that is
// not one contiguous run, or a header this parser cannot follow.
// With `timing`, adds the header's time to waitIoMs and the pixel data's to
// readIoMs (the reads) and exrMs (the interleave), declined files included.
bool readRgbHalf(const std::string& path, int part, const std::string (&rgb)[3],
                 Frame::HalfBuffer& out, int& dispW, int& dispH,
                 ReadTiming* timing = nullptr);

// The interleave kernel on its own (planar R, G, B halves -> packed RGB), for the
// benchmark and for tests. `n` pixels.
void interleaveRgbHalf(const uint16_t* r, const uint16_t* g, const uint16_t* b, uint16_t* dst,
                       size_t n);

} // namespace exrfast

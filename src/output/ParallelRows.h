#pragma once

#include <algorithm>
#include <future>
#include <thread>
#include <vector>

// Run fn(yBegin, yEnd) over `h` rows, split into bands across the pool.
//
// Shared by the output backends' per-frame CPU passes (NDI's HDR encode, DeckLink's
// raster fit), where every row is independent. The NDI encode costs ~40 ms/frame at
// 1080p and ~160 ms at 4K single-threaded — enough to halve the playback rate on its
// own — and banding brings that to ~5 ms and ~14 ms. The join is a hard barrier, so
// no band outlives the caller's buffers.
template <typename Fn>
void parallelRows(int h, Fn fn) {
    unsigned n = std::thread::hardware_concurrency();
    if (n < 2 || h < 64) { // not worth the fan-out
        fn(0, h);
        return;
    }
    n = (std::min)(n, (unsigned)(h / 32)); // >= 32 rows per band
    const int band = (h + (int)n - 1) / (int)n;
    std::vector<std::future<void>> bands;
    bands.reserve(n - 1);
    for (int y = band; y < h; y += band) {
        const int y0 = y, y1 = (std::min)(y + band, h);
        bands.push_back(std::async(std::launch::async, [&fn, y0, y1] { fn(y0, y1); }));
    }
    fn(0, (std::min)(band, h)); // this thread takes the first band
    for (auto& b : bands)
        b.get();
}

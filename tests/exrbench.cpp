// exrbench: decode cost of ExrSequenceSource::readFrame, which is what caps how
// many EXR frames a second the cache can ready and how long the first frame (or
// a scrub target) takes to appear.
//
//   latency   one reader thread, frames read back to back: the single-frame case
//             (launch, scrubbing), where only OpenEXR's own pool can add cores.
//   fps @ W   W reader threads draining the sequence in parallel: the playback
//             case, as FrameCache's worker pool does it.
//
// Each row sets Imf::setGlobalThreadCount first, so the rows compare OpenEXR's
// internal threading against none. The sequence is read once untimed up front so
// every row sees a warm OS file cache and measures decode, not disk.
//
// The checksum line hashes the decoded buffers of two frames; it must not change
// across decoder rewrites.
//
//   exrbench <sequence path> [exr thread counts...]   (default: 0 4 <cores>)

#include "ExrSource.h"

#include <OpenEXR/ImfThreading.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

using Clock = std::chrono::steady_clock;

static double msOf(Clock::duration d) {
    return std::chrono::duration<double, std::milli>(d).count();
}

static uint64_t fnv(uint64_t h, const void* data, size_t n) {
    const auto* p = static_cast<const unsigned char*>(data);
    for (size_t i = 0; i < n; ++i)
        h = (h ^ p[i]) * 0x100000001b3ull;
    return h;
}

static uint64_t frameHash(const Frame& f) {
    // rgba8() rather than a member: for an EXR the 8-bit buffer is built on first
    // use now (see Frame::rgba8), and hashing it here is what keeps that build --
    // the same LUT pass readFrame used to run inline -- inside the checksum.
    uint64_t h = 0xcbf29ce484222325ull;
    h = fnv(h, f.rgba8().data(), f.rgba8().size());
    h = fnv(h, f.linearRgb.data(), f.linearRgb.size() * sizeof(Imath::half));
    return h;
}

static double medianLatencyMs(ExrSequenceSource& src, int64_t frames) {
    std::vector<double> ms;
    for (int64_t i = 0; i < frames; ++i) {
        auto t0 = Clock::now();
        FramePtr f = src.readFrame(i);
        ms.push_back(msOf(Clock::now() - t0));
    }
    std::sort(ms.begin(), ms.end());
    return ms[ms.size() / 2];
}

static double throughputFps(ExrSequenceSource& src, int workers, int64_t total) {
    std::atomic<int64_t> next{0};
    auto t0 = Clock::now();
    std::vector<std::thread> ts;
    for (int w = 0; w < workers; ++w)
        ts.emplace_back([&] {
            for (int64_t i; (i = next++) < total;)
                src.readFrame(i % src.frameCount());
        });
    for (auto& t : ts)
        t.join();
    return total / (msOf(Clock::now() - t0) / 1000.0);
}

int main(int argc, char** argv) {
    if (argc < 2) {
        printf("usage: exrbench <sequence path> [exr thread counts...]\n");
        return 1;
    }
    const int cores = (int)std::thread::hardware_concurrency();
    std::vector<int> exrThreads;
    for (int i = 2; i < argc; ++i)
        exrThreads.push_back(std::atoi(argv[i]));
    if (exrThreads.empty())
        exrThreads = { 0, 4, cores };

    std::string err;
    auto src = ExrSequenceSource::open(argv[1], err);
    if (!src) {
        printf("could not open %s: %s\n", argv[1], err.c_str());
        return 1;
    }
    const int64_t n = src->frameCount();
    printf("== %s\n   %dx%d, %lld frames, %d cores\n", argv[1], src->width(), src->height(),
           (long long)n, cores);
    for (const InfoField& f : src->describe())
        printf("   %s: %s\n", f.key.c_str(), f.value.c_str());

    Imf::setGlobalThreadCount(0);
    auto t0 = Clock::now();
    for (int64_t i = 0; i < n; ++i)
        src->readFrame(i);
    printf("   warm-up pass (untimed in rows): %.0f ms\n", msOf(Clock::now() - t0));

    FramePtr a = src->readFrame(0), b = src->readFrame(n / 2);
    printf("   checksum: %016llx %016llx, %.1f MiB/frame\n",
           a ? (unsigned long long)frameHash(*a) : 0ull,
           b ? (unsigned long long)frameHash(*b) : 0ull,
           a ? a->bytes() / (1024.0 * 1024.0) : 0.0);
    a.reset();
    b.reset();

    // The app's auto decode pool (App::init): cores - 2, clamped to 2..8.
    const int autoWorkers = std::clamp(cores - 2, 2, 8);
    const int64_t latencyFrames = std::min<int64_t>(n, 16);
    const int64_t total = std::max<int64_t>(n, 48);

    printf("\n   %10s %14s %10s %10s %10s\n", "exr thrds", "latency (ms)", "fps @ 1",
           ("fps @ " + std::to_string(autoWorkers)).c_str(),
           ("fps @ " + std::to_string(cores)).c_str());
    for (int t : exrThreads) {
        Imf::setGlobalThreadCount(t);
        const double lat = medianLatencyMs(*src, latencyFrames);
        const double f1 = throughputFps(*src, 1, std::min<int64_t>(total, 24));
        const double fa = throughputFps(*src, autoWorkers, total);
        const double fc = throughputFps(*src, cores, total);
        printf("   %10d %14.1f %10.1f %10.1f %10.1f\n", t, lat, f1, fa, fc);
    }
    return 0;
}

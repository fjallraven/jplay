// cachebench: measure the FrameCache hot paths that a code review flagged as
// complexity problems, so any fix is aimed at a measured cost rather than a
// big-O reading.
//
//   A. a submitCacheRequests() tick — the UI thread's per-rendered-frame cost of
//                           republishing a prefetch window of P frames.
//   B. evictLocked() scan — a worker's per-inserted-frame cost. Each insertion
//                           over budget rescans all of map_ to pick one victim,
//                           so cost tracks N (resident frames), not frame size.
//
// What A measured, against the review that prompted this tool: the per-tick cost
// is superlinear in P, but it is NOT request()'s linear dedup scan over pending_.
// Replacing that scan with an unordered_set moved nothing in the reachable range
// (P=2048: 8.2ms -> 9.9ms). Dropping the worker pool from 8 threads to 1, with
// the UI thread doing identical work, took P=4096 from 32.8ms to 5.8ms. The cost
// is contention for mtx_: workerLoop() holds it across an O(P) scan for the
// best runnable job AND an O(P) pending_.erase(), once per dequeued job, so the
// UI thread blocks behind every worker. Any fix belongs on the worker side.

#include "FrameCache.h"
#include "Media.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

using Clock = std::chrono::steady_clock;

static double msOf(Clock::duration d) {
    return std::chrono::duration<double, std::milli>(d).count();
}

// A frame of a requested byte size. Content is irrelevant to both paths under
// test — they only ever consult Frame::bytes() — but the bytes must really be
// allocated, since bytes() is derived from the vector's true size.
static FramePtr makeFrame(size_t bytes) {
    auto f = std::make_shared<Frame>();
    f->width = 1;
    f->height = (int)(bytes / 8);
    f->linearRgb.resize(bytes / sizeof(Imath::half));
    return f;
}

// One tick of submitCacheRequests(): beginRequests(), the whole prefetch window
// through request(), then endRequests(). Reports the median UI-thread time over
// several ticks.
//
// Workers are left running and draining, which is what the app really does. The
// media fast-fails to open (Media latches opened_ after the first miss, so
// ensureOpen then returns instantly), so what is measured is queue mechanics
// only, with no decode time mixed in. That makes these numbers a LOWER bound:
// a real 20 MiB EXR decode takes tens of ms, so pending_ stays far deeper in
// the app than it does here, and every O(P) walk over it costs more.
static void benchRequest(const char* label, bool resident, int numThreads) {
    const size_t kFrameBytes = 4096;

    printf("\n  %s (%d worker%s)\n", label, numThreads, numThreads == 1 ? "" : "s");
    printf("    %8s %16s %18s\n", "window P", "median tick (ms)", "of a 60fps tick");
    for (int P : { 64, 256, 1024, 2048, 4096 }) {
        // A missing path under a real local directory: the open fails in
        // microseconds, without an unmapped drive letter's lookup latency.
        auto media = std::make_shared<Media>(ClipType::ImageSequence,
                                             "D:/sdk/jplay2/build/cachebench_absent.####.exr");
        FrameCache cache(resident ? (size_t)P * kFrameBytes * 2 : (size_t)1 << 30, numThreads);
        if (resident) {
            // Make every key a map_ hit, so request() takes its early-return path
            // and never reaches the pending_ dedup at all.
            for (int i = 0; i < P; ++i)
                cache.put(CacheKey{ "bench", i }, makeFrame(kFrameBytes));
        }
        const int kTicks = 15;
        std::vector<double> ticks;
        ticks.reserve(kTicks);
        for (int t = 0; t < kTicks; ++t) {
            auto t0 = Clock::now();
            cache.beginRequests();
            for (int i = 0; i < P; ++i)
                cache.request(CacheKey{ "bench", i }, media, i, i);
            cache.endRequests();
            ticks.push_back(msOf(Clock::now() - t0));
        }
        std::sort(ticks.begin(), ticks.end());
        double med = ticks[ticks.size() / 2];
        printf("    %8d %16.3f %17.1f%%\n", P, med, med / 16.67 * 100.0);
    }
}

static void benchEvict(size_t frameBytes) {
    printf("\n  frame size %zu B (scan cost tracks N, not bytes)\n", frameBytes);
    printf("    %8s %16s %20s\n", "N", "per evict (us)", "per 24 decodes (ms)");
    for (int N : { 128, 512, 2048, 8192, 32768 }) {
        FrameCache cache((size_t)N * frameBytes, 8);
        // Fill to the budget untimed; from here every put() evicts ~one frame and
        // N holds steady, which is the state a playing cache lives in. put() does
        // not signal cv_, so no worker wakes and this stays pure main-thread time.
        for (int i = 0; i < N; ++i)
            cache.put(CacheKey{ "bench", i }, makeFrame(frameBytes));

        const int kIter = 200;
        auto t0 = Clock::now();
        for (int i = 0; i < kIter; ++i)
            cache.put(CacheKey{ "bench", N + i }, makeFrame(frameBytes));
        double per = msOf(Clock::now() - t0) / kIter;
        printf("    %8d %16.2f %20.2f\n", N, per * 1000.0, per * 24.0);
    }
}

int main(int argc, char** argv) {
    const char* exrPath = argc > 1 ? argv[1]
                                   : "D:/exr_sequences-v1.0.0/unh0400_0010_lighting.####.exr";

    printf("== real media: %s\n", exrPath);
    size_t realBytes = 0;
    {
        auto real = std::make_shared<Media>(ClipType::ImageSequence, exrPath);
        std::string err;
        auto src = real->ensureOpen(err);
        if (!src) {
            printf("   could not open: %s\n", err.c_str());
        } else {
            FramePtr f = src->readFrame(0);
            if (f) {
                realBytes = f->bytes();
                printf("   %dx%d, %lld frames, %.2f MiB/frame decoded\n",
                       f->width, f->height, (long long)src->frameCount(),
                       realBytes / (1024.0 * 1024.0));
                for (double gb : { 2.0, 8.0, 16.0 }) {
                    size_t n = (size_t)(gb * 1024.0 * 1024.0 * 1024.0) / realBytes;
                    printf("   cache %5.1f GiB holds N = %zu frames of this media\n", gb, n);
                }
            }
        }
    }

    printf("\n== A. request(): UI-thread cost of one submitCacheRequests() tick\n");
    // Identical UI-thread work at both worker counts. If the cost were the UI
    // thread's own dedup scan over pending_ it would not move; if it is
    // contention for mtx_ with workers walking and erasing pending_, it
    // collapses at one worker.
    benchRequest("cold cache / post-seek: every key misses and enters pending_", false, 8);
    benchRequest("cold cache / post-seek: every key misses and enters pending_", false, 1);
    benchRequest("steady state: every key already resident (early return)", true, 8);

    printf("\n== B. evictLocked(): worker cost per inserted frame\n");
    benchEvict(4096);

    return 0;
}

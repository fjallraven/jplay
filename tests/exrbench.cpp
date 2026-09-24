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
// Before the table: the interleave kernel the uncompressed fast path is built on
// (checked against a scalar reference and timed), and the cache-fill case -- every
// frame decoded and *held*, as at launch, once into fresh memory and once into
// memory the pool got back from the previous fill (see FrameAlloc.h).
//
//   exrbench <sequence path> [exr thread counts...]   (default: 0 4 <cores>)
//   JPLAY_EXR_NOFAST=1 exrbench ...                   OpenEXR path for every read
//   JPLAY_BENCH_COLD=1,4,8,16 exrbench ...            cold-read sweep instead (below)
//   JPLAY_BENCH_KEEP=1 JPLAY_BENCH_COLD=...           same sweep on cached files
//
// The cold sweep is the network-share case: before each row the files it reads
// are dropped from the OS cache (posix_fadvise DONTNEED, no root needed), so
// every read goes to the disk or server. Each worker count reads its own slice
// of the sequence, and the sweep runs up then down so a busy server's drift
// shows as two disagreeing rows rather than a fake trend. "cpu/worker" is
// process CPU time over wall time per worker: near 100% is decode-bound, low is
// time spent waiting on I/O.

#include "ExrFast.h"
#include "ExrSource.h"
#include "FrameAlloc.h"

#include <OpenEXR/ImfThreading.h>

#ifndef _WIN32
#include <fcntl.h>
#include <sys/resource.h>
#include <unistd.h>
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <random>
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

// Decode every frame with `workers` threads and hold all of them: the cache fill
// on launch, where nothing is recycled until the budget is reached. Returns the
// wall-clock time; `perFrame` gets each read's own latency.
static double fillMs(ExrSequenceSource& src, int workers, std::vector<FramePtr>& keep,
                     std::vector<double>& perFrame) {
    const int64_t n = src.frameCount();
    keep.assign((size_t)n, nullptr);
    perFrame.assign((size_t)n, 0.0);
    std::atomic<int64_t> next{0};
    auto t0 = Clock::now();
    std::vector<std::thread> ts;
    for (int w = 0; w < workers; ++w)
        ts.emplace_back([&] {
            for (int64_t i; (i = next++) < n;) {
                auto a = Clock::now();
                keep[(size_t)i] = src.readFrame(i);
                perFrame[(size_t)i] = msOf(Clock::now() - a);
            }
        });
    for (auto& t : ts)
        t.join();
    return msOf(Clock::now() - t0);
}

static double medianOf(std::vector<double> v) {
    std::sort(v.begin(), v.end());
    return v.empty() ? 0.0 : v[v.size() / 2];
}

// Empty the frame pool so the next allocations commit fresh memory from the OS.
static void drainPool() {
    FramePool::instance().setCapacity(0);
    FramePool::instance().setCapacity((size_t)16 << 30);
}

static void checkInterleaveKernel() {
    const size_t n = 4096 * 64; // 64 rows of a 4K frame
    std::vector<uint16_t> r(n), g(n), b(n), out(n * 3), ref(n * 3);
    std::mt19937 rng(1);
    for (size_t i = 0; i < n; ++i) {
        r[i] = (uint16_t)rng();
        g[i] = (uint16_t)rng();
        b[i] = (uint16_t)rng();
        ref[i * 3] = r[i];
        ref[i * 3 + 1] = g[i];
        ref[i * 3 + 2] = b[i];
    }
    // Odd lengths exercise the scalar tail as well as the vector body.
    for (size_t len : { n, n - 1, n - 5, (size_t)7 }) {
        std::fill(out.begin(), out.end(), 0);
        exrfast::interleaveRgbHalf(r.data(), g.data(), b.data(), out.data(), len);
        if (!std::equal(out.begin(), out.begin() + (ptrdiff_t)(len * 3), ref.begin())) {
            printf("   interleave kernel: MISMATCH at length %zu\n", len);
            return;
        }
    }
    const int reps = 200;
    auto t0 = Clock::now();
    for (int k = 0; k < reps; ++k)
        exrfast::interleaveRgbHalf(r.data(), g.data(), b.data(), out.data(), n);
    const double ms = msOf(Clock::now() - t0) / reps;
    printf("   interleave kernel: ok, %.1f GB/s moved (%zu px in %.3f ms)\n",
           (double)n * 6 * 2 / (ms * 1e6), n, ms);
}

#ifndef _WIN32
static bool evict(const std::string& path) {
    const int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0) return false;
    const bool ok = ::posix_fadvise(fd, 0, 0, POSIX_FADV_DONTNEED) == 0;
    ::close(fd);
    return ok;
}

static double cpuSeconds() {
    rusage ru{};
    getrusage(RUSAGE_SELF, &ru);
    return ru.ru_utime.tv_sec + ru.ru_stime.tv_sec +
           (ru.ru_utime.tv_usec + ru.ru_stime.tv_usec) / 1e6;
}

static int coldSweep(ExrSequenceSource& src, const char* spec) {
    std::vector<int> counts;
    for (const char* p = spec; *p;) {
        counts.push_back(std::max(1, std::atoi(p)));
        while (*p && *p != ',') ++p;
        if (*p) ++p;
    }
    const auto& files = src.sequenceFiles();
    const int64_t n = src.frameCount();
    const int64_t slice = std::min<int64_t>(48, n);
    const bool keep = std::getenv("JPLAY_BENCH_KEEP") != nullptr;
    for (const auto& f : files)
        if (!keep) evict(f);

    std::vector<int> order = counts;
    order.insert(order.end(), counts.rbegin(), counts.rend());
    printf("\n   %s reads, %lld frames per row, sweep up then down\n", keep ? "cached" : "cold",
           (long long)slice);
    printf("   %8s %8s %10s %12s %12s %11s\n", "workers", "fps", "MB/s", "read p50 ms",
           "read p95 ms", "cpu/worker");
    int64_t start = 0;
    for (int w : order) {
        if (start + slice > n) start = 0;
        for (int64_t i = start; i < start + slice && !keep; ++i)
            evict(files[(size_t)i]);
        std::vector<double> lat((size_t)slice);
        std::atomic<int64_t> next{0};
        std::atomic<size_t> bytes{0};
        const double cpu0 = cpuSeconds();
        auto t0 = Clock::now();
        std::vector<std::thread> ts;
        for (int k = 0; k < w; ++k)
            ts.emplace_back([&] {
                for (int64_t i; (i = next++) < slice;) {
                    auto a = Clock::now();
                    FramePtr f = src.readFrame(start + i);
                    lat[(size_t)i] = msOf(Clock::now() - a);
                    if (f) bytes += f->linearRgb.size() * sizeof(Imath::half);
                }
            });
        for (auto& t : ts)
            t.join();
        const double wall = msOf(Clock::now() - t0) / 1000.0;
        const double cpu = cpuSeconds() - cpu0;
        std::sort(lat.begin(), lat.end());
        printf("   %8d %8.1f %10.0f %12.0f %12.0f %10.0f%%\n", w, slice / wall,
               bytes / wall / 1e6, lat[lat.size() / 2], lat[lat.size() * 95 / 100],
               100.0 * cpu / (wall * w));
        fflush(stdout);
        start += slice;
    }
    return 0;
}
#endif

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
#ifndef _WIN32
    if (const char* cold = std::getenv("JPLAY_BENCH_COLD"))
        return coldSweep(*src, cold);
#endif

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

    // Whether the uncompressed fast path (ExrFast.h) covers this sequence, tried
    // directly on the first file: the unprefixed R/G/B of each part in turn.
    {
        Frame::HalfBuffer buf;
        int w = 0, h = 0, handled = -1;
        const std::string rgb[3] = { "R", "G", "B" };
        for (int part = 0; part < 4 && handled < 0; ++part)
            if (exrfast::readRgbHalf(src->sequenceFiles().front(), part, rgb, buf, w, h))
                handled = part;
        if (handled >= 0)
            printf("   fast path: handled (part %d)\n", handled);
        else
            printf("   fast path: declined -- OpenEXR reads this sequence\n");
    }

    // The app's auto decode pool (App::init): cores - 2, clamped to 2..8.
    const int autoWorkers = std::clamp(cores - 2, 2, 8);

    checkInterleaveKernel();

    {
        std::vector<FramePtr> keep;
        std::vector<double> lat;
        drainPool();
        const double freshWall = fillMs(*src, autoWorkers, keep, lat);
        const double freshLat = medianOf(lat);
        keep.clear(); // every buffer goes back to the pool...
        const double pooledWall = fillMs(*src, autoWorkers, keep, lat); // ...and is reused here
        const double pooledLat = medianOf(lat);
        keep.clear();
        printf("   fill @ %d, all %lld frames held:  fresh memory %6.0f ms (%5.1f fps, %5.1f ms/read p50)\n"
               "   %*s pooled       %6.0f ms (%5.1f fps, %5.1f ms/read p50)\n",
               autoWorkers, (long long)n, freshWall, n * 1000.0 / freshWall, freshLat,
               (int)(std::to_string(autoWorkers).size() + std::to_string(n).size() + 32), "",
               pooledWall, n * 1000.0 / pooledWall, pooledLat);

        // One read alone -- the first frame of a launch, a scrub target -- into
        // fresh memory and into a pooled buffer.
        drainPool();
        auto t0 = Clock::now();
        FramePtr f = src->readFrame(0);
        const double freshOne = msOf(Clock::now() - t0);
        f.reset();
        t0 = Clock::now();
        f = src->readFrame(1);
        const double pooledOne = msOf(Clock::now() - t0);
        f.reset();
        printf("   single read:  fresh memory %.1f ms | pooled %.1f ms\n", freshOne, pooledOne);
    }
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

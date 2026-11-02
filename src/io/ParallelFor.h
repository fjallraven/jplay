#pragma once

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <thread>
#include <vector>

// Run fn(i) for every i in [0, n) on up to `maxThreads` threads, the calling one
// among them, and return once all of them have finished.
//
// Meant for batches of independent waits rather than CPU work: NFS opens, header
// reads, Python callbacks that let go of the GIL for each listdir / stat. Indices
// are handed out one at a time, so a slow item holds up only its own thread.
// Once stopped() returns true no further index starts (the ones in flight run to
// completion), and the indices that did run are flagged in `ran` when given.
template <typename Fn, typename Stop>
void parallelFor(size_t n, size_t maxThreads, Fn fn, Stop stopped,
                 std::vector<char>* ran = nullptr) {
    if (ran)
        ran->assign(n, 0);
    std::atomic<size_t> next{ 0 };
    auto worker = [&] {
        for (size_t i; !stopped() && (i = next.fetch_add(1, std::memory_order_relaxed)) < n;) {
            fn(i);
            if (ran)
                (*ran)[i] = 1;
        }
    };
    std::vector<std::thread> pool;
    for (size_t t = 1; t < (std::min)(n, maxThreads); ++t)
        pool.emplace_back(worker);
    worker(); // this thread is one of them
    for (auto& t : pool)
        t.join();
}

#pragma once

// A split memcpy for the one synchronous copy on the GPU upload path, with a
// persistent set of helper threads behind it.
//
// Header-only so the benchmark measures this code rather than a copy of it.

#include <SDL3/SDL.h>

#include <algorithm>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <thread>
#include <vector>

// The copy into the mapped buffer, split across a few threads.
//
// This is the one synchronous part of the upload -- the texture call itself only
// queues a DMA -- and it is pure memory bandwidth, which a single thread does not
// saturate: for a 4K scene-linear half frame (50.6 MB) it measured 5.6 ms on one
// thread and 2.1 ms on four, and 8.9 vs 6.0 ms with the decode pool running flat
// out beside it. On a 60 Hz refresh the UI thread has 16.7 ms for everything, so
// that is several percent of the budget handed back on every displayed frame.
//
// The helpers are persistent: spawning three threads per frame costs more than the
// copy saves. The calling thread takes a share too, so kThreads-1 helpers do the
// rest and nothing is waiting on an idle core. Small buffers are copied inline --
// waking the helpers is worth it only when there is real work to share.
class StageCopy {
public:
    void run(void* dst, const void* src, size_t bytes) {
        if (bytes < kMinSplit || kThreads <= 1) {
            SDL_memcpy(dst, src, bytes);
            return;
        }
        start_();
        const size_t chunk = (bytes + kThreads - 1) / kThreads;
        {
            std::lock_guard<std::mutex> lk(mtx_);
            dst_ = (char*)dst;
            src_ = (const char*)src;
            bytes_ = bytes;
            chunk_ = chunk;
            remaining_ = kThreads - 1;
            ++job_;
        }
        cv_.notify_all();
        copyChunk_(0); // this thread's share, while the helpers take theirs
        std::unique_lock<std::mutex> lk(mtx_);
        done_.wait(lk, [this] { return remaining_ == 0; });
    }

    ~StageCopy() {
        {
            std::lock_guard<std::mutex> lk(mtx_);
            stop_ = true;
        }
        cv_.notify_all();
        for (std::thread& t : helpers_)
            if (t.joinable())
                t.join();
    }

private:
    static constexpr int kThreads = 4;
    static constexpr size_t kMinSplit = 4u << 20; // 4 MB

    void copyChunk_(int i) {
        const size_t off = (size_t)i * chunk_;
        if (off < bytes_)
            SDL_memcpy(dst_ + off, src_ + off, std::min(chunk_, bytes_ - off));
    }

    void start_() {
        if (!helpers_.empty())
            return;
        for (int i = 1; i < kThreads; ++i)
            helpers_.emplace_back([this, i] {
                uint64_t seen = 0;
                for (;;) {
                    std::unique_lock<std::mutex> lk(mtx_);
                    cv_.wait(lk, [&] { return stop_ || job_ != seen; });
                    if (stop_)
                        return;
                    seen = job_;
                    lk.unlock();
                    copyChunk_(i);
                    lk.lock();
                    if (--remaining_ == 0)
                        done_.notify_one();
                }
            });
    }

    std::vector<std::thread> helpers_;
    std::mutex mtx_;
    std::condition_variable cv_, done_;
    char* dst_ = nullptr;
    const char* src_ = nullptr;
    size_t bytes_ = 0, chunk_ = 0;
    uint64_t job_ = 0;
    int remaining_ = 0;
    bool stop_ = false;
};

// One pool for the process: the GL uploads it serves all run on the render thread,
// so there is never more than one copy in flight. Function-local so it is built on
// the first staged upload and torn down with the process, after the last one.
inline StageCopy& stageCopy() {
    static StageCopy s;
    return s;
}


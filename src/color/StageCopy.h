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
// Two ways to run it. run() is synchronous: the calling thread takes a share and
// returns when the copy is complete. start()/wait() hand the whole copy to the
// helpers and return at once, for a caller that has something else to do in the
// meantime -- OcioGpu::render starts the copy into a persistently mapped buffer
// before its first GL call, which on a vsynced swap chain is where the driver
// makes the thread wait out the previous frame's vblank; the copy runs inside
// that wait instead of after it.
//
// The helpers are persistent: spawning threads per frame costs more than the copy
// saves. Small buffers are copied inline -- waking the helpers is worth it only
// when there is real work to share.
class StageCopy {
public:
    // Copy synchronously: this thread takes one share, the helpers the rest.
    void run(void* dst, const void* src, size_t bytes) {
        wait(); // never two jobs at once
        if (bytes < kMinSplit) {
            SDL_memcpy(dst, src, bytes);
            return;
        }
        dispatch_(dst, src, bytes, kThreads - 1); // helpers take chunks 1..kThreads-1
        copyChunk_(0);                            // this thread's share, meanwhile
        finish_();
    }

    // Start the copy on the helpers alone and return. `src` and `dst` must stay
    // valid until wait() returns; there is at most one job in flight.
    void start(void* dst, const void* src, size_t bytes) {
        wait();
        if (bytes < kMinSplit) {
            SDL_memcpy(dst, src, bytes);
            return;
        }
        dispatch_(dst, src, bytes, kThreads); // helpers take every chunk
        pending_ = true;
    }

    // Block until the copy start() began is complete. A no-op with none pending.
    void wait() {
        if (!pending_)
            return;
        finish_();
        pending_ = false;
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
    static constexpr int kThreads = 4;               // chunks per copy, one thread each
    static constexpr size_t kMinSplit = 4u << 20;    // 4 MB

    // Chunk i of the current job; a no-op past the end of the buffer.
    void copyChunk_(int i) {
        const size_t off = (size_t)i * chunk_;
        if (off < bytes_)
            SDL_memcpy(dst_ + off, src_ + off, std::min(chunk_, bytes_ - off));
    }

    // Publish a job. Helper h copies chunk kThreads-1-h, so `helperChunks` helpers
    // take the top chunks and the rest of the helpers sit this one out.
    void dispatch_(void* dst, const void* src, size_t bytes, int helperChunks) {
        start_();
        {
            std::lock_guard<std::mutex> lk(mtx_);
            dst_ = (char*)dst;
            src_ = (const char*)src;
            bytes_ = bytes;
            chunk_ = (bytes + kThreads - 1) / kThreads;
            helperChunks_ = helperChunks;
            remaining_ = helperChunks;
            ++job_;
        }
        cv_.notify_all();
    }

    void finish_() {
        std::unique_lock<std::mutex> lk(mtx_);
        done_.wait(lk, [this] { return remaining_ == 0; });
    }

    void start_() {
        if (!helpers_.empty())
            return;
        for (int h = 0; h < kThreads; ++h)
            helpers_.emplace_back([this, h] {
                uint64_t seen = 0;
                for (;;) {
                    std::unique_lock<std::mutex> lk(mtx_);
                    cv_.wait(lk, [&] { return stop_ || job_ != seen; });
                    if (stop_)
                        return;
                    seen = job_;
                    const bool mine = h < helperChunks_;
                    lk.unlock();
                    if (!mine)
                        continue;
                    copyChunk_(kThreads - 1 - h);
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
    int helperChunks_ = 0;
    uint64_t job_ = 0;
    int remaining_ = 0;
    bool stop_ = false;
    bool pending_ = false; // a start()ed job not yet wait()ed for (caller's thread only)
};

// One pool for the process: the GL uploads it serves all run on the render thread,
// so there is never more than one copy in flight. Function-local so it is built on
// the first staged upload and torn down with the process, after the last one.
inline StageCopy& stageCopy() {
    static StageCopy s;
    return s;
}

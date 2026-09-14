#include "FrameCache.h"

#include <SDL3/SDL_log.h>

#include <algorithm>

FrameCache::FrameCache(size_t maxBytes, int numThreads)
    : maxBytes_(maxBytes) {
    numThreads = std::max(1, numThreads);
    workers_.reserve((size_t)numThreads);
    for (int i = 0; i < numThreads; ++i)
        workers_.emplace_back(&FrameCache::workerLoop, this);
}

FrameCache::~FrameCache() {
    {
        std::lock_guard<std::mutex> lk(mtx_);
        stop_ = true;
        pending_.clear();
    }
    cv_.notify_all();
    for (auto& t : workers_)
        t.join();
}

FramePtr FrameCache::get(const CacheKey& key) {
    std::lock_guard<std::mutex> lk(mtx_);
    auto it = map_.find(key);
    if (it == map_.end())
        return nullptr;
    it->second.tick = ++tick_;
    return it->second.frame;
}

bool FrameCache::mediaFailed(const std::string& mediaId) const {
    std::lock_guard<std::mutex> lk(mtx_);
    return failedMedia_.count(mediaId) != 0;
}

bool FrameCache::has(const CacheKey& key) {
    std::lock_guard<std::mutex> lk(mtx_);
    return map_.count(key) != 0;
}

void FrameCache::put(const CacheKey& key, FramePtr frame) {
    if (!frame)
        return;
    std::vector<FramePtr> doomed;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        if (map_.count(key)) // already resident (a worker may have just fetched it)
            return;
        totalBytes_ += frame->bytes();
        // wantEpoch 0 keeps it out of the current wanted set: evictLocked() treats it
        // as evictable-first so it can't push out frames the playhead actually needs.
        map_[key] = { std::move(frame), ++tick_, 0, 0 };
        evictLocked(doomed);
    }
    // doomed is freed here, with the lock released.
}

void FrameCache::snapshotKeys(std::vector<CacheKey>& out) {
    out.clear();
    std::lock_guard<std::mutex> lk(mtx_);
    out.reserve(map_.size());
    for (auto& kv : map_)
        out.push_back(kv.first);
}

void FrameCache::beginRequests() {
    staged_.clear();
    stagedKeys_.clear();
}

void FrameCache::request(const CacheKey& key, std::shared_ptr<Media> media, int64_t srcFrame, int priority) {
    if (!media)
        return;
    // Staging only: no lock, and dedup by hash. The look-ahead laps a short loop
    // range and a dissolve asks for both halves, so duplicates are the norm rather
    // than the exception -- and the window can be hundreds of frames deep.
    if (!stagedKeys_.insert(key).second)
        return;
    staged_.push_back({ key, std::move(media), srcFrame, priority });
}

void FrameCache::endRequests() {
    {
        std::lock_guard<std::mutex> lk(mtx_);
        // The epoch is bumped here rather than in beginRequests() so it changes
        // together with the set it describes: a worker evicting in the gap between
        // the two would otherwise see an empty wanted set and be free to drop the
        // frames the playhead needs.
        ++wantEpoch_; // frames not in this batch become evictable
        pending_.clear();
        for (auto& s : staged_) {
            auto it = map_.find(s.key);
            if (it != map_.end()) {
                // Already resident: refresh its standing in the current wanted set so
                // eviction keeps frames near the playhead and drops distant ones first.
                if (it->second.wantEpoch != wantEpoch_ || s.priority < it->second.wantPrio) {
                    it->second.wantEpoch = wantEpoch_;
                    it->second.wantPrio = s.priority;
                }
                continue;
            }
            // Skip only a read that is still live. One started before a clear()
            // will have its result thrown away on arrival, so leaving the frame out
            // of the batch would mean nothing is decoding it at all (see inflight_).
            auto inf = inflight_.find(s.key);
            if (inf != inflight_.end() && inf->second == generation_)
                continue;
            pending_.push_back(std::move(s));
        }
    }
    cv_.notify_all();
}

void FrameCache::clear() {
    // Swapped out rather than cleared in place, for the same reason as
    // evictLocked's doomed list: a full cache is gigabytes of frame buffers, and
    // freeing them under the lock stalls every worker and the UI thread with it.
    std::unordered_map<CacheKey, Entry, CacheKeyHash> doomed;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        pending_.clear();
        doomed.swap(map_);
        failedMedia_.clear(); // a re-request may well succeed (the render may have finished)
        totalBytes_ = 0;
        ++generation_;
    }
}

uint64_t FrameCache::generation() const {
    std::lock_guard<std::mutex> lk(mtx_);
    return generation_;
}

size_t FrameCache::bytesUsed() const {
    std::lock_guard<std::mutex> lk(mtx_);
    return totalBytes_;
}

void FrameCache::setMaxBytes(size_t maxBytes) {
    std::vector<FramePtr> doomed;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        maxBytes_ = maxBytes;
        evictLocked(doomed);
    }
}

size_t FrameCache::avgFrameBytes() const {
    std::lock_guard<std::mutex> lk(mtx_);
    return map_.empty() ? 0 : totalBytes_ / map_.size();
}

void FrameCache::evictLocked(std::vector<FramePtr>& doomed) {
    // Returns true if `a` should be evicted before `b`. Frames outside the
    // current wanted set go first (oldest tick first); among them the wanted set
    // is protected, and only sacrificed under real pressure least-important
    // (highest priority value, i.e. farthest from the playhead) first.
    auto moreEvictable = [this](const Entry& a, const Entry& b) {
        bool aWanted = a.wantEpoch == wantEpoch_;
        bool bWanted = b.wantEpoch == wantEpoch_;
        if (aWanted != bWanted)
            return !aWanted;
        if (!aWanted)
            return a.tick < b.tick;
        if (a.wantPrio != b.wantPrio)
            return a.wantPrio > b.wantPrio;
        return a.tick < b.tick;
    };
    auto overBudget = [this] { return totalBytes_ > maxBytes_ && map_.size() > 1; };
    if (!overBudget())
        return;
    auto drop = [&](decltype(map_.begin()) it) {
        totalBytes_ -= it->second.frame ? it->second.frame->bytes() : 0;
        doomed.push_back(std::move(it->second.frame)); // freed by the caller, unlocked
        map_.erase(it);
    };
    // One victim is the usual case -- a single insert tipped the total over -- and
    // picking it by one linear pass beats ordering the whole map.
    auto victim = map_.begin();
    for (auto it = map_.begin(); it != map_.end(); ++it)
        if (moreEvictable(it->second, victim->second))
            victim = it;
    drop(victim);
    if (!overBudget())
        return;
    // More than one: a large frame arriving among small ones, or a shrunk budget.
    // Rescanning per victim is quadratic in the resident count, so order the map
    // once and walk it instead. Erasing from an unordered_map leaves the other
    // iterators valid, so the order stays usable as we go.
    std::vector<decltype(map_.begin())> order;
    order.reserve(map_.size());
    for (auto it = map_.begin(); it != map_.end(); ++it)
        order.push_back(it);
    std::sort(order.begin(), order.end(),
              [&](auto a, auto b) { return moreEvictable(a->second, b->second); });
    for (auto it : order) {
        if (!overBudget())
            return;
        drop(it);
    }
}

void FrameCache::workerLoop() {
    std::unique_lock<std::mutex> lk(mtx_);
    for (;;) {
        // Pick the most urgent runnable request (lowest priority value). A video
        // whose decoder another worker is inside is not runnable: VideoSource is
        // stateful and mutex-serialized, so a second worker gains no parallelism —
        // it only races for the decoder out of order and forces keyframe seeks.
        // Keeping one worker per video drains its requests in priority order,
        // which is sequential forward decode. Image-sequence frames are stateless
        // independent reads and stay fully parallel.
        size_t best = SIZE_MAX;
        cv_.wait(lk, [&] {
            if (stop_)
                return true;
            best = SIZE_MAX;
            for (size_t i = 0; i < pending_.size(); ++i) {
                if (pending_[i].media->type() != ClipType::ImageSequence &&
                    busyVideos_.count(pending_[i].key.media))
                    continue;
                if (best == SIZE_MAX || pending_[i].priority < pending_[best].priority)
                    best = i;
            }
            return best != SIZE_MAX;
        });
        if (stop_)
            return;

        Pending job = std::move(pending_[best]);
        pending_.erase(pending_.begin() + (ptrdiff_t)best);
        if (map_.count(job.key))
            continue;
        inflight_.insert_or_assign(job.key, generation_);
        bool isVideo = job.media->type() != ClipType::ImageSequence;
        if (isVideo)
            busyVideos_.insert(job.key.media);
        uint64_t gen = generation_;

        lk.unlock();
        std::string err;
        auto src = job.media->ensureOpen(err);          // slow: first call opens the decoder
        // + slateOffset: under a proxy mode whose substitute opens on a slate card,
        // the shot starts behind it and every index shifts by that much.
        FramePtr frame = src ? src->readFrame(job.srcFrame + job.media->slateOffset())
                             : nullptr; // slow: decode/IO
        lk.lock();

        // Only if this read is still the live one: a doomed read finishing after a
        // fresh one was started for the same frame must not retract its entry.
        auto inf = inflight_.find(job.key);
        if (inf != inflight_.end() && inf->second == gen)
            inflight_.erase(inf);
        if (isVideo) {
            busyVideos_.erase(job.key.media);
            cv_.notify_all(); // wake workers blocked on this video's queued frames
        }
        if (!frame) {
            // Log once per media, not once per requested frame: a broken sequence is
            // asked for dozens of times a second by the prefetch.
            if (failedMedia_.insert(job.key.media).second)
                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Failed to read frame %lld of %s%s%s",
                            (long long)job.srcFrame, job.media->path().c_str(),
                            err.empty() ? "" : ": ", err.c_str());
        } else {
            failedMedia_.erase(job.key.media);
            if (gen == generation_ && !map_.count(job.key)) {
                totalBytes_ += frame->bytes();
                map_[job.key] = { std::move(frame), ++tick_, wantEpoch_, job.priority };
                std::vector<FramePtr> doomed;
                evictLocked(doomed);
                if (!doomed.empty()) {
                    lk.unlock();
                    doomed.clear();  // slow: see evictLocked
                    lk.lock();
                }
            }
        }
    }
}

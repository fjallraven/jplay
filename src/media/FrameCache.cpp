#include "FrameCache.h"
#include "FrameAlloc.h"

#include <SDL3/SDL_log.h>

#include <algorithm>
#include <iterator>

FrameCache::FrameCache(size_t maxBytes, int numThreads)
    : maxBytes_(maxBytes) {
    // The frame buffers this cache evicts stay resident in the process-wide pool
    // for the next decode to reuse (see FrameAlloc.h), and the pool may hold at
    // most what the cache itself may: pooled plus cached never exceeds twice the
    // budget, and in the steady state of one eviction per insert, the pool holds
    // about one frame per decode thread.
    FramePool::instance().setCapacity(maxBytes);
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
    // A frame can grow after admission by building its 8-bit buffer (Frame::rgba8),
    // and the ones that do are the ones handed out here -- the displayed frame a
    // scope, the inspector or an uncoloured-managed upload just read. Reconciling
    // on the way out keeps the budget honest without a walk of its own.
    const size_t now = it->second.frame ? it->second.frame->bytes() : 0;
    if (now != it->second.bytes) {
        totalBytes_ = totalBytes_ - it->second.bytes + now;
        it->second.bytes = now;
    }
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
        const size_t admitted = frame->bytes();
        totalBytes_ += admitted;
        // wantEpoch 0 keeps it out of the current wanted set: evictLocked() treats it
        // as evictable-first so it can't push out frames the playhead actually needs.
        map_[key] = { std::move(frame), admitted, ++tick_, 0, 0 };
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
    // ...and then not freed on this thread either. clear() is called from the UI
    // thread on a proxy switch, a project load and a colour-management change, and
    // returning the frames to the allocator is ~5 ms each: a full 12 GiB cache of
    // 4K EXR frames took 690 ms with nothing else running, and 840 ms while the
    // decode pool was busy -- a visible freeze at exactly the moment the user just
    // asked for something. Nothing here needs to have happened before the next
    // frame is drawn: the map is already detached and unreachable, so a detached
    // thread can take its time over it.
    //
    // Detached rather than pooled because this must not queue behind decode work
    // (the pool is what the refill runs on) and it must survive a shutdown that
    // does not join anything (App::run ends in _Exit). One thread per flush, and a
    // flush is a user action, so they cannot pile up.
    if (!doomed.empty())
        std::thread([d = std::move(doomed)]() mutable { d.clear(); }).detach();
}

void FrameCache::dropMedia(const std::string& mediaId) {
    std::vector<FramePtr> doomed;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        ++mediaEpoch_[mediaId];
        for (auto it = map_.begin(); it != map_.end();) {
            if (it->first.media == mediaId) {
                totalBytes_ -= std::min(totalBytes_, it->second.bytes);
                doomed.push_back(std::move(it->second.frame));
                it = map_.erase(it);
            } else {
                ++it;
            }
        }
        // Its reads in flight are doomed, so none of them may hold off a re-request
        // (see inflight_): the next endRequests() starts fresh ones straight away.
        for (auto it = inflight_.begin(); it != inflight_.end();)
            it = it->first.media == mediaId ? inflight_.erase(it) : std::next(it);
        failedMedia_.erase(mediaId);
    }
    // Freed off the lock and off this thread, as clear() does and for its reason.
    if (!doomed.empty())
        std::thread([d = std::move(doomed)]() mutable { d.clear(); }).detach();
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
    FramePool::instance().setCapacity(maxBytes); // see the constructor
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
        totalBytes_ -= std::min(totalBytes_, it->second.bytes);
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
            // Lowest value wins, so the pick is a prefetch read only once no
            // on-screen frame is left pending; the hold then waits out the ones
            // still being read (see firstFrameHold_).
            if (best != SIZE_MAX && firstFrameHold_ && holdReads_ > 0 &&
                pending_[best].priority > 0)
                best = SIZE_MAX;
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
        const uint64_t mediaEpoch = mediaEpochLocked(job.key.media);
        const bool held = firstFrameHold_ && job.priority <= 0;
        if (held)
            ++holdReads_;

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
        // Doomed by clear() (the generation moved) or by dropMedia() (its media's
        // epoch did).
        const bool live = gen == generation_ && mediaEpochLocked(job.key.media) == mediaEpoch;
        auto inf = inflight_.find(job.key);
        if (live && inf != inflight_.end() && inf->second == gen)
            inflight_.erase(inf);
        if (isVideo) {
            busyVideos_.erase(job.key.media);
            cv_.notify_all(); // wake workers blocked on this video's queued frames
        }
        if (held) {
            --holdReads_;
            // A doomed read (flushed by a project load) settles nothing: the
            // batch that replaced it has its own on-screen frame to wait for.
            if (live && holdReads_ == 0 &&
                std::none_of(pending_.begin(), pending_.end(),
                             [](const Pending& p) { return p.priority <= 0; }))
                firstFrameHold_ = false;
            cv_.notify_all(); // wake the workers holding off their prefetch
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
            if (live && !map_.count(job.key)) {
                const size_t admitted = frame->bytes();
                totalBytes_ += admitted;
                map_[job.key] = { std::move(frame), admitted, ++tick_, wantEpoch_, job.priority };
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

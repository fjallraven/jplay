#pragma once

#include "Media.h"

#include <condition_variable>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

struct CacheKey {
    std::string media;   // Media::id — frames are shared by every clip on this media
    int64_t frame = -1;  // source frame within the media
    bool operator==(const CacheKey& o) const { return frame == o.frame && media == o.media; }
};

struct CacheKeyHash {
    size_t operator()(const CacheKey& k) const {
        return std::hash<std::string>()(k.media) ^ (std::hash<int64_t>()(k.frame) * 0x9E3779B97F4A7C15ull);
    }
};

// LRU frame cache with a pool of background reader threads. The UI thread
// republishes its wanted set every tick between beginRequests()/endRequests();
// workers always pick the lowest-priority-value pending request, so the
// playhead frame loads first and prefetch fills outward from it.
class FrameCache {
public:
    FrameCache(size_t maxBytes, int numThreads);
    ~FrameCache();

    FramePtr get(const CacheKey& key); // touches LRU; nullptr if absent
    bool has(const CacheKey& key);
    // Insert an already-decoded frame (e.g. lifted from a hover-preview decode) so
    // playback can reuse it. Stored outside the current wanted set, so it is among
    // the first evicted under pressure and never displaces playhead-wanted frames.
    void put(const CacheKey& key, FramePtr frame);
    void snapshotKeys(std::vector<CacheKey>& out); // for cache-strip drawing

    // One wanted set per tick: beginRequests(), a request() per frame, then
    // endRequests() publishes the batch. request() only stages on the calling
    // thread, so all three must be called from that one thread (the UI thread).
    void beginRequests();
    // The media's decoder is opened lazily on a worker thread the first time one
    // of its frames is pulled, so a freshly-loaded project never blocks the UI.
    void request(const CacheKey& key, std::shared_ptr<Media> media, int64_t srcFrame, int priority);
    void endRequests(); // publish the batch; drops all not-yet-started requests

    // A media whose frames the workers could not read: the decoder wouldn't open,
    // or readFrame kept failing. Without this a source that opens but decodes to
    // nothing (a half-written render, an unreadable frame) leaves the player on
    // LOADING for ever with nothing said anywhere. Cleared by clear(), and by the
    // first frame of that media that does decode.
    bool mediaFailed(const std::string& mediaId) const;

    void clear(); // drop cached frames + pending work (in-flight reads finish and are discarded)
    // Bumped by clear(). A caller that skips re-submitting an unchanged wanted set
    // watches this so a flushed cache is refilled rather than left empty.
    uint64_t generation() const;
    size_t bytesUsed() const;
    size_t bytesMax() const { return maxBytes_; }
    void setMaxBytes(size_t maxBytes); // resize the budget; evicts immediately if shrunk
    size_t avgFrameBytes() const; // mean resident frame size (0 when empty) — for sizing a prefetch window to the budget

private:
    struct Pending {
        CacheKey key;
        std::shared_ptr<Media> media;
        int64_t srcFrame = 0;
        int priority = 0;
    };
    struct Entry {
        FramePtr frame;
        uint64_t tick = 0;
        uint64_t wantEpoch = 0; // last beginRequests() epoch that re-requested this frame
        int wantPrio = 0;       // its priority in that epoch (lower = nearer the playhead)
    };

    void workerLoop();
    // Trims to budget, moving each evicted frame into `doomed` instead of
    // destroying it. Freeing a frame is slow -- tens of MB of munmap for a large
    // one -- so the caller drops `doomed` after releasing the lock. Doing it under
    // the lock made the UI thread block for hundreds of ms behind a burst of
    // evictions, in whichever of its two cache calls landed first.
    void evictLocked(std::vector<FramePtr>& doomed);

    mutable std::mutex mtx_;
    std::condition_variable cv_;
    std::vector<std::thread> workers_;
    bool stop_ = false;
    uint64_t generation_ = 0; // bumped by clear(); stale in-flight results are dropped

    // The wanted set is staged on the UI thread and committed by endRequests()
    // under a single lock. Locking per request cost thousands of contended
    // lock/unlock pairs and a linear dedup scan of pending_ every tick, both on
    // the UI thread; hashed dedup and one lock replace that.
    std::vector<Pending> staged_;
    std::unordered_set<CacheKey, CacheKeyHash> stagedKeys_;

    std::vector<Pending> pending_;
    // Reads a worker is currently inside, each against the generation() it was
    // started for. clear() cannot stop a read that is already running -- the
    // result is discarded when it arrives -- so a bare "is it in flight" answer
    // would let endRequests() skip re-requesting a frame whose only read is
    // already doomed, dropping it out of the wanted set until the next refresh
    // (half a second later) with nothing decoding it in the meantime. The
    // generation is what tells a live read from a doomed one.
    std::unordered_map<CacheKey, uint64_t, CacheKeyHash> inflight_;
    std::unordered_set<std::string> busyVideos_; // media ids a worker is currently decoding (one worker per video)
    std::unordered_set<std::string> failedMedia_; // media ids whose reads failed (see mediaFailed)
    std::unordered_map<CacheKey, Entry, CacheKeyHash> map_;
    size_t totalBytes_ = 0;
    size_t maxBytes_;
    uint64_t tick_ = 0;
    uint64_t wantEpoch_ = 0; // bumped by beginRequests(); defines the current wanted set
};

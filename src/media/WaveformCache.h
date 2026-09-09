#pragma once

#include <map>
#include <set>
#include <string>
#include <vector>

class WorkQueue;

// Background peak-data store for the waveforms drawn inside timeline clips. For
// an audio file (a video's embedded track, or an external WAV) it decodes the
// track on the shared WorkQueue and keeps a compact max-amplitude envelope (one
// 0..1 peak per fixed time bin). Rendering samples this envelope per pixel
// column; precision is deliberately coarse (kBinsPerSec) since the waveform is a
// faint decoration. Bins are placed by the decoder's reported source time, so a
// seek landing before the requested point shifts nothing.
//
// A time range is its own cache entry. Whole-track is what an audio clip wants
// (the file *is* the clip), but embedded audio is demuxed out of a video
// container, so decoding a whole track there reads the entire video file - tens
// of GB for a master. A video clip only ever shows its own in/out span, so it
// asks for exactly that span and pays only for it.
//
// All public methods are main-thread only: the decode runs on a worker but the
// result is published through the queue's completion (also main thread), so
// cache_/inflight_ need no locking.
class WaveformCache {
public:
    static constexpr int kBinsPerSec = 100; // envelope resolution (10 ms bins)

    struct Peaks {
        std::vector<float> bins;   // max |sample| per bin, 0..1
        double binsPerSec = 0.0;   // == kBinsPerSec once decoded
        double startSec = 0.0;     // source time of bins[0]
        bool hasAudio = false;     // false => file had no audio track / open failed
    };

    // Schedule a background decode for `path` if not already cached or in flight.
    // durSec > 0 asks for just [startSec, startSec + durSec), which is a separate
    // entry from the whole-track one. Cheap and idempotent: safe to call every
    // frame for every visible clip.
    void ensure(const std::string& path, WorkQueue& work,
                double startSec = 0.0, double durSec = 0.0);

    // Decoded envelope for the same (path, range) `ensure` was called with, or
    // nullptr while still pending / unrequested.
    const Peaks* find(const std::string& path,
                      double startSec = 0.0, double durSec = 0.0) const;

    // Drop everything (on project change / media replace). In-flight decodes are
    // abandoned; their completions, if they still run, harmlessly re-cache.
    void clear();

private:
    // Cache key: the path alone for a whole-track envelope, plus the range in
    // bins for a partial one.
    static std::string keyFor(const std::string& path, double startSec, double durSec);

    std::map<std::string, Peaks> cache_;
    std::set<std::string> inflight_;
};

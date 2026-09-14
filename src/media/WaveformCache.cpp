#include "WaveformCache.h"

#include "AudioSource.h"
#include "WorkQueue.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <memory>

namespace {

// Decode `path` into a max-amplitude envelope: the whole track when nBins < 0,
// otherwise only the bins covering [startSec, startSec + nBins/kBinsPerSec).
// Each sample lands in the bin its own source time names (the decoder reports
// where it actually is), so the packet-boundary undershoot of a seek costs
// nothing but a few discarded samples. Cooperative: bails early when `stop` is
// raised (project change / shutdown).
void decodeInto(const std::string& path, double startSec, long long nBins,
                WaveformCache::Peaks& out, const std::atomic<bool>& stop) {
    out.binsPerSec = WaveformCache::kBinsPerSec;
    out.startSec = startSec;
    std::string err;
    auto src = AudioSource::open(path, err);
    if (!src)
        return; // no audio track / open failed => hasAudio stays false
    out.hasAudio = true;

    const int rate = src->sampleRate();
    const int binSamples = std::max(1, rate / WaveformCache::kBinsPerSec);
    if (startSec > 0.0)
        src->seek(startSec);

    // Bins fill in order; a gap (a stream that starts late) stays zero.
    long long curBin = -1;
    float peak = 0.0f;
    auto flush = [&] {
        if (curBin < 0)
            return;
        if ((long long)out.bins.size() < curBin)
            out.bins.resize((size_t)curBin, 0.0f);
        if ((long long)out.bins.size() == curBin)
            out.bins.push_back(std::min(peak, 1.0f));
        else
            out.bins[(size_t)curBin] = std::max(out.bins[(size_t)curBin],
                                                std::min(peak, 1.0f));
    };

    const int kChunk = 8192; // stereo frames per read
    std::vector<float> buf((size_t)kChunk * 2);
    for (;;) {
        if (stop)
            return;
        int got = src->read(buf.data(), kChunk);
        if (got <= 0)
            break;
        // Sample index of this chunk's first sample, counted from startSec: the
        // position after the read, less what the read consumed. A seek lands at
        // or before the request, so this can start negative.
        long long rel = (long long)std::llround((src->positionSeconds() - startSec) * rate)
                        - got;
        bool done = false;
        for (int i = 0; i < got; ++i, ++rel) {
            if (rel < 0)
                continue; // before the requested range: seek undershoot
            long long bin = rel / binSamples;
            if (nBins >= 0 && bin >= nBins) {
                done = true;
                break;
            }
            if (bin != curBin) {
                flush();
                curBin = bin;
                peak = 0.0f;
            }
            float l = std::fabs(buf[(size_t)i * 2]);
            float r = std::fabs(buf[(size_t)i * 2 + 1]);
            peak = std::max(peak, std::max(l, r));
        }
        if (done)
            break;
    }
    flush();
}

} // namespace

std::string WaveformCache::keyFor(const std::string& path, double startSec, double durSec) {
    if (durSec <= 0.0)
        return path; // whole track
    return path + '|' + std::to_string((long long)std::llround(startSec * kBinsPerSec)) +
           '|' + std::to_string((long long)std::llround(durSec * kBinsPerSec));
}

void WaveformCache::ensure(const std::string& path, WorkQueue& work,
                           double startSec, double durSec) {
    if (path.empty())
        return;
    std::string key = keyFor(path, startSec, durSec);
    if (cache_.count(key) || inflight_.count(key))
        return;
    inflight_.insert(key);
    auto peaks = std::make_shared<Peaks>();
    const bool ranged = durSec > 0.0;
    const double start = ranged ? std::max(startSec, 0.0) : 0.0;
    const long long nBins = ranged ? (long long)std::ceil(durSec * kBinsPerSec) : -1;
    work.submit(
        [path, start, nBins, peaks](const std::atomic<bool>& stop) {
            decodeInto(path, start, nBins, *peaks, stop);
        },
        [this, key, peaks]() {
            cache_[key] = std::move(*peaks);
            inflight_.erase(key);
        });
}

const WaveformCache::Peaks* WaveformCache::find(const std::string& path,
                                                double startSec, double durSec) const {
    auto it = cache_.find(keyFor(path, startSec, durSec));
    return it != cache_.end() ? &it->second : nullptr;
}

void WaveformCache::clear() {
    cache_.clear();
    inflight_.clear();
}

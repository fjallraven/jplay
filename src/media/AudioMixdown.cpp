#include "AudioMixdown.h"
#include "AudioSource.h"

#include <algorithm>
#include <cmath>

namespace {

// Linear gain at file time `sec`, interpolated between envelope samples and held
// flat outside it -- the same reading AudioEngine::Channel::gainAt gives.
float gainAt(const AudioMixdown::Segment& s, double sec) {
    if (s.gain.empty() || s.gainHz <= 0.0)
        return 1.0f;
    const double x = (sec - s.gainStartSec) * s.gainHz;
    if (x <= 0.0)
        return s.gain.front();
    const size_t i = (size_t)x;
    if (i + 1 >= s.gain.size())
        return s.gain.back();
    const float f = (float)(x - (double)i);
    return s.gain[i] + (s.gain[i + 1] - s.gain[i]) * f;
}

} // namespace

AudioMixdown::AudioMixdown(std::vector<Segment> segments, double fps, int rate)
    : segs_(std::move(segments)), fps_(fps > 0.0 ? fps : 24.0), rate_(rate > 0 ? rate : 48000) {
    segStart_.reserve(segs_.size());
    segEnd_.reserve(segs_.size());
    for (const Segment& s : segs_) {
        segStart_.push_back(sampleAt(s.tlStart));
        segEnd_.push_back(sampleAt(s.tlEnd));
    }
}

AudioMixdown::~AudioMixdown() = default;

int64_t AudioMixdown::sampleAt(int64_t frame) const {
    return (int64_t)std::llround((double)frame * rate_ / fps_);
}

bool AudioMixdown::hasAudio(const std::vector<Segment>& segs,
                            int64_t startFrame, int64_t endFrame) {
    for (const Segment& s : segs)
        if (s.tlStart <= endFrame && s.tlEnd > startFrame)
            return true;
    return false;
}

void AudioMixdown::begin(int64_t startFrame, int64_t endFrame) {
    startSample_ = sampleAt(startFrame);
    endSample_ = std::max(startSample_, sampleAt(endFrame + 1));
    pos_ = startSample_;
    voices_.assign(segs_.size(), Voice{});
}

// Open segment i's decoder positioned on mix sample `atSample`. The seek lands on
// a packet boundary at or before the wanted time, so the decoder is read one
// sample to learn where it really is and then run forward to the exact sample.
// A seek that lands past it (a stream that starts late) is made up with silence
// instead; the one probe sample is dropped in that case, which is inaudible.
void AudioMixdown::openVoice(size_t i, int64_t atSample) {
    Voice& v = voices_[i];
    v.opened = true;
    std::string err;
    v.src = AudioSource::open(segs_[i].path, err, rate_);
    if (!v.src)
        return;

    double target = segs_[i].srcStartSec + (double)(atSample - segStart_[i]) / rate_;
    if (target < 0.0) {
        v.pad = (int64_t)std::llround(-target * rate_);
        target = 0.0;
    }
    v.src->seek(target);

    float probe[2];
    if (v.src->read(probe, 1) != 1) {
        v.src.reset(); // nothing to decode at or after the seek
        return;
    }
    const double actual = v.src->positionSeconds() - 1.0 / rate_;
    const int64_t ahead = (int64_t)std::llround((target - actual) * rate_);
    if (ahead <= 0) {
        v.pad += 1 - ahead;
        return;
    }
    // Discard the ahead-1 samples still between the probe and the target.
    int64_t skip = ahead - 1;
    tmp_.resize(4096 * 2);
    while (skip > 0) {
        const int n = (int)std::min<int64_t>(skip, 4096);
        const int got = v.src->read(tmp_.data(), n);
        if (got <= 0) {
            v.src.reset();
            return;
        }
        skip -= got;
    }
}

int AudioMixdown::render(float* out, int frames) {
    const int n = (int)std::min<int64_t>(frames, endSample_ - pos_);
    if (n <= 0)
        return 0;
    std::fill(out, out + (size_t)n * 2, 0.0f);
    const int64_t chunkEnd = pos_ + n;

    for (size_t i = 0; i < segs_.size(); ++i) {
        const int64_t s0 = segStart_[i], s1 = segEnd_[i];
        if (s1 <= pos_ || s0 >= chunkEnd)
            continue;
        Voice& v = voices_[i];
        const int64_t a = std::max(s0, pos_);
        const int64_t b = std::min(s1, chunkEnd);
        if (!v.opened)
            openVoice(i, a);
        if (v.src) {
            int at = (int)(a - pos_);
            int want = (int)(b - a);
            const int pad = (int)std::min<int64_t>(v.pad, want);
            v.pad -= pad;
            at += pad;
            want -= pad;
            if (want > 0) {
                tmp_.resize((size_t)want * 2);
                const int got = v.src->read(tmp_.data(), want);
                const Segment& s = segs_[i];
                const bool env = !s.gain.empty() && s.gainHz > 0.0;
                // Chunk frame at+j is mix sample pos_+at+j, i.e. this file time.
                const double sec0 = s.srcStartSec + (double)(pos_ + at - s0) / rate_;
                float* dst = out + (size_t)at * 2;
                for (int j = 0; j < got; ++j) {
                    const float g = env ? gainAt(s, sec0 + (double)j / rate_) : 1.0f;
                    dst[j * 2]     += tmp_[(size_t)j * 2]     * g;
                    dst[j * 2 + 1] += tmp_[(size_t)j * 2 + 1] * g;
                }
            }
        }
        if (s1 <= chunkEnd)
            v.src.reset(); // played out: free the decoder
    }

    for (int k = 0; k < n * 2; ++k)
        out[k] = std::clamp(out[k], -1.0f, 1.0f);
    pos_ = chunkEnd;
    return n;
}

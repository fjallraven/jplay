#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

class AudioSource;

// Offline counterpart of AudioEngine: renders a stretch of the timeline's audio
// mix to interleaved F32 stereo, sample-accurately and as fast as the decoders
// run, for export. The host snapshots what is audible as a list of segments on
// the main thread (see App::exportAudioSegments); the mixdown then only decodes
// and sums, so it can run on the export thread.
//
// Each segment gets its own decoder, opened when the render reaches it and
// dropped once it has played out, so only what overlaps the current chunk is
// ever open. The sum is hard-clamped, as the live mixer's is.
class AudioMixdown {
public:
    // One stretch of one file placed on the timeline: timeline frames
    // [tlStart, tlEnd) play `path` from file time `srcStartSec` on, in real time.
    struct Segment {
        std::string path;
        int64_t tlStart = 0;
        int64_t tlEnd = 0;
        double srcStartSec = 0.0;
        // Volume envelope, same convention as AudioEngine::FeedItem: linear gain
        // sampled every 1/gainHz of FILE time from gainStartSec, the last value
        // holding past the end. Empty (or gainHz == 0) means unity.
        double gainStartSec = 0.0;
        double gainHz = 0.0;
        std::vector<float> gain;
    };

    // `rate` is the mix rate: every source is decoded straight to it, so an export
    // at the output's own rate needs no further resampling.
    AudioMixdown(std::vector<Segment> segments, double fps, int rate = 48000);
    ~AudioMixdown();

    AudioMixdown(const AudioMixdown&) = delete;
    AudioMixdown& operator=(const AudioMixdown&) = delete;

    // True if any of `segs` overlaps timeline frames [startFrame, endFrame].
    static bool hasAudio(const std::vector<Segment>& segs, int64_t startFrame, int64_t endFrame);

    int rate() const { return rate_; }

    // Cue the render to timeline frames [startFrame, endFrame] (inclusive).
    void begin(int64_t startFrame, int64_t endFrame);

    int64_t totalFrames() const { return endSample_ - startSample_; }   // stereo frames
    int64_t renderedFrames() const { return pos_ - startSample_; }

    // Mix the next `frames` stereo frames (frames*2 floats) into `out`. Returns
    // the number written; 0 once the range is done.
    int render(float* out, int frames);

private:
    struct Voice {
        std::shared_ptr<AudioSource> src; // null => not open yet, failed, or played out
        bool opened = false;
        int64_t pad = 0; // silent frames still owed before the source's first sample
    };

    int64_t sampleAt(int64_t frame) const; // timeline frame -> mix sample index
    void openVoice(size_t i, int64_t atSample);

    std::vector<Segment> segs_;
    std::vector<int64_t> segStart_, segEnd_; // segment bounds in mix samples
    std::vector<Voice> voices_;
    double fps_ = 24.0;
    int rate_ = 48000;

    int64_t startSample_ = 0;
    int64_t endSample_ = 0;
    int64_t pos_ = 0;
    std::vector<float> tmp_;
};

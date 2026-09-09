#pragma once

#include <memory>
#include <string>
#include <vector>
#include <cstdint>

extern "C" {
struct AVFormatContext;
struct AVCodecContext;
struct AVFrame;
struct AVPacket;
struct SwrContext;
}

// FFmpeg-backed audio reader for a single media file. Opens its OWN format
// context (independent of VideoSource, so the two never contend on one demuxer)
// and decodes the best audio stream, resampling everything to interleaved F32
// stereo — at the source's native sample rate by default, or at `outRate` when
// given (the mixer opens every source at the device rate so channels sum
// directly). Streaming access, not indexed: seek() to a source time, then
// read() sample chunks until EOF.
class AudioSource {
public:
    static std::shared_ptr<AudioSource> open(const std::string& path, std::string& err,
                                             int outRate = 0); // 0 = native rate
    ~AudioSource();

    int sampleRate() const { return outRate_; }
    int channels() const { return 2; } // always down/up-mixed to stereo

    // Container duration in seconds (0 if unknown).
    double durationSeconds() const { return durationSec_; }

    // Source time (seconds from the stream start) of the next sample read() will
    // return. Taken from the decoded frame's own timestamp, so it reports where
    // the decoder actually is after a seek - which lands on a packet boundary at
    // or before the requested time, never exactly on it.
    double positionSeconds() const {
        return pendingBaseSec_ + (double)(pendingPos_ / 2) / (double)outRate_;
    }

    // Re-cue to `seconds` from the start of the file (keyframe-accurate). Drops
    // any buffered output.
    void seek(double seconds);

    // Fill up to `frames` stereo frames (frames*2 floats) into `out`. Returns the
    // number of stereo frames written; 0 once the stream is exhausted.
    int read(float* out, int frames);

private:
    AudioSource() = default;
    bool openInternal(const std::string& path, std::string& err, int outRate);
    bool decodeMore(); // decode one packet's worth into pending_; false at EOF/error
    void close();

    std::string path_;
    AVFormatContext* fmt_ = nullptr;
    AVCodecContext* codec_ = nullptr;
    SwrContext* swr_ = nullptr;
    AVFrame* frm_ = nullptr;
    AVPacket* pkt_ = nullptr;
    int streamIdx_ = -1;
    double tb_ = 0.0;      // stream time_base as double
    int64_t startPts_ = 0;
    int outRate_ = 48000;
    double durationSec_ = 0.0;
    bool eof_ = false;

    std::vector<float> pending_; // decoded interleaved stereo float, not yet read
    size_t pendingPos_ = 0;      // read cursor (in floats) into pending_
    double pendingBaseSec_ = 0.0; // source time of pending_[0]
};

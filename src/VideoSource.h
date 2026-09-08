#pragma once

#include "MediaSource.h"

#include <mutex>

extern "C" {
struct AVFormatContext;
struct AVCodecContext;
struct AVFrame;
struct AVPacket;
struct SwsContext;
}

// FFmpeg-backed video file source. Decodes on demand with keyframe seeking and
// sequential decode-ahead; a single internal decoder is serialized by a mutex.
class VideoSource : public MediaSource {
public:
    static std::shared_ptr<VideoSource> open(const std::string& path, std::string& err);
    ~VideoSource() override;

    int64_t frameCount() const override { return nFrames_; }
    double fps() const override { return fps_; }
    int width() const override { return width_; }
    int height() const override { return height_; }
    FramePtr readFrame(int64_t index) override;
    const std::string& path() const override { return path_; }
    SourceColorTags colorTags() const override { return colorTags_; }
    std::vector<InfoField> describe() const override;

private:
    VideoSource() = default;
    bool openInternal(const std::string& path, std::string& err);
    bool seekTo(int64_t index);
    bool decodeNext(); // fills frm_, returns false on EOF/error
    int64_t ptsToIndex(const AVFrame* f) const;
    FramePtr convert(const AVFrame* f);
    void close();

    std::string path_;
    std::mutex mtx_;

    AVFormatContext* fmt_ = nullptr;
    AVCodecContext* codec_ = nullptr;
    SwsContext* sws_ = nullptr;

    // Colour conversion state for sws_. swscale defaults to BT.601 coefficients and
    // limited range for every stream, which is wrong for anything HD or wider, so
    // convert() overrides them from the stream's own tags. That override re-inits
    // the context's YUV tables, so it is applied only when the context is (re)built
    // rather than on every frame; these track what the live context was built for.
    SwsContext* swsConfigured_ = nullptr;
    int swsSrcFmt_ = -1, swsW_ = 0, swsH_ = 0;
    AVFrame* frm_ = nullptr;
    AVFrame* keep_ = nullptr;
    AVPacket* pkt_ = nullptr;
    int streamIdx_ = -1;

    int64_t nFrames_ = 0;
    int width_ = 0, height_ = 0;
    double fps_ = 24.0;

    // Descriptive metadata captured once at open time (for the info overlay).
    std::string codecName_;
    std::string pixFmtName_;
    int bitDepth_ = 0;
    // Stream colour tags, as FFmpeg AVCol* enum values (int-typed so this header
    // needs no FFmpeg includes). Resolved through the unspecified-tag fallbacks in
    // convert(); colorTags_ is the name form handed to the colour management.
    int avColorSpace_ = 2 /*AVCOL_SPC_UNSPECIFIED*/;
    int avColorRange_ = 0 /*AVCOL_RANGE_UNSPECIFIED*/;
    SourceColorTags colorTags_;
    bool hasAudio_ = false;
    std::string audioCodec_;
    int audioChannels_ = 0;
    int audioSampleRate_ = 0;
    double tb_ = 0.0; // stream time_base as double
    int64_t startPts_ = 0;
    int64_t nextIndex_ = -1; // index the decoder will produce next, -1 = unknown (forces seek)

    // Retired rgba buffers from released Frames, reused by convert() instead of a
    // fresh allocation on every decoded frame (frame size is typically constant
    // across a clip). Held via shared_ptr so a Frame's release deleter can return
    // its buffer here even after this VideoSource itself has been destroyed
    // (the Frame may outlive it, cached elsewhere).
    struct BufferPool {
        std::mutex mtx;
        std::vector<std::vector<uint8_t>> buffers;
        std::vector<std::vector<uint16_t>> buffers16; // only used by deep sources
    };
    std::shared_ptr<BufferPool> pool_ = std::make_shared<BufferPool>();
};

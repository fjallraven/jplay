#include "VideoSource.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libavutil/pixdesc.h>
#include <libswscale/swscale.h>
}

#include <cmath>

// If a request is at most this far ahead of the decoder position we decode
// forward instead of seeking; keyframe seeks are usually more expensive.
static const int64_t kDecodeAheadLimit = 64;

// AVColorSpace -> the SWS_CS_* coefficient table id. The two enumerations coincide
// for most values, but not all — the 601 coefficients live at 5, where AVColorSpace
// has BT470BG and puts SMPTE170M at 6 — so the mapping is spelled out rather than
// passed through. Anything unrecognised falls back to swscale's own default.
static int swsColorspace(int avcs) {
    switch (avcs) {
    case AVCOL_SPC_BT709:     return SWS_CS_ITU709;
    case AVCOL_SPC_FCC:       return SWS_CS_FCC;
    case AVCOL_SPC_BT470BG:   return SWS_CS_ITU601;
    case AVCOL_SPC_SMPTE170M: return SWS_CS_SMPTE170M;
    case AVCOL_SPC_SMPTE240M: return SWS_CS_SMPTE240M;
    case AVCOL_SPC_BT2020_NCL:
    case AVCOL_SPC_BT2020_CL: return SWS_CS_BT2020;
    default:                  return SWS_CS_DEFAULT;
    }
}

// True for a tag value that carries no information (the container left it out, or
// it names a reserved code point). AVCOL_*_UNSPECIFIED and the reserved entries
// share the value 2 across all three tag enumerations.
static bool tagUnset(int v) {
    return v == 2 /* UNSPECIFIED */ || v == 3 /* RESERVED */ || v == 0 /* RESERVED0 */;
}

std::shared_ptr<VideoSource> VideoSource::open(const std::string& path, std::string& err) {
    auto src = std::shared_ptr<VideoSource>(new VideoSource());
    if (!src->openInternal(path, err))
        return nullptr;
    return src;
}

VideoSource::~VideoSource() {
    close();
}

void VideoSource::close() {
    // swsConfigured_ is only ever compared, never dereferenced, but a fresh context
    // allocated at the freed address would otherwise read as already configured.
    if (sws_) { sws_freeContext(sws_); sws_ = nullptr; swsConfigured_ = nullptr; }
    if (frm_) av_frame_free(&frm_);
    if (keep_) av_frame_free(&keep_);
    if (pkt_) av_packet_free(&pkt_);
    if (codec_) avcodec_free_context(&codec_);
    if (fmt_) avformat_close_input(&fmt_);
}

bool VideoSource::openInternal(const std::string& path, std::string& err) {
    path_ = path;
    if (avformat_open_input(&fmt_, path.c_str(), nullptr, nullptr) < 0) {
        err = "cannot open file";
        return false;
    }
    if (avformat_find_stream_info(fmt_, nullptr) < 0) {
        err = "cannot read stream info";
        return false;
    }
    const AVCodec* dec = nullptr;
    streamIdx_ = av_find_best_stream(fmt_, AVMEDIA_TYPE_VIDEO, -1, -1, &dec, 0);
    if (streamIdx_ < 0 || !dec) {
        err = "no video stream";
        return false;
    }
    AVStream* st = fmt_->streams[streamIdx_];

    codec_ = avcodec_alloc_context3(dec);
    if (!codec_ || avcodec_parameters_to_context(codec_, st->codecpar) < 0) {
        err = "decoder setup failed";
        return false;
    }
    codec_->thread_count = 0; // auto
    if (avcodec_open2(codec_, dec, nullptr) < 0) {
        err = "cannot open decoder";
        return false;
    }

    width_ = codec_->width;
    height_ = codec_->height;
    tb_ = av_q2d(st->time_base);
    startPts_ = st->start_time == AV_NOPTS_VALUE ? 0 : st->start_time;

    AVRational fr = av_guess_frame_rate(fmt_, st, nullptr);
    fps_ = (fr.num > 0 && fr.den > 0) ? av_q2d(fr) : 24.0;

    nFrames_ = st->nb_frames;
    if (nFrames_ <= 0 && st->duration > 0)
        nFrames_ = (int64_t)llround(st->duration * tb_ * fps_);
    if (nFrames_ <= 0 && fmt_->duration > 0)
        nFrames_ = (int64_t)llround(fmt_->duration / (double)AV_TIME_BASE * fps_);
    if (nFrames_ <= 0)
        nFrames_ = 1;

    // Descriptive metadata for the info overlay.
    codecName_ = dec->long_name ? dec->long_name : (dec->name ? dec->name : "");
    if (const AVPixFmtDescriptor* pd = av_pix_fmt_desc_get(codec_->pix_fmt)) {
        pixFmtName_ = pd->name;
        bitDepth_ = pd->comp[0].depth; // bits per component
    }
    // Colour tags. Two separate resolutions come out of these:
    //
    //  - the YUV->RGB matrix and input range for swscale, which must always be
    //    decided; an untagged stream takes the resolution heuristic every decoder
    //    uses (SD is 601, anything larger is 709) and an untagged range is limited
    //    for YUV, full for RGB.
    //  - colorTags_, the primaries/transfer names the colour management picks an
    //    OCIO input colour space from. Those stay *empty* when the container said
    //    nothing, so an untagged file falls through to the config's file rules
    //    rather than being asserted to be Rec.709 on no evidence.
    {
        const AVPixFmtDescriptor* pd = av_pix_fmt_desc_get(codec_->pix_fmt);
        const bool isRgb = pd && (pd->flags & AV_PIX_FMT_FLAG_RGB);

        avColorSpace_ = codec_->colorspace;
        if (tagUnset(avColorSpace_))
            avColorSpace_ = isRgb          ? AVCOL_SPC_RGB
                          : height_ <= 576 ? AVCOL_SPC_SMPTE170M
                                           : AVCOL_SPC_BT709;

        avColorRange_ = codec_->color_range;
        if (avColorRange_ == AVCOL_RANGE_UNSPECIFIED)
            avColorRange_ = isRgb ? AVCOL_RANGE_JPEG : AVCOL_RANGE_MPEG;

        if (!tagUnset(codec_->color_primaries))
            if (const char* n = av_color_primaries_name((AVColorPrimaries)codec_->color_primaries))
                colorTags_.primaries = n;
        if (!tagUnset(codec_->color_trc))
            if (const char* n = av_color_transfer_name((AVColorTransferCharacteristic)codec_->color_trc))
                colorTags_.transfer = n;
        colorTags_.fullRange = (avColorRange_ == AVCOL_RANGE_JPEG);
        colorTags_.video = true; // a container, whatever it turned out to declare
    }

    int audioIdx = av_find_best_stream(fmt_, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
    if (audioIdx >= 0) {
        const AVCodecParameters* ap = fmt_->streams[audioIdx]->codecpar;
        hasAudio_ = true;
        audioCodec_ = avcodec_get_name(ap->codec_id);
        audioChannels_ = ap->ch_layout.nb_channels;
        audioSampleRate_ = ap->sample_rate;
    }

    frm_ = av_frame_alloc();
    keep_ = av_frame_alloc();
    pkt_ = av_packet_alloc();
    if (!frm_ || !keep_ || !pkt_) {
        err = "out of memory";
        return false;
    }
    return true;
}

int64_t VideoSource::ptsToIndex(const AVFrame* f) const {
    int64_t pts = f->best_effort_timestamp;
    if (pts == AV_NOPTS_VALUE)
        pts = f->pts;
    if (pts == AV_NOPTS_VALUE)
        return nextIndex_ >= 0 ? nextIndex_ : 0;
    return (int64_t)llround((pts - startPts_) * tb_ * fps_);
}

bool VideoSource::seekTo(int64_t index) {
    int64_t ts = startPts_;
    if (tb_ > 0.0 && fps_ > 0.0)
        ts = startPts_ + (int64_t)llround(index / (fps_ * tb_));
    if (av_seek_frame(fmt_, streamIdx_, ts, AVSEEK_FLAG_BACKWARD) < 0) {
        if (av_seek_frame(fmt_, streamIdx_, startPts_, AVSEEK_FLAG_BACKWARD) < 0)
            return false;
    }
    avcodec_flush_buffers(codec_);
    return true;
}

bool VideoSource::decodeNext() {
    for (;;) {
        int r = avcodec_receive_frame(codec_, frm_);
        if (r == 0)
            return true;
        if (r == AVERROR(EAGAIN)) {
            for (;;) {
                int rr = av_read_frame(fmt_, pkt_);
                if (rr < 0) {
                    avcodec_send_packet(codec_, nullptr); // start draining
                    break;
                }
                if (pkt_->stream_index == streamIdx_) {
                    avcodec_send_packet(codec_, pkt_);
                    av_packet_unref(pkt_);
                    break;
                }
                av_packet_unref(pkt_);
            }
            continue;
        }
        return false; // AVERROR_EOF or real error
    }
}

FramePtr VideoSource::convert(const AVFrame* f) {
    if (!f || f->width <= 0 || f->height <= 0)
        return nullptr;
    const AVPixelFormat srcFmt = (AVPixelFormat)f->format;
    // A stream deeper than 8 bits is scaled to 16-bit RGBA and the 8-bit buffer
    // derived from that, rather than the other way round: a display transform
    // applied to 8-bit log or 10-bit material bands badly in the shadows, and the
    // consumers that want 8 bits (scopes, filmstrip, thumbnails) do not care.
    const bool deep = bitDepth_ > 8;
    const AVPixelFormat dstFmt = deep ? AV_PIX_FMT_RGBA64LE : AV_PIX_FMT_RGBA;
    sws_ = sws_getCachedContext(sws_, f->width, f->height, srcFmt,
                                f->width, f->height, dstFmt,
                                SWS_BILINEAR, nullptr, nullptr, nullptr);
    if (!sws_)
        return nullptr;

    // Left alone, swscale converts every stream with BT.601 coefficients and a
    // limited-range input assumption, which is wrong for anything HD or wider.
    // Push the stream's own matrix and range in instead. This re-initialises the
    // context's YUV tables, so it runs only when the context is (re)built — which
    // sws_getCachedContext does whenever the geometry or source format changes,
    // and which may hand back a different pointer. dstRange is passed as 0 because
    // swscale zeroes it for a non-YUV destination anyway; RGB output is full range.
    if (sws_ != swsConfigured_ || srcFmt != swsSrcFmt_ ||
        f->width != swsW_ || f->height != swsH_) {
        sws_setColorspaceDetails(sws_,
                                 sws_getCoefficients(swsColorspace(avColorSpace_)),
                                 avColorRange_ == AVCOL_RANGE_JPEG ? 1 : 0,
                                 sws_getCoefficients(SWS_CS_DEFAULT), 0,
                                 0, 1 << 16, 1 << 16);
        swsConfigured_ = sws_;
        swsSrcFmt_ = srcFmt;
        swsW_ = f->width;
        swsH_ = f->height;
    }

    // Reuse a retired rgba buffer instead of allocating fresh every frame.
    std::vector<uint8_t> buf;
    std::vector<uint16_t> buf16;
    {
        std::lock_guard<std::mutex> lk(pool_->mtx);
        if (!pool_->buffers.empty()) {
            buf = std::move(pool_->buffers.back());
            pool_->buffers.pop_back();
        }
        if (deep && !pool_->buffers16.empty()) {
            buf16 = std::move(pool_->buffers16.back());
            pool_->buffers16.pop_back();
        }
    }

    auto* raw = new Frame();
    raw->width = f->width;
    raw->height = f->height;
    const size_t n = (size_t)f->width * f->height;
    const size_t packed = n * 4;
    buf.reserve(packed + (size_t)f->width * 4 + 64);
    buf.resize(packed);
    raw->rgba = std::move(buf);

    if (deep) {
        buf16.resize(packed);
        raw->rgba16 = std::move(buf16);
        uint8_t* dst[4] = { (uint8_t*)raw->rgba16.data(), nullptr, nullptr, nullptr };
        int dstStride[4] = { f->width * 8, 0, 0, 0 };
        sws_scale(sws_, f->data, f->linesize, 0, f->height, dst, dstStride);
        // 8-bit companion, by truncation: 65535 >> 8 is 255, so the endpoints are
        // exact. No dither — this buffer only feeds the scopes and the small
        // downscaled previews, never the display transform.
        const uint16_t* s16 = raw->rgba16.data();
        uint8_t* s8 = raw->rgba.data();
        for (size_t i = 0; i < packed; ++i)
            s8[i] = (uint8_t)(s16[i] >> 8);
    } else {
        uint8_t* dst[4] = { raw->rgba.data(), nullptr, nullptr, nullptr };
        int dstStride[4] = { f->width * 4, 0, 0, 0 };
        sws_scale(sws_, f->data, f->linesize, 0, f->height, dst, dstStride);
    }

    std::shared_ptr<BufferPool> pool = pool_;
    return std::shared_ptr<Frame>(raw, [pool](Frame* p) {
        {
            std::lock_guard<std::mutex> lk(pool->mtx);
            if (pool->buffers.size() < 4)
                pool->buffers.push_back(std::move(p->rgba));
            if (!p->rgba16.empty() && pool->buffers16.size() < 4)
                pool->buffers16.push_back(std::move(p->rgba16));
        }
        delete p;
    });
}

FramePtr VideoSource::readFrame(int64_t index) {
    std::lock_guard<std::mutex> lock(mtx_);
    if (!fmt_)
        return nullptr;
    if (index < 0) index = 0;
    if (index >= nFrames_) index = nFrames_ - 1;

    if (nextIndex_ < 0 || index < nextIndex_ || index > nextIndex_ + kDecodeAheadLimit) {
        if (!seekTo(index)) {
            nextIndex_ = -1;
            return nullptr;
        }
        nextIndex_ = -1;
    }

    bool any = false;
    int guard = 0;
    for (;;) {
        if (!decodeNext()) {
            nextIndex_ = -1; // EOF/error: force a seek on the next request
            break;
        }
        av_frame_unref(keep_);
        av_frame_ref(keep_, frm_);
        any = true;
        int64_t idx = ptsToIndex(keep_);
        nextIndex_ = idx + 1;
        if (idx >= index || ++guard > 4000)
            break;
    }
    return any ? convert(keep_) : nullptr;
}

std::vector<InfoField> VideoSource::describe() const {
    std::vector<InfoField> f;
    if (!codecName_.empty())
        f.push_back({ "Codec", codecName_ });
    if (!pixFmtName_.empty()) {
        std::string v = pixFmtName_;
        if (bitDepth_ > 0)
            v += " (" + std::to_string(bitDepth_) + "-bit)";
        f.push_back({ "Pixel format", v });
    }
    // Colour tags as the container declared them; "unspecified" is worth showing,
    // since it is what sends a media to the config's file rules for its input space.
    {
        std::string enc = colorTags_.primaries.empty() ? "unspecified" : colorTags_.primaries;
        enc += " / ";
        enc += colorTags_.transfer.empty() ? "unspecified" : colorTags_.transfer;
        enc += colorTags_.fullRange ? " / full" : " / limited";
        f.push_back({ "Color encoding", enc });
        if (const char* m = av_color_space_name((AVColorSpace)avColorSpace_))
            f.push_back({ "Matrix", m });
    }
    if (hasAudio_) {
        std::string a = audioCodec_;
        if (audioChannels_ > 0)
            a += ", " + std::to_string(audioChannels_) + " ch";
        if (audioSampleRate_ > 0)
            a += ", " + std::to_string(audioSampleRate_) + " Hz";
        f.push_back({ "Audio", a });
    } else {
        f.push_back({ "Audio", "none" });
    }
    return f;
}

#include "AudioSource.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libswresample/swresample.h>
}

#include <algorithm>
#include <cmath>
#include <cstring>

std::shared_ptr<AudioSource> AudioSource::open(const std::string& path, std::string& err,
                                               int outRate) {
    auto s = std::shared_ptr<AudioSource>(new AudioSource());
    if (!s->openInternal(path, err, outRate))
        return nullptr;
    return s;
}

AudioSource::~AudioSource() {
    close();
}

void AudioSource::close() {
    if (swr_) swr_free(&swr_);
    if (frm_) av_frame_free(&frm_);
    if (pkt_) av_packet_free(&pkt_);
    if (codec_) avcodec_free_context(&codec_);
    if (fmt_) avformat_close_input(&fmt_);
}

bool AudioSource::openInternal(const std::string& path, std::string& err, int outRate) {
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
    streamIdx_ = av_find_best_stream(fmt_, AVMEDIA_TYPE_AUDIO, -1, -1, &dec, 0);
    if (streamIdx_ < 0 || !dec) {
        err = "no audio stream";
        return false;
    }
    AVStream* st = fmt_->streams[streamIdx_];

    codec_ = avcodec_alloc_context3(dec);
    if (!codec_ || avcodec_parameters_to_context(codec_, st->codecpar) < 0) {
        err = "decoder setup failed";
        return false;
    }
    if (avcodec_open2(codec_, dec, nullptr) < 0) {
        err = "cannot open decoder";
        return false;
    }

    tb_ = av_q2d(st->time_base);
    startPts_ = st->start_time == AV_NOPTS_VALUE ? 0 : st->start_time;
    outRate_ = outRate > 0 ? outRate
                           : (codec_->sample_rate > 0 ? codec_->sample_rate : 48000);

    // Duration: prefer the stream's own, fall back to the container's.
    if (st->duration != AV_NOPTS_VALUE && tb_ > 0.0)
        durationSec_ = (double)st->duration * tb_;
    else if (fmt_->duration != AV_NOPTS_VALUE)
        durationSec_ = (double)fmt_->duration / AV_TIME_BASE;

    // Resampler: whatever the source is -> interleaved F32 stereo at outRate_.
    AVChannelLayout outLayout;
    av_channel_layout_default(&outLayout, 2); // stereo
    int rc = swr_alloc_set_opts2(&swr_, &outLayout, AV_SAMPLE_FMT_FLT, outRate_,
                                 &codec_->ch_layout, codec_->sample_fmt, codec_->sample_rate,
                                 0, nullptr);
    av_channel_layout_uninit(&outLayout);
    if (rc < 0 || swr_init(swr_) < 0) {
        err = "resampler setup failed";
        return false;
    }

    frm_ = av_frame_alloc();
    pkt_ = av_packet_alloc();
    if (!frm_ || !pkt_) {
        err = "out of memory";
        return false;
    }
    return true;
}

bool AudioSource::decodeMore() {
    for (;;) {
        int r = avcodec_receive_frame(codec_, frm_);
        if (r == 0) {
            int cap = swr_get_out_samples(swr_, frm_->nb_samples); // upper bound
            size_t base = pending_.size();
            // Anchor the reported position on this frame's timestamp whenever the
            // buffer was empty (which read() guarantees before every refill).
            if (base == 0 && frm_->best_effort_timestamp != AV_NOPTS_VALUE && tb_ > 0.0)
                pendingBaseSec_ = (double)(frm_->best_effort_timestamp - startPts_) * tb_;
            pending_.resize(base + (size_t)std::max(cap, 0) * 2);
            uint8_t* outPtr = reinterpret_cast<uint8_t*>(pending_.data() + base);
            int got = swr_convert(swr_, &outPtr, cap,
                                  (const uint8_t**)frm_->extended_data, frm_->nb_samples);
            pending_.resize(base + (size_t)std::max(got, 0) * 2);
            av_frame_unref(frm_);
            return true;
        }
        if (r == AVERROR(EAGAIN)) {
            for (;;) {
                int rr = av_read_frame(fmt_, pkt_);
                if (rr < 0) {
                    avcodec_send_packet(codec_, nullptr); // start draining
                    eof_ = true;
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

int AudioSource::read(float* out, int frames) {
    int written = 0;
    while (written < frames) {
        if (pendingPos_ >= pending_.size()) {
            // Keep the position running for streams whose frames carry no
            // timestamp; a frame that has one overwrites this in decodeMore().
            pendingBaseSec_ += (double)(pending_.size() / 2) / (double)outRate_;
            pending_.clear();
            pendingPos_ = 0;
            if (!decodeMore())
                break; // EOF / error
            continue;
        }
        int availFrames = (int)((pending_.size() - pendingPos_) / 2);
        if (availFrames <= 0) {
            pendingPos_ = pending_.size(); // decoded frame yielded nothing; refill
            continue;
        }
        int n = std::min(frames - written, availFrames);
        std::memcpy(out + (size_t)written * 2, pending_.data() + pendingPos_,
                    (size_t)n * 2 * sizeof(float));
        pendingPos_ += (size_t)n * 2;
        written += n;
    }
    return written;
}

void AudioSource::seek(double seconds) {
    if (!fmt_)
        return;
    int64_t ts = startPts_;
    if (tb_ > 0.0)
        ts = startPts_ + (int64_t)llround(seconds / tb_);
    av_seek_frame(fmt_, streamIdx_, ts, AVSEEK_FLAG_BACKWARD);
    avcodec_flush_buffers(codec_);
    pending_.clear();
    pendingPos_ = 0;
    pendingBaseSec_ = seconds; // provisional; the first decoded frame's pts wins
    eof_ = false;
}

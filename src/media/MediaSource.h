#pragma once

#include "FrameAlloc.h"

#include <Imath/half.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

// A decoded image.
//
// Three pixel buffers, of which the 8-bit one is the common denominator and the
// other two are higher-precision variants of it that the colour-managed display
// path prefers when they exist. The scopes, the filmstrip, the thumbnail tiles
// and the export writer stay unaware of source bit depth by reading rgba8().
//
// rgba8():   width * height * 4 uint8_t. Display-referred, in the source's own
//            encoding (an sRGB-ish LUT of the linear values for EXR). Video and
//            still sources fill it as they decode; for an EXR sequence, which
//            carries linearRgb, it is derived from that buffer and is therefore
//            built ON FIRST USE rather than at decode time. Building it is 16 ms
//            for a 4K frame and a third again of the frame's memory, and the
//            colour-managed display path never reads it -- it feeds linearRgb
//            straight to the GPU transform -- so a playing cache holds neither
//            the cost nor the bytes. Decoders build it up front anyway while
//            decodeRgba8() is set, which is what the display path asks for when
//            colour management is off and every frame really does need it.
// rgba16:    width * height * 4 uint16_t, populated by VideoSource for streams
//            deeper than 8 bits. Same encoding as rgba8(), which is derived from it.
// linearRgb: width * height * 3 Imath::half, scene-linear RGB; populated by
//            ExrSequenceSource so the display path can apply an OCIO transform
//            instead of the baked sRGB LUT. Empty for video frames.
// pixelAspect: pixel width / height (see MediaSource::pixelAspect). Carried on
//            the frame so a consumer holding one knows its shape without the
//            source; 1 for the square-pixel case, which is everything but an
//            anamorphic EXR.
//
// A frame handed out by the cache is const and shared between threads, so the
// on-demand build is guarded and rgba8() is safe to call from any of them.
struct Frame {
    // The scene-linear buffer's type: a vector whose allocator default-initialises
    // (resize() does not zero-fill 50 MB the decoder is about to overwrite) and
    // recycles freed blocks through a process-wide pool (see FrameAlloc.h).
    using HalfBuffer = std::vector<Imath::half, FrameAlloc<Imath::half>>;

    int width = 0;
    int height = 0;
    float pixelAspect = 1.0f;
    std::vector<uint16_t> rgba16;    // width * height * 4 (sources deeper than 8 bit)
    HalfBuffer            linearRgb; // width * height * 3 (EXR only)

    Frame() = default;
    ~Frame() = default;
    // The guard is not part of the value: a copy carries the buffers, and is
    // "already built" exactly when the frame it was copied from was.
    Frame(const Frame& o);
    Frame& operator=(const Frame& o);

    // The display-referred 8-bit buffer, built from linearRgb if it does not
    // exist yet. Empty only for a degenerate frame that carries neither.
    const std::vector<uint8_t>& rgba8() const;
    // Whether rgba8() would return without building anything. A consumer that
    // has a higher-precision path available should prefer it over forcing a
    // build; one that samples a few pixels (a scope, a probe) should read
    // linearRgb directly rather than materialising 34 MB to look at 200k of it.
    bool hasRgba8() const;

    // Decoder-side access: fill it in place, hand one over, or take the buffer
    // back for a pool. All three mark the buffer built.
    std::vector<uint8_t>& rgba8Mut();
    void setRgba8(std::vector<uint8_t> px);
    std::vector<uint8_t> releaseRgba8();
    // Build it now, filling `recycled` (a retired buffer from the decoder's pool)
    // rather than a fresh allocation. A no-op if it is already built.
    void buildRgba8(std::vector<uint8_t> recycled = {});

    // What this frame currently occupies. It grows if rgba8() later builds the
    // 8-bit buffer, so a cache budgeting by it records the value it saw rather
    // than re-reading it (see FrameCache::Entry::bytes).
    size_t bytes() const;

private:
    void buildLocked_() const; // caller holds rgbaMtx_

    mutable std::vector<uint8_t> rgba_;
    mutable std::atomic<bool> rgbaReady_{ false };
    mutable std::mutex rgbaMtx_;
};

// Whether decoders should build the 8-bit buffer as they decode instead of
// leaving it to the first rgba8(). Set by the player for the states where every
// displayed frame needs it anyway -- colour management off -- so that build
// happens on a decode worker, as it always did, rather than on the UI thread in
// the middle of playback. Off by default; reading it is cheap enough for a
// per-frame decode path.
void setDecodeRgba8(bool on);
bool decodeRgba8();

// Where one frame read spent its time, for the Playback Timings panel. Filled by
// a source that can tell (MediaSource::readFrameTimed); `valid` stays false for
// one that cannot. The cache workers time every read, panel open or not: it is a
// handful of clock readings against milliseconds of IO, and a frame read before
// the panel was opened then still has its figures when it is shown.
struct ReadTiming {
    double waitIoMs = 0.0; // opening the file and reading its header: the wait for first data
    double readIoMs = 0.0; // reading the pixel data
    double exrMs = 0.0;    // turning it into the frame's buffer: interleave or decode, 8-bit build
    // Whether readIoMs was measured on its own. OpenEXR's readPixels reads and
    // decodes in one call, so for a file it handles readIoMs is 0 and exrMs holds both.
    bool ioSplit = false;
    bool valid = false;
};

// Milliseconds on a monotonic clock, for the ReadTiming stamps.
inline double readTimingNowMs() {
    using namespace std::chrono;
    return duration<double, std::milli>(steady_clock::now().time_since_epoch()).count();
}

// One scene-linear half (by bit pattern) through the same baked sRGB LUT rgba8()
// is built with. For a consumer that samples a frame rather than reading all of
// it -- a scope over 200k of 8.8M pixels -- this gives the identical code value
// per sample without materialising the whole 8-bit buffer to find it.
uint8_t srgb8FromHalfBits(unsigned short bits);

using FramePtr = std::shared_ptr<const Frame>;

// The colour encoding a source declares for its own pixels, as canonical FFmpeg
// tag names ("bt709", "smpte2084", "iec61966-2-1", "arib-std-b67", ...) so this
// header stays FFmpeg-free. Empty strings mean the source carries no such tag —
// either it has no notion of one (EXR, PNG) or the container left it unspecified.
// Used to pick an OCIO input colour space for a media that has no explicit one.
struct SourceColorTags {
    std::string primaries; // colour primaries tag
    std::string transfer;  // transfer characteristics tag
    bool fullRange = false;
    // True for a source whose pixels are scene-referred float (EXR). Such a source
    // has no encoding tags to speak of; what it needs is the config's scene_linear
    // role rather than any of the display-referred fallbacks.
    bool sceneLinear = false;
    // True for a source decoded from a video container (a quicktime, an MP4) rather
    // than from an image sequence. Studio configs conventionally name the space a
    // show publishes its videos in with a `default_video` role; this flag is what
    // says that role applies. See OcioManager::colorSpaceForMedia.
    bool video = false;
};

// One labelled metadata line for the info overlay (e.g. {"Codec", "H.264"}).
struct InfoField {
    std::string key;
    std::string value;
};

// Which of a multi-channel source's channels feed the displayed R, G and B: an
// EXR's AOV layers, views and single channels. `part` < 0 is the source's own
// pick (the beauty layer it chose at open), which every source starts on.
// Otherwise `rgb` holds full channel names within that part; one name in all
// three slots shows that channel as grayscale, and "" leaves a slot black.
struct ChannelSelection {
    int part = -1;
    std::string rgb[3];
    bool isDefault() const { return part < 0; }
    bool operator==(const ChannelSelection& o) const {
        return part == o.part && rgb[0] == o.rgb[0] && rgb[1] == o.rgb[1] && rgb[2] == o.rgb[2];
    }
    bool operator!=(const ChannelSelection& o) const { return !(*this == o); }
};

// A source of frames (video file or EXR sequence). readFrame() must be safe to
// call from multiple threads (implementations serialize internally as needed).
class MediaSource {
public:
    virtual ~MediaSource() = default;

    virtual int64_t frameCount() const = 0;
    virtual double fps() const = 0; // 0 if the source has no intrinsic rate
    virtual int width() const = 0;  // intrinsic pixel dimensions (0 if unknown)
    virtual int height() const = 0;

    // Pixel aspect ratio: the width of one stored pixel divided by its height.
    // 1 (the default) means square pixels; an anamorphic source stores e.g. 2.0
    // and must be stretched horizontally by that factor to display in its true
    // shape. Constant for the whole source.
    virtual float pixelAspect() const { return 1.0f; }
    virtual FramePtr readFrame(int64_t index) = 0;
    // readFrame(), also reporting where the read spent its time. The default
    // reports nothing (timing.valid stays false); the EXR source fills it in.
    virtual FramePtr readFrameTimed(int64_t index, ReadTiming& /*timing*/) { return readFrame(index); }
    virtual const std::string& path() const = 0;

    // The real frame number of source index 0, used for the clip-frame readout.
    // For an image sequence this is the trailing number of its first file (e.g.
    // 994 for "shot.0994.exr"); video and stills have no numbering, so 0.
    virtual int64_t firstFrameNumber() const { return 0; }

    // The colour encoding the source declares for its pixels (see SourceColorTags).
    // Empty by default: a source with no tags of its own leaves the choice of an
    // OCIO input colour space to the config's file rules.
    virtual SourceColorTags colorTags() const { return {}; }

    // Source-specific detail lines for the info overlay (codec/audio for video;
    // bit depth/channels/layers/views for EXR). Captured at open time; empty by
    // default. Common fields (resolution, frame count, fps) are added by callers.
    virtual std::vector<InfoField> describe() const { return {}; }

    // Switch which channels readFrame() decodes (see ChannelSelection). Frames
    // already read keep what they were read with, so the caller drops them from
    // any cache. Safe against concurrent readFrame() calls: each read uses the
    // selection it started with. Returns false, changing nothing, for a source
    // with no such choice or a selection naming channels it does not have.
    virtual bool setChannelSelection(const ChannelSelection&) { return false; }

    // For an image sequence, its files as scanned at open(), sorted by frame
    // number; lets freshness hashing reuse that set instead of re-scanning the
    // directory. Empty for single-file sources (video).
    virtual const std::vector<std::string>& sequenceFiles() const {
        static const std::vector<std::string> kNone;
        return kNone;
    }
};

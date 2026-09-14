#pragma once

#include <Imath/half.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

// A decoded image.
//
// Three pixel buffers, of which `rgba` is always present and the other two are
// higher-precision variants of it that the colour-managed display path prefers
// when they exist. Keeping the 8-bit buffer unconditionally is what lets the
// scopes, the filmstrip, the thumbnail tiles and the export writer stay unaware
// of source bit depth; they all read `rgba` as they always have.
//
// rgba:      width * height * 4 uint8_t, always present. Display-referred, in the
//            source's own encoding (an sRGB-ish LUT of the linear values for EXR).
// rgba16:    width * height * 4 uint16_t, populated by VideoSource for streams
//            deeper than 8 bits. Same encoding as `rgba`, which is derived from it.
// linearRgb: width * height * 3 Imath::half, scene-linear RGB; populated by
//            ExrSequenceSource so the display path can apply an OCIO transform
//            instead of the baked sRGB LUT. Empty for video frames.
// pixelAspect: pixel width / height (see MediaSource::pixelAspect). Carried on
//            the frame so a consumer holding one knows its shape without the
//            source; 1 for the square-pixel case, which is everything but an
//            anamorphic EXR.
struct Frame {
    int width = 0;
    int height = 0;
    float pixelAspect = 1.0f;
    std::vector<uint8_t>     rgba;      // width * height * 4
    std::vector<uint16_t>    rgba16;    // width * height * 4 (sources deeper than 8 bit)
    std::vector<Imath::half> linearRgb; // width * height * 3 (EXR only)
    size_t bytes() const {
        return rgba.size() + rgba16.size() * sizeof(uint16_t) +
               linearRgb.size() * sizeof(Imath::half);
    }
};

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

    // For an image sequence, its files as scanned at open(), sorted by frame
    // number; lets freshness hashing reuse that set instead of re-scanning the
    // directory. Empty for single-file sources (video).
    virtual const std::vector<std::string>& sequenceFiles() const {
        static const std::vector<std::string> kNone;
        return kNone;
    }
};

#pragma once

#include "MediaSource.h"

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

// What an EXR holds, as read from its first file's headers at open(): every part
// with its channels, for the Clip panel's CHANNELS tab to list and pick
// from (see ChannelSelection). The sequence is assumed homogeneous, as it is for
// the dimensions.
struct ExrLayout {
    struct Channel {
        std::string name;      // full name, e.g. "diffuse.R", "right.Z", "A"
        // The name taken apart: the view it belongs to ("" outside a multi-view
        // file), the layer it sits in with any view component removed ("" = the
        // unprefixed base layer), and its final component ("R", "Z", "x").
        std::string view, layer, base;
        std::string type;      // "half" / "float" / "uint"
        bool half = false;     // type is half (what the fast reader takes)
        bool subsampled = false; // x/y sampling != 1: listed, but not selectable
    };
    struct Part {
        std::string name;      // part name ("" for a single-part file)
        std::string view;      // the part's view attribute (multi-part stereo), or ""
        bool deep = false;     // deep data: listed, not readable here
        bool compressed = false;
        std::vector<Channel> channels; // in file (alphabetical) order
    };
    std::vector<Part> parts;
    // The multiView attribute of a single-part multi-view file, default view
    // first; its other views' channels carry the view name as their first prefix.
    std::vector<std::string> views;
};

// OpenEXR image-sequence source. Constructed from any file of the sequence; the
// sibling frames are collected and sorted by ImageSeq::files. Frames are decoded
// into the EXR *display window*:
// the data window is composited into display-window space and pixels outside
// the data window are black. Loading is stateless per file, so reads from
// multiple threads proceed fully in parallel.
class ExrSequenceSource : public MediaSource {
public:
    static std::shared_ptr<ExrSequenceSource> open(const std::string& anyFrameFile, std::string& err);

    int64_t frameCount() const override { return (int64_t)files_.size(); }
    double fps() const override { return 0.0; } // sequences carry no rate
    int width() const override { return width_; }
    int height() const override { return height_; }
    float pixelAspect() const override { return pixelAspect_; }
    FramePtr readFrame(int64_t index) override;
    const std::string& path() const override { return firstFile_; }
    int64_t firstFrameNumber() const override { return firstFrame_; }
    // Scene-referred float; no encoding tags of its own (see SourceColorTags).
    SourceColorTags colorTags() const override {
        SourceColorTags t;
        t.sceneLinear = true;
        return t;
    }
    std::vector<InfoField> describe() const override { return details_; }
    bool setChannelSelection(const ChannelSelection& sel) override;
    // The file's parts and channels, and the channels the source picked itself
    // (what ChannelSelection's default stands for, spelled out).
    const ExrLayout& layout() const { return layout_; }
    ChannelSelection defaultSelection() const;
    const std::vector<std::string>& sequenceFiles() const override { return files_; }

private:
    ExrSequenceSource() = default;

    std::string firstFile_;          // representative path (serialized in projects)
    int64_t firstFrame_ = 0;         // trailing frame number of firstFile_ (0 if none)
    std::vector<std::string> files_; // one file per frame, sorted by frame number
    int width_ = 0, height_ = 0;     // display-window dimensions of the first frame
    // pixelAspectRatio from the first frame's header (1 = square). A required EXR
    // attribute, so it is always readable; read once because a sequence is assumed
    // homogeneous, exactly as the dimensions above are.
    float pixelAspect_ = 1.0f;
    std::vector<InfoField> details_; // bit depth/channels/layers/views, read once at open

    ExrLayout layout_;

    // How readFrame() gets the picture: which part, which channels, by which
    // route. The default plan is settled from the first file's headers at open();
    // setChannelSelection() swaps in another. Immutable once published and read
    // through an atomic shared_ptr, so each of the parallel readFrame() calls
    // takes one snapshot and a swap mid-read cannot tear it.
    struct ReadPlan {
        // The part of the file that holds the image: multi-part EXRs (one part
        // per layer) do not necessarily keep it in part 0.
        int part = 0;
        // Full channel names feeding R, G, B. A name repeated is a grayscale
        // view of that channel; "" is a black slot.
        std::string rgb[3];
        // The layer name for Imf::RgbaInputFile ("" = unprefixed R/G/B or Y),
        // used when `direct` is not set.
        std::string rgbaLayer;
        // True when the channels are all at full resolution, which readFrame()
        // decodes straight into the frame's linearRgb. Anything else (Y, Y/RY/BY
        // luma-chroma, subsampled channels) is read through Imf::RgbaInputFile,
        // which converts to RGB itself -- the default plan only.
        bool direct = false;
        // Whether the part is compressed at all. An uncompressed read has nothing
        // for OpenEXR's thread pool to do but hand rows between threads, which
        // measured slower than reading on the calling thread (4K: 27 ms alone vs
        // 33 ms pooled), so readFrame() only uses the pool when this is set.
        bool compressed = false;
        // Whether readFrame() may bypass OpenEXR altogether: an uncompressed
        // scanline part whose chosen channels are half has nothing to decode,
        // only rows to interleave (see ExrFast.h). Every read still checks its
        // own file and falls back to OpenEXR for one that differs.
        bool fast = false;
    };
    std::shared_ptr<const ReadPlan> defaultPlan_;
    std::shared_ptr<const ReadPlan> plan_; // std::atomic_load / atomic_store

    // Retired 8-bit buffers from released Frames, reused by readFrame() instead of
    // a fresh allocation every read (frame size is typically constant across a
    // sequence). Reads run fully in parallel across threads (see class doc), so
    // this pool is mutex-protected and held via shared_ptr so a Frame's release
    // deleter can return its buffer here even after this source is destroyed (the
    // Frame may outlive it, cached elsewhere). linearRgb needs no such pool: its
    // allocator recycles through the process-wide FramePool (see FrameAlloc.h).
    struct BufferPool {
        std::mutex mtx;
        std::vector<std::vector<uint8_t>> freeRgba;
    };
    std::shared_ptr<BufferPool> pool_ = std::make_shared<BufferPool>();
};

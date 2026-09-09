#pragma once

#include "MediaSource.h"

#include <cstdint>
#include <mutex>

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

    // The part of the file, and channel-name prefix within it, that holds the
    // image: multi-part EXRs (one part per layer) do not necessarily keep it in
    // part 0. Chosen from the first file's headers at open() and read-only after,
    // so the parallel readFrame() calls can share it.
    int part_ = 0;
    std::string layer_;              // "" = channels named R/G/B (or Y), unprefixed

    // Retired output buffers from released Frames, reused by readFrame() instead
    // of a fresh allocation every read (frame size is typically constant across a
    // sequence). Reads run fully in parallel across threads (see class doc), so
    // this pool is mutex-protected and held via shared_ptr so a Frame's release
    // deleter can return its buffers here even after this source is destroyed
    // (the Frame may outlive it, cached elsewhere).
    struct ExrBuffers {
        std::vector<uint8_t> rgba;
        std::vector<Imath::half> linearRgb;
    };
    struct BufferPool {
        std::mutex mtx;
        std::vector<ExrBuffers> free;
    };
    std::shared_ptr<BufferPool> pool_ = std::make_shared<BufferPool>();
};

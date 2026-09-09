#pragma once

#include "MediaSource.h"

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

// 8-bit sRGB image-sequence source (PNG / JPEG / TIFF), decoded through
// SDL_image. Constructed from any file of the sequence; siblings sharing the
// prefix, extension and a trailing frame number are collected and sorted (see
// ImageSeq::files). Each read is an independent IMG_Load, so reads from multiple
// threads proceed fully in parallel, as they do for EXR.
//
// These formats are display-referred: only Frame::rgba is filled and
// Frame::linearRgb is left empty, which routes them through the same
// already-sRGB display path as video (no OCIO transform, no scene-linear
// tech-check modes). Any alpha channel is dropped and frames are marked opaque.
class StillSequenceSource : public MediaSource {
public:
    static std::shared_ptr<StillSequenceSource> open(const std::string& anyFrameFile, std::string& err);

    int64_t frameCount() const override { return (int64_t)files_.size(); }
    double fps() const override { return 0.0; } // sequences carry no rate
    int width() const override { return width_; }
    int height() const override { return height_; }
    FramePtr readFrame(int64_t index) override;
    const std::string& path() const override { return firstFile_; }
    int64_t firstFrameNumber() const override { return firstFrame_; }
    std::vector<InfoField> describe() const override { return details_; }
    const std::vector<std::string>& sequenceFiles() const override { return files_; }

private:
    StillSequenceSource() = default;

    std::string firstFile_;          // representative path (serialized in projects)
    int64_t firstFrame_ = 0;         // trailing frame number of firstFile_ (0 if none)
    std::vector<std::string> files_; // one file per frame, sorted by frame number
    int width_ = 0, height_ = 0;     // dimensions of the first frame
    std::vector<InfoField> details_; // format/bit depth/colour space, read once at open

    // Retired rgba buffers from released Frames, reused by readFrame() instead of
    // a fresh allocation every read. Held via shared_ptr so a Frame's release
    // deleter can return its buffer even after this source is destroyed (the
    // Frame may outlive it, cached elsewhere). Mirrors ExrSequenceSource.
    struct BufferPool {
        std::mutex mtx;
        std::vector<std::vector<uint8_t>> free;
    };
    std::shared_ptr<BufferPool> pool_ = std::make_shared<BufferPool>();
};

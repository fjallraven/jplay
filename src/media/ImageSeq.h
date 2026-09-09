#pragma once

#include "MediaSource.h"

#include <memory>
#include <string>
#include <vector>

// Numbered image sequences, shared by the two still-image decoders: OpenEXR
// (ExrSequenceSource, scene-linear + OCIO display path) and the 8-bit sRGB
// formats (StillSequenceSource). A "sequence" is the set of
// <prefix><digits><ext> siblings in one directory; an un-numbered file is a
// one-frame sequence.
namespace ImageSeq {

// True for an extension (leading dot, any case) jplay treats as image-sequence
// media: OpenEXR plus the 8-bit sRGB stills.
bool isSequenceExt(const std::string& ext);
bool isSequencePath(const std::string& path);

// True only for OpenEXR, the one format read as scene-linear.
bool isExrPath(const std::string& path);

// Substitute a concrete frame number into a frame-pattern path, zero-padded to
// the pattern's width ("shot.####.exr" + 25 -> "shot.0025.exr", "shot.#.exr" +
// 1041 -> "shot.1041.exr", "shot.%04d.png" + 25 -> "shot.0025.png"). Returns
// `path` unchanged when it carries no pattern. Lets a caller that already knows
// the first frame name a real file without a directory scan.
std::string substituteFrame(const std::string& path, int64_t frame);

// The files belonging to the sequence containing `anyFrameFile`, sorted by frame
// number. `anyFrameFile` may instead be a frame-pattern path ("shot.####.exr",
// "shot.%04d.png"), which is globbed. Returns {anyFrameFile} for a standalone
// still, or an empty vector if the file is missing. Shared by the sources'
// open() and by freshness hashing so both see the exact same set.
std::vector<std::string> files(const std::string& anyFrameFile);

// Open the decoder matching `anyFrameFile`'s extension. Null on failure, with
// `err` set.
std::shared_ptr<MediaSource> open(const std::string& anyFrameFile, std::string& err);

// Display label for the sequence's format, e.g. "EXR Sequence", "PNG Sequence".
const char* typeLabel(const std::string& path);

} // namespace ImageSeq

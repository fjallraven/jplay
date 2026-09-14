#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace MediaScan {

// Expand a folder (top level only) into the media items it contains: one entry
// per distinct image sequence/still and per supported video file, in natural
// filename order. Frame runs collapse to their lowest-numbered frame (any frame
// re-expands to the full sequence via ImageSeq::files).
std::vector<std::string> collectDirectoryMedia(const std::filesystem::path& dir);

} // namespace MediaScan

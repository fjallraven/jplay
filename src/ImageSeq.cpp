#include "ImageSeq.h"

#include "ExrSource.h"
#include "StillSource.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <utility>

namespace fs = std::filesystem;

namespace {

std::string lowerExt(const fs::path& p) {
    std::string e = p.extension().string();
    std::transform(e.begin(), e.end(), e.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    return e;
}

// Locate a frame-number placeholder in a filename: a run of '#' (hash padding,
// e.g. "shot.####.exr") or a printf token %0Nd / %Nd / %d (e.g. "shot.%04d.exr").
// The '#' run is checked first; the placeholder's width is not used (frames are
// matched by any-length digit run), only its position. Returns false if none.
bool findFramePattern(const std::string& name, size_t& pos, size_t& len) {
    size_t h = name.find('#');
    if (h != std::string::npos) {
        size_t e = h;
        while (e < name.size() && name[e] == '#') ++e;
        pos = h;
        len = e - h;
        return true;
    }
    for (size_t pct = name.find('%'); pct != std::string::npos; pct = name.find('%', pct + 1)) {
        size_t i = pct + 1;
        while (i < name.size() && std::isdigit((unsigned char)name[i])) ++i;
        if (i < name.size() && name[i] == 'd') {
            pos = pct;
            len = i - pct + 1;
            return true;
        }
    }
    return false;
}

// Glob the directory for files matching <prefix><digits><suffix>, returning them
// ordered by frame number. Backs the frame-pattern path form.
std::vector<std::string> globPattern(const fs::path& dir,
                                     const std::string& prefix,
                                     const std::string& suffix) {
    std::error_code ec;
    std::vector<std::pair<long long, std::string>> found;
    for (const auto& entry : fs::directory_iterator(dir, ec)) {
        // Name-only filter; skip is_regular_file() to avoid a per-entry stat on
        // network storage (see the note in files()).
        std::string s = entry.path().filename().string();
        if (s.size() <= prefix.size() + suffix.size())
            continue;
        if (s.compare(0, prefix.size(), prefix) != 0)
            continue;
        if (s.compare(s.size() - suffix.size(), suffix.size(), suffix) != 0)
            continue;
        std::string num = s.substr(prefix.size(), s.size() - prefix.size() - suffix.size());
        if (!std::all_of(num.begin(), num.end(), [](unsigned char c) { return std::isdigit(c); }))
            continue;
        found.emplace_back(std::stoll(num), entry.path().string());
    }
    std::sort(found.begin(), found.end());
    std::vector<std::string> result;
    result.reserve(found.size());
    for (auto& f : found)
        result.push_back(std::move(f.second));
    return result;
}

} // namespace

namespace ImageSeq {

bool isSequenceExt(const std::string& ext) {
    std::string e = ext;
    std::transform(e.begin(), e.end(), e.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    static const char* kExts[] = { ".exr", ".png", ".jpg", ".jpeg", ".tif", ".tiff" };
    for (const char* k : kExts)
        if (e == k)
            return true;
    return false;
}

bool isSequencePath(const std::string& path) {
    return isSequenceExt(lowerExt(fs::path(path)));
}

bool isExrPath(const std::string& path) {
    return lowerExt(fs::path(path)) == ".exr";
}

std::string substituteFrame(const std::string& path, int64_t frame) {
    fs::path p(path);
    std::string filename = p.filename().string();
    size_t pos = 0, len = 0;
    if (!findFramePattern(filename, pos, len))
        return path;
    // Padding width: the '#' run's length, or the printf token's zero-pad digits
    // ("%04d" -> 4; a plain "%d" pads not at all).
    size_t width = 0;
    if (filename[pos] == '#') {
        width = len;
    } else {
        size_t i = pos + 1;
        std::string digits;
        while (i < filename.size() && std::isdigit((unsigned char)filename[i]))
            digits.push_back(filename[i++]);
        if (!digits.empty())
            width = (size_t)std::stoul(digits);
    }
    std::string num = std::to_string(frame);
    if (num.size() < width)
        num.insert(0, width - num.size(), '0');
    filename.replace(pos, len, num);
    return (p.parent_path() / filename).string();
}

std::vector<std::string> files(const std::string& anyFrameFile) {
    fs::path p(anyFrameFile);
    std::error_code ec;
    std::vector<std::string> result;

    // A frame-number placeholder (####, %04d, ...) turns the path into a template:
    // the placeholder position is a digit run to glob, not a concrete file. This
    // is the form passed as a command-line argument; a dropped/browsed file has a
    // real frame number and takes the digit-run path below.
    std::string filename = p.filename().string();
    size_t phPos = 0, phLen = 0;
    if (findFramePattern(filename, phPos, phLen)) {
        std::string prefix = filename.substr(0, phPos);
        std::string suffix = filename.substr(phPos + phLen);
        fs::path dir = p.parent_path();
        if (dir.empty()) dir = "."; // bare filename: glob the working directory
        return globPattern(dir, prefix, suffix); // empty => nothing matched
    }

    if (!fs::exists(p, ec))
        return result; // empty => missing

    // Split the stem into <prefix><digits>; the digit run identifies the sequence.
    std::string stem = p.stem().string();
    size_t digitStart = stem.size();
    while (digitStart > 0 && std::isdigit((unsigned char)stem[digitStart - 1]))
        --digitStart;

    if (digitStart == stem.size()) {
        result.push_back(anyFrameFile); // no frame number: a single still image
        return result;
    }

    // Siblings must share the seed file's extension: a directory holding both
    // shot.0001.exr and shot.0001.png carries two distinct sequences.
    const std::string ext = lowerExt(p);
    std::string prefix = stem.substr(0, digitStart);
    std::vector<std::pair<long long, std::string>> found;
    for (const auto& entry : fs::directory_iterator(p.parent_path(), ec)) {
        // Filter on the name readdir already gave us; do NOT call is_regular_file()
        // — on network storage readdir returns no d_type, so it forces a per-entry
        // stat() round-trip, which dominates the scan. A file matching
        // <prefix><digits><ext> is a frame; a directory named that way doesn't occur.
        const fs::path& ep = entry.path();
        if (lowerExt(ep) != ext)
            continue;
        std::string s = ep.stem().string();
        if (s.size() <= prefix.size() || s.compare(0, prefix.size(), prefix) != 0)
            continue;
        std::string num = s.substr(prefix.size());
        if (!std::all_of(num.begin(), num.end(), [](unsigned char c) { return std::isdigit(c); }))
            continue;
        found.emplace_back(std::stoll(num), ep.string());
    }
    std::sort(found.begin(), found.end());

    for (auto& f : found)
        result.push_back(std::move(f.second));
    if (result.empty())
        result.push_back(anyFrameFile);
    return result;
}

std::shared_ptr<MediaSource> open(const std::string& anyFrameFile, std::string& err) {
    if (isExrPath(anyFrameFile))
        return ExrSequenceSource::open(anyFrameFile, err);
    return StillSequenceSource::open(anyFrameFile, err);
}

const char* typeLabel(const std::string& path) {
    const std::string ext = lowerExt(fs::path(path));
    if (ext == ".exr")                       return "EXR Sequence";
    if (ext == ".png")                       return "PNG Sequence";
    if (ext == ".jpg" || ext == ".jpeg")     return "JPEG Sequence";
    if (ext == ".tif" || ext == ".tiff")     return "TIFF Sequence";
    return "Image Sequence";
}

} // namespace ImageSeq

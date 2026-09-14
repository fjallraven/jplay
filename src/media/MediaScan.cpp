#include "MediaScan.h"

#include "ImageSeq.h"

#include <algorithm>
#include <cctype>
#include <map>
#include <utility>

namespace fs = std::filesystem;

namespace {

std::string lowerExt(const fs::path& p) {
    std::string e = p.extension().string();
    std::transform(e.begin(), e.end(), e.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    return e;
}

// Container formats we'll pick up when scanning a folder. Anything not here
// (and not an image-sequence extension) is skipped silently rather than handed
// to a decoder.
bool isVideoExt(const std::string& ext) {
    static const char* kVideoExts[] = {
        ".mov", ".mp4", ".m4v", ".mkv", ".avi",  ".mxf", ".webm",
        ".mpg", ".mpeg", ".m2v", ".m2ts", ".mts", ".ts", ".wmv",
        ".flv", ".y4m", ".ogv", ".3gp",
    };
    for (const char* v : kVideoExts)
        if (ext == v)
            return true;
    return false;
}

// Case-insensitive natural order: embedded digit runs compare as numbers, so
// "shot2" sorts before "shot10".
bool naturalLess(const std::string& a, const std::string& b) {
    size_t i = 0, j = 0;
    while (i < a.size() && j < b.size()) {
        unsigned char ca = (unsigned char)a[i], cb = (unsigned char)b[j];
        if (std::isdigit(ca) && std::isdigit(cb)) {
            size_t ai = i, bj = j;
            while (ai < a.size() && std::isdigit((unsigned char)a[ai])) ++ai;
            while (bj < b.size() && std::isdigit((unsigned char)b[bj])) ++bj;
            size_t as = i, bs = j; // skip leading zeros (keep one digit)
            while (as < ai - 1 && a[as] == '0') ++as;
            while (bs < bj - 1 && b[bs] == '0') ++bs;
            size_t alen = ai - as, blen = bj - bs;
            if (alen != blen) return alen < blen;
            int cmp = a.compare(as, alen, b, bs, blen);
            if (cmp != 0) return cmp < 0;
            i = ai; j = bj;
        } else {
            char la = (char)std::tolower(ca), lb = (char)std::tolower(cb);
            if (la != lb) return la < lb;
            ++i; ++j;
        }
    }
    return (a.size() - i) < (b.size() - j);
}

} // namespace

namespace MediaScan {

std::vector<std::string> collectDirectoryMedia(const fs::path& dir) {
    std::error_code ec;
    std::vector<std::string> videos, stills;
    // prefix+extension -> (lowest frame#, path). Keyed on the extension too, so a
    // folder holding both shot.0001.exr and shot.0001.png yields two sequences.
    std::map<std::string, std::pair<long long, std::string>> seqs;

    for (const auto& entry : fs::directory_iterator(dir, ec)) {
        // Filter on the name readdir already gave us; skip is_regular_file() to
        // avoid a per-entry stat() round-trip on network storage (see
        // ImageSeq::files). The extension test below rejects non-media entries.
        const fs::path& p = entry.path();
        std::string ext = lowerExt(p);
        if (ImageSeq::isSequenceExt(ext)) {
            std::string stem = p.stem().string();
            size_t digitStart = stem.size();
            while (digitStart > 0 && std::isdigit((unsigned char)stem[digitStart - 1]))
                --digitStart;
            if (digitStart == stem.size()) {
                stills.push_back(p.string()); // no frame number: standalone still
            } else {
                std::string key = stem.substr(0, digitStart) + ext;
                long long num = std::stoll(stem.substr(digitStart));
                auto it = seqs.find(key);
                if (it == seqs.end() || num < it->second.first)
                    seqs[key] = { num, p.string() };
            }
        } else if (isVideoExt(ext)) {
            videos.push_back(p.string());
        }
    }

    std::vector<std::string> result;
    result.reserve(videos.size() + seqs.size() + stills.size());
    for (auto& v : videos) result.push_back(std::move(v));
    for (auto& s : seqs) result.push_back(std::move(s.second.second));
    for (auto& s : stills) result.push_back(std::move(s));

    std::sort(result.begin(), result.end(), [](const std::string& a, const std::string& b) {
        return naturalLess(fs::path(a).filename().string(), fs::path(b).filename().string());
    });
    return result;
}

} // namespace MediaScan

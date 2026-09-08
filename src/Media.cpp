#include "Media.h"

#include "AudioSource.h"
#include "ImageSeq.h"
#include "ProxyMode.h"
#include "VideoSource.h"

#include <algorithm>
#include <filesystem>
#include <istream>
#include <ostream>
#include <vector>

namespace fs = std::filesystem;

namespace {

// 64-bit hash mixer (boost::hash_combine style).
uint64_t mix(uint64_t h, uint64_t v) {
    h ^= v + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
    return h;
}

// Fingerprint a single file by size + modification time. Returns 0 if the file
// is missing or unreadable, which deliberately reads as "not fresh".
uint64_t statHash(const fs::path& p) {
    std::error_code ec;
    uint64_t sz = (uint64_t)fs::file_size(p, ec);
    if (ec)
        return 0;
    auto t = fs::last_write_time(p, ec);
    if (ec)
        return 0;
    return mix(mix(1469598103934665603ULL, sz), (uint64_t)t.time_since_epoch().count());
}

template <typename T>
void writeRaw(std::ostream& os, const T& v) {
    os.write(reinterpret_cast<const char*>(&v), sizeof(T));
}

template <typename T>
bool readRaw(std::istream& is, T& v) {
    is.read(reinterpret_cast<char*>(&v), sizeof(T));
    return is.good();
}

void writeStr(std::ostream& os, const std::string& s) {
    writeRaw(os, (uint32_t)s.size());
    os.write(s.data(), (std::streamsize)s.size());
}

bool readStr(std::istream& is, std::string& s) {
    uint32_t n = 0;
    if (!readRaw(is, n) || n > (1u << 20))
        return false;
    s.resize(n);
    is.read(s.data(), (std::streamsize)n);
    return is.good() || (n == 0 && !is.bad());
}

} // namespace

Media::Media(ClipType type, std::string path, std::string id)
    : id_(id.empty() ? generateId(type, path) : std::move(id))
    , type_(type)
    , path_(std::move(path)) {}

// FNV-1a 64-bit hash of type + path → 16 lowercase hex chars.
std::string Media::generateId(ClipType type, const std::string& path) {
    uint64_t h = 14695981039346656037ULL;
    h ^= (uint64_t)type;
    h *= 1099511628211ULL;
    for (unsigned char c : path) {
        h ^= c;
        h *= 1099511628211ULL;
    }
    char buf[17];
    for (int i = 15; i >= 0; --i) {
        buf[i] = "0123456789abcdef"[h & 0xF];
        h >>= 4;
    }
    buf[16] = '\0';
    return std::string(buf);
}

MediaInfo Media::info() const {
    std::lock_guard<std::mutex> lk(infoMtx_);
    return info_;
}

void Media::setInfo(const MediaInfo& i) {
    std::lock_guard<std::mutex> lk(infoMtx_);
    info_ = i;
}

std::shared_ptr<MediaSource> Media::ensureOpen(std::string& err) {
    // Fast path: source_ is current for the live proxy generation, so
    // steady-state calls (once per decoded frame from the cache worker) can
    // read it via the atomic shared_ptr ops with no lock. A generation bump
    // (a proxy-mode switch) makes every Media fall through to the slow path
    // exactly once, on its next call.
    if (opened_.load(std::memory_order_acquire) &&
        openedGeneration_.load(std::memory_order_relaxed) == jplay::proxyGeneration()) {
        auto src = std::atomic_load(&source_);
        if (!src)
            err = "media unavailable";
        return src;
    }

    // Freed after openMtx_ is released below: closing a VideoSource/ImageSeq
    // (FFmpeg teardown, network directory handle) is slow and must not happen
    // while other threads are blocked on this media's lock.
    std::shared_ptr<MediaSource> outgoing;
    std::shared_ptr<MediaSource> newSrc;
    {
        std::lock_guard<std::mutex> lk(openMtx_);
        const uint64_t gen = jplay::proxyGeneration();
        if (opened_.load(std::memory_order_relaxed) &&
            openedGeneration_.load(std::memory_order_relaxed) == gen) {
            // Lost the race to another opener for this same generation.
            newSrc = std::atomic_load(&source_);
            if (!newSrc)
                err = "media unavailable";
            return newSrc;
        }
        if (type_ == ClipType::Audio) {
            // No frame decoder, no proxy concept: probe the nominal file so
            // openFailed reflects a missing/unreadable source, but keep
            // source_ null (nothing renders frames).
            openFailed_ = !AudioSource::open(path_, err);
            openedGeneration_.store(gen, std::memory_order_relaxed);
            opened_.store(true, std::memory_order_release);
            return nullptr;
        }
        std::string openPath = path_;
        std::string proxyPath;
        const bool substituted = jplay::resolveProxyPath(path_, proxyPath);
        if (substituted)
            openPath = proxyPath;
        // A media that got a substitute skips the mode's slate. One that did not
        // decodes its nominal path -- which may be a slated file in its own right
        // (a published quicktime added directly rather than substituted in), and
        // is so under every mode including Full, so the count comes from the path
        // rather than from the mode. Ordinary full-res frames answer 0.
        slateOffset_.store(substituted ? jplay::proxySlateFrames()
                                       : jplay::pathSlateFrames(openPath),
                           std::memory_order_relaxed);
        // Decoder by the RESOLVED path's extension, not by type_: a proxy mode may
        // substitute a different container for the same shot (a published h264 for
        // an EXR sequence), and dispatching on the clip's own type would hand that
        // .mp4 to ImageSeq and read as unavailable. Equivalent to type_ for every
        // non-proxy open — an ImageSequence media's path is a sequence extension by
        // construction, a video's is not.
        newSrc = ImageSeq::isSequencePath(openPath)
                     ? ImageSeq::open(openPath, err)  // EXR or 8-bit still, by extension
                     : VideoSource::open(openPath, err);
        openFailed_ = !newSrc;
        outgoing = std::atomic_load(&source_);
        std::atomic_store(&source_, newSrc);
        openedGeneration_.store(gen, std::memory_order_relaxed);
        opened_.store(true, std::memory_order_release);
    } // openMtx_ released; `outgoing` (if any) is freed below, unlocked.
    return newSrc;
}

bool Media::isOpen() const {
    std::lock_guard<std::mutex> lk(openMtx_);
    return (bool)source_;
}

std::string Media::resolvedPath() const {
    std::lock_guard<std::mutex> lk(openMtx_);
    return source_ ? source_->path() : path_;
}

bool Media::openFailed() const {
    std::lock_guard<std::mutex> lk(openMtx_);
    return openFailed_;
}

bool Media::refreshMetadata() {
    std::string err;
    if (type_ == ClipType::Audio) {
        // Re-probe availability + freshness. frameCount (frames at the project
        // fps, computed when the clip was added) is kept as loaded.
        auto asrc = AudioSource::open(path_, err);
        {
            std::lock_guard<std::mutex> lk(openMtx_);
            opened_ = true;
            openFailed_ = !asrc;
        }
        if (!asrc)
            return false;
        MediaInfo i = info();
        i.freshHash = computeFreshHash(type_, path_);
        setInfo(i);
        return true;
    }
    auto src = ensureOpen(err);
    if (!src)
        return false;
    MediaInfo i;
    i.width = src->width();
    i.height = src->height();
    // Minus the slate: the read offset is invisible above this line -- readFrame()
    // adds it to whatever index it is given -- so the length reported here is the
    // frames a caller can actually reach. Clip duration at add, trim limits, the
    // inspector and the OTIO export all want that rather than the file's own
    // count, whose head is not shot. 0 for everything but a slated file.
    i.frameCount = std::max<int64_t>(src->frameCount() - slateOffset(), 1);
    i.fps = src->fps();
    i.pixelAspect = src->pixelAspect();
    // The source already scanned the sequence directory at open(); reuse that
    // file set for the freshness hash instead of re-scanning it here (the scan
    // dominates refreshMetadata on network storage). Empty for video.
    const std::vector<std::string>& seqFiles = src->sequenceFiles();
    i.freshHash = seqFiles.empty() ? computeFreshHash(type_, path_)
                                   : freshHashFromFiles(seqFiles);
    setInfo(i);
    return true;
}

bool Media::fresh() const {
    uint64_t cached = info().freshHash;
    return cached != 0 && computeFreshHash(type_, path_) == cached;
}

uint64_t Media::freshHashFromFiles(const std::vector<std::string>& files) {
    if (files.empty())
        return 0;
    uint64_t h = mix(1469598103934665603ULL, (uint64_t)files.size());
    h = mix(h, statHash(files.front()));
    h = mix(h, statHash(files.back()));
    return h;
}

uint64_t Media::computeFreshHash(ClipType type, const std::string& path) {
    if (type == ClipType::ImageSequence)
        return freshHashFromFiles(ImageSeq::files(path));
    return statHash(path);
}

const std::string& Media::metaValue(const std::string& key) const {
    static const std::string empty;
    auto it = meta_.find(key);
    return it == meta_.end() ? empty : it->second;
}

std::string Media::colorSpaceOverride() const {
    std::lock_guard<std::mutex> lk(colorMtx_);
    return colorSpaceOverride_;
}

void Media::setColorSpaceOverride(std::string cs) {
    std::lock_guard<std::mutex> lk(colorMtx_);
    colorSpaceOverride_ = std::move(cs);
}

std::string Media::resolvedColorSpace(const std::string& configKey) const {
    std::lock_guard<std::mutex> lk(colorMtx_);
    return resolvedConfigKey_ == configKey ? resolvedColorSpace_ : std::string();
}

void Media::setResolvedColorSpace(const std::string& configKey, std::string cs) {
    std::lock_guard<std::mutex> lk(colorMtx_);
    resolvedConfigKey_ = configKey;
    resolvedColorSpace_ = std::move(cs);
}

void Media::serialize(std::ostream& os) const {
    MediaInfo i = info();
    writeStr(os, id_);
    writeRaw(os, (uint8_t)type_);
    writeStr(os, path_);
    writeStr(os, name_);
    writeRaw(os, (uint32_t)meta_.size());
    for (const auto& kv : meta_) {
        writeStr(os, kv.first);
        writeStr(os, kv.second);
    }
    writeRaw(os, i.width);
    writeRaw(os, i.height);
    writeRaw(os, i.frameCount);
    writeRaw(os, i.fps);
    writeRaw(os, i.pixelAspect);
    writeRaw(os, i.freshHash);
    // Cached applicable-picker set (path-derived; resolved once via Python).
    writeRaw(os, (uint8_t)pickersResolved_);
    writeRaw(os, (uint32_t)applicablePickers_.size());
    for (const auto& k : applicablePickers_)
        writeStr(os, k);
    // Explicit input colour space (v32+); "" = resolved from the OCIO config. Only
    // the override is stored — the resolved answer belongs to one config and is
    // recomputed on load.
    writeStr(os, colorSpaceOverride());
}

std::shared_ptr<Media> Media::deserialize(std::istream& is, uint32_t version, bool& ok) {
    ok = false;
    std::string id, path, name;
    uint8_t type = 0;
    if (!readStr(is, id) || !readRaw(is, type) || !readStr(is, path) || !readStr(is, name))
        return nullptr;

    auto m = std::make_shared<Media>((ClipType)type, std::move(path), std::move(id));
    m->setName(std::move(name));

    // Metadata as a key/value map (keys defined by the naming config's pickers).
    uint32_t n = 0;
    if (!readRaw(is, n) || n > 256)
        return nullptr;
    for (uint32_t k = 0; k < n; ++k) {
        std::string key, val;
        if (!readStr(is, key) || !readStr(is, val))
            return nullptr;
        m->setMetaValue(std::move(key), std::move(val));
    }

    MediaInfo i;
    if (!readRaw(is, i.width) || !readRaw(is, i.height) || !readRaw(is, i.frameCount) ||
        !readRaw(is, i.fps))
        return nullptr;
    // Pixel aspect (v28+). Older files load square, which is what every source
    // but an anamorphic EXR is; the real value returns with the next probe.
    if (version >= 28 && !readRaw(is, i.pixelAspect))
        return nullptr;
    if (!readRaw(is, i.freshHash))
        return nullptr;
    m->setInfo(i);

    // Cached applicable-picker set. Unresolved => resolved lazily on the first
    // frame Python answers.
    {
        uint8_t resolved = 0;
        uint32_t nk = 0;
        if (!readRaw(is, resolved) || !readRaw(is, nk) || nk > 256)
            return nullptr;
        std::vector<std::string> keys;
        keys.reserve(nk);
        for (uint32_t k = 0; k < nk; ++k) {
            std::string key;
            if (!readStr(is, key))
                return nullptr;
            keys.push_back(std::move(key));
        }
        if (resolved)
            m->setApplicablePickers(std::move(keys));
    }

    // Explicit input colour space (v32+). Older projects carry none, which is the
    // same as "resolve from the config" — so they open colour-managed by default.
    if (version >= 32) {
        std::string cs;
        if (!readStr(is, cs))
            return nullptr;
        m->setColorSpaceOverride(std::move(cs));
    }

    ok = true;
    return m;
}

#pragma once

#include "MediaSource.h"

#include <atomic>
#include <cstdint>
#include <iosfwd>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

// Serialized as a u8 in .jpproj; the wire values are fixed.
enum class ClipType : uint8_t {
    Video = 0,
    ImageSequence = 1, // numbered still frames: EXR, or 8-bit sRGB (PNG/JPEG/TIFF)
    Audio = 2,
};

// Cached, serializable metadata for a media item. `freshHash` fingerprints the
// file(s) on disk (size + modification time); when it still matches, the cached
// width/height/length/fps are trusted and the source need not be probed.
struct MediaInfo {
    int32_t width = 0;
    int32_t height = 0;
    int64_t frameCount = 0;
    double fps = 0.0;
    // Pixel width / height (see MediaSource::pixelAspect); 1 = square pixels.
    // Cached alongside the dimensions because the display shape is needed where
    // no frame is at hand — thumbnail tiles are laid out from it.
    float pixelAspect = 1.0f;
    uint64_t freshHash = 0; // 0 = unknown / not yet computed
};

// One physical media item (a video file, an image sequence, or an audio file),
// shared by every clip that references the same path. Holds cached metadata plus
// a lazily-opened decoder. The id is project-scoped and stable across saves
// (generated from type+path via generateId). All members are safe to touch
// concurrently. Audio media has no frame decoder: ensureOpen probes the file
// (so openFailed reflects a missing source) but always returns null; playback
// opens its own AudioSource per mixer channel.
class Media {
public:
    Media(ClipType type, std::string path, std::string id = {});

    // Generate a stable project-scoped id from type + path.
    static std::string generateId(ClipType type, const std::string& path);

    const std::string& id()   const { return id_; }
    ClipType           type() const { return type_; }
    const std::string& path() const { return path_; }

    // The concrete representative file for this media. For an image sequence opened
    // from a frame-pattern path (e.g. "shot_####.exr" / "shot_%04d.exr", the
    // command-line form), path() keeps the placeholder but this returns the
    // resolved first frame; otherwise it equals path(). Naming-convention parsing
    // needs a real frame number, not the placeholder. Falls back to path() when
    // the decoder isn't open.
    std::string resolvedPath() const;

    // Asset metadata (set once at load/creation; read from any thread).
    const std::string& name() const { return name_; }
    void setName(std::string v) { name_ = std::move(v); }

    // Picker metadata keyed by naming-config picker key (e.g. "department",
    // "asset", "version"). The set of keys is whatever the naming config's
    // [picker:*] sections define; there are no fixed fields. metaValue returns
    // an empty string for a key that was never set.
    const std::string& metaValue(const std::string& key) const;
    void setMetaValue(std::string key, std::string value) { meta_[std::move(key)] = std::move(value); }
    const std::map<std::string, std::string>& meta() const { return meta_; }

    // Cached set of picker keys that apply to this media's path (the pickers
    // describe_pickers reports with options). Which pickers apply is purely a
    // function of the path, so it is resolved once — from Python the first frame
    // it is available — then cached and serialized. This drives picker visibility
    // without re-parsing the path each frame; the live option lists are still
    // fetched from Python when a picker is opened.
    bool pickersResolved() const { return pickersResolved_; }
    const std::vector<std::string>& applicablePickers() const { return applicablePickers_; }
    void setApplicablePickers(std::vector<std::string> keys) {
        applicablePickers_ = std::move(keys);
        pickersResolved_ = true;
    }

    // ── Colour management ─────────────────────────────────────────────────
    // The OCIO colour space this media's pixels are interpreted in, in two parts.
    //
    // The override is the user's explicit choice and is serialized with the project;
    // empty means "let the config decide". The resolved space is what
    // OcioManager::colorSpaceForMedia answered for this media, cached rather than
    // recomputed per rendered frame. That answer is only meaningful against the
    // config it came from, so it is stored with the config's key and reads back
    // empty once a different config is active — the caller then resolves again.
    std::string colorSpaceOverride() const;
    void setColorSpaceOverride(std::string cs);
    std::string resolvedColorSpace(const std::string& configKey) const;
    void setResolvedColorSpace(const std::string& configKey, std::string cs);

    MediaInfo info() const;            // thread-safe snapshot
    void setInfo(const MediaInfo& i);

    // Set by an importer whose sidecar had already verified this media on disk and
    // so could supply width/height/length without opening anything (an .otio's
    // media_verified clips — see OtioImport). It lets the post-load metadata probe
    // skip the media outright, which is the whole point: opening a few hundred
    // sequences over network storage costs seconds, and everything the probe would
    // have learned is already here. Written once during import, before the Media is
    // shared with any worker. Not serialized: a .jpproj carries a freshHash, which
    // is the stronger guarantee.
    bool infoTrusted() const { return infoTrusted_; }
    void setInfoTrusted() { infoTrusted_ = true; }

    // A path carrying a plausible frame number, for naming-convention parsing only —
    // never for reading pixels. An .otio whose media wasn't verified on disk names a
    // frame-pattern file ("shot.#.exr"), and the convention's regexes need a digit
    // run where the frame goes; the pattern's start frame is the best guess available
    // without opening the sequence, and a wrong guess costs nothing here because the
    // query only ever parses the string (path() keeps the pattern, so every real read
    // still globs the directory). Empty when path() already names a concrete frame.
    // Set once at import; not serialized.
    const std::string& namingPathHint() const { return namingPathHint_; }
    void setNamingPathHint(std::string p) { namingPathHint_ = std::move(p); }

    // Opens the decoder against whatever the global proxy mode (ProxyMode.h)
    // currently resolves path() to — the nominal path when the mode is "Full"
    // or resolves to nothing — and caches the result; a failed open is
    // remembered and never retried *for that mode's generation* (see
    // openedGeneration_ below: switching modes gives every media a fresh open
    // attempt, including re-probing a path that previously failed under a
    // different mode). Thread-safe. Returns null if unavailable.
    std::shared_ptr<MediaSource> ensureOpen(std::string& err);

    // Frames to skip at the head of the open source: the active proxy mode's
    // slate count (ProxyMode.h) when the last ensureOpen() actually substituted a
    // proxy for this media, 0 otherwise — including under a mode whose resolver
    // found no proxy for this source, which decodes the nominal path from frame 0
    // as ever. Every reader adds it to the index it asks for, so a clip's frame 0
    // is the shot's first frame rather than the slate card. Valid once
    // ensureOpen() has returned a source.
    int64_t slateOffset() const { return slateOffset_.load(std::memory_order_relaxed); }
    bool isOpen() const;
    bool openFailed() const; // true once an open has been attempted and failed

    // ensureOpen() + read dimensions/length/fps + recompute the freshness hash.
    bool refreshMetadata();

    // True when the file(s) on disk still match the cached freshness hash.
    bool fresh() const;
    static uint64_t computeFreshHash(ClipType type, const std::string& path);
    // Freshness hash from an already-scanned EXR file set (count + first/last
    // file stats), avoiding a redundant directory scan when the source is open.
    static uint64_t freshHashFromFiles(const std::vector<std::string>& files);

    void serialize(std::ostream& os) const;
    static std::shared_ptr<Media> deserialize(std::istream& is, uint32_t version, bool& ok);

private:
    const std::string id_;
    const ClipType    type_;
    const std::string path_;

    std::string name_;
    std::map<std::string, std::string> meta_;
    std::vector<std::string> applicablePickers_;
    bool pickersResolved_ = false;

    mutable std::mutex colorMtx_;
    std::string colorSpaceOverride_;  // user's explicit choice; "" = resolve from config
    std::string resolvedConfigKey_;   // config resolvedColorSpace_ was resolved against
    std::string resolvedColorSpace_;

    mutable std::mutex infoMtx_;
    MediaInfo info_;
    bool infoTrusted_ = false;
    std::string namingPathHint_;

    mutable std::mutex openMtx_;
    // Read via std::atomic_load / written via std::atomic_store so ensureOpen()'s
    // fast path can read it without openMtx_ even though, unlike the old
    // write-once design, a proxy-mode switch can now replace it after the first
    // open (see openedGeneration_).
    std::shared_ptr<MediaSource> source_;
    // Set once opened_ is true, so ensureOpen()'s fast path can check it
    // lock-free; the release store publishes the source_ write above.
    std::atomic<bool> opened_{false};
    // Which jplay::proxyGeneration() source_ was opened for. The fast path
    // treats source_ as valid only while this still matches the live
    // generation; a stale generation retakes openMtx_ and reopens against
    // whatever the (possibly new) mode resolves to. openFailed_ is likewise
    // only meaningful for the generation it was recorded under.
    std::atomic<uint64_t> openedGeneration_{0};
    // Published by the same release store as source_ (written before it), so a
    // fast-path reader that sees the source sees the offset it was opened with.
    std::atomic<int64_t> slateOffset_{0};
    bool openFailed_ = false;
};

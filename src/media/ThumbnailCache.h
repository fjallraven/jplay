#pragma once

#include "Media.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

// One clip's thumbnail request. `key` is the stable filename stem (no extension)
// the caller computed (a hash of sequence + shot name); `media` may be null (an
// unset clip), in which case no thumbnail is produced and the UI shows a
// placeholder. `srcFrame` is the source-frame index to grab (clip middle frame).
struct ThumbItem {
    std::string key;
    std::shared_ptr<Media> media;
    int64_t srcFrame = 0;
};

// Box-average `src` down to dw x dh into `out` (dw*dh*4 RGBA, opaque; cleared
// when there is nothing to scale).
//
// The buffer read is always the frame's 8-bit companion, which every source fills
// with display-encoded pixels: an EXR's is its scene-linear halves through the
// sRGB transfer curve (ExrSource bakes that with a LUT), a video's or a still's is
// the decode itself. A thumbnail is that image and nothing else — no config, no
// view, no context — so a tile never depends on review state and never has to be
// retired when it changes.
//
// Downscale first: a 4K frame is eight million pixels, its 512px tile a quarter of
// a million. Averaging happens in the encoded space rather than in linear light —
// the approximation this box filter has always made, and one nobody can see at tile
// scale.
//
// Shared by the thumbnail workers and the timeline's hover preview, which want the
// same small image at different sizes.
void renderFrameScaled(const Frame& src, int dw, int dh, std::vector<uint8_t>& out);

// Generates 512x512 thumbnail JPEGs in the background, one per ThumbItem, into a
// directory the caller resolves (project's .thumbnails, else a temp folder). The
// frame is decoded directly off the Media (not via the playback FrameCache, whose
// per-tick wanted-set would cancel these), scaled to fit 512x512 preserving its
// display aspect ratio (the source's pixel aspect is baked in, so an anamorphic
// EXR is stored in its true shape), and centered on a black square so every file is
// exactly 512x512. Files already present are skipped (no freshness check;
// kFormatTag is how a shape change retires them). Generation is fanned out across
// several worker threads (see kMaxWorkers).
//
// Starting a pass never blocks the caller: the outgoing run is flagged cancelled
// and abandoned rather than joined, so a scroll that re-fans the workers does not
// wait on an in-flight network read. Abandoned workers stop at the next item and
// are reaped later; only the destructor joins.
class ThumbnailCache {
public:
    static constexpr int kSize = 512;
    static constexpr unsigned kMaxWorkers = 8;
    // Cache-format tag every caller mixes into its thumbnail keys. Bumping it
    // retires the files older builds wrote (their keys no longer match) instead of
    // leaving stale images behind; nothing prunes the directory, so those files are
    // simply never read again.
    //   "2" = the frame's pixel aspect is baked into the image.
    //   "3" = the image is the OCIO display rendering, not the decoded pixels.
    //   "4" = the image is the frame's 8-bit companion again; no display rendering.
    static constexpr const char* kFormatTag = "4\x1f";

    ~ThumbnailCache();

    // Resolve where thumbnails live: "<projectDir>/.thumbnails" when projectPath
    // is a saved file and that directory is writable, otherwise
    // "<temp>/jplay_thumbnails". Creates the directory. Returns the chosen path.
    static std::string resolveDir(const std::string& projectPath);

    // Set the directory thumbnails are read from / written to (created if needed).
    void setDir(const std::string& dir);
    const std::string& dir() const { return dir_; }

    // Start (or restart) background generation for `items`. Cancels any current run
    // first (without waiting for it). Items whose JPEG already exists, or whose
    // media is null, are skipped by the worker.
    void start(std::vector<ThumbItem> items);

    // Signal the current run to stop. Does not wait for it. Idempotent; safe to
    // call when idle.
    void stop();

    // "<dir>/<key>.jpg".
    std::string pathFor(const std::string& key) const;

    // Encode `frame` as "<dir>/<key>.jpg" unless that file already exists (returns
    // false then, as it does on any failure). Shares no state; the temp file it
    // renames from is unique per calling thread. Used by the workers and by the
    // timeline's hover preview, which reuses its decode to fill in thumbnails no
    // background pass has produced yet.
    static bool writeIfMissing(const std::string& dir, const std::string& key,
                               const Frame& frame);

private:
    // One generation pass. Held by shared_ptr so an abandoned run's workers keep it
    // alive after the cache has moved on — and, because the worker is a free
    // function taking copies, after the cache itself is gone. `alive` counts the
    // threads still running, which is what lets a retired run be joined without
    // waiting.
    struct Run {
        std::atomic<bool> cancel{ false };
        std::atomic<int> alive{ 0 };
    };

    // One bucket's generation loop. A static taking copies rather than a member:
    // holding no `this` is what makes an abandoned run safe to outlive the cache.
    static void worker_(std::string dir, std::vector<ThumbItem> items,
                        std::shared_ptr<Run> run);

    // How many cancelled-but-still-running passes may be outstanding before a
    // reap waits for the oldest instead of letting them accumulate.
    static constexpr size_t kMaxRetired = 4;

    // Join the retired runs whose threads have all finished. Called when a pass
    // starts or stops, so the list never grows; blocks only past kMaxRetired.
    void reap_();

    std::string dir_;
    std::shared_ptr<Run> run_;                                          // current pass
    std::vector<std::pair<std::shared_ptr<Run>, std::vector<std::thread>>> retired_;
    std::vector<std::thread> threads_;                                  // of run_
};

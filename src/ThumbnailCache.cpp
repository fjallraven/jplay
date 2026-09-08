#include "ThumbnailCache.h"

#include "MediaSource.h"
#include "ProxyMode.h"

#include <SDL3/SDL.h>
#include <SDL3_image/SDL_image.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <functional>

namespace fs = std::filesystem;

ThumbnailCache::~ThumbnailCache() {
    stop(); // cancels the live run and moves it to retired_
    // Teardown is the one place we wait: a detached worker must not outlive the
    // process writing files or touching the Media it holds.
    for (auto& r : retired_) {
        r.first->cancel = true;
        for (auto& t : r.second)
            if (t.joinable())
                t.join();
    }
    retired_.clear();
}

std::string ThumbnailCache::resolveDir(const std::string& projectPath) {
    std::error_code ec;

    // Preferred: a ".thumbnails" folder next to the saved project file. Only used
    // when the project actually has a path and the folder can be created (a probe
    // write would be heavier; create_directory failing is a good enough signal).
    if (!projectPath.empty()) {
        fs::path p = fs::path(projectPath).parent_path();
        if (!p.empty()) {
            fs::path t = p / ".thumbnails";
            fs::create_directories(t, ec);
            if (!ec && fs::is_directory(t, ec))
                return t.string();
        }
    }

    // Fallback: a shared folder under the system temp directory.
    fs::path tmp = fs::temp_directory_path(ec);
    if (!ec) {
        fs::path t = tmp / "jplay_thumbnails";
        fs::create_directories(t, ec);
        if (!ec && fs::is_directory(t, ec))
            return t.string();
    }
    return {};
}

void ThumbnailCache::setDir(const std::string& dir) {
    dir_ = dir;
}

std::string ThumbnailCache::pathFor(const std::string& key) const {
    if (dir_.empty() || key.empty())
        return {};
    return (fs::path(dir_) / (key + ".jpg")).string();
}

void ThumbnailCache::reap_() {
    for (size_t i = 0; i < retired_.size();) {
        if (retired_[i].first->alive.load(std::memory_order_acquire) == 0) {
            for (auto& t : retired_[i].second)
                if (t.joinable())
                    t.join(); // already exited: returns at once
            retired_.erase(retired_.begin() + (ptrdiff_t)i);
        } else {
            ++i; // still working; leave it for the next pass boundary
        }
    }
    // A run that cannot be reaped is one still inside a read it can't be pulled out
    // of. Those are bounded: past kMaxRetired we wait for the oldest rather than let
    // stop-start-stop over slow storage stack up worker threads without limit. In
    // practice this never fires — a cancelled worker exits after the frame it is on.
    while (retired_.size() > kMaxRetired) {
        for (auto& t : retired_.front().second)
            if (t.joinable())
                t.join();
        retired_.erase(retired_.begin());
    }
}

void ThumbnailCache::stop() {
    if (run_) {
        run_->cancel = true;
        retired_.emplace_back(std::move(run_), std::move(threads_));
        threads_.clear(); // moved-from: make it definitely empty
    }
    reap_();
}

void ThumbnailCache::start(std::vector<ThumbItem> items) {
    stop();
    if (dir_.empty() || items.empty())
        return;

    // Fan the work out across several threads, bucketed by what the underlying
    // decoder allows:
    //  - Video: every item sharing a Media goes to the same thread. A VideoSource
    //    is a stateful seek+forward-decode pipeline serialized on its own mutex, so
    //    spreading those items would only queue them on that one lock — FrameCache
    //    declines the same parallelism via busyVideos_. Bucketing by Media pointer
    //    sidesteps concurrent same-source reads without any per-frame locking.
    //  - ImageSequence: dealt out round-robin. ExrSource/StillSource are stateless
    //    per-frame readers (thread-local scratch, no seek state), so many frames of
    //    one sequence decode in parallel — which is the common case for a timeline
    //    cut from a single plate, where per-Media bucketing collapsed the whole
    //    grid onto one thread.
    unsigned hw = std::thread::hardware_concurrency();
    unsigned n = std::min<unsigned>(kMaxWorkers, std::max<unsigned>(1, hw));
    n = std::min<unsigned>(n, (unsigned)items.size());

    std::vector<std::vector<ThumbItem>> buckets(n);
    size_t rr = 0;
    for (auto& item : items) {
        Media* m = item.media.get();
        size_t b = (m && m->type() == ClipType::ImageSequence)
                       ? rr++ % n
                       : std::hash<Media*>{}(m) % n;
        buckets[b].push_back(std::move(item));
    }

    int live = 0;
    for (const auto& b : buckets)
        if (!b.empty())
            ++live;
    if (live == 0)
        return;

    // alive is set before any thread starts, so a reap can never see a run that
    // only looks finished because its threads have yet to be spawned.
    auto run = std::make_shared<Run>();
    run->alive.store(live, std::memory_order_release);

    threads_.reserve((size_t)live);
    for (auto& b : buckets) {
        if (!b.empty())
            threads_.emplace_back(&ThumbnailCache::worker_, dir_, std::move(b), run);
    }
    run_ = std::move(run);
}

namespace {

// Box-average `src` (sw x sh RGBA) down to dw x dh.
void boxAverage(const uint8_t* src, int sw, int sh, uint8_t* dst, int dw, int dh) {
    for (int y = 0; y < dh; ++y) {
        const int sy0 = (int)((int64_t)y * sh / dh);
        const int sy1 = std::max(sy0 + 1, (int)((int64_t)(y + 1) * sh / dh));
        for (int x = 0; x < dw; ++x) {
            const int sx0 = (int)((int64_t)x * sw / dw);
            const int sx1 = std::max(sx0 + 1, (int)((int64_t)(x + 1) * sw / dw));
            float acc[4] = {};
            int n = 0;
            for (int yy = sy0; yy < sy1; ++yy) {
                const uint8_t* p = src + ((size_t)yy * sw + sx0) * 4;
                for (int xx = sx0; xx < sx1; ++xx, p += 4) {
                    for (int c = 0; c < 4; ++c)
                        acc[c] += (float)p[c];
                    ++n;
                }
            }
            uint8_t* d = dst + ((size_t)y * dw + x) * 4;
            for (int c = 0; c < 4; ++c)
                d[c] = (uint8_t)(acc[c] / (float)n + 0.5f);
        }
    }
}

// Scale `src` to fit within size x size preserving aspect ratio and center the
// result on an opaque-black size x size canvas. Returns a size*size*4 RGBA buffer.
//
// The shape fitted is the frame's *display* shape: an anamorphic source (an EXR
// with pixelAspect != 1) is stretched horizontally on the way down, so the JPEG
// holds square pixels and every consumer can draw it as a plain image.
std::vector<uint8_t> fitOnBlackSquare(const Frame& src, int size) {
    std::vector<uint8_t> out((size_t)size * size * 4);
    for (size_t i = 0; i < out.size(); i += 4) {
        out[i + 0] = 0;
        out[i + 1] = 0;
        out[i + 2] = 0;
        out[i + 3] = 255;
    }
    const int sw = src.width, sh = src.height;
    if (sw <= 0 || sh <= 0)
        return out;

    const double dispW = sw * (src.pixelAspect > 0.0f ? src.pixelAspect : 1.0f);
    const double scale = std::min((double)size / dispW, (double)size / sh);
    const int dw = std::max(1, std::min(size, (int)std::lround(dispW * scale)));
    const int dh = std::max(1, std::min(size, (int)std::lround(sh * scale)));
    const int ox = (size - dw) / 2;
    const int oy = (size - dh) / 2;

    std::vector<uint8_t> tile;
    renderFrameScaled(src, dw, dh, tile);
    if (tile.empty())
        return out;

    for (int y = 0; y < dh; ++y)
        std::memcpy(&out[(((size_t)(oy + y) * size) + ox) * 4],
                    &tile[(size_t)y * dw * 4], (size_t)dw * 4);
    return out;
}

} // namespace

void renderFrameScaled(const Frame& src, int dw, int dh, std::vector<uint8_t>& out) {
    const int sw = src.width, sh = src.height;
    const size_t n = (size_t)sw * sh;
    if (sw <= 0 || sh <= 0 || dw <= 0 || dh <= 0 || src.rgba.size() < n * 4) {
        out.clear();
        return; // nothing to scale
    }

    out.resize((size_t)dw * dh * 4);
    boxAverage(src.rgba.data(), sw, sh, out.data(), dw, dh);
    for (size_t i = 3; i < out.size(); i += 4)
        out[i] = 255; // opaque: these images are drawn, never blended
}

bool ThumbnailCache::writeIfMissing(const std::string& dir, const std::string& key,
                                    const Frame& frame) {
    if (dir.empty() || key.empty() || frame.width <= 0 || frame.height <= 0)
        return false;

    const std::string path = (fs::path(dir) / (key + ".jpg")).string();
    std::error_code ec;
    if (fs::exists(path, ec))
        return false; // already generated; no freshness check by design

    std::vector<uint8_t> px = fitOnBlackSquare(frame, kSize);
    SDL_Surface* surf = SDL_CreateSurfaceFrom(kSize, kSize, SDL_PIXELFORMAT_RGBA32,
                                              px.data(), kSize * 4);
    if (!surf)
        return false;
    // Write to a temp name then rename, so a reader never sees a half-written
    // JPEG (the UI polls for existence each frame). The temp name carries the
    // thread id because two writers can now target the same key (a worker and the
    // hover preview); both produce a valid file, so last rename wins. Quality 90
    // is visually lossless at thumbnail scale; alpha is dropped (thumbnails are
    // opaque).
    const std::string tmp =
        path + "." +
        std::to_string(std::hash<std::thread::id>{}(std::this_thread::get_id())) + ".part";
    bool ok = false;
    if (IMG_SaveJPG(surf, tmp.c_str(), 90)) {
        fs::rename(tmp, path, ec);
        if (ec) fs::remove(tmp, ec);
        else    ok = true;
    }
    SDL_DestroySurface(surf);
    return ok;
}

void ThumbnailCache::worker_(std::string dir, std::vector<ThumbItem> items,
                             std::shared_ptr<Run> run) {
    // Whichever way this returns, the run has to see one fewer live thread — that
    // count is what lets an abandoned run be reaped without waiting on it.
    struct AliveGuard {
        std::shared_ptr<Run> run;
        ~AliveGuard() { run->alive.fetch_sub(1, std::memory_order_release); }
    } guard{ run };

    for (const auto& item : items) {
        if (run->cancel)
            return;
        if (item.key.empty() || !item.media)
            continue;

        const std::string path = (fs::path(dir) / (item.key + ".jpg")).string();
        std::error_code ec;
        if (fs::exists(path, ec))
            continue; // already generated; no freshness check by design

        std::string err;
        std::shared_ptr<MediaSource> src = item.media->ensureOpen(err);
        if (run->cancel)
            return;
        if (!src)
            continue;

        // The thumbnail bakes the source's pixel aspect in (see fitOnBlackSquare)
        // and the Overview grid shapes its tiles from the cached MediaInfo, which a
        // project saved before that field existed carries as square. This open is
        // the cheapest place to set it right; the value sticks with the next save.
        // Skipped while a proxy mode is active: `src` may then be decoding a
        // substituted file (see ProxyMode.h / Media::ensureOpen), and a proxy
        // with a different pixel aspect than full-res must never silently
        // overwrite the project's saved, full-res metadata.
        if (jplay::currentProxyMode().empty()) {
            if (MediaInfo mi = item.media->info(); mi.pixelAspect != src->pixelAspect()) {
                mi.pixelAspect = src->pixelAspect();
                item.media->setInfo(mi);
            }
        }

        // A proxy's leading slate frames are not part of the shot: they come off
        // the length and are added back to the index actually read.
        const int64_t slate = item.media->slateOffset();
        int64_t count = src->frameCount() - slate;
        int64_t f = item.srcFrame;
        if (count > 0)
            f = std::clamp<int64_t>(f, 0, count - 1);
        else if (f < 0)
            f = 0;

        FramePtr frame = src->readFrame(f + slate);
        if (run->cancel)
            return;
        if (!frame)
            continue;

        writeIfMissing(dir, item.key, *frame);
    }
}

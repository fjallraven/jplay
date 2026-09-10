// Clip thumbnail / grid-view translation unit: the shared per-clip thumbnail
// cache (thumbs_, see ThumbnailCache) plus the player's F3 grid view. Thumbnails
// are generated in the background while the grid is open and freed on shutdown.
// App members, split out of App.cpp.

#include "App.h"
#include "AppInternal.h"
#include "ProxyMode.h"

#include <SDL3_image/SDL_image.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>

namespace fs = std::filesystem;
using namespace jplay;

const Sequence* App::sequenceOfClip(int clipId) const {
    return timeline_.sequenceOfClip(clipId);
}

std::string App::clipThumbKey(const Clip& c) const {
    const Sequence* seq = sequenceOfClip(c.id);
    auto pm = timeline_.findMediaById(c.mediaId);
    std::string path = pm ? pm->path() : std::string();
    // Include full path and sourceOffset so clips from the same file at different
    // positions each get their own thumbnail (stem-only keys caused the first clip's
    // thumbnail to appear for every clip that shared the same source file). The
    // leading tag is the cache format (ThumbnailCache::kFormatTag).
    //
    // Nothing else about the review session enters the key: a tile is the frame's
    // own 8-bit encoding (see renderFrameScaled), so it stays valid across every
    // config, display and view change. The proxy mode is the one exception — it
    // changes which physical file decodes to those pixels (see ProxyMode.h /
    // Media::ensureOpen), so a tile generated under one mode must not be served
    // for another; ThumbnailCache treats an on-disk tile as permanently valid
    // once generated, so without this a switch would leave stale thumbnails.
    return fnv1aHex(ThumbnailCache::kFormatTag + (seq ? seq->name : std::string()) + "\x1f" +
                    path + "\x1f" + std::to_string(c.sourceOffset) + "\x1f" +
                    jplay::currentProxyMode());
}

// (Re)start background generation of a thumbnail for each tile the grid is
// drawing (gridCells_, rebuilt by renderGridView). Clips outside the timeline's
// current zoom window get no tile and so no thumbnail — decoding a frame for
// every clip in the project stalled the workers on cells nobody can see.
void App::startClipThumbnails() {
    thumbs_.setDir(ThumbnailCache::resolveDir(projectHasPath_ ? projectPath_ : std::string()));
    std::vector<ThumbItem> items;
    thumbStartedIds_.clear();
    for (const auto& cell : gridCells_) {
        thumbStartedIds_.push_back(cell.clipId);
        const Clip* clip = clipById(cell.clipId);
        if (!clip || clip->audio)
            continue; // gone with a reload, or audio -> no frames to thumbnail
        auto pm = timeline_.findMediaById(clip->mediaId);
        if (!pm)
            continue; // no source -> placeholder box, nothing to decode
        // Nothing here touches the filesystem or the colour pipeline: the worker
        // skips the items already on disk, so this loop is a hash per tile and
        // stays off the frame budget however many tiles the grid is drawing.
        items.push_back({ clipThumbKey(*clip), pm, clip->sourceOffset + clip->duration / 2 });
    }
    thumbPendingIds_.clear();
    thumbPendingMs_ = 0;
    thumbs_.start(std::move(items));
}

// Called once the grid has laid its tiles out: if the drawn set no longer matches
// what the running pass covers, restart it — but only after the set has held
// still briefly, so zooming or scrolling through it doesn't stop and re-fan the
// workers on every notch.
void App::syncGridThumbnails() {
    std::vector<int> ids;
    ids.reserve(gridCells_.size());
    for (const auto& cell : gridCells_)
        ids.push_back(cell.clipId);

    if (ids == thumbStartedIds_) {
        thumbPendingIds_.clear();
        thumbPendingMs_ = 0;
        return;
    }
    // An empty started-set means no pass covers anything yet (the grid was just
    // opened): start immediately rather than leaving the tiles grey for the delay.
    const Uint64 now = SDL_GetTicks();
    if (!thumbStartedIds_.empty()) {
        if (ids != thumbPendingIds_) {
            thumbPendingIds_ = std::move(ids);
            thumbPendingMs_ = now;
            return;
        }
        if (now - thumbPendingMs_ < 150)
            return;
    }
    startClipThumbnails();
}

// The one place stage_ is written. Each stage owns playerRect_ outright, so
// entering one leaves whichever was up — and that stage's teardown has to run
// whether the user left it with its own key or by pressing the other stage's.
//
// Entering the Overview arms thumbnail generation (the first render starts it for
// the tiles it lays out); leaving stops the workers. Entering the Layout or the
// Stack stands up the comparison scratch sequence and leaving drops it again, so
// that sequence lives exactly as long as the pair of stages does — F4 and F5 hand
// it back and forth rather than rebuilding it. The Layout's slot textures are not
// torn down: they are a handful, and keeping them means toggling back mid-playback
// re-uploads nothing.
void App::setPlayerStage(PlayerStage s) {
    if (s == stage_)
        return;
    // A source view is one clip on a scratch sequence: the Overview has a single
    // tile to lay out and the Layout a single track — and a stack of one has
    // nothing to cycle — so none of them says anything the frame doesn't. The View
    // menu greys those rows out while a view is up; the keys still arrive, so
    // answer them here rather than switching to nothing.
    if (s != PlayerStage::Frame && sourceViewActive()) {
        setStatusWarn("SOURCE VIEW: F2 OR BACKSPACE TO RETURN TO THE TIMELINE", 2000);
        return;
    }
    const bool leftOverview = gridView() && s != PlayerStage::Overview;
    // F4 <-> F5 is a change of presentation, not an entry: the compact state the
    // pair of stages borrowed was taken on the way into the first of them.
    const bool wasCompare = compareStage();
    // Leaving the comparison stages drops the sequence they stood up, putting back
    // the scope, playhead, selection and zoom it borrowed — but only on the way out
    // of both of them: F4 <-> F5 is a change of presentation, not of what is being
    // compared. It parks stage_ on Frame on the way out (the paths that drop a view
    // without naming a stage need that); the assignment below then names the stage
    // actually asked for.
    if (layoutSeqActive() && s != PlayerStage::Layout && s != PlayerStage::Stack)
        dropScratchView();
    stage_ = s;
    if (gridView()) {
        // Drop last visit's tile positions so tiles appear in place rather than
        // gliding in from wherever the grid happened to be when it was closed.
        gridTileRects_.clear();
        gridAnimMs_ = 0;
        // Open on whatever is playing rather than at the top of a long contact
        // sheet; the next render knows where that clip's tile landed.
        gridScrollToLive_ = true;
        // Nothing is thumbnailed until the grid has laid out its tiles: the first
        // render's syncGridThumbnails starts the pass for what it actually drew.
        gridCells_.clear();
        thumbStartedIds_.clear();
        thumbPendingIds_.clear();
    } else if (leftOverview) {
        thumbs_.stop();
        thumbStartedIds_.clear();
        thumbPendingIds_.clear();
    }
    // The stage is its scratch sequence: with nothing to compare there is no stage
    // to enter, so fall back to the frame (openLayoutView set the status saying so).
    if (compareStage() && !layoutSeqActive() && !openLayoutView())
        stage_ = PlayerStage::Frame;
    // F4 <-> F5 hand the one sequence back and forth, so the name it was stood up
    // with is not the stage that is up. The scope bar shows that name, so it has to
    // follow the presentation rather than say where the comparison started.
    if (compareStage() && layoutSeqActive())
        if (Sequence* sq = timeline_.findSequenceById(scratchSeqId_))
            sq->name = stackView() ? "STACK" : "LAYOUT";
    // Both comparison stages run compact, and stay there: the tiles want the
    // height, and the track stack under them is one row per tile / per stack entry.
    // The docked pane is left alone — entering a stage is not the "show me the
    // image alone" that TAB's own compact is. Leaving hands back whatever the user
    // had before the stage borrowed it, which for a compact user is compact still.
    if (compareStage()) {
        if (!wasCompare)
            compactBeforeCompare_ = compactTimeline_;
        setCompactTimeline(true, /*closePanes=*/false, /*persist=*/false);
    } else if (wasCompare) {
        restoreCompactAfterCompare();
    }
    // A stage change is not a cycle, so the last one's overlay does not carry into
    // it — and coming back to the Stack must not resume a countdown from before.
    stackOverlayUntil_ = 0;
}

// A left-click on a grid tile. A plain click parks the playhead on the clip so it
// becomes the live tile; a double-click also marks the playback range to it, and
// Shift+click grows the range to reach it without moving the playhead — the same
// three gestures the timeline's own clip body answers to. Only the range's frames
// are cached (see submitCacheRequests), so a range that would leave the playhead
// outside it pulls the playhead in.
void App::gridClickClip(const Clip& c, bool doubleClick, bool shiftHeld) {
    const int64_t cs = c.timelineStart;
    const int64_t ce = c.end() - 1; // outPoint is the last playable frame
    auto markStatus = [&] {
        setStatus("RANGE: " + std::to_string(timeline_.inPoint) + " - " +
                  std::to_string(timeline_.outPoint));
    };

    // Shift wins over the click count: a shift+double-click extends twice (the
    // union is idempotent) rather than extending and then jumping the playhead.
    if (shiftHeld) {
        if (timeline_.outPoint >= 0) {
            timeline_.inPoint  = std::min(timeline_.inPoint, cs);
            timeline_.outPoint = std::max(timeline_.outPoint, ce);
        } else if (const Clip* live = getTopMostClipAtFrame(timeline_.playhead)) {
            // Nothing marked yet: span from the clip the playhead is on to this one,
            // which is what "extend to here" means with no range to extend — and
            // leaves the playhead inside the result.
            timeline_.inPoint  = std::min(live->timelineStart, cs);
            timeline_.outPoint = std::max(live->end() - 1, ce);
        } else {
            timeline_.inPoint  = cs; // playhead in a gap: the clicked clip alone
            timeline_.outPoint = ce;
        }
        if (timeline_.playhead < timeline_.inPoint || timeline_.playhead > timeline_.outPoint)
            setPlayhead(timeline_.inPoint);
        markStatus();
        return;
    }

    setPlayhead(cs);
    if (doubleClick) {
        timeline_.inPoint  = cs;
        timeline_.outPoint = ce;
        markStatus();
    }
}

// Ctrl+wheel over the grid resizes the tiles. The size is the grid's own — it
// says nothing about the timeline — so it is clamped to a sane band and kept in
// the user's prefs rather than recomputed per project.
void App::gridSetThumbH(float h) {
    float clamped = std::clamp(h, 24.0f, 480.0f);
    if (clamped == gridThumbH_)
        return;
    gridThumbH_ = clamped;
    writePrefs();
}

void App::freeOverviewTextures() {
    for (auto& kv : thumbTex_)
        if (kv.second)
            SDL_DestroyTexture(kv.second);
    thumbTex_.clear();
}

// Lazily load <key>.jpg from `cache` into `texMap` the first time it exists on
// disk; cached thereafter. Returns null while the background worker hasn't
// produced it yet (the caller then draws the dark-grey placeholder). Shared by
// the Overview grid and the SOURCES bin, which keep separate caches/texture maps.
SDL_Texture* App::loadThumbTexture(ThumbnailCache& cache,
                                   std::unordered_map<std::string, SDL_Texture*>& texMap,
                                   const std::string& key) {
    auto it = texMap.find(key);
    if (it != texMap.end())
        return it->second;

    // A tile the workers haven't written yet is probed at most every kThumbProbeMs
    // rather than once per frame: a grid full of placeholders would otherwise cost
    // a filesystem stat per tile per frame, which is felt at once when the
    // thumbnail directory is on a share.
    const Uint64 now = SDL_GetTicks();
    auto miss = thumbMissAt_.find(key);
    if (miss != thumbMissAt_.end() && now - miss->second < kThumbProbeMs)
        return nullptr;

    std::string path = cache.pathFor(key);
    std::error_code ec;
    if (path.empty() || !fs::exists(path, ec)) {
        thumbMissAt_[key] = now;
        return nullptr;
    }
    thumbMissAt_.erase(key);

    SDL_Surface* s = IMG_Load(path.c_str());
    if (!s)
        return nullptr;
    SDL_Texture* tex = SDL_CreateTextureFromSurface(renderer_, s);
    SDL_DestroySurface(s);
    if (tex)
        SDL_SetTextureScaleMode(tex, SDL_SCALEMODE_LINEAR);
    texMap[key] = tex; // cache even if null-on-failure to avoid re-attempting every frame
    return tex;
}

// Grid view: load a per-clip thumbnail. Returns null (caller draws a
// placeholder) until the background worker has produced it.
SDL_Texture* App::overviewThumb(const std::string& key) {
    return loadThumbTexture(thumbs_, thumbTex_, key);
}

// Player grid view: clips laid out as cells shaped to each clip's own aspect
// ratio (no letterbox bars), filling the player area without scrolling. Grouped
// by sequence — each sequence starts under a name header, its clips wrapped into
// centered rows at a uniform cell height. The focused sequence only when filtered,
// otherwise every sequence. The clip under the playhead (liveClip) plays live in
// its cell via texture_; every other cell shows the (bar-cropped) Overview
// thumbnail. gridCells_ is rebuilt for hit-testing.
void App::renderGridView(const Clip* liveClip) {
    gridCells_.clear();
    gridSb_.bar = {}; // refilled below if this render draws a scrollbar

    auto sortClips = [](std::vector<const Clip*>& v) {
        std::sort(v.begin(), v.end(), [](const Clip* a, const Clip* b) {
            if (a->timelineStart != b->timelineStart)
                return a->timelineStart < b->timelineStart;
            return a->track < b->track;
        });
    };

    // Every clip of the sequences in scope, whatever the timeline below is zoomed
    // to: the grid is a contact sheet of the scope, not a mirror of the timeline
    // window. Audio clips have no frames to show.
    auto visible = [](const Clip& c) { return !c.audio; };

    // Group clips by sequence: the in-view sequences that have clips, in timeline
    // order — one group when a single sequence is scoped, the project's groups when
    // a project is, otherwise every sequence.
    struct SeqGroup {
        int seqIdx = -1; std::string name; std::vector<const Clip*> clips;
    };
    std::vector<SeqGroup> groups;
    auto addGroup = [&](int seqIdx, const Sequence& seq) {
        SeqGroup g;
        g.seqIdx = seqIdx;
        g.name = seq.name;
        for (const auto& c : seq.clips)
            if (visible(c))
                g.clips.push_back(&c);
        if (!g.clips.empty()) {
            sortClips(g.clips);
            groups.push_back(std::move(g));
        }
    };
    for (int si : viewSeqIndices())
        addGroup(si, timeline_.sequences[si]);

    if (groups.empty()) {
        drawText(playerRect_.x + 16.0f, playerRect_.y + 16.0f,
                 SDL_Color{ 120, 124, 132, 255 }, "No clips");
        gridTileRects_.clear();
        gridScroll_ = 0.0f;
        syncGridThumbnails(); // nothing drawn -> let the running pass wind down
        return;
    }

    const float pad = 10.0f * dpiScale;
    const float gap = 6.0f * dpiScale;
    const float hdrH = textFont_.lineHeight() + 6.0f * dpiScale; // sequence-name row
    const float secGap = 12.0f * dpiScale;                       // space between sequences
    // The scrollbar strip is reserved whether or not it is drawn: taking it out of
    // the wrap width only when the content overflows would let a row that just fits
    // push the content taller, which brings the bar in, which rewraps the row.
    const float sbW = scrollbarStripW(dpiScale, true);
    const float areaX = playerRect_.x + pad;
    const float areaY = playerRect_.y + pad;
    const float areaW = playerRect_.w - 2 * pad - sbW;
    const float areaH = playerRect_.h - 2 * pad;
    if (areaW <= 0.0f || areaH <= 0.0f)
        return;

    // Per-clip display aspect ratio (width/height with the pixel aspect applied,
    // which is how the thumbnail was baked), memoized per source. Media::info()
    // returns cached metadata (no decode), so this stays cheap even for many
    // clips; memoizing per media id collapses clips that share a source.
    std::unordered_map<std::string, float> arCache;
    auto clipAr = [&](const Clip* c) -> float {
        if (!c->mediaId.empty()) {
            auto it = arCache.find(c->mediaId);
            if (it != arCache.end())
                return it->second;
        }
        float a = 0.0f;
        if (auto media = timeline_.findMediaById(c->mediaId)) {
            MediaInfo info = media->info();
            if (info.width > 0 && info.height > 0)
                a = (float)info.width * (info.pixelAspect > 0.0f ? info.pixelAspect : 1.0f)
                    / (float)info.height;
        }
        if (a <= 0.0f) // dimensions not cached yet: live frame for the current clip, else 16:9
            a = (liveClip && c->id == liveClip->id && texW_ > 0 && texH_ > 0)
                    ? texDispW() / (float)texH_ : 16.0f / 9.0f;
        if (!c->mediaId.empty())
            arCache[c->mediaId] = a;
        return a;
    };

    // Tile height is the size the user picked, held down where the area itself is
    // the limit (shorter than one tile, or too narrow for the widest clip's tile to
    // fit a row at all) and pushed up where the picked size would leave the sheet
    // part-empty: too few clips to fill the area grow their tiles into the slack
    // instead of leaving a margin down the right and bottom.
    float maxAr = 1.0f;
    for (const auto& g : groups)
        for (const Clip* c : g.clips)
            maxAr = std::max(maxAr, clipAr(c));

    // Content height for a candidate tile height, wrapping rows exactly as the
    // layout below does. Monotone in h (taller tiles are wider, so rows wrap no
    // later and each is taller), which is what makes the fit a bisection.
    auto measureH = [&](float h) {
        float y = 0.0f;
        for (const auto& g : groups) {
            y += hdrH;
            float x = 0.0f;
            for (const Clip* clip : g.clips) {
                const float w = h * clipAr(clip);
                if (x > 0.0f && x + gap + w > areaW) { y += h + gap; x = 0.0f; }
                if (x > 0.0f)
                    x += gap;
                x += w;
            }
            y += h + secGap;
        }
        return y - secGap;
    };

    const float hMax = std::min(areaH, areaW / maxAr); // one tile filling the area
    float cellH = std::min(gridThumbH_, hMax);
    if (cellH <= 0.0f)
        return;
    if (measureH(cellH) < areaH) {
        if (measureH(hMax) <= areaH) {
            cellH = hMax;
        } else {
            float lo = cellH, hi = hMax; // tallest tile whose content still fits
            for (int i = 0; i < 20; ++i) {
                const float mid = 0.5f * (lo + hi);
                if (measureH(mid) <= areaH)
                    lo = mid;
                else
                    hi = mid;
            }
            cellH = lo;
        }
    }

    // Lay the groups out in content coordinates — y measured from the top of the
    // content rather than of the screen — so the total height is known before
    // anything is drawn and the scroll offset can be clamped against it.
    struct Tile { const Clip* c; const SeqGroup* g; SDL_FRect r; };
    struct Header { const SeqGroup* g; float y; };
    std::vector<Tile> tiles;
    std::vector<Header> headers;
    float contentH = 0.0f;
    {
        float y = 0.0f;
        for (const auto& g : groups) {
            headers.push_back({ &g, y });
            y += hdrH;
            float x = 0.0f; // greedy row wrap: a row holds as many tiles as fit areaW
            for (const Clip* clip : g.clips) {
                const float w = cellH * clipAr(clip);
                if (x > 0.0f && x + gap + w > areaW) { y += cellH + gap; x = 0.0f; }
                if (x > 0.0f)
                    x += gap;
                tiles.push_back({ clip, &g, { x, y, w, cellH } });
                x += w;
            }
            y += cellH + secGap;
        }
        contentH = y - secGap; // the last group takes no trailing gap
    }

    // The pending "put this on screen" request, resolved now that the tiles have
    // places. One-shot: after this the scroll is the user's.
    const float maxScroll = std::max(0.0f, contentH - areaH);
    if (gridScrollToLive_) {
        if (liveClip)
            for (const auto& t : tiles)
                if (t.c->id == liveClip->id) {
                    gridScroll_ = t.r.y - (areaH - cellH) * 0.5f; // its row, centred
                    break;
                }
        gridScrollToLive_ = false;
    }
    // Dragging the playhead onto another clip pulls its row into view when it is
    // offscreen: just far enough past whichever edge it fell behind, so following
    // the playhead never re-centres a grid the user has scrolled somewhere.
    if (liveClip && liveClip->id != gridLiveClip_) {
        for (const auto& t : tiles)
            if (t.c->id == liveClip->id) {
                if (t.r.y - gap < gridScroll_)
                    gridScroll_ = t.r.y - gap; // its header side
                else if (t.r.y + cellH + gap > gridScroll_ + areaH)
                    gridScroll_ = t.r.y + cellH + gap - areaH;
                break;
            }
    }
    gridLiveClip_ = liveClip ? liveClip->id : 0;
    gridScroll_ = std::clamp(gridScroll_, 0.0f, maxScroll);
    const float oy = areaY - gridScroll_; // content y -> screen y

    float mx = 0.0f, my = 0.0f;
    uiMouse(mx, my);
    // The rows just off the top and bottom are laid out (and so hit-testable) but
    // clipped away, so hover only counts while the cursor is over the grid itself.
    const bool mouseOverGrid = inRect(playerRect_, mx, my);

    // Running the cursor along the ruler outlines whichever clip that frame
    // belongs to, so the grid reads as a map of the timeline while scrubbing.
    int rulerHoverClip = 0;
    if (tlHoverActive_) {
        const int64_t len = timeline_.length();
        if (len > 0) {
            const int64_t hf = std::clamp<int64_t>(scrubFrame(tlHoverX_), 0, len - 1);
            if (const Clip* hc = getTopMostClipAtFrame(hf))
                rulerHoverClip = hc->id;
        }
    }

    // Tiles glide to their new slot instead of snapping there, so a reflow reads as
    // motion rather than a jump. Exponential smoothing on the drawn rect, stepped by
    // the render dt so it settles in ~120ms at any framerate.
    const Uint64 nowMs = SDL_GetTicks();
    const float dt = gridAnimMs_ ? (float)(nowMs - gridAnimMs_) / 1000.0f : 0.0f;
    gridAnimMs_ = nowMs;
    const float lerpK = dt > 0.0f ? 1.0f - std::exp(-std::min(dt, 0.1f) / 0.05f) : 1.0f;

    // `target` is in content coordinates, and so is the glide: a tile chases where
    // it sits in the layout, not where it sits on screen, so scrolling slides it
    // rigidly with the rest instead of leaving it lagging behind the drag.
    auto drawTile = [&](const Clip* c, const SDL_FRect& target) {
        // Glide from wherever this tile was last laid out; one that wasn't in the
        // layout last frame just appears at its target.
        SDL_FRect rect = target;
        auto prev = gridTileRects_.find(c->id);
        if (prev != gridTileRects_.end() && lerpK < 1.0f) {
            const SDL_FRect& p = prev->second;
            rect.x = p.x + (target.x - p.x) * lerpK;
            rect.y = p.y + (target.y - p.y) * lerpK;
            rect.w = p.w + (target.w - p.w) * lerpK;
            rect.h = p.h + (target.h - p.h) * lerpK;
            if (std::fabs(target.x - rect.x) < 0.5f && std::fabs(target.y - rect.y) < 0.5f &&
                std::fabs(target.w - rect.w) < 0.5f && std::fabs(target.h - rect.h) < 0.5f)
                rect = target; // settled: stop crawling by sub-pixels
        }
        gridTileRects_[c->id] = rect;
        rect.x += areaX; // content -> screen
        rect.y += oy;

        const bool isLive = liveClip && c->id == liveClip->id;
        // Black backing for the live cell; dark-grey placeholder for the rest
        // (shows until the thumbnail BMP has been generated).
        SDL_SetRenderDrawColor(renderer_, isLive ? 0 : 52, isLive ? 0 : 54, isLive ? 0 : 60, 255);
        jplay::fillRect(renderer_, &rect);

        // The player's texture is only this clip's frame once the decode for the
        // new playhead has landed; until then it still holds whatever was on
        // screen before — a different source after a click on another tile. Match
        // the displayed key against what this cell is standing for and fall back
        // to the clip's own thumbnail while they disagree.
        const bool liveTexReady =
            isLive && displayedKey_ == CacheKey{ c->mediaId,
                c->sourceOffset + (timeline_.playhead - c->timelineStart) };

        bool drewLive = false;
        if (liveTexReady && hasTexture_ && texture_ && texW_ > 0 && texH_ > 0) {
            float fit = std::min(rect.w / texDispW(), rect.h / (float)texH_);
            float dw = texDispW() * fit, dh = texH_ * fit;
            SDL_FRect dst = { rect.x + (rect.w - dw) * 0.5f, rect.y + (rect.h - dh) * 0.5f, dw, dh };
            SDL_RenderTexture(renderer_, texture_, nullptr, &dst);
            drewLive = true;
        }
        if (!drewLive && !c->mediaId.empty()) {
            if (SDL_Texture* tex = overviewThumb(clipThumbKey(*c))) {
                // Crop the baked-in black letterbox from the square thumbnail using
                // this clip's own aspect, so the frame fills the cell without bars.
                float a = clipAr(c);
                const float S = (float)ThumbnailCache::kSize;
                float cw = a >= 1.0f ? S : S * a;
                float ch = a >= 1.0f ? S / a : S;
                SDL_FRect src = { (S - cw) * 0.5f, (S - ch) * 0.5f, cw, ch };
                SDL_RenderTexture(renderer_, tex, &src, &rect);
            }
        }

        // Shot name across the tile's foot, over a scrim so it reads on any frame.
        // Clips with no shot fall back to the source stem, which is what the
        // timeline labels them with. Dropped once the cell is too short for the bar
        // to be anything but an obstruction, or too narrow to fit any of the name.
        {
            const Shot* sh = timeline_.findShotById(c->shotId);
            std::string name = sh ? sh->name : std::string();
            if (name.empty()) {
                if (auto pm = timeline_.findMediaById(c->mediaId))
                    name = hashSeqStem(fs::path(pm->path()).stem().string(),
                                       pm->type() == ClipType::ImageSequence);
            }
            const float lineH = textFont_.lineHeight();
            const float barH = lineH + 2.0f * dpiScale;
            if (!name.empty() && rect.h >= lineH * 3.0f) {
                std::string fitted = fitText(name, rect.w - 6.0f * dpiScale);
                if (!fitted.empty()) {
                    SDL_FRect bar = { rect.x, rect.y + rect.h - barH, rect.w, barH };
                    SDL_SetRenderDrawColor(renderer_, 0, 0, 0, 150);
                    jplay::fillRect(renderer_, &bar);
                    drawText(rect.x + 3.0f * dpiScale, bar.y + (barH - lineH) * 0.5f,
                             SDL_Color{ 232, 235, 242, 255 }, fitted);
                }
            }
        }

        if (isLive) {
            SDL_SetRenderDrawColor(renderer_, 90, 150, 240, 255);
            jplay::drawRect(renderer_, &rect);
            SDL_FRect inner = { rect.x + 1, rect.y + 1, rect.w - 2, rect.h - 2 };
            jplay::drawRect(renderer_, &inner);
        } else if ((mouseOverGrid && inRect(rect, mx, my)) ||
                   (rulerHoverClip && c->id == rulerHoverClip)) {
            SDL_SetRenderDrawColor(renderer_, 200, 210, 235, 220);
            jplay::drawRect(renderer_, &rect);
        }

        gridCells_.push_back({ c->id, rect });
    };

    // Draw what the scroll offset puts on screen, plus a row's worth either side:
    // those neighbours are clipped away, but registering them as cells gets their
    // thumbnails decoded before the scroll reaches them.
    const float cullPad = cellH + gap;
    const SDL_Rect clipRect = { (int)playerRect_.x, (int)playerRect_.y,
                                (int)playerRect_.w, (int)playerRect_.h };
    SDL_SetRenderClipRect(renderer_, &clipRect);

    for (const auto& h : headers) {
        const float y = h.y + oy;
        if (y + hdrH < areaY || y > areaY + areaH)
            continue;
        const SeqGroup& g = *h.g;
        if (!g.name.empty()) {
            const float lh = textFont_.lineHeight();
            drawText(areaX, y + (hdrH - lh) * 0.5f,
                     SDL_Color{ 225, 228, 235, 255 }, g.name);
        }
        SDL_SetRenderDrawColor(renderer_, 70, 74, 84, 255);
        jplay::drawLine(renderer_, areaX, y + hdrH - 3.0f, areaX + areaW, y + hdrH - 3.0f);
    }

    for (const auto& t : tiles) {
        const float y = t.r.y + oy;
        if (y + cellH < areaY - cullPad || y > areaY + areaH + cullPad)
            continue;
        drawTile(t.c, t.r);
    }

    SDL_SetRenderClipRect(renderer_, nullptr);
    const SDL_FRect sbView{ areaX, areaY, areaW + sbW, areaH };
    // Grabbable, so this also records what the bar maps onto for a press on it.
    drawScrollbar(renderer_, sbView, contentH, gridScroll_, dpiScale, false, gridSb_);

    // Hover tooltip: the full identity of the tile under the cursor, since the tile
    // itself carries only the shot name. Drawn after every tile so it overlays its
    // neighbours, and held inside the player area so the panels drawn later can't
    // cut across it.
    for (const auto& cell : gridCells_) {
        if (!mouseOverGrid || !inRect(cell.rect, mx, my))
            continue;
        const Clip* clip = clipById(cell.clipId);
        if (!clip)
            break;
        std::vector<std::string> rows;
        if (const Sequence* sq = sequenceOfClip(clip->id); sq && !sq->name.empty())
            rows.push_back("Sequence: " + sq->name);
        if (const Shot* sh = timeline_.findShotById(clip->shotId); sh && !sh->name.empty())
            rows.push_back("Shot: " + sh->name);
        if (auto pm = timeline_.findMediaById(clip->mediaId)) {
            const std::string& dept = pm->metaValue("department");
            if (!dept.empty())
                rows.push_back("Department: " + dept);
            rows.push_back("Source: " + fs::path(pm->path()).filename().string());
        }
        if (rows.empty())
            break;

        const float pad = 6.0f * dpiScale;
        const float lineH = textFont_.lineHeight();
        float bw = 0.0f;
        for (const auto& row : rows)
            bw = std::max(bw, textFont_.measure(renderer_, row.c_str()));
        bw += pad * 2.0f;
        const float bh = lineH * (float)rows.size() + pad * 2.0f;
        // Offset off the cursor, then pulled back inside the player area.
        const float bx = std::clamp(mx + 14.0f * dpiScale, playerRect_.x,
                                    std::max(playerRect_.x, playerRect_.x + playerRect_.w - bw));
        const float by = std::clamp(my + 16.0f * dpiScale, playerRect_.y,
                                    std::max(playerRect_.y, playerRect_.y + playerRect_.h - bh));
        SDL_FRect box = { bx, by, bw, bh };
        SDL_SetRenderDrawColor(renderer_, 20, 21, 24, 240);
        jplay::fillRect(renderer_, &box);
        SDL_SetRenderDrawColor(renderer_, 80, 82, 90, 255);
        jplay::drawRect(renderer_, &box);
        float ty = by + pad;
        for (const auto& row : rows) {
            drawText(bx + pad, ty, SDL_Color{ 235, 238, 245, 255 }, row);
            ty += lineH;
        }
        break;
    }

    // Forget tiles that left the view, so a clip zoomed back into view appears at
    // its slot instead of gliding in from wherever it sat when it left. gridCells_
    // is the list of everything just drawn.
    std::unordered_map<int, SDL_FRect> live;
    live.reserve(gridCells_.size());
    for (const auto& cell : gridCells_) {
        auto it = gridTileRects_.find(cell.clipId);
        if (it != gridTileRects_.end())
            live.emplace(cell.clipId, it->second);
    }
    gridTileRects_.swap(live);

    // Thumbnail exactly the tiles just drawn (once the set has settled).
    syncGridThumbnails();
}

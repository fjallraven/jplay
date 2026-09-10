// Layout stage translation unit: the player shows every track carrying a clip at
// the playhead, tiled, all of them playing live. The Stack stage at the bottom of
// the file is its other half — the same clips, one at a time. App members, split
// out of App_Player.cpp.
//
// Entering either stage stands up a scratch sequence out of the clips being
// compared — the timeline selection, or the clip under the playhead when nothing
// is selected — one clip per video track (see openLayoutView). Leaving both of
// them drops that sequence again. It is a Sequence::temporary like the source view
// is, so the cut the comparison was launched from is untouched throughout and
// nothing is added to the project format.
//
// The tile set itself is derived, never stored. Sequences are concatenated end to
// end (Timeline::repackSequences), so exactly one is live at any frame and the
// tiles are simply that sequence's rows, top first — one per row that carries a
// clip, whether or not that clip reaches the frame on screen, so the arrangement
// holds still while the playhead moves and a slot with nothing to show just goes
// black (see layoutClipsAt).
//
// One tile is the *program*: renderPlayer builds texture_ from the top-most clip
// at the playhead, so that tile alone carries the annotations, the letterbox, and
// whatever the output devices and the review window are fed. Its index is
// layoutProgramTile_ — the row that clip sits on, which is the first row only
// while the first row has a frame here. The rest are comparison images and stop at
// the display transform — no readback, and no compositing (a dissolve or fade on a
// lower track shows its outgoing clip alone).

#include "App.h"
#include "AppInternal.h"

#include <algorithm>
#include <cmath>
#include <filesystem>

namespace fs = std::filesystem;
using namespace jplay;

namespace {
// Gap between tiles and inset from the player edge, in logical units.
constexpr float kTileGap = 4.0f;
constexpr float kTilePad = 6.0f;
constexpr SDL_Color kTileBorder{ 70, 74, 82, 255 };
constexpr SDL_Color kTileLabel{ 232, 235, 242, 255 };

// Stack stage: how long the basename overlay stays up after a cycle, and how much
// of that is spent fading out rather than holding — a hard cut to nothing reads as
// a glitch at the end of a spin through the stack.
constexpr Uint64 kStackOverlayMs = 1000;
constexpr Uint64 kStackFadeMs    = 250;
// The three lines it draws: the program's name, then the two under it in the
// rotation, each fainter. Alpha, not colour: they are the same white.
constexpr int   kStackOverlayLines = 3;
constexpr Uint8 kStackLineAlpha[kStackOverlayLines] = { 255, 150, 80 };
} // namespace

// L toggles the player stage between the single video frame and the tiled tracks.
void App::toggleLayoutView() {
    setPlayerStage(layoutView() ? PlayerStage::Frame : PlayerStage::Layout);
}

namespace {
// Frame number the first frame of `m` carries, which is a property of an image
// sequence's file names and known only to its open decoder; video and audio are
// 0-based. Same rule as App::alignedSourceRange and OtioExport use, so the three
// agree on what an absolute source frame is.
int64_t firstFrameOf(Media& m) {
    if (m.type() != ClipType::ImageSequence)
        return 0;
    std::string err;
    if (auto src = m.ensureOpen(err))
        return src->firstFrameNumber();
    return 0;
}
} // namespace

// Stand up the scratch sequence the stage tiles.
//
// The clips are the timeline selection, or the clip under the playhead when
// nothing is selected — so "compare these" is a selection plus one key, and
// "compare what is stacked here" is selecting that stack first. Audio is dropped:
// it has no frames to tile.
//
// They are *copied*, not re-added from their media. A copy keeps the cut's source
// range, its annotations, fades and volume curve, and costs neither a second pool
// entry nor an audio-pairing query — where addMediaFileAt would re-derive the
// extent from the file and place the whole source.
//
// Placement is by absolute source frame: the earliest first frame among them
// becomes the sequence's own frame 0 and every clip sits at its own offset from
// it, so two versions of a shot with different handles line up on content rather
// than on where they happened to be cut. Nothing is trimmed to a reference — each
// keeps its full cut range, and a tile simply starts or ends where its source does.
bool App::openLayoutView() {
    // Copies rather than pointers: standing the scratch sequence up below repacks
    // the timeline out from under them.
    std::vector<Clip> picked;
    for (int id : selectedClipIds_)
        if (const Clip* c = timeline_.findClipById(id); c && !c->audio)
            picked.push_back(*c);
    if (picked.empty()) {
        if (const Clip* clip = getTopMostClipAtFrame(timeline_.playhead))
            picked.push_back(*clip);
    }
    if (picked.empty()) {
        setStatusWarn(stackView()
                          ? "NOTHING TO STACK: SELECT CLIPS, OR PARK THE PLAYHEAD ON ONE"
                          : "NOTHING TO LAY OUT: SELECT CLIPS, OR PARK THE PLAYHEAD ON ONE",
                      3000);
        return false;
    }
    // Track order then time, so the cut's top track stays the program tile and a
    // multi-clip selection reads the way it did on the timeline.
    std::stable_sort(picked.begin(), picked.end(), [](const Clip& a, const Clip& b) {
        return a.track != b.track ? a.track < b.track : a.timelineStart < b.timelineStart;
    });

    // Absolute source frame each clip starts on, and the earliest of them, which
    // becomes the sequence's frame 0.
    std::vector<int64_t> absIn(picked.size(), 0);
    int64_t minAbs = INT64_MAX;
    for (size_t i = 0; i < picked.size(); ++i) {
        int64_t base = 0;
        if (auto media = timeline_.findMediaById(picked[i].mediaId))
            base = firstFrameOf(*media);
        absIn[i] = base + picked[i].sourceOffset;
        minAbs = std::min(minAbs, absIn[i]);
    }

    // The frame on screen right now, in the same absolute terms, so the playhead
    // can be put back on it once the sequence is up. -1 when the playhead is not
    // over any of the picked clips (a selection made elsewhere in the cut), in
    // which case the layout opens on its own first frame.
    int64_t shownAbs = -1;
    for (size_t i = 0; i < picked.size(); ++i) {
        const Clip& clip = picked[i];
        if (timeline_.playhead >= clip.timelineStart && timeline_.playhead < clip.end()) {
            shownAbs = absIn[i] + (timeline_.playhead - clip.timelineStart);
            break;
        }
    }

    // Named after the stage standing it up, since that name is what the scope bar
    // shows; the other stage borrows the sequence and renames it (setPlayerStage).
    const int idx = beginScratchSequence(stackView() ? "STACK" : "LAYOUT",
                                         ScratchKind::Layout);
    const int64_t base = timeline_.seqRegions()[idx].start;
    Sequence& seq = timeline_.sequences[idx];
    seq.clips.reserve(picked.size());
    for (size_t i = 0; i < picked.size(); ++i) {
        Clip clip = std::move(picked[i]);
        clip.id = nextClipId_++;
        clip.track = (int)i;          // one per row, top first: the tile order
        clip.timelineStart = base + (absIn[i] - minAbs);
        clip.shotId = -1;             // the shots belong to the cut, not to this view
        clip.linkedTo = 0;            // its audio parent stayed behind
        clip.linkOffset = 0;
        clip.hidden = false;          // a row hidden in the cut is one you asked to compare
        seq.clips.push_back(std::move(clip));
    }
    scratchTrackCount_ = (int)seq.clips.size();
    timeline_.repackSequences();

    // Fit and park. scopeToSequence ran while the sequence was still empty, so the
    // zoom it chose has nothing to do with what is in it now.
    clearClipSelection(); // the originals are out of scope; the copies are not them
    fitToFilteredSequence();
    int64_t a = 0, b = 0;
    const int64_t start = shownAbs >= 0 ? base + (shownAbs - minAbs) : base;
    setPlayhead(timeline_.sequenceSpan(timeline_.sequences[idx], a, b)
                    ? std::clamp(start, a, std::max(a, b - 1))
                    : start);
    return true;
}

void App::freeLayoutSlots() {
    for (LayoutSlot& s : layoutSlots_)
        if (s.tex)
            SDL_DestroyTexture(s.tex);
    layoutSlots_.clear();
}

// One entry per video row in the view, top row first: the clip visible on it at
// `frame`, or null where the row has nothing there.
//
// The entry count is the *row* count, not the count of clips that happen to reach
// this frame — so the tiling stays put while the playhead moves. Each source keeps
// its cell for as long as the stage is up, and a clip whose range has run out
// leaves a black slot behind rather than repacking every other tile around it,
// which on a comparison is the whole point: the eye has to be able to keep looking
// at the same spot.
//
// The filter on what a slot *shows* is getTopMostClipAtFrame's — audio has no
// frames, and a disabled clip is the user saying "not this one", which here means
// its slot goes dark rather than letting another row show through it. Rows are
// counted over every video clip regardless, so disabling one does not re-arrange
// the stage either.
//
// Overlap on a track is prevented at edit time (Timeline::trackHasOverlap), so a
// row resolves to at most one clip and needs no tie-break.
void App::layoutClipsAt(int64_t frame, std::vector<const Clip*>& out) const {
    int rows = 0;
    forEachViewClip([&](const Clip& c) {
        if (!c.audio && c.track + 1 > rows)
            rows = c.track + 1;
    });
    out.assign((size_t)rows, nullptr);
    forEachViewClip([&](const Clip& c) {
        if (c.audio || timeline_.clipDisabled(c) || c.track >= rows)
            return;
        if (frame >= c.timelineStart && frame < c.end())
            out[(size_t)c.track] = &c;
    });
}

// The clip a row belongs to, whatever the playhead is doing — what a slot with
// nothing to show still names itself after. Earliest first, for the rows the user
// has since dropped a second clip onto.
const Clip* App::layoutRowClip(int track) const {
    const Clip* best = nullptr;
    forEachViewClip([&](const Clip& c) {
        if (c.audio || c.track != track)
            return;
        if (!best || c.timelineStart < best->timelineStart)
            best = &c;
    });
    return best;
}

// Auto-pack `n` cells of display aspect `ar` over the player area: of every
// rows x cols arrangement that holds n, take the one whose cell fits the largest
// image, then shrink the cells to exactly that image and centre the block.
//
// The shrink is what stops a tile being a wide box with the image floating in the
// middle of it: the cell *is* the image, so the border hugs the frame and all the
// slack ends up at the edges of the stage where it reads as margin instead of as
// dead space inside each tile. A zoomed tile then overflows its cell and is
// clipped to it, which is the point.
//
// Rows are left-aligned, so a short last row leaves its gap on the right and the
// tiles stay in track order down the columns.
void App::buildLayoutTiles(int n, float ar) {
    layoutTiles_.clear();
    if (n <= 0)
        return;
    if (ar <= 0.0f)
        ar = 16.0f / 9.0f;
    const float pad = kTilePad * dpiScale;
    const float gap = kTileGap * dpiScale;
    const float areaX = playerRect_.x + pad;
    const float areaY = playerRect_.y + pad;
    const float areaW = playerRect_.w - 2 * pad;
    const float areaH = playerRect_.h - 2 * pad;
    if (areaW <= 0.0f || areaH <= 0.0f)
        return;

    int bestCols = 1;
    float bestScore = -1.0f;
    for (int cols = 1; cols <= n; ++cols) {
        const int rows = (n + cols - 1) / cols;
        const float cw = (areaW - gap * (cols - 1)) / (float)cols;
        const float ch = (areaH - gap * (rows - 1)) / (float)rows;
        if (cw <= 0.0f || ch <= 0.0f)
            continue;
        // How big the image itself lands in that cell, which is what the eye
        // judges — not the cell area, which would favour cells of the wrong shape.
        const float score = std::min(cw / ar, ch);
        if (score > bestScore) {
            bestScore = score;
            bestCols = cols;
        }
    }
    if (bestScore <= 0.0f)
        return; // no arrangement fits (a player area smaller than the gaps)
    const int cols = bestCols;
    const int rows = (n + cols - 1) / cols;
    // The cell is the fitted image: bestScore is its height, ar gives its width.
    const float ch = bestScore;
    const float cw = ch * ar;
    // Centre the whole block in the stage, so the slack becomes an even margin.
    const float blockW = cw * cols + gap * (cols - 1);
    const float blockH = ch * rows + gap * (rows - 1);
    const float ox = areaX + (areaW - blockW) * 0.5f;
    const float oy = areaY + (areaH - blockH) * 0.5f;
    layoutTiles_.reserve(n);
    for (int i = 0; i < n; ++i) {
        const int r = i / cols, c = i % cols;
        layoutTiles_.push_back({ ox + c * (cw + gap), oy + r * (ch + gap), cw, ch });
    }
}

int App::layoutTileAt(float x, float y) const {
    for (size_t i = 0; i < layoutTiles_.size(); ++i)
        if (inRect(layoutTiles_[i], x, y))
            return (int)i;
    return -1;
}

// Build and draw every tile except the program's, which renderPlayer draws from
// texture_ — so this is only ever the comparison images. The program's cell is
// whichever row its clip sits on (layoutProgramTile_), not necessarily the first:
// the top row's clip may not reach this frame.
//
// Each slot owns its texture and remembers which frame is in it, so a settled
// layout re-uploads nothing: the up-to-date check is the same one the program
// makes, minus the dissolve keys a tile never has.
//
// The OCIO *config* is not touched here. renderPlayer resolves it from the clip
// under the playhead (per-source config rules, plus its context variables), and a
// tile switching it would reload the config on every frame. Tiles therefore render
// through the program clip's config, and only the input colour space varies per
// tile — which is what OcioGpu's version-keyed program cache is for, so N tiles
// cost N binds rather than N shader builds.
void App::renderLayoutTiles(bool invalidate) {
    // The two are filled together in renderPlayer, so they always agree; the check
    // is here because everything below indexes one by the other.
    if (layoutTiles_.size() < 2 || layoutTiles_.size() != layoutClipIds_.size())
        return;
    // Slots are indexed by tile, so the program's slot simply goes unused rather
    // than shifting every slot after it when the program changes rows.
    if (layoutSlots_.size() < layoutTiles_.size())
        layoutSlots_.resize(layoutTiles_.size());

    const bool ocioActive = ocio_.isReady() && ocio_.isEnabled();
    const int gradeTech = (techMode_ == TechMode::Luminance) ? 0 : (int)techMode_;
    const bool wantGrade = grade_.active() || gradeTech != 0;
    if (wantGrade)
        ensureGradeGpu();

    for (size_t i = 0; i < layoutTiles_.size(); ++i) {
        if ((int)i == layoutProgramTile_)
            continue; // renderPlayer draws that one, from the program texture
        const SDL_FRect& cell = layoutTiles_[i];
        LayoutSlot& slot = layoutSlots_[i];
        const Clip* clip = timeline_.findClipById(layoutClipIds_[i]);

        // Cell backing, so a tile that has nothing to show reads as an empty slot
        // rather than as a hole in the stage.
        SDL_SetRenderDrawColor(renderer_, 0, 0, 0, 255);
        jplay::fillRect(renderer_, &cell);

        auto media = clip && !clip->mediaId.empty() ? timeline_.findMediaById(clip->mediaId) : nullptr;
        const bool missing = !clip || !media || media->openFailed();
        if (!missing) {
            const CacheKey key{ clip->mediaId,
                                clip->sourceOffset + (timeline_.playhead - clip->timelineStart) };
            if (FramePtr f = cache_->get(key)) {
                const std::string cs = ocioActive ? mediaColorSpace(*media) : std::string();
                slot.pa = f->pixelAspect > 0.0f ? f->pixelAspect : 1.0f;
                if (!slot.tex || slot.w != f->width || slot.h != f->height) {
                    if (slot.tex)
                        SDL_DestroyTexture(slot.tex);
                    // Display-referred 8-bit, tagged sRGB. A tile always holds the
                    // finished display rendering (unlike the program's float texture,
                    // which the HDR pass transforms at draw time), so saying so is
                    // what lets SDL decode it correctly when the main swapchain is
                    // scRGB instead of sRGB.
                    SDL_PropertiesID tp = SDL_CreateProperties();
                    SDL_SetNumberProperty(tp, SDL_PROP_TEXTURE_CREATE_FORMAT_NUMBER,
                                          SDL_PIXELFORMAT_RGBA32);
                    SDL_SetNumberProperty(tp, SDL_PROP_TEXTURE_CREATE_ACCESS_NUMBER,
                                          SDL_TEXTUREACCESS_STREAMING);
                    SDL_SetNumberProperty(tp, SDL_PROP_TEXTURE_CREATE_WIDTH_NUMBER, f->width);
                    SDL_SetNumberProperty(tp, SDL_PROP_TEXTURE_CREATE_HEIGHT_NUMBER, f->height);
                    SDL_SetNumberProperty(tp, SDL_PROP_TEXTURE_CREATE_COLORSPACE_NUMBER,
                                          SDL_COLORSPACE_SRGB);
                    slot.tex = SDL_CreateTextureWithProperties(renderer_, tp);
                    SDL_DestroyProperties(tp);
                    SDL_SetTextureScaleMode(slot.tex, SDL_SCALEMODE_LINEAR);
                    slot.w = f->width;
                    slot.h = f->height;
                    slot.has = false;
                }
                const bool upToDate = slot.has && !invalidate && slot.key == key &&
                                      slot.clipId == clip->id && slot.cs == cs;
                if (slot.tex && !upToDate) {
                    const void* px = nullptr;
                    OcioGpu::InputFormat ifmt = OcioGpu::InputFormat::Rgba8;
                    frameInput(*f, px, ifmt);

                    bool didGpu = false;
                    if (ocioActive && ocioGpu_.isReady() && px) {
                        OcioManager::Transform t = ocio_.transformFor(cs);
                        ocioGpu_.setProcessors(t.toWorking, t.toDisplay, t.version);
                        SDL_PropertiesID tp = SDL_GetTextureProperties(slot.tex);
                        Sint64 texId = SDL_GetNumberProperty(
                            tp, SDL_PROP_TEXTURE_OPENGL_TEXTURE_NUMBER, 0);
                        Sint64 texTarget = SDL_GetNumberProperty(
                            tp, SDL_PROP_TEXTURE_OPENGL_TEXTURE_TARGET_NUMBER, 0);
                        if (texId != 0 && texTarget != 0)
                            didGpu = ocioGpu_.render(px, ifmt, f->width, f->height,
                                                     (unsigned)texId, (unsigned)texTarget,
                                                     grade_.gain);
                    }
                    bool gainHandledUpstream = didGpu;
                    if (!didGpu) {
                        if (ocioActive) {
                            // No GL renderer (the HDR/gpu backend) or the GPU pass
                            // refused: the CPU transform, as the program does.
                            ocioPixels_.resize((size_t)f->width * f->height * 4);
                            ocio_.cpuTransformFor(cs, grade_.gain).apply(*f, ocioPixels_.data());
                            gainHandledUpstream = true;
                            SDL_UpdateTexture(slot.tex, nullptr, ocioPixels_.data(),
                                              f->width * 4);
                        } else {
                            SDL_UpdateTexture(slot.tex, nullptr, f->rgba.data(), f->width * 4);
                        }
                    }
                    // Grade + tech-check post-pass, in place on the display texture
                    // — the same pass the program takes, so the tiles are being
                    // compared under one look.
                    if (gradeGpu_.isReady() && wantGrade) {
                        SDL_PropertiesID gtp = SDL_GetTextureProperties(slot.tex);
                        Sint64 gId = SDL_GetNumberProperty(
                            gtp, SDL_PROP_TEXTURE_OPENGL_TEXTURE_NUMBER, 0);
                        Sint64 gTarget = SDL_GetNumberProperty(
                            gtp, SDL_PROP_TEXTURE_OPENGL_TEXTURE_TARGET_NUMBER, 0);
                        if (gId != 0 && gTarget != 0) {
                            grade::State gs = grade_;
                            if (gainHandledUpstream)
                                gs.gain = 0.0f;
                            gradeGpu_.apply((unsigned)gId, (unsigned)gTarget,
                                            f->width, f->height, gs, gradeTech);
                        }
                    }
                    slot.key = key;
                    slot.clipId = clip->id;
                    slot.cs = cs;
                    slot.has = true;
                }
            }
            // No frame yet: the slot keeps the last one it rendered, so a tile holds
            // rather than flashing black while its decode lands. Only while it is
            // still the same clip — see the draw below.
        }

        // The image, clipped to its cell — a zoomed tile must not spill into its
        // neighbours.
        //
        // Drawn only while the slot's content belongs to the clip this cell now
        // stands for. Slots are indexed by tile, so a playhead move onto a different
        // set of tracks can hand slot i a new clip; holding the previous image then
        // would show one source under another's label. The cell goes black for the
        // frame or two until its own decode lands instead.
        if (slot.has && slot.tex && slot.w > 0 && slot.h > 0 && clip && slot.clipId == clip->id) {
            const SDL_Rect clipRect = { (int)cell.x, (int)cell.y, (int)cell.w, (int)cell.h };
            SDL_SetRenderClipRect(renderer_, &clipRect);
            SDL_FRect dst = fitImageIn(cell, (float)slot.w * slot.pa, (float)slot.h);
            SDL_RenderTexture(renderer_, slot.tex, nullptr, &dst);
            SDL_SetRenderClipRect(renderer_, nullptr);
        }

        drawLayoutTileChrome(cell, clip, (int)i);
    }
}

// A tile's label bar and border. Shared by the program tile (drawn from
// renderPlayer, after its own image) and the comparison tiles above, so every tile
// gets the same furniture.
//
// The label names the track, since on this stage a tile *is* a track — that is how
// the user chose what to compare, and the track name is the one they can edit. The
// source stem follows it, because two versions of a shot differ by file rather
// than by shot name.
//
// A slot whose clip does not reach this frame is still named, after the clip that
// owns the row: the tiling is fixed for as long as the stage is up, so an empty
// slot is "this version has no frame here", not an anonymous gap.
void App::drawLayoutTileChrome(const SDL_FRect& cell, const Clip* c, int track) {
    if (!c)
        c = layoutRowClip(track);
    const float lineH = textFont_.lineHeight();
    const float barH = lineH + 2.0f * dpiScale;
    if (c && cell.h >= lineH * 3.0f) {
        std::string label = trackLabel(track);
        if (auto media = timeline_.findMediaById(c->mediaId)) {
            std::string stem = hashSeqStem(fs::path(media->path()).stem().string(),
                                           media->type() == ClipType::ImageSequence);
            if (!stem.empty())
                label += "  " + stem;
        }
        std::string fitted = fitText(label, cell.w - 6.0f * dpiScale);
        if (!fitted.empty()) {
            SDL_FRect bar = { cell.x, cell.y + cell.h - barH, cell.w, barH };
            SDL_SetRenderDrawColor(renderer_, 0, 0, 0, 150);
            jplay::fillRect(renderer_, &bar);
            drawText(cell.x + 3.0f * dpiScale, bar.y + (barH - lineH) * 0.5f,
                     kTileLabel, fitted);
        }
    }
    SDL_SetRenderDrawColor(renderer_, kTileBorder.r, kTileBorder.g, kTileBorder.b,
                           kTileBorder.a);
    jplay::drawRect(renderer_, &cell);
}

// ------------------------------------------------------------- Stack stage
// The same comparison sequence, one image at a time: the top-most clip, which is
// what the Frame stage already draws — so the stage carries no render path of its
// own and the program image, its annotations, the letterbox, the output devices
// and the review window all follow the top of the stack for free.
//
// Cycling rotates the clips' track numbers rather than picking an index to show,
// so "on top" means the same thing here as everywhere else in the app: the
// compact timeline's rows reorder with it, and every consumer of the program is
// right without being told about the stage. The rotation is confined to the
// scratch sequence, which goes away with the stage, so there is nothing to undo.

// The stack from the top down, one entry per video row. Earliest clip wins a row
// the user has dropped a second one onto — layoutRowClip's rule, for every row at
// once, so the order is the rotation order rather than what reaches the playhead.
void App::stackRowClips(std::vector<const Clip*>& out) const {
    int rows = 0;
    forEachViewClip([&](const Clip& c) {
        if (!c.audio && c.track + 1 > rows)
            rows = c.track + 1;
    });
    out.assign((size_t)rows, nullptr);
    forEachViewClip([&](const Clip& c) {
        if (c.audio || c.track < 0 || c.track >= rows)
            return;
        const Clip*& slot = out[(size_t)c.track];
        if (!slot || c.timelineStart < slot->timelineStart)
            slot = &c;
    });
}

// Up/Down on the stage. dir +1 brings the clip below the program up to it, -1 the
// one above — the rotation wraps, so a held key spins through the stack.
void App::cycleStack(int dir) {
    if (!stackView() || dir == 0)
        return;
    Sequence* seq = nullptr;
    for (Sequence& q : timeline_.sequences)
        if (q.id == scratchSeqId_) {
            seq = &q;
            break;
        }
    if (!seq)
        return;
    int rows = 0;
    for (const Clip& c : seq->clips)
        if (!c.audio)
            rows = std::max(rows, c.track + 1);
    if (rows < 2) {
        setStatusWarn("NOTHING TO CYCLE: THE STACK HOLDS ONE CLIP", 2000);
        return;
    }
    // Row d becomes the top, so every row moves down by d: with d = dir the clip
    // one below the program lands on it. Rows are dense (one clip each, numbered
    // from 0 by openLayoutView and by a drop onto the stage), so the modulo walks
    // the stack rather than skipping a gap.
    for (Clip& c : seq->clips)
        if (!c.audio)
            c.track = ((c.track - dir) % rows + rows) % rows;

    stackOverlayUntil_ = SDL_GetTicks() + kStackOverlayMs;
}

// The overlay a cycle arms: the program's basename in white, centred, with the two
// under it in the rotation below it and fainter, so a spin reads as a list moving
// past rather than as a name replacing a name. Fades out at the end of its second
// instead of cutting.
void App::renderStackOverlay() {
    if (!stackView() || launcherVisible())
        return;
    const Uint64 now = SDL_GetTicks();
    if (now >= stackOverlayUntil_)
        return;
    std::vector<const Clip*> rows;
    stackRowClips(rows);
    if (rows.size() < 2)
        return;

    // Whole-overlay fade over the last stretch of its life.
    const Uint64 left = stackOverlayUntil_ - now;
    const float fade = left >= kStackFadeMs ? 1.0f : (float)left / (float)kStackFadeMs;

    // The names, top of the stack first. A row whose clip has no media left is
    // still a step in the rotation, so it keeps its line rather than pulling the
    // one under it up into a position it does not hold.
    std::string names[kStackOverlayLines];
    int n = 0;
    for (size_t i = 0; i < rows.size() && n < kStackOverlayLines; ++i, ++n) {
        const Clip* clip = rows[i];
        auto media = clip && !clip->mediaId.empty() ? timeline_.findMediaById(clip->mediaId) : nullptr;
        if (!media) {
            names[n] = "(missing)";
            continue;
        }
        fs::path mp = fs::u8path(media->path());
        names[n] = hashSeqStem(mp.stem().u8string(), media->type() == ClipType::ImageSequence) +
                   mp.extension().u8string();
    }

    // The program's line is the one being read, so it is the large one; the two
    // under it are smaller as well as fainter.
    const float topScale  = 1.6f;
    const float restScale = 1.15f;
    const float lineH     = headerFont_.lineHeight();
    const float gap       = 6.0f * dpiScale;
    float blockH = 0.0f;
    for (int i = 0; i < n; ++i)
        blockH += lineH * (i == 0 ? topScale : restScale) + (i ? gap : 0.0f);

    // Centred in the player area, and clipped to it: a long name must not run out
    // over the panels beside the image.
    const SDL_Rect cr = { (int)playerRect_.x, (int)playerRect_.y,
                          (int)playerRect_.w, (int)playerRect_.h };
    SDL_SetRenderClipRect(renderer_, &cr);
    float y = playerRect_.y + (playerRect_.h - blockH) * 0.5f;
    for (int i = 0; i < n; ++i) {
        const float scale = i == 0 ? topScale : restScale;
        const float tw = headerFont_.measure(renderer_, names[i].c_str()) * scale;
        const float th = lineH * scale;
        const Uint8 a = (Uint8)std::lround(kStackLineAlpha[i] * fade);
        // A plate behind each line, so the names stay readable over a bright frame.
        SDL_FRect plate = { playerRect_.x + (playerRect_.w - tw) * 0.5f - 8.0f * dpiScale,
                            y, tw + 16.0f * dpiScale, th };
        SDL_SetRenderDrawColor(renderer_, 0, 0, 0, (Uint8)std::lround(140 * fade * a / 255.0f));
        jplay::fillRect(renderer_, &plate);
        headerFont_.draw(renderer_, playerRect_.x + (playerRect_.w - tw) * 0.5f, y,
                         SDL_Color{ 255, 255, 255, a }, names[i].c_str(), scale);
        y += th + gap;
    }
    SDL_SetRenderClipRect(renderer_, nullptr);
}

// Timeline-clip translation unit: the clip right-click metadata picker menu
// (buildMetaPickers / openClipPickerMenu / rebuildClipPickerMenu) plus the
// view-agnostic picker cascade core it shares with the Clip Source panel
// (pickerDisplayIndex / buildPickerColumns / startPickerNavigate /
// startPickerCommit — see App_ClipSource.cpp for the other consumer), and
// media replacement, plus the filtered-clip lookup used by
// the scoped view. Split out of App.cpp; all are App members. Shared helpers come
// from AppInternal.h.

#include "App.h"
#include "AppInternal.h"
#include "ImageSeq.h"
#include "Layout.h"
#include "PythonBridge.h"
#include "RevealFile.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <string>
#include <utility>
#include <vector>

using namespace jplay;

// ---- Clip Range band on the clip right-click popup. Base (1x) sizes; every one
// is multiplied by dpiScale where it is used, since the band draws text and the
// font is loaded at 10 * dpiScale.
namespace {
constexpr float kRangeBandH  = 78.0f;  // total band height reserved (matches the rows below)
constexpr float kRangePad    = 4.0f;
constexpr float kRangeFieldW = 46.0f;  // in / out entry boxes
constexpr float kRangeBtnPad = 7.0f;   // text inset inside a range button
constexpr float kRangeGap    = 4.0f;   // between the two buttons

constexpr SDL_Color kRangeTitle  { 160, 165, 175, 255 }; // "Clip Range" caption
constexpr SDL_Color kRangeDim    { 130, 134, 142, 255 }; // source first/last numbers
constexpr SDL_Color kRangeTrack  {  26,  27,  31, 255 }; // ruler: the full source
constexpr SDL_Color kRangeCut    {  70, 110, 180, 255 }; // ruler: the clip's cut range
constexpr SDL_Color kRangeBorder {  90,  92, 100, 255 }; // matches the popup outline
constexpr SDL_Color kRangeBtn    {  48,  49,  55, 255 };
constexpr SDL_Color kRangeText   { 225, 228, 235, 255 };
} // namespace

// Bucket the view's video clips by row, each row sorted by timelineStart. Hidden
// and disabled clips stay in — the lookups filter them, and includeHidden wants
// them. Audio never composites, so it is left out entirely.
void App::buildClipIndex() {
    for (auto& row : clipIndex_)
        row.clear(); // keep the rows' capacity across frames
    forEachViewClip([&](const Clip& c) {
        if (c.audio || c.track < 0)
            return;
        if ((size_t)c.track >= clipIndex_.size())
            clipIndex_.resize((size_t)c.track + 1);
        clipIndex_[(size_t)c.track].push_back(&c);
    });
    for (auto& row : clipIndex_)
        std::sort(row.begin(), row.end(), [](const Clip* a, const Clip* b) {
            return a->timelineStart < b->timelineStart;
        });
    clipIndexValid_ = true;
}

// Lowest-numbered row holding a live clip at `frame`, above `belowTrack`. Within
// a row clips never overlap, so the only candidate is the last one starting at or
// before the frame; a disabled clip there does not hide the rows under it, so the
// walk simply continues down (which is what lets a fade find its second layer).
const Clip* App::indexedClipAt(int64_t frame, bool includeHidden, int belowTrack) const {
    for (int t = belowTrack + 1; t < (int)clipIndex_.size(); ++t) {
        const auto& row = clipIndex_[(size_t)t];
        auto it = std::upper_bound(row.begin(), row.end(), frame,
                                   [](int64_t f, const Clip* c) {
                                       return f < c->timelineStart;
                                   });
        if (it == row.begin())
            continue;
        const Clip* c = *(it - 1);
        if (frame >= c->end())
            continue; // the gap after that clip
        if (timeline_.clipDisabled(*c) && !includeHidden)
            continue;
        return c;
    }
    return nullptr;
}

const Clip* App::getTopMostClipAtFrame(int64_t frame, bool includeHidden) const {
    if (clipIndexValid_)
        return indexedClipAt(frame, includeHidden, -1);
    const Clip* best = nullptr;
    forEachViewClip([&](const Clip& c) {
        if (c.audio || (timeline_.clipDisabled(c) && !includeHidden))
            return;
        if (frame >= c.timelineStart && frame < c.end())
            if (!best || c.track < best->track)
                best = &c;
    });
    return best;
}

const Clip* App::getClipBelow(int64_t frame, int aboveTrack) const {
    if (clipIndexValid_)
        return indexedClipAt(frame, false, aboveTrack);
    const Clip* best = nullptr;
    forEachViewClip([&](const Clip& c) {
        if (c.audio || timeline_.clipDisabled(c) || c.track <= aboveTrack)
            return;
        if (frame >= c.timelineStart && frame < c.end())
            if (!best || c.track < best->track)
                best = &c;
    });
    return best;
}

App::ProgramSource App::programSourceAt(int64_t frame) const {
    ProgramSource ps;
    const Clip* top = getTopMostClipAtFrame(frame);
    if (!top)
        return ps;
    ps.a = top;
    ps.aSrc = top->sourceOffset + (frame - top->timelineStart);

    // ---- a dissolve on one of top's cuts -------------------------------------
    // submitCacheRequests calls this once per prefetched frame, so a project with
    // no dissolves must not pay for the sequence lookup below. This scan is over
    // sequences, not clips.
    bool anyTransitions = false;
    for (const auto& s : timeline_.sequences)
        if (!s.transitions.empty()) { anyTransitions = true; break; }
    // A transition joins two clips of one sequence, and for every frame of its
    // span the topmost clip is one of them — the outgoing clip before the cut, the
    // incoming one at/after it. So the search is confined to top's own sequence,
    // and a span reaching under a higher track's clip correctly finds nothing.
    const Sequence* seq = anyTransitions ? timeline_.sequenceOfClip(top->id) : nullptr;
    if (seq) {
        for (const auto& t : seq->transitions) {
            Timeline::TransitionSpan sp;
            if (!timeline_.resolveTransition(*seq, t, sp))
                continue;
            if (frame < sp.start || frame >= sp.end)
                continue;
            if (sp.a != top && sp.b != top)
                continue;
            const Clip* other = (top == sp.a) ? sp.b : sp.a;
            if (timeline_.clipDisabled(*other))
                break; // a disabled side dissolves to nothing; show the live clip alone
            // Both sides sample at their natural rate; `other` is simply outside its
            // own trimmed range here, which is what its handle is for.
            const int64_t oSrc = other->sourceOffset + (frame - other->timelineStart);
            if (oSrc < 0 || oSrc >= timeline_.clipSourceFrames(*other))
                break; // handle no longer reaches (a trim outran pruneTransitions)
            // Ramp over the whole span, offset by one frame so the first frame of
            // the span is not pure a and the last not pure b.
            const float tt = (float)(frame - sp.start + 1) / (float)(t.duration() + 1);
            ps.b = other;
            ps.bSrc = oSrc;
            ps.mix = (top == sp.a) ? tt : 1.0f - tt; // weight of ps.b
            return ps; // a dissolve owns the frame; a fade on the same cut defers
        }
    }

    // ---- top's own fade ramps ------------------------------------------------
    // A dissolve and a fade-out both live in a clip's tail and are each capped at
    // half the clip, so they can overlap. The dissolve above already returned if it
    // covered this frame, which settles it: the cut wins, the fade defers.
    const float opacity = Timeline::clipOpacity(*top, frame);
    if (opacity >= 1.0f)
        return ps;
    // Fade over the next visible clip below, or toward black when the track stack
    // has nothing under this frame. Fading against black costs no second decode.
    if (const Clip* below = getClipBelow(frame, top->track)) {
        const int64_t bSrc = below->sourceOffset + (frame - below->timelineStart);
        if (bSrc >= 0 && bSrc < timeline_.clipSourceFrames(*below)) {
            ps.b = below;
            ps.bSrc = bSrc;
            ps.mix = 1.0f - opacity; // top dims, the clip beneath comes up
            return ps;
        }
    }
    ps.fade = opacity;
    return ps;
}

// Load the naming config's [picker:*] set (list_pickers) into metaPickers_, in
// display order. Config-driven and served by Python, which is not ready until
// after init() — so this runs lazily from openClipPickerMenu and builds once,
// the first time the clip right-click menu is opened. Only the key, label and the
// multi-select flag are kept; the clip menu fetches each picker's options on
// demand (describe_pickers).
void App::buildMetaPickers() {
    if (metaPickersBuilt_)
        return;
    std::vector<PickerDef> defs;
    if (!jplayListPickers(defs))
        return; // Python not ready yet; retried on the next open

    for (const auto& d : defs)
        metaPickers_.push_back(MetaPicker{ d.key, d.label, d.multiCommit });
    metaPickersBuilt_ = true;
}

// "Unpack Clip > <picker>": for every option of picker `key` other than the one
// the clip already matches, add a new clip that mirrors the source clip's span
// (same start / duration / source offset) but with its media resolved to that
// option. Each new clip is stacked on the first track below the source whose
// frame span is free, growing the track count as needed. Undoable as one step.
void App::unpackClip(int clipId, const std::string& key) {
    Clip* src = timeline_.findClipById(clipId);
    if (!src || src->mediaId.empty())
        return;
    auto srcMedia = timeline_.findMediaById(src->mediaId);
    if (!srcMedia)
        return;
    const std::string srcPath = srcMedia->resolvedPath();

    // The picker's option list and which one the source clip currently matches.
    std::vector<PickerState> states;
    if (!jplayDescribePickers(srcPath, states))
        return;
    const PickerState* st = nullptr;
    for (const auto& s : states)
        if (s.key == key) { st = &s; break; }
    if (!st || st->options.size() <= 1) {
        // The menu lists every configured picker without querying, so a row landing
        // here is expected: say so rather than appearing to do nothing.
        setStatus("NOTHING TO UNPACK", 4000);
        return;
    }

    Sequence* seq = timeline_.sequenceOfClipMut(clipId);
    if (!seq)
        return;
    const int seqId = seq->id;

    // Every unpacked clip mirrors the source clip's span, so they all occupy the
    // same [start, end) and must each land on a distinct track below the source.
    const int64_t start   = src->timelineStart;
    const int64_t dur     = src->duration;
    const int64_t srcOff  = src->sourceOffset;
    const int srcTrack    = src->track;

    // A track is free for this span if no video clip in the sequence overlaps it there.
    auto trackFree = [&](int track) {
        for (const Clip& c : seq->clips)
            if (!c.audio && c.track == track && start < c.end() && c.timelineStart < start + dur)
                return false;
        return true;
    };

    std::vector<Clip> created;
    for (int i = 0; i < (int)st->options.size(); ++i) {
        if (i == st->currentIndex)
            continue; // leave the source clip's own element as-is
        const std::string& value = st->options[i].value;

        std::string newPath;
        if (!jplayResolvePath(srcPath, key, value, newPath) || newPath == srcPath)
            continue; // no media for this element (or it maps back to the source)

        // Reuse an existing pool entry, else open and metadata-tag new media.
        ClipType type = ImageSeq::isSequencePath(newPath) ? ClipType::ImageSequence : ClipType::Video;
        auto media = timeline_.findMediaByPath(type, newPath);
        if (!media) {
            media = std::make_shared<Media>(type, newPath);
            std::string err;
            if (!media->ensureOpen(err)) {
                setStatus("FAILED TO OPEN " + fileLabel(newPath) + ": " + err, 5000);
                continue;
            }
            media->refreshMetadata();
            std::map<std::string, std::string> vals;
            jplayGetPathValues(media->resolvedPath(), vals);
            for (auto& kv : vals)
                media->setMetaValue(kv.first, std::move(kv.second));
            timeline_.media[media->id()] = media;
        }

        // First free track below the source (already-created clips share this
        // span, so trackFree() naturally stacks each new one on its own row).
        int track = srcTrack + 1;
        while (!trackFree(track))
            ++track;

        int64_t avail = std::max<int64_t>(media->info().frameCount - srcOff, 1);
        Clip c;
        c.id            = nextClipId_++;
        c.mediaId       = media->id();
        c.track         = track;
        c.timelineStart = start;
        c.duration      = std::min<int64_t>(dur, avail); // clamp if the media is shorter
        c.sourceOffset  = srcOff;
        c.shotId        = -1;
        seq->clips.push_back(c);
        created.push_back(c);
    }

    if (created.empty()) {
        setStatus("NO MEDIA TO UNPACK", 4000);
        return;
    }

    ensureTrailingEmptyTrack();
    timeline_.repackSequences();

    std::vector<int> ids;
    for (const Clip& c : created)
        ids.push_back(c.id);
    undoStack_.push({
        "UNPACK CLIP",
        [this, ids, seqId] {
            if (Sequence* s = timeline_.findSequenceById(seqId))
                s->clips.erase(std::remove_if(s->clips.begin(), s->clips.end(),
                    [&](const Clip& c) {
                        return std::find(ids.begin(), ids.end(), c.id) != ids.end();
                    }), s->clips.end());
            ensureTrailingEmptyTrack();
            timeline_.repackSequences();
        },
        [this, created, seqId] {
            if (Sequence* s = timeline_.findSequenceById(seqId))
                for (const Clip& c : created)
                    s->clips.push_back(c);
            ensureTrailingEmptyTrack();
            timeline_.repackSequences();
        },
    });

    setStatus("UNPACKED " + std::to_string((int)created.size()) + " CLIP(S)");
}

// Append an "Unpack Clip" row holding one child per configured picker, in
// metaPickers_ display order. Deliberately does not describe_pickers to filter the
// list down to the pickers that have something to unpack: that call can reach a
// site database, and opening a menu must not pay for it — a plain right-click on a
// clip is a common gesture, not a deliberate picker query. Whether a picker offers
// more than one option is decided when a child is clicked (see unpackClip).
// Used by the clip right-click menu.
void App::appendUnpackItems(const Clip& clip, std::vector<ContextMenu::Item>& items) {
    if (!timeline_.findMediaById(clip.mediaId))
        return;
    buildMetaPickers(); // load the config picker list on first use (needs Python)
    if (metaPickers_.empty())
        return;
    ContextMenu::Item parent;
    parent.label = "Unpack Clip";
    for (const auto& p : metaPickers_) {
        ContextMenu::Item it;
        it.label = p.label;
        const int clipId = clip.id;
        const std::string key = p.key;
        it.action = [this, clipId, key] { unpackClip(clipId, key); };
        parent.children.push_back(std::move(it));
    }
    items.push_back(std::move(parent));
}

// Append "Find and Attach Audio" (Current / All Clips) when `clip` is an image
// sequence with no audio already sitting under it. Offered regardless of the
// auto-pair-on-add setting: that preference only governs what happens when media
// is added, this row pulls the paired audio in on demand via query_audio.
// Used by the clip right-click menu.
void App::appendAttachAudioItems(const Clip& clip, std::vector<ContextMenu::Item>& items) {
    auto m = timeline_.findMediaById(clip.mediaId);
    if (!m || m->type() != ClipType::ImageSequence)
        return;
    const int owner = timeline_.seqIndexOfClip(clip.id);
    if (owner >= 0)
        for (const auto& c : timeline_.sequences[owner].clips)
            if (c.audio && clip.timelineStart < c.end() && clip.end() > c.timelineStart)
                return;

    ContextMenu::Item it;
    it.label = "Find and Attach Audio";
    const int clipId = clip.id;
    ContextMenu::Item cur;
    cur.label = "Current";
    cur.action = [this, clipId] { findAndAttachAudio(clipId); };
    ContextMenu::Item all;
    all.label = "All Clips";
    all.action = [this] { findAndAttachAudioAll(); };
    it.children.push_back(std::move(cur));
    it.children.push_back(std::move(all));
    items.push_back(std::move(it));
}

// Plain right-click on a timeline clip: the clip-actions popup. Copy/paste act on
// the whole selection exactly like Ctrl+C / Ctrl+V (paste anchors at the
// playhead).
// Link/Unlink also act on the selection: shift-click a video clip and the audio
// to pair them, or right-click either half of an existing pair to break it.
void App::openClipContextMenu(const Clip& clip, float mx, float my, bool growDown) {
    std::vector<ContextMenu::Item> items;
    auto add = [&](const char* label, ContextMenu::Action action) {
        ContextMenu::Item it;
        it.label = label;
        it.action = std::move(action);
        items.push_back(std::move(it));
    };
    // A rule between groups, collapsed when the group before or after it is empty
    // (most rows here are conditional).
    auto sep = [&] {
        if (!items.empty() && !items.back().separator) {
            ContextMenu::Item it;
            it.separator = true;
            items.push_back(std::move(it));
        }
    };
    add("Copy Clip", [this] { copySelectedClips(); });
    add("Paste Clip", [this] { pasteClips(); });
    sep();
    add("Copy Source Path", [this] {
        std::vector<std::string> paths; // one line per distinct source, selection order
        for (int id : selectedClipIds_) {
            const Clip* c = timeline_.findClipById(id);
            if (!c || c->mediaId.empty())
                continue;
            auto m = timeline_.findMediaById(c->mediaId);
            if (!m || std::find(paths.begin(), paths.end(), m->path()) != paths.end())
                continue;
            paths.push_back(m->path());
        }
        std::string text;
        for (const std::string& p : paths) {
            if (!text.empty())
                text += '\n';
            text += p;
        }
        if (text.empty()) {
            setStatus("NO SOURCE PATH", 3000);
            return;
        }
        SDL_SetClipboardText(text.c_str());
        setStatus("PATH COPIED", 2000);
    });
    // The clicked clip alone, not the selection: one reveal can only open one
    // folder. A sequence's path is a frame pattern rather than a real file, so
    // revealInFileManager falls back to opening its directory.
    if (auto m = timeline_.findMediaById(clip.mediaId))
        add("Show in File Browser", [this, path = m->path()] {
            if (!revealInFileManager(path))
                setStatus("COULD NOT OPEN FILE BROWSER", 3000);
        });
    sep();
    if (!clip.audio) {
        // Fades act on the whole selection (they are per-clip state, so there is no
        // one cut to tie them to) and composite over the track below, or black.
        // An edge that already has a ramp offers no row: resizing it is the corner
        // dot's job, and the row would only ever reset it to the default length.
        if (clip.fadeInFrames <= 0)
            add("Fade In", [this] { addFadeToSelection(0); });
        if (clip.fadeOutFrames <= 0)
            add("Fade Out", [this] { addFadeToSelection(1); });
        if (clip.fadeInFrames > 0 || clip.fadeOutFrames > 0)
            add("Remove Fades", [this] {
                ContentSnapshot before = captureContent();
                for (int id : selectedClipIds_)
                    if (Clip* c = clipById(id)) { c->fadeInFrames = 0; c->fadeOutFrames = 0; }
                pushContentUndo("REMOVE FADES", before);
            });
    }
    // Audio/video link. Both rows can apply at once: a selection holding a linked
    // pair plus a second audio clip can drop the link it has and make the new one.
    sep();
    if (selectionHasLink())
        add("Unlink Audio", [this] { unlinkSelectedClips(); });
    if (selectionCanLink())
        add("Link Audio", [this] { linkSelectedClips(); });
    sep();
    appendUnpackItems(clip, items);
    appendAttachAudioItems(clip, items);
    if (!items.empty() && items.back().separator)
        items.pop_back();

    // Clip Range band above the rows, for the clicked clip alone (the rows act on
    // the whole selection, but one ruler can only describe one clip).
    ContextMenu::Header hdr;
    clipRangeClipId_ = timeline_.findMediaById(clip.mediaId) ? clip.id : -1;
    if (clipRangeClipId_ >= 0) {
        hdr.h = kRangeBandH * dpiScale;
        hdr.minW = clipRangeBandW(timeline_.findShotById(clip.shotId) != nullptr);
        hdr.render = [this](SDL_Renderer* r, const SDL_FRect& box) { renderClipRangeHeader(r, box); };
        hdr.handle = [this](const SDL_Event& e, const SDL_FRect& box) {
            return clipRangeHandleEvent(e, box);
        };
    }

    // ContextMenu anchors its bottom edge at the given y and grows upward, which
    // is what a timeline clip wants: the rows stay clear of the tracks below.
    // Over the player frame there is nothing below to keep clear and the click is
    // usually high up, so the caller asks for the menu to drop from the cursor.
    clipToolboxMenu_.open(mx, my, winW_, winH_, std::move(items), std::move(hdr), growDown);
}

// ------------------------------------------------------------- Clip Range band

// Source frame numbering for the band: `frames` is what the media can supply and
// `base` the real number of source frame 0 (1001 for shot.1001.exr; 0 for video,
// which has no numbering). Mirrors the drag-trim readout in renderTimeline.
void App::clipRangeSource(const Clip& c, int64_t& frames, int64_t& base) const {
    frames = 1;
    base = 0;
    auto pm = timeline_.findMediaById(c.mediaId);
    if (!pm)
        return;
    frames = std::max<int64_t>(pm->info().frameCount, c.sourceOffset + c.duration);
    if (pm->isOpen()) {
        std::string err;
        if (auto src = pm->ensureOpen(err))
            base = src->firstFrameNumber();
    }
}

// Width a range button needs to just fit its label. Measured rather than fixed
// so the buttons hug their text at any DPI; the layout and the minimum band
// width both go through here, so a button can never be narrower than its label.
float App::clipRangeBtnW(const char* label) const {
    return textFont_.measure(renderer_, label) + 2.0f * kRangeBtnPad * dpiScale;
}

// Narrowest the popup may be for the band to hold its content: whichever of the
// button row and the two entry fields is wider, plus the side padding.
float App::clipRangeBandW(bool hasShot) const {
    const float s = dpiScale;
    float buttons = clipRangeBtnW("Source Range");
    if (hasShot)
        buttons += kRangeGap * s + clipRangeBtnW("Shot Range");
    const float fields = 2.0f * kRangeFieldW * s + kRangeGap * s;
    return std::max(buttons, fields) + 2.0f * kRangePad * s;
}

// Sub-rects of the band, derived from the box alone so the render and the event
// pass can never drift. The Shot Range button is only laid out when the clip
// belongs to a shot. Both buttons are label-width and sit left-aligned.
App::ClipRangeLayout App::clipRangeLayout(const SDL_FRect& box, bool hasShot) const {
    const float s = dpiScale;
    ClipRangeLayout L;
    SDL_FRect in = inset(box, kRangePad * s, kRangePad * s);
    L.title = cutTop(in, 12.0f * s);
    L.ruler = cutTop(in, 10.0f * s);
    L.ends  = cutTop(in, 11.0f * s);
    gapTop(in, 2.0f * s);
    SDL_FRect fields = cutTop(in, 16.0f * s);
    L.inField  = cutLeft(fields, kRangeFieldW * s);
    L.outField = cutRight(fields, kRangeFieldW * s);
    gapTop(in, 3.0f * s);
    SDL_FRect buttons = cutTop(in, 16.0f * s);
    L.srcBtn = cutLeft(buttons, clipRangeBtnW("Source Range"));
    if (hasShot) {
        gapLeft(buttons, kRangeGap * s);
        L.shotBtn = cutLeft(buttons, clipRangeBtnW("Shot Range"));
    }
    return L;
}

void App::renderClipRangeHeader(SDL_Renderer* r, const SDL_FRect& box) {
    const Clip* c = timeline_.findClipById(clipRangeClipId_);
    if (!c)
        return;
    const Shot* shot = timeline_.findShotById(c->shotId);
    const ClipRangeLayout L = clipRangeLayout(box, shot != nullptr);

    int64_t frames = 1, base = 0;
    clipRangeSource(*c, frames, base);

    auto text = [&](const SDL_FRect& slot, SDL_Color col, const std::string& s, bool rightAlign) {
        const float tw = textFont_.measure(r, s.c_str());
        const float x = rightAlign ? slot.x + slot.w - tw : slot.x;
        textFont_.draw(r, x, slot.y + (slot.h - textFont_.lineHeight()) * 0.5f, col, s.c_str());
    };
    auto fill = [&](const SDL_FRect& b, SDL_Color col) {
        SDL_SetRenderDrawColor(r, col.r, col.g, col.b, col.a);
        jplay::fillRect(r, &b);
    };

    text(L.title, kRangeTitle, "Clip Range", false);

    // Ruler: the whole track is the source, the filled span the clip's cut. A
    // clip using the entire source therefore fills it edge to edge.
    SDL_FRect track = inset(L.ruler, 0.0f, 2.0f * dpiScale);
    fill(track, kRangeTrack);
    const double span = (double)std::max<int64_t>(frames, 1);
    SDL_FRect cut = track;
    cut.x = track.x + (float)((double)c->sourceOffset / span * track.w);
    cut.w = std::max(1.0f, (float)((double)c->duration / span * track.w));
    if (cut.x + cut.w > track.x + track.w)
        cut.w = track.x + track.w - cut.x;
    fill(cut, kRangeCut);
    SDL_SetRenderDrawColor(r, kRangeBorder.r, kRangeBorder.g, kRangeBorder.b, kRangeBorder.a);
    jplay::drawRect(r, &track);

    // The source's own first / last frame numbers, under the ruler ends.
    text(L.ends, kRangeDim, std::to_string(base), false);
    text(L.ends, kRangeDim, std::to_string(base + frames - 1), true);

    // The editable in/out. While a field has focus it owns its own text; otherwise
    // it mirrors the clip, so an undo or a drag-trim shows up here immediately.
    if (!clipRangeIn_.focused())
        clipRangeIn_.setText(std::to_string(base + c->sourceOffset));
    if (!clipRangeOut_.focused())
        clipRangeOut_.setText(std::to_string(base + c->sourceOffset + c->duration - 1));
    clipRangeIn_.setRect(L.inField);
    clipRangeOut_.setRect(L.outField);
    clipRangeIn_.render(r, &textFont_);
    clipRangeOut_.render(r, &textFont_);

    drawButton(r, &textFont_, L.srcBtn, "Source Range", kRangeBtn, kRangeBorder, kRangeText);
    if (shot)
        drawButton(r, &textFont_, L.shotBtn, "Shot Range", kRangeBtn, kRangeBorder, kRangeText);
}

bool App::clipRangeHandleEvent(const SDL_Event& e, const SDL_FRect& box) {
    const Clip* c = timeline_.findClipById(clipRangeClipId_);
    if (!c)
        return false;
    const Shot* shot = timeline_.findShotById(c->shotId);
    const ClipRangeLayout L = clipRangeLayout(box, shot != nullptr);

    // An active field captures typing, Enter (apply) and Escape (drop).
    if (clipRangeEditing_) {
        if (e.type == SDL_EVENT_KEY_DOWN) {
            if (e.key.key == SDLK_RETURN || e.key.key == SDLK_KP_ENTER) { endClipRangeEdit(true); return true; }
            if (e.key.key == SDLK_ESCAPE) { endClipRangeEdit(false); return true; }
            clipRangeIn_.handleEvent(e);
            clipRangeOut_.handleEvent(e);
            return true;
        }
        if (e.type == SDL_EVENT_TEXT_INPUT) {
            clipRangeIn_.handleEvent(e);
            clipRangeOut_.handleEvent(e);
            return true;
        }
        // Motion / release: consumed only while a drag-select is actually live
        // (TextInput reports that), so hovering the rows below still highlights.
        if (e.type == SDL_EVENT_MOUSE_MOTION || e.type == SDL_EVENT_MOUSE_BUTTON_UP) {
            const bool a = clipRangeIn_.handleEvent(e);
            const bool b = clipRangeOut_.handleEvent(e);
            return a || b;
        }
    }

    if (e.type != SDL_EVENT_MOUSE_BUTTON_DOWN)
        return false;
    const float mx = e.button.x, my = e.button.y;
    if (!inRect(box, mx, my)) {
        endClipRangeEdit(true); // a click elsewhere applies, then falls through
        return false;
    }
    if (inRect(L.inField, mx, my) || inRect(L.outField, mx, my)) {
        const bool wantIn = inRect(L.inField, mx, my);
        clipRangeIn_.setFocus(wantIn);
        clipRangeOut_.setFocus(!wantIn);
        (wantIn ? clipRangeIn_ : clipRangeOut_).handleEvent(e); // place the caret
        if (!clipRangeEditing_) {
            clipRangeEditing_ = true;
            SDL_StartTextInput(window_);
        }
        return true;
    }
    endClipRangeEdit(false); // a click on a button drops a half-typed field
    if (inRect(L.srcBtn, mx, my)) {
        int64_t frames = 1, base = 0;
        clipRangeSource(*c, frames, base);
        applyClipRange(clipRangeClipId_, 0, frames - 1);
    } else if (shot && inRect(L.shotBtn, mx, my)) {
        applyClipRange(clipRangeClipId_, shot->cutIn, shot->cutOut - 1);
    }
    return true; // clicks anywhere in the band stay in the band
}

// Apply the focused field (or drop it) and put the band back in display mode.
void App::endClipRangeEdit(bool apply) {
    if (!clipRangeEditing_)
        return;
    const bool editingIn = clipRangeIn_.focused();
    const std::string txt = editingIn ? clipRangeIn_.text() : clipRangeOut_.text();
    clipRangeIn_.setFocus(false);
    clipRangeOut_.setFocus(false);
    clipRangeEditing_ = false;
    SDL_StopTextInput(window_);

    const Clip* c = apply ? timeline_.findClipById(clipRangeClipId_) : nullptr;
    if (!c)
        return;
    int64_t v = 0;
    try { v = std::stoll(txt); } catch (...) { return; }
    int64_t frames = 1, base = 0;
    clipRangeSource(*c, frames, base);
    const int64_t curIn  = c->sourceOffset;
    const int64_t curOut = c->sourceOffset + c->duration - 1;
    if (editingIn)
        applyClipRange(clipRangeClipId_, v - base, curOut);
    else
        applyClipRange(clipRangeClipId_, curIn, v - base);
}

// The popup closing (row action, Escape, click outside) ends any live edit, so
// text input never stays on with no field to receive it.
void App::clipRangeCloseIfMenuClosed() {
    if (clipRangeClipId_ >= 0 && !clipToolboxMenu_.isOpen()) {
        endClipRangeEdit(false);
        clipRangeClipId_ = -1;
    }
}

// Set the clip's source range, keeping its position on the timeline: sourceOffset
// becomes `inSrc` and duration spans through `outSrc` (inclusive). Both are 0-based
// source frames and are clamped to what the media has; an inverted pair collapses
// to a single frame rather than being rejected.
void App::applyClipRange(int clipId, int64_t inSrc, int64_t outSrc) {
    Clip* c = clipById(clipId);
    if (!c)
        return;
    int64_t frames = 1, base = 0;
    clipRangeSource(*c, frames, base);
    const int64_t newIn = std::clamp<int64_t>(inSrc, 0, frames - 1);
    const int64_t newOut = std::clamp<int64_t>(outSrc, newIn, frames - 1);
    const int64_t newDur = newOut - newIn + 1;
    const int64_t oldIn = c->sourceOffset, oldDur = c->duration;
    if (newIn == oldIn && newDur == oldDur)
        return;

    // Snapshot every position: growing the clip ripples its sequence and can
    // shift later sequences on repack, so undo has to restore the whole layout.
    // This re-cuts the clip exactly like a drag-trim does, so the linked audio
    // follows it the same way and rides along in the geometry snapshot.
    const int track = c->track;
    const int64_t start = c->timelineStart;
    std::vector<int> geomIds{ clipId };
    addLinkedFollowers(geomIds);
    PositionSnapshot before = capturePositions();
    std::vector<ClipGeom> geomBefore = captureClipGeom(geomIds);
    c->sourceOffset = newIn;
    c->duration = newDur;
    trimLinkedFollowers(clipId, newIn - oldIn, newDur - oldDur);
    if (Sequence* seq = timeline_.sequenceOfClipMut(clipId))
        rippleMakeRoomInSeq(*seq, track, start, newDur, clipId);
    resyncLinkedClips(); // the ripple moved one track: re-abut the pairs
    timeline_.repackSequences();
    timeline_.clampPlayhead();
    // A new cut range redraws the clip's handles, so any dissolve spending them
    // has to be re-clamped (or dropped) before `after` is snapshotted.
    pruneTransitions();
    PositionSnapshot after = capturePositions();
    std::vector<ClipGeom> geomAfter = captureClipGeom(geomIds);
    undoStack_.push({
        "SET CLIP RANGE",
        [this, clipId, before, geomBefore] {
            restorePositions(before);
            restoreClipGeom(geomBefore);
            selectClipSingle(clipId);
        },
        [this, clipId, after, geomAfter] {
            restorePositions(after);
            restoreClipGeom(geomAfter);
            selectClipSingle(clipId);
        },
    });
}


// Open the clip right-click picker menu. Navigation runs against a single
// representative path — the first selectable clip's media — so choosing an
// upstream picker cascades cleanly; the final commit re-applies the navigated
// chain to every selected clip (see startPickerCommit). Just seeds the cascade
// state and hands off to rebuildClipPickerMenu, which builds the columns.
void App::openClipPickerMenu(const std::vector<int>& clipIds, float mx, float my) {
    if (clipIds.empty())
        return;

    buildMetaPickers(); // load the config picker list on first use (needs Python)
    if (metaPickers_.empty())
        return;

    // Representative path = the first selectable clip's media; the rest ride along
    // on the final commit. Skip clips with no media so a stray selection can't
    // block the menu.
    std::vector<int> valid;
    std::string rep;
    for (int id : clipIds) {
        Clip* c = timeline_.findClipById(id);
        if (!c || c->mediaId.empty())
            continue;
        auto m = timeline_.findMediaById(c->mediaId);
        if (!m)
            continue;
        if (rep.empty())
            rep = m->resolvedPath();
        valid.push_back(id);
    }
    if (valid.empty() || rep.empty())
        return;

    menuCascade_.clipIds = std::move(valid);
    menuCascade_.repPath = std::move(rep);
    menuCascade_.chain.clear();
    // Labels and colors, whenever they arrive, go into the table in place — see
    // relabelClipPickerMenu. A decoration that lands after the menu was dismissed
    // must not put it back up, hence the isOpen() guard.
    menuCascade_.decorateApply = [this](const std::vector<PickerState>& st) {
        if (clipMenu_.isOpen())
            relabelClipPickerMenu(st);
    };
    pickerMenuX_ = mx;
    pickerMenuY_ = my;

    // The first describe runs on pickerWork_ like every click that follows, so a
    // site whose describe_pickers reaches a database can't freeze the frame the
    // right-click lands on; the menu opens when the result does. `loading` stays
    // clear on purpose — it means "an open view is waiting", and cancelPickerIfClosed
    // would read a not-yet-open menu as a cancel and drop this very query. The
    // queryId guard alone is enough: a second right-click bumps it and supersedes
    // this one.
    const uint64_t id = ++menuCascade_.queryId;
    struct DescribeResult {
        bool ok = false;
        std::vector<PickerState> states;
    };
    auto res = std::make_shared<DescribeResult>();
    const std::string path = menuCascade_.repPath;
    pickerWork_.submit(
        [path, res](const std::atomic<bool>&) {
            res->ok = jplayDescribePickers(path, res->states);
        },
        [this, id, res] {
            if (id != menuCascade_.queryId) // superseded by a newer open
                return;
            if (!res->ok)
                return;
            rebuildClipPickerMenu(res->states, /*navigating=*/false);
            // The annotations follow separately, so the columns are up as soon as
            // the option lists are known.
            startPickerDecorate(menuCascade_, res->states);
        });
}

// The values `key` currently takes across `clipIds`, deduplicated and in
// selection order, so the primary clip's own value leads. This is what the
// selection IS on, not what its shots could offer: read off each media's cached
// naming-convention values (path-derived, serialized — see Media::metaValue), so
// it costs a map lookup per clip rather than a describe query per clip.
std::vector<PickerOption> App::selectionPickerValues(const std::string& key,
                                                     const std::vector<int>& clipIds) const {
    std::vector<PickerOption> out;
    for (int id : clipIds) {
        const Clip* c = timeline_.findClipById(id);
        if (!c || c->mediaId.empty())
            continue;
        auto m = timeline_.findMediaById(c->mediaId);
        if (!m)
            continue;
        const std::string& v = m->metaValue(key);
        if (v.empty())
            continue; // this media carries no value for the key (or doesn't match the convention)
        bool seen = false;
        for (const PickerOption& o : out)
            if (o.value == v) { seen = true; break; }
        if (!seen)
            out.push_back(PickerOption{ v, v, 0, std::string(), 0 }); // undecorated; decorate_pickers may still annotate it
    }
    return out;
}

int App::pickerDisplayIndex(const std::string& key) const {
    for (int i = 0; i < (int)metaPickers_.size(); ++i)
        if (metaPickers_[i].key == key)
            return i;
    return -1;
}

// Turn a describe_pickers result into the per-picker view model for cascade `c`:
// one entry per applicable picker, in display order. Each entry's checkedIdx marks
// the representative path's current value — but only while that path is still the
// real current clip (nothing navigated yet). Once the user picks an upstream value
// the path is re-resolved onto arbitrary downstream values, so the picker right
// after the pick and the commit picker both come back unselected (see clearChecks
// below), forcing an explicit pick rather than showing a version that belongs to
// some other asset/department as pre-selected.
// Every entry except the commit one cascades (clicking re-resolves the representative
// path, drops downstream picks, and rebuilds); the commit one swaps the source.
// With several clips in the cascade the list stops earlier, at the config's
// multi_select_picker — see the clamp below.
std::vector<App::PickerColumn> App::buildPickerColumns(const std::vector<PickerState>& states,
                                                      const PickerCascade& c) const {
    auto stateFor = [&](const std::string& key) -> const PickerState* {
        for (const auto& s : states)
            if (s.key == key && !s.options.empty())
                return &s;
        return nullptr;
    };

    // Pickers more than one step past the last navigated one stay cleared (see
    // below). On first open nothing is navigated, so nothing is cleared.
    int navIdx = c.chain.empty() ? (int)metaPickers_.size()
                                 : pickerDisplayIndex(c.chain.back().first);

    // The commit picker is the last configured one that has options for this path.
    // Which pickers apply is a property of the naming convention, not of the config:
    // a take-named tree fills TAKE and leaves VERSION empty, a versioned tree the
    // other way round, and a path that carries neither ends at ASSET LAYER. Pinning
    // the commit to the last *configured* picker leaves those layouts with a trailing
    // empty section that cannot be clicked — a dead swap target, and no way to swap
    // the source at all.
    // While a query is in flight (or nothing has been described yet) the applicable
    // set is not known, so keep the last configured picker rather than let a section
    // flip its commit-ness — and its hint line — mid-query.
    int commitIdx = (int)metaPickers_.size() - 1;
    if (!states.empty() && !c.loading)
        for (int i = 0; i < (int)metaPickers_.size(); ++i)
            if (stateFor(metaPickers_[i].key))
                commitIdx = i;

    // With SEVERAL clips selected the menu stops earlier: one version number means
    // nothing across a dozen shots, so the config names where to stop
    // (multi_select_picker -> MetaPicker::multiCommit) and everything after it is
    // dropped. Each clip then resolves the pick on its own and takes the top option
    // of its own last picker — see startPickerCommit.
    //
    // The configured stop is clamped down to the last picker that actually applies
    // to this path: naming "department" on a layout that also has an asset picker
    // stops at department, but a stop naming "asset" on a path that has none would
    // otherwise land on a picker that isn't drawn, leaving nothing to commit. With
    // nothing applicable at or before the stop, the commit found above stands rather
    // than a menu that cannot commit at all.
    if (c.clipIds.size() > 1 && commitIdx > 0) {
        int stop = commitIdx - 1; // unset: the picker before the commit
        for (int i = 0; i <= commitIdx; ++i)
            if (metaPickers_[i].multiCommit) {
                stop = i;
                break;
            }
        if (stop < commitIdx) { // naming the commit picker itself asks for no truncation
            for (int i = stop; i >= 0; --i)
                if (stateFor(metaPickers_[i].key)) {
                    commitIdx = i;
                    break;
                }
        }
    }

    // What the commit picker lists with SEVERAL clips selected: the values the
    // selection itself carries, not the representative clip's siblings. The
    // describe behind those options is one clip's shot, and it says nothing about
    // the departments the other selected clips are on — while the pick applies to
    // every one of them. So the column becomes the selection's own set, and
    // picking from it moves the whole selection onto one of the values already in
    // play. Empty for a single clip (the panel's case, and a lone right-click),
    // which leaves the described list exactly as it was.
    std::vector<PickerOption> selOpts;
    if (c.clipIds.size() > 1 && commitIdx >= 0)
        selOpts = selectionPickerValues(metaPickers_[commitIdx].key, c.clipIds);

    // The "active" picker is the next real choice after the last navigated one.
    // Normally that's navIdx+1, but a forced picker there (0 or 1 option — its value
    // is already fixed in the resolved path, and it gets skipped below) offers no
    // choice, so advance over it. Without this, picking e.g. a department whose asset
    // layer has a single option strands the commit picker cleared: the asset picker
    // is dropped as "no choice" and the version picker, sitting past navIdx+1, never
    // fills in. Capped at commitIdx so the commit picker always becomes active.
    int activeIdx = navIdx + 1;
    while (activeIdx < commitIdx) {
        const PickerState* s = stateFor(metaPickers_[activeIdx].key);
        if (s && s->options.size() > 1)
            break;
        ++activeIdx;
    }

    std::vector<PickerColumn> cols;
    for (int i = 0; i <= commitIdx; ++i) { // past the commit picker nothing is shown
        const PickerState* s = stateFor(metaPickers_[i].key);
        const bool isCommit = (i == commitIdx);
        // While a query is in flight (the panel's target clip just changed) the states
        // cover at most the sections upstream of what changed, so which of the rest
        // apply isn't known yet: keep them all — cleared — rather than dropping the
        // section headers and popping them back when the result lands.
        // Otherwise every picker the path gives no options is dropped, trailing ones
        // included: with the commit now chosen from the pickers that do apply, an
        // empty VERSION on a take path (or an empty TAKE on a versioned one) is just
        // a section that doesn't belong here.
        if (!isCommit && !states.empty() && !c.loading
            && (!s || s->options.empty())) // doesn't apply to this path
            continue;
        PickerColumn col;
        col.key = metaPickers_[i].key;
        col.label = metaPickers_[i].label;
        // Column headers read as captions in both consumers, so upper-case them here
        // rather than in each view. ASCII-only: a UTF-8 label's non-ASCII bytes pass
        // through untouched.
        for (char& ch : col.label)
            if (ch >= 'a' && ch <= 'z')
                ch -= 'a' - 'A';
        col.commit = isCommit;
        // The multi-clip commit column: the selection's own values stand in for the
        // described ones (see selOpts above). Ahead of the cleared case below, so
        // the column still fills when the representative path offers this picker
        // nothing — the other selected clips' values are the list either way.
        if (isCommit && !selOpts.empty()) {
            col.options = selOpts;
            // Index 0 is the representative clip's value (it leads the selection),
            // which is the same thing the described list's currentIndex marks. A
            // navigated chain re-resolved that path, so the mark stops meaning
            // anything then — exactly as clearChecks has it below.
            col.checkedIdx = c.chain.empty() ? 0 : -1;
            cols.push_back(std::move(col));
            continue;
        }
        // Pickers beyond the active one stay listed but cleared (no options), so e.g.
        // Version keeps its slot after Department is chosen and only fills in once
        // Asset Layer is picked. The commit picker is likewise cleared while the
        // current path gives it no options.
        if (i > activeIdx || !s) {
            cols.push_back(std::move(col)); // no options => cleared
            continue;
        }
        // A single-option picker is still shown (its lone value, e.g. an asset layer
        // that has only one asset): it stays visible so the user sees what's
        // selected. It's just not the active one — activeIdx already advanced past
        // it, so the downstream commit picker fills in without an extra forced pick.
        // The picker right after an upstream pick starts unselected: the pick
        // re-resolved the path onto an arbitrary value here, so the user must
        // choose explicitly. The commit picker is likewise left unselected once
        // anything has been navigated — its checkmark only means something while the
        // path is still the real current clip, so a version resolved for some other
        // asset/department must not look pre-selected.
        const bool clearChecks = !c.chain.empty() && ((i == activeIdx && !isCommit) || isCommit);
        col.options = s->options;
        col.checkedIdx = clearChecks ? -1 : s->currentIndex; // the rep path's current value
        cols.push_back(std::move(col));
    }
    return cols;
}

// (Re)build clipMenu_ from the menu cascade: one table column per picker (label
// header at the bottom, options above), wired so every column except the last
// navigates and the last commits.
void App::rebuildClipPickerMenu(const std::vector<PickerState>& states, bool navigating) {
    std::vector<PickerColumn> picks = buildPickerColumns(states, menuCascade_);

    std::vector<ContextMenu::Column> columns;
    for (const PickerColumn& p : picks) {
        ContextMenu::Column col;
        col.label = p.label;
        for (int oi = 0; oi < (int)p.options.size(); ++oi) {
            ContextMenu::Item it;
            it.label = p.options[oi].label;      // decorated; never sent back to Python
            it.textColor = p.options[oi].color;
            it.badge = p.options[oi].badge;
            it.badgeColor = p.options[oi].badgeColor;
            it.checked = (oi == p.checkedIdx);
            const std::string key = p.key, value = p.options[oi].value;
            if (p.commit) {
                it.action = [this, key, value] {
                    clipMenu_.setLoading(true);
                    startPickerCommit(menuCascade_, key, value, [this] {
                        clipMenu_.setLoading(false);
                        clipMenu_.close();
                    });
                };
            } else {
                const int myIdx = pickerDisplayIndex(key);
                it.action = [this, key, value, myIdx] {
                    clipMenu_.setLoading(true);
                    startPickerNavigate(menuCascade_, key, value, myIdx,
                        [this](const std::vector<PickerState>& st, bool ok) {
                            clipMenu_.setLoading(false);
                            if (!ok) { // nothing to show
                                clipMenu_.close();
                                return;
                            }
                            // Also the replay path for a decoration that landed after
                            // the navigation (see startPickerNavigate), which can be
                            // a menu the user has dismissed meanwhile — rebuilding
                            // then would pop the popup back up.
                            if (!clipMenu_.isOpen())
                                return;
                            rebuildClipPickerMenu(st, /*navigating=*/true);
                        });
                };
            }
            col.items.push_back(std::move(it));
        }
        columns.push_back(std::move(col));
    }
    // Nothing to pick anywhere (e.g. the path doesn't match the naming config):
    // don't open a menu of blank columns. Close so a navigation that lands on an
    // unpickable path dismisses the panel rather than leaving stale columns up.
    bool anyItems = false;
    for (const auto& col : columns)
        if (!col.items.empty())
            anyItems = true;
    if (columns.empty() || !anyItems) {
        clipMenu_.close();
        return;
    }

    clipMenu_.openTable(pickerMenuX_, pickerMenuY_, winW_, winH_, std::move(columns), navigating);
}

// A decoration instalment landed for the open menu. Taken into the live table
// rather than reopening it: with a site that streams its labels the menu would
// otherwise be rebuilt every few seconds under the user's cursor. Falls back to a
// rebuild if the table has moved on, which only a bug in the merge rules could do.
void App::relabelClipPickerMenu(const std::vector<PickerState>& states) {
    std::vector<std::vector<ContextMenu::Label>> cols;
    for (const PickerColumn& p : buildPickerColumns(states, menuCascade_)) {
        std::vector<ContextMenu::Label> col;
        for (const PickerOption& o : p.options)
            col.push_back({ o.label, o.color, o.badge, o.badgeColor });
        cols.push_back(std::move(col));
    }
    if (!clipMenu_.relabelTable(cols))
        rebuildClipPickerMenu(states, /*navigating=*/true);
}

namespace {
// Resolve one clip's path through the whole pick chain. Runs on the picker worker
// thread (Python via the GIL), so it touches no App/timeline state. Returns false
// if an upstream pick doesn't exist for this clip; a missing exact final pick
// (`commitKey`) keeps the path resolved so far (the chosen asset's latest) and
// raises `missedPick` if given. That flag is how a caller tells "loaded what you
// asked for" from "loaded something else, or nothing at all": with no navigation
// above it the path resolved so far is the clip's own media, so the swap below
// would be a no-op the user never hears about.
bool resolvePickerChain(std::string path,
                        const std::vector<std::pair<std::string, std::string>>& chain,
                        const std::string& commitKey, std::string& out,
                        bool* missedPick = nullptr) {
    for (const auto& kv : chain) {
        std::string next;
        if (jplayResolvePath(path, kv.first, kv.second, next)) {
            path = next;
        } else if (kv.first == commitKey) {
            if (missedPick)
                *missedPick = true;
            continue;
        } else {
            return false;
        }
    }
    out = std::move(path);
    return true;
}

// The clip's own newest: re-describe `path` and resolve onto the TOP option of
// picker `key` — the last configured one, whose options come back latest-first.
// This is what makes a multi-clip pick mean "this department, each clip at its own
// latest" rather than one version number imposed on a dozen different shots. Runs
// on the picker worker beside resolvePickerChain (Python via the GIL, no App
// state). A path that doesn't offer that picker, or a resolve that fails, keeps
// `path`: the pick above it already landed on something real.
std::string topOfPicker(const std::string& path, const std::string& key) {
    if (key.empty() || path.empty())
        return path;
    std::vector<PickerState> states;
    if (!jplayDescribePickers(path, states))
        return path;
    for (const auto& s : states) {
        if (s.key != key || s.options.empty())
            continue;
        std::string next;
        if (jplayResolvePath(path, key, s.options.front().value, next) && !next.empty())
            return next;
        break;
    }
    return path;
}
} // namespace

// Upstream pick: resolve the representative path for (key,value), then re-describe.
// Runs on pickerWork_ while the view shows LOADING; the completion applies on the
// main thread unless the query was cancelled/superseded, then hands the fresh
// states to `onDone` so the calling view can rebuild itself.
void App::startPickerNavigate(PickerCascade& c, const std::string& key, const std::string& value,
                              int myIdx,
                              std::function<void(const std::vector<PickerState>&, bool)> onDone) {
    const uint64_t id = ++c.queryId;
    c.loading = true;

    struct NavResult {
        bool resolved = false;   // resolve_path succeeded
        bool described = false;  // describe_pickers succeeded (for the path below)
        std::string path;        // resolved path on success, else the unchanged rep path
        std::vector<PickerState> states;
    };
    auto res = std::make_shared<NavResult>();
    const std::string rep = c.repPath;

    pickerWork_.submit(
        [rep, key, value, res](const std::atomic<bool>&) {
            std::string next;
            res->resolved = jplayResolvePath(rep, key, value, next);
            res->path = res->resolved ? next : rep; // failure: re-describe the current path
            res->described = jplayDescribePickers(res->path, res->states);
        },
        [this, &c, id, key, value, myIdx, rep, res, onDone] {
            if (id != c.queryId) // view closed or a newer click superseded this one
                return;
            c.loading = false;
            if (!res->described) { // nothing to show
                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                            "Picker: describe_pickers failed for %s (after %s=%s)",
                            res->path.c_str(), key.c_str(), value.c_str());
                onDone({}, false);
                return;
            }
            if (!res->resolved) {
                // Leave the cascade untouched on failure so the user can choose again.
                // The states are a fresh (undecorated) describe of the same path the
                // view already showed, so they still need the decorate pass — without
                // it the failed pick would strip the annotations off the view.
                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                            "Picker: %s=%s has no media (from %s) - selection unchanged",
                            key.c_str(), value.c_str(), rep.c_str());
                setStatusWarn("NO MEDIA FOR " + value);
                onDone(res->states, true);
                startPickerDecorate(c, res->states);
                return;
            }
            c.repPath = res->path;
            // Re-picking an upstream picker invalidates any downstream picks: drop
            // chain entries at or after this picker's level, then record the pick.
            c.chain.erase(
                std::remove_if(c.chain.begin(), c.chain.end(),
                    [&](const std::pair<std::string, std::string>& kv) {
                        return pickerDisplayIndex(kv.first) >= myIdx;
                    }),
                c.chain.end());
            c.chain.push_back({ key, value });
            onDone(res->states, true);
            // Annotations for the freshly described options, applied by the view's
            // own decorateApply once they arrive.
            startPickerDecorate(c, res->states);
        });
}

void App::cancelPickerDecorate(PickerCascade& c) {
    if (!c.decorate)
        return;
    c.decorate->cancel.store(true, std::memory_order_relaxed);
    c.decorate.reset(); // the worker holds the other end and outlives this
}

void App::startPickerDecorate(PickerCascade& c, const std::vector<PickerState>& states) {
    cancelPickerDecorate(c); // whatever was decorating describes an older path
    if (states.empty() || !c.decorateApply || !jplayHasDecoratePickers())
        return;

    auto s = std::make_shared<PickerDecorateStream>();
    s->states = states;
    c.decorate = s;
    c.decorateQueryId = c.queryId; // not bumped: see the declaration in App.h

    const std::string path = c.repPath;
    // No completion callback: everything this produces reaches the main thread
    // through `s`, including the last batch, so a cancelled job simply stops
    // being polled. `stop` covers app teardown — the queue's destructor raises it
    // and a generator between batches lets go rather than holding the quit.
    decorateWork_.submit([path, s](const std::atomic<bool>& stop) {
        std::vector<PickerState> work;
        {
            std::lock_guard<std::mutex> lk(s->mtx);
            work = s->states;
        }
        const bool changed = jplayDecoratePickers(
            path, work,
            [&s](const std::vector<PickerState>& merged) {
                std::lock_guard<std::mutex> lk(s->mtx);
                s->states = merged;
                s->dirty = true;
            },
            [&s, &stop] {
                return stop.load(std::memory_order_relaxed)
                    || s->cancel.load(std::memory_order_relaxed);
            });
        // Publish the finished state too: it is the only publication a callback
        // that returns rather than yields ever makes, and a harmless repeat of the
        // last batch for one that yields.
        if (changed) {
            std::lock_guard<std::mutex> lk(s->mtx);
            s->states = std::move(work);
            s->dirty = true;
        }
        s->done.store(true, std::memory_order_release);
    });
}

void App::pollPickerDecorations() {
    auto poll = [](PickerCascade& c) {
        auto s = c.decorate; // kept alive across apply() below
        if (!s)
            return;
        if (c.decorateQueryId != c.queryId) { // the view closed, or a click moved it on
            s->cancel.store(true, std::memory_order_relaxed);
            c.decorate.reset();
            return;
        }
        std::vector<PickerState> batch;
        bool have = false;
        {
            std::lock_guard<std::mutex> lk(s->mtx);
            if (s->dirty) {
                batch = s->states;
                s->dirty = false;
                have = true;
            }
        }
        // Read after the snapshot: a batch published just before the worker
        // finished must not be dropped by releasing the stream first.
        const bool finished = s->done.load(std::memory_order_acquire);
        if (have)
            c.decorateApply(batch); // refreshes the view — never under the lock
        if (finished)
            c.decorate.reset();
    };
    poll(menuCascade_);
    poll(panelCascade_);
}

// Commit the navigated chain plus the final pick (`key`=`value`) to every clip the
// cascade covers. The per-clip resolve chain (the Python-heavy part) runs off the
// main thread — on pickerWork_ for one clip, and for several on the progress
// worker, which puts a modal dialog with a Cancel in front of the wait; each
// clip's starting path is captured now on the main thread. The completion swaps
// each clip's media on the main thread (replaceClipMedia touches the media pool /
// undo / thumbnails, so it must not run on the worker).
void App::startPickerCommit(PickerCascade& c, const std::string& key, const std::string& value,
                            std::function<void()> onDone) {
    if (value.empty())
        return;
    const uint64_t id = ++c.queryId;
    c.loading = true;

    std::vector<std::pair<std::string, std::string>> chain = c.chain;
    chain.push_back({ key, value }); // the commit picker, never part of the chain yet

    // Several clips: the menu stopped short of the last picker (see
    // buildPickerColumns), so each clip resolves the pick and then takes the top
    // option of that last picker for itself — its own newest version, not one
    // number imposed on every shot. And no commitKey: the fallback that keeps
    // "whatever resolved so far" when the exact pick is missing is right for a
    // version the user named, but here a clip that has no such department must be
    // left exactly as it is (and said so in the log), not quietly re-pointed.
    const bool multi = c.clipIds.size() > 1;
    const std::string commitKey = multi ? std::string() : key;
    const std::string topKey =
        (multi && !metaPickers_.empty() && metaPickers_.back().key != key)
            ? metaPickers_.back().key
            : std::string();

    // `from` is the clip's path going in: the resolve runs on a worker, so the
    // warning the completion may have to write can't go looking for it then.
    struct ClipResolve { int clipId; std::string from; std::string path; bool ok; bool missed; };
    auto starts = std::make_shared<std::vector<ClipResolve>>();
    for (int cid : c.clipIds) {
        Clip* c = timeline_.findClipById(cid);
        if (!c || c->mediaId.empty())
            continue;
        auto m = timeline_.findMediaById(c->mediaId);
        if (!m)
            continue;
        starts->push_back({ cid, m->resolvedPath(), std::string(), false, false });
    }
    if (starts->empty()) { // nothing the pick could apply to
        c.loading = false;
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "Picker: %s=%s not applied - no clip in the selection has media",
                    key.c_str(), value.c_str());
        setStatusWarn("NO CLIP TO APPLY " + value + " TO");
        onDone();
        return;
    }
    auto out = std::make_shared<std::vector<ClipResolve>>();

    // The per-clip resolve. One Python round-trip per clip, two once the
    // top-of-picker step joins in, so a whole selection of them is long enough to
    // owe the user a dialog: `pr` reports progress on the multi-clip path and is
    // null on the single-clip one, which stays as immediate as it always was.
    auto resolveAll = [starts, out, chain, commitKey, topKey](ProgressReporter* pr) {
        const size_t n = starts->size();
        for (size_t i = 0; i < n; ++i) {
            if (pr) {
                if (pr->cancelled())
                    return;
                pr->update((float)i / (float)n,
                           "Resolving clip " + std::to_string(i + 1) + "/" + std::to_string(n));
            }
            const auto& s = (*starts)[i];
            std::string path;
            bool missed = false;
            bool ok = resolvePickerChain(s.from, chain, commitKey, path, &missed);
            if (ok)
                path = topOfPicker(path, topKey); // no-op unless this is a multi-clip pick
            out->push_back({ s.clipId, s.from, ok ? path : std::string(), ok, missed });
        }
    };

    // Apply what resolved -- on the main thread either way, since replaceClipMedia
    // touches the media pool / undo / thumbnails.
    auto apply = [this, &c, id, key, value, out, onDone] {
        if (id != c.queryId) // cancelled or superseded
            return;
        c.loading = false;

        bool anyChanged = false;
        int missing = 0;    // the pick has no media for this clip: nothing loaded
        int fellBack = 0;   // loaded, but the asset's latest instead of this pick
        int openFailed = 0; // resolved to a path the media layer wouldn't open
        for (const auto& r : *out) {
            Clip* c = timeline_.findClipById(r.clipId);
            if (!c || c->mediaId.empty())
                continue;
            auto m = timeline_.findMediaById(c->mediaId);
            if (!m)
                continue;
            const bool sameAsNow = r.ok && r.path == m->resolvedPath();
            // Two ways a pick loads nothing at all: an upstream pick in the chain
            // has no media for this clip (!ok), or the final pick itself doesn't
            // exist and resolvePickerChain kept what it had resolved so far
            // (missed) — which, with nothing navigated above it, is this clip's
            // own media. Both used to end here without a word.
            if (!r.ok || (r.missed && sameAsNow)) {
                ++missing;
                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                            "Picker: %s=%s did not resolve for clip %d - nothing loaded, "
                            "still on %s",
                            key.c_str(), value.c_str(), r.clipId, r.from.c_str());
                continue;
            }
            if (r.missed) {
                // The navigated chain resolved but the exact pick didn't, so this
                // is the chosen asset's latest standing in for it — deliberate
                // (see resolvePickerChain), still not the version the row named.
                ++fellBack;
                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                            "Picker: %s=%s not found for clip %d (%s) - loading %s instead",
                            key.c_str(), value.c_str(), r.clipId, r.from.c_str(),
                            r.path.c_str());
            }
            if (sameAsNow) // already on it: the pick asked for nothing new
                continue;
            const std::string before = c->mediaId;
            replaceClipMedia(*c, r.path);
            if (c->mediaId == before) { // replace bailed (e.g. open failure)
                ++openFailed;
                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                            "Picker: %s=%s resolved to %s for clip %d, which failed to open",
                            key.c_str(), value.c_str(), r.path.c_str(), r.clipId);
                continue;
            }
            anyChanged = true;
        }
        // Say it where the user is looking as well as in the log: the badge
        // names the pick, the log lines above name the clip and the path.
        // Last, so it outlives replaceClipMedia's own "SWITCHED TO …".
        const int bad = missing + openFailed + fellBack;
        if (bad > 0) {
            std::string msg = missing    ? "NO MEDIA FOR " + value
                            : openFailed ? "FAILED TO OPEN " + value
                                         : value + " NOT FOUND - LOADED LATEST";
            if (bad > 1)
                msg += " (" + std::to_string(bad) + " CLIPS)";
            setStatusWarn(msg);
        }
        // Switching source gives each clip a new thumbnail key; regenerate the
        // grid's thumbnails so the new sources aren't left grey.
        if (anyChanged && gridView())
            startClipThumbnails();
        onDone();
    };

    // Several clips is the slow case, so put a modal with a Cancel in front of it.
    // beginProgress runs one task at a time; if something else already holds it,
    // resolve on pickerWork_ as usual rather than dropping the commit.
    if (multi && !progressActive()) {
        auto cancelled = std::make_shared<bool>(false);
        beginProgress("Switch Source: " + value +
                          " (" + std::to_string(starts->size()) + " clips)",
                      [resolveAll, cancelled](ProgressReporter& pr) {
                          resolveAll(&pr);
                          *cancelled = pr.cancelled();
                      },
                      [this, &c, id, apply, cancelled, onDone] {
                          if (!*cancelled) {
                              apply();
                              return;
                          }
                          // Withdrawn mid-flight: nothing has been swapped yet (every
                          // replaceClipMedia happens in apply), so the honest answer to a
                          // cancelled request is to swap nothing at all.
                          if (id == c.queryId)
                              c.loading = false;
                          setStatus("SWITCH CANCELLED");
                          onDone();
                      });
    } else {
        pickerWork_.submit([resolveAll](const std::atomic<bool>&) { resolveAll(nullptr); }, apply);
    }
}

// Switch `clipIds` to the media one picker pick names, synchronously. The
// control-channel counterpart of startPickerCommit: same per-clip resolve through
// resolvePickerChain and the same replaceClipMedia swap, but run inline on the
// calling (main) thread so a control command can report what it did in its reply.
//
// Two differences from the interactive commit, both because this is a single pick
// rather than the tail of a navigated cascade:
//   - the chain is just (key,value), with NO commitKey. A cascade passes its final
//     picker as commitKey so a missing exact version keeps the asset's latest;
//     here that would turn "this shot has no compositing" into a silent no-op, so
//     an unresolvable pick is reported as a miss instead.
//   - clips whose pick resolves to the media they already have are reported as
//     "unchanged" rather than counted as switched.
//
// Blocks for one resolve_path call per clip (each a directory scan), so a large
// timeline takes a while — the same synchronous bargain App::controlOpen makes.
std::vector<App::PickerSwitch> App::switchClipsByPicker(const std::string& key,
                                                        const std::string& value,
                                                        const std::vector<int>& clipIds) {
    std::vector<PickerSwitch> results;
    const std::vector<std::pair<std::string, std::string>> chain{ { key, value } };

    bool anyChanged = false;
    for (int cid : clipIds) {
        Clip* clip = timeline_.findClipById(cid);
        if (!clip || clip->mediaId.empty())
            continue;
        auto media = timeline_.findMediaById(clip->mediaId);
        if (!media)
            continue;

        PickerSwitch r;
        r.clipId = cid;
        r.from   = media->resolvedPath();

        std::string resolved;
        if (!resolvePickerChain(r.from, chain, /*commitKey=*/std::string(), resolved)
            || resolved.empty()) {
            r.note = "no media for " + key + "=" + value;
            results.push_back(std::move(r));
            continue;
        }
        if (resolved == r.from) {
            r.to = resolved;
            r.note = "already on this source";
            results.push_back(std::move(r));
            continue;
        }

        const std::string before = clip->mediaId;
        replaceClipMedia(*clip, resolved);
        if (clip->mediaId == before) {
            // replaceClipMedia bails without swapping when the new media won't
            // open; it has already reported why in the status line.
            r.note = "failed to open " + resolved;
        } else {
            r.to = resolved;
            r.switched = true;
            anyChanged = true;
        }
        results.push_back(std::move(r));
    }

    // Same follow-up the interactive commit does: a new source means a new
    // thumbnail key, so the grid would otherwise be left grey.
    if (anyChanged && gridView())
        startClipThumbnails();
    return results;
}

// Same resolve a commit performs — the cascade's navigated chain plus one final
// pick, starting from the cascade's clip's current path — but yielding the path
// instead of swapping any media. Inline rather than on pickerWork_: the caller (the
// panel's drag-out) needs the path before the drag can carry anything.
bool App::resolvePickerPick(const PickerCascade& c, const std::string& key,
                            const std::string& value, std::string& out,
                            bool* missedPick) const {
    if (value.empty())
        return false;
    std::string start;
    for (int cid : c.clipIds) {
        const Clip* cl = timeline_.findClipById(cid);
        if (!cl || cl->mediaId.empty())
            continue;
        if (auto m = timeline_.findMediaById(cl->mediaId)) {
            start = m->resolvedPath();
            break;
        }
    }
    if (start.empty())
        return false;
    std::vector<std::pair<std::string, std::string>> chain = c.chain;
    chain.push_back({ key, value });
    return resolvePickerChain(start, chain, key, out, missedPick) && !out.empty();
}

// Right-click in the timeline: the clip-actions menu for the clip under the
// cursor, or the metadata picker menu when Ctrl is held. Ignores presses outside
// the track rows or on empty track space.
// Right-clicking a clip outside the current selection resets the selection to
// just that clip; right-clicking a selected clip keeps the whole selection so
// the menu (and any source switch) applies to every selected clip.
void App::handleTimelineRightClick(float mx, float my) {
    if (mx < headerX_ || my < tracksTop_ || my >= tracksViewBottom())
        return;
    // In the curve lane a right-click removes the point under the cursor (or
    // resets the clip's curve from its start box) rather than opening the
    // clip-actions menu, whose rows would all act on a hidden stack.
    if (curveLaneMouseDown(mx, my, /*rightButton*/ true, 1))
        return;
    int track = trackFromY(my);
    Clip* hit = clipAtTrack(track, (int64_t)std::llround(xToFrame(mx)));
    if (!hit)
        return;
    openClipRightClickMenu(*hit, mx, my);
}

// The clip menu a right-click opens, wherever the click resolved the clip from:
// the timeline tracks or the video frame (which resolves the clip being viewed).
void App::openClipRightClickMenu(const Clip& clip, float mx, float my, bool growDown) {
    if (clip.mediaId.empty())
        return;
    if (!isClipSelected(clip.id))
        selectClipSingle(clip.id);
    if (SDL_GetModState() & SDL_KMOD_CTRL)
        openClipPickerMenu(selectedClipIds_, mx, my);
    else
        openClipContextMenu(clip, mx, my, growDown);
}

void App::replaceClipMedia(Clip& clip, const std::string& newPath) {
    ClipType type = ImageSeq::isSequencePath(newPath) ? ClipType::ImageSequence : ClipType::Video;

    // Reuse an existing pool entry; otherwise create and open new media.
    auto media = timeline_.findMediaByPath(type, newPath);
    if (!media) {
        media = std::make_shared<Media>(type, newPath);
        std::string err;
        if (!media->ensureOpen(err)) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Failed to open %s: %s",
                        newPath.c_str(), err.c_str());
            setStatusWarn("FAILED TO OPEN " + fileLabel(newPath) + ": " + err);
            return;
        }
        media->refreshMetadata();
        {
            std::map<std::string, std::string> vals;
            jplayGetPathValues(newPath, vals);
            for (auto& kv : vals)
                media->setMetaValue(kv.first, std::move(kv.second));
        }
        timeline_.media[media->id()] = media;
    }
    int64_t newFrames = std::max<int64_t>(media->info().frameCount, 1);

    std::string oldId = clip.mediaId;

    // The frame the player is showing, as (clip, offset into it) rather than as a
    // number: the re-fit below changes this clip's length and ripples every later
    // clip on its track, so an absolute playhead ends up on different content than
    // the one the user was looking at. Taken before anything moves, put back after.
    int     pinClipId = -1;
    int64_t pinOffset = 0;
    if (const Clip* pc = getTopMostClipAtFrame(timeline_.playhead)) {
        pinClipId = pc->id;
        pinOffset = timeline_.playhead - pc->timelineStart;
    }

    // Swap the media reference and re-fit the clip's span. The clip keeps its
    // timeline position and in-point; the out end re-fits within what the new
    // media can supply: it shrinks when the media is too short, and grows back
    // toward the shot's original out (never past it) when the media is longer.
    clip.mediaId = media->id();
    int64_t oldEnd      = clip.end();
    int64_t oldDuration = clip.duration;
    int64_t avail = newFrames - clip.sourceOffset;
    if (avail <= 0) {
        clip.sourceOffset = 0;
        avail = newFrames;
    }

    Shot* shot = timeline_.findShotById(clip.shotId);
    if (shot) {
        // Restore toward the original out (source frame), capped by the media.
        int64_t targetOut = std::min(shot->originalCutOut, newFrames);
        clip.duration = std::clamp<int64_t>(targetOut - clip.sourceOffset, 1, avail);
    } else {
        // No shot to anchor an original extent: only ever clamp down.
        clip.duration = std::min(clip.duration, avail);
    }
    int64_t delta = oldDuration - clip.duration; // >0 shrank, <0 grew

    // Keep the replaced clip's shot mirroring its new span and source range.
    if (shot) {
        shot->duration = clip.duration;
        shot->setCut(clip.sourceOffset, clip.sourceOffset + clip.duration);
    }

    // The clip's length changed: shift every later clip on the same track to
    // match — left to close the gap when it shrank, right to make room when it
    // grew — dragging each one's shot along. The sequence bar is derived from
    // clip spans, so it follows automatically.
    //
    // Confined to the clip's own sequence, as the trim ripple is (see
    // updateClipTrim): timelineStart is absolute, so every clip of every sequence
    // behind this one is also "later on the same track" and used to be dragged
    // along — on that one row only, which slides those sequences' own rows out of
    // sync with each other. What the sequences behind this one owe the change is a
    // uniform shift, and that is repackSequences' job.
    if (Sequence* seq = delta != 0 ? timeline_.sequenceOfClipMut(clip.id) : nullptr) {
        int64_t shift = -delta; // <0 close gap (shrank); >0 make room (grew)
        for (Clip& c : seq->clips) {
            if (c.track != clip.track || c.audio != clip.audio || c.id == clip.id) continue;
            if (c.timelineStart < oldEnd) continue;
            c.timelineStart += shift;
            if (Shot* s = timeline_.findShotById(c.shotId))
                s->timelineStart += shift;
        }
        // The shift walks one track, so any audio linked to a clip it moved was
        // left behind. Positions only: the replacement's length says nothing about
        // how long its audio is, so no duration is propagated here.
        resyncLinkedClips();
        timeline_.repackSequences(); // re-abut: this sequence changed length
    }

    // Put the frame indicator back on the frame it was on, following the clip it
    // was showing to wherever the ripple and the repack left it, and pulling in to
    // that clip's last frame if it is the one that just got shorter.
    //
    // The clip, not the number: this used to clamp against the *replaced* clip's
    // new end, which is right while the playhead sits on that clip — the single
    // switch the Clip Source panel makes — and wrong for every other clip a
    // selection-wide commit walks. Each clip in the selection ending before the
    // playhead looked like a clip that had shrunk out from under it, so the first
    // one processed dragged the playhead back to its own last frame, i.e. to the
    // top of the track.
    if (const Clip* pc = pinClipId >= 0 ? timeline_.findClipById(pinClipId) : nullptr)
        setPlayhead(pc->timelineStart + std::min(pinOffset, pc->duration - 1));

    // Drop the previous media from the pool once no clip references it.
    if (!oldId.empty() && oldId != media->id()) {
        bool stillUsed = false;
        timeline_.forEachClip([&](const Clip& c) {
            if (c.mediaId == oldId) stillUsed = true;
        });
        if (!stillUsed)
            timeline_.media.erase(oldId);
    }

    // The cache is left alone: a CacheKey names its media by id, and an id is a
    // hash of type+path, so no frame the old source left behind can be handed to
    // the replacement - and switching back to a version still resident shows it
    // with no decode at all.
    clearFramePreview();
    // hasTexture_ stays: until the replacement's own frame is composited the
    // player holds the last one up, the same hold it does for any frame that
    // hasn't decoded yet, rather than blacking the stage out for the length of a
    // decode. The new key differs from the displayed one, so the frame is still
    // re-rendered the moment it lands.
    displayedKey_ = CacheKey{};
    infoClipId_ = -1;

    setStatus("SWITCHED TO " + fileLabel(newPath));
}

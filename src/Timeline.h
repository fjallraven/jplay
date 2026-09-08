#pragma once

#include "Annotation.h"
#include "Curve.h"
#include "Media.h"

#include <algorithm>
#include <cstdint>
#include <map>
#include <memory>
#include <random>
#include <string>
#include <vector>

// A placed media instance on the timeline. Clips are owned by their Sequence.
struct Clip {
    int id = 0;
    std::string mediaId;     // key into Timeline::media
    int track = 0;           // row in the single track stack: 0 = top, higher = lower.
                             // A track holds one type only; its type is whatever its
                             // clips are (see Clip::audio / Timeline::trackKind).
    bool audio = false;      // true = audio media (skipped in compositing, drawn as a
                             // waveform); makes the track it sits on an "Audio" track.
    int64_t timelineStart = 0;
    int64_t duration = 0;
    int64_t sourceOffset = 0;
    int shotId = -1;         // id of the Shot this clip belongs to; -1 = none
    bool hidden = false;     // disabled/hidden: skipped in compositing so a lower
                             // track shows through; drawn transparent on the timeline

    // Opacity ramps at the clip's head and tail, in frames (0 = none). The clip
    // fades over whatever is beneath it: the next visible clip on a lower track,
    // or black when there is none — so "fade to black" is just the case with an
    // empty track below, not a separate feature. Unlike a Transition these spend
    // no handles (they only dim frames the clip already owns), so they work on
    // clips cut to their full source extent, and they need no anchoring: being
    // fields of the clip, they follow it through every move, trim and repack.
    // Each is capped at duration/2 (see Timeline::fadeLimit) so the two spans can
    // touch but never overlap.
    int64_t fadeInFrames = 0;
    int64_t fadeOutFrames = 0;

    // Audio-follows-video link. A clip with linkedTo != 0 is a *follower*: the id
    // names the clip it follows, always one in the same sequence. The relation is
    // asymmetric — editing the parent carries the follower along and deleting the
    // parent deletes it, but the follower never moves the parent. linkOffset is
    // the follower's start relative to the parent's, recorded when the link is
    // made (and re-recorded whenever the follower alone is slipped), so the pair
    // survives every edit that shifts both by the same amount.
    //
    // Like a Transition this is stored as a clip id rather than a position, so it
    // follows its clips through moves, ripples and repackSequences with no
    // bookkeeping, and goes stale by itself when the parent disappears. A stale
    // link is inert rather than wrong — Timeline::linkParent simply stops
    // resolving it — so nothing has to prune it, and an undo that brings the
    // parent back revives the link.
    int     linkedTo = 0;    // parent clip id; 0 = not a follower
    int64_t linkOffset = 0;  // timelineStart - parent.timelineStart

    // Per-clip volume automation, in dB (0 = unity, kVolMinDb = silence). A
    // default-constructed Curve is exactly unity, so a clip that was never touched
    // costs nothing and behaves as it always did. Anchored to source frames like
    // the annotations below, so a dip stays on the word it was drawn over.
    // Only audio clips are fed through it today (see App::updateAudio); the field
    // is on every clip because the lane that edits it is parameter-agnostic.
    Curve volume;

    // Pencil markup, keyed by source frame (sourceOffset + local offset), so it
    // stays glued to the media frame it was drawn on even when the clip is moved
    // or the timeline is repacked. Per placement: two clips referencing the same
    // media carry independent annotations. Empty frames are not stored.
    std::map<int64_t, std::vector<AnnotStroke>> annotations;

    int64_t end() const { return timelineStart + duration; }
};

// A cross-dissolve anchored to the cut between two adjacent clips on one track.
// Not a clip: it owns no media and occupies no span of its own, so it never
// changes the sequence's length.
//
// The cut is aClipId's end(), which is also bClipId's timelineStart. Anchoring to
// the pair rather than to an absolute frame means the transition follows its edit
// through moves, ripples and repackSequences with no bookkeeping, and goes stale
// by itself the moment the two clips stop being adjacent (see
// Timeline::resolveTransition — a stale transition simply doesn't render, and
// App::pruneTransitions drops it on the next edit).
//
// The span reaches inFrames before the cut and outFrames after it, and is served
// from the clips' *handles* — material outside their trimmed ranges. Before the
// cut the incoming clip has to be pulled back in front of its in point, so
// inFrames spends b's head handle; after the cut the outgoing clip runs past its
// out point, so outFrames spends a's tail handle. Timeline::transitionLimits
// states both caps.
struct Transition {
    int id = 0;
    int aClipId = 0;       // outgoing clip; the cut is its end()
    int bClipId = 0;       // incoming clip
    int64_t inFrames = 0;  // span before the cut (spends b's head handle)
    int64_t outFrames = 0; // span after the cut  (spends a's tail handle)

    int64_t duration() const { return inFrames + outFrames; }
};

// A named shot with an explicit timeline position. Optional; drawn on the
// timeline bar when present. Clips reference shots (via shotId), not the
// reverse. Global pool stored in Timeline::shots.
struct Shot {
    int id = 0;
    std::string name;
    std::string sceneName;   // scene this shot belongs to (from OTIO "scene_name"); may be empty
    int64_t timelineStart = 0;
    int64_t duration = 0;
    int64_t cutIn = 0;   // source_range start in frames
    int64_t cutOut = 0;  // source_range end in frames
    int64_t originalCutIn = 0;   // cut range as first created/imported
    int64_t originalCutOut = 0;
    bool    cutDiffer = false;   // true when cutIn/cutOut differ from the original

    int64_t end() const { return timelineStart + duration; }

    // Adopt (in, out) as both the current and original cut range; clears the diff.
    void initCut(int64_t in, int64_t out) {
        cutIn = originalCutIn = in;
        cutOut = originalCutOut = out;
        cutDiffer = false;
    }

    // Update the current cut range and flag whether it now differs from original.
    void setCut(int64_t in, int64_t out) {
        cutIn = in;
        cutOut = out;
        cutDiffer = (cutIn != originalCutIn || cutOut != originalCutOut);
    }
};

// Sequence background palette: an 8 x 8 grid of packed 0xRRGGBB tints. One hue
// family per row (navy, indigo, plum, rust, olive, forest, teal, slate), each
// ramped left to right from dark and muted to richer and brighter; all stay dark
// enough that the white sequence-bar label reads on top. The color button on the
// Project Explorer's sequence rows offers the whole grid as unlabelled swatches.
inline constexpr int kSeqBgCols = 8;
inline constexpr int kSeqBgRows = 8;
inline constexpr uint32_t kSeqBgColors[kSeqBgRows * kSeqBgCols] = {
    0x1D212B, 0x1D2537, 0x1D2842, 0x1A2A4E, 0x172C5C, 0x122D6A, 0x0B2D79, 0x002A89, // navy
    0x201D29, 0x241F34, 0x28203F, 0x2A1F4A, 0x2D1E57, 0x2F1C65, 0x2F1873, 0x2D1182, // indigo
    0x20181E, 0x291B25, 0x311D2C, 0x3A1E33, 0x441F3B, 0x4F2043, 0x5A1F4B, 0x661C53, // plum
    0x291E1D, 0x34211F, 0x3F2321, 0x4A2320, 0x572420, 0x65231E, 0x73201A, 0x821B13, // rust
    0x221F18, 0x2B2619, 0x352D19, 0x3E3418, 0x493C17, 0x544314, 0x604B10, 0x6C5209, // olive
    0x18201D, 0x1B2923, 0x1D3129, 0x1E3A2E, 0x1F4435, 0x204F3B, 0x1F5A41, 0x1C6646, // forest
    0x172022, 0x17282B, 0x163035, 0x14383E, 0x124149, 0x0D4A54, 0x085360, 0x005D6C, // teal
    0x1E1E20, 0x252529, 0x2C2C31, 0x33333A, 0x3B3B44, 0x43434F, 0x4B4B5A, 0x535366, // slate
};

// A calm color for a newly created sequence: only the muted left half of a row's
// ramp, and never the rust row, so nothing arrives shouting or reading as an
// error state. The whole grid is still reachable from the color button.
inline uint32_t randomSeqBgColor() {
    static thread_local std::mt19937 rng{ std::random_device{}() };
    constexpr int kRustRow = 3;
    int row = std::uniform_int_distribution<int>(0, kSeqBgRows - 2)(rng);
    if (row >= kRustRow) ++row;                                     // skip the reds
    int col = std::uniform_int_distribution<int>(0, 3)(rng);         // muted half only
    return kSeqBgColors[row * kSeqBgCols + col];
}

// A project document some of this timeline's sequences were imported from (an
// .otio, or a .jpproj opened as a project). Identity is the record, not the name:
// two files whose stems match are two projects, which is why the popup groups them
// apart and why opening the second one still reads it.
struct SourceProject {
    int id = 0;
    std::string name;         // display name, the file's stem
    std::string path;         // the file it was read from; empty if not known
    // The file was read whole, so every sequence it holds is loaded and this
    // project's sequence list is complete. Only then does the Sequence popup give it
    // a header row — a header promises to list all of them. A project reached by
    // grafting one sequence at a time stays false until it is opened whole.
    bool openedWhole = false;
};

// A named grouping that owns its clips and references shots by id. Projects
// must contain at least one Sequence; one called "Default Sequence" is created
// automatically when the first clip is added to an empty project.
struct Sequence {
    int id = 0;
    std::string name;
    // The project this sequence was imported from (an id into Timeline::projects),
    // or -1 for one that came out of no project file — every sequence the user
    // creates in the app. Those stay ungrouped in the Sequence popup, one row each.
    int projectId = -1;
    // Scratch sequence that lives only in memory: the source view (App::
    // openSourceView), which shows one source on its own so a bin double-click or
    // F1 on the clip under the playhead needs no room in the cut, and the Layout
    // stage (App::openLayoutView), which stacks the clips being compared one per
    // video track so they can be tiled. Neither is part of the cut. Everything that
    // persists or enumerates the project skips it — Project::saveTo (so it reaches
    // neither the file nor the dirty signature), OtioExport, the Project Explorer's
    // sequence tree and the Sequence popup. There is only ever one, and it is
    // discarded the moment the view scope leaves it.
    bool temporary = false;
    // Timeline sequence-bar fill, packed 0xRRGGBB. Randomized on construction so
    // every newly created sequence — in the app, on OTIO import, or from the
    // command-line tools — gets a color without each site picking one; a loaded
    // project overwrites it with the stored value.
    uint32_t bgColor = randomSeqBgColor();
    std::vector<int> shotIds;  // shot ids belonging to this sequence
    std::vector<Clip> clips;   // placed clips owned by this sequence
    // Dissolves on this sequence's cuts. Both clips of a transition are always
    // clips of this sequence.
    std::vector<Transition> transitions;
};

struct Timeline {
    double fps = 24.0;
    int64_t playhead = 0;
    int64_t inPoint = 0;
    int64_t outPoint = -1; // -1 = unset (end of timeline)
    int trackCount = 2;      // rows in the single track stack (top to bottom)
    // Custom track labels, indexed by track number. An entry that is empty (or
    // past the end of the vector) means the row shows its derived name instead
    // ("Video 1" / "Audio 1" / "Track N", see renderTimeline). Written only by
    // the inline rename on the track header; persisted per-project.
    std::vector<std::string> trackNames;
    // Disabled track rows, indexed by track number (non-zero = disabled). A
    // disabled row is skipped in compositing and audio exactly as if every clip
    // on it carried Clip::hidden; the per-clip flags are left untouched, so
    // re-enabling the row restores whatever each clip was. Toggled by the eye in
    // the track header; persisted per-project. Rows past the end are enabled.
    std::vector<uint8_t> disabledTracks;

    // Letterbox matte: mask the program image to a target aspect ratio (W/H).
    // 0 = off. Bars go top/bottom when the target is wider than the image, or
    // left/right when narrower. Applied in the player, the review monitor, and
    // the external (NDI/SDI) output. Persisted per-project (see Project.cpp).
    double letterboxRatio = 0.0;
    float  letterboxOpacity = 1.0f; // matte bar opacity, 0..1 (1 = solid black)

    // Color management: true = OCIO display transform, false = built-in
    // scene-linear→sRGB fallback. Persisted per-project; applied to the
    // OcioManager on load. Both a fresh session and File > New set it from the
    // [color_management] color_pipeline preference (see OcioManager::prefersOcio),
    // so this initialiser never decides the mode on its own.
    bool ocioEnabled = false;

    // The OCIO view transform the project was last reviewed through ("" = take the
    // config's default view). Persisted per-project and handed to the OcioManager as
    // its preferred view on load, so it survives the per-source config switching
    // rather than only until the playhead reaches a config not yet seen.
    std::string ocioView;

    // The global proxy/full media-representation mode last selected in the
    // top-bar "Proxy" dropdown ("" = built-in "Full"). Persisted per-project
    // and handed to jplay::setProxyMode on load (see ProxyMode.h); the values
    // beyond "" are whatever the project's naming config's list_proxy_modes()
    // callback supplies, so this is opaque to the Timeline itself.
    std::string proxyMode;

    std::map<std::string, std::shared_ptr<Media>> media; // keyed by Media::id
    std::vector<Shot>     shots;     // global pool; optional (empty = none shown)
    std::vector<Sequence> sequences; // at least one required
    // The project documents these sequences were imported from, referenced by
    // Sequence::projectId. Empty in a project only ever built in the app.
    std::vector<SourceProject> projects;

    // ---- media lookup ---------------------------------------------------

    std::shared_ptr<Media> findMediaById(const std::string& id) const {
        auto it = media.find(id);
        return it != media.end() ? it->second : nullptr;
    }

    // Scan pool for an entry with matching (type, path) — used for dedup on add.
    std::shared_ptr<Media> findMediaByPath(ClipType type, const std::string& path) const {
        for (const auto& kv : media)
            if (kv.second && kv.second->type() == type && kv.second->path() == path)
                return kv.second;
        return nullptr;
    }

    // Keep old name so callers that haven't been updated yet still compile.
    std::shared_ptr<Media> findMedia(ClipType type, const std::string& path) const {
        return findMediaByPath(type, path);
    }

    // ---- clip helpers ---------------------------------------------------

    // Call fn(const Clip&) for every clip across all sequences.
    template<typename Fn>
    void forEachClip(Fn&& fn) const {
        for (const auto& s : sequences)
            for (const auto& c : s.clips)
                fn(c);
    }

    // Sequence the track helpers below are confined to (an id, or -1 for every
    // sequence, which is the normal state). App sets this while a scratch view is
    // up: the rows on screen are then that one sequence's alone, so the cut's
    // clips must not type a row, occupy it or extend it. Runtime only -- it is
    // neither serialized nor carried in an undo snapshot.
    int trackScopeSeqId = -1;

    // Call fn(const Clip&) for every clip the track helpers can see: all of them,
    // or one sequence's while a scratch view is up (see trackScopeSeqId).
    template<typename Fn>
    void forEachTrackClip(Fn&& fn) const {
        for (const auto& s : sequences) {
            if (trackScopeSeqId >= 0 && s.id != trackScopeSeqId)
                continue;
            for (const auto& c : s.clips)
                fn(c);
        }
    }

    // Call fn(Clip&) for every clip across all sequences.
    template<typename Fn>
    void forEachClipMut(Fn&& fn) {
        for (auto& s : sequences)
            for (auto& c : s.clips)
                fn(c);
    }

    const Clip* findClipById(int id) const {
        for (const auto& s : sequences)
            for (const auto& c : s.clips)
                if (c.id == id) return &c;
        return nullptr;
    }

    Clip* findClipById(int id) {
        for (auto& s : sequences)
            for (auto& c : s.clips)
                if (c.id == id) return &c;
        return nullptr;
    }

    // The sequence that owns clipId, or null.
    const Sequence* sequenceOfClip(int clipId) const {
        for (const auto& s : sequences)
            for (const auto& c : s.clips)
                if (c.id == clipId) return &s;
        return nullptr;
    }

    Sequence* sequenceOfClipMut(int clipId) {
        for (auto& s : sequences)
            for (auto& c : s.clips)
                if (c.id == clipId) return &s;
        return nullptr;
    }

    Sequence* findSequenceById(int id) {
        for (auto& s : sequences)
            if (s.id == id) return &s;
        return nullptr;
    }

    // ---- audio/video links ----------------------------------------------

    // The clip `c` follows, or null when it follows nothing or the link has gone
    // stale (parent removed, or no longer in the same sequence). Callers must
    // treat null as "not linked".
    const Clip* linkParent(const Clip& c) const {
        if (c.linkedTo == 0)
            return nullptr;
        const Sequence* s = sequenceOfClip(c.id);
        if (!s)
            return nullptr;
        for (const auto& k : s->clips)
            if (k.id == c.linkedTo)
                return &k;
        return nullptr;
    }

    // Ids of the clips following `clipId` (its own sequence only, which is where
    // every link lives). Empty when it has no followers.
    std::vector<int> linkFollowers(int clipId) const {
        std::vector<int> ids;
        if (const Sequence* s = sequenceOfClip(clipId))
            for (const auto& c : s->clips)
                if (c.linkedTo == clipId)
                    ids.push_back(c.id);
        return ids;
    }

    // ---- transitions ----------------------------------------------------

    // Source frames a clip's media can supply (>=1). Used to clamp trims so a
    // clip never references frames the source doesn't have, and to measure the
    // tail handle a transition can borrow.
    int64_t clipSourceFrames(const Clip& c) const {
        if (auto pm = findMediaById(c.mediaId)) {
            int64_t n = pm->info().frameCount;
            if (n > 0) return n;
        }
        return c.sourceOffset + c.duration; // unknown: pin to current extent (no growth)
    }

    // Longest legal fade at either end of `c`. Half the clip, so a head and a tail
    // fade can touch but never overlap — the same rule transitionLimits uses, which
    // is what lets one clip carry a dissolve on one cut and a fade at its other end
    // with no interaction.
    static int64_t fadeLimit(const Clip& c) { return std::max<int64_t>(c.duration / 2, 0); }

    // Effective ramp lengths: the stored values clamped to what the clip's current
    // duration allows. Clamping on read rather than rewriting the clip on every
    // trim is what keeps fades out of the edit paths entirely — no trim, drop,
    // ripple or repack has to know they exist — and it means shortening a clip and
    // lengthening it again restores the fade the user asked for instead of having
    // quietly destroyed it.
    static void clipFades(const Clip& c, int64_t& in, int64_t& out) {
        const int64_t cap = fadeLimit(c);
        in  = std::clamp<int64_t>(c.fadeInFrames, 0, cap);
        out = std::clamp<int64_t>(c.fadeOutFrames, 0, cap);
    }

    // Opacity of `c` at `frame`, 1 outside its ramps. Reaches exactly 0 on the
    // clip's first/last frame — a fade is expected to bottom out at nothing, unlike
    // a dissolve, whose ramp deliberately avoids pure-either-side frames.
    static float clipOpacity(const Clip& c, int64_t frame) {
        int64_t fin = 0, fout = 0;
        clipFades(c, fin, fout);
        if (fin > 0 && frame < c.timelineStart + fin)
            return (float)(frame - c.timelineStart) / (float)fin;
        if (fout > 0 && frame >= c.end() - fout)
            return (float)(c.end() - 1 - frame) / (float)fout;
        return 1.0f;
    }

    // Resolved geometry of a transition, valid only while it is live.
    struct TransitionSpan {
        const Clip* a = nullptr;    // outgoing
        const Clip* b = nullptr;    // incoming
        int64_t cut = 0;            // a->end() == b->timelineStart
        int64_t start = 0, end = 0; // [cut - inFrames, cut + outFrames)
    };

    // Resolve `t` against `s`. False when it has gone stale: either clip is gone,
    // they are no longer on the same track or no longer abut, or the span is
    // empty. Callers must treat a false return as "no transition here".
    bool resolveTransition(const Sequence& s, const Transition& t, TransitionSpan& out) const {
        const Clip* a = nullptr;
        const Clip* b = nullptr;
        for (const auto& c : s.clips) {
            if (c.id == t.aClipId) a = &c;
            if (c.id == t.bClipId) b = &c;
        }
        if (!a || !b || a->track != b->track || a->audio || b->audio)
            return false;
        if (a->end() != b->timelineStart)
            return false;
        if (t.duration() <= 0)
            return false;
        out.a = a;
        out.b = b;
        out.cut = a->end();
        out.start = out.cut - t.inFrames;
        out.end = out.cut + t.outFrames;
        return true;
    }

    // Largest legal inFrames / outFrames for a dissolve on the cut between `a`
    // and `b`. Each side is bounded by two things:
    //
    //  - the handle it spends: inFrames pulls `b` back in front of its in point,
    //    outFrames runs `a` past its out point;
    //  - half the clip the span lies inside. A clip can carry a dissolve on each
    //    end, and the two would fight over the middle if either could claim more
    //    than half: with both capped at half, in + out <= duration, so the spans
    //    can touch but never overlap and every frame belongs to one dissolve.
    void transitionLimits(const Clip& a, const Clip& b, int64_t& maxIn, int64_t& maxOut) const {
        const int64_t bHead = b.sourceOffset;                                    // frames before b's in point
        const int64_t aTail = clipSourceFrames(a) - (a.sourceOffset + a.duration); // frames past a's out point
        maxIn  = std::max<int64_t>(std::min(bHead, a.duration / 2), 0);
        maxOut = std::max<int64_t>(std::min(aTail, b.duration / 2), 0);
    }

    // ---- project helpers ------------------------------------------------

    SourceProject* findProjectById(int id) {
        if (id < 0) return nullptr;
        for (auto& p : projects)
            if (p.id == id) return &p;
        return nullptr;
    }

    const SourceProject* findProjectById(int id) const {
        if (id < 0) return nullptr;
        for (const auto& p : projects)
            if (p.id == id) return &p;
        return nullptr;
    }

    // The project a file maps to, matched on path so two same-stem files stay
    // apart. A record with no path of its own can only be matched by name — which
    // is what an older project file's sequences migrate to.
    const SourceProject* findProjectByPath(const std::string& path,
                                           const std::string& name) const {
        if (!path.empty())
            for (const auto& p : projects)
                if (p.path == path) return &p;
        if (!name.empty())
            for (const auto& p : projects)
                if (p.path.empty() && p.name == name) return &p;
        return nullptr;
    }

    // findProjectByPath, recording the project if it isn't known yet. Hands back the
    // id rather than a pointer: adding a record can reallocate the vector, so a
    // caller that goes on to add another must not be holding one.
    int projectIdForPath(const std::string& path, const std::string& name) {
        if (const SourceProject* p = findProjectByPath(path, name))
            return p->id;
        int maxId = 0;
        for (const auto& p : projects)
            maxId = std::max(maxId, p.id);
        SourceProject rec;
        rec.id   = maxId + 1;
        rec.name = name;
        rec.path = path;
        projects.push_back(std::move(rec));
        return projects.back().id;
    }

    // The name to show for a sequence's project; empty for one belonging to none.
    std::string projectNameOfSeq(const Sequence& s) const {
        const SourceProject* p = findProjectById(s.projectId);
        return p ? p->name : std::string();
    }

    // ---- shot helpers ---------------------------------------------------

    Shot* findShotById(int id) {
        for (auto& s : shots)
            if (s.id == id) return &s;
        return nullptr;
    }

    const Shot* findShotById(int id) const {
        for (const auto& s : shots)
            if (s.id == id) return &s;
        return nullptr;
    }

    // The scene a named sequence belongs to: the sceneName of the first shot in
    // that sequence carrying one. Empty if no such sequence, or none of its
    // shots have a scene tag.
    std::string sceneForSequence(const std::string& sequenceName) const {
        for (const auto& seq : sequences) {
            if (seq.name != sequenceName) continue;
            for (int sid : seq.shotIds)
                if (const Shot* sh = findShotById(sid))
                    if (!sh->sceneName.empty())
                        return sh->sceneName;
        }
        return {};
    }

    // Whether any shot of `seq` is tagged with `sceneName` (OTIO "scene_name").
    // A sequence out of the OTIO is named after its sequence_name and can span
    // several scenes, so this — not the sequence's name — is what ties one back to
    // the scene a media path resolves to.
    bool sequenceHasScene(const Sequence& seq, const std::string& sceneName) const {
        if (sceneName.empty())
            return false;
        for (int sid : seq.shotIds)
            if (const Shot* sh = findShotById(sid))
                if (sh->sceneName == sceneName)
                    return true;
        return false;
    }

    // ---- timeline metrics -----------------------------------------------

    bool hasClips() const {
        for (const auto& s : sequences)
            if (!s.clips.empty()) return true;
        return false;
    }

    int64_t length() const {
        int64_t n = 0;
        forEachClip([&](const Clip& c) { n = std::max(n, c.end()); });
        return n;
    }

    int64_t effectiveOut() const {
        int64_t last = std::max<int64_t>(length() - 1, 0);
        return (outPoint >= 0 && outPoint <= last) ? outPoint : last;
    }

    // Topmost (lowest track index) video clip covering the frame. Audio clips
    // never composite, so they are always skipped. Hidden clips are skipped (so
    // a lower track shows through where a higher one is disabled) unless
    // includeHidden is set.
    const Clip* clipAt(int64_t frame, bool includeHidden = false) const {
        const Clip* best = nullptr;
        forEachClip([&](const Clip& c) {
            if (c.audio || (clipDisabled(c) && !includeHidden))
                return;
            if (frame >= c.timelineStart && frame < c.end())
                if (!best || c.track < best->track)
                    best = &c;
        });
        return best;
    }

    // A single index space: a track number identifies one row, and every clip on
    // it shares one type (video or audio). Track helpers key on the number alone.
    enum class TrackKind { Empty, Video, Audio };

    // Type of a track, derived from the clips currently on it (Empty if none).
    TrackKind trackKind(int track) const {
        TrackKind k = TrackKind::Empty;
        forEachTrackClip([&](const Clip& c) {
            if (c.track == track) k = c.audio ? TrackKind::Audio : TrackKind::Video;
        });
        return k;
    }

    int64_t trackEnd(int track) const {
        int64_t n = 0;
        forEachTrackClip([&](const Clip& c) {
            if (c.track == track) n = std::max(n, c.end());
        });
        return n;
    }

    // Custom label for `track`, or empty when the row has none. Names belong to the
    // project's stack, so a scratch view's rows have none of their own and must not
    // borrow the ones sitting at the same indices (see trackScopeSeqId).
    const std::string& trackName(int track) const {
        static const std::string none;
        if (trackScopeSeqId >= 0)
            return none;
        return (track >= 0 && track < (int)trackNames.size()) ? trackNames[track] : none;
    }

    void setTrackName(int track, const std::string& name) {
        if (track < 0) return;
        if ((int)trackNames.size() <= track) trackNames.resize((size_t)track + 1);
        trackNames[track] = name;
    }

    // Whether `track` is switched off (its clips contribute nothing). Off states
    // belong to the project's stack, like the names above: a row switched off in the
    // cut must not silently blank the scratch view's row at that index — which, on
    // the Layout stage, is a clip the user explicitly asked to compare.
    bool trackDisabled(int track) const {
        if (trackScopeSeqId >= 0)
            return false;
        return track >= 0 && track < (int)disabledTracks.size() && disabledTracks[track] != 0;
    }

    void setTrackDisabled(int track, bool off) {
        if (track < 0) return;
        if ((int)disabledTracks.size() <= track) disabledTracks.resize((size_t)track + 1, 0);
        disabledTracks[track] = off ? 1 : 0;
    }

    // A clip contributes nothing while it is hidden itself or its track is off.
    bool clipDisabled(const Clip& c) const { return c.hidden || trackDisabled(c.track); }

    bool trackEmpty(int track) const {
        bool any = false;
        forEachTrackClip([&](const Clip& c) { if (c.track == track) any = true; });
        return !any;
    }

    bool trackHasOverlap(int track, int64_t start, int64_t dur, int excludeId) const {
        int64_t e = start + dur;
        bool found = false;
        forEachTrackClip([&](const Clip& c) {
            if (found || c.id == excludeId || c.track != track) return;
            if (start < c.end() && e > c.timelineStart) found = true;
        });
        return found;
    }

    // Span of a sequence based on its owned clips. Returns false if empty.
    bool sequenceSpan(const Sequence& s, int64_t& start, int64_t& end) const {
        if (s.clips.empty()) return false;
        start = s.clips[0].timelineStart;
        end   = s.clips[0].end();
        for (const auto& c : s.clips) {
            start = std::min(start, c.timelineStart);
            end   = std::max(end, c.end());
        }
        return true;
    }

    // ---- multi-sequence layout ------------------------------------------
    // Sequences are laid end-to-end on one concatenated timeline, auto-packed
    // (no gaps between sequences). In memory a clip's timelineStart is its
    // absolute position on that concatenated axis; on disk it is stored
    // relative to its owning sequence (see Project.cpp). These helpers expose
    // each sequence's absolute region so editing can be confined to one
    // sequence and reordering can shift whole sequences as a block.

    // Absolute [start,end) each sequence occupies, in sequence order. Empty
    // sequences are zero-width points. The first non-empty sequence keeps its
    // own leading position; later ones are reported packed against the prior.
    struct SeqRegion { int64_t start = 0; int64_t end = 0; };
    std::vector<SeqRegion> seqRegions() const {
        std::vector<SeqRegion> regs(sequences.size());
        int64_t cursor = 0;
        bool first = true;
        for (size_t i = 0; i < sequences.size(); ++i) {
            int64_t a = 0, b = 0;
            if (!sequenceSpan(sequences[i], a, b)) {
                regs[i] = { cursor, cursor }; // empty: zero-width
                continue;
            }
            if (first) { cursor = a; first = false; } // keep the timeline's leading offset
            regs[i] = { cursor, cursor + (b - a) };
            cursor = regs[i].end;
        }
        return regs;
    }

    // Index of the sequence owning clipId, or -1.
    int seqIndexOfClip(int clipId) const {
        for (size_t i = 0; i < sequences.size(); ++i)
            for (const auto& c : sequences[i].clips)
                if (c.id == clipId) return (int)i;
        return -1;
    }

    // Sequence index whose region contains an absolute frame (the last sequence
    // for frames at/after the end); -1 only when there are no sequences.
    int seqIndexAtFrame(int64_t frame) const {
        auto regs = seqRegions();
        for (size_t i = 0; i < regs.size(); ++i)
            if (frame < regs[i].end || i + 1 == regs.size()) return (int)i;
        return regs.empty() ? -1 : (int)regs.size() - 1;
    }

    // Re-pack sequences so each non-empty one begins exactly where the previous
    // ended, shifting that sequence's clips and linked shots as a block. The
    // first non-empty sequence keeps its leading position (so single-sequence
    // projects never move). Call after any edit that changes a sequence's
    // extent, and after reordering the sequence list.
    //
    // When pinLead is given, the packed result is anchored so the first
    // non-empty sequence begins exactly at *pinLead. Reordering changes which
    // sequence is first; without a pin the timeline would jump to that
    // sequence's own leading position, so pass the pre-reorder leading edge.
    void repackSequences(const int64_t* pinLead = nullptr) {
        int64_t cursor = 0;
        bool first = true;
        for (auto& s : sequences) {
            int64_t a = 0, b = 0;
            if (!sequenceSpan(s, a, b))
                continue; // empty: contributes no width, defines no offset
            if (first) { cursor = pinLead ? *pinLead : a; first = false; }
            int64_t delta = cursor - a;
            if (delta != 0) {
                for (auto& c : s.clips)
                    c.timelineStart += delta;
                for (int sid : s.shotIds)
                    if (Shot* sh = findShotById(sid))
                        sh->timelineStart += delta;
            }
            cursor = b + delta;
        }
    }

    // Absolute frame the timeline's content starts at (the first non-empty
    // sequence's leading position); 0 when there are no clips.
    int64_t leadFrame() const {
        auto regs = seqRegions();
        for (size_t i = 0; i < sequences.size(); ++i)
            if (!sequences[i].clips.empty())
                return regs[i].start;
        return 0;
    }

    // Pull the whole concatenated timeline back so its first clip sits on frame 0,
    // carrying the playhead and in/out points along so they stay on the same
    // content. A lead is only ever residue — content that used to sit in front of
    // the first clip and is now gone — since a gap before the first clip can be
    // neither selected nor deleted in the timeline. Left in place it shows up as a
    // black head on the program and offsets every sequence behind it.
    void normalizeLead() {
        const int64_t lead = leadFrame();
        if (lead == 0)
            return;
        const int64_t zero = 0;
        repackSequences(&zero);
        playhead = std::max<int64_t>(playhead - lead, 0);
        inPoint  = std::max<int64_t>(inPoint - lead, 0);
        if (outPoint >= 0)
            outPoint = std::max<int64_t>(outPoint - lead, 0);
        clampPlayhead();
    }

    void clampPlayhead() {
        int64_t last = std::max<int64_t>(length() - 1, 0);
        playhead = std::min(std::max<int64_t>(playhead, 0), last);
    }
};

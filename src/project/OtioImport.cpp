#include "OtioImport.h"

#include "ImageSeq.h"
#include "Media.h"

#include <opentimelineio/anyDictionary.h>
#include <opentimelineio/clip.h>
#include <opentimelineio/externalReference.h>
#include <opentimelineio/gap.h>
#include <opentimelineio/item.h>
#include <opentimelineio/serializableObject.h>
#include <opentimelineio/stack.h>
#include <opentimelineio/timeline.h>
#include <opentimelineio/track.h>
#include <opentimelineio/transition.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>

namespace fs = std::filesystem;
namespace otio = OTIO_NS;

namespace {

// OTIO times carry their own rate; rescale to the timeline rate and round to a
// whole frame so positions/durations are exact integer frame counts.
int64_t toFrames(const opentime::RationalTime& rt, double fps) {
    return (int64_t)std::llround(rt.value_rescaled_to(fps));
}

// Turn a media reference's target_url into a local filesystem path: strip a
// file:// scheme (handling the file:///C:/... Windows-drive form) and resolve a
// relative path against the directory holding the .otio file.
std::string resolveUrl(const std::string& url, const fs::path& baseDir) {
    std::string p = url;
    const std::string scheme = "file://";
    if (p.rfind(scheme, 0) == 0) {
        p = p.substr(scheme.size());
        if (p.size() >= 3 && p[0] == '/' && p[2] == ':') // file:///C:/x -> C:/x
            p = p.substr(1);
    }
    fs::path path(p);
    if (path.is_relative())
        path = baseDir / path;
    return path.lexically_normal().string();
}

// The path to hand Media for a media reference. A frame-pattern target_url
// ("shot.#.exr" — what create_otio_project writes for media it could not read)
// carries no frame number, and it stays a pattern here: only the writer knows which
// frame actually exists, and a pattern is what ImageSeq globs correctly. Guessing
// the first frame from available_range does not work in general — that number comes
// from the production configs, which routinely disagree with the frames on disk, and
// a path naming a frame that isn't there reads as missing media.
//
// A verified clip is different: its available_range start was read off the disk, so
// substituting it names a real file, and Media then holds a concrete path exactly as
// a browsed or dropped sequence does — nothing has to be opened to resolve it.
std::string mediaPath(const std::string& url, const fs::path& baseDir,
                      int64_t firstFrame, bool verified) {
    const std::string resolved = resolveUrl(url, baseDir);
    if (!verified || firstFrame <= 0 || !ImageSeq::isSequencePath(resolved))
        return resolved;
    return ImageSeq::substituteFrame(resolved, firstFrame);
}

std::string toLower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return (char)std::tolower(c); });
    return s;
}

// Read shot_name from OTIO metadata, falling back to the clip's name().
std::string shotNameOf(const otio::Clip* clip) {
    const otio::AnyDictionary& meta = clip->metadata();
    auto it = meta.find("shot_name");
    if (it != meta.end()) {
        try { return std::any_cast<std::string>(it->second); } catch (...) {}
    }
    return clip->name();
}

// Read an integer from OTIO metadata. OTIO's AnyDictionary keeps whatever type the
// JSON held, so a number can arrive as int64_t or double depending on how it was
// written; 0 when the key is absent or holds neither.
int64_t intMetaOf(const otio::Clip* clip, const char* key) {
    const otio::AnyDictionary& meta = clip->metadata();
    auto it = meta.find(key);
    if (it == meta.end())
        return 0;
    try { return std::any_cast<int64_t>(it->second); } catch (...) {}
    try { return (int64_t)std::any_cast<double>(it->second); } catch (...) {}
    return 0;
}

// True when the writer verified this clip's media on disk and so wrote real
// dimensions and a real available_range (create_otio_project's media_verified).
bool mediaVerifiedOf(const otio::Clip* clip) {
    const otio::AnyDictionary& meta = clip->metadata();
    auto it = meta.find("media_verified");
    if (it == meta.end())
        return false;
    try { return std::any_cast<bool>(it->second); } catch (...) {}
    return false;
}

// Read sequence_name from OTIO metadata; empty if the clip carries none.
std::string sequenceNameOf(const otio::Clip* clip) {
    const otio::AnyDictionary& meta = clip->metadata();
    auto it = meta.find("sequence_name");
    if (it != meta.end()) {
        try { return std::any_cast<std::string>(it->second); } catch (...) {}
    }
    return {};
}

// Read scene_name from OTIO metadata; empty if the clip carries none.
std::string sceneNameOf(const otio::Clip* clip) {
    const otio::AnyDictionary& meta = clip->metadata();
    auto it = meta.find("scene_name");
    if (it != meta.end()) {
        try { return std::any_cast<std::string>(it->second); } catch (...) {}
    }
    return {};
}

// Find the best shot for a clip: name match first, then largest timeline overlap.
// Only shots from `from` on are candidates, which is how the nested shape keeps a
// clip inside its own sequence (see OtioImport::load).
Shot* matchShot(std::vector<Shot>& shots, size_t from, const std::string& clipName,
                int64_t clipStart, int64_t clipEnd) {
    if (!clipName.empty()) {
        const std::string low = toLower(clipName);
        for (size_t i = from; i < shots.size(); ++i)
            if (toLower(shots[i].name) == low)
                return &shots[i];
    }
    Shot* best = nullptr;
    int64_t bestOverlap = 0;
    for (size_t i = from; i < shots.size(); ++i) {
        Shot& s = shots[i];
        int64_t oStart = std::max(clipStart, s.timelineStart);
        int64_t oEnd   = std::min(clipEnd, s.end());
        if (oEnd > oStart && (oEnd - oStart) > bestOverlap) {
            bestOverlap = oEnd - oStart;
            best = &s;
        }
    }
    return best;
}

} // namespace

bool OtioImport::load(const std::string& path, Timeline& out,
                      int& nextClipId, int& nextSeqId, int& nextShotId,
                      std::string& err) {
    otio::ErrorStatus es;
    otio::SerializableObject::Retainer<otio::SerializableObject> root(
        otio::SerializableObject::from_json_file(path, &es));
    if (!root || otio::is_error(es)) {
        err = otio::is_error(es) ? es.full_description : "cannot read OTIO file";
        return false;
    }
    auto* timeline = dynamic_cast<otio::Timeline*>(root.value);
    if (!timeline) {
        err = "not an OTIO timeline";
        return false;
    }

    double fps = timeline->duration(&es).rate();
    if (!(fps > 0.0))
        fps = 24.0;

    Timeline tl;
    tl.fps = fps;
    const fs::path baseDir = fs::path(path).parent_path();

    // In the flat shape (see below) clips are grouped into jplay sequences by their
    // "sequence_name" metadata. Clips with no tag fall into a single "Default
    // Sequence", so a file that carries no tags imports exactly as before.
    // Sequences are created in first-seen order. The nested shape names its
    // sequences from the stacks themselves and does not come through here.
    std::vector<Sequence> seqs;
    std::map<std::string, size_t> seqIndexByName; // sequence_name -> index in seqs
    int nextSeqIdCounter = 1;
    // Every sequence records the project it came from, so a later graft into
    // another timeline keeps its grouping. The record holds the file's path, so two
    // projects whose stems match stay distinct. Whether it counts as opened whole is
    // the caller's to say — this reads the file either way, but a graft brings only
    // one sequence of it in.
    const int projectId = tl.projectIdForPath(path, fs::path(path).stem().string());
    auto seqIndexForName = [&](const std::string& name) -> size_t {
        std::string key = name.empty() ? "Default Sequence" : name;
        auto it = seqIndexByName.find(key);
        if (it != seqIndexByName.end())
            return it->second;
        Sequence s;
        s.id   = nextSeqIdCounter++;
        s.name = key;
        s.projectId = projectId;
        seqIndexByName[key] = seqs.size();
        seqs.push_back(std::move(s));
        return seqs.size() - 1;
    };
    std::map<int, size_t> seqOfShot; // shot id -> owning sequence index

    // Two file shapes are read, and both stay supported:
    //
    //   nested — one root video track (the "reels" track) whose children are
    //     Stacks, one per sequence, each holding that sequence's own track stack
    //     (track 0 = shots, 1..N = media layers). What create_otio_project and
    //     createproject write: the grouping is explicit, so sequences can neither
    //     interleave nor be ordered by anything but the file's own order.
    //   flat — video tracks straight off the root stack, clips laid end-to-end and
    //     grouped by their "sequence_name" metadata (see seqIndexForName above).
    //     What shotdetect and third-party files carry.
    //
    // A composition nested deeper than that is a pre-comp, which jplay's model has
    // no place for; it is flattened into the track holding it (see walkTrack).
    auto collectVideoTracks = [](otio::Composition* comp) {
        std::vector<otio::Track*> tracks;
        for (const auto& trackR : comp->children()) {
            auto* track = dynamic_cast<otio::Track*>(trackR.value);
            if (track && track->kind() == otio::Track::Kind::video)
                tracks.push_back(track);
        }
        return tracks;
    };

    otio::Stack* stack = timeline->tracks();
    std::vector<otio::Track*> videoTracks = collectVideoTracks(stack);

    if (videoTracks.empty()) {
        err = "no video tracks found in OTIO timeline";
        return false;
    }

    // The reels track, when the file is in the nested shape: a lone video track
    // holding nothing but Stacks and the gaps between them.
    otio::Track* reels = nullptr;
    if (videoTracks.size() == 1) {
        bool anyStack = false, onlyStacks = true;
        for (const auto& childR : videoTracks[0]->children()) {
            if (dynamic_cast<otio::Stack*>(childR.value))
                anyStack = true;
            else if (!dynamic_cast<otio::Gap*>(childR.value))
                onlyStacks = false;
        }
        if (anyStack && onlyStacks)
            reels = videoTracks[0];
    }

    // Helper: find or create a Media entry in the pool.
    auto ensureMedia = [&](ClipType type, const std::string& filePath) -> std::shared_ptr<Media> {
        auto existing = tl.findMediaByPath(type, filePath);
        if (existing)
            return existing;
        auto m = std::make_shared<Media>(type, filePath);
        tl.media[m->id()] = m;
        return m;
    };

    // A clip the writer verified on disk carries everything the post-load metadata
    // probe would have opened the source to learn: available_range is the media's
    // real extent, and media_width/media_height its real dimensions. Seeding the
    // Media from that and marking it trusted lets the probe skip it, so an import of
    // a fully verified .otio touches no media file at all. Unverified clips — an
    // older .otio, or media that wasn't reachable when the file was written — are
    // left alone and probed exactly as before.
    //
    // fps stays 0 on purpose: an image sequence has no rate of its own (so does
    // ExrSequenceSource::fps()), and its consumers fall back to the project's.
    // An unverified clip gets no metadata and keeps its frame-pattern path; all it
    // can contribute is the naming hint (see Media::namingPathHint), which spares the
    // post-load tagging pass an open per media without putting a guessed frame number
    // anywhere that reads pixels.
    auto seedFromClip = [](const std::shared_ptr<Media>& media, const otio::Clip* clip,
                           int64_t availFrames, int64_t firstFrame) {
        if (!mediaVerifiedOf(clip)) {
            if (firstFrame > 0 && media->namingPathHint().empty() &&
                media->type() == ClipType::ImageSequence)
                media->setNamingPathHint(ImageSeq::substituteFrame(media->path(), firstFrame));
            return;
        }
        MediaInfo mi;
        mi.width      = (int32_t)intMetaOf(clip, "media_width");
        mi.height     = (int32_t)intMetaOf(clip, "media_height");
        mi.frameCount = availFrames;
        media->setInfo(mi);
        media->setInfoTrusted();
    };

    int shotId = 1;
    int clipId = 1;
    int transId = 1;

    // ---- a stack's track 0 → shots (and media clips when the clip carries one) --
    // shotdetect produces a single track where each clip is simultaneously a
    // shot definition AND an ExternalReference to the shot's media file.
    // We always create a Shot; if the clip also has a media reference we create
    // a Clip snapped directly into that shot so it is immediately playable.
    //
    // `base` is the absolute frame the track's own frame 0 maps to — 0 in the flat
    // shape, the sequence's start in the nested one — and `fixedSeq` the sequence
    // every item belongs to, or -1 to resolve it per clip from the metadata tag.
    // Returns the absolute frame the track ended on.
    auto importShotTrack = [&](otio::Track* shotTrack, int fixedSeq, int64_t base) -> int64_t {
        int64_t cursor = base;
        for (const auto& itemR : shotTrack->children()) {
            otio::Composable* item = itemR.value;

            if (auto* gap = dynamic_cast<otio::Gap*>(item)) {
                cursor += std::max<int64_t>(toFrames(gap->duration(&es), fps), 0);
                continue;
            }

            auto* clip = dynamic_cast<otio::Clip*>(item);
            if (!clip) {
                // Anything else on the shot track (a nested composition, say) defines
                // no shot, but its span still has to be spent: dropping it outright
                // would pull everything behind it forward.
                if (auto* other = dynamic_cast<otio::Item*>(item))
                    cursor += std::max<int64_t>(toFrames(other->duration(&es), fps), 0);
                continue;
            }

            const otio::TimeRange tr = clip->trimmed_range(&es);
            const int64_t duration = toFrames(tr.duration(), fps);
            if (duration <= 0) {
                cursor += std::max<int64_t>(duration, 0);
                continue;
            }

            const size_t si = fixedSeq >= 0 ? (size_t)fixedSeq
                                            : seqIndexForName(sequenceNameOf(clip));

            Shot s;
            s.id   = shotId++;
            s.name = shotNameOf(clip);
            s.sceneName = sceneNameOf(clip);
            s.timelineStart = cursor;
            s.duration = duration;

            auto* ext = dynamic_cast<otio::ExternalReference*>(clip->media_reference());

            // The media's first frame comes from the media reference's
            // available_range.start_time (e.g. 1001), which OTIO already parsed
            // from the file — no disk access. source_range gives the cut in/out as
            // ABSOLUTE editorial frame numbers (e.g. 1049-1139), so jplay's 0-based
            // sourceOffset is the cut start minus the media's first frame
            // (1049 - 1001 = 48). Without this rebase the offset runs off the end
            // of the file list and every frame clamps to the last file — the clip
            // freezes on one frame. (createProjectFromDirectory does the same
            // rebase using the opened media's firstFrameNumber.)
            int64_t firstFrame = 0;
            int64_t availFrames = 0;
            if (ext) {
                if (auto ar = ext->available_range()) {
                    firstFrame  = toFrames(ar->start_time(), fps);
                    availFrames = toFrames(ar->duration(), fps);
                }
            }

            int64_t srcOffset = 0;
            int64_t clipDur   = duration;
            if (auto sr = clip->source_range()) {
                const int64_t in  = toFrames(sr->start_time(), fps);
                const int64_t dur = toFrames(sr->duration(), fps);
                srcOffset = std::max<int64_t>(in - firstFrame, 0);
                clipDur   = std::max<int64_t>(dur, 1);
                s.initCut(srcOffset, srcOffset + clipDur);
            }

            // If the shot clip also carries a media reference, create a Clip for
            // it and snap it straight into the shot (single-track shotdetect format).
            if (ext) {
                const std::string filePath = mediaPath(ext->target_url(), baseDir, firstFrame,
                                                       mediaVerifiedOf(clip));
                const ClipType type = ImageSeq::isSequencePath(filePath) ? ClipType::ImageSequence : ClipType::Video;
                auto media = ensureMedia(type, filePath);
                seedFromClip(media, clip, availFrames, firstFrame);

                Clip c;
                c.id            = clipId++;
                c.mediaId       = media->id();
                c.track         = 0;
                c.timelineStart = cursor;
                c.duration      = clipDur;
                c.sourceOffset  = srcOffset;
                c.shotId        = s.id;
                seqs[si].clips.push_back(std::move(c));
            }

            seqs[si].shotIds.push_back(s.id);
            seqOfShot[s.id] = si;
            tl.shots.push_back(std::move(s));
            cursor += duration;
        }
        return cursor;
    };

    // ---- a stack's remaining tracks → media clips -------------------------
    // What one walk of a composition's items onto a single jplay track needs to
    // know. The window bounds only ever narrow, and only for a flattened pre-comp:
    // a top-level track is unbounded.
    struct Walk {
        int trackIndex = 0;             // jplay track the items land on
        int fixedSeq = -1;              // sequence they belong to; -1 = per-clip metadata
        int64_t base = 0;               // absolute frame the composition's frame 0 maps to
        int64_t winStart = INT64_MIN;   // absolute trim window; items wholly outside are dropped
        int64_t winEnd = INT64_MAX;
        size_t shotFrom = 0;            // first tl.shots entry shot matching may consider
        int depth = 8;                  // nesting levels left to flatten
    };
    // Returns the absolute frame the walk ended on. A nested composition is
    // flattened into the same jplay track: a pre-comp has no representation in
    // jplay's model, so it imports as the cut it renders to — for a Stack that is
    // its topmost video track, which is the layer jplay itself would show.
    std::function<int64_t(otio::Composition*, Walk)> walkTrack;
    walkTrack = [&](otio::Composition* comp, Walk w) -> int64_t {
        int64_t cursor = w.base;
        // OTIO models a transition exactly as jplay does: a zero-duration item
        // between two clips carrying in_offset / out_offset around the cut. Only the
        // incoming clip's id closes the pair, so a transition passed here is held
        // until the next clip lands. Anything that breaks the abutment in between —
        // a gap, an unusable clip, a nested composition — discards it.
        int prevClipId = -1;
        size_t prevSeqIdx = 0;
        bool pendingTrans = false;
        int64_t pendingIn = 0, pendingOut = 0;

        for (const auto& itemR : comp->children()) {
            otio::Composable* item = itemR.value;

            if (auto* gap = dynamic_cast<otio::Gap*>(item)) {
                cursor += std::max<int64_t>(toFrames(gap->duration(&es), fps), 0);
                prevClipId = -1;
                pendingTrans = false;
                continue;
            }

            if (auto* trans = dynamic_cast<otio::Transition*>(item)) {
                pendingTrans = true;
                pendingIn  = std::max<int64_t>(toFrames(trans->in_offset(), fps), 0);
                pendingOut = std::max<int64_t>(toFrames(trans->out_offset(), fps), 0);
                continue; // zero duration: the cursor does not move
            }

            if (auto* inner = dynamic_cast<otio::Composition*>(item)) {
                // A pre-comp: flatten its top layer onto this same track, then spend
                // its span whether or not anything came out of it.
                const int64_t dur = std::max<int64_t>(toFrames(inner->duration(&es), fps), 0);
                otio::Composition* layer = inner;
                if (dynamic_cast<otio::Stack*>(inner)) {
                    auto tracks = collectVideoTracks(inner);
                    layer = tracks.empty() ? nullptr : tracks.front();
                }
                if (layer && w.depth > 0) {
                    Walk sub = w;
                    sub.depth = w.depth - 1;
                    // Its source_range is a trim: frame 0 sits that far behind the
                    // cursor, and only what the trim keeps belongs on the timeline.
                    int64_t trimIn = 0;
                    if (auto sr = inner->source_range())
                        trimIn = toFrames(sr->start_time(), fps);
                    sub.base     = cursor - trimIn;
                    sub.winStart = std::max(w.winStart, cursor);
                    sub.winEnd   = std::min(w.winEnd, cursor + dur);
                    walkTrack(layer, sub);
                }
                cursor += dur;
                prevClipId = -1;
                pendingTrans = false;
                continue;
            }

            auto* clip = dynamic_cast<otio::Clip*>(item);
            if (!clip) {
                if (auto* other = dynamic_cast<otio::Item*>(item))
                    cursor += std::max<int64_t>(toFrames(other->duration(&es), fps), 0);
                prevClipId = -1;
                pendingTrans = false;
                continue;
            }

            const otio::TimeRange tr = clip->trimmed_range(&es);
            const int64_t duration = toFrames(tr.duration(), fps);
            const int64_t offset   = toFrames(tr.start_time(), fps);

            auto* ext = dynamic_cast<otio::ExternalReference*>(clip->media_reference());
            if (!ext || duration <= 0 ||
                cursor + duration <= w.winStart || cursor >= w.winEnd) {
                cursor += std::max<int64_t>(duration, 0);
                prevClipId = -1;
                pendingTrans = false;
                continue;
            }

            // Same first-frame substitution as the shot track: a file referenced from
            // both tracks must land on one Media, so both passes must name it the same
            // way.
            int64_t firstFrame = 0;
            int64_t availFrames = 0;
            if (auto ar = ext->available_range()) {
                firstFrame  = toFrames(ar->start_time(), fps);
                availFrames = toFrames(ar->duration(), fps);
            }
            const std::string filePath = mediaPath(ext->target_url(), baseDir, firstFrame,
                                                   mediaVerifiedOf(clip));
            const ClipType type = ImageSeq::isSequencePath(filePath) ? ClipType::ImageSequence : ClipType::Video;
            auto media = ensureMedia(type, filePath);
            seedFromClip(media, clip, availFrames, firstFrame);

            Clip c;
            c.id            = clipId++;
            c.mediaId       = media->id();
            c.track         = w.trackIndex;
            c.timelineStart = cursor;
            c.duration      = duration;
            c.sourceOffset  = offset;

            // Match clip to a shot: name first, then largest overlap. In the nested
            // shape only this sequence's shots are candidates (shotFrom), since shot
            // names repeat across sequences and a clip must never adopt a foreign
            // one. The clip joins the sequence owning its matched shot; if unmatched
            // it falls back to its own sequence_name tag (or the default sequence).
            const std::string clipName = shotNameOf(clip);
            Shot* matched = matchShot(tl.shots, w.shotFrom, clipName, cursor, cursor + duration);
            size_t si;
            if (matched) {
                c.shotId = matched->id;
                // Snap position and duration to the shot's cut range.
                c.timelineStart = matched->timelineStart;
                if (matched->cutOut > matched->cutIn) {
                    c.sourceOffset = matched->cutIn;
                    c.duration     = matched->cutOut - matched->cutIn;
                } else {
                    c.duration = matched->duration;
                }
            }
            if (w.fixedSeq >= 0) {
                si = (size_t)w.fixedSeq;
            } else if (matched) {
                auto so = seqOfShot.find(matched->id);
                si = so != seqOfShot.end() ? so->second : seqIndexForName(sequenceNameOf(clip));
            } else {
                si = seqIndexForName(sequenceNameOf(clip));
            }

            const int placedId = c.id;
            seqs[si].clips.push_back(std::move(c));
            // Close a held transition, but only when both clips ended up in the same
            // sequence — the shot-matching above can split neighbours apart, and a
            // transition never spans sequences. One that no longer abuts (matching
            // snapped a clip to its shot) or whose offsets outrun the handles is
            // dropped or clamped by App::adoptLoadedTransitions after the import.
            if (pendingTrans && prevClipId >= 0 && prevSeqIdx == si &&
                (pendingIn > 0 || pendingOut > 0)) {
                Transition t;
                t.id = transId++;
                t.aClipId = prevClipId;
                t.bClipId = placedId;
                t.inFrames = pendingIn;
                t.outFrames = pendingOut;
                seqs[si].transitions.push_back(t);
            }
            pendingTrans = false;
            prevClipId = placedId;
            prevSeqIdx = si;
            cursor += duration;
        }
        return cursor;
    };

    // ---- drive those two passes over whichever shape the file is in -------
    int maxTrackSeen = 0; // highest track index the file offers, empty ones included
    if (reels) {
        int64_t base = 0;
        for (const auto& childR : reels->children()) {
            if (auto* gap = dynamic_cast<otio::Gap*>(childR.value)) {
                base += std::max<int64_t>(toFrames(gap->duration(&es), fps), 0);
                continue;
            }
            auto* seqStack = dynamic_cast<otio::Stack*>(childR.value);
            if (!seqStack)
                continue;

            // One sequence per Stack, always — two stacks sharing a name are still
            // two sequences, so this deliberately bypasses seqIndexForName.
            Sequence s;
            s.id   = nextSeqIdCounter++;
            s.name = !seqStack->name().empty()
                         ? seqStack->name()
                         : "Sequence " + std::to_string(seqs.size() + 1);
            s.projectId = projectId;
            const size_t si = seqs.size();
            seqs.push_back(std::move(s));

            const size_t shotFrom = tl.shots.size(); // this sequence's shots start here
            std::vector<otio::Track*> tracks = collectVideoTracks(seqStack);
            maxTrackSeen = std::max(maxTrackSeen, (int)tracks.size() - 1);

            int64_t end = base;
            if (!tracks.empty())
                end = std::max(end, importShotTrack(tracks[0], (int)si, base));
            for (size_t ti = 1; ti < tracks.size(); ++ti) {
                Walk w;
                w.trackIndex = (int)ti;
                w.fixedSeq   = (int)si;
                w.base       = base;
                w.shotFrom   = shotFrom;
                end = std::max(end, walkTrack(tracks[ti], w));
            }
            // Sequences are laid end-to-end and never overlap, so the next one starts
            // where this one ends: its stated extent when it has one, else whatever
            // its tracks used. (jplay repacks them on load anyway, but the positions
            // have to stay disjoint here for shot matching to work.)
            const int64_t dur = std::max<int64_t>(toFrames(seqStack->duration(&es), fps), 0);
            base += std::max(dur, end - base);
        }
    } else {
        maxTrackSeen = (int)videoTracks.size() - 1;
        importShotTrack(videoTracks[0], -1, 0);
        // Track index starts at 1; track 0 is reserved for the shot-track clips
        // generated above. clipId continues from where that pass left off.
        for (size_t ti = 1; ti < videoTracks.size(); ++ti) {
            Walk w;
            w.trackIndex = (int)ti;
            walkTrack(videoTracks[ti], w);
        }
    }

    bool anyClips = false;
    for (const auto& s : seqs)
        if (!s.clips.empty()) { anyClips = true; break; }
    if (!anyClips && tl.shots.empty()) {
        err = "no video clips or shots found in OTIO timeline";
        return false;
    }

    // A timeline must own at least one sequence; guarantee the default when the
    // file produced no clips and no shots on any track.
    if (seqs.empty())
        seqIndexForName("");

    // Tracks the file offered but left empty still count, so the layering of the
    // ones that do hold clips survives. A single-track file (shotdetect output, or
    // a nested stack holding only a shot track) leaves maxTrackSeen at 0, which is
    // correct — those clips sit on track 0.
    int maxTrackUsed = maxTrackSeen;
    for (const auto& s : seqs)
        for (const auto& c : s.clips)
            maxTrackUsed = std::max(maxTrackUsed, c.track);
    tl.trackCount = std::max(maxTrackUsed + 1, 1);

    for (auto& s : seqs)
        tl.sequences.push_back(std::move(s));
    out = std::move(tl);
    nextClipId = clipId;
    nextSeqId  = nextSeqIdCounter; // ids 1..nextSeqIdCounter-1 were used
    nextShotId = shotId;
    return true;
}

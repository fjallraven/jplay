#include "OtioExport.h"

#include "ImageSeq.h"

#include <opentimelineio/clip.h>
#include <opentimelineio/externalReference.h>
#include <opentimelineio/gap.h>
#include <opentimelineio/serializableObject.h>
#include <opentimelineio/stack.h>
#include <opentimelineio/timeline.h>
#include <opentimelineio/track.h>
#include <opentimelineio/transition.h>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <optional>

namespace fs = std::filesystem;
namespace otio = OTIO_NS;

namespace {

std::string plural(int n, const char* one, const char* many) {
    return std::to_string(n) + " " + (n == 1 ? one : many);
}

// The media's nominal first frame number (e.g. 1001) and resolved on-disk path,
// deliberately independent of Media::ensureOpen()/resolvedPath(): with the
// global Proxy mode active those may be decoding a substituted file (see
// ProxyMode.h), and an exported .otio leaves the app for other tools/vendors —
// it must always name the real, full-res file and its real first frame, never
// a proxy's. A throwaway ImageSeq::open bypasses Media entirely rather than
// disturbing its cached decoder. Video/audio need no scratch open: path() is
// already a concrete file for them. Video sources are 0-based.
struct NominalRef {
    int64_t firstFrame = 0;
    std::string path;
};

NominalRef nominalRefOf(Media& m) {
    NominalRef out;
    out.path = m.path();
    if (m.type() == ClipType::ImageSequence) {
        std::string err;
        if (auto src = ImageSeq::open(m.path(), err)) {
            out.firstFrame = src->firstFrameNumber();
            out.path = src->path();
        }
    }
    return out;
}

} // namespace

std::vector<std::string> OtioExport::survey(const Timeline& tl) {
    std::vector<std::string> out;

    int annotClips = 0, annotFrames = 0, annotStrokes = 0;
    int audioClips = 0, hiddenClips = 0, lowerTrackClips = 0;
    tl.forEachClip([&](const Clip& c) {
        if (!c.annotations.empty()) {
            ++annotClips;
            for (const auto& [frame, strokes] : c.annotations) {
                (void)frame;
                ++annotFrames;
                annotStrokes += (int)strokes.size();
            }
        }
        if (c.audio) ++audioClips;
        else if (c.track > 0) ++lowerTrackClips;
        if (c.hidden) ++hiddenClips;
    });

    if (annotClips > 0)
        out.push_back("Annotations: " + plural(annotStrokes, "stroke", "strokes") +
                      " on " + plural(annotFrames, "frame", "frames") +
                      " of " + plural(annotClips, "clip", "clips"));
    if (audioClips > 0)
        out.push_back("Audio: " + plural(audioClips, "clip", "clips") +
                      " (only video tracks are written)");
    if (hiddenClips > 0)
        out.push_back("Disabled clips: " + plural(hiddenClips, "clip", "clips") +
                      " will come back visible");
    // The one loss that moves picture: reading the file back into jplay snaps every
    // clip below the top track onto the shot it matches (see OtioImport), taking
    // its position, length and source offset from that shot.
    if (lowerTrackClips > 0)
        out.push_back("Track layering: " + plural(lowerTrackClips, "clip", "clips") +
                      " below the top track will move if the .otio is reopened in jplay");

    if (tl.letterboxRatio > 0.0) {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "Letterbox: %.4g matte", tl.letterboxRatio);
        out.push_back(buf);
    }
    if (tl.playhead != 0 || tl.inPoint != 0 || tl.outPoint >= 0)
        out.push_back("Playback state: playhead and in/out points");
    // OTIO carries no colour-management mode, and an import defaults to OCIO on,
    // so only the built-in transform is actually lost.
    if (!tl.ocioEnabled)
        out.push_back("Colour management: set to the built-in transform, reloads as OCIO");

    int cutDiffer = 0;
    for (const Shot& s : tl.shots)
        if (s.cutDiffer) ++cutDiffer;
    if (cutDiffer > 0)
        out.push_back("Cut changes: " + plural(cutDiffer, "shot", "shots") +
                      " marked as differing from the original cut");

    // Only media a clip actually references gets written, matching Project::save.
    int taggedMedia = 0;
    for (const auto& kv : tl.media) {
        if (!kv.second || (kv.second->name().empty() && kv.second->meta().empty()))
            continue;
        bool used = false;
        tl.forEachClip([&](const Clip& c) { if (c.mediaId == kv.first) used = true; });
        if (used) ++taggedMedia;
    }
    if (taggedMedia > 0)
        out.push_back("Media names and metadata: " + plural(taggedMedia, "item", "items") +
                      " (re-derived from the path on reload)");

    return out;
}

bool OtioExport::save(const std::string& path, const Timeline& tl, std::string& err) {
    const double rate = tl.fps > 0 ? tl.fps : 24.0;

    // Nested shape, mirroring what OtioImport reads back: the root stack holds one
    // video track (the reels), whose children are one Stack per jplay sequence,
    // each holding that sequence's own tracks. Positions inside a Stack are
    // relative to that sequence's own leading edge, so the reels track lays the
    // sequences end-to-end exactly as jplay concatenates them.
    // The source view is a scratch sequence, not part of the project, so it is
    // skipped here and in the reel loop below (see Sequence::temporary).
    int maxTrackAny = -1;
    for (const Sequence& s : tl.sequences) {
        if (s.temporary)
            continue;
        for (const Clip& c : s.clips)
            if (!c.audio && c.track >= 0)
                maxTrackAny = std::max(maxTrackAny, c.track);
    }
    if (maxTrackAny < 0) {
        err = "no video clips to export";
        return false;
    }

    otio::SerializableObject::Retainer<otio::Timeline> timeline(
        new otio::Timeline(fs::path(path).stem().string()));
    auto* reels = new otio::Track("Sequences", std::nullopt, otio::Track::Kind::video);

    for (const Sequence& s : tl.sequences) {
        if (s.temporary)
            continue;
        // The sequence's own frame 0: its first clip. A sequence has no leading gap
        // by definition (its extent starts at its earliest clip), so every clip
        // rebases to a non-negative position here.
        int64_t seqStart = 0, seqEnd = 0;
        const bool hasSpan = tl.sequenceSpan(s, seqStart, seqEnd);

        int maxTrack = -1;
        for (const Clip& c : s.clips)
            if (!c.audio && c.track >= 0)
                maxTrack = std::max(maxTrack, c.track);

        auto* seqStack = new otio::Stack(s.name);
        int64_t seqDur = 0;

        // One OTIO track per jplay track index this sequence uses, top first —
        // including indices that hold only audio, which export as empty video tracks
        // so the layering of the remaining video tracks is preserved.
        for (int t = 0; t <= maxTrack; ++t) {
            std::vector<const Clip*> entries;
            for (const Clip& c : s.clips)
                if (!c.audio && c.track == t)
                    entries.push_back(&c);
            std::sort(entries.begin(), entries.end(), [](const Clip* a, const Clip* b) {
                return a->timelineStart < b->timelineStart;
            });

            auto* track = new otio::Track("Track " + std::to_string(t + 1),
                                          std::nullopt, otio::Track::Kind::video);
            int64_t cursor = 0;
            for (const Clip* c : entries) {
                const int64_t start = c->timelineStart - seqStart;
                if (start > cursor) {
                    const int64_t gapLen = start - cursor;
                    track->append_child(new otio::Gap(
                        opentime::RationalTime((double)gapLen, rate)));
                    cursor += gapLen;
                }

                auto m = tl.findMediaById(c->mediaId);
                const NominalRef nom = m ? nominalRefOf(*m) : NominalRef{};
                const int64_t firstFrame = nom.firstFrame;
                // Normalize to forward slashes so the target_url is a clean, portable
                // path for any OTIO consumer (mixed separators are legal but ugly on
                // Windows).
                const std::string url = fs::path(nom.path).generic_string();

                // available_range: the media's on-disk extent from its first frame
                // number; source_range: the cut as absolute editorial frames. Media
                // whose length was never probed fall back to the clip's own extent.
                const int64_t avail = m && m->info().frameCount > 0
                                          ? m->info().frameCount
                                          : c->sourceOffset + c->duration;
                auto* ref = new otio::ExternalReference(
                    url,
                    otio::TimeRange(opentime::RationalTime((double)firstFrame, rate),
                                    opentime::RationalTime((double)std::max<int64_t>(avail, 1), rate)));
                otio::TimeRange src(
                    opentime::RationalTime((double)(firstFrame + c->sourceOffset), rate),
                    opentime::RationalTime((double)std::max<int64_t>(c->duration, 1), rate));

                const Shot* shot = c->shotId >= 0 ? tl.findShotById(c->shotId) : nullptr;
                std::string name = shot && !shot->name.empty() ? shot->name
                                 : m && !m->name().empty()     ? m->name()
                                                               : fs::path(url).stem().string();
                auto* clip = new otio::Clip(name, ref, src);
                if (shot && !shot->name.empty())
                    clip->metadata()["shot_name"] = shot->name;
                if (shot && !shot->sceneName.empty())
                    clip->metadata()["scene_name"] = shot->sceneName;
                // The stack's name already says which sequence this is; the tag is
                // written anyway, both for readers that only know the flat shape and
                // because a single grafted sequence keeps its name through it.
                if (!s.name.empty())
                    clip->metadata()["sequence_name"] = s.name;
                // A dissolve landing on this clip's in point goes in ahead of it, which
                // is how OTIO carries one: a zero-duration item between the two clips.
                // Reading its offsets back yields the same transition (see OtioImport).
                for (const Transition& tr : s.transitions) {
                    Timeline::TransitionSpan sp;
                    if (tr.bClipId != c->id || !tl.resolveTransition(s, tr, sp))
                        continue;
                    track->append_child(new otio::Transition(
                        "Cross Dissolve", otio::Transition::Type::SMPTE_Dissolve,
                        opentime::RationalTime((double)tr.inFrames, rate),
                        opentime::RationalTime((double)tr.outFrames, rate)));
                    break;
                }
                track->append_child(clip);
                cursor += c->duration;
            }
            seqStack->append_child(track);
            seqDur = std::max(seqDur, cursor);
        }

        // Always the sequence's full extent, never a trim: a jplay sequence is a
        // reel, not a pre-comp, and the nesting is only there to group it. An empty
        // sequence gets no range at all rather than a zero-length one.
        if (hasSpan && seqDur > 0)
            seqStack->set_source_range(
                otio::TimeRange(opentime::RationalTime(0.0, rate),
                                opentime::RationalTime((double)seqDur, rate)));
        reels->append_child(seqStack);
    }

    timeline->tracks()->append_child(reels);

    otio::ErrorStatus es;
    if (!timeline->to_json_file(path, &es)) {
        err = otio::is_error(es) ? es.full_description : "OTIO serialization failed";
        return false;
    }
    return true;
}

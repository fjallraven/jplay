#include "Project.h"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>

namespace {

const char kMagic[4] = { 'J', 'P', 'L', 'Y' };
const uint32_t kVersion = 35;

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
    if (!s.empty())
        os.write(s.data(), (std::streamsize)s.size());
}

bool readStr(std::istream& is, std::string& s) {
    uint32_t n = 0;
    if (!readRaw(is, n) || n > (1u << 20))
        return false;
    s.resize(n);
    if (n)
        is.read(s.data(), (std::streamsize)n);
    return is.good() || (n == 0 && !is.bad());
}

} // namespace

namespace Project {

// Serialize the timeline into an already-open binary stream. Shared by the
// file (save) and buffer (saveBuffer) entry points.
static bool saveTo(std::ostream& os, const Timeline& tl, const std::string& projectId,
                   std::string& err, const ViewState& view) {
    os.write(kMagic, 4);
    writeRaw(os, kVersion);
    writeRaw(os, tl.fps);
    writeRaw(os, tl.playhead);
    writeRaw(os, tl.inPoint);
    writeRaw(os, tl.outPoint);
    writeRaw(os, (int32_t)tl.trackCount);

    // The sequences that actually get written. A temporary sequence (the source
    // view — see Sequence::temporary) lives only in memory, so it is skipped
    // everywhere below: its clips don't hold media in the pool, its shots get no
    // storage offset, and it is absent from the sequence table. That also keeps it
    // out of the dirty signature, which is this same byte stream hashed.
    std::vector<size_t> saved;
    for (size_t i = 0; i < tl.sequences.size(); ++i)
        if (!tl.sequences[i].temporary)
            saved.push_back(i);

    // Media pool (only entries still referenced by at least one clip are saved).
    // Collect referenced ids first.
    std::vector<std::shared_ptr<Media>> pool;
    for (const auto& kv : tl.media) {
        bool used = false;
        for (size_t i : saved)
            for (const Clip& c : tl.sequences[i].clips)
                if (c.mediaId == kv.first) used = true;
        if (used)
            pool.push_back(kv.second);
    }
    writeRaw(os, (uint32_t)pool.size());
    for (const auto& m : pool)
        m->serialize(os);

    // Per-sequence storage offsets: positions are written relative to each
    // sequence's region start, with the first non-empty sequence's leading
    // position kept (subtract regions[i].start - leadStart). repackSequences()
    // on load restores the absolute concatenated layout.
    auto regions = tl.seqRegions();
    int64_t leadStart = 0;
    for (size_t i : saved)
        if (!tl.sequences[i].clips.empty()) { leadStart = regions[i].start; break; }
    auto seqStoreOffset = [&](size_t i) -> int64_t {
        return (i < regions.size() ? regions[i].start : 0) - leadStart;
    };
    // shotId -> storage offset of the sequence that owns it (0 if unowned).
    std::map<int, int64_t> shotStoreOffset;
    for (size_t i : saved)
        for (int sid : tl.sequences[i].shotIds)
            shotStoreOffset.emplace(sid, seqStoreOffset(i));

    // Shot table (global pool).
    writeRaw(os, (uint32_t)tl.shots.size());
    for (const auto& s : tl.shots) {
        int64_t off = 0;
        if (auto it = shotStoreOffset.find(s.id); it != shotStoreOffset.end())
            off = it->second;
        writeRaw(os, (int32_t)s.id);
        writeRaw(os, s.timelineStart - off);
        writeRaw(os, s.duration);
        writeRaw(os, s.cutIn);
        writeRaw(os, s.cutOut);
        writeRaw(os, s.originalCutIn);
        writeRaw(os, s.originalCutOut);
        writeRaw(os, (uint8_t)(s.cutDiffer ? 1 : 0));
        writeStr(os, s.name);
        writeStr(os, s.sceneName);
    }

    // Sequence table.
    writeRaw(os, (uint32_t)saved.size());
    for (size_t i : saved) {
        const auto& s = tl.sequences[i];
        const int64_t off = seqStoreOffset(i);
        writeRaw(os, (int32_t)s.id);
        writeStr(os, s.name);
        writeRaw(os, (int32_t)s.projectId); // v29: was the project's name in v22..28
        writeRaw(os, (uint32_t)s.bgColor);
        writeRaw(os, (uint32_t)s.shotIds.size());
        for (int sid : s.shotIds)
            writeRaw(os, (int32_t)sid);
        writeRaw(os, (uint32_t)s.clips.size());
        for (const auto& c : s.clips) {
            writeRaw(os, (int32_t)c.id);
            writeStr(os, c.mediaId);
            writeRaw(os, (int32_t)c.track);
            writeRaw(os, c.timelineStart - off);
            writeRaw(os, c.duration);
            writeRaw(os, c.sourceOffset);
            writeRaw(os, (int32_t)c.shotId);
            writeRaw(os, (uint8_t)(c.hidden ? 1 : 0));
            writeRaw(os, (uint8_t)(c.audio ? 1 : 0)); // v18: audio-track clip
            writeRaw(os, c.fadeInFrames);             // v25: opacity ramps
            writeRaw(os, c.fadeOutFrames);
            writeRaw(os, (int32_t)c.linkedTo);        // v26: audio follows video
            writeRaw(os, c.linkOffset);
            // Pencil annotations, keyed by source frame.
            writeRaw(os, (uint32_t)c.annotations.size());
            for (const auto& [srcFrame, strokes] : c.annotations) {
                writeRaw(os, srcFrame);
                writeRaw(os, (uint32_t)strokes.size());
                for (const auto& s : strokes) {
                    writeRaw(os, s.r);
                    writeRaw(os, s.g);
                    writeRaw(os, s.b);
                    writeRaw(os, (uint32_t)s.pts.size());
                    for (const auto& p : s.pts) {
                        writeRaw(os, p.x);
                        writeRaw(os, p.y);
                        writeRaw(os, p.hw);
                    }
                }
            }
            // Volume automation (v34+). Written last in the clip record so the
            // reader's annotation path is untouched.
            writeRaw(os, c.volume.base);
            writeRaw(os, (uint8_t)c.volume.interp);
            writeRaw(os, (uint32_t)c.volume.pts.size());
            for (const auto& p : c.volume.pts) {
                writeRaw(os, p.t);
                writeRaw(os, p.v);
            }
        }
        // Dissolves on this sequence's cuts (v24+). Positions are implied by the
        // clip pair, so nothing here needs the storage offset.
        writeRaw(os, (uint32_t)s.transitions.size());
        for (const auto& t : s.transitions) {
            writeRaw(os, (int32_t)t.id);
            writeRaw(os, (int32_t)t.aClipId);
            writeRaw(os, (int32_t)t.bClipId);
            writeRaw(os, t.inFrames);
            writeRaw(os, t.outFrames);
        }
    }

    writeStr(os, projectId);
    writeRaw(os, (int32_t)view.seqIdx);
    writeRaw(os, tl.letterboxRatio);   // v17+
    writeRaw(os, tl.letterboxOpacity); // v17+
    writeRaw(os, (uint8_t)(tl.ocioEnabled ? 1 : 0)); // v20+
    writeRaw(os, (int32_t)view.projId);              // v29+
    // The project table Sequence::projectId indexes. Written last so a reader that
    // stops early still gets a usable timeline, as every trailer field before it.
    writeRaw(os, (uint32_t)tl.projects.size());
    for (const SourceProject& p : tl.projects) {
        writeRaw(os, (int32_t)p.id);
        writeStr(os, p.name);
        writeStr(os, p.path);
        writeRaw(os, (uint8_t)(p.openedWhole ? 1 : 0));
    }
    // Custom track labels (v30+), one per track row; empty = the derived name.
    {
        uint32_t nameCount =
            (uint32_t)std::min(tl.trackNames.size(), (size_t)std::max(tl.trackCount, 0));
        writeRaw(os, nameCount);
        for (uint32_t i = 0; i < nameCount; ++i)
            writeStr(os, tl.trackNames[i]);
    }
    // Disabled track rows (v31+), one flag per track row.
    {
        uint32_t offCount =
            (uint32_t)std::min(tl.disabledTracks.size(), (size_t)std::max(tl.trackCount, 0));
        writeRaw(os, offCount);
        for (uint32_t i = 0; i < offCount; ++i)
            writeRaw(os, tl.disabledTracks[i]);
    }
    writeStr(os, tl.ocioView); // v33+
    writeStr(os, tl.proxyMode); // v35+

    os.flush();
    if (!os) {
        err = "write failed";
        return false;
    }
    return true;
}

// Deserialize a timeline from an already-open binary stream. Shared by the file
// (load) and buffer (loadBuffer) entry points.
static bool loadFrom(std::istream& is, Timeline& tl, int& nextClipId, int& nextSeqId,
                     int& nextShotId, std::string& projectId, std::string& err,
                     ViewState* view) {
    projectId.clear();
    if (view)
        *view = ViewState{};

    char magic[4] = {};
    is.read(magic, 4);
    if (!is || std::memcmp(magic, kMagic, 4) != 0) {
        err = "not a JPLY project file";
        return false;
    }
    uint32_t version = 0;
    if (!readRaw(is, version) || version != kVersion) {
        err = "unsupported project version (re-create from source)";
        return false;
    }

    Timeline fresh;
    int32_t trackCount = 2;
    if (!readRaw(is, fresh.fps) || !readRaw(is, fresh.playhead) ||
        !readRaw(is, fresh.inPoint) || !readRaw(is, fresh.outPoint) ||
        !readRaw(is, trackCount)) {
        err = "corrupt project file";
        return false;
    }

    // Media pool.
    uint32_t mediaCount = 0;
    if (!readRaw(is, mediaCount) || mediaCount > 1000000) {
        err = "corrupt project file (media count)";
        return false;
    }
    for (uint32_t i = 0; i < mediaCount; ++i) {
        bool ok = false;
        auto m = Media::deserialize(is, version, ok);
        if (!ok) {
            err = "corrupt project file (media table)";
            return false;
        }
        fresh.media[m->id()] = std::move(m);
    }

    // Shot pool.
    uint32_t shotCount = 0;
    if (!readRaw(is, shotCount) || shotCount > 100000) {
        err = "corrupt project file (shot count)";
        return false;
    }
    int maxShotId = 0;
    for (uint32_t i = 0; i < shotCount; ++i) {
        Shot s;
        int32_t sid = 0;
        uint8_t cutDiffer = 0;
        if (!readRaw(is, sid) ||
            !readRaw(is, s.timelineStart) || !readRaw(is, s.duration) ||
            !readRaw(is, s.cutIn) || !readRaw(is, s.cutOut) ||
            !readRaw(is, s.originalCutIn) || !readRaw(is, s.originalCutOut) ||
            !readRaw(is, cutDiffer) ||
            !readStr(is, s.name)) {
            err = "corrupt project file (shot table)";
            return false;
        }
        if (!readStr(is, s.sceneName)) {
            err = "corrupt project file (shot table)";
            return false;
        }
        s.cutDiffer = (cutDiffer != 0);
        s.id = sid;
        maxShotId = std::max(maxShotId, s.id);
        fresh.shots.push_back(std::move(s));
    }

    // Sequence table.
    uint32_t seqCount = 0;
    if (!readRaw(is, seqCount) || seqCount > 100000) {
        err = "corrupt project file (sequence count)";
        return false;
    }
    int maxClipId = 0, maxTrack = 0, maxSeqId = 0;
    for (uint32_t i = 0; i < seqCount; ++i) {
        Sequence s;
        int32_t seqId = 0;
        if (!readRaw(is, seqId) || !readStr(is, s.name)) {
            err = "corrupt project file (sequence table)";
            return false;
        }
        {
            int32_t pid = -1;
            if (!readRaw(is, pid)) {
                err = "corrupt project file (sequence projectId)";
                return false;
            }
            s.projectId = pid;
        }
        if (!readRaw(is, s.bgColor)) {
            err = "corrupt project file (sequence bgColor)";
            return false;
        }
        s.id = seqId;
        maxSeqId = std::max(maxSeqId, s.id);

        uint32_t nShots = 0;
        if (!readRaw(is, nShots) || nShots > 100000) {
            err = "corrupt project file (sequence shotIds)";
            return false;
        }
        for (uint32_t j = 0; j < nShots; ++j) {
            int32_t sid = 0;
            if (!readRaw(is, sid)) {
                err = "corrupt project file (sequence shotIds)";
                return false;
            }
            s.shotIds.push_back(sid);
        }

        uint32_t clipCount = 0;
        if (!readRaw(is, clipCount) || clipCount > 100000) {
            err = "corrupt project file (sequence clip count)";
            return false;
        }
        for (uint32_t j = 0; j < clipCount; ++j) {
            Clip c;
            int32_t cid = 0, ctrack = 0, cshotId = -1;
            uint8_t hidden = 0, caudio = 0;
            if (!readRaw(is, cid) || !readStr(is, c.mediaId) || !readRaw(is, ctrack) ||
                !readRaw(is, c.timelineStart) || !readRaw(is, c.duration) ||
                !readRaw(is, c.sourceOffset) || !readRaw(is, cshotId) ||
                !readRaw(is, hidden) || !readRaw(is, caudio)) {
                err = "corrupt project file (clip data)";
                return false;
            }
            c.hidden = (hidden != 0);
            c.audio = (caudio != 0);
            // Opacity ramps. Values are re-clamped on read against the clip's
            // duration (Timeline::clipFades), so nothing here has to validate them.
            if (!readRaw(is, c.fadeInFrames) || !readRaw(is, c.fadeOutFrames)) {
                err = "corrupt project file (clip fades)";
                return false;
            }
            // Audio→video link.
            {
                int32_t linkedTo = 0;
                if (!readRaw(is, linkedTo) || !readRaw(is, c.linkOffset)) {
                    err = "corrupt project file (clip link)";
                    return false;
                }
                c.linkedTo = linkedTo;
            }
            {
                uint32_t frameCount = 0;
                if (!readRaw(is, frameCount) || frameCount > 10000000) {
                    err = "corrupt project file (clip annotations)";
                    return false;
                }
                for (uint32_t fi = 0; fi < frameCount; ++fi) {
                    int64_t srcFrame = 0;
                    uint32_t strokeCount = 0;
                    if (!readRaw(is, srcFrame) || !readRaw(is, strokeCount) ||
                        strokeCount > 1000000) {
                        err = "corrupt project file (clip annotations)";
                        return false;
                    }
                    std::vector<AnnotStroke> strokes;
                    strokes.reserve(strokeCount);
                    for (uint32_t si = 0; si < strokeCount; ++si) {
                        AnnotStroke s;
                        uint32_t ptCount = 0;
                        if (!readRaw(is, s.r) || !readRaw(is, s.g) || !readRaw(is, s.b) ||
                            !readRaw(is, ptCount) || ptCount > 10000000) {
                            err = "corrupt project file (clip annotations)";
                            return false;
                        }
                        s.pts.reserve(ptCount);
                        for (uint32_t pi = 0; pi < ptCount; ++pi) {
                            AnnotPt p{};
                            if (!readRaw(is, p.x) || !readRaw(is, p.y) || !readRaw(is, p.hw)) {
                                err = "corrupt project file (clip annotations)";
                                return false;
                            }
                            s.pts.push_back(p);
                        }
                        strokes.push_back(std::move(s));
                    }
                    if (!strokes.empty())
                        c.annotations[srcFrame] = std::move(strokes);
                }
            }
            // Volume automation. Values are not validated here: valueAt copes with
            // any finite dB, and the lane clamps whatever it edits back into range.
            {
                uint8_t interp = 0;
                uint32_t ptCount = 0;
                if (!readRaw(is, c.volume.base) || !readRaw(is, interp) ||
                    !readRaw(is, ptCount) || ptCount > 1000000) {
                    err = "corrupt project file (clip volume curve)";
                    return false;
                }
                c.volume.interp = interp == (uint8_t)Curve::Interp::Linear
                                      ? Curve::Interp::Linear
                                      : Curve::Interp::Smooth;
                c.volume.pts.reserve(ptCount);
                for (uint32_t vi = 0; vi < ptCount; ++vi) {
                    CurvePoint cp{};
                    if (!readRaw(is, cp.t) || !readRaw(is, cp.v)) {
                        err = "corrupt project file (clip volume curve)";
                        return false;
                    }
                    c.volume.pts.push_back(cp);
                }
            }
            c.id = cid;
            c.track = std::max<int32_t>(ctrack, 0);
            c.shotId = cshotId;
            maxClipId = std::max(maxClipId, c.id);
            maxTrack = std::max(maxTrack, c.track); // unified space: audio + video share it
            s.clips.push_back(std::move(c));
        }

        // Dissolves.
        {
            uint32_t transCount = 0;
            if (!readRaw(is, transCount) || transCount > 100000) {
                err = "corrupt project file (sequence transition count)";
                return false;
            }
            for (uint32_t j = 0; j < transCount; ++j) {
                Transition t;
                int32_t tid = 0, aId = 0, bId = 0;
                if (!readRaw(is, tid) || !readRaw(is, aId) || !readRaw(is, bId) ||
                    !readRaw(is, t.inFrames) || !readRaw(is, t.outFrames)) {
                    err = "corrupt project file (transition data)";
                    return false;
                }
                t.id = tid;
                t.aClipId = aId;
                t.bClipId = bId;
                // A transition that no longer resolves is dropped by
                // App::pruneTransitions after load, and the id allocator is rebuilt
                // from the loaded ids there too — so nothing here has to validate.
                s.transitions.push_back(t);
            }
        }
        fresh.sequences.push_back(std::move(s));
    }

    // Project id.
    std::string id;
    readStr(is, id);
    projectId = std::move(id);

    // Sequence view filter. Absent/invalid → -1 (All).
    {
        int32_t vsi = -1;
        if (readRaw(is, vsi) && view)
            *view = ViewState{ vsi, -1 };
    }

    // Letterbox matte. Absent → off (fresh defaults).
    {
        double ratio = 0.0;
        float opacity = 1.0f;
        if (readRaw(is, ratio) && readRaw(is, opacity)) {
            fresh.letterboxRatio = ratio;
            fresh.letterboxOpacity = opacity;
        }
    }

    // Color-management mode.
    {
        uint8_t ocioOn = 0;
        if (readRaw(is, ocioOn))
            fresh.ocioEnabled = (ocioOn != 0);
    }

    // Project half of the view scope, plus the project table.
    {
        int32_t vpid = -1;
        uint32_t projCount = 0;
        if (readRaw(is, vpid) && readRaw(is, projCount) && projCount <= 100000) {
            if (view)
                view->projId = vpid;
            for (uint32_t i = 0; i < projCount; ++i) {
                SourceProject p;
                int32_t pid = 0;
                uint8_t whole = 0;
                if (!readRaw(is, pid) || !readStr(is, p.name) || !readStr(is, p.path) ||
                    !readRaw(is, whole))
                    break; // truncated trailer: keep what came in, as the fields above do
                p.id = pid;
                p.openedWhole = (whole != 0);
                fresh.projects.push_back(std::move(p));
            }
        }
    }

    // Custom track labels. A truncated trailer leaves the remaining rows on their
    // derived names.
    {
        uint32_t nameCount = 0;
        if (readRaw(is, nameCount) && nameCount <= 100000) {
            for (uint32_t i = 0; i < nameCount; ++i) {
                std::string name;
                if (!readStr(is, name))
                    break;
                fresh.trackNames.push_back(std::move(name));
            }
        }
    }

    // Disabled track rows. A truncated trailer leaves the rest enabled.
    {
        uint32_t offCount = 0;
        if (readRaw(is, offCount) && offCount <= 100000) {
            for (uint32_t i = 0; i < offCount; ++i) {
                uint8_t off = 0;
                if (!readRaw(is, off))
                    break;
                fresh.disabledTracks.push_back(off);
            }
        }
    }

    // The OCIO view transform. Absent or truncated leaves it empty, which the app
    // reads as "no preference": the config picks its own default view.
    {
        std::string ocioView;
        if (readStr(is, ocioView))
            fresh.ocioView = std::move(ocioView);
    }

    // The global Proxy dropdown's last selection. Absent or truncated leaves it
    // empty, i.e. "Full".
    {
        std::string proxyMode;
        if (readStr(is, proxyMode))
            fresh.proxyMode = std::move(proxyMode);
    }

    fresh.trackCount = std::max({ trackCount, maxTrack + 1, 1 });
    // Positions are stored per-sequence-local; repacking lays the sequences out
    // contiguously and restores absolute clip/shot positions in memory. A no-op
    // for single-sequence projects.
    fresh.repackSequences();
    fresh.clampPlayhead();
    tl = std::move(fresh);
    nextClipId = maxClipId + 1;
    nextSeqId  = maxSeqId  + 1;
    nextShotId = maxShotId + 1;
    return true;
}

bool save(const std::string& path, const Timeline& tl, const std::string& projectId,
          std::string& err, const ViewState& view) {
    std::ofstream os(path, std::ios::binary | std::ios::trunc);
    if (!os) {
        err = "cannot open file for writing";
        return false;
    }
    return saveTo(os, tl, projectId, err, view);
}

bool load(const std::string& path, Timeline& tl, int& nextClipId, int& nextSeqId,
          int& nextShotId, std::string& projectId, std::string& err,
          ViewState* view) {
    std::ifstream is(path, std::ios::binary);
    if (!is) {
        err = "cannot open file";
        return false;
    }
    return loadFrom(is, tl, nextClipId, nextSeqId, nextShotId, projectId, err, view);
}

bool saveBuffer(std::string& out, const Timeline& tl, const std::string& projectId,
                std::string& err, const ViewState& view) {
    std::ostringstream os(std::ios::binary);
    if (!saveTo(os, tl, projectId, err, view))
        return false;
    out = os.str();
    return true;
}

bool loadBuffer(const std::string& in, Timeline& tl, int& nextClipId, int& nextSeqId,
                int& nextShotId, std::string& projectId, std::string& err,
                ViewState* view) {
    std::istringstream is(in, std::ios::binary);
    return loadFrom(is, tl, nextClipId, nextSeqId, nextShotId, projectId, err, view);
}

} // namespace Project

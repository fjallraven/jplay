#pragma once

#include "Timeline.h"

#include <cstdint>
#include <string>
#include <vector>

// Binary project file ("JPLY"). Layout, little-endian. Only the current version
// (34) loads; the (vN+) tags record which version first wrote each field.
//   char[4]   magic "JPLY"
//   u32       version (34)
//   f64       fps
//   i64       playhead, inPoint, outPoint
//   i32       trackCount           (single unified track stack)
//   u32       mediaCount
//   per media:
//     u32+str   id
//     u8        type (ClipType)
//     u32+str   path
//     u32+str   name
//     u32       metaCount
//     per meta:
//       u32+str   key
//       u32+str   value
//     i32       width, height
//     i64       frameCount
//     f64       fps
//     f32       pixelAspect          (v28+; pixel width / height, 1 = square)
//     u64       freshHash
//     u8        pickersResolved
//     u32       pickerCount
//     u32+str   pickerKey[pickerCount]
//     u32+str   colorSpace           (v32+; explicit OCIO input space, "" = auto)
//   u32       shotCount
//   per shot:
//     i32       id
//     i64       timelineStart, duration, cutIn, cutOut
//     u32+str   name
//   u32       sequenceCount
//   per sequence:
//     i32       id
//     u32+str   name
//     i32       projectId            (v29+; index into the project table, -1 = none;
//                                     was a u32+str project name in v22..v28)
//     u32       shotIdCount
//     i32       shotId[shotIdCount]
//     u32       clipCount
//     per clip:
//       i32     id
//       u32+str mediaId
//       i32     track (index within its group: video tracks or audio tracks)
//       i64     timelineStart, duration, sourceOffset
//       i32     shotId (-1 = none)
//       u8      hidden
//       u8      audio                 (v18+; 1 = clip lives on an audio track)
//       i64     fadeInFrames          (v25+; head/tail opacity ramps, 0 = none)
//       i64     fadeOutFrames
//       i32     linkedTo              (v26+; parent clip this one follows, 0 = none)
//       i64     linkOffset            (start relative to that parent)
//       u32     annotFrameCount
//       per annotated source frame:
//         i64     sourceFrame
//         u32     strokeCount
//         per stroke:
//           f32     r, g, b
//           u32     pointCount
//           f32     x, y, hw          (pointCount times)
//       f32     volumeBase            (v34+; dB, 0 = unity)
//       u8      volumeInterp          (Curve::Interp; 0 = linear, 1 = smooth)
//       u32     volumePointCount
//       per volume point:
//         i64     sourceFrame
//         f32     dB
//     u32     transitionCount         (v24+; dissolves on this sequence's cuts)
//     per transition:
//       i32     id
//       i32     aClipId, bClipId      (the cut is aClipId's end; no stored position)
//       i64     inFrames, outFrames
//   u32+str   projectId
//   i32       viewSeqIdx (v14+; -1 = All)
//   f64       letterboxRatio (v17+; target aspect W/H, 0 = off)
//   f32       letterboxOpacity (v17+; matte bar opacity 0..1)
//   u8        ocioEnabled (v20+)
//   i32       viewProjId (v29+; project view scope, -1 = none. v27..v28 wrote a
//                         u32+str project name plus a u32-counted list of the
//                         project names that had been opened whole)
//   u32       projectCount (v29+; the project table Sequence::projectId indexes)
//   per project:
//     i32       id
//     u32+str   name
//     u32+str   path
//     u8        openedWhole
//   u32       trackNameCount (v30+; custom track labels, one per row, empty = the
//                             row's derived name)
//   u32+str   trackName[trackNameCount]
//   u32       disabledTrackCount (v31+; one flag per row, non-zero = row switched
//                                 off, its clips skipped in compositing and audio)
//   u8        disabledTrack[disabledTrackCount]
//   u32+str   ocioView (v33+; the OCIO view transform the project was reviewed
//                       through, "" = the config's default view)
namespace Project {

// The view scope that was in force when the project was saved: either a single
// sequence (seqIdx >= 0) or every sequence of one project (projId >= 0, seqIdx -1);
// neither = the All view. The scoped project's members are not stored — they are
// the sequences carrying that projectId, recomputed on load. Which projects were
// opened whole is part of the timeline now (SourceProject::openedWhole), which is
// what lets the Sequence popup group their sequences under a header again.
struct ViewState {
    int seqIdx = -1;
    int projId = -1;
};

struct Thumbnail {
    int32_t width = 0;
    int32_t height = 0;
    std::vector<uint8_t> rgba;
    bool valid() const {
        return width > 0 && height > 0 && rgba.size() == (size_t)width * height * 4;
    }
};

bool save(const std::string& path, const Timeline& tl, const std::string& projectId,
          std::string& err, const ViewState& view = {});

bool load(const std::string& path, Timeline& tl, int& nextClipId, int& nextSeqId,
          int& nextShotId, std::string& projectId, std::string& err,
          ViewState* view = nullptr);

// In-memory variants: serialize the project to / from a byte buffer, using the
// exact same layout as the file form. Used to ship a project over the network
// for sync review (see SyncSession).
bool saveBuffer(std::string& out, const Timeline& tl, const std::string& projectId,
                std::string& err, const ViewState& view = {});

bool loadBuffer(const std::string& in, Timeline& tl, int& nextClipId, int& nextSeqId,
                int& nextShotId, std::string& projectId, std::string& err,
                ViewState* view = nullptr);

} // namespace Project

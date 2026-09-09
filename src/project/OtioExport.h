#pragma once

#include "Timeline.h"

#include <string>
#include <vector>

// Writes a jplay Timeline out as an OpenTimelineIO (.otio) document, in the same
// shape OtioImport reads: one video Track per jplay track, each Clip an
// ExternalReference with the media's available_range and the cut as an absolute
// source_range, Gaps filling the space between clips, and shot/sequence/scene
// names carried in clip metadata.
//
// Deliberately lossy, and deliberately export-only. OTIO carries the edit and
// nothing else, so everything jplay keeps alongside it — annotations, audio
// clips, hidden flags, letterbox, in/out points, colour-management mode, cached
// media names and metadata — has nowhere to go; survey() lists what a given
// project would lose so the caller can warn before writing. Re-importing an
// exported file will not reproduce the same timeline either: OtioImport reserves
// track 0 as a shot track and snaps clips on lower tracks onto the shots they
// match. The .jpproj remains the only round-trip format.
namespace OtioExport {

// Human-readable lines naming what `tl` holds that the .otio cannot carry.
// Empty when the project is entirely representable.
std::vector<std::string> survey(const Timeline& tl);

// Write `tl` to `path`. Returns false (with `err` set) on a write error or when
// the timeline has no video clips to export.
//
// Opens the decoder of every referenced image sequence to read its first frame
// number, so the exported frame ranges are the editorial ones other tools
// expect. That is a directory scan per sequence, which on a network share makes
// this a blocking call — acceptable for a deliberate export, like the frame
// snapshot the movie/image-sequence exports build up front. A sequence that
// can't be opened (and any video source) is treated as 0-based, which stays
// self-consistent because an import rebases by the same number.
bool save(const std::string& path, const Timeline& tl, std::string& err);

} // namespace OtioExport

#pragma once

#include "Timeline.h"

#include <string>

// Reads an OpenTimelineIO (.otio) document into a jplay Timeline. The inverse
// of shotdetect's OTIO export: each OTIO video Track becomes a jplay track,
// each Clip a jplay clip backed by its ExternalReference's target_url, with the
// on-timeline duration and source offset taken from the clip's (trimmed) source
// range. Gaps advance the position so clips keep their place on the track.
namespace OtioImport {

// Parse `path` into a fresh Timeline. Media decoders are NOT opened and metadata
// is NOT probed here (Media carry only path/type); callers should run the usual
// background metadata refresh afterwards. `nextClipId`, `nextSeqId`, and
// `nextShotId` receive the next free ids. Returns false on parse error or when
// no video clips are present.
bool load(const std::string& path, Timeline& tl,
          int& nextClipId, int& nextSeqId, int& nextShotId,
          std::string& err);

} // namespace OtioImport

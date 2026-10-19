# Draw tool

Freehand pencil markup over the frame, for review notes.

## Using it

Open the **Draw** panel from the icon strip down the left edge (or
**View ▸ Panels ▸ Draw**). 

Holding `E` takes the left button back for the exposure/gamma scrub, so checking
exposure does not mean leaving the draw tool.

## Where markup lives

Strokes are stored **per clip, keyed by source frame** — `sourceOffset` plus the
local offset — so a note stays glued to the media frame it was drawn on even
when the clip is moved or the timeline is repacked. Two clips referencing the
same media carry independent annotations. Frames with nothing on them are not
stored.

Points are in image coordinates at the media's resolution, not screen pixels, so
a stroke tracks the frame under zoom and pan and reproduces at the right place
on the [review monitor](PRESENTATION_MODE.md#review-monitor).

Annotations are part of the project: they are serialized into the `.jpproj` and
come back when it is reopened.

## In a sync session

Spectators may draw too. A spectator sends each completed stroke to the host,
which merges all the markup for that frame and broadcasts the combined set back;
spectators then replace their own markup with it. A **Clear** travels in either
direction. See [sync session](SYNC_SESSION.md).

## Where it shows

The player and the review monitor both draw annotations. The review monitor
shows the clean program frame plus the pencil and nothing else — no status text,
no badges, no legend.

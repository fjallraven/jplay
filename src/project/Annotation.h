#pragma once

#include <vector>

// Pencil-annotation primitives, shared between the live editing/rendering path
// (App) and the data model (Clip stores per-source-frame strokes; Project
// serializes them). Positions are in image coordinates (media resolution) so a
// stroke tracks the frame under zoom/pan and reproduces on the review monitor.
struct AnnotPt { float x, y, hw; };            // image-space position + image half-width

// Each stroke captures the pencil color active when it was drawn, so recoloring
// the wheel only affects subsequent strokes.
struct AnnotStroke { std::vector<AnnotPt> pts; float r = 1, g = 1, b = 1; };

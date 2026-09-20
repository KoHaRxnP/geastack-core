// SPDX-License-Identifier: Apache-2.0
#pragma once

namespace gea::embedded::ui {

#ifndef GEA_EMBEDDED_DIRTY_REGION_MAX_RECTS
#define GEA_EMBEDDED_DIRTY_REGION_MAX_RECTS 32
#endif

class DirtyRegions {
public:
	struct Rect {
		int x0, y0, x1, y1;
		// Originating node id, or -1 for "multi-source / unknown". Set by
		// `add(...)` when a fresh rect is recorded; cleared to -1 when a
		// new rect coalesces with an existing one whose origin differs.
		// The replay path uses this to skip the full `drawNodeOrder`
		// iteration when the rect's only contributing nodes are the
		// origin's ancestor chain (typical for non-overlapping ball-style
		// keyed lists).
		int origin = -1;
	};

	static constexpr int kMaxRects = GEA_EMBEDDED_DIRTY_REGION_MAX_RECTS;

	static void add(Rect *rects, int *count, int maxCount, Rect rect);

private:
	static int area(const Rect *rect);
	static Rect unite(const Rect *a, const Rect *b);
	static int shouldMerge(const Rect *a, const Rect *b);
};

}  // namespace gea::embedded::ui

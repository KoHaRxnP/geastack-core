// SPDX-License-Identifier: Apache-2.0
#include "dirty_regions.h"

namespace gea::embedded::ui {

int DirtyRegions::area(const Rect *r)
{
	if (!r || r->x0 > r->x1 || r->y0 > r->y1) return 0;
	return (r->x1 - r->x0 + 1) * (r->y1 - r->y0 + 1);
}

DirtyRegions::Rect DirtyRegions::unite(const Rect *a, const Rect *b)
{
	Rect out = *a;
	if (b->x0 < out.x0) out.x0 = b->x0;
	if (b->y0 < out.y0) out.y0 = b->y0;
	if (b->x1 > out.x1) out.x1 = b->x1;
	if (b->y1 > out.y1) out.y1 = b->y1;
	// Coalesced rects can mix two unrelated node origins. The replay path
	// only short-circuits to the ancestor-chain walk when the origin is
	// definite; mark the rect as multi-source by clearing the origin so
	// the fallback (full drawNodeOrder iteration) runs and stays correct.
	if (a->origin != b->origin) out.origin = -1;
	return out;
}

int DirtyRegions::shouldMerge(const Rect *a, const Rect *b)
{
	if (a->x0 <= b->x1 + 1 && a->x1 + 1 >= b->x0 &&
	    a->y0 <= b->y1 + 1 && a->y1 + 1 >= b->y0)
		return 1;
	Rect merged = unite(a, b);
	int merged_area = area(&merged);
	int separate_area = area(a) + area(b);
	return merged_area <= separate_area + 512;
}

void DirtyRegions::add(Rect *rects, int *count, int max_count, Rect rect)
{
	if (!rects || !count || max_count <= 0 || rect.x0 > rect.x1 || rect.y0 > rect.y1) return;

	for (int i = 0; i < *count; i++) {
		if (!shouldMerge(&rects[i], &rect)) continue;
		rects[i] = unite(&rects[i], &rect);
		return;
	}

	if (*count < max_count) {
		rects[(*count)++] = rect;
		return;
	}

	int best = 0;
	int best_cost = 0x7fffffff;
	for (int i = 0; i < *count; i++) {
		Rect merged = unite(&rects[i], &rect);
		int cost = area(&merged) - area(&rects[i]);
		if (cost < best_cost) {
			best_cost = cost;
			best = i;
		}
	}
	rects[best] = unite(&rects[best], &rect);
}

}  // namespace gea::embedded::ui

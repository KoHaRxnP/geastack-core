// SPDX-License-Identifier: Apache-2.0
#include "layout_snapshot.h"
#include "internal.h"
#include "tree_state.h"

namespace gea::embedded::ui {

void LayoutSnapshot::capture()
{
	auto &state = treeState();
	// Snapshotting previous_transform_* costs ~15 pooled rstyle() reads per node. They
	// are ONLY read back when a transform is active (transformedBounds early-outs on
	// !anyTransformActive), and anyTransformActive checks both current and previous
	// transforms — so for a tree with no transforms (e.g. bouncing-balls) these reads
	// are pure per-frame waste and the stale previous_* values are never observed. Skip
	// them. The filter snapshot is independent (no transform), so it always runs.
	const bool captureTransforms = ViewRenderer::anyTransformActive();
	for (int i = 0; i < state.nodeCount; i++) {
		state.nodes[i].layout.previous_x = state.nodes[i].layout.x;
		state.nodes[i].layout.previous_y = state.nodes[i].layout.y;
		state.nodes[i].layout.previous_width = state.nodes[i].layout.width;
		state.nodes[i].layout.previous_height = state.nodes[i].layout.height;
		state.nodes[i].layout.previous_scroll_x = state.nodes[i].layout.scroll_x;
		state.nodes[i].layout.previous_scroll_y = state.nodes[i].layout.scroll_y;
		// ONE RareStyle pool lookup per node, not ~16 — rstyle() is an out-of-line
		// deque index (separate cache line from the node). Per-field lookups here cost
		// ~1ms/frame on the spinning css-3d-cube (the "snap" phase); spine read these as
		// direct ComputedStyle fields. (Same regression class as the reproject path.)
		const RareStyle &rs = rstyle(state.nodes[i].style);
		if (captureTransforms) {
			state.nodes[i].render.previous_transform_rotate = rs.transform_rotate;
			state.nodes[i].render.previous_transform_rotate_x = rs.transform_rotate_x;
			state.nodes[i].render.previous_transform_rotate_y = rs.transform_rotate_y;
			state.nodes[i].render.previous_transform_translate_x = rs.transform_translate_x;
			state.nodes[i].render.previous_transform_translate_y = rs.transform_translate_y;
			state.nodes[i].render.previous_transform_translate_z = rs.transform_translate_z;
			state.nodes[i].render.previous_transform_translate_x_percent = rs.transform_translate_x_percent;
			state.nodes[i].render.previous_transform_translate_y_percent = rs.transform_translate_y_percent;
			state.nodes[i].render.previous_transform_scale_x = rs.transform_scale_x;
			state.nodes[i].render.previous_transform_scale_y = rs.transform_scale_y;
			state.nodes[i].render.previous_transform_origin_x = rs.transform_origin_x;
			state.nodes[i].render.previous_transform_origin_y = rs.transform_origin_y;
			state.nodes[i].render.previous_perspective = rs.perspective;
			state.nodes[i].render.previous_perspective_origin_x = rs.perspective_origin_x;
			state.nodes[i].render.previous_perspective_origin_y = rs.perspective_origin_y;
		}
		state.nodes[i].render.previous_filter_blur_radius = rs.filter_blur_radius;
		state.nodes[i].render.dirty = 0;
		state.nodes[i].render.layout_dirty = 0;
		state.nodes[i].render.scroll_dirty = 0;
		state.nodes[i].render.non_scroll_dirty = 0;
		state.nodes[i].render.transform_dirty = 0;
		state.nodes[i].render.bg_recolor_pending = 0;
		state.nodes[i].render.text_layout_stable = 0;
		state.nodes[i].render.text_partial_dirty = 0;
		state.nodeCommandDirty[i] = 0;
		state.nodeCommandDirtyBoundsValid[i] = 0;
		state.nodeCommandDirtyCanOverpaint[i] = 0;
		if (state.nodes[i].type == NodeType::VirtualList) VirtualListRenderer::captureSnapshot(i);
	}
	for (int i = 0; i < kScrollDirtyWordCount; i++)
		state.scrollDirtyNodes[i] = 0;
	state.scrollDirtyAny = false;
}

void LayoutSnapshot::markChangesDirty()
{
	auto &state = treeState();
	for (int i = 0; i < state.nodeCount; i++) {
		Node *n = &state.nodes[i];
		if (n->layout.x != n->layout.previous_x ||
		    n->layout.y != n->layout.previous_y ||
		    n->layout.width != n->layout.previous_width ||
		    n->layout.height != n->layout.previous_height) {
			n->render.dirty = 1;
			n->render.layout_dirty = 1;
			n->render.non_scroll_dirty = 1;
		}
	}
}

}  // namespace gea::embedded::ui

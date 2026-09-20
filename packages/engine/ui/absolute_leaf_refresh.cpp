// SPDX-License-Identifier: Apache-2.0
#include "absolute_leaf_refresh.h"

#include "internal.h"  // ViewRenderer::anyTransformActive (whole-tree transform fast-out)
#include "refresh_perf.h"
#include "tree_state.h"

namespace gea::embedded::ui {

namespace {

struct ScopedRefreshStat
{
	std::int64_t start;
	std::int64_t &slot;

	explicit ScopedRefreshStat(std::int64_t &slot)
		: start(refreshPerfNowUs()), slot(slot)
	{
	}

	~ScopedRefreshStat()
	{
		slot += refreshPerfNowUs() - start;
	}
};

bool hasTransformState(const Node &node)
{
	// Whole-tree fast out: a transform-free tree (the common case — e.g. the bouncing
	// balls) skips all the pooled rstyle transform reads below. Safe to gate here now
	// that anyTransformPresent() carries a DURABLE no-transform cache: it no longer
	// re-scans on every mid-refresh refreshSerial bump, so calling it from mode() (which
	// runs before those bumps) doesn't trigger the scan-then-invalidate that regressed
	// fps before. A transform ADD routes through setStyle → clears the durable cache.
	if (!ViewRenderer::anyTransformActive()) return false;
	const RareStyle &rs = rstyle(node.style); // one pool lookup, not 11
	return rs.transform_rotate != 0 ||
	       node.render.previous_transform_rotate != 0 ||
	       rs.transform_rotate_x != 0 ||
	       node.render.previous_transform_rotate_x != 0 ||
	       rs.transform_rotate_y != 0 ||
	       node.render.previous_transform_rotate_y != 0 ||
	       rs.transform_translate_x != 0 ||
	       node.render.previous_transform_translate_x != 0 ||
	       rs.transform_translate_y != 0 ||
	       node.render.previous_transform_translate_y != 0 ||
	       rs.transform_translate_z != 0 ||
	       node.render.previous_transform_translate_z != 0 ||
	       rs.transform_translate_x_percent != 0 ||
	       node.render.previous_transform_translate_x_percent != 0 ||
	       rs.transform_translate_y_percent != 0 ||
	       node.render.previous_transform_translate_y_percent != 0 ||
	       rs.transform_scale_x != 1000 ||
	       node.render.previous_transform_scale_x != 1000 ||
	       rs.transform_scale_y != 1000 ||
	       node.render.previous_transform_scale_y != 1000 ||
	       rs.perspective > 0 ||
	       node.render.previous_perspective > 0;
}

int alignedAbsoluteOffset(const Node &parent, const Node &child, bool horizontal)
{
	const bool row = usesRowLayout(parent.style);
	const int crossAlign = child.style.align_self >= 0 ? child.style.align_self : parent.style.align_items;
	const int align = horizontal
		? (row ? parent.style.justify_content : crossAlign)
		: (row ? crossAlign : parent.style.justify_content);
	const int parentSize = horizontal ? parent.layout.width : parent.layout.height;
	const int childSize = horizontal ? child.layout.width : child.layout.height;
	const int beforeMargin = horizontal ? child.style.margin[3] : child.style.margin[0];
	const int afterMargin = horizontal ? child.style.margin[1] : child.style.margin[2];
	int available = parentSize;
	if (available < 0) available = 0;
	const int occupied = childSize + beforeMargin + afterMargin;
	int free = available - occupied;
	if (free < 0) free = 0;

	int offset = beforeMargin;
	if (align == 1) offset += free / 2;
	else if (align == 2) offset += free;
	return offset;
}

int resolvedPositionOffset(const Node &parent, const Node &child, int side)
{
	const int basis = (side == 0 || side == 2) ? parent.layout.height : parent.layout.width;
	int offset = child.style.pos_offsets[side] != kUnset ? child.style.pos_offsets[side] : 0;
	const int percent = child.style.pos_offset_percent[side];
	if (percent != kUnset) {
		const int numerator = basis * percent;
		offset += (numerator + (numerator >= 0 ? 500 : -500)) / 1000;
	}
	return offset;
}

bool hasPositionOffset(const Node &node, int side)
{
	return node.style.pos_offsets[side] != kUnset || node.style.pos_offset_percent[side] != kUnset;
}

bool hasExplicitWidth(const Node &node)
{
	return node.style.width != kUnset || node.style.width_percent != kUnset;
}

bool hasExplicitHeight(const Node &node)
{
	return node.style.height != kUnset || node.style.height_percent != kUnset;
}

bool retainableAbsoluteLayoutNode(const Node &node)
{
	if ((!isViewLikeNodeType(node.type) && node.type != NodeType::Image) || node.style.position != 1) return false;
	if (node.parent < 0) return false;
	if (!hasExplicitWidth(node) || !hasExplicitHeight(node)) return false;
	if (node.first_child >= 0) {
		if (node.style.width != kUnset && node.style.width != node.layout.width) return false;
		if (node.style.height != kUnset && node.style.height != node.layout.height) return false;
		if (node.layout.width != node.layout.previous_width || node.layout.height != node.layout.previous_height) return false;
	}
	if (node.style.min_width != 0 ||
	    node.style.min_height != 0 ||
	    node.style.max_width != kUnset ||
	    node.style.max_height != kUnset) return false;
	if (node.style.flex != 0 ||
	    node.style.padding[0] ||
	    node.style.padding[1] ||
	    node.style.padding[2] ||
	    node.style.padding[3]) return false;
	if (node.style.margin[0] || node.style.margin[1] || node.style.margin[2] || node.style.margin[3]) return false;
	return true;
}

bool retainableAbsoluteNode(const Node &node)
{
	if (!retainableAbsoluteLayoutNode(node)) return false;
	if (hasTransformState(node)) return false;
	if (rstyle(node.style).transform_origin_x != node.render.previous_transform_origin_x ||
	    rstyle(node.style).transform_origin_y != node.render.previous_transform_origin_y) return false;
	return true;
}

bool retainableStaticTransformedPaintNode(const Node &node)
{
	if (!isViewLikeNodeType(node.type) || node.style.position != 1) return false;
	if (node.parent < 0) return false;
	if (!hasTransformState(node) || node.render.transform_dirty) return false;
	if (node.layout.x != node.layout.previous_x ||
	    node.layout.y != node.layout.previous_y ||
	    node.layout.width != node.layout.previous_width ||
	    node.layout.height != node.layout.previous_height)
		return false;
	if (rstyle(node.style).transform_origin_x != node.render.previous_transform_origin_x ||
	    rstyle(node.style).transform_origin_y != node.render.previous_transform_origin_y)
		return false;
	return true;
}

void translateDescendantLayouts(TreeState &state, int node, int dx, int dy)
{
	if ((dx == 0 && dy == 0) || node < 0 || node >= state.nodeCount) return;
	for (int child = state.nodes[node].first_child; child >= 0; child = state.nodes[child].next_sibling) {
		state.nodes[child].layout.x += dx;
		state.nodes[child].layout.y += dy;
		translateDescendantLayouts(state, child, dx, dy);
	}
}

}  // namespace

int AbsoluteLeafRefresh::mode()
{
	auto &state = treeState();
	auto &perf = refreshPerfStatsMutable();
	ScopedRefreshStat timer(perf.treeAbsModeUs);
	perf.treeAbsModeCalls++;
	int dirtyNodes = 0;
	for (int i = 0; i < state.nodeCount; i++) {
		if (state.nodes[i].render.dirty)
			dirtyNodes++;
	}
	perf.treeAbsModeDirtyNodes += dirtyNodes;
	if (dirtyNodes == 0) {
		perf.treeAbsModeNoop++;
		return -1;
	}
	auto reject = [&](int node, int reason) {
		perf.treeAbsModeFull++;
		perf.treeAbsModeRejectNode = node;
		perf.treeAbsModeRejectReason = reason;
		return 0;
	};
	for (int i = 0; i < state.nodeCount; i++) {
		Node *n = &state.nodes[i];
		if (!n->render.dirty) continue;
		if (n->render.transform_dirty) {
			if (retainableAbsoluteLayoutNode(*n)) continue;
			if (n->layout.x != n->layout.previous_x ||
			    n->layout.y != n->layout.previous_y ||
			    n->layout.width != n->layout.previous_width ||
			    n->layout.height != n->layout.previous_height) return reject(i, 1);
			continue;
		}
		if (n->type == NodeType::VirtualList) return reject(i, 2);
		// Text nodes whose new content was pre-measured by Tree::setText are
		// content-only updates and don't need a full re-layout pass. Absolute
		// text may resize out of flow; in-flow text is admitted only when
		// setText confirmed the existing layout box stayed unchanged.
		if (n->type == NodeType::Text && n->render.text_layout_stable &&
		    n->parent >= 0 && n->first_child < 0) {
			if (hasTransformState(*n)) return reject(i, 3);
			if (rstyle(n->style).transform_origin_x != n->render.previous_transform_origin_x ||
			    rstyle(n->style).transform_origin_y != n->render.previous_transform_origin_y) return reject(i, 4);
			if (n->style.position != 1 &&
			    (n->layout.x != n->layout.previous_x ||
			     n->layout.y != n->layout.previous_y ||
			     n->layout.width != n->layout.previous_width ||
			     n->layout.height != n->layout.previous_height))
				return reject(i, 5);
			continue;
		}
		// An absolute <img> leaf that reaches here is content-dirty — its image
		// id changed (e.g. a runtime src swap: an EPUB cover extracted to SD and
		// applied via setAttribute after mount). The fast position-only refresh
		// would re-blit the stale cached paint and never re-record the new image,
		// so force a full re-record. Rare, so the lost fast-path frame is fine.
		if (n->type == NodeType::Image) return reject(i, 7);
		if (retainableStaticTransformedPaintNode(*n)) continue;
		if (!retainableAbsoluteNode(*n)) return reject(i, 6);
		// A size change (layout != style) stays on the fast path only for leaves:
		// out of flow, no children -> refreshPositions() reconciles layout from
		// style with no tree-wide relayout. Non-leaf nodes must keep stable size
		// so their descendants can be translated instead of reflowed.
	}
	perf.treeAbsModeFast++;
	return 1;
}

void AbsoluteLeafRefresh::refreshPositions()
{
	auto &state = treeState();
	for (int i = 0; i < state.nodeCount; i++) {
		Node *n = &state.nodes[i];
		if (!n->render.dirty) continue;
		if (n->render.transform_dirty && !retainableAbsoluteLayoutNode(*n)) continue;
		if (n->type == NodeType::Text && n->render.text_layout_stable && n->style.position != 1) continue;
		// Reconcile size from style for resized absolute leaves (mode() allowed
		// the change). Before position so right/bottom-anchored offsets use the
		// new extent. Percent-sized nodes keep the already-resolved layout box;
		// the full layout pass owns recomputing them when their containing block
		// changes.
		if (n->style.width != kUnset) n->layout.width = n->style.width;
		if (n->style.height != kUnset) n->layout.height = n->style.height;
		Node *p = &state.nodes[n->parent];
		const int before_x = n->layout.x;
		const int before_y = n->layout.y;
		int parent_x = p->layout.x;
		if (p->style.overflow == 2 && scrollsOverflowX(p->style)) parent_x -= p->layout.scroll_x;
		if (hasPositionOffset(*n, 3))
			n->layout.x = parent_x + resolvedPositionOffset(*p, *n, 3);
		else if (hasPositionOffset(*n, 1))
			n->layout.x = parent_x + p->layout.width - n->layout.width - resolvedPositionOffset(*p, *n, 1);
		else
			n->layout.x = parent_x + alignedAbsoluteOffset(*p, *n, true);

		int parent_y = p->layout.y;
		if (p->style.overflow == 2 && scrollsOverflowY(p->style)) parent_y -= p->layout.scroll_y;
		if (hasPositionOffset(*n, 0))
			n->layout.y = parent_y + resolvedPositionOffset(*p, *n, 0);
		else if (hasPositionOffset(*n, 2))
			n->layout.y = parent_y + p->layout.height - n->layout.height - resolvedPositionOffset(*p, *n, 2);
		else
			n->layout.y = parent_y + alignedAbsoluteOffset(*p, *n, false);
		translateDescendantLayouts(state, i, n->layout.x - before_x, n->layout.y - before_y);
	}
}

}  // namespace gea::embedded::ui

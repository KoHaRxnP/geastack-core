// SPDX-License-Identifier: Apache-2.0
#include "mirror.h"

#include "internal.h"
#include "tree_state.h"

namespace gea::embedded::ui {

Mirror &Mirror::instance()
{
	static Mirror mirror;
	return mirror;
}

bool Mirror::scrollDirtyAny() const
{
	return treeState().scrollDirtyAny;
}

void Mirror::copyScrollDirty(uint64_t *dst, int word_count) const
{
	if (!dst || word_count <= 0) return;
	auto &state = treeState();
	for (int i = 0; i < word_count; i++)
		dst[i] = i < kScrollDirtyWordCount ? state.scrollDirtyNodes[i] : 0;
}

void Mirror::clearScrollDirty() const
{
	auto &state = treeState();
	for (int i = 0; i < kScrollDirtyWordCount; i++)
		state.scrollDirtyNodes[i] = 0;
	state.scrollDirtyAny = false;
}

bool Mirror::nodeIsScrollable(int node) const
{
	auto &state = treeState();
	if (node < 0 || node >= state.nodeCount) return false;
	Node *n = &state.nodes[node];
	return isViewLikeNodeType(n->type) &&
	       ((n->type != NodeType::VirtualList && scrollsOverflowX(n->style) && ViewRenderer::scrollMaxX(*n) > 0) ||
	        ((n->type == NodeType::VirtualList || scrollsOverflowY(n->style)) && ViewRenderer::scrollMaxY(*n) > 0));
}

int Mirror::scrollX(int node) const
{
	auto &state = treeState();
	if (node < 0 || node >= state.nodeCount) return 0;
	return state.nodes[node].layout.scroll_x;
}

void Mirror::setScrollX(int node, int scroll_x) const
{
	auto &state = treeState();
	if (node < 0 || node >= state.nodeCount) return;
	Node *n = &state.nodes[node];
	if (!isViewLikeNodeType(n->type) || !scrollsOverflowX(n->style) || n->type == NodeType::VirtualList) return;

	int max_x = ViewRenderer::scrollMaxX(*n);
	if (scroll_x < 0) scroll_x = 0;
	if (scroll_x > max_x) scroll_x = max_x;
	if (scroll_x == n->layout.scroll_x) return;

	n->layout.scroll_x = scroll_x;
	n->render.dirty = 1;
	n->render.layout_dirty = 1;
	n->render.non_scroll_dirty = 1;
	n->render.scroll_dirty = 1;
	state.scrollDirtyNodes[node / 64] |= (1ull << (node % 64));
	state.scrollDirtyAny = true;
}

int Mirror::scrollY(int node) const
{
	auto &state = treeState();
	if (node < 0 || node >= state.nodeCount) return 0;
	return state.nodes[node].layout.scroll_y;
}

void Mirror::setScrollY(int node, int scroll_y) const
{
	auto &state = treeState();
	if (node < 0 || node >= state.nodeCount) return;
	Node *n = &state.nodes[node];
	if (!isViewLikeNodeType(n->type) || (n->type != NodeType::VirtualList && !scrollsOverflowY(n->style))) return;

	int max_y = ViewRenderer::scrollMaxY(*n);
	if (scroll_y < 0) scroll_y = 0;
	if (scroll_y > max_y) scroll_y = max_y;
	if (scroll_y == n->layout.scroll_y) return;

	n->layout.scroll_y = scroll_y;
	n->render.dirty = 1;
	n->render.layout_dirty = 1;
	n->render.scroll_dirty = 1;
	state.scrollDirtyNodes[node / 64] |= (1ull << (node % 64));
	state.scrollDirtyAny = true;
}

}  // namespace gea::embedded::ui

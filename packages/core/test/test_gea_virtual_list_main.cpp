#include "native_test_harness.h"

#include "app.h"
#include "events.h"
#include "ui/tree_internal.h"

#include <cstdlib>
#include <cstdio>
#include <string>
#include <vector>

extern void __gea_top_level();

namespace {
constexpr int kViewportWidth = 720;
constexpr int kViewportHeight = 1440;
constexpr double kDevicePixelRatio = 2.0;

bool renderOnce()
{
	auto &tree = gea::embedded::ui::Tree::instance();
	const int root = tree.mountedRoot();
	if (root < 0) return false;
	tree.refresh(root, kViewportWidth, kViewportHeight);
	return true;
}

bool isDescendantOf(int nodeId, int rootId)
{
	auto &tree = gea::embedded::ui::Tree::instance();
	for (int current = nodeId; current >= 0 && current < tree.nodeCount(); current = tree.node(current).parent) {
		if (current == rootId) return true;
	}
	return false;
}

int descendantWithClass(int nodeId, const char *className)
{
	auto &tree = gea::embedded::ui::Tree::instance();
	if (nodeId < 0 || nodeId >= tree.nodeCount()) return -1;
	if (tree.hasClass(nodeId, className)) return nodeId;
	for (int child = tree.node(nodeId).first_child; child >= 0; child = tree.node(child).next_sibling) {
		const int found = descendantWithClass(child, className);
		if (found >= 0) return found;
	}
	return -1;
}

bool verifyVisibleRowBindings(int listNode, const char *phase)
{
	auto &tree = gea::embedded::ui::Tree::instance();
	const auto &list = tree.node(listNode);
	int checked = 0;
	for (int rowId = list.first_child; rowId >= 0; rowId = tree.node(rowId).next_sibling) {
		const auto &row = tree.node(rowId);
		if (row.style.display == 1) continue;
		if (row.layout.height <= 0) continue;
		const int labelNode = descendantWithClass(rowId, "probe-row-index");
		const int pixelsNode = descendantWithClass(rowId, "probe-row-pixels");
		if (labelNode < 0 || pixelsNode < 0) {
			std::fprintf(stderr, "[test_gea_virtual_list_main] %s row %d missing label/pixels\n", phase, rowId);
			return false;
		}
		const std::string label = tree.node(labelNode).text;
		const std::string pixels = tree.node(pixelsNode).text;
		if (label.size() < 2 || label[0] != '#') {
			std::fprintf(stderr, "[test_gea_virtual_list_main] %s row %d invalid label %s\n", phase, rowId, label.c_str());
			return false;
		}
		const int itemNumber = std::atoi(label.c_str() + 1);
		const int pixelTop = std::atoi(pixels.c_str());
		const int expectedTop = (itemNumber - 1) * row.layout.height;
		if (itemNumber <= 0 || pixelTop != expectedTop || row.style.pos_offsets[0] != expectedTop) {
			std::fprintf(stderr,
			             "[test_gea_virtual_list_main] %s row %d binding mismatch: label=%s pixels=%s top=%d rowH=%d expectedTop=%d\n",
			             phase,
			             rowId,
			             label.c_str(),
			             pixels.c_str(),
			             row.style.pos_offsets[0],
			             row.layout.height,
			             expectedTop);
			return false;
		}
		const int expectedY = list.layout.y - list.layout.scroll_y + expectedTop;
		if (row.layout.y != expectedY) {
			std::fprintf(stderr,
			             "[test_gea_virtual_list_main] %s row %d layout mismatch: y=%d expectedY=%d scroll=%d\n",
			             phase,
			             rowId,
			             row.layout.y,
			             expectedY,
			             list.layout.scroll_y);
			return false;
		}
		checked++;
	}
	if (checked < 8) {
		std::fprintf(stderr, "[test_gea_virtual_list_main] %s expected at least 8 visible rows, checked %d\n", phase, checked);
		return false;
	}
	return true;
}
}  // namespace

int main()
{
	using namespace gea::embedded::test;
	using gea::embedded::ui::NodeType;
	using gea::embedded::ui::Tree;

	resetNativeHost();
	setNativeDisplaySize(kViewportWidth, kViewportHeight);
	gea::framework::app::Application::init(kViewportWidth, kViewportHeight, kDevicePixelRatio);
	__gea_top_level();
	refresh();

	if (!expectContains(rootTextContent(), "rows x", "initial text", "test_gea_virtual_list_main")) {
		dumpTree("test_gea_virtual_list_main");
		return 1;
	}

	auto &tree = Tree::instance();
	const auto lists = nodesWithType(NodeType::VirtualList);
	std::vector<int> mountedLists;
	for (const int nodeId : lists) {
		if (isDescendantOf(nodeId, tree.mountedRoot())) mountedLists.push_back(nodeId);
	}
	if (mountedLists.size() != 1) {
		std::fprintf(stderr,
		             "[test_gea_virtual_list_main] expected one mounted virtual-list node, got %zu/%zu\n",
		             mountedLists.size(),
		             lists.size());
		dumpTree("test_gea_virtual_list_main");
		return 1;
	}

	const auto &list = tree.node(mountedLists[0]);
	if (list.layout.x != 0 || list.layout.y != 118 * 2 ||
	    list.layout.width != kViewportWidth || list.layout.height != kViewportHeight) {
		std::fprintf(stderr,
		             "[test_gea_virtual_list_main] expected list layout (0,236 %dx%d), got (%d,%d %dx%d)\n",
		             kViewportWidth,
		             kViewportHeight,
		             list.layout.x,
		             list.layout.y,
		             list.layout.width,
		             list.layout.height);
		dumpTree("test_gea_virtual_list_main");
		return 1;
	}

	if (!renderOnce()) {
		std::fprintf(stderr, "[test_gea_virtual_list_main] expected mounted root before render\n");
		dumpTree("test_gea_virtual_list_main");
		return 1;
	}

	if (displayNonzeroPixelCount() < kViewportWidth * 100) {
		std::fprintf(stderr,
		             "[test_gea_virtual_list_main] expected virtual-list render to paint the display, nonzero=%d\n",
		             displayNonzeroPixelCount());
		dumpTree("test_gea_virtual_list_main");
		return 1;
	}

	gea::framework::app::Application::frame(16);
	if (!verifyVisibleRowBindings(mountedLists[0], "initial")) {
		dumpTree("test_gea_virtual_list_main");
		return 1;
	}

	const int dragX = kViewportWidth / 2;
	const int dragY = list.layout.y + 500;
	dispatchTouch(gea::framework::events::TouchPhase::Down, true, dragX, dragY);
	gea::framework::app::Application::frame(32);
	for (int i = 1; i <= 6; ++i) {
		dispatchTouch(gea::framework::events::TouchPhase::Move, true, dragX, dragY - i * 83);
		gea::framework::app::Application::frame(32 + i * 16);
		if (!verifyVisibleRowBindings(mountedLists[0], "drag")) {
			dumpTree("test_gea_virtual_list_main");
			return 1;
		}
	}
	dispatchTouch(gea::framework::events::TouchPhase::Up, false, dragX, dragY - 6 * 83);

	return 0;
}

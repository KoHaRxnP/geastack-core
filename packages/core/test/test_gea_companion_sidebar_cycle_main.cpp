#include "native_test_harness.h"

#include "ui/node.h"
#include "ui/tree_internal.h"

#include <cstdio>
#include <string>
#include <vector>

extern void __gea_top_level();
extern bool gea_companion_fake_exec_saw(const char *needle);
extern void gea_companion_fake_exec_dump(const char *label);

namespace {

int contentRoot()
{
	auto &tree = gea::embedded::ui::Tree::instance();
	const auto nodes = gea::embedded::test::nodesWithClass("content");
	for (const int nodeId : nodes) {
		const int parent = tree.node(nodeId).parent;
		if (parent >= 0 && std::string(tree.tagName(parent)) != "template") return nodeId;
	}
	return -1;
}

int firstDescendantText(int root, const std::string &text)
{
	auto &tree = gea::embedded::ui::Tree::instance();
	for (int i = 0; i < tree.nodeCount(); ++i) {
		if (i != root && tree.containsNode(root, i) && tree.node(i).text == text) return i;
	}
	return -1;
}

bool expectVisibleText(int contentNode, const std::string &text)
{
	auto &tree = gea::embedded::ui::Tree::instance();
	gea::embedded::ui::NodeHandle(contentNode).style().width(632);
	gea::embedded::ui::NodeHandle(contentNode).style().height(548);
	tree.computeLayout(contentNode, 632, 548);
	const int textNode = firstDescendantText(contentNode, text);
	if (textNode >= 0 && tree.node(textNode).layout.width > 0 && tree.node(textNode).layout.height > 0) return true;
	std::fprintf(stderr, "[test_gea_companion_sidebar_cycle] missing visible text %s node=%d\n", text.c_str(), textNode);
	gea::embedded::test::dumpTree("test_gea_companion_sidebar_cycle");
	return false;
}

bool expectNoDescendantText(int contentNode, const std::string &text)
{
	const int textNode = firstDescendantText(contentNode, text);
	if (textNode < 0) return true;
	std::fprintf(stderr, "[test_gea_companion_sidebar_cycle] stale detail-pane text %s node=%d\n", text.c_str(), textNode);
	gea::embedded::test::dumpTree("test_gea_companion_sidebar_cycle");
	return false;
}

bool pressAndExpect(int contentNode, const std::string &sidebarLabel, const std::string &title)
{
	if (!gea::embedded::test::pressFirstText(sidebarLabel, true)) {
		std::fprintf(stderr, "[test_gea_companion_sidebar_cycle] failed to press %s\n", sidebarLabel.c_str());
		gea::embedded::test::dumpTree("test_gea_companion_sidebar_cycle");
		return false;
	}
	gea::embedded::test::refresh();
	return expectVisibleText(contentNode, title);
}

bool pressDescendantText(int root, const std::string &text)
{
	auto &tree = gea::embedded::ui::Tree::instance();
	gea::embedded::ui::NodeHandle(root).style().width(632);
	gea::embedded::ui::NodeHandle(root).style().height(548);
	tree.computeLayout(root, 632, 548);
	int textNode = -1;
	for (int i = 0; i < tree.nodeCount(); ++i) {
		if (i == root || !tree.containsNode(root, i) || tree.node(i).text != text) continue;
		if (tree.node(i).layout.width <= 0 || tree.node(i).layout.height <= 0) continue;
		textNode = i;
		break;
	}
	if (textNode < 0) {
		std::fprintf(stderr, "[test_gea_companion_sidebar_cycle] missing press target %s\n", text.c_str());
		gea::embedded::test::dumpTree("test_gea_companion_sidebar_cycle");
		return false;
	}
	if (gea::embedded::test::dispatchPress(textNode)) return true;
	std::fprintf(stderr, "[test_gea_companion_sidebar_cycle] press target did not dispatch %s node=%d\n", text.c_str(), textNode);
	gea::embedded::test::dumpTree("test_gea_companion_sidebar_cycle");
	return false;
}

}  // namespace

int main()
{
	using namespace gea::embedded::test;
	resetNativeHost();
	__gea_top_level();
	refresh();

	const int contentNode = contentRoot();
	if (contentNode < 0) {
		std::fprintf(stderr, "[test_gea_companion_sidebar_cycle] missing content root\n");
		dumpTree("test_gea_companion_sidebar_cycle");
		return 1;
	}

	if (!expectVisibleText(contentNode, "Watch Face")) return 2;
	if (!pressAndExpect(contentNode, "Firmware", "Firmware")) return 3;
	if (!expectVisibleText(contentNode, "Watch Firmware")) return 4;
	if (!pressAndExpect(contentNode, "USB Monitor", "USB Monitor")) return 20;
	if (!pressDescendantText(contentNode, "Start Monitor")) return 21;
	gea::embedded::test::refresh();
	if (!gea_companion_fake_exec_saw("usb-monitor-helper.mjs\" start")) {
		gea_companion_fake_exec_dump("missing start");
		gea::embedded::test::dumpTree("test_gea_companion_sidebar_cycle");
		return 22;
	}
	if (!pressDescendantText(contentNode, "Stop Monitor")) return 23;
	gea::embedded::test::refresh();
	if (!gea_companion_fake_exec_saw("usb-monitor-helper.mjs\" stop")) {
		gea_companion_fake_exec_dump("missing stop");
		return 24;
	}
	if (!pressAndExpect(contentNode, "Display & Brightness", "Display & Brightness")) return 5;
	if (!expectNoDescendantText(contentNode, "Watch Face")) return 15;
	if (!expectNoDescendantText(contentNode, "Digital")) return 16;
	if (!expectNoDescendantText(contentNode, "Launcher")) return 17;
	if (!pressAndExpect(contentNode, "Firmware", "Firmware")) return 6;
	if (!pressAndExpect(contentNode, "Notifications", "Notifications")) return 7;
	if (!pressAndExpect(contentNode, "Firmware", "Firmware")) return 8;
	if (!pressAndExpect(contentNode, "Date & Time", "Date & Time")) return 9;
	if (!pressAndExpect(contentNode, "Watch Face", "Watch Face")) return 10;
	if (!expectNoDescendantText(contentNode, "Display & Brightness")) return 18;
	if (!expectNoDescendantText(contentNode, "Brightness")) return 19;
	if (!pressAndExpect(contentNode, "Firmware", "Firmware")) return 11;
	if (!pressAndExpect(contentNode, "Battery", "Battery")) return 12;
	if (!pressAndExpect(contentNode, "Firmware", "Firmware")) return 13;
	if (!pressAndExpect(contentNode, "About", "About")) return 14;

	return 0;
}

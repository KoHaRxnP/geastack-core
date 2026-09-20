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

int ancestorWithClass(int nodeId, const std::string &className)
{
	auto &tree = gea::embedded::ui::Tree::instance();
	for (int current = nodeId; current >= 0 && current < tree.nodeCount(); current = tree.node(current).parent) {
		if (tree.hasClass(current, className)) return current;
	}
	return -1;
}

int descendantInputWithClass(int root, const std::string &className)
{
	auto &tree = gea::embedded::ui::Tree::instance();
	for (int i = 0; i < tree.nodeCount(); ++i) {
		if (i == root || !tree.containsNode(root, i)) continue;
		if (std::string(tree.tagName(i)) != "input") continue;
		if (tree.hasClass(i, className)) return i;
	}
	return -1;
}

bool dispatchInput(int inputNode)
{
	gea::framework::events::PointerEvent event{gea::framework::events::PointerEventType::Input};
	event.targetId = inputNode;
	return gea::embedded::ui::Tree::instance().dispatchEvent(event);
}

bool visibleRowsDoNotOverlap(int contentNode, const std::vector<std::string> &labels)
{
	auto &tree = gea::embedded::ui::Tree::instance();
	gea::embedded::ui::NodeHandle(contentNode).style().width(632);
	gea::embedded::ui::NodeHandle(contentNode).style().height(548);
	tree.computeLayout(contentNode, 632, 548);

	int previousBottom = -1;
	for (const std::string &label : labels) {
		const int labelNode = firstDescendantText(contentNode, label);
		if (labelNode < 0) {
			std::fprintf(stderr, "[test_gea_companion_native_controls] missing label %s\n", label.c_str());
			gea::embedded::test::dumpTree("test_gea_companion_native_controls");
			return false;
		}
		const int rowNode = ancestorWithClass(labelNode, "row");
		if (rowNode < 0) {
			std::fprintf(stderr, "[test_gea_companion_native_controls] label %s has no row ancestor\n", label.c_str());
			gea::embedded::test::dumpTree("test_gea_companion_native_controls");
			return false;
		}
		const auto &row = tree.node(rowNode);
		if (row.layout.width <= 0 || row.layout.height < 40) {
			std::fprintf(stderr,
			             "[test_gea_companion_native_controls] row for %s has bad layout %dx%d at y=%d\n",
			             label.c_str(),
			             row.layout.width,
			             row.layout.height,
			             row.layout.y);
			gea::embedded::test::dumpTree("test_gea_companion_native_controls");
			return false;
		}
		if (previousBottom > row.layout.y) {
			std::fprintf(stderr,
			             "[test_gea_companion_native_controls] row for %s overlaps previous row: prevBottom=%d y=%d\n",
			             label.c_str(),
			             previousBottom,
			             row.layout.y);
			gea::embedded::test::dumpTree("test_gea_companion_native_controls");
			return false;
		}
		previousBottom = row.layout.y + row.layout.height;
	}
	return true;
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
		std::fprintf(stderr, "[test_gea_companion_native_controls] missing content root\n");
		dumpTree("test_gea_companion_native_controls");
		return 1;
	}

	pumpFrame(1);
	pumpFrame(2);
	refresh();
	if (!pressFirstText("Display & Brightness", true)) return 2;
	refresh();
	if (!visibleRowsDoNotOverlap(contentNode,
	                             {"Brightness", "Auto-Brightness", "Always-On Display", "Wake on Wrist Raise"})) {
		return 3;
	}
	const int sliderNode = descendantInputWithClass(contentNode, "slider");
	if (sliderNode < 0) {
		std::fprintf(stderr, "[test_gea_companion_native_controls] missing brightness slider\n");
		dumpTree("test_gea_companion_native_controls");
		return 8;
	}
	if (std::string(gea::embedded::ui::Tree::instance().getAttribute(sliderNode, "value")) != "73%") {
		std::fprintf(stderr,
		             "[test_gea_companion_native_controls] expected device brightness to hydrate slider, got value=%s\n",
		             gea::embedded::ui::Tree::instance().getAttribute(sliderNode, "value"));
		gea_companion_fake_exec_dump("brightness hydrate");
		dumpTree("test_gea_companion_native_controls");
		return 9;
	}
	gea::embedded::ui::Tree::instance().setAttribute(sliderNode, "value", "45");
	if (!dispatchInput(sliderNode)) {
		std::fprintf(stderr, "[test_gea_companion_native_controls] slider input event did not dispatch\n");
		dumpTree("test_gea_companion_native_controls");
		return 10;
	}
	refresh();
	if (!gea_companion_fake_exec_saw("brightness 45")) {
		gea_companion_fake_exec_dump("missing brightness set");
		return 11;
	}

	if (!pressFirstText("Date & Time", true)) return 4;
	refresh();
	if (!visibleRowsDoNotOverlap(contentNode, {"Set Automatically", "24-Hour Time", "gea Watch Time"})) return 5;

	if (!pressFirstText("Notifications", true)) return 6;
	refresh();
	if (!visibleRowsDoNotOverlap(contentNode, {"Allow Notifications", "Show When Awake", "Vibrate"})) return 7;

	if (!pressFirstText("Firmware", true)) return 12;
	refresh();
	const int firmwareSwitchNode = descendantInputWithClass(contentNode, "switch");
	if (firmwareSwitchNode < 0) {
		std::fprintf(stderr, "[test_gea_companion_native_controls] missing firmware acknowledgement switch\n");
		dumpTree("test_gea_companion_native_controls");
		return 13;
	}
	gea::embedded::ui::Tree::instance().setAttribute(firmwareSwitchNode, "checked", "true");
	if (!dispatchInput(firmwareSwitchNode)) {
		std::fprintf(stderr, "[test_gea_companion_native_controls] switch input event did not dispatch\n");
		dumpTree("test_gea_companion_native_controls");
		return 14;
	}
	refresh();
	const int flashText = firstDescendantText(contentNode, "Flash Selected Firmware");
	const int flashButton = ancestorWithClass(flashText, "btn-row");
	if (flashButton < 0 || !gea::embedded::ui::Tree::instance().hasClass(flashButton, "btn-danger")) {
		std::fprintf(stderr, "[test_gea_companion_native_controls] switch input did not update dependent button class\n");
		dumpTree("test_gea_companion_native_controls");
		return 15;
	}

	return 0;
}

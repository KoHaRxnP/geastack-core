#include "native_test_harness.h"

#include "ui/node.h"
#include "ui/tree_internal.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

extern void __gea_top_level();

namespace {

void dumpCompanionTree()
{
	auto &tree = gea::embedded::ui::Tree::instance();
	for (int i = 0; i < tree.nodeCount(); ++i) {
		const auto &node = tree.node(i);
		std::fprintf(stderr,
		             "node[%d] tag=%s class=\"%s\" text=\"%s\" parent=%d first=%d next=%d layout=%dx%d display=%d\n",
		             i,
		             tree.tagName(i),
		             tree.className(i).c_str(),
		             node.text.c_str(),
		             node.parent,
		             node.first_child,
		             node.next_sibling,
		             node.layout.width,
		             node.layout.height,
		             static_cast<int>(node.style.display));
	}
}

std::vector<int> descendantsWithClass(int root, const std::string &className)
{
	auto &tree = gea::embedded::ui::Tree::instance();
	std::vector<int> matches;
	for (int i = 0; i < tree.nodeCount(); ++i) {
		if (i != root && tree.containsNode(root, i) && tree.hasClass(i, className)) {
			matches.push_back(i);
		}
	}
	return matches;
}

int firstDescendantWithText(int root, const std::string &text)
{
	auto &tree = gea::embedded::ui::Tree::instance();
	for (int i = 0; i < tree.nodeCount(); ++i) {
		if (i != root && tree.containsNode(root, i) && tree.node(i).text == text) {
			return i;
		}
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

bool expectVisibleDetailTitle(int contentNode, const std::string &title)
{
	auto &tree = gea::embedded::ui::Tree::instance();
	gea::embedded::ui::NodeHandle(contentNode).style().width(632);
	gea::embedded::ui::NodeHandle(contentNode).style().height(548);
	tree.computeLayout(contentNode, 632, 548);
	const int titleNode = firstDescendantWithText(contentNode, title);
	if (titleNode >= 0 && tree.node(titleNode).layout.width > 0 && tree.node(titleNode).layout.height > 0) return true;
	std::fprintf(stderr,
	             "[test_gea_companion_detailpane] %s text has no visible layout, node=%d\n",
	             title.c_str(),
	             titleNode);
	dumpCompanionTree();
	return false;
}

bool expectFirmwareDetailPageScrolls(int contentNode)
{
	auto &tree = gea::embedded::ui::Tree::instance();
	gea::embedded::ui::NodeHandle(contentNode).style().width(632);
	gea::embedded::ui::NodeHandle(contentNode).style().height(548);
	tree.computeLayout(contentNode, 632, 548);
	const int titleNode = firstDescendantWithText(contentNode, "Firmware");
	const int pageNode = ancestorWithClass(titleNode, "page");
	if (pageNode < 0) {
		std::fprintf(stderr, "[test_gea_companion_detailpane] missing firmware page ancestor\n");
		dumpCompanionTree();
		return false;
	}
	const auto &page = tree.node(pageNode);
	if (page.style.overflow == 2 && page.layout.scroll_content_height > page.layout.height) return true;
	std::fprintf(stderr,
	             "[test_gea_companion_detailpane] expected firmware page to scroll, overflow=%d content=%d height=%d\n",
	             static_cast<int>(page.style.overflow),
	             page.layout.scroll_content_height,
	             page.layout.height);
	dumpCompanionTree();
	return false;
}

}  // namespace

int main()
{
	using namespace gea::embedded::test;
	resetNativeHost();
	__gea_top_level();
	refresh();
	auto &tree = gea::embedded::ui::Tree::instance();
	if (std::getenv("GEA_DUMP_COMPANION_TREE")) {
		dumpCompanionTree();
	}

	if (nodesWithClass("content").empty()) {
		std::fprintf(stderr, "[test_gea_companion_detailpane] missing content node class\n");
		dumpCompanionTree();
		return 1;
	}
	const auto contentNodes = nodesWithClass("content");
	int contentNode = -1;
	for (const int candidate : contentNodes) {
		const int parent = tree.node(candidate).parent;
		if (parent >= 0 && std::string(tree.tagName(parent)) != "template") {
			contentNode = candidate;
			break;
		}
	}
	if (contentNode < 0) {
		std::fprintf(stderr, "[test_gea_companion_detailpane] missing detail content root\n");
		dumpCompanionTree();
		return 2;
	}
	const auto detailPages = descendantsWithClass(contentNode, "page");
	if (detailPages.empty()) {
		std::fprintf(stderr, "[test_gea_companion_detailpane] missing mounted page under detail content root\n");
		dumpCompanionTree();
		return 3;
	}
	if (!expectVisibleDetailTitle(contentNode, "Watch Face")) return 4;
	if (!pressFirstText("Display & Brightness", true)) {
		std::fprintf(stderr, "[test_gea_companion_detailpane] failed to press Display & Brightness\n");
		dumpCompanionTree();
		return 5;
	}
	refresh();
	if (!expectVisibleDetailTitle(contentNode, "Display & Brightness")) return 6;
	if (!pressFirstText("Apps", true)) {
		std::fprintf(stderr, "[test_gea_companion_detailpane] failed to press Apps\n");
		dumpCompanionTree();
		return 7;
	}
	refresh();
	if (!expectVisibleDetailTitle(contentNode, "Apps")) return 8;
	if (!pressFirstText("Firmware", true)) {
		std::fprintf(stderr, "[test_gea_companion_detailpane] failed to press Firmware\n");
		dumpCompanionTree();
		return 9;
	}
	refresh();
	if (!expectVisibleDetailTitle(contentNode, "Firmware")) return 10;
	if (!expectFirmwareDetailPageScrolls(contentNode)) return 11;
	return 0;
}

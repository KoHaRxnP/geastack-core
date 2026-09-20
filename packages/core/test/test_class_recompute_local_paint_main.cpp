#include "native_test_harness.h"

#include "ui/document.h"
#include "ui/node.h"
#include "ui/style.h"
#include "ui/tree_internal.h"

#include <cstdio>

namespace gea::framework::app::generated {
void drainMicrotasks() {}
}  // namespace gea::framework::app::generated

namespace gea::framework::graphics::generated {
void ensureLinked() {}
}  // namespace gea::framework::graphics::generated

int main()
{
	using namespace gea::embedded::test;
	using namespace gea::embedded::ui;

	resetNativeHost();
	setNativeDisplaySize(120, 120);
	StyleSheet::instance().clear();

	StyleSheet::instance().registerRule("paint-off", "position", "absolute");
	StyleSheet::instance().registerRule("paint-off", "left", "20px");
	StyleSheet::instance().registerRule("paint-off", "top", "20px");
	StyleSheet::instance().registerRule("paint-off", "width", "40px");
	StyleSheet::instance().registerRule("paint-off", "height", "40px");
	StyleSheet::instance().registerRule("paint-off", "background-color", "#111111");
	StyleSheet::instance().registerRule("paint-on", "position", "absolute");
	StyleSheet::instance().registerRule("paint-on", "left", "20px");
	StyleSheet::instance().registerRule("paint-on", "top", "20px");
	StyleSheet::instance().registerRule("paint-on", "width", "40px");
	StyleSheet::instance().registerRule("paint-on", "height", "40px");
	StyleSheet::instance().registerRule("paint-on", "background-color", "#ffffff");

	auto root = Document::instance().createView();
	auto box = Document::instance().createView();
	root.style().width(120);
	root.style().height(120);
	box.classList().set("paint-off");
	root.appendChild(box);
	Document::instance().mount(root, 120, 120);
	refresh();

	box.classList().set("paint-on");
	Tree &tree = Tree::instance();
	if (tree.displayListRebuildRequired()) {
		std::fprintf(stderr,
		             "[test_class_recompute_local_paint] background-only class recompute should not request a structural display-list rebuild\n");
		dumpTree("test_class_recompute_local_paint");
		return 1;
	}
	if (!tree.nodeDisplayCommandsDirty(box.id())) {
		std::fprintf(stderr,
		             "[test_class_recompute_local_paint] background-only class recompute should rerecord only the changed node commands\n");
		dumpTree("test_class_recompute_local_paint");
		return 1;
	}

	return 0;
}

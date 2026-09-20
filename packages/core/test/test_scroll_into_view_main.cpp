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

namespace {

using namespace gea::embedded::test;
using namespace gea::embedded::ui;

bool fail(const char *message)
{
	std::fprintf(stderr, "[test_scroll_into_view] %s\n", message);
	return false;
}

int makeDiv(int parent, const char *cls)
{
	const int id = Tree::instance().createView();
	Tree::instance().setTagName(id, "div");
	if (parent >= 0) NodeHandle(parent).appendChild(NodeHandle(id));
	if (cls) NodeHandle(id).classList().set(cls);
	return id;
}

}  // namespace

int main()
{
	resetNativeHost();
	StyleSheet::instance().clear();
	setViewportMetrics(200, 200, 1.0);

	StyleSheet::instance().registerRule("root", "width", "200");
	StyleSheet::instance().registerRule("root", "height", "200");
	StyleSheet::instance().registerRule("list", "display", "flex");
	StyleSheet::instance().registerRule("list", "flex-direction", "column");
	StyleSheet::instance().registerRule("list", "width", "188");
	StyleSheet::instance().registerRule("list", "height", "158");
	StyleSheet::instance().registerRule("list", "overflow-y", "scroll");
	StyleSheet::instance().registerRule("list", "overflow-x", "hidden");
	StyleSheet::instance().registerRule("list", "gap", "4");
	StyleSheet::instance().registerRule("row", "width", "188");
	StyleSheet::instance().registerRule("row", "height", "60");
	StyleSheet::instance().registerRule("row", "flex-shrink", "0");

	const int root = makeDiv(-1, "root");
	const int list = makeDiv(root, "list");
	int rows[5]{};
	for (int i = 0; i < 5; ++i) rows[i] = makeDiv(list, "row");

	Tree::instance().mount(root, 200, 200);
	Tree::instance().computeLayout(root, 200, 200);

	const Node &beforeList = Tree::instance().node(list);
	if (beforeList.layout.scroll_content_height <= beforeList.layout.height) {
		std::fprintf(stderr,
		             "[test_scroll_into_view] list is not scrollable: content=%d height=%d\n",
		             beforeList.layout.scroll_content_height,
		             beforeList.layout.height);
		return 1;
	}
	if (beforeList.layout.scroll_y != 0) return fail("expected initial scroll_y to be 0") ? 0 : 1;

	NodeHandle(rows[3]).scrollIntoView();
	const int afterScrollY = Tree::instance().node(list).layout.scroll_y;
	if (afterScrollY <= 0) {
		std::fprintf(stderr, "[test_scroll_into_view] expected scrollIntoView to scroll, got scroll_y=%d\n", afterScrollY);
		return 1;
	}

	Tree::instance().computeLayout(root, 200, 200);
	const Node &afterList = Tree::instance().node(list);
	const Node &target = Tree::instance().node(rows[3]);
	if (target.layout.y < afterList.layout.y || target.layout.y + target.layout.height > afterList.layout.y + afterList.layout.height) {
		std::fprintf(stderr,
		             "[test_scroll_into_view] target not visible: list=(%d,%d %dx%d scroll=%d) target=(%d,%d %dx%d)\n",
		             afterList.layout.x,
		             afterList.layout.y,
		             afterList.layout.width,
		             afterList.layout.height,
		             afterList.layout.scroll_y,
		             target.layout.x,
		             target.layout.y,
		             target.layout.width,
		             target.layout.height);
		return 1;
	}

	return 0;
}

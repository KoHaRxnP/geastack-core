// Background placement for a box that is not a slot of the tree.
//
// ViewRenderer::recordBox() accepts any Node, and the renderer tests record
// Nodes built on the stack. For such a node ViewGeometry::nodeIndex() is -1,
// and StyleValues::backgroundPlacement() used to index treeState().nodes[-1]
// regardless: it read the bytes before the heap-allocated TreeState as a Node
// and passed that garbage ComputedStyle to computedBorderWidth() -> rstyle(),
// whose rare_style handle then indexed the RareStyle pool. Whether that
// segfaulted depended on the address layout, so the css-3d-cube pipeline
// crashed in roughly one run of six under ASLR and never under gdb.
//
// run-detached-background-placement.sh builds this with ASan + UBSan and no
// recovery, so the out-of-bounds read fails every run, not one in six; the
// inset expectations below also fail without a sanitizer whenever the bytes
// before the tree do not happen to hold the detached box's own border.

#include "native_test_harness.h"

#include "display.h"
#include "ui/internal.h"
#include "ui/node.h"
#include "ui/style_values.h"
#include "ui/tree_internal.h"
#include "ui/tree_state.h"

#include <cstdio>

namespace gea::framework::app::generated {
void drainMicrotasks() {}
}  // namespace gea::framework::app::generated

namespace gea::framework::graphics::generated {
void ensureLinked() {}
}  // namespace gea::framework::graphics::generated

namespace {

using namespace gea::embedded::ui;

bool expectEqual(int actual, int expected, const char *label)
{
	if (actual == expected) return true;
	std::fprintf(stderr, "[test_detached_background_placement] %s expected %d, got %d\n", label, expected, actual);
	return false;
}

void styleGridBox(ComputedStyle &style, int border, int padding)
{
	style.has_bg = 1;
	style.bg_color = 0x0000;
	style.bg_alpha = 255;
	setComputedBorderWidth(style, -1, border, nullptr);
	for (int i = 0; i < 4; ++i) style.padding[i] = padding;
	auto &rare = rstyleMut(style);
	rare.bg_grid_axes = 3;
	rare.bg_grid_color = 0xffff;
	rare.bg_grid_alpha = 255;
	rare.bg_grid_step_x = 12;
	rare.bg_grid_step_y = 12;
	rare.bg_grid_line_x = 1;
	rare.bg_grid_line_y = 1;
}

bool expectDetachedPlacement()
{
	// A populated tree, so nodes[-1] is not trivially the first slot of an empty one.
	auto &tree = Tree::instance();
	for (int i = 0; i < 4; ++i) tree.createView();

	Node box{};
	box.type = NodeType::View;
	box.layout.x = 8;
	box.layout.y = 10;
	box.layout.width = 48;
	box.layout.height = 40;
	styleGridBox(box.mutableStyle(), 3, 5);

	// Default background-origin is padding-box: the area is inset by the
	// detached box's OWN border, and there is no scroll state to apply.
	const auto placement = StyleValues::backgroundPlacement(box.computedStyle(), -1, 0, box.layout.x, box.layout.y,
	                                                        box.layout.width, box.layout.height);
	if (!expectEqual(placement.origin, 1, "detached default origin")) return false;
	if (!expectEqual(placement.areaX, 8 + 3, "detached padding-box x")) return false;
	if (!expectEqual(placement.areaY, 10 + 3, "detached padding-box y")) return false;
	if (!expectEqual(placement.areaWidth, 48 - 6, "detached padding-box width")) return false;
	if (!expectEqual(placement.areaHeight, 40 - 6, "detached padding-box height")) return false;

	// The full path from the crash report: recordBox -> recordBackgroundGrid
	// -> backgroundPlacement -> computedBorderWidth -> rstyle.
	gea::platform::display::Display::clearNoFlush();
	DisplayList::instance().clear();
	ViewRenderer::recordBox(box);
	DisplayList::instance().replay();
	if (gea::embedded::test::displayPixelAt(20, 30) == 0) {
		std::fprintf(stderr, "[test_detached_background_placement] detached grid did not paint its vertical line\n");
		return false;
	}
	if (gea::embedded::test::displayPixelAt(24, 26) != 0) {
		std::fprintf(stderr, "[test_detached_background_placement] detached grid painted an open cell\n");
		return false;
	}
	DisplayList::instance().clear();
	return true;
}

bool expectTreeNodePlacement()
{
	// A box that IS in the tree keeps using its own slot for the insets, even
	// when the style it is asked about (a propagated canvas background) is
	// another node's.
	auto &tree = Tree::instance();
	const int id = tree.createView();
	setComputedBorderWidth(tree.node(id).mutableStyle(), -1, 6, nullptr);
	Node source{};
	styleGridBox(source.mutableStyle(), 1, 0);
	const auto placement = StyleValues::backgroundPlacement(source.computedStyle(), id, 0, 0, 0, 100, 80);
	if (!expectEqual(placement.areaX, 6, "tree node padding-box x")) return false;
	if (!expectEqual(placement.areaWidth, 100 - 12, "tree node padding-box width")) return false;
	if (!expectEqual(placement.areaHeight, 80 - 12, "tree node padding-box height")) return false;
	return true;
}

}  // namespace

int main()
{
	gea::embedded::test::resetNativeHost();
	if (!expectDetachedPlacement()) return 1;
	if (!expectTreeNodePlacement()) return 1;
	std::printf("detached background placement ok\n");
	return 0;
}

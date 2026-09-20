#include "native_test_harness.h"

#include <csignal>
#include <cstdio>
#include <cstdlib>

#include <unistd.h>

#include "ui/dirty_regions.h"
#include "display.h"
#include "host/timers.h"
#include "ui/internal.h"
#include "ui/tree_internal.h"

extern void __gea_top_level();
namespace gea::framework::app::generated {
void drainMicrotasks();
}  // namespace gea::framework::app::generated

namespace {
constexpr const char *kTestName = "test_gea_analog_clock_main";

bool expect_transformed_pill_not_recorded_as_ellipse()
{
	using namespace gea::embedded::ui;

	gea::embedded::test::resetNativeHost();
	gea::platform::display::Display::clearNoFlush();
	DisplayList::instance().clear();

	Tree &tree = Tree::instance();
	const int pill = tree.createView();
	Node &node = tree.node(pill);
	node.layout.x = 100;
	node.layout.y = 60;
	node.layout.width = 12;
	node.layout.height = 120;
	node.style.has_bg = 1;
	node.style.bg_color = 0xffff;
	node.style.bg_alpha = 255;
	rstyleMut(node.style).transform_rotate = 1;
	for (int i = 0; i < 4; i++) node.style.border_radius[i] = 6;

	ViewRenderer::recordBox(node);
	DisplayList::instance().replay();

	const auto edgePixel = gea::embedded::test::displayPixelAt(101, 70);
	if (edgePixel != 0xffff) {
		std::fprintf(stderr,
		             "[%s] transformed pill should keep its straight sides instead of becoming a giant ellipse, edge pixel=0x%04x\n",
		             kTestName,
		             edgePixel);
		return false;
	}
	return true;
}

void fail_on_alarm(int /*signal*/)
{
	std::fputs("[test_gea_analog_clock_main] timed out while driving clock frames\n", stderr);
	std::_Exit(124);
}

void drain_refresh()
{
	gea::framework::app::generated::drainMicrotasks();
	gea::embedded::test::refresh();
}

void pump(double timestampMs)
{
	gea::framework::app::generated::drainMicrotasks();
	gea::host::runAnimationFrameCallbacks(timestampMs);
	gea::framework::app::generated::drainMicrotasks();
	gea::embedded::test::pumpFrame(timestampMs);
	gea::framework::app::generated::drainMicrotasks();
	gea::embedded::test::refresh();
}

}  // namespace

int main()
{
	std::signal(SIGALRM, fail_on_alarm);
	alarm(5);

	if (!expect_transformed_pill_not_recorded_as_ellipse()) return 1;

	gea::embedded::test::resetNativeHost();
	__gea_top_level();
	drain_refresh();

	const auto text = gea::embedded::test::rootTextContent();
	if (!gea::embedded::test::expectContains(text, "Analog Clock", "initial text", kTestName)) {
		gea::embedded::test::dumpTree(kTestName);
		return 1;
	}
	if (!gea::embedded::test::expectContains(text, "12", "initial text", kTestName)) return 1;

	auto &tree = gea::embedded::ui::Tree::instance();
	const auto faces = gea::embedded::test::nodesWithClass("clock-face");
	const auto shadows = gea::embedded::test::nodesWithClass("clock-shadow");
	const auto hours = gea::embedded::test::nodesWithClass("clock-hour-hand");
	const auto minutes = gea::embedded::test::nodesWithClass("clock-minute-hand");
	const auto seconds = gea::embedded::test::nodesWithClass("clock-second-hand");
	if (faces.size() != 1 || shadows.size() != 1 || hours.size() != 1 || minutes.size() != 1 || seconds.size() != 1) {
		std::fprintf(stderr,
		             "[%s] expected one clock face, shadow, and three hands, got face=%zu shadow=%zu hour=%zu minute=%zu second=%zu\n",
		             kTestName,
		             faces.size(),
		             shadows.size(),
		             hours.size(),
		             minutes.size(),
		             seconds.size());
		gea::embedded::test::dumpTree(kTestName);
		return 1;
	}
	const int handNodes[3] = {hours[0], minutes[0], seconds[0]};
	for (int hand : handNodes) {
		const auto &node = tree.node(hand);
		for (int radius : node.style.border_radius) {
			if (radius != 0) {
				std::fprintf(stderr,
				             "[%s] clock hands should be straight line-like rectangles, got border-radius=%d on node %d\n",
				             kTestName,
				             radius,
				             hand);
				gea::embedded::test::dumpTree(kTestName);
				return 1;
			}
		}
	}

	pump(1000);
	const int beforeRotate = rstyle(tree.node(seconds[0]).style).transform_rotate;
	gea::framework::app::generated::drainMicrotasks();
	gea::host::runAnimationFrameCallbacks(8500);
	gea::framework::app::generated::drainMicrotasks();
	if (tree.displayListRebuildRequired()) {
		std::fprintf(stderr,
		             "[%s] expected hand transform updates to stay local, but display list rebuild was requested\n",
		             kTestName);
		gea::embedded::test::dumpTree(kTestName);
		return 1;
	}
	if (!tree.nodeDisplayCommandsDirty(seconds[0])) {
		std::fprintf(stderr,
		             "[%s] expected hand transform update to rerecord the second-hand commands\n",
		             kTestName);
		gea::embedded::test::dumpTree(kTestName);
		return 1;
	}
	gea::embedded::test::pumpFrame(8500);
	gea::framework::app::generated::drainMicrotasks();
	gea::embedded::test::refresh();
	const int flushRectsBeforeHandFrame = gea::embedded::test::flushRectCount();
	const int flushPixelsBeforeHandFrame = gea::embedded::test::flushPixelCount();
	gea::framework::app::generated::drainMicrotasks();
	gea::host::runAnimationFrameCallbacks(8516);
	gea::framework::app::generated::drainMicrotasks();
	gea::embedded::test::pumpFrame(8516);
	gea::framework::app::generated::drainMicrotasks();
	gea::embedded::test::refresh();
	const int handFrameFlushRects = gea::embedded::test::flushRectCount() - flushRectsBeforeHandFrame;
	const int handFrameFlushPixels = gea::embedded::test::flushPixelCount() - flushPixelsBeforeHandFrame;
	const auto &shadow = tree.node(shadows[0]);
	const int shadowArea = shadow.layout.width * shadow.layout.height;
	if (handFrameFlushRects <= 0 || handFrameFlushRects > gea::embedded::ui::DirtyRegions::kMaxRects / 2) {
		std::fprintf(stderr,
		             "[%s] expected a transform-only hand frame to coalesce transfer regions, rects=%d max=%d\n",
		             kTestName,
		             handFrameFlushRects,
		             gea::embedded::ui::DirtyRegions::kMaxRects);
		gea::embedded::test::dumpTree(kTestName);
		return 1;
	}
	if (handFrameFlushPixels <= 0 || static_cast<long long>(handFrameFlushPixels) * 2 >= shadowArea) {
		std::fprintf(stderr,
		             "[%s] expected a transform-only hand frame to avoid flushing the full clock body, pixels=%d shadowArea=%d\n",
		             kTestName,
		             handFrameFlushPixels,
		             shadowArea);
		gea::embedded::test::dumpTree(kTestName);
		return 1;
	}
	pump(3000);
	const int afterRotate = rstyle(tree.node(seconds[0]).style).transform_rotate;
	if (afterRotate == beforeRotate) {
		std::fprintf(stderr,
		             "[%s] expected second hand rotation to update, before=%d after=%d\n",
		             kTestName,
		             beforeRotate,
		             afterRotate);
		gea::embedded::test::dumpTree(kTestName);
		return 1;
	}

	alarm(0);
	return 0;
}

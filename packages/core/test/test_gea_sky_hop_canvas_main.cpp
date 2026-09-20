#include "native_test_harness.h"

#include "ui/canvas_element.h"
#include "ui/document.h"
#include "ui/node_model.h"
#include "ui/tree_internal.h"

#include <cstdio>

extern void __gea_top_level();

int main()
{
	using namespace gea::embedded::test;
	resetNativeHost();
	gea::embedded::ui::canvasPerfStatsReset();
	gea::embedded::ui::Document::instance().ensureAppRoot("app");
	__gea_top_level();
	refresh();

	const auto objectTextNodes = nodesWithText("[object Object]", true);
	const auto canvasNodes = nodesWithType(gea::embedded::ui::NodeType::Canvas);
	if (!objectTextNodes.empty() || canvasNodes.empty()) {
		std::fprintf(stderr,
		             "[test_gea_sky_hop_canvas_main] expected mounted canvas, got objectText=%zu canvas=%zu\n",
		             objectTextNodes.size(),
		             canvasNodes.size());
		dumpTree("test_gea_sky_hop_canvas_main");
		return 1;
	}
	const auto &canvasNode = gea::embedded::ui::Tree::instance().node(canvasNodes.front());
	if (canvasNode.layout.width <= 0 || canvasNode.layout.height <= 0) {
		std::fprintf(stderr,
		             "[test_gea_sky_hop_canvas_main] expected positive canvas layout, got %dx%d\n",
		             canvasNode.layout.width,
		             canvasNode.layout.height);
		dumpTree("test_gea_sky_hop_canvas_main");
		return 1;
	}

	if (displayNonzeroPixelCount() <= 0) {
		std::fprintf(stderr, "[test_gea_sky_hop_canvas_main] startup canvas draws left the display blank\n");
		dumpTree("test_gea_sky_hop_canvas_main");
		return 1;
	}

	gea::embedded::ui::canvasPerfStatsReset();
	pumpFrame(16.67);
	pumpFrame(33.34);

	if (imageLoadCount() != 6) {
		std::fprintf(stderr,
		             "[test_gea_sky_hop_canvas_main] expected 6 image loads, got %d\n",
		             imageLoadCount());
		dumpTree("test_gea_sky_hop_canvas_main");
		return 1;
	}
	if (displayNonzeroPixelCount() <= 0) {
		std::fprintf(stderr, "[test_gea_sky_hop_canvas_main] RAF canvas draws left the display blank\n");
		dumpTree("test_gea_sky_hop_canvas_main");
		return 1;
	}
	auto &tree = gea::embedded::ui::Tree::instance();
	if (!tree.hasListenersForType("pointerdown") || !tree.hasEventListener(tree.mountedRoot())) {
		std::fprintf(stderr, "[test_gea_sky_hop_canvas_main] pointer listeners were not bound to the mounted root\n");
		dumpTree("test_gea_sky_hop_canvas_main");
		return 1;
	}
	constexpr std::uint16_t waterPixel = ((42 & 0xf8) << 8) | ((153 & 0xfc) << 3) | (210 >> 3);
	const std::uint16_t floorPixel = displayPixelAt(40, 370);
	if (floorPixel == waterPixel) {
		std::fprintf(stderr,
		             "[test_gea_sky_hop_canvas_main] expected floor sprite at (40,370), got background pixel %u\n",
		             static_cast<unsigned>(floorPixel));
		dumpTree("test_gea_sky_hop_canvas_main");
		return 1;
	}

	const std::uint16_t inactiveLeftButtonPixel = displayPixelAt(20, 430);
	dispatchTouch(gea::framework::events::TouchPhase::Down, true, 43, 457);
	gea::embedded::ui::canvasPerfStatsReset();
	pumpFrame(50.01);
	const std::uint16_t activeLeftButtonPixel = displayPixelAt(20, 430);
	if (activeLeftButtonPixel == inactiveLeftButtonPixel) {
		std::fprintf(stderr,
		             "[test_gea_sky_hop_canvas_main] left control touch did not change rendered button pixel (%u)\n",
		             static_cast<unsigned>(activeLeftButtonPixel));
		dumpTree("test_gea_sky_hop_canvas_main");
		return 1;
	}

	dispatchTouch(gea::framework::events::TouchPhase::Up, false, 43, 457);
	pumpFrame(66.68);

	const std::uint16_t inactiveRightButtonPixel = displayPixelAt(90, 430);
	dispatchTouch(gea::framework::events::TouchPhase::Down, true, 117, 457);
	pumpFrame(83.35);
	const std::uint16_t activeRightButtonPixel = displayPixelAt(90, 430);
	if (activeRightButtonPixel == inactiveRightButtonPixel) {
		std::fprintf(stderr, "[test_gea_sky_hop_canvas_main] right control touch did not activate the rendered button\n");
		dumpTree("test_gea_sky_hop_canvas_main");
		return 1;
	}

	auto dispatchSecondaryTouch = [](gea::framework::events::TouchPhase phase, bool touching, int x, int y) {
		gea::framework::events::Event event{};
		event.type = gea::framework::events::EventType::Touch;
		event.touchPhase = phase;
		event.touching = touching;
		event.x = x;
		event.y = y;
		event.pointerId = 1;
		gea::framework::events::TouchRuntime::dispatchEvent(event);
	};

	const std::uint16_t inactiveJumpButtonPixel = displayPixelAt(310, 430);
	dispatchSecondaryTouch(gea::framework::events::TouchPhase::Down, true, 354, 457);
	pumpFrame(100.02);
	const std::uint16_t activeJumpButtonPixel = displayPixelAt(310, 430);
	if (activeJumpButtonPixel == inactiveJumpButtonPixel) {
		std::fprintf(stderr,
		             "[test_gea_sky_hop_canvas_main] secondary jump touch did not activate while right control was held\n");
		dumpTree("test_gea_sky_hop_canvas_main");
		return 1;
	}
	dispatchSecondaryTouch(gea::framework::events::TouchPhase::Up, false, 354, 457);
	pumpFrame(116.69);
	const std::uint16_t rightAfterJumpReleasePixel = displayPixelAt(90, 430);
	if (rightAfterJumpReleasePixel != activeRightButtonPixel) {
		std::fprintf(stderr,
		             "[test_gea_sky_hop_canvas_main] releasing secondary jump touch cancelled held right control\n");
		dumpTree("test_gea_sky_hop_canvas_main");
		return 1;
	}

	dispatchTouch(gea::framework::events::TouchPhase::Up, false, 117, 457);
	pumpFrame(133.36);
	const std::uint16_t inactiveRightAfterReleasePixel = displayPixelAt(90, 430);
	dispatchTouch(gea::framework::events::TouchPhase::Down, true, 354, 457);
	pumpFrame(150.03);
	if (displayPixelAt(90, 430) != inactiveRightAfterReleasePixel) {
		std::fprintf(stderr, "[test_gea_sky_hop_canvas_main] jump-only touch activated right control\n");
		dumpTree("test_gea_sky_hop_canvas_main");
		return 1;
	}
	dispatchTouch(gea::framework::events::TouchPhase::Up, false, 354, 457);
	pumpFrame(166.70);

	return 0;
}

#include "native_test_harness.h"

#include "app.h"
#include "display.h"
#include "ui/tree_internal.h"

#include <cstdio>

extern void __gea_top_level();

namespace {
constexpr int kViewportWidth = 1170;
constexpr int kViewportHeight = 2532;
constexpr int kDevicePixelRatio = 3;

int visibleBrickPixels()
{
	using gea::embedded::ui::Tree;
	using namespace gea::embedded::test;

	auto &tree = Tree::instance();
	const auto backgroundPixel = displayPixelAt(5, 5);
	int visiblePixels = 0;
	for (int brickId : nodesWithClass("brick")) {
		const auto &brick = tree.node(brickId);
		if (brick.style.opacity == 0 || brick.style.display == 1) continue;
		const auto pixel = displayPixelAt(brick.layout.x + brick.layout.width / 2,
		                                  brick.layout.y + brick.layout.height / 2);
		if (pixel != backgroundPixel && pixel != 0) visiblePixels++;
	}
	return visiblePixels;
}

bool expectVisiblePaddle(const char *phase)
{
	using gea::embedded::ui::Tree;
	using namespace gea::embedded::test;

	auto &tree = Tree::instance();
	const auto paddles = nodesWithClass("paddle");
	if (paddles.size() != 1) {
		std::fprintf(stderr, "[test_gea_tilt_breakout_final_loss_main] expected one paddle %s, got %zu\n", phase, paddles.size());
		dumpTree("test_gea_tilt_breakout_final_loss_main");
		return false;
	}
	const auto &paddle = tree.node(paddles[0]);
	const auto backgroundPixel = displayPixelAt(5, 5);
	const auto pixel = displayPixelAt(paddle.layout.x + paddle.layout.width / 2,
	                                  paddle.layout.y + paddle.layout.height / 2);
	if (pixel == backgroundPixel || pixel == 0) {
		std::fprintf(stderr,
		             "[test_gea_tilt_breakout_final_loss_main] expected paddle to paint %s, got pixel=%u background=%u\n",
		             phase,
		             static_cast<unsigned>(pixel),
		             static_cast<unsigned>(backgroundPixel));
		dumpTree("test_gea_tilt_breakout_final_loss_main");
		return false;
	}
	return true;
}
}

int main()
{
	using namespace gea::embedded::test;
	using gea::embedded::ui::Tree;

	resetNativeHost();
	setNativeDisplaySize(kViewportWidth, kViewportHeight);
	gea::framework::app::Application::init(kViewportWidth, kViewportHeight, kDevicePixelRatio);
	__gea_top_level();
	refresh();

	auto text = rootTextContent();
	if (!expectContains(text, "Score 0   Lives 0", "text after final loss", "test_gea_tilt_breakout_final_loss_main")) {
		dumpTree("test_gea_tilt_breakout_final_loss_main");
		return 1;
	}

	const auto bricks = nodesWithClass("brick");
	if (bricks.size() != 24) {
		std::fprintf(stderr, "[test_gea_tilt_breakout_final_loss_main] expected 24 bricks after final loss reset, got %zu\n", bricks.size());
		dumpTree("test_gea_tilt_breakout_final_loss_main");
		return 1;
	}
	const int initialPausePaintedBricks = visibleBrickPixels();
	if (initialPausePaintedBricks < 20 || !expectVisiblePaddle("before final-loss pause clear")) {
		std::fprintf(stderr,
		             "[test_gea_tilt_breakout_final_loss_main] expected initial final-loss pause to be visible, visible_bricks=%d\n",
		             initialPausePaintedBricks);
		return 1;
	}

	gea::platform::display::Display::clearNoFlush();
	if (!Tree::instance().refreshRequired()) {
		std::fprintf(stderr, "[test_gea_tilt_breakout_final_loss_main] expected display dirty canvas to require refresh\n");
		return 1;
	}
	pumpFrame(300);
	text = rootTextContent();
	if (!expectContains(text, "Score 0   Lives 0", "text during final-loss pause", "test_gea_tilt_breakout_final_loss_main")) {
		dumpTree("test_gea_tilt_breakout_final_loss_main");
		return 1;
	}
	const int pausePaintedBricks = visibleBrickPixels();
	if (pausePaintedBricks < 20 || !expectVisiblePaddle("during final-loss pause")) {
		std::fprintf(stderr,
		             "[test_gea_tilt_breakout_final_loss_main] expected bricks and paddle to repaint while final-loss pause is waiting, visible_bricks=%d\n",
		             pausePaintedBricks);
		dumpTree("test_gea_tilt_breakout_final_loss_main");
		return 1;
	}

	gea::platform::display::Display::clearNoFlush();
	pumpFrame(1000);
	pumpFrame(1016);
	text = rootTextContent();
	if (!expectContains(text, "Score 0   Lives 3", "text after final-loss restart", "test_gea_tilt_breakout_final_loss_main")) {
		dumpTree("test_gea_tilt_breakout_final_loss_main");
		return 1;
	}

	const int paintedBricks = visibleBrickPixels();
	if (paintedBricks < 20 || !expectVisiblePaddle("after final-loss restart")) {
		std::fprintf(stderr,
		             "[test_gea_tilt_breakout_final_loss_main] expected bricks and paddle to be visible after final-loss restart, visible_bricks=%d\n",
		             paintedBricks);
		dumpTree("test_gea_tilt_breakout_final_loss_main");
		return 1;
	}

	return 0;
}

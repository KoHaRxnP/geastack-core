#include "native_test_harness.h"

#include "app.h"
#include "ui/tree_internal.h"

#include <cstdio>

extern void __gea_top_level();

namespace {
constexpr int kViewportWidth = 1170;
constexpr int kViewportHeight = 2532;
constexpr int kDevicePixelRatio = 3;
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
	if (!expectContains(text, "Score 0   Lives 3", "initial text", "test_gea_tilt_breakout_main")) return 1;

	if (nodesWithClass("brick").size() != 24) {
		std::fprintf(stderr, "[test_gea_tilt_breakout_main] expected 24 visible bricks, got %zu\n", nodesWithClass("brick").size());
		dumpTree("test_gea_tilt_breakout_main");
		return 1;
	}
	if (nodesWithClass("paddle").size() != 1 || nodesWithClass("ball").size() != 1) {
		std::fprintf(stderr, "[test_gea_tilt_breakout_main] expected paddle and ball nodes\n");
		dumpTree("test_gea_tilt_breakout_main");
		return 1;
	}

	auto &tree = Tree::instance();
	const auto roots = nodesWithClass("breakout-app");
	const auto fields = nodesWithClass("brick-field");
	if (roots.empty() || fields.empty()) {
		std::fprintf(stderr, "[test_gea_tilt_breakout_main] expected breakout root and brick field nodes\n");
		dumpTree("test_gea_tilt_breakout_main");
		return 1;
	}
	const auto &root = tree.node(roots[0]);
	const auto &field = tree.node(fields[0]);
	if (root.layout.width != kViewportWidth || root.layout.height != kViewportHeight ||
	    field.layout.width != kViewportWidth || field.layout.height != kViewportHeight) {
		std::fprintf(stderr,
		             "[test_gea_tilt_breakout_main] expected full viewport layout root=%dx%d field=%dx%d, got root=%dx%d field=%dx%d\n",
		             kViewportWidth,
		             kViewportHeight,
		             kViewportWidth,
		             kViewportHeight,
		             root.layout.width,
		             root.layout.height,
		             field.layout.width,
		             field.layout.height);
		dumpTree("test_gea_tilt_breakout_main");
		return 1;
	}
	const auto bricks = nodesWithClass("brick");
	const auto &firstBrick = tree.node(bricks[0]);
	const auto &secondBrick = tree.node(bricks[1]);
	const auto &lastBrick = tree.node(bricks[5]);
	if (secondBrick.layout.x <= firstBrick.layout.x + firstBrick.layout.width ||
	    lastBrick.layout.x + lastBrick.layout.width < kViewportWidth * 85 / 100) {
		std::fprintf(stderr,
		             "[test_gea_tilt_breakout_main] expected first brick row to scale across the viewport without overlap: first=(%d,%d %dx%d) second=(%d,%d %dx%d) last=(%d,%d %dx%d)\n",
		             firstBrick.layout.x,
		             firstBrick.layout.y,
		             firstBrick.layout.width,
		             firstBrick.layout.height,
		             secondBrick.layout.x,
		             secondBrick.layout.y,
		             secondBrick.layout.width,
		             secondBrick.layout.height,
		             lastBrick.layout.x,
		             lastBrick.layout.y,
		             lastBrick.layout.width,
		             lastBrick.layout.height);
		dumpTree("test_gea_tilt_breakout_main");
		return 1;
	}
	const auto &paddle = tree.node(nodesWithClass("paddle")[0]);
	const auto &ball = tree.node(nodesWithClass("ball")[0]);
	if (paddle.layout.y < kViewportHeight * 75 / 100 || ball.layout.y < kViewportHeight * 55 / 100) {
		std::fprintf(stderr,
		             "[test_gea_tilt_breakout_main] expected paddle and ball to scale into the full viewport: paddle=(%d,%d %dx%d) ball=(%d,%d %dx%d)\n",
		             paddle.layout.x,
		             paddle.layout.y,
		             paddle.layout.width,
		             paddle.layout.height,
		             ball.layout.x,
		             ball.layout.y,
		             ball.layout.width,
		             ball.layout.height);
		dumpTree("test_gea_tilt_breakout_main");
		return 1;
	}
	const auto backgroundPixel = displayPixelAt(5, 5);
	const auto brickPixel = displayPixelAt(firstBrick.layout.x + firstBrick.layout.width / 2,
	                                      firstBrick.layout.y + firstBrick.layout.height / 2);
	if (brickPixel == backgroundPixel || brickPixel == 0) {
		std::fprintf(stderr,
		             "[test_gea_tilt_breakout_main] expected first brick to paint a visible color, got brick=%u background=%u\n",
		             static_cast<unsigned>(brickPixel),
		             static_cast<unsigned>(backgroundPixel));
		dumpTree("test_gea_tilt_breakout_main");
		return 1;
	}
	auto visibleBrickPixels = [&]() {
		int count = 0;
		for (int brickId : nodesWithClass("brick")) {
			const auto &brick = tree.node(brickId);
			if (brick.layout.width <= 0 || brick.layout.height <= 0 || brick.style.opacity == 0) continue;
			const auto pixel = displayPixelAt(brick.layout.x + brick.layout.width / 2,
			                                  brick.layout.y + brick.layout.height / 2);
			if (pixel != backgroundPixel && pixel != 0) count++;
		}
		return count;
	};
	if (visibleBrickPixels() < 20) {
		std::fprintf(stderr, "[test_gea_tilt_breakout_main] expected most bricks to paint initially, got %d visible centers\n", visibleBrickPixels());
		dumpTree("test_gea_tilt_breakout_main");
		return 1;
	}

	pumpFrame(16);
	text = rootTextContent();
	if (!expectContains(text, "Score 0   Lives 3", "frame text", "test_gea_tilt_breakout_main")) return 1;

	bool sawDestroyedBrick = false;
	for (int frame = 2; frame <= 140; ++frame) {
		pumpFrame(frame * 16.0);
		text = rootTextContent();
		if (text.find("Score 10   Lives 3") != std::string::npos) {
			sawDestroyedBrick = true;
			break;
		}
	}
	if (!sawDestroyedBrick) {
		std::fprintf(stderr, "[test_gea_tilt_breakout_main] expected a brick collision within 140 frames, got:\n%s\n", text.c_str());
		dumpTree("test_gea_tilt_breakout_main");
		return 1;
	}
	if (visibleBrickPixels() < 12) {
		std::fprintf(stderr,
		             "[test_gea_tilt_breakout_main] expected surviving bricks to remain painted after animation, got %d visible centers; first brick has_bg=%d bg=%u opacity=%u\n",
		             visibleBrickPixels(),
		             static_cast<int>(firstBrick.style.has_bg),
		             static_cast<unsigned>(firstBrick.style.bg_color),
		             static_cast<unsigned>(firstBrick.style.opacity));
		dumpTree("test_gea_tilt_breakout_main");
		return 1;
	}

	return 0;
}

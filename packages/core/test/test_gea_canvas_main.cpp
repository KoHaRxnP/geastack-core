#include "native_test_harness.h"

#include <cstdio>

#include "ui/node_model.h"

extern void __gea_top_level();

int main()
{
	using namespace gea::embedded::test;
	resetNativeHost();
	__gea_top_level();
	refresh();

	const auto canvases = nodesWithType(gea::embedded::ui::NodeType::Canvas);
	if (canvases.size() != 1) {
		std::fprintf(stderr, "[test_gea_canvas_main] expected one canvas node, got %zu\n", canvases.size());
		dumpTree("test_gea_canvas_main");
		return 1;
	}

	if (nodesWithClass("canvas-basic").size() != 1) {
		std::fprintf(stderr, "[test_gea_canvas_main] expected canvas-basic class\n");
		dumpTree("test_gea_canvas_main");
		return 1;
	}

	return 0;
}

#include "native_test_harness.h"

#include "ui/document.h"
#include "ui/style.h"
#include "ui/tree_internal.h"

#include <cstdio>
#include <string>

extern void __gea_top_level();

namespace {

bool expectStopwatchButtonGeometry()
{
	using namespace gea::embedded::test;
	auto &tree = gea::embedded::ui::Tree::instance();
	const auto startButtons = nodesWithClass("stopwatch-start-button");
	const auto lapButtons = nodesWithClass("stopwatch-lap-button");
	const auto resetButtons = nodesWithClass("stopwatch-reset-button");
	if (startButtons.size() != 1 || lapButtons.size() != 1 || resetButtons.size() != 1) {
		std::fprintf(stderr,
		             "[test_gea_stopwatch_main] expected one stopwatch control button of each kind, got start=%zu lap=%zu reset=%zu\n",
		             startButtons.size(),
		             lapButtons.size(),
		             resetButtons.size());
		dumpTree("test_gea_stopwatch_main");
		return false;
	}

	for (const int nodeId : {startButtons[0], lapButtons[0], resetButtons[0]}) {
		const auto &node = tree.node(nodeId);
		if (node.layout.width != 144 || node.layout.height != 144) {
			std::fprintf(stderr,
			             "[test_gea_stopwatch_main] expected 20vw control button layout 144x144, got (%d,%d %dx%d)\n",
			             node.layout.x,
			             node.layout.y,
			             node.layout.width,
			             node.layout.height);
			dumpTree("test_gea_stopwatch_main");
			return false;
		}
		if (node.style.border_radius[0] != 72 || node.style.border_radius[1] != 72 ||
		    node.style.border_radius[2] != 72 || node.style.border_radius[3] != 72) {
			std::fprintf(stderr,
			             "[test_gea_stopwatch_main] expected 10vw control radius 72, got %d/%d/%d/%d\n",
			             node.style.border_radius[0],
			             node.style.border_radius[1],
			             node.style.border_radius[2],
			             node.style.border_radius[3]);
			dumpTree("test_gea_stopwatch_main");
			return false;
		}
	}

	const auto &start = tree.node(startButtons[0]).layout;
	const int centerX = start.x + start.width / 2;
	int paintedRows = 0;
	for (int y = start.y; y < start.y + start.height; y++) {
		if (displayPixelAt(centerX, y) != 0) paintedRows++;
	}
	if (paintedRows < start.height - 4) {
		std::fprintf(stderr,
		             "[test_gea_stopwatch_main] expected center column to paint almost full button height, got %d/%d rows\n",
		             paintedRows,
		             start.height);
		dumpTree("test_gea_stopwatch_main");
		return false;
	}

	return true;
}

}  // namespace

int main()
{
	using namespace gea::embedded::test;
	resetNativeHost();
	setNativeDisplaySize(720, 1440);
	gea::embedded::ui::Document::setPreferredMountSize(720, 1440);
	gea::embedded::ui::setViewportMetrics(720, 1440, 2.0);
	__gea_top_level();
	refresh();
	if (!expectStopwatchButtonGeometry()) return 1;

	auto text = rootTextContent();
	if (!expectContains(text, "Ready", "initial text", "test_gea_stopwatch_main")) return 1;
	if (!expectContains(text, "00:00.00", "initial text", "test_gea_stopwatch_main")) return 1;
	if (!expectContains(text, "Lap 1", "initial text", "test_gea_stopwatch_main")) return 1;
	if (!expectContains(text, "--:--.--", "initial text", "test_gea_stopwatch_main")) return 1;

	if (!pressFirstText("Start", true)) return 1;
	pumpFrame(0);
	pumpFrame(1230);
	refresh();
	text = rootTextContent();
	if (!expectContains(text, "Running", "running text", "test_gea_stopwatch_main")) return 1;
	if (!expectContains(text, "00:01.23", "running text", "test_gea_stopwatch_main")) return 1;
	if (!expectContains(text, "Pause", "running text", "test_gea_stopwatch_main")) return 1;

	if (!pressFirstText("Lap", true)) return 1;
	refresh();
	text = rootTextContent();
	if (!expectContains(text, "Lap saved", "lap text", "test_gea_stopwatch_main")) return 1;
	if (!expectContains(text, "00:01.23", "lap text", "test_gea_stopwatch_main")) return 1;

	if (!pressFirstText("Reset", true)) return 1;
	refresh();
	text = rootTextContent();
	if (!expectContains(text, "Ready", "reset text", "test_gea_stopwatch_main")) return 1;
	if (!expectContains(text, "00:00.00", "reset text", "test_gea_stopwatch_main")) return 1;
	if (!expectContains(text, "Start", "reset text", "test_gea_stopwatch_main")) return 1;

	return 0;
}

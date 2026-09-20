#include "native_test_harness.h"

#include "display.h"
#include "ui/document.h"
#include "ui/style.h"
#include "ui/tree_internal.h"

#include <cstdio>
#include <cstdlib>
#include <vector>

extern void __gea_top_level();

namespace {

bool expectTicTacToeBoardFits(int viewportWidth)
{
	using namespace gea::embedded::test;
	auto &tree = gea::embedded::ui::Tree::instance();
	const auto cells = nodesWithClass("tic-cell");
	if (cells.size() != 9) {
		std::fprintf(stderr, "[test_gea_tic_tac_toe_main] expected 9 tic-cell nodes, got %zu\n", cells.size());
		dumpTree("test_gea_tic_tac_toe_main");
		return false;
	}

	const auto &first = tree.node(cells[0]).layout;
	for (int cell : cells) {
		const auto &box = tree.node(cell).layout;
		if (box.x < 0 || box.x + box.width > viewportWidth || box.width <= 0 || box.height <= 0) {
			std::fprintf(stderr,
			             "[test_gea_tic_tac_toe_main] expected cell to fit viewport width %d, got (%d,%d %dx%d)\n",
			             viewportWidth,
			             box.x,
			             box.y,
			             box.width,
			             box.height);
			dumpTree("test_gea_tic_tac_toe_main");
			return false;
		}
		if (std::abs(box.width - box.height) > 2 ||
		    std::abs(box.width - first.width) > 2 ||
		    std::abs(box.height - first.height) > 2) {
			std::fprintf(stderr,
			             "[test_gea_tic_tac_toe_main] expected square equal flex cells, first=%dx%d current=%dx%d\n",
			             first.width,
			             first.height,
			             box.width,
			             box.height);
			dumpTree("test_gea_tic_tac_toe_main");
			return false;
		}
	}

	return true;
}

std::vector<gea::embedded::ui::LayoutBox> ticCellLayouts()
{
	using namespace gea::embedded::test;
	auto &tree = gea::embedded::ui::Tree::instance();
	std::vector<gea::embedded::ui::LayoutBox> out;
	for (int cell : nodesWithClass("tic-cell")) out.push_back(tree.node(cell).layout);
	return out;
}

bool expectSameCellLayouts(const std::vector<gea::embedded::ui::LayoutBox> &before, const char *label)
{
	using namespace gea::embedded::test;
	const auto after = ticCellLayouts();
	if (before.size() != after.size()) {
		std::fprintf(stderr,
		             "[test_gea_tic_tac_toe_main] expected %zu cells after %s, got %zu\n",
		             before.size(),
		             label,
		             after.size());
		dumpTree("test_gea_tic_tac_toe_main");
		return false;
	}
	for (std::size_t i = 0; i < before.size(); i++) {
		if (before[i].x != after[i].x ||
		    before[i].y != after[i].y ||
		    before[i].width != after[i].width ||
		    before[i].height != after[i].height) {
			std::fprintf(stderr,
			             "[test_gea_tic_tac_toe_main] cell %zu layout changed after %s: before=(%d,%d %dx%d) after=(%d,%d %dx%d)\n",
			             i,
			             label,
			             before[i].x,
			             before[i].y,
			             before[i].width,
			             before[i].height,
			             after[i].x,
			             after[i].y,
			             after[i].width,
			             after[i].height);
			dumpTree("test_gea_tic_tac_toe_main");
			return false;
		}
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
	gea::platform::display::Display::flushStatsReset();
	auto &tree = gea::embedded::ui::Tree::instance();

	if (!expectTicTacToeBoardFits(720)) return 1;
	const auto initialCellLayouts = ticCellLayouts();

	auto text = rootTextContent();
	if (!expectContains(text, "Turn: X", "initial text", "test_gea_tic_tac_toe_main")) return 1;

	auto cells = nodesWithClass("tic-cell");
	if (!dispatchPress(cells[0])) return 1;
	gea::platform::display::Display::flushStatsReset();
	refresh();
	const int xFlushPixels = flushPixelCount();
	text = rootTextContent();
	if (!expectContains(text, "X", "after first move", "test_gea_tic_tac_toe_main")) return 1;
	if (!expectContains(text, "Turn: O", "after first move", "test_gea_tic_tac_toe_main")) return 1;
	if (!expectSameCellLayouts(initialCellLayouts, "first move")) return 1;
	if (xFlushPixels <= 0 || xFlushPixels >= 720 * 1440) {
		std::fprintf(stderr,
		             "[test_gea_tic_tac_toe_main] expected X move to refresh incrementally, flushed=%d viewport=%d\n",
		             xFlushPixels,
		             720 * 1440);
		return 1;
	}

	cells = nodesWithClass("tic-cell");
	if (cells.size() != 9 || !dispatchPress(cells[4])) return 1;
	const auto centerCellLayout = tree.node(cells[4]).layout;
	gea::platform::display::Display::flushStatsReset();
	refresh();
	const int oFlushPixels = flushPixelCount();
	text = rootTextContent();
	if (!expectContains(text, "O", "after second move", "test_gea_tic_tac_toe_main")) return 1;
	if (!expectContains(text, "Turn: X", "after second move", "test_gea_tic_tac_toe_main")) return 1;
	if (!expectSameCellLayouts(initialCellLayouts, "second move")) return 1;
	if (oFlushPixels <= 0 || oFlushPixels >= 720 * 1440) {
		std::fprintf(stderr,
		             "[test_gea_tic_tac_toe_main] expected O move to refresh incrementally, flushed=%d viewport=%d\n",
		             oFlushPixels,
		             720 * 1440);
		return 1;
	}
	const int centerCellFlushStrip = (centerCellLayout.x + centerCellLayout.width) * centerCellLayout.height;
	if (oFlushPixels >= centerCellFlushStrip / 2) {
		std::fprintf(stderr,
		             "[test_gea_tic_tac_toe_main] expected O move to repaint glyph band, not the cell box; flushed=%d halfCellStrip=%d cell=(%d,%d %dx%d)\n",
		             oFlushPixels,
		             centerCellFlushStrip / 2,
		             centerCellLayout.x,
		             centerCellLayout.y,
		             centerCellLayout.width,
		             centerCellLayout.height);
		return 1;
	}

	const auto xMarks = nodesWithText("X", true);
	const auto oMarks = nodesWithText("O", true);
	if (xMarks.empty() || oMarks.empty()) {
		std::fprintf(stderr, "[test_gea_tic_tac_toe_main] expected styled X and O marks\n");
		dumpTree("test_gea_tic_tac_toe_main");
		return 1;
	}
	const auto &xStyle = tree.node(xMarks[0]).style;
	const auto &oStyle = tree.node(oMarks[0]).style;
	if (xStyle.font_size != 96 || oStyle.font_size != 96 || xStyle.font_id < 0 || oStyle.font_id < 0 ||
	    xStyle.text_color == oStyle.text_color) {
		std::fprintf(stderr,
		             "[test_gea_tic_tac_toe_main] expected X/O font styling and different colors, xFont=%d oFont=%d xSize=%d oSize=%d xColor=%u oColor=%u\n",
		             xStyle.font_id,
		             oStyle.font_id,
		             xStyle.font_size,
		             oStyle.font_size,
		             static_cast<unsigned>(xStyle.text_color),
		             static_cast<unsigned>(oStyle.text_color));
		dumpTree("test_gea_tic_tac_toe_main");
		return 1;
	}

	return 0;
}

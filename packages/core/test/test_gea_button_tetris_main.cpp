// Test harness for button-tetris compiled via:
// gea-embedded compat -> vite-plugin-gea -> geatsc -> native retained UI.

#include "native_test_harness.h"

#include "display.h"
#include "ui/document.h"
#include "ui/internal.h"
#include "ui/style.h"
#include "ui/tree_internal.h"

#include <cmath>
#include <cstdio>
#include <csignal>
#include <cstdlib>
#include <vector>
#include <unistd.h>

extern void __gea_top_level();

namespace {

void fail_on_alarm(int /*signal*/)
{
	std::fputs("[test_gea_button_tetris_main] timed out while driving tetris drops\n", stderr);
	std::_Exit(124);
}

constexpr int kViewportWidth = 390;
constexpr int kViewportHeight = 844;
constexpr double kDevicePixelRatio = 3.0;

constexpr int kBoardCols = 10;
constexpr int kBoardRows = 18;
constexpr int kPlayfieldBorderWidth = 2;

int expectedCellSize()
{
	// Mirrors examples/apps/button-tetris/constants.tsx: cell bounded by both axes.
	int cellFromWidth = static_cast<int>(std::lround(kViewportWidth * 0.052));
	if (cellFromWidth < 1) cellFromWidth = 1;
	int cellFromHeight = static_cast<int>(std::floor((kViewportHeight * 0.86) / (kBoardRows + 2)));
	if (cellFromHeight < 1) cellFromHeight = 1;
	return cellFromWidth < cellFromHeight ? cellFromWidth : cellFromHeight;
}

int expectedBoardX()
{
	return static_cast<int>(std::lround((kViewportWidth - kBoardCols * expectedCellSize()) / 2.0));
}

int expectedBoardY()
{
	return static_cast<int>(std::lround(kViewportHeight * 0.14));
}

int expectedBlockSize()
{
	int gap = static_cast<int>(std::lround(expectedCellSize() * 0.1));
	if (gap < 1) gap = 1;
	return expectedCellSize() - gap;
}

int scaledGridPosition(int base, int cell, int index)
{
	return base + cell * index;
}

bool nearValue(int value, int expected, int tolerance, const char *label)
{
	if (std::abs(value - expected) <= tolerance) return true;
	std::fprintf(stderr, "[test_gea_button_tetris_main] expected %s around %d, got %d\n", label, expected, value);
	return false;
}

bool matchesGridPosition(int value, int base, int cell, int count)
{
	for (int i = 0; i < count; ++i) {
		if (std::abs(value - scaledGridPosition(base, cell, i)) <= 2) return true;
	}
	return false;
}

bool dispatchDrop()
{
	using namespace gea::embedded::test;
	const auto drops = nodesWithText("Drop", true);
	if (drops.empty()) {
		std::fprintf(stderr, "[test_gea_button_tetris_main] expected Drop button text\n");
		dumpTree("test_gea_button_tetris_main");
		return false;
	}
	return dispatchPress(drops.front());
}

// The locked stack, as the app renders it TODAY.
//
// This used to look for `tetris-block` not on `tetris-block-hidden`, anywhere
// under `tetris-stack-layer`. `TetrisBoard.tsx` no longer spells the stack that
// way: `tetris-block` is now the ACTIVE falling piece only, the stack is a
// keyed `tetris.cells.map(...)` over the whole 10x18 grid, and an empty cell is
// expressed by `width: cell.filled === 0 ? 0 : BLOCK_SIZE` rather than by a
// `-hidden` class. (`.tetris-block-hidden` survives in `styles.css` with no
// element left using it, which is what made the drift easy to miss.) So the old
// selector matched nothing under the stack layer and this returned an empty
// vector no matter what the game did -- `expectLockedBlocks(4)` could never
// pass, on any compiler.
//
// Same test, same strength: exactly one node per LOCKED cell, still scoped to
// the stack, and the caller still checks each one's grid position, opacity,
// display and bottom-row membership. Scoped to the COLOR layer because the
// stack is drawn twice -- `tetris-stack-monochrome-block` carries
// `tetris-stack-block` too, and both layers render all 180 cells -- so matching
// the class alone would count every locked cell twice.
std::vector<int> stackBlocks()
{
	using namespace gea::embedded::test;
	std::vector<int> blocks;
	auto &tree = gea::embedded::ui::Tree::instance();
	const auto layers = nodesWithClass("tetris-stack-color-layer");
	if (layers.size() != 1) return blocks;
	const int layer = layers[0];
	for (int node = 0; node < tree.nodeCount(); ++node) {
		if (node == layer) continue;
		const auto &candidate = tree.node(node);
		// A zero-sized cell is this app's "empty", not a locked block.
		if (tree.hasClass(node, "tetris-stack-block") &&
		    candidate.layout.width > 0 && candidate.layout.height > 0 &&
		    candidate.style.display != 1) {
			for (int parent = tree.node(node).parent; parent >= 0; parent = tree.node(parent).parent) {
				if (parent == layer) {
					blocks.push_back(node);
					break;
				}
			}
		}
	}
	return blocks;
}

bool expectBoardGeometry()
{
	using namespace gea::embedded::test;
	auto &tree = gea::embedded::ui::Tree::instance();
	const int board_x = expectedBoardX();
	const int board_y = expectedBoardY();
	const int cell_size = expectedCellSize();
	const int block_size = expectedBlockSize();

	const auto playfields = nodesWithClass("tetris-board-playfield");
	if (playfields.size() != 1) {
		std::fprintf(stderr, "[test_gea_button_tetris_main] expected one playfield, got %zu\n", playfields.size());
		dumpTree("test_gea_button_tetris_main");
		return false;
	}

	const auto &playfield = tree.node(playfields[0]);
	if (!nearValue(playfield.style.pos_offsets[3], board_x - kPlayfieldBorderWidth, 2, "playfield left") ||
	    !nearValue(playfield.style.pos_offsets[0], board_y - kPlayfieldBorderWidth, 2, "playfield top") ||
	    !nearValue(playfield.style.width, cell_size * 10 + kPlayfieldBorderWidth * 2, 4, "playfield width") ||
	    !nearValue(playfield.style.height, cell_size * 18 + kPlayfieldBorderWidth * 2, 6, "playfield height") ||
	    playfield.style.border_width != kPlayfieldBorderWidth) {
		dumpTree("test_gea_button_tetris_main");
		return false;
	}

	bool sawWidthAwareBorder = false;
	auto &displayList = gea::embedded::ui::DisplayList::instance();
	for (int i = 0; i < displayList.nodeCommandCount(playfields[0]); ++i) {
		const auto *command = displayList.nodeCommandAt(playfields[0], i);
		if (!command || command->type != gea::embedded::ui::DisplayCommandType::StrokeRoundedRect) continue;
		if (command->strokeRoundedRect.lineWidth == kPlayfieldBorderWidth &&
		    command->strokeRoundedRect.tl == 0 && command->strokeRoundedRect.tr == 0 &&
		    command->strokeRoundedRect.br == 0 && command->strokeRoundedRect.bl == 0) {
			sawWidthAwareBorder = true;
			break;
		}
	}
	if (!sawWidthAwareBorder) {
		std::fprintf(stderr, "[test_gea_button_tetris_main] expected a width-aware 2px square playfield border command\n");
		dumpTree("test_gea_button_tetris_main");
		return false;
	}

	for (int id : nodesWithClass("tetris-block")) {
		const auto &node = tree.node(id);
		if (node.style.opacity == 0 || node.style.display == 1) continue;
		const int left = node.style.pos_offsets[3];
		const int top = node.style.pos_offsets[0];
		if (!nearValue(node.style.width, block_size, 2, "block width") ||
		    !nearValue(node.style.height, block_size, 2, "block height") ||
		    left < board_x - 2 ||
		    left > scaledGridPosition(board_x, cell_size, 9) + 2 ||
		    top < board_y - 2 ||
		    top > scaledGridPosition(board_y, cell_size, 17) + 2) {
			std::fprintf(stderr, "[test_gea_button_tetris_main] visible block id=%d escaped playfield left=%d top=%d\n", id, left, top);
			dumpTree("test_gea_button_tetris_main");
			return false;
		}
	}

	return true;
}

bool expectLockedBlocks(std::size_t expectedCount)
{
	using namespace gea::embedded::test;
	auto &tree = gea::embedded::ui::Tree::instance();
	const int board_x = expectedBoardX();
	const int board_y = expectedBoardY();
	const int cell_size = expectedCellSize();
	const auto blocks = stackBlocks();
	if (blocks.size() != expectedCount) {
		std::fprintf(stderr, "[test_gea_button_tetris_main] expected %zu locked stack blocks, got %zu\n", expectedCount, blocks.size());
		dumpTree("test_gea_button_tetris_main");
		return false;
	}

	bool saw_bottom_block = false;
	for (int id : blocks) {
		const auto &node = tree.node(id);
		const int left = node.style.pos_offsets[3];
		const int top = node.style.pos_offsets[0];
		if (!matchesGridPosition(left, board_x, cell_size, 10) || !matchesGridPosition(top, board_y, cell_size, 18)) {
			std::fprintf(stderr, "[test_gea_button_tetris_main] invalid locked block geometry id=%d left=%d top=%d\n", id, left, top);
			dumpTree("test_gea_button_tetris_main");
			return false;
		}
		if (node.style.opacity != 255 || node.style.display == 1) {
			std::fprintf(stderr, "[test_gea_button_tetris_main] locked block id=%d is hidden: opacity=%u display=%d\n",
			             id,
			             static_cast<unsigned>(node.style.opacity),
			             static_cast<int>(node.style.display));
			dumpTree("test_gea_button_tetris_main");
			return false;
		}
		if (std::abs(top - scaledGridPosition(board_y, cell_size, 17)) <= 2) saw_bottom_block = true;
	}

	if (!saw_bottom_block) {
		std::fprintf(stderr,
		             "[test_gea_button_tetris_main] expected at least one locked block on the bottom row top=%d\n",
		             scaledGridPosition(board_y, cell_size, 17));
		dumpTree("test_gea_button_tetris_main");
		return false;
	}

	return true;
}

}  // namespace

int main()
{
	using namespace gea::embedded::test;
	std::signal(SIGALRM, fail_on_alarm);
	alarm(5);

	resetNativeHost();
	setNativeDisplaySize(kViewportWidth, kViewportHeight);
	gea::embedded::ui::Document::setPreferredMountSize(kViewportWidth, kViewportHeight);
	gea::embedded::ui::setViewportMetrics(kViewportWidth, kViewportHeight, kDevicePixelRatio);
	__gea_top_level();
	refresh();

	if (!expectContains(rootTextContent(), "Score 0", "rendered text", "test_gea_button_tetris_main")) return 1;
	if (!expectBoardGeometry()) return 1;

	gea::platform::display::Display::flushStatsReset();
	if (!dispatchDrop()) return 1;
	refresh();
	const int firstDropFlushPixels = flushPixelCount();
	if (!expectContains(rootTextContent(), "Score 4", "rendered text after first drop", "test_gea_button_tetris_main")) return 1;
	if (!expectLockedBlocks(4)) return 1;
	if (firstDropFlushPixels <= 0 || firstDropFlushPixels >= kViewportWidth * kViewportHeight) {
		std::fprintf(stderr,
		             "[test_gea_button_tetris_main] expected first drop to refresh incrementally, flushed=%d viewport=%d\n",
		             firstDropFlushPixels,
		             kViewportWidth * kViewportHeight);
		return 1;
	}

	gea::platform::display::Display::flushStatsReset();
	if (!dispatchDrop()) return 1;
	refresh();
	const int secondDropFlushPixels = flushPixelCount();
	if (!expectContains(rootTextContent(), "Score 8", "rendered text after second drop", "test_gea_button_tetris_main")) return 1;
	if (!expectLockedBlocks(8)) return 1;
	if (secondDropFlushPixels <= 0 || secondDropFlushPixels >= kViewportWidth * kViewportHeight) {
		std::fprintf(stderr,
		             "[test_gea_button_tetris_main] expected second drop to refresh incrementally, flushed=%d viewport=%d\n",
		             secondDropFlushPixels,
		             kViewportWidth * kViewportHeight);
		return 1;
	}

	alarm(0);
	return 0;
}

#include "native_test_harness.h"
#include "ui/internal.h"
#include "ui/tree_internal.h"

#include <cstdio>

extern void __gea_top_level();

namespace {

int paintedRowCountAtX(int x, int y, int h)
{
	int rows = 0;
	for (int row = y; row < y + h; ++row) {
		if (gea::embedded::test::displayPixelAt(x, row) != 0) ++rows;
	}
	return rows;
}

bool rowHasPixelAtX(int x, int y)
{
	return gea::embedded::test::displayPixelAt(x, y) != 0;
}

int recordStrikeSample(int fontSize)
{
	using namespace gea::embedded::ui;
	using namespace gea::embedded::test;

	resetNativeHost();
	DisplayList::instance().clear();

	Node node{};
	node.type = NodeType::Text;
	node.text = "        ";
	node.layout.x = 40;
	node.layout.y = 40;
	node.layout.width = 180;
	node.layout.height = 80;
	node.style.font_id = -1;
	node.style.font_size = fontSize;
	node.style.text_color = 0xffff;
	node.style.text_alpha = 255;
	node.style.text_decoration = 2;

	TextRenderer::record(node);
	DisplayList::instance().replay();

	return paintedRowCountAtX(node.layout.x + 2, node.layout.y, node.layout.height);
}

bool expectLineThroughDecorationGeometry()
{
	using namespace gea::embedded::test;

	const int twentyPxRows = recordStrikeSample(20);
	if (twentyPxRows < 2) {
		std::fprintf(stderr, "[test_gea_typography_main] expected 20px line-through to paint at least 2 rows, got %d\n",
		             twentyPxRows);
		return false;
	}
	if (!rowHasPixelAtX(42, 52)) {
		std::fprintf(stderr, "[test_gea_typography_main] expected 20px line-through to sit lower through the text box\n");
		return false;
	}

	const int thirtyFourPxRows = recordStrikeSample(34);
	if (thirtyFourPxRows <= twentyPxRows) {
		std::fprintf(stderr,
		             "[test_gea_typography_main] expected line-through thickness to scale with font size, got 20px=%d rows 34px=%d rows\n",
		             twentyPxRows,
		             thirtyFourPxRows);
		return false;
	}

	return true;
}

bool expectWordBoundaryWrapping()
{
	using namespace gea::embedded::ui;
	using namespace gea::embedded::test;

	resetNativeHost();
	DisplayList::instance().clear();

	Node wrapped{};
	wrapped.type = NodeType::Text;
	wrapped.text = "hello world";
	wrapped.layout.x = 8;
	wrapped.layout.y = 8;
	wrapped.layout.width = 64;  // eight bitmap cells: old code split `wo|rld`
	wrapped.layout.height = 40;
	wrapped.style.font_id = -1;
	wrapped.style.font_size = 16;
	wrapped.style.text_color = 0xffff;
	wrapped.style.text_alpha = 255;
	TextRenderer::record(wrapped);

	Node expected = wrapped;
	expected.text = "world";
	expected.layout.x = 96;
	expected.layout.y = 24;
	expected.layout.width = 40;
	expected.layout.height = 16;
	TextRenderer::record(expected);
	DisplayList::instance().replay();

	for (int y = 0; y < 16; ++y) {
		for (int x = 0; x < 40; ++x) {
			const bool actualPixel = displayPixelAt(8 + x, 24 + y) != 0;
			const bool expectedPixel = displayPixelAt(96 + x, 24 + y) != 0;
			if (actualPixel != expectedPixel) {
				std::fprintf(stderr, "[test_gea_typography_main] expected wrapped second line to start at the word boundary\n");
				return false;
			}
		}
	}
	return true;
}

}  // namespace

int main()
{
	using namespace gea::embedded::test;
	resetNativeHost();
	if (!expectLineThroughDecorationGeometry()) return 1;
	if (!expectWordBoundaryWrapping()) return 1;

	resetNativeHost();
	__gea_top_level();
	refresh();

	const auto text = rootTextContent();
	if (!expectContains(text, "Typography", "initial text", "test_gea_typography_main")) return 1;
	if (!expectContains(text, "Heading One", "initial text", "test_gea_typography_main")) return 1;
	if (!expectContains(text, "Bebas Neue", "initial text", "test_gea_typography_main")) return 1;
	if (!expectContains(text, "Cossette Texte", "initial text", "test_gea_typography_main")) return 1;
	if (!expectContains(text, "Mixed Composition", "initial text", "test_gea_typography_main")) return 1;

	const auto appNodes = nodesWithClass("typography-app");
	if (appNodes.size() != 1 || nodesWithClass("typography-heading-card").empty()) {
		std::fprintf(stderr, "[test_gea_typography_main] expected typography app and heading card classes\n");
		dumpTree("test_gea_typography_main");
		return 1;
	}

	auto &tree = gea::embedded::ui::Tree::instance();
	const auto introTextNodes = nodesWithText("A scrollable type specimen");
	if (introTextNodes.empty() || tree.node(introTextNodes[0]).style.font_size != 16) {
		std::fprintf(stderr, "[test_gea_typography_main] expected unstyled intro text to inherit typography-app font-size 16\n");
		dumpTree("test_gea_typography_main");
		return 1;
	}
	const auto introCopyNodes = nodesWithClass("typography-intro-copy");
	const auto introDivTokens = nodesWithClass("typography-token-div");
	const auto introSpanTokens = nodesWithClass("typography-token-span");
	const auto introPTokens = nodesWithClass("typography-token-p");
	if (introCopyNodes.size() != 1 || introDivTokens.size() != 1 || introSpanTokens.size() != 1 || introPTokens.size() != 1) {
		std::fprintf(stderr, "[test_gea_typography_main] expected intro copy and inline tag token spans\n");
		dumpTree("test_gea_typography_main");
		return 1;
	}
	// A paragraph carrying inline children lowers as a plain BLOCK. The engine
	// synthesizes the inline formatting context from its all-inline children
	// (LayoutNodePass::resolveInlineFormattingRow) and flows real line boxes. It
	// used to be forced to `display: flex; flex-wrap: wrap`, which the engine could
	// not tell from an authored display:flex — so every run stayed an atomic flex
	// item and none of them could share, or split across, a line.
	if (tree.node(introCopyNodes[0]).type != gea::embedded::ui::NodeType::View ||
	    tree.node(introCopyNodes[0]).style.display != gea::embedded::ui::kDisplayBlock ||
	    tree.node(introCopyNodes[0]).style.display_explicit != 0 ||
	    tree.node(introCopyNodes[0]).style.flex_direction_explicit != 0 ||
	    tree.node(introCopyNodes[0]).style.font_size != 16) {
		std::fprintf(stderr, "[test_gea_typography_main] expected mixed intro paragraph to lower as an inherited inline-flow BLOCK\n");
		dumpTree("test_gea_typography_main");
		return 1;
	}
	// "The page uses div, span, p and headings…" — the three token spans belong to
	// ONE line box, in order. Atomic flex items put each on its own line.
	if (tree.node(introDivTokens[0]).layout.y != tree.node(introSpanTokens[0]).layout.y ||
	    tree.node(introSpanTokens[0]).layout.y != tree.node(introPTokens[0]).layout.y ||
	    tree.node(introDivTokens[0]).layout.x >= tree.node(introSpanTokens[0]).layout.x ||
	    tree.node(introSpanTokens[0]).layout.x >= tree.node(introPTokens[0]).layout.x) {
		std::fprintf(stderr,
		             "[test_gea_typography_main] expected div/span/p tokens to share one line box in order, got "
		             "div=(%d,%d) span=(%d,%d) p=(%d,%d)\n",
		             tree.node(introDivTokens[0]).layout.x, tree.node(introDivTokens[0]).layout.y,
		             tree.node(introSpanTokens[0]).layout.x, tree.node(introSpanTokens[0]).layout.y,
		             tree.node(introPTokens[0]).layout.x, tree.node(introPTokens[0]).layout.y);
		dumpTree("test_gea_typography_main");
		return 1;
	}
	if (tree.node(introDivTokens[0]).layout.width <= 0 ||
	    tree.node(introSpanTokens[0]).layout.width <= 0 ||
	    tree.node(introPTokens[0]).layout.width <= 0) {
		std::fprintf(stderr, "[test_gea_typography_main] expected intro inline token spans to render with non-zero width\n");
		dumpTree("test_gea_typography_main");
		return 1;
	}
	const auto copyNodes = nodesWithClass("mixed-composition-copy");
	if (copyNodes.size() != 1) {
		std::fprintf(stderr, "[test_gea_typography_main] expected one mixed composition copy node\n");
		dumpTree("test_gea_typography_main");
		return 1;
	}
	const int copy = copyNodes[0];
	if (tree.node(copy).type != gea::embedded::ui::NodeType::View ||
	    tree.node(copy).style.display != gea::embedded::ui::kDisplayBlock ||
	    tree.node(copy).style.display_explicit != 0 ||
	    tree.node(copy).style.font_size != 17 ||
	    tree.node(copy).style.flex_direction_explicit != 0) {
		std::fprintf(stderr, "[test_gea_typography_main] expected mixed copy to lower to an inline-flow BLOCK at font-size 17\n");
		dumpTree("test_gea_typography_main");
		return 1;
	}
	const auto labels = nodesWithClass("mixed-composition-label");
	const auto loud = nodesWithClass("mixed-composition-loud");
	const auto tail = nodesWithText(" without leaving the block.", true);
	if (labels.size() != 1 || loud.size() != 1 || tail.size() != 1) {
		std::fprintf(stderr, "[test_gea_typography_main] expected all inline mixed-composition spans and tail text to be present\n");
		dumpTree("test_gea_typography_main");
		return 1;
	}
	if (tree.node(labels[0]).style.font_size != 17 ||
	    tree.node(tail[0]).style.font_size != 17 ||
	    tree.node(loud[0]).style.font_size != 22 ||
	    tree.node(labels[0]).layout.width <= 0 ||
	    tree.node(loud[0]).layout.width <= 0) {
		std::fprintf(stderr, "[test_gea_typography_main] expected inline span text to inherit/render and loud span to keep explicit font-size 22\n");
		dumpTree("test_gea_typography_main");
		return 1;
	}
	// The run after the LOUD MOMENT span continues on the span's OWN line box and
	// wraps its remainder at the paragraph's left edge — that is what a text run
	// splitting across lines looks like: a full-width box whose first line is
	// indented to the pen it inherited. The atomic-flex-item lowering could only
	// push the whole run onto the next line.
	// The pen continues exactly at the span's right edge, and the two boxes overlap
	// vertically (they share a line box; their TOPS differ because a 22px Bebas
	// span and 17px Cossette text align on the baseline, not the box edge).
	const int loudRight = tree.node(loud[0]).layout.x + tree.node(loud[0]).layout.width;
	const int tailPen = tree.node(tail[0]).layout.x + tree.node(tail[0]).layout.inline_indent;
	const int loudTop = tree.node(loud[0]).layout.y;
	const int loudBottom = loudTop + tree.node(loud[0]).layout.height;
	const int tailTop = tree.node(tail[0]).layout.y;
	if (tailPen != loudRight || tailTop >= loudBottom || loudTop >= tailTop + tree.node(tail[0]).layout.height) {
		std::fprintf(stderr,
		             "[test_gea_typography_main] expected the tail run to continue on the LOUD span's line, got "
		             "loud=(%d,%d w=%d) tail=(%d,%d w=%d indent=%d)\n",
		             tree.node(loud[0]).layout.x, tree.node(loud[0]).layout.y, tree.node(loud[0]).layout.width,
		             tree.node(tail[0]).layout.x, tree.node(tail[0]).layout.y, tree.node(tail[0]).layout.width,
		             tree.node(tail[0]).layout.inline_indent);
		dumpTree("test_gea_typography_main");
		return 1;
	}
	const auto headingOne = nodesWithText("Heading One", true);
	const auto headingTwo = nodesWithText("Heading Two", true);
	const auto headingThree = nodesWithText("Heading Three", true);
	const auto headingFour = nodesWithText("Heading Four", true);
	const auto headingFive = nodesWithText("Heading Five", true);
	const auto headingSix = nodesWithText("Heading Six", true);
	if (headingOne.size() != 1 || headingTwo.size() != 1 || headingThree.size() != 1 ||
	    headingFour.size() != 1 || headingFive.size() != 1 || headingSix.size() != 1) {
		std::fprintf(stderr, "[test_gea_typography_main] expected heading text nodes\n");
		dumpTree("test_gea_typography_main");
		return 1;
	}
	if (tree.node(headingOne[0]).style.font_size != 32 ||
	    tree.node(headingTwo[0]).style.font_size != 28 ||
	    tree.node(headingThree[0]).style.font_size != 24 ||
	    tree.node(headingFour[0]).style.font_size != 20 ||
	    tree.node(headingFive[0]).style.font_size != 18 ||
	    tree.node(headingSix[0]).style.font_size != 16) {
		std::fprintf(stderr, "[test_gea_typography_main] expected h1-h6 heading font-size rules\n");
		dumpTree("test_gea_typography_main");
		return 1;
	}
	if (tree.node(headingOne[0]).style.margin[2] != 6 ||
	    tree.node(headingTwo[0]).style.margin[2] != 6 ||
	    tree.node(headingThree[0]).style.margin[2] != 6 ||
	    tree.node(headingFour[0]).style.margin[2] != 6 ||
	    tree.node(headingFive[0]).style.margin[2] != 6 ||
	    tree.node(headingSix[0]).style.margin[2] != 0) {
		std::fprintf(stderr, "[test_gea_typography_main] expected comma-separated heading element CSS to apply margin-bottom\n");
		dumpTree("test_gea_typography_main");
		return 1;
	}
	const int app = appNodes[0];
	const auto &appNode = tree.node(app);
	const int touchX = appNode.layout.x + appNode.layout.width / 2;
	const int touchY = appNode.layout.y + appNode.layout.height / 2;
	const int flushesBeforeScroll = flushCallCount();
	setNativeNowMs(0);
	tree.pointerDown(touchX, touchY);
	setNativeNowMs(16);
	if (!tree.pointerMove(touchX, touchY - 24)) {
		std::fprintf(stderr, "[test_gea_typography_main] expected typography root to scroll\n");
		dumpTree("test_gea_typography_main");
		return 1;
	}
	setNativeNowMs(32);
	if (!tree.pointerMove(touchX, touchY - 48)) {
		std::fprintf(stderr, "[test_gea_typography_main] expected typography root to scroll\n");
		dumpTree("test_gea_typography_main");
		return 1;
	}
	if (tree.node(app).layout.scroll_y <= 0) {
		std::fprintf(stderr, "[test_gea_typography_main] expected scroll_y to increase\n");
		return 1;
	}
	if (flushCallCount() != flushesBeforeScroll) {
		std::fprintf(stderr, "[test_gea_typography_main] scroll move should defer repaint to the frame tick\n");
		return 1;
	}
	if (!tree.refreshRequired()) {
		std::fprintf(stderr, "[test_gea_typography_main] expected scroll move to leave a pending refresh\n");
		return 1;
	}
	refresh();
	if (flushCallCount() <= flushesBeforeScroll) {
		std::fprintf(stderr, "[test_gea_typography_main] expected deferred scroll refresh to flush\n");
		return 1;
	}
	tree.pointerUp();
	const int scrollAfterDrag = tree.node(app).layout.scroll_y;
	pumpFrame(48);
	pumpFrame(64);
	if (tree.node(app).layout.scroll_y <= scrollAfterDrag) {
		std::fprintf(stderr, "[test_gea_typography_main] expected momentum scroll to continue after pointerUp\n");
		return 1;
	}

	return 0;
}

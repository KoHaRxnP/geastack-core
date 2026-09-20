// Repro for "a wrapped run draws more lines than its box is tall".
//
// TextRenderer::layout() measures the run against the parent's content width W
// and stores the measured MAX LINE WIDTH as layout.width. TextRenderer::record()
// then re-wraps the same run at layout.width to draw it. That only agrees if the
// line breaker is idempotent at its own max line width — and it was not: the
// widest line's trailing space "overflowed" the narrower box and pushed that
// line's last word down, adding a line the box has no room for. The extra line
// draws below the block and lands on top of the next sibling (typography's
// specimen paragraphs overlapped exactly one line).
//
// The assertion is deliberately made against PIXELS, not boxes: the layout math
// is self-consistent either way, so only what the renderer actually paints can
// tell the two apart.
#include "native_test_harness.h"

#include "display.h"
#include "ui/document.h"
#include "ui/internal.h"
#include "ui/node.h"
#include "ui/style.h"
#include "ui/tree_internal.h"

#include <cstdio>

namespace gea::framework::app::generated {
void drainMicrotasks() {}
}  // namespace gea::framework::app::generated

namespace gea::framework::graphics::generated {
void ensureLinked() {}
}  // namespace gea::framework::graphics::generated

using namespace gea::embedded::ui;

namespace {

constexpr int kDisplayW = 260;
constexpr int kDisplayH = 420;

// Bottom-most row carrying ink, or -1 when nothing painted.
int lastPaintedRow()
{
	for (int y = kDisplayH - 1; y >= 0; --y) {
		for (int x = 0; x < kDisplayW; ++x) {
			if (gea::embedded::test::displayPixelAt(x, y) != 0) return y;
		}
	}
	return -1;
}

int runCase(const char *label, const char *copy, int fontSize)
{
	using namespace gea::embedded::test;

	resetNativeHost();
	setNativeDisplaySize(kDisplayW, kDisplayH);
	gea::embedded::ui::setViewportMetrics(kDisplayW, kDisplayH, 1.0);

	// A plain column block (the specimen <div>) holding one paragraph, exactly
	// the shape every typography specimen has. No background colour anywhere:
	// every lit pixel below is glyph ink.
	auto root = Document::instance().createView();
	root.style().width(kDisplayW);
	root.style().height(kDisplayH);
	root.style().display(kDisplayFlex);
	root.style().set(Property::FlexDirection, 1);  // column

	auto paragraph = Document::instance().createView();
	paragraph.setTagName("p");
	paragraph.style().set(Property::FontSize, fontSize);
	auto copyNode = Document::instance().createText(copy);
	root.appendChild(paragraph);
	paragraph.appendChild(copyNode);

	Document::instance().mount(root, kDisplayW, kDisplayH);

	const Node &block = Tree::instance().node(paragraph.id());
	const Node &run = Tree::instance().node(copyNode.id());
	const int boxBottom = block.layout.y + block.layout.height;
	const int inkBottom = lastPaintedRow();

	std::printf("[%s] block=(y=%d h=%d) run=(w=%d h=%d) boxBottom=%d inkBottom=%d\n",
	            label, block.layout.y, block.layout.height, run.layout.width, run.layout.height,
	            boxBottom, inkBottom);

	if (inkBottom < 0) {
		std::fprintf(stderr, "[FAIL] %s: nothing painted — the case proves nothing\n", label);
		return 1;
	}
	if (inkBottom >= boxBottom) {
		std::fprintf(stderr,
		             "[FAIL] %s: the run paints down to y=%d but its block ends at y=%d — "
		             "the renderer wrapped it into more lines than layout measured, so it "
		             "draws on top of whatever follows\n",
		             label, inkBottom, boxBottom);
		return 1;
	}
	return 0;
}

}  // namespace

int main()
{
	int rc = 0;
	// The two specimen paragraphs that overlapped on device/simulator.
	rc |= runCase("literata-copy-large",
	              "Literata is a serif built for long-form reading on screens. Its sturdy strokes "
	              "and open counters stay legible on e-paper and low-density panels alike.",
	              18);
	rc |= runCase("cossette-copy-large",
	              "Cossette Texte brings a warmer reading texture. It is friendly for longer "
	              "passages, notes, instruction copy, and UI surfaces that should feel more editorial.",
	              18);
	rc |= runCase("pangram",
	              "Grumpy wizards make toxic brew for the jovial queen.",
	              17);

	if (rc == 0) std::printf("[PASS] wrapped runs stay inside the box layout measured for them\n");
	return rc;
}

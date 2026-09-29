#include "native_test_harness.h"
#include "canvas.h"
#include "display.h"
#include "graphics/font.h"
#include "ui/document.h"
#include "ui/internal.h"
#include "ui/node.h"
#include "ui/style.h"
#include "ui/tree_internal.h"
#include <cassert>
#include <cstdint>
#include <cstdio>

namespace gea::framework::app::generated { void drainMicrotasks() {} }
namespace gea::framework::graphics::generated {
void ensureLinked() {}
const RasterizedFontData *lookupFontForFamily(int, int)
{
	static const std::uint8_t atlas[]{255, 128, 0, 255, 0, 255, 128, 0, 0, 128, 255, 0, 255, 0, 0, 255};
	static const Glyph glyphs[]{{32, 0, 0, 0, 0, 6, 0, 0}, {88, 0, 0, 4, 4, 6, 0, 4}};
	static const RasterizedFontData font{9301, 4, 8, 4, -4, 2, glyphs, 4, 4, atlas};
	return &font;
}
}

using namespace gea::embedded::test;
using namespace gea::embedded::ui;
using gea::platform::display::Display;
namespace pixel = gea::framework::graphics::pixel;

static void fingerprint(const char *label)
{
	std::uint64_t hash = 1469598103934665603ull;
	int painted = 0;
	for (int y = 0; y < 120; ++y) for (int x = 0; x < 120; ++x) {
		const auto color = displayPixelAt(x, y);
		hash = (hash ^ color) * 1099511628211ull;
		painted += color != 0;
	}
	assert(painted > 100);
	std::printf("%s %016llx\n", label, static_cast<unsigned long long>(hash));
}

int main()
{
	resetNativeHost();
	setNativeDisplaySize(120, 120);
	setViewportMetrics(120, 120, 1.0);
	StyleSheet::instance().clear();
	Display::clearNoFlush();
	auto *canvas = Display::canvas();
	assert(canvas);
	// Exercise all three circle cache paths, including the rounded CSS circle
	// shortcut whose old cache-miss branch simply returned without painting.
	for (int aa : {0, 4}) {
		Display::setAA(aa);
		canvas->clear(0);
		canvas->fillCircle(25, 25, 12, pixel::nativeColor(255, 20, 20));
		canvas->fillRoundedRect(45, 10, 24, 24, 12, 12, 12, 12, pixel::nativeColor(20, 255, 20));
		canvas->setGlobalAlpha(140);
		canvas->fillRoundedRect(10, 50, 60, 35, 9, 9, 9, 9, pixel::nativeColor(20, 20, 255));
		canvas->setGlobalAlpha(255);
		const std::int16_t xs[]{75, 88}, ys[]{22, 58};
		const pixel::native_t colors[]{pixel::nativeColor(200, 90, 40), pixel::nativeColor(40, 90, 200)};
		canvas->fillRoundedRectBoxesRgb565(xs, ys, 2, 18, 18, 9, 9, 9, 9, colors);
		assert(displayPixelAt(57, 22) != 0);
		fingerprint(aa ? "shapes-aa" : "shapes");
	}

	auto root = Document::instance().createView();
	root.style().width(120); root.style().height(120);
	root.style().backgroundColor(pixel::nativeColor(18, 24, 30));
	auto card = Document::instance().createView();
	card.style().setProperty("position", "absolute");
	card.style().left(10); card.style().top(10);
	card.style().width(90); card.style().height(90);
	card.style().setProperty("border-radius", "12px");
	card.style().set(Property::FontSize, 8);
	card.style().set(Property::FontId, 9301);
	card.style().setProperty("color", "white");
	card.appendChild(Document::instance().createText("X X X"));
	root.appendChild(card);
	Document::instance().mount(root, 120, 120);
	const char *backgrounds[]{
		"linear-gradient(135deg, #ff2040, #2040ff)",
		"linear-gradient(to bottom, rgba(255, 32, 64, 0.4), rgba(32, 64, 255, 0.8))",
		"radial-gradient(ellipse at center, #ff2040, #2040ff)",
		"radial-gradient(ellipse at center, rgba(255, 32, 64, 0.4), rgba(32, 64, 255, 0.8))",
	};
	for (const auto *background : backgrounds) {
		card.style().setProperty("background", background);
		for (int frame = 0; frame < 3; ++frame) {
			Document::instance().refresh(root, 120, 120);
			DisplayList::instance().replay();
			fingerprint(background);
		}
	}
	card.style().setProperty("background", backgrounds[0]);
	card.style().setProperty("transform", "rotate(17deg) scale(0.8)");
	for (int frame = 0; frame < 3; ++frame) {
		if (frame == 1) card.style().setProperty("transform", "rotate(31deg) scale(0.9)");
		Document::instance().refresh(root, 120, 120);
		fingerprint("transformed-gradient");
		const Node &node = Tree::instance().node(card.id());
		int16_t xs[4], ys[4];
		ViewRenderer::transformedCorners(node, false, xs, ys);
		assert(xs[0] != node.layout.x || ys[0] != node.layout.y);
		std::printf("corners %d %d %d %d\n", xs[0], ys[0], xs[2], ys[2]);
		ViewRenderer::transformedCorners(node, true, xs, ys);
		std::printf("previous-corners %d %d %d %d\n", xs[0], ys[0], xs[2], ys[2]);
	}
	return 0;
}

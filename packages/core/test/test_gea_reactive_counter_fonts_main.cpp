#include "native_test_harness.h"

#include "graphics/font.h"
#include "ui/document.h"
#include "ui/internal.h"
#include "ui/node.h"
#include "ui/style.h"
#include "ui/tree_internal.h"

#include <cstdlib>
#include <cstdio>
#include <map>
#include <string>
#include <tuple>

namespace gea::framework::app::generated {
void drainMicrotasks();
}  // namespace gea::framework::app::generated

extern void __gea_top_level();

namespace {

bool expectEqual(int actual, int expected, const char *label)
{
	if (actual == expected) return true;
	std::fprintf(stderr, "[test_gea_reactive_counter_fonts] %s expected %d, got %d\n", label, expected, actual);
	return false;
}

bool expectNear(int actual, int expected, int tolerance, const char *label)
{
	if (std::abs(actual - expected) <= tolerance) return true;
	std::fprintf(stderr,
	             "[test_gea_reactive_counter_fonts] %s expected %d +/- %d, got %d\n",
	             label,
	             expected,
	             tolerance,
	             actual);
	return false;
}

// A font has one baseline for a given line box. Centering each glyph's ink
// independently makes digits, symbols and descenders jump when text changes.
// Recover the baseline from painted glyph metrics, then compare it across the
// substitutions within each layout mode and line-box height.
// A constrained box can clamp the baseline, so different heights are separate cases.
bool expectStableBaseline(const char *text,
                          const gea::framework::graphics::RasterizedFont &font,
                          int inkTop,
                          int inkBottom,
                          int boxTop,
                          int boxHeight,
                          const char *label)
{
	if (inkTop < boxTop || inkBottom >= boxTop + boxHeight) {
		std::fprintf(stderr, "[test_gea_reactive_counter_fonts] %s ink %d..%d escapes box %d..%d\n",
		             label, inkTop, inkBottom, boxTop, boxTop + boxHeight - 1);
		return false;
	}
	gea::framework::graphics::Glyph glyph{};
	if (!text || !text[0] || text[1] || !font.glyph(text[0], &glyph)) return false;
	const int baselineFromCenter2 = 2 * (inkTop + glyph.bearingY) - (2 * boxTop + boxHeight - 1);
	static std::map<std::tuple<std::string, int, int>, int> baselines;
	const auto inserted = baselines.emplace(std::make_tuple(std::string(label), font.sizePx(), boxHeight), baselineFromCenter2);
	if (!inserted.second && std::abs(baselineFromCenter2 - inserted.first->second) > 2) {
		std::fprintf(stderr, "glyph=%s size=%d boxHeight=%d inkTop=%d bearingY=%d\n", text, font.sizePx(), boxHeight, inkTop, glyph.bearingY);
	}
	// Rasterized bounds can trim a transparent edge row; allow one pixel.
	return inserted.second || expectNear(baselineFromCenter2, inserted.first->second, 2, label);
}

bool assertCounterFonts(int width, int height, double devicePixelRatio, int titleSize, int countSize, int labelSize, int resetSize)
{
	using namespace gea::embedded::ui;

	Tree::instance().clear();
	StyleSheet::instance().clear();
	setViewportMetrics(width, height, devicePixelRatio);

	StyleSheet::instance().registerRule("counter-title", "font-size", "clamp(24px, 7vh, 92px)");
	StyleSheet::instance().registerRule("counter-count", "font-size", "clamp(46px, 17vh, 220px)");
	StyleSheet::instance().registerRule("counter-minus-label", "font-size", "clamp(24px, 9vh, 120px)");
	StyleSheet::instance().registerRule("counter-reset-label", "font-size", "clamp(12px, 4.2vh, 52px)");

	const int titleId = Tree::instance().createText();
	NodeHandle(titleId).classList().add("counter-title");
	if (!expectEqual(Tree::instance().node(titleId).style.font_size, titleSize, "title font-size")) return false;

	const int countId = Tree::instance().createText();
	NodeHandle(countId).classList().add("counter-count");
	if (!expectEqual(Tree::instance().node(countId).style.font_size, countSize, "count font-size")) return false;

	const int labelId = Tree::instance().createText();
	NodeHandle(labelId).classList().add("counter-minus-label");
	if (!expectEqual(Tree::instance().node(labelId).style.font_size, labelSize, "control label font-size")) return false;

	const int resetId = Tree::instance().createText();
	NodeHandle(resetId).classList().add("counter-reset-label");
	if (!expectEqual(Tree::instance().node(resetId).style.font_size, resetSize, "reset label font-size")) return false;

	return true;
}

bool assertCounterCornerRadii()
{
	using namespace gea::embedded::ui;

	Tree::instance().clear();
	StyleSheet::instance().clear();
	setViewportMetrics(720, 1440, 1.0);

	StyleSheet::instance().registerRule("counter-card", "border-radius", "clamp(14px, 4vh, 42px)");

	const int cardId = Tree::instance().createView();
	NodeHandle(cardId).classList().add("counter-card");
	const auto &style = Tree::instance().node(cardId).style;
	for (int corner = 0; corner < 4; ++corner) {
		if (!expectEqual(style.border_radius[corner], 42, "counter-card border-radius corner")) return false;
	}

	return true;
}

bool paintedBounds(int x0, int y0, int x1, int y1, int *outX0, int *outY0, int *outX1, int *outY1)
{
	bool any = false;
	int bx0 = 0;
	int by0 = 0;
	int bx1 = -1;
	int by1 = -1;
	for (int y = y0; y <= y1; ++y) {
		for (int x = x0; x <= x1; ++x) {
			if (gea::embedded::test::displayPixelAt(x, y) == 0) continue;
			if (!any) {
				bx0 = bx1 = x;
				by0 = by1 = y;
				any = true;
			} else {
				if (x < bx0) bx0 = x;
				if (x > bx1) bx1 = x;
				if (y < by0) by0 = y;
				if (y > by1) by1 = y;
			}
		}
	}
	if (!any) return false;
	*outX0 = bx0;
	*outY0 = by0;
	*outX1 = bx1;
	*outY1 = by1;
	return true;
}

bool commandTextInkBounds(const gea::embedded::ui::DisplayCommand &command,
                          int *outX0,
                          int *outY0,
                          int *outX1,
                          int *outY1)
{
	using namespace gea::framework::graphics;

	if (command.type != gea::embedded::ui::DisplayCommandType::DrawText ||
	    !command.text.text ||
	    !command.text.text[0] ||
	    command.text.fontId < 0)
		return false;

	const int fontSize = static_cast<int>(command.text.scale * 16.0f + 0.5f);
	const RasterizedFont font = FontRegistry::rasterizedFamily(command.text.fontId, fontSize);
	if (!font.valid()) return false;

	const int lineAdvance = command.text.lineHeight > 0 ? command.text.lineHeight : font.lineHeight();
	const int lineBoxOffset = command.text.lineHeight > 0 ? (lineAdvance - font.lineHeight()) / 2 : 0;
	const int lineY = command.text.y + lineBoxOffset;

	bool any = false;
	int bx0 = 0;
	int by0 = 0;
	int bx1 = -1;
	int by1 = -1;
	int penX = command.text.x;
	for (const char *p = command.text.text; *p && *p != '\n';) {
		const unsigned char c = static_cast<unsigned char>(*p++);
		int cp = c;
		if (c >= 0x80) continue;

		Glyph glyph{};
		if (!font.glyph(cp, &glyph)) {
			penX += font.sizePx() / 2;
			continue;
		}
		const int gx0 = penX + glyph.bearingX;
		const int gy0 = lineY + font.ascender() - glyph.bearingY;
		const int gx1 = gx0 + glyph.width - 1;
		const int gy1 = gy0 + glyph.height - 1;
		if (!any) {
			bx0 = gx0;
			by0 = gy0;
			bx1 = gx1;
			by1 = gy1;
			any = true;
		} else {
			if (gx0 < bx0) bx0 = gx0;
			if (gy0 < by0) by0 = gy0;
			if (gx1 > bx1) bx1 = gx1;
			if (gy1 > by1) by1 = gy1;
		}
		penX += glyph.advance;
	}

	if (!any) return false;
	*outX0 = bx0;
	*outY0 = by0;
	*outX1 = bx1;
	*outY1 = by1;
	return true;
}

bool expectActualClassTextBaseline(const char *className)
{
	using namespace gea::embedded::ui;

	const auto nodes = gea::embedded::test::nodesWithClass(className);
	if (nodes.size() != 1) {
		std::fprintf(stderr, "[test_gea_reactive_counter_fonts] expected one node for %s, got %zu\n", className, nodes.size());
		return false;
	}

	const Tree &tree = Tree::instance();
	const Node &textNode = tree.node(nodes[0]);
	if (textNode.parent < 0) {
		std::fprintf(stderr, "[test_gea_reactive_counter_fonts] expected %s to have a parent\n", className);
		return false;
	}
	const Node &parent = tree.node(textNode.parent);

	int x0 = 0;
	int y0 = 0;
	int x1 = -1;
	int y1 = -1;
	const DisplayCommand *textCommand = nullptr;
	for (int i = 0; i < DisplayList::instance().nodeCommandCount(nodes[0]); ++i) {
		const DisplayCommand *command = DisplayList::instance().nodeCommandAt(nodes[0], i);
		if (command && command->type == DisplayCommandType::DrawText) {
			textCommand = command;
			break;
		}
	}
	if (!textCommand || !commandTextInkBounds(*textCommand, &x0, &y0, &x1, &y1)) {
		std::fprintf(stderr, "[test_gea_reactive_counter_fonts] expected text command ink bounds for %s\n", className);
		return false;
	}

	const int fontSize = static_cast<int>(textCommand->text.scale * 16.0f + 0.5f);
	const auto font = gea::framework::graphics::FontRegistry::rasterizedFamily(textCommand->text.fontId, fontSize);
	return expectStableBaseline(textCommand->text.text, font, y0, y1,
	                            parent.layout.y, parent.layout.height, "counter label baseline");
}

bool assertActualReactiveCounterTextBaselines()
{
	using namespace gea::embedded::ui;

	gea::embedded::test::resetNativeHost();
	gea::embedded::test::setNativeDisplaySize(720, 1440);
	Document::setPreferredMountSize(720, 1440);
	setViewportMetrics(720, 1440, 2.0);
	__gea_top_level();
	gea::embedded::test::refresh();

	if (!expectActualClassTextBaseline("counter-count")) return false;
	if (!expectActualClassTextBaseline("counter-minus-label")) return false;
	if (!expectActualClassTextBaseline("counter-plus-label")) return false;
	return true;
}

bool assertGeneratedTextBaseline(const char *text, int fontSize)
{
	using namespace gea::embedded::ui;
	using namespace gea::framework::graphics;

	const int familyId = FontRegistry::familyId("Oswald");
	if (familyId < 0) {
		std::fprintf(stderr, "[test_gea_reactive_counter_fonts] expected generated Oswald font family\n");
		return false;
	}
	const RasterizedFont font = FontRegistry::rasterizedFamily(familyId, fontSize);
	if (!font.valid()) {
		std::fprintf(stderr, "[test_gea_reactive_counter_fonts] expected Oswald %dpx atlas\n", fontSize);
		return false;
	}

	gea::embedded::test::resetNativeHost();
	gea::embedded::test::setNativeDisplaySize(720, 720);
	DisplayList::instance().clear();

	Node node{};
	node.type = NodeType::Text;
	node.text = text;
	node.layout.x = 120;
	node.layout.y = 160;
	node.layout.width = 480;
	node.layout.height = font.lineHeight();
	node.style.font_id = familyId;
	node.style.font_size = fontSize;
	node.style.text_color = 0xffff;
	node.style.text_alpha = 255;
	node.style.text_align = 1;
	node.style.white_space = 1;

	TextRenderer::record(node);
	DisplayList::instance().replay();

	int x0 = 0;
	int y0 = 0;
	int x1 = -1;
	int y1 = -1;
	if (!paintedBounds(0, 0, 719, 719, &x0, &y0, &x1, &y1)) {
		std::fprintf(stderr, "[test_gea_reactive_counter_fonts] expected painted pixels for '%s'\n", text);
		return false;
	}

	return expectStableBaseline(text, font, y0, y1, node.layout.y, node.layout.height,
	                            "generated text baseline");
}

bool assertFlexGeneratedTextBaseline(const char *text, int fontSize, int parentWidth, int parentHeight)
{
	using namespace gea::embedded::ui;
	using namespace gea::framework::graphics;

	const int familyId = FontRegistry::familyId("Oswald");
	if (familyId < 0) {
		std::fprintf(stderr, "[test_gea_reactive_counter_fonts] expected generated Oswald font family\n");
		return false;
	}

	gea::embedded::test::resetNativeHost();
	gea::embedded::test::setNativeDisplaySize(720, 720);

	Tree::instance().clear();
	StyleSheet::instance().clear();

	const int rootId = Tree::instance().createView();
	NodeHandle(rootId).style().set(Property::Display, kDisplayFlex);
	NodeHandle(rootId).style().set(Property::FlexDirection, 0);
	NodeHandle(rootId).style().set(Property::JustifyContent, 1);
	NodeHandle(rootId).style().set(Property::AlignItems, 1);
	NodeHandle(rootId).style().set(Property::Width, 720);
	NodeHandle(rootId).style().set(Property::Height, 720);

	const int parentId = Tree::instance().createView();
	NodeHandle(parentId).style().set(Property::Display, kDisplayFlex);
	NodeHandle(parentId).style().set(Property::FlexDirection, 0);
	NodeHandle(parentId).style().set(Property::JustifyContent, 1);
	NodeHandle(parentId).style().set(Property::AlignItems, 1);
	NodeHandle(parentId).style().set(Property::Width, parentWidth);
	NodeHandle(parentId).style().set(Property::Height, parentHeight);
	NodeHandle(rootId).appendChild(NodeHandle(parentId));

	const int textId = Tree::instance().createText();
	Tree::instance().setText(textId, text);
	NodeHandle(textId).style().set(Property::FontId, familyId);
	NodeHandle(textId).style().set(Property::FontSize, fontSize);
	NodeHandle(textId).style().set(Property::Color, 0xffff);
	NodeHandle(textId).style().set(Property::TextAlign, 1);
	NodeHandle(textId).style().set(Property::WhiteSpace, 1);
	NodeHandle(parentId).appendChild(NodeHandle(textId));

	Tree::instance().mount(rootId, 720, 720);

	int x0 = 0;
	int y0 = 0;
	int x1 = -1;
	int y1 = -1;
	if (!paintedBounds(0, 0, 719, 719, &x0, &y0, &x1, &y1)) {
		std::fprintf(stderr, "[test_gea_reactive_counter_fonts] expected painted pixels for flex '%s'\n", text);
		return false;
	}

	const Node &laidOutParent = Tree::instance().node(parentId);
	const auto font = FontRegistry::rasterizedFamily(familyId, fontSize);
	return expectStableBaseline(text, font, y0, y1, laidOutParent.layout.y, laidOutParent.layout.height,
	                            "flex-generated text baseline");
}

bool assertFlexGeneratedSpanBaseline(const char *text, int fontSize, int parentWidth, int parentHeight)
{
	using namespace gea::embedded::ui;
	using namespace gea::framework::graphics;

	const int familyId = FontRegistry::familyId("Oswald");
	if (familyId < 0) {
		std::fprintf(stderr, "[test_gea_reactive_counter_fonts] expected generated Oswald font family\n");
		return false;
	}

	gea::embedded::test::resetNativeHost();
	gea::embedded::test::setNativeDisplaySize(720, 720);

	Tree::instance().clear();
	StyleSheet::instance().clear();

	const int rootId = Tree::instance().createView();
	NodeHandle(rootId).style().set(Property::Display, kDisplayFlex);
	NodeHandle(rootId).style().set(Property::FlexDirection, 0);
	NodeHandle(rootId).style().set(Property::JustifyContent, 1);
	NodeHandle(rootId).style().set(Property::AlignItems, 1);
	NodeHandle(rootId).style().set(Property::Width, 720);
	NodeHandle(rootId).style().set(Property::Height, 720);

	const int parentId = Tree::instance().createView();
	NodeHandle(parentId).style().set(Property::Display, kDisplayFlex);
	NodeHandle(parentId).style().set(Property::FlexDirection, 0);
	NodeHandle(parentId).style().set(Property::JustifyContent, 1);
	NodeHandle(parentId).style().set(Property::AlignItems, 1);
	NodeHandle(parentId).style().set(Property::Width, parentWidth);
	NodeHandle(parentId).style().set(Property::Height, parentHeight);
	NodeHandle(rootId).appendChild(NodeHandle(parentId));

	const int spanId = Tree::instance().createView();
	NodeHandle(spanId).style().set(Property::FontId, familyId);
	NodeHandle(spanId).style().set(Property::FontSize, fontSize);
	NodeHandle(spanId).style().set(Property::Color, 0xffff);
	NodeHandle(parentId).appendChild(NodeHandle(spanId));

	const int textId = Tree::instance().createText();
	Tree::instance().setText(textId, text);
	NodeHandle(textId).style().set(Property::FontId, familyId);
	NodeHandle(textId).style().set(Property::FontSize, fontSize);
	NodeHandle(textId).style().set(Property::Color, 0xffff);
	NodeHandle(textId).style().set(Property::TextAlign, 1);
	NodeHandle(textId).style().set(Property::WhiteSpace, 1);
	NodeHandle(spanId).appendChild(NodeHandle(textId));

	Tree::instance().mount(rootId, 720, 720);

	int x0 = 0;
	int y0 = 0;
	int x1 = -1;
	int y1 = -1;
	if (!paintedBounds(0, 0, 719, 719, &x0, &y0, &x1, &y1)) {
		std::fprintf(stderr, "[test_gea_reactive_counter_fonts] expected painted pixels for flex span '%s'\n", text);
		return false;
	}

	const Node &laidOutParent = Tree::instance().node(parentId);
	const auto font = FontRegistry::rasterizedFamily(familyId, fontSize);
	return expectStableBaseline(text, font, y0, y1, laidOutParent.layout.y, laidOutParent.layout.height,
	                            "flex-generated span baseline");
}

}  // namespace

int main()
{
	if (!assertActualReactiveCounterTextBaselines()) return 1;

	gea::embedded::test::resetNativeHost();

	if (!assertCounterFonts(720, 1440, 1.0, 92, 220, 120, 52)) return 1;
	if (!assertCounterFonts(720, 1440, 2.0, 101, 245, 130, 60)) return 1;
	if (!assertCounterFonts(200, 200, 1.0, 24, 46, 24, 12)) return 1;
	if (!assertCounterCornerRadii()) return 1;
	if (!assertGeneratedTextBaseline("-", 120)) return 1;
	if (!assertGeneratedTextBaseline("+", 120)) return 1;
	if (!assertGeneratedTextBaseline("0", 220)) return 1;
	if (!assertFlexGeneratedTextBaseline("-", 120, 240, 154)) return 1;
	if (!assertFlexGeneratedTextBaseline("+", 120, 240, 154)) return 1;
	if (!assertFlexGeneratedTextBaseline("0", 220, 620, 360)) return 1;
	if (!assertFlexGeneratedSpanBaseline("-", 120, 240, 154)) return 1;
	if (!assertFlexGeneratedSpanBaseline("+", 120, 240, 154)) return 1;
	if (!assertFlexGeneratedSpanBaseline("0", 220, 620, 360)) return 1;
	if (!assertGeneratedTextBaseline("-", 130)) return 1;
	if (!assertGeneratedTextBaseline("+", 130)) return 1;
	if (!assertGeneratedTextBaseline("0", 245)) return 1;
	if (!assertFlexGeneratedTextBaseline("-", 130, 245, 173)) return 1;
	if (!assertFlexGeneratedTextBaseline("+", 130, 245, 173)) return 1;
	if (!assertFlexGeneratedTextBaseline("0", 245, 620, 346)) return 1;
	if (!assertFlexGeneratedSpanBaseline("-", 130, 245, 173)) return 1;
	if (!assertFlexGeneratedSpanBaseline("+", 130, 245, 173)) return 1;
	if (!assertFlexGeneratedSpanBaseline("0", 245, 620, 346)) return 1;

	for (int size : {120, 130, 220, 245}) {
		for (const char *text : {"0", "A", "g", "+", "-"}) {
			if (!assertGeneratedTextBaseline(text, size)) return 1;
			if (!assertFlexGeneratedTextBaseline(text, size, 620, 400)) return 1;
			if (!assertFlexGeneratedSpanBaseline(text, size, 620, 400)) return 1;
		}
	}

	return 0;
}

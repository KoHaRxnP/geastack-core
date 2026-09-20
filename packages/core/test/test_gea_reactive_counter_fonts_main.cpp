#include "native_test_harness.h"

#include "graphics/font.h"
#include "ui/document.h"
#include "ui/internal.h"
#include "ui/node.h"
#include "ui/style.h"
#include "ui/tree_internal.h"

#include <cstdlib>
#include <cstdio>

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

bool expectActualClassTextCentered(const char *className)
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

	const int paintedCenter2 = y0 + y1;
	const int parentCenter2 = parent.layout.y * 2 + parent.layout.height - 1;
	if (!expectNear(paintedCenter2, parentCenter2, 2, className)) {
		const int fontSize = textCommand ? static_cast<int>(textCommand->text.scale * 16.0f + 0.5f) : 0;
		const auto font = gea::framework::graphics::FontRegistry::rasterizedFamily(textCommand ? textCommand->text.fontId : -1, fontSize);
		std::fprintf(stderr,
		             "[test_gea_reactive_counter_fonts] %s parent=(y=%d h=%d) text=(y=%d h=%d font=%d size=%d line=%d cmdY=%d)\n",
		             className,
		             parent.layout.y,
		             parent.layout.height,
		             textNode.layout.y,
		             textNode.layout.height,
		             textCommand ? textCommand->text.fontId : -1,
		             fontSize,
		             font.valid() ? font.lineHeight() : -1,
		             textCommand ? textCommand->text.y : -1);
		return false;
	}
	return true;
}

bool assertActualReactiveCounterTextCenters()
{
	using namespace gea::embedded::ui;

	gea::embedded::test::resetNativeHost();
	gea::embedded::test::setNativeDisplaySize(720, 1440);
	Document::setPreferredMountSize(720, 1440);
	setViewportMetrics(720, 1440, 2.0);
	__gea_top_level();
	gea::embedded::test::refresh();

	if (!expectActualClassTextCentered("counter-count")) return false;
	if (!expectActualClassTextCentered("counter-minus-label")) return false;
	if (!expectActualClassTextCentered("counter-plus-label")) return false;
	return true;
}

bool assertGeneratedTextInkCentersInLineBox(const char *text, int fontSize)
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

	const int paintedCenter2 = y0 + y1;
	const int boxCenter2 = node.layout.y * 2 + node.layout.height - 1;
	return expectNear(paintedCenter2, boxCenter2, 2, "generated text ink vertical center");
}

bool assertFlexCenteredGeneratedTextInk(const char *text, int fontSize, int parentWidth, int parentHeight)
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
	Node &root = Tree::instance().node(rootId);
	root.style.display = kDisplayFlex;
	root.style.flex_direction = 0;
	root.style.flex_direction_explicit = 1;
	root.style.justify_content = 1;
	root.style.align_items = 1;
	root.style.width = 720;
	root.style.height = 720;

	const int parentId = Tree::instance().createView();
	Node &parent = Tree::instance().node(parentId);
	parent.style.display = kDisplayFlex;
	parent.style.flex_direction = 0;
	parent.style.flex_direction_explicit = 1;
	parent.style.justify_content = 1;
	parent.style.align_items = 1;
	parent.style.width = parentWidth;
	parent.style.height = parentHeight;
	NodeHandle(rootId).appendChild(NodeHandle(parentId));

	const int textId = Tree::instance().createText();
	Tree::instance().setText(textId, text);
	Node &textNode = Tree::instance().node(textId);
	textNode.style.font_id = familyId;
	textNode.style.font_size = fontSize;
	textNode.style.text_color = 0xffff;
	textNode.style.text_alpha = 255;
	textNode.style.text_align = 1;
	textNode.style.white_space = 1;
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
	const int paintedCenter2 = y0 + y1;
	const int parentCenter2 = laidOutParent.layout.y * 2 + laidOutParent.layout.height - 1;
	return expectNear(paintedCenter2, parentCenter2, 2, "flex-centered generated text ink");
}

bool assertFlexCenteredGeneratedSpanInk(const char *text, int fontSize, int parentWidth, int parentHeight)
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
	Node &root = Tree::instance().node(rootId);
	root.style.display = kDisplayFlex;
	root.style.flex_direction = 0;
	root.style.flex_direction_explicit = 1;
	root.style.justify_content = 1;
	root.style.align_items = 1;
	root.style.width = 720;
	root.style.height = 720;

	const int parentId = Tree::instance().createView();
	Node &parent = Tree::instance().node(parentId);
	parent.style.display = kDisplayFlex;
	parent.style.flex_direction = 0;
	parent.style.flex_direction_explicit = 1;
	parent.style.justify_content = 1;
	parent.style.align_items = 1;
	parent.style.width = parentWidth;
	parent.style.height = parentHeight;
	NodeHandle(rootId).appendChild(NodeHandle(parentId));

	const int spanId = Tree::instance().createView();
	Node &span = Tree::instance().node(spanId);
	span.style.font_id = familyId;
	span.style.font_size = fontSize;
	span.style.text_color = 0xffff;
	span.style.text_alpha = 255;
	NodeHandle(parentId).appendChild(NodeHandle(spanId));

	const int textId = Tree::instance().createText();
	Tree::instance().setText(textId, text);
	Node &textNode = Tree::instance().node(textId);
	textNode.style.font_id = familyId;
	textNode.style.font_size = fontSize;
	textNode.style.text_color = 0xffff;
	textNode.style.text_alpha = 255;
	textNode.style.text_align = 1;
	textNode.style.white_space = 1;
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
	const int paintedCenter2 = y0 + y1;
	const int parentCenter2 = laidOutParent.layout.y * 2 + laidOutParent.layout.height - 1;
	return expectNear(paintedCenter2, parentCenter2, 2, "flex-centered generated span ink");
}

}  // namespace

int main()
{
	if (!assertActualReactiveCounterTextCenters()) return 1;

	gea::embedded::test::resetNativeHost();

	if (!assertCounterFonts(720, 1440, 1.0, 92, 220, 120, 52)) return 1;
	if (!assertCounterFonts(720, 1440, 2.0, 101, 245, 130, 60)) return 1;
	if (!assertCounterFonts(200, 200, 1.0, 24, 46, 24, 12)) return 1;
	if (!assertCounterCornerRadii()) return 1;
	if (!assertGeneratedTextInkCentersInLineBox("-", 120)) return 1;
	if (!assertGeneratedTextInkCentersInLineBox("+", 120)) return 1;
	if (!assertGeneratedTextInkCentersInLineBox("0", 220)) return 1;
	if (!assertFlexCenteredGeneratedTextInk("-", 120, 240, 154)) return 1;
	if (!assertFlexCenteredGeneratedTextInk("+", 120, 240, 154)) return 1;
	if (!assertFlexCenteredGeneratedTextInk("0", 220, 620, 360)) return 1;
	if (!assertFlexCenteredGeneratedSpanInk("-", 120, 240, 154)) return 1;
	if (!assertFlexCenteredGeneratedSpanInk("+", 120, 240, 154)) return 1;
	if (!assertFlexCenteredGeneratedSpanInk("0", 220, 620, 360)) return 1;
	if (!assertGeneratedTextInkCentersInLineBox("-", 130)) return 1;
	if (!assertGeneratedTextInkCentersInLineBox("+", 130)) return 1;
	if (!assertGeneratedTextInkCentersInLineBox("0", 245)) return 1;
	if (!assertFlexCenteredGeneratedTextInk("-", 130, 245, 173)) return 1;
	if (!assertFlexCenteredGeneratedTextInk("+", 130, 245, 173)) return 1;
	if (!assertFlexCenteredGeneratedTextInk("0", 245, 620, 346)) return 1;
	if (!assertFlexCenteredGeneratedSpanInk("-", 130, 245, 173)) return 1;
	if (!assertFlexCenteredGeneratedSpanInk("+", 130, 245, 173)) return 1;
	if (!assertFlexCenteredGeneratedSpanInk("0", 245, 620, 346)) return 1;

	return 0;
}

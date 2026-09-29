#include "display.h"
#include "image.h"
#include "graphics/font.h"
#include "native_test_harness.h"
#include "ui/document.h"
#include "ui/internal.h"
#include "ui/node.h"
#include "ui/node_lifecycle.h"
#include "ui/style.h"
#include "ui/refresh_perf.h"
#include "ui/tree_internal.h"
#if __has_include("ui/tree_inspection.h")
#include "ui/tree_inspection.h"
#endif
#include "ui/tree_state.h"
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>
namespace gea::framework::app::generated
{
void drainMicrotasks() {}
} // namespace gea::framework::app::generated
namespace gea::framework::graphics::generated
{
void ensureLinked() {}
const RasterizedFontData *lookupFontForFamily(int, int)
{
	static const std::uint8_t atlas[]{255, 128, 0,	 255, 0,   255, 128, 0,
									  0,   128, 255, 0,	  255, 0,	0,	 255};
	static const Glyph glyphs[]{{32, 0, 0, 0, 0, 6, 0, 0},
								{88, 0, 0, 4, 4, 6, 0, 4}};
	static const RasterizedFontData font{9301, 4,	   8, 4, -4,
										 2,	   glyphs, 4, 4, atlas};
	return &font;
}
} // namespace gea::framework::graphics::generated
#ifndef GEA_CSS_RADIUS_INDEX
#define GEA_CSS_RADIUS_INDEX(index) (index)
#endif
#ifndef GEA_CSS_CORNER_RADIUS
#define GEA_CSS_CORNER_RADIUS 1
#endif
#ifndef GEA_CSS_AXIS_GAP
#define GEA_CSS_AXIS_GAP 1
#endif
using namespace gea::embedded::test;
using namespace gea::embedded::ui;
using gea::platform::display::Display;
int main()
{
	std::fprintf(stderr, "Node=%zu TreeState=%zu RareStyle=%zu\n", sizeof(Node),
				 sizeof(TreeState), sizeof(RareStyle));
	// Alpha storage is independent for text and borders. Opaque updates and
	// currentColor must agree in every layout; retained fields keep transparency.
	{
		resetNativeHost();
		auto root = Document::instance().createView();
		auto child = Document::instance().createView();
		root.appendChild(child);
		for (const char *color : {"#123", "#123f", "#123456", "#123456ff"}) {
			root.style().setProperty("color", color);
			child.style().setProperty("border", "2px solid currentColor");
			Document::instance().refresh(root, 160, 120);
			const auto &parentStyle = Tree::instance().node(root.id()).style;
			const auto &childStyle = Tree::instance().node(child.id()).style;
			assert(parentStyle.text_alpha == 255 && childStyle.text_alpha == 255);
			assert(childStyle.text_color == parentStyle.text_color);
			assert(borderPaintAlpha(childStyle, -1) == 255);
		}
#if GEA_CSS_TEXT_ALPHA
		root.style().setProperty("color", "#12345680");
		Document::instance().refresh(root, 160, 120);
		assert(Tree::instance().node(root.id()).style.text_alpha == 128);
		assert(Tree::instance().node(child.id()).style.text_alpha == 128);
		assert(borderPaintAlpha(Tree::instance().node(child.id()).style, -1) == 128);
#endif
#if GEA_CSS_BORDER_ALPHA
		child.style().setProperty("border-color", "#12345640");
		assert(Tree::instance().node(child.id()).style.border_alpha == 64);
		assert(borderPaintAlpha(Tree::instance().node(child.id()).style, -1) == 64);
		child.style().setProperty("border-color", "transparent");
		assert(Tree::instance().node(child.id()).style.border_alpha == 0);
#endif
		root.style().setProperty("color", "#fff");
		child.style().setProperty("border-color", "#fff");
		Document::instance().refresh(root, 160, 120);
		assert(Tree::instance().node(child.id()).style.text_alpha == 255);
		assert(Tree::instance().node(child.id()).style.border_alpha == 255);
	}
	// Every proven unsigned field retains the boundary value after CSS scaling
	// and inheritance. Values above it are covered by the wide native variant.
	{
		resetNativeHost();
		setViewportMetrics(600, 450, 2.0);
		auto n = Document::instance().createView();
		for (const char *property : {"padding", "gap", "border-width", "border-radius", "font-size", "line-height"})
			n.style().setProperty(property, "127.5px");
		n.style().setProperty("flex", "255 255 auto");
		const auto &style = Tree::instance().node(n.id()).style;
		assert(style.padding[0] == 255 && style.padding[3] == 255);
		assert(style.gap == 255 && style.border_width == 255);
		assert(style.border_radius[0] == 255 && style.font_size == 255);
		assert(style.line_height == 255 && style.flex == 255 && style.flex_shrink == 255);
		auto child = Document::instance().createView();
		n.appendChild(child);
		assert(Tree::instance().node(child.id()).style.font_size == 255);
		assert(Tree::instance().node(child.id()).style.line_height == 255);
		static_assert(sizeof(style.padding[0]) == (GEA_CSS_U8_PADDING ? 1 : 2));
		static_assert(sizeof(style.gap) == (GEA_CSS_U8_GAP ? 1 : 2));
		static_assert(sizeof(style.border_width) == (GEA_CSS_U8_BORDER ? 1 : 2));
		static_assert(sizeof(style.border_radius[0]) == (GEA_CSS_U8_RADIUS ? 1 : 2));
		static_assert(sizeof(style.font_size) == (GEA_CSS_U8_FONT ? 1 : 2));
		static_assert(sizeof(style.line_height) == (GEA_CSS_U8_LINE_HEIGHT ? 1 : 2));
		static_assert(sizeof(style.flex) == (GEA_CSS_U8_FLEX ? 1 : 2));
		static_assert(sizeof(style.flex_shrink) == (GEA_CSS_U8_FLEX ? 1 : 2));
#if !GEA_CSS_U8_PADDING
		n.style().setProperty("padding", "128px"); assert(style.padding[0] == 256);
#endif
#if !GEA_CSS_U8_FONT
		n.style().setProperty("font-size", "128px"); assert(style.font_size == 256);
#endif
#if !GEA_CSS_U8_FLEX
		n.style().setProperty("flex", "256 256 auto"); assert(style.flex == 256 && style.flex_shrink == 256);
#endif
	}
	// Native payload families retain their positive behavior independently.
	{
		resetNativeHost();
		setNativeDisplaySize(64, 32);
		setViewportMetrics(64, 32, 1.0);
		auto &tree = Tree::instance();
#if GEA_UI_IMAGE_NODES
		static gea::framework::graphics::pixel::native_t pixels[6];
		for (auto &pixel : pixels) pixel = gea::framework::graphics::pixel::nativeColor(255, 255, 255);
		const int image = tree.createImage();
		const int asset = gea::framework::graphics::ImageStore::instance().registerBuffer(pixels, 3, 2);
		tree.setStyle(image, Property::ImageId, asset);
		NodeHandle(image).style().setProperty("width", "3px");
		NodeHandle(image).style().setProperty("height", "2px");
		tree.mount(image, 64, 32);
		DisplayList::instance().replay();
		assert(tree.node(image).image_id == asset);
		assert(displayNonzeroPixelCount() == 6);
#else
		static_assert(Node::image_id == -1);
#endif
#if GEA_UI_INPUT_NODES
		const int input = tree.createView();
		tree.setTagName(input, "input");
		tree.setActiveInput(input);
		assert(tree.activeInputId() == input && tree.activeInputCaretVisible());
		tree.tickInputCaret(100);
		tree.tickInputCaret(700);
		assert(!tree.activeInputCaretVisible());
		tree.setActiveInput(-1);
		assert(tree.activeInputId() == -1);
#else
		static_assert(TreeState::activeInputId == -1 && !TreeState::caretVisible);
#endif
#if !GEA_CSS_SCROLLING
		static_assert(RenderState::non_scroll_dirty == 0);
#endif
	}
	// A clipped, rounded app root with absolute descendants exercises the
	// maintenance-screen shape; a nonzero background alone can hide a blank
	// descendant regression in simpler geometry fingerprints.
	{
		resetNativeHost();
		setNativeDisplaySize(600, 450);
		setViewportMetrics(600, 450, 2.0);
		auto &sheet = StyleSheet::instance();
		sheet.clear();
		sheet.registerRule("panel", "position", "relative");
		sheet.registerRule("panel", "width", "100%");
		sheet.registerRule("panel", "height", "100%");
		sheet.registerRule("panel", "min-height", "205px");
		sheet.registerRule("panel", "background", "#000000");
		sheet.registerRule("panel", "border-radius", "65px");
		sheet.registerRule("panel", "overflow", "hidden");
		sheet.registerRule("action", "position", "absolute");
		sheet.registerRule("action", "left", "28px");
		sheet.registerRule("action", "top", "151px");
		sheet.registerRule("action", "width", "calc(100% - 56px)");
		sheet.registerRule("action", "height", "28px");
		sheet.registerRule("action", "background", "#343d45");
		sheet.registerRule("action", "border-radius", "9px");
		auto panel = Document::instance().createView();
		panel.classList().set("panel");
		auto wrapper = Document::instance().createView();
		auto action = Document::instance().createView();
		action.classList().set("action");
		wrapper.appendChild(action);
		panel.appendChild(wrapper);
		Tree::instance().mount(panel.id(), 600, 450);
		Tree::instance().refresh(panel.id(), 600, 450);
		std::fprintf(stderr, "clipped panel pixels=%d action=%dx%d at %d,%d\n",
		             displayNonzeroPixelCount(), Tree::instance().node(action.id()).layout.width,
		             Tree::instance().node(action.id()).layout.height,
		             Tree::instance().node(action.id()).layout.x,
		             Tree::instance().node(action.id()).layout.y);
		assert(displayNonzeroPixelCount() > 1000);
	}
	// Event dispatch remains stable when callbacks erase themselves, grow the
	// unified list, mutate other event types or remove the whole node.
	{
		resetNativeHost();
		auto &tree = Tree::instance();
		const int node = tree.createView();
		using namespace gea::framework::events;
		std::vector<int> calls;
		EventListenerId first = 0, second = 0;
		first = tree.setEventListener(node, "click", [&](PointerEvent &) {
			calls.push_back(1);
			assert(tree.removeEventListener(node, "click", first));
			assert(tree.removeEventListener(node, "click", second));
			for (int i = 0; i < 20; ++i) tree.setEventListener(node, "input", [](PointerEvent &) {});
			tree.setEventListener(node, "click", [&](PointerEvent &) { calls.push_back(3); });
		});
		second = tree.setEventListener(node, "click", [&](PointerEvent &) { calls.push_back(2); });
		int touch = 0;
		const auto touchId = tree.setEventListener(node, "pointerdown", [&](PointerEvent &) { ++touch; });
		assert(!tree.removeEventListener(node, "click", touchId));
		PointerEvent event{PointerEventType::Click}; event.targetId = node;
		assert(tree.dispatchEvent(event));
		assert((calls == std::vector<int>{1}));
		assert(tree.dispatchEvent(event));
		assert((calls == std::vector<int>{1, 3}));
		event.type = PointerEventType::TouchStart;
		assert(tree.dispatchEvent(event) && touch == 1);
		assert(tree.removeEventListener(node, "touchstart", touchId));
		assert(!tree.hasListenersForType("pointerdown"));
		assert(tree.hasListenersForType("input"));
		tree.setEventListener(node, "click", [&](PointerEvent &) { tree.removeNode(node); });
		event.type = PointerEventType::Click;
		assert(tree.dispatchEvent(event));
		assert(!tree.hasListenersForType("click") && !tree.hasListenersForType("input"));
		const int reused = tree.createView();
		assert(!tree.hasEventListener(reused));
		assert(!tree.setEventListener(reused, "unsupported", [](PointerEvent &) {}));
	}
#if GEA_CSS_SCROLLING && GEA_CSS_OVERFLOW_AXES
	// visible computes to auto beside hidden. The analyzer must retain this
	// coupled-axis case; it is not equivalent to fully non-scrolling clipping.
	{
		resetNativeHost();
		setNativeDisplaySize(100, 100);
		setViewportMetrics(100, 100, 1.0);
		auto root = Document::instance().createView();
		root.style().width(100); root.style().height(100);
		root.style().setProperty("display", "flex");
		root.style().setProperty("flex-direction", "row");
		root.style().setProperty("overflow", "visible hidden");
		auto child = Document::instance().createView();
		child.style().width(240); child.style().height(20);
		child.style().setProperty("flex-shrink", "0");
		root.appendChild(child);
		// Document mounting applies the host's 410x502 preferred size; this
		// focused geometry case needs an explicit 100-pixel viewport.
		Tree::instance().mount(root.id(), 100, 100);
		Tree::instance().computeLayout(root.id(), 100, 100);
		assert(scrollsOverflowX(Tree::instance().node(root.id()).style));
		if (ViewRenderer::scrollMaxX(Tree::instance().node(root.id())) <= 0) {
			const auto &r = Tree::instance().node(root.id());
			const auto &c = Tree::instance().node(child.id());
			std::fprintf(stderr, "overflow fixture: ids=%d/%d parent=%d child=%d root=%dx%d content=%dx%d child=%dx%d style-width=%d/%d display=%d shrink=%d\n", root.id(), child.id(), c.parent, r.first_child, r.layout.width, r.layout.height, r.layout.scroll_content_width, r.layout.scroll_content_height, c.layout.width, c.layout.height, r.style.width, c.style.width, r.style.display, c.style.flex_shrink);
		}
		assert(ViewRenderer::scrollMaxX(Tree::instance().node(root.id())) > 0);
		Tree::instance().setScrollLeft(root.id(), 50);
		assert(Tree::instance().scrollLeft(root.id()) == 50);
		root.style().setProperty("overflow", "clip hidden");
		Document::instance().refresh(root, 100, 100);
		assert(!scrollsOverflowX(Tree::instance().node(root.id()).style));
		assert(Tree::instance().scrollLeft(root.id()) == 0);
	}
#endif
#if GEA_CSS_SCROLLING
	// Virtual-list allocation is optional; its large scroll dimensions remain
	// 32-bit and survive layout/cache changes.
	{
		resetNativeHost();
		auto &tree = Tree::instance();
		const int plain = tree.createView();
		assert(!ensureRareData(plain).virtualList);
		const int list = tree.createVirtualList();
		assert(rareDataFor(list)->virtualList);
		tree.setAttribute(list, "item-count", "5000");
		assert(VirtualListRenderer::virtualContentHeight(list, 259) == 1295000);
		assert(VirtualListRenderer::rowHeight(list) == 259);
		const int clone = tree.cloneNode(list, false);
		assert(VirtualListRenderer::itemCount(clone) == 0);
		tree.removeNode(list);
		const int reused = tree.createView();
		assert(!ensureRareData(reused).virtualList);
	}
#endif
	// Creating, copying and recycling other labels must not invalidate
	// retained display-list pointers or share mutable buffers between clones.
#if __has_include("ui/node_text.h")
	NodeText label;
	label.assign("short");
	const char *stable = label.c_str();
	NodeText clone = label;
	assert(clone == "short" && clone.c_str() != stable);
	const auto textPoolBefore = NodeText::storageBytes();
	for (int i = 0; i < 100; ++i) {
		NodeText transient;
		transient.assign("temporary long label that allocates");
	}
	assert(NodeText::storageBytes() <= textPoolBefore + 1024);
	assert(label.c_str() == stable && label == "short");
	clone.assign("changed");
	assert(label == "short");
	NodeText moved = std::move(clone);
	assert(clone.empty() && moved == "changed");
	moved.clear();
	assert(moved.empty());
	std::fprintf(stderr,
				 "NodeTextPool=%zu bytes including spare slots/map, excluding "
				 "character buffers/allocator metadata\n",
				 NodeText::storageBytes());
	label.assign(label.c_str() + 1);
	assert(label == "hort");
	std::vector<NodeText> pageLabels(100);
	for (auto &entry : pageLabels)
		entry.assign("X");
	assert(label.c_str() == stable && label == "hort");
	const auto highWater = NodeText::storageBytes();
	pageLabels.clear();
	pageLabels.resize(100);
	for (auto &entry : pageLabels)
		entry.assign("a different label");
	assert(NodeText::storageBytes() == highWater);
	assert(label.c_str() == stable && label == "hort");
	pageLabels.clear();
#endif
    // Attribute-free rare records must allocate no attribute block. Copies
    // remain independent and adding entries must preserve retained pointers.
    NodeAttributeStore attributes;
    assert(!attributes.values);
    attributes.set("id", "first");
    const char *attributePointer = attributes.get("id");
    attributes.set("data-value", "second");
    assert(attributes.get("id") == attributePointer);
    NodeAttributeStore copied = attributes;
    copied.set("id", "copy");
    assert(std::string(attributes.get("id")) == "first");
    assert(copied.get("id") != attributePointer);
    copied = copied;
    assert(std::string(copied.get("id")) == "copy");
    NodeAttributeStore movedAttributes = std::move(copied);
    assert(copied.count == 0 && !copied.values);
    assert(std::string(movedAttributes.get("id")) == "copy");
    const char *secondPointer = attributes.get("data-value");
    assert(attributes.remove("id"));
    assert(attributes.get("data-value") == secondPointer);
    assert(attributes.idAtom == kInvalidCssAtom);
    assert(attributes.remove("data-value"));
    assert(!attributes.values && attributes.count == 0);
    attributes = movedAttributes;
    attributes.clear();
    assert(!attributes.values && attributes.count == 0);
    assert(std::string(movedAttributes.get("id")) == "copy");
    // Exact-sized records grow safely, preserve other retained pointers,
    // handle overlapping input, and retain the prior public length limits.
    attributes.set("short", "x");
    const char *unchanged = attributes.get("short");
    attributes.set("grow", "a");
    attributes.set("grow", std::string(100, 'g').c_str());
    assert(std::strlen(attributes.get("grow")) == kNodeAttributeValueMax - 1);
    assert(attributes.get("short") == unchanged);
    attributes.set("grow", attributes.get("grow") + 3);
    assert(std::strlen(attributes.get("grow")) == kNodeAttributeValueMax - 4);
    attributes.set("grow", nullptr);
    assert(attributes.has("grow") && !*attributes.get("grow"));
    attributes.clear();
    for (int i = 0; i < kMaxNodeAttributes + 2; ++i) {
        std::string key = "attribute-" + std::to_string(i);
        attributes.set(key.c_str(), "value");
    }
    assert(attributes.count == kMaxNodeAttributes);
    assert(!attributes.has("attribute-8"));
    std::fprintf(stderr, "NodeAttributeStore=%zu NodeRareData=%zu; attribute header=%zu bytes plus actual bounded strings\n",
                 sizeof(NodeAttributeStore), sizeof(NodeRareData), sizeof(NodeAttributeEntry));
    // Binding a common border color must need neither a rare-style record nor
    // a larger node. Side color payloads stay sparse when they are actually used.
    Node borderNode{};
    NodeLifecycle::init(&borderNode, NodeType::View);
#if GEA_CSS_RARE_STYLE && !GEA_EMBEDDED_RARE_STYLE_INLINE
    const auto rareBeforeBorder = rareStylePool().size();
#endif
    assert(setBorderColorBinding(borderNode.style, -1, false));
    assert(!borderColorIsCurrent(borderNode.style));
    assert(borderNode.style.rare_style == -1);
#if GEA_CSS_RARE_STYLE && !GEA_EMBEDDED_RARE_STYLE_INLINE
    assert(rareStylePool().size() == rareBeforeBorder);
#endif
    assert(setBorderColorBinding(borderNode.style, -1, true));
    assert(borderColorIsCurrent(borderNode.style));
    assert(borderNode.style.rare_style == -1);
    NodeLifecycle::init(&borderNode, NodeType::View);
    assert(borderNode.style.border_color_flags == 0);
	// The sparse override block must retain ownership across both directions of
	// move assignment, growth, removal and reuse after clear.
	const Property overrideProperties[] = {
		Property::Width, Property::Height, Property::MinWidth, Property::MinHeight,
		Property::MaxWidth, Property::MaxHeight, Property::Opacity,
		Property::Color, Property::TextAlign, Property::Visibility};
	for (int count : {0, 1, 4, 5, 8, 10}) {
		NodeStyleOverrideStore original;
		for (int i = 0; i < count; ++i) original.set(overrideProperties[i], i + 1);
		assert((original.block != nullptr) == (count != 0));
		NodeStyleOverrideStore copy = original;
		if (count) {
			copy.set(overrideProperties[0], 99);
			assert(original.at(0).value == 1 && copy.at(0).value == 99);
		}
		NodeStyleOverrideStore movedOverrides = std::move(original);
		assert(original.empty() && movedOverrides.size() == static_cast<size_t>(count));
		original.set(Property::Width, 123);
		assert(original.size() == 1 && original.at(0).value == 123);
		for (int targetCount : {0, 4, 10}) {
			NodeStyleOverrideStore destination;
			for (int i = 0; i < targetCount; ++i) destination.set(overrideProperties[i], i);
			destination = movedOverrides;
			assert(destination.size() == movedOverrides.size());
			NodeStyleOverrideStore moveSource = movedOverrides;
			destination = std::move(moveSource);
			assert(moveSource.empty());
			assert(destination.size() == static_cast<size_t>(count));
			for (int i = 0; i < count; ++i) {
				assert(destination.at(i).property == overrideProperties[i]);
				assert(destination.at(i).value == i + 1);
			}
			for (int i = count - 1; i >= 0; --i) assert(destination.remove(overrideProperties[i]));
			assert(destination.empty());
			destination.set(Property::Height, 73);
			destination.clear();
			assert(destination.empty());
			destination.set(Property::Width, 42);
			assert(destination.size() == 1 && destination.at(0).value == 42);
		}
	}
	std::fprintf(stderr, "NodeStyleOverrideStore=%zu NodeRareData=%zu\n",
	             sizeof(NodeStyleOverrideStore), sizeof(NodeRareData));
	resetNativeHost();
	// Keep both memo slots useful, including MRU promotion. An unchanged box
	// under two available widths is the alternating flex-measurement case.
	{
		auto cached = Document::instance().createView();
		cached.style().setProperty("width", "20px");
		cached.style().setProperty("height", "10px");
		auto &engine = LayoutEngine::instance();
		engine.beginLayoutPass();
		engine.layoutNode(cached.id(), 100, 100);
		engine.layoutNode(cached.id(), 120, 100);
		const auto calls = refreshPerfStatsMutable().treeLayoutNodeCalls;
		const auto hits = refreshPerfStatsMutable().treeLayoutMemoHits;
		for (int i = 0; i < 32; ++i) engine.layoutNode(cached.id(), i % 2 ? 120 : 100, 100);
		assert(refreshPerfStatsMutable().treeLayoutNodeCalls == calls);
		assert(refreshPerfStatsMutable().treeLayoutMemoHits == hits + 32);
		auto &box = treeState().nodes[cached.id()].layout;
		box.width = 99;
		engine.layoutNode(cached.id(), 100, 100);
		assert(box.width == 20);
		assert(refreshPerfStatsMutable().treeLayoutNodeCalls == calls + 1);
		// A mutation outside the style API deliberately leaves the cached tag
		// alone. A new pass must still force layout, including a complete tag
		// cycle while this node is untouched.
		Tree::instance().nodes()[cached.id()].style.width = 40;
		for (int i = 0; i < 65535; ++i) engine.beginLayoutPass();
		engine.layoutNode(cached.id(), 120, 100);
		assert(Tree::instance().node(cached.id()).layout.width == 40);
		Tree::instance().nodes()[cached.id()].style.width = 60;
		for (int i = 0; i < 65535; ++i) engine.beginLayoutPass();
		engine.layoutNode(cached.id(), 100, 100);
		assert(Tree::instance().node(cached.id()).layout.width == 60);
	}
	resetNativeHost();
	setNativeDisplaySize(120, 120);
	setViewportMetrics(120, 120, 1.0);
	StyleSheet::instance().clear();
	StyleSheet::instance().registerRule("rounded", "border-radius", "5px");
	Display::clearNoFlush();
	auto root = Document::instance().createView();
	root.style().width(120);
	root.style().height(120);
	root.style().setProperty("background", "#123456");
	root.style().setProperty("display", "flex");
	root.style().setProperty("padding", "5px");
	root.style().setProperty("gap", "3px");
	auto card = Document::instance().createView();
	card.classList().set("rounded");
	card.style().setProperty("box-sizing", "border-box");
	card.style().setProperty("width", "calc(100% - 20px)");
	card.style().setProperty("height", "40px");
	card.style().setProperty("padding", "4px");
	card.style().setProperty("border", "2px solid #ffee00");
	card.style().setProperty("border-radius", "7px");
	card.style().setProperty("background", "#cc4422");
	card.style().set(Property::FontSize, 8);
	card.style().set(Property::FontId, 9301);
	card.style().setProperty("color", "white");
	auto caption = Document::instance().createText("X X X");
	card.appendChild(caption);
	root.appendChild(card);
	Document::instance().mount(root, 120, 120);
#if __has_include("ui/tree_inspection.h")
    assert(TreeInspection::nodeCount() == Tree::instance().nodeCount());
    assert(TreeInspection::mountedWidth() == Tree::instance().mountedWidth());
    assert(TreeInspection::mountedWidth() > 0);
    assert(TreeInspection::hasMountedText("X X X"));
    assert(!TreeInspection::hasMountedText(nullptr));
    assert(!TreeInspection::hasMountedText("absent"));
    auto detached = Document::instance().createText("detached");
    assert(!TreeInspection::hasMountedText("detached"));
    card.style().setProperty("display", "none");
    Document::instance().refresh(root, 120, 120);
    assert(!TreeInspection::hasMountedText("X X X"));
    card.style().setProperty("display", "block");
#endif
	for (int frame = 0; frame < 22; ++frame) {
		if (frame == 1)
			card.style().setProperty("width", "70px");
		if (frame == 2)
			card.style().setProperty("background", "#55aaee");
		if (frame == 3)
			card.style().setProperty("border", "0");
		if (frame == 4)
			caption.setText("X X X X X X X X X X X X X X X X X X X X X X X");
		if (frame == 5)
			caption.setText("X");
		if (frame == 6)
			caption.setText("");
		if (frame == 7)
			caption.setText("X X X");
		if (frame == 8) card.style().setProperty("border-radius", "0");
		if (frame == 9) card.style().setProperty("border-radius", "11px");
		if (frame == 10) card.style().setProperty("border-radius", "");
		if (frame == 11) card.style().setProperty("border-radius", "7px 7px 7px 7px");
		if (frame == 12) card.style().setProperty("border-radius", "");
		if (frame == 13) card.style().setProperty("border-radius", "9px");
		if (frame == 14) card.classList().set("");
		if (frame == 15) card.style().setProperty("border-radius", "");
		if (frame == 16) card.classList().set("rounded");
		if (frame == 17) card.classList().set("");
		// Clipped/visible overflow must retain painting and zero offsets even
		// when the unused scroll geometry and child-extent walk are absent.
		if (frame == 18) {
			card.style().setProperty("overflow", "hidden");
			caption.style().setProperty("width", "180px");
			caption.style().setProperty("height", "60px");
			caption.style().setProperty("background", "#335588");
		}
		if (frame == 19) card.style().setProperty("overflow", "clip");
		if (frame == 20) card.style().setProperty("overflow", "visible");
		if (frame == 21) card.style().setProperty("overflow", "hidden");
		Tree::instance().setScrollTop(card.id(), 20);
		Tree::instance().setScrollLeft(card.id(), 20);
		assert(Tree::instance().scrollTop(card.id()) == 0);
		assert(Tree::instance().scrollLeft(card.id()) == 0);
		Document::instance().refresh(root, 120, 120);
		DisplayList::instance().replay();
		std::uint64_t hash = 1469598103934665603ull;
		int painted = 0;
		for (int y = 0; y < 120; ++y)
			for (int x = 0; x < 120; ++x) {
				auto color = displayPixelAt(x, y);
				hash = (hash ^ color) * 1099511628211ull;
				painted += color != 0;
			}
		assert(painted > 100);
		const auto &box = Tree::instance().node(card.id()).layout;
		std::printf("%d %016llx %d %d %d %d\n", frame,
					static_cast<unsigned long long>(hash), box.x, box.y,
					box.width, box.height);
	}
    card.style().setProperty("border-radius", "9px");
    for (int corner = 0; corner < 4; ++corner)
        assert(Tree::instance().node(card.id()).style.border_radius[GEA_CSS_RADIUS_INDEX(corner)] == 9);
#if GEA_CSS_CORNER_RADIUS
    card.style().setProperty("border-radius", "1px 2px 3px 4px");
    for (int corner = 0; corner < 4; ++corner)
        assert(Tree::instance().node(card.id()).style.border_radius[corner] == corner + 1);
    card.style().setProperty("border-top-right-radius", "8px");
    assert(Tree::instance().node(card.id()).style.border_radius[1] == 8);
#else
    static_assert(sizeof(ComputedStyle::border_radius) == sizeof(ComputedStyle::border_radius[0]));
#endif
    card.style().setProperty("border-radius", "7px");
    root.style().setProperty("gap", "6px");
    assert(Tree::instance().node(root.id()).style.gap == 6);
#if GEA_CSS_AXIS_GAP
    root.style().setProperty("gap", "3px 5px");
    assert(Tree::instance().node(root.id()).style.row_gap == 3);
    assert(Tree::instance().node(root.id()).style.column_gap == 5);
    root.style().setProperty("column-gap", "8px");
    assert(Tree::instance().node(root.id()).style.column_gap == 8);
#else
    static_assert(ComputedStyle::row_gap == kUnset && ComputedStyle::column_gap == kUnset);
#endif
    root.style().setProperty("gap", "3px");
#if GEA_CSS_Z_INDEX
    card.style().setProperty("z-index", "7");
    assert(Tree::instance().node(card.id()).style.z_index == 7);
    assert(!Tree::instance().node(card.id()).style.z_index_auto);
    card.style().setProperty("z-index", "auto");
    assert(Tree::instance().node(card.id()).style.z_index_auto);
#else
    static_assert(ComputedStyle::z_index == 0 && ComputedStyle::z_index_auto == 1);
#endif
#if GEA_CSS_ASPECT_RATIO
    card.style().setProperty("aspect-ratio", "2 / 1");
    assert(rstyle(Tree::instance().node(card.id()).style).aspect_ratio != 0);
    card.style().setProperty("aspect-ratio", "auto");
#else
    static_assert(RareStyle::aspect_ratio == 0);
#endif
#if GEA_CSS_MARGIN_TRIM
    card.style().setProperty("margin-trim", "block");
    assert(rstyle(Tree::instance().node(card.id()).style).margin_trim != 0);
    card.style().setProperty("margin-trim", "none");
#else
    static_assert(RareStyle::margin_trim == 0);
#endif
#if GEA_CSS_CONTAINMENT
    card.style().setProperty("contain", "paint");
    assert(rstyle(Tree::instance().node(card.id()).style).containment & 16);
    card.style().setProperty("contain", "none");
#else
    static_assert(RareStyle::containment == 0);
#endif
#if GEA_CSS_JUSTIFY_SELF
    card.style().setProperty("justify-self", "center");
    assert(rstyle(Tree::instance().node(card.id()).style).justify_self == 1);
    card.style().setProperty("justify-self", "auto");
#else
    static_assert(RareStyle::justify_self == -1);
#endif
#if GEA_CSS_FLEX_LINE_COUNT
    card.style().setProperty("flex-line-count", "3");
    assert(rstyle(Tree::instance().node(card.id()).style).flex_line_count == 3);
    card.style().setProperty("flex-line-count", "1");
#else
    static_assert(RareStyle::flex_line_count == 1);
#endif
#if GEA_CSS_BOX_EXPRESSIONS
    card.style().setProperty("margin-left", "10%");
    assert(rstyle(Tree::instance().node(card.id()).style).margin_expression[3] >= 0);
    card.style().setProperty("margin-left", "3px");
    assert(rstyle(Tree::instance().node(card.id()).style).margin_expression[3] == -1);
    card.style().setProperty("padding-top", "calc(10% + 2px)");
    assert(rstyle(Tree::instance().node(card.id()).style).padding_expression[0] >= 0);
    card.style().setProperty("padding-top", "2px");
    assert(rstyle(Tree::instance().node(card.id()).style).padding_expression[0] == -1);
#else
    static_assert(RareStyle::margin_expression[0] == -1 && RareStyle::padding_expression[0] == -1);
#endif
    auto literalEdge = Document::instance().createView();
    literalEdge.style().setProperty("margin-left", "3px");
    literalEdge.style().setProperty("padding-top", "2px");
    assert(Tree::instance().node(literalEdge.id()).style.margin[3] == 3);
    assert(Tree::instance().node(literalEdge.id()).style.padding[0] == 2);
    assert(Tree::instance().node(literalEdge.id()).style.rare_style < 0);
#if GEA_CSS_BLINK
    Tree::instance().frame(0);
    card.style().set(Property::BlinkInterval, 100);
    Tree::instance().frame(100);
    assert(!Tree::instance().node(card.id()).style.blink_visible);
    Tree::instance().frame(200);
    assert(Tree::instance().node(card.id()).style.blink_visible);
    card.style().set(Property::BlinkInterval, 0);
    assert(Tree::instance().node(card.id()).style.blink_visible);
#else
    static_assert(ComputedStyle::blink_interval_ms == 0);
    static_assert(ComputedStyle::blink_visible == 1);
#endif
#if GEA_CSS_ORDER
    root.style().setProperty("flex-direction", "column");
    auto second = Document::instance().createView();
    second.style().width(10);
    second.style().height(10);
    root.appendChild(second);
    card.style().setProperty("order", "2");
    second.style().setProperty("order", "-1");
    Document::instance().refresh(root, 120, 120);
    assert(Tree::instance().node(second.id()).layout.y < Tree::instance().node(card.id()).layout.y);
    assert(Tree::instance().node(card.id()).style.order == 2);
#else
    static_assert(ComputedStyle::order == 0);
#endif

#if GEA_CSS_PERCENT_RADIUS
    card.style().setProperty("border-radius", "25%");
    Document::instance().refresh(root, 120, 120);
    assert(Tree::instance().node(card.id()).style.border_radius_percent[0] != kUnset);
    card.style().setProperty("border-radius", "3px");
    Document::instance().refresh(root, 120, 120);
    assert(Tree::instance().node(card.id()).style.border_radius_percent[0] == kUnset);
#else
    static_assert(ComputedStyle::border_radius_percent[0] == kUnset);
#endif
#if GEA_CSS_PERCENT_GAP
    root.style().setProperty("gap", "10%");
    Document::instance().refresh(root, 120, 120);
    assert(Tree::instance().node(root.id()).style.row_gap_percent != kUnset);
    root.style().set(Property::Gap, 3);
    assert(Tree::instance().node(root.id()).style.row_gap_percent == kUnset);
#else
    static_assert(ComputedStyle::row_gap_percent == kUnset);
#endif

#if GEA_CSS_WRITING_MODE
    root.style().setProperty("direction", "rtl");
    root.style().setProperty("writing-mode", "vertical-rl");
    Document::instance().refresh(root, 120, 120);
    assert(Tree::instance().node(root.id()).style.direction == 1);
    assert(Tree::instance().node(root.id()).style.writing_mode == 2);
#else
    static_assert(ComputedStyle::direction == 0);
    static_assert(ComputedStyle::writing_mode == 0);
#endif

    auto presentation = Document::instance().createView();
    root.appendChild(presentation);
#if GEA_CSS_OPACITY
    presentation.style().set(Property::Opacity, 128);
    assert(Tree::instance().node(presentation.id()).style.opacity == 128);
    Document::instance().refresh(root, 120, 120);
    assert(Tree::instance().node(presentation.id()).style.opacity == 128);
    presentation.style().set(Property::Opacity, 255);
    assert(Tree::instance().node(presentation.id()).style.opacity == 255);
#else
    static_assert(ComputedStyle::opacity == 255);
#endif
#if GEA_CSS_TEXT_DECORATION
    presentation.style().set(Property::TextDecoration, 1);
    assert(Tree::instance().node(presentation.id()).style.text_decoration == 1);
    Document::instance().refresh(root, 120, 120);
    assert(Tree::instance().node(presentation.id()).style.text_decoration == 1);
    presentation.style().set(Property::TextDecoration, 0);
    assert(Tree::instance().node(presentation.id()).style.text_decoration == 0);
#else
    static_assert(ComputedStyle::text_decoration == 0);
#endif
#if GEA_CSS_TEXT_TRANSFORM
    presentation.style().set(Property::TextTransform, 1);
    assert(Tree::instance().node(presentation.id()).style.text_transform == 1);
    Document::instance().refresh(root, 120, 120);
    assert(Tree::instance().node(presentation.id()).style.text_transform == 1);
    presentation.style().set(Property::TextTransform, 0);
    assert(Tree::instance().node(presentation.id()).style.text_transform == 0);
#else
    static_assert(ComputedStyle::text_transform == 0);
#endif
#if GEA_CSS_VISIBILITY
    presentation.style().set(Property::Visibility, 1);
    assert(Tree::instance().node(presentation.id()).style.visibility == 1);
    Document::instance().refresh(root, 120, 120);
    assert(Tree::instance().node(presentation.id()).style.visibility == 1);
    presentation.style().set(Property::Visibility, 0);
    assert(Tree::instance().node(presentation.id()).style.visibility == 0);
#else
    static_assert(ComputedStyle::visibility == 0);
#endif
#if GEA_CSS_POINTER_EVENTS
    presentation.style().set(Property::PointerEvents, 1);
    assert(Tree::instance().node(presentation.id()).style.pointer_events == 1);
    Document::instance().refresh(root, 120, 120);
    assert(Tree::instance().node(presentation.id()).style.pointer_events == 1);
    presentation.style().set(Property::PointerEvents, 0);
    assert(Tree::instance().node(presentation.id()).style.pointer_events == 0);
#else
    static_assert(ComputedStyle::pointer_events == 0);
#endif
#if GEA_CSS_MASK
    presentation.style().set(Property::MaskRightFadeWidth, 12);
    assert(Tree::instance().node(presentation.id()).style.mask_right_fade_width == 12);
    Document::instance().refresh(root, 120, 120);
    assert(Tree::instance().node(presentation.id()).style.mask_right_fade_width == 12);
    presentation.style().set(Property::MaskRightFadeWidth, 0);
    assert(Tree::instance().node(presentation.id()).style.mask_right_fade_width == 0);
#else
    static_assert(ComputedStyle::mask_right_fade_width == 0);
#endif
#if GEA_CSS_IMAGE_FIT
    presentation.style().set(Property::ImageFit, 2);
    assert(Tree::instance().node(presentation.id()).style.image_fit == 2);
    Document::instance().refresh(root, 120, 120);
    assert(Tree::instance().node(presentation.id()).style.image_fit == 2);
    presentation.style().set(Property::ImageFit, 0);
    assert(Tree::instance().node(presentation.id()).style.image_fit == 0);
#else
    static_assert(ComputedStyle::image_fit == 0);
#endif
#if GEA_CSS_TEXT_TRANSFORM
    root.style().setProperty("text-transform", "uppercase");
    auto inherited = Document::instance().createView();
    root.appendChild(inherited);
    Document::instance().refresh(root, 120, 120);
    assert(Tree::instance().node(inherited.id()).style.text_transform == 1);
    root.style().setProperty("text-transform", "none");
#endif
#if GEA_CSS_VISIBILITY
    root.style().setProperty("visibility", "hidden");
    auto inheritedVisibility = Document::instance().createView();
    root.appendChild(inheritedVisibility);
    Document::instance().refresh(root, 120, 120);
    assert(Tree::instance().node(inheritedVisibility.id()).style.visibility == 1);
    root.style().setProperty("visibility", "visible");
#endif
    auto borderOnly = Document::instance().createView();
    borderOnly.style().setProperty("border", "2px solid #aabbcc");
    assert(Tree::instance().node(borderOnly.id()).style.rare_style == -1);
#if GEA_CSS_SIDE_BORDERS
    borderOnly.style().setProperty("border-top-color", "#112233");
    assert(Tree::instance().node(borderOnly.id()).style.rare_style >= 0);
    assert(!borderColorIsCurrent(Tree::instance().node(borderOnly.id()).style, 0));
#endif
    borderOnly.style().setProperty("border-color", "currentColor");
    assert(borderColorIsCurrent(Tree::instance().node(borderOnly.id()).style));

#if GEA_CSS_FLEX_WRAP
    presentation.style().set(Property::FlexWrap, 1);
    Document::instance().refresh(root, 120, 120);
    assert(Tree::instance().node(presentation.id()).style.flex_wrap == 1);
    presentation.style().set(Property::FlexWrap, 0);
#else
    static_assert(ComputedStyle::flex_wrap == 0);
#endif
#if GEA_CSS_JUSTIFY_ITEMS
    presentation.style().set(Property::JustifyItems, 2);
    Document::instance().refresh(root, 120, 120);
    assert(Tree::instance().node(presentation.id()).style.justify_items == 2);
    presentation.style().set(Property::JustifyItems, 0);
#else
    static_assert(ComputedStyle::justify_items == 0);
#endif
#if GEA_CSS_ALIGN_CONTENT
    presentation.style().set(Property::AlignContent, 3);
    Document::instance().refresh(root, 120, 120);
    assert(Tree::instance().node(presentation.id()).style.align_content == 3);
    presentation.style().set(Property::AlignContent, 0);
#else
    static_assert(ComputedStyle::align_content == 0);
#endif
#if GEA_CSS_ALIGN_SELF
    presentation.style().set(Property::AlignSelf, 2);
    Document::instance().refresh(root, 120, 120);
    assert(Tree::instance().node(presentation.id()).style.align_self == 2);
    presentation.style().set(Property::AlignSelf, -1);
#else
    static_assert(ComputedStyle::align_self == -1);
#endif
#if GEA_CSS_MIN_WIDTH
    presentation.style().set(Property::MinWidth, 18);
    Document::instance().refresh(root, 120, 120);
    assert(Tree::instance().node(presentation.id()).style.min_width == 18);
    presentation.style().set(Property::MinWidth, kUnset);
#else
    static_assert(ComputedStyle::min_width == kUnset);
#endif
#if GEA_CSS_HEIGHT_EXPRESSIONS
    presentation.style().setProperty("height", "calc(50% - 10px)");
    Document::instance().refresh(root, 120, 120);
    assert(Tree::instance().node(presentation.id()).style.height_expression >= 0);
    presentation.style().setProperty("height", "20px");
    assert(Tree::instance().node(presentation.id()).style.height_expression == -1);
#else
    static_assert(ComputedStyle::height_expression == -1);
#endif
#if GEA_CSS_FILTERS
    presentation.style().setProperty("filter", "blur(2px)");
    Document::instance().refresh(root, 120, 120);
    assert(rstyle(Tree::instance().node(presentation.id()).style).filter_blur_radius > 0);
    assert(Tree::instance().node(presentation.id()).render.previous_filter_blur_radius > 0);
    presentation.style().setProperty("filter", "none");
    assert(rstyle(Tree::instance().node(presentation.id()).style).filter_blur_radius == 0);
#else
    static_assert(RareStyle::filter_blur_radius == 0);
    static_assert(RenderState::previous_filter_blur_radius == 0);
#endif
#if GEA_CSS_BOX_SHADOW
    presentation.style().setProperty("box-shadow", "inset 2px 3px 1px #ff0000");
    Document::instance().refresh(root, 120, 120);
    assert(rstyle(Tree::instance().node(presentation.id()).style).box_shadow_alpha > 0);
    assert(rstyle(Tree::instance().node(presentation.id()).style).box_shadow_offset_x != 0);
    presentation.style().setProperty("box-shadow", "none");
    assert(rstyle(Tree::instance().node(presentation.id()).style).box_shadow_alpha == 0);
#else
    static_assert(RareStyle::box_shadow_alpha == 0);
#endif

    // A disabled optional switch label must not hide the shared length body.
    // The earlier narrow scene used the numeric font API and missed this path.
    auto &lengthSheet = StyleSheet::instance();
    lengthSheet.registerRule("compiled-lengths", "font-size", "8px");
    lengthSheet.registerRule("compiled-lengths", "min-height", "9px");
    lengthSheet.registerRule("compiled-lengths", "max-width", "70px");
#if GEA_CSS_MAX_HEIGHT
    lengthSheet.registerRule("compiled-lengths", "max-height", "40px");
#endif
    lengthSheet.registerRule("compiled-lengths", "padding-left", "3px");
    lengthSheet.registerRule("compiled-lengths", "margin-bottom", "4px");
    auto compiledLengths = Document::instance().createView();
    compiledLengths.classList().set("compiled-lengths");
    root.appendChild(compiledLengths);
    Document::instance().refresh(root, 120, 120);
    const auto &lengthStyle = Tree::instance().node(compiledLengths.id()).style;
    assert(lengthStyle.font_size == 8 && lengthStyle.line_height == 0);
    assert(lengthStyle.min_height == 9 && lengthStyle.max_width == 70);
#if GEA_CSS_MAX_HEIGHT
    assert(lengthStyle.max_height == 40);
#else
    static_assert(ComputedStyle::max_height == kUnset);
#endif
    assert(lengthStyle.padding[3] == 3 && lengthStyle.margin[2] == 4);

    // Pool-table growth must never invalidate an optional record retained by
    // a re-entrant event callback or cloning/style operation.
    auto stableNode = Document::instance().createView();
    auto *stableRecord = &ensureRareData(stableNode.id());
    stableRecord->attributes.set("id", "retained");
    for (int i = 0; i < 96; ++i) {
        auto added = Document::instance().createView();
        ensureRareData(added.id()).attributes.set("id", "growth");
        assert(rareDataFor(stableNode.id()) == stableRecord);
        assert(std::string(stableRecord->attributes.get("id")) == "retained");
    }
    releaseRareData(stableNode.id());
    assert(rareDataFor(stableNode.id()) == nullptr);
    assert(&ensureRareData(stableNode.id()) == stableRecord);
    assert(stableRecord->attributes.count == 0);

#if GEA_CSS_BACKGROUND_LAYERS
    borderOnly.style().setProperty("background-image", "none, none");
    borderOnly.style().setProperty("background-clip", "border-box, content-box");
    assert(rstyle(Tree::instance().node(borderOnly.id()).style).bg_image_layer_count == 2);
    assert(rstyle(Tree::instance().node(borderOnly.id()).style).bg_clip != 0);
#else
    static_assert(RareStyle::bg_image_layer_count == 1 && RareStyle::bg_clip == 0);
#endif
#if GEA_CSS_LINE_HEIGHT_EXPRESSIONS
    borderOnly.style().setProperty("line-height", "1.5em");
    assert(rstyle(Tree::instance().node(borderOnly.id()).style).line_height_expression >= 0);
    borderOnly.style().setProperty("line-height", "12px");
    assert(rstyle(Tree::instance().node(borderOnly.id()).style).line_height_expression == -1);
#else
    static_assert(RareStyle::line_height_expression == -1);
#endif
    // Exercise all four scalar pairs, including class replay and write masks.
    // Compiled records also carry mixed translation/rotation/scale payloads.
    {
        resetNativeHost();
        setViewportMetrics(120, 120, 1.0);
        auto &sheet = StyleSheet::instance();
        sheet.registerStaticPropertyGroupRule(StaticStyleSelectorKind::Class, "four-properties",
            {{Property::Width, 71}, {Property::Height, 43}, {Property::Gap, 7}, {Property::BorderWidth, 9}});
        auto subject = Document::instance().createView();
        for (int i = 0; i < 4; ++i) {
            subject.classList().set("four-properties");
            const auto &style = Tree::instance().node(subject.id()).style;
            assert(style.width == 71 && style.height == 43 && style.gap == 7 && style.border_width == 9);
            subject.classList().set("");
        }
#if GEA_CSS_TRANSFORMS
        sheet.registerStaticTransformRule(StaticStyleSelectorKind::Class, "static-transform",
            7u | (7u << 3) | (1u << 8) | (1u << 9) | (1u << 13), 100, 200, 300,
            {StaticStyleLengthUnit::Px, 11}, {StaticStyleLengthUnit::Px, 13}, {StaticStyleLengthUnit::Px, 17},
            1250, 1500, nullptr, 1750);
        sheet.registerRule("parsed-transform", "transform", "rotateX(10deg) translate3d(11px,13px,17px) scale3d(1.25,1.5,1.75)");
        for (int i = 0; i < 4; ++i) {
            for (const char *name : {"static-transform", "parsed-transform"}) {
                subject.classList().set(name);
                const auto &rare = rstyle(Tree::instance().node(subject.id()).style);
                assert(rare.transform_scale_x == 1250 && rare.transform_scale_y == 1500 && rare.transform_scale_z == 1750);
                assert(rare.transform_translate_x == 11 && rare.transform_translate_y == 13 && rare.transform_translate_z == 17);
                assert(rare.transform_rotate_x == 100);
                subject.classList().set("");
            }
        }
#endif
    }
    // Packed targets keep all 16 color bits without sign extension; full-color
    // targets retain both signed 32-bit carriers, including their high bits.
    {
        NodeCustomPropertyStore colors;
        const auto key = internCssAtom("--cache-boundary");
#if GEA_PIXEL_FORMAT_IS_8888
        const std::int32_t values[] = {0, 32767, 32768, 65535, 0x12345678, -1, INT32_MIN};
#else
        const std::int32_t values[] = {0, 32767, 32768, 65535};
#endif
        for (auto value : values) {
            colors.setColor(key, "boundary", value, value, 128);
            assert(colors.getEntry(key)->colorStyle == value);
            assert(colors.getEntry(key)->colorNative == value);
            assert(colors.getEntry(key)->colorAlpha == 128);
            auto copy = colors;
            colors.clear();
            assert(copy.getEntry(key)->colorStyle == value);
            assert(copy.getEntry(key)->colorNative == value);
        }
    }
    // CSS variable copies reuse immutable atoms, and updates preserve the old
    // owner. Exercise table exhaustion last: it must retain the exact value,
    // including self-assignment, without corrupting cached scalar fields.
    NodeCustomPropertyStore variables;
    const auto name = internCssAtom("--immutable-value");
    const std::string longValue(180, 'x');
    variables.setColor(name, longValue, 123, 456, 128);
    const std::string *original = variables.get(name);
    NodeCustomPropertyStore variableCopy = variables;
    assert(*variableCopy.get(name) == longValue);
    variables.set(name, "replacement");
    assert(*original == longValue && *variableCopy.get(name) == longValue);
    assert(variableCopy.getEntry(name)->hasColor());
    assert(variableCopy.getEntry(name)->colorNative == 456);
    variables.set(name, "");
    assert(variables.get(name)->empty());
    for (int i = 0; internCssAtom("atom-limit-probe-" + std::to_string(i)); ++i) assert(i < 65535);
    assert(*original == longValue);
    const std::string overflow(181, 'z');
    variables.setLength(name, overflow, 1.5f, 1);
    assert(variables.getEntry(name)->valueAtom == kInvalidCssAtom);
    assert(*variables.get(name) == overflow);
    assert(variables.getEntry(name)->hasLength() == bool(GEA_CSS_CUSTOM_PROPERTY_LENGTHS));
    variableCopy = variables;
    variables.set(name, *variables.get(name));
    assert(*variables.get(name) == overflow && *variableCopy.get(name) == overflow);
    variables.clear();
    assert(*variableCopy.get(name) == overflow);

}

#include "native_test_harness.h"

#include "ui/document.h"
#include "ui/node.h"
#include "ui/tree_state.h"
#include "ui/style.h"
#include "ui/tree_internal.h"

#include <cstdint>
#include <cstdio>
#include <cstring>

namespace gea::framework::app::generated {
void drainMicrotasks() {}
}  // namespace gea::framework::app::generated

namespace gea::framework::graphics::generated {
void ensureLinked() {}
}  // namespace gea::framework::graphics::generated

namespace {

using namespace gea::embedded::ui;
using namespace gea::embedded::test;

int gFailures = 0;

bool expectEqual(int actual, int expected, const char *label)
{
	if (actual == expected) return true;
	std::fprintf(stderr, "[css_block_flexbasis] FAIL %s: expected %d, got %d\n", label, expected, actual);
	gFailures++;
	return false;
}

bool expectTrue(bool cond, const char *label)
{
	if (cond) return true;
	std::fprintf(stderr, "[css_block_flexbasis] FAIL %s\n", label);
	gFailures++;
	return false;
}

int nativeRgb565(std::uint16_t color)
{
	return static_cast<int>(gea::framework::graphics::pixel::fromRgb565(color));
}

const NodeCustomProperty *customPropertyForNode(int nodeId, const char *name)
{
	const NodeRareData *rare = rareDataFor(nodeId);
	if (!rare) return nullptr;
	return rare->customProperties.getEntry(internCssAtom(name));
}

int makeSpan(int parent, const char *cls)
{
	const int id = Tree::instance().createView();
	Tree::instance().setTagName(id, "span");
	NodeHandle(parent).appendChild(NodeHandle(id));
	NodeHandle(id).classList().set(cls);
	return id;
}

int makeDiv(int parent, const char *cls)
{
	const int id = Tree::instance().createView();
	Tree::instance().setTagName(id, "div");
	if (parent >= 0) NodeHandle(parent).appendChild(NodeHandle(id));
	if (cls) NodeHandle(id).classList().set(cls);
	return id;
}

int findDirectChildByTag(int parent, const char *tag)
{
	Tree &tree = Tree::instance();
	if (parent < 0 || parent >= tree.nodeCount()) return -1;
	for (int child = tree.node(parent).first_child; child >= 0; child = tree.node(child).next_sibling) {
		if (std::strcmp(tree.tagName(child), tag) == 0) return child;
	}
	return -1;
}

// BUG #1: two `display:block` <span>s inside a plain-block <button> must STACK
// vertically (block-level boxes), not flow side-by-side as an inline row. This is
// the weather city list's .city-name / .city-detail.
void testDisplayBlockStacks()
{
	resetNativeHost();
	StyleSheet::instance().clear();
	setViewportMetrics(200, 100, 1.0);

	const int sel = Tree::instance().createView();
	Tree::instance().setTagName(sel, "button");  // plain block, no display rule
	NodeHandle(sel).classList().set("sel");
	const int name = makeSpan(sel, "name");
	const int detail = makeSpan(sel, "detail");
	Tree::instance().mount(sel, 200, 100);

	StyleSheet::instance().registerRule("name", "display", "block");
	StyleSheet::instance().registerRule("name", "width", "40");
	StyleSheet::instance().registerRule("name", "height", "12");
	StyleSheet::instance().registerRule("detail", "display", "block");
	StyleSheet::instance().registerRule("detail", "width", "60");
	StyleSheet::instance().registerRule("detail", "height", "10");
	// re-set classes so the registered rules apply
	NodeHandle(name).classList().set("name");
	NodeHandle(detail).classList().set("detail");
	Tree::instance().computeLayout(sel, 200, 100);

	const auto &n = Tree::instance().node(name);
	const auto &d = Tree::instance().node(detail);
	std::fprintf(stderr, "[css_block_flexbasis] block: name=(%d,%d %dx%d) detail=(%d,%d %dx%d)\n",
	             n.layout.x, n.layout.y, n.layout.width, n.layout.height,
	             d.layout.x, d.layout.y, d.layout.width, d.layout.height);
	expectEqual(n.layout.x, d.layout.x, "block: spans share x (stacked)");
	expectTrue(d.layout.y >= n.layout.y + n.layout.height, "block: detail below name (stacked, not row)");
}

// REGRESSION: two DEFAULT <span>s (no display rule) inside a plain block must
// still flow as an inline ROW (gea's inline-emulation). Guards that the explicit
// flag does not change default behavior.
void testDefaultSpansStillRow()
{
	resetNativeHost();
	StyleSheet::instance().clear();
	setViewportMetrics(200, 100, 1.0);

	const int box = Tree::instance().createView();
	Tree::instance().setTagName(box, "div");
	NodeHandle(box).classList().set("ibox");
	const int s1 = makeSpan(box, "s1");
	const int s2 = makeSpan(box, "s2");
	Tree::instance().mount(box, 200, 100);

	StyleSheet::instance().registerRule("s1", "width", "40");
	StyleSheet::instance().registerRule("s1", "height", "12");
	StyleSheet::instance().registerRule("s2", "width", "60");
	StyleSheet::instance().registerRule("s2", "height", "10");
	NodeHandle(s1).classList().set("s1");
	NodeHandle(s2).classList().set("s2");
	Tree::instance().computeLayout(box, 200, 100);

	const auto &a = Tree::instance().node(s1);
	const auto &b = Tree::instance().node(s2);
	std::fprintf(stderr, "[css_block_flexbasis] inline: s1=(%d,%d %dx%d) s2=(%d,%d %dx%d)\n",
	             a.layout.x, a.layout.y, a.layout.width, a.layout.height,
	             b.layout.x, b.layout.y, b.layout.width, b.layout.height);
	// They share a BASELINE, not a top edge. An inline-block with no in-flow line
	// box takes its baseline from its bottom margin edge, so two empty spans of
	// different heights align along their bottoms — Chrome puts these same two at
	// s1=(0,3 40x12) s2=(40,5 60x10), a +2 offset the engine reproduces exactly.
	expectEqual(a.layout.y + a.layout.height, b.layout.y + b.layout.height,
	            "inline: default spans share a baseline (row)");
	expectTrue(b.layout.x >= a.layout.x + a.layout.width, "inline: s2 to the right of s1 (row)");
}

// Native Gea uses simple block/inline heuristics for non-flex containers. A
// vertical margin on a span means the author expects a stacked label, as in Sky
// Hop's win overlay: "Course Clear" above "Coins 2/8".
void testVerticalMarginSpanStacks()
{
	resetNativeHost();
	StyleSheet::instance().clear();
	setViewportMetrics(320, 160, 1.0);

	const int panel = Tree::instance().createView();
	Tree::instance().setTagName(panel, "div");
	NodeHandle(panel).classList().set("panel");
	const int title = makeSpan(panel, "title");
	const int score = makeSpan(panel, "score");
	Tree::instance().mount(panel, 320, 160);

	StyleSheet::instance().registerRule("panel", "width", "316");
	StyleSheet::instance().registerRule("panel", "height", "116");
	StyleSheet::instance().registerRule("panel", "align-items", "center");
	StyleSheet::instance().registerRule("panel", "justify-content", "center");
	StyleSheet::instance().registerRule("title", "width", "120");
	StyleSheet::instance().registerRule("title", "height", "30");
	StyleSheet::instance().registerRule("title", "margin-bottom", "14");
	StyleSheet::instance().registerRule("score", "width", "64");
	StyleSheet::instance().registerRule("score", "height", "16");
	NodeHandle(panel).classList().set("panel");
	NodeHandle(title).classList().set("title");
	NodeHandle(score).classList().set("score");
	Tree::instance().computeLayout(panel, 320, 160);

	const auto &t = Tree::instance().node(title);
	const auto &s = Tree::instance().node(score);
	std::fprintf(stderr, "[css_block_flexbasis] vertical-margin: title=(%d,%d %dx%d) score=(%d,%d %dx%d)\n",
	             t.layout.x, t.layout.y, t.layout.width, t.layout.height,
	             s.layout.x, s.layout.y, s.layout.width, s.layout.height);
	expectEqual(t.layout.x + t.layout.width / 2, s.layout.x + s.layout.width / 2,
	            "vertical-margin: stacked spans share horizontal center");
	expectTrue(s.layout.y >= t.layout.y + t.layout.height + 14,
	           "vertical-margin: score below title with margin gap");
}

// BUG #2: `flex: 0 0 26px` on a display:grid flex item must size the item to 26px
// (its flex-basis), not stretch it to fill the row. This is the weather forecast's
// .hour / .day cards.
void testFlexBasisFixedWidth()
{
	resetNativeHost();
	StyleSheet::instance().clear();
	setViewportMetrics(400, 120, 1.0);

	const int row = makeDiv(-1, "frow");
	const int card1 = makeDiv(row, "fcard");
	makeDiv(card1, "finner");
	const int card2 = makeDiv(row, "fcard");
	makeDiv(card2, "finner");
	Tree::instance().mount(row, 400, 120);

	StyleSheet::instance().registerRule("frow", "display", "flex");
	StyleSheet::instance().registerRule("frow", "width", "300");
	StyleSheet::instance().registerRule("frow", "height", "80");
	StyleSheet::instance().registerRule("fcard", "display", "grid");
	StyleSheet::instance().registerRule("fcard", "flex", "0 0 26px");
	StyleSheet::instance().registerRule("finner", "width", "20");
	StyleSheet::instance().registerRule("finner", "height", "40");
	NodeHandle(row).classList().set("frow");
	NodeHandle(card1).classList().set("fcard");
	NodeHandle(card2).classList().set("fcard");
	Tree::instance().computeLayout(row, 400, 120);

	const auto &c1 = Tree::instance().node(card1);
	const auto &c2 = Tree::instance().node(card2);
	std::fprintf(stderr, "[css_block_flexbasis] flexbasis: card1=(%d,%d %dx%d) card2=(%d,%d %dx%d)\n",
	             c1.layout.x, c1.layout.y, c1.layout.width, c1.layout.height,
	             c2.layout.x, c2.layout.y, c2.layout.width, c2.layout.height);
	expectEqual(c1.layout.width, 26, "flexbasis: card1 width == basis 26px");
	expectEqual(c2.layout.width, 26, "flexbasis: card2 width == basis 26px");
	expectEqual(c2.layout.x, 26, "flexbasis: card2 sits right after card1 (row of fixed cards)");
}

// BUG #3: CSS specificity. A more-specific descendant selector (`.box .row`,
// registered EARLY) must beat a less-specific base rule (`.row`, registered LATE)
// for the same property — real CSS resolves by specificity, not source order. This
// is what makes the weather forecast's `.forecast.show-days .hour-row { display:none }`
// actually hide the hour row when the base `.hour-row { display:flex }` comes later.
void testSpecificityDescendantBeatsBase()
{
	resetNativeHost();
	StyleSheet::instance().clear();
	setViewportMetrics(200, 100, 1.0);

	const int box = Tree::instance().createView();
	Tree::instance().setTagName(box, "div");
	NodeHandle(box).classList().set("box");
	const int row = makeDiv(box, "row");
	Tree::instance().mount(box, 200, 100);

	// Register the MORE-specific descendant rule FIRST, the base rule LAST — so a
	// pure source-order cascade would (wrongly) let the base win.
	StyleSheet::instance().registerSelectorRule(".box .row", "display", "none");
	StyleSheet::instance().registerRule("row", "display", "flex");
	StyleSheet::instance().registerRule("row", "width", "50");
	StyleSheet::instance().registerRule("row", "height", "20");
	NodeHandle(box).classList().set("box");
	NodeHandle(row).classList().set("row");
	Tree::instance().computeLayout(box, 200, 100);

	const int display = Tree::instance().node(row).style.display;
	std::fprintf(stderr, "[css_block_flexbasis] specificity: .row display=%d (expect %d=none)\n",
	             display, kDisplayNone);
	expectEqual(display, kDisplayNone, "specificity: descendant .box .row beats base .row");
}

// BUG #4: selectors with more than two descendant parts must walk the full
// ancestor chain. `.vn-menu .vn-menu-list div` in voice-notes used to register
// correctly but never match the row divs because the matcher skipped the middle
// selector part incorrectly.
void testThreePartDescendantSelector()
{
	resetNativeHost();
	StyleSheet::instance().clear();
	setViewportMetrics(200, 100, 1.0);

	const int outer = makeDiv(-1, "outer");
	const int middle = makeDiv(outer, "middle");
	const int row = Tree::instance().createView();
	NodeHandle(middle).appendChild(NodeHandle(row));
	Tree::instance().setTagName(row, "div");
	Tree::instance().mount(outer, 200, 100);

	StyleSheet::instance().registerSelectorRule(".outer .middle div", "display", "flex");
	StyleSheet::instance().registerSelectorRule(".outer .middle div", "width", "70");
	StyleSheet::instance().registerSelectorRule(".outer .middle div", "height", "12");
	NodeHandle(outer).classList().set("outer");
	NodeHandle(middle).classList().set("middle");
	Tree::instance().computeLayout(outer, 200, 100);

	const auto &r = Tree::instance().node(row);
	std::fprintf(stderr, "[css_block_flexbasis] descendant3: row display=%d box=(%d,%d %dx%d)\n",
	             r.style.display, r.layout.x, r.layout.y, r.layout.width, r.layout.height);
	expectEqual(r.style.display, kDisplayFlex, "descendant3: .outer .middle div display applies");
	expectEqual(r.layout.width, 70, "descendant3: .outer .middle div width applies");
	expectEqual(r.layout.height, 12, "descendant3: .outer .middle div height applies");
}

void testRootAndIdSelectors()
{
	resetNativeHost();
	StyleSheet::instance().clear();
	setViewportMetrics(200, 100, 1.0);

	const int root = makeDiv(-1, nullptr);
	Tree::instance().setAttribute(root, "id", "app");
	const int special = makeDiv(root, nullptr);
	Tree::instance().setAttribute(special, "id", "special");
	const int other = makeDiv(root, nullptr);
	Tree::instance().setAttribute(other, "id", "other");
	Tree::instance().mount(root, 200, 100);

	StyleSheet::instance().registerSelectorRule(":root", "width", "123");
	StyleSheet::instance().registerSelectorRule(":root", "height", "80");
	StyleSheet::instance().registerSelectorRule("#special", "width", "45");
	StyleSheet::instance().registerSelectorRule("#special", "height", "20");
	StyleSheet::instance().registerSelectorRule("#other", "display", "none");
	StyleSheet::instance().recomputeSubtree(root);
	Tree::instance().computeLayout(root, 200, 100);

	const auto &rootNode = Tree::instance().node(root);
	const auto &specialNode = Tree::instance().node(special);
	const auto &otherNode = Tree::instance().node(other);
	std::fprintf(stderr,
	             "[css_block_flexbasis] root-id: root=%dx%d special=%dx%d otherDisplay=%d\n",
	             rootNode.style.width,
	             rootNode.style.height,
	             specialNode.style.width,
	             specialNode.style.height,
	             otherNode.style.display);
	expectEqual(rootNode.style.width, 123, "root-id: :root width applies to root");
	expectEqual(rootNode.style.height, 80, "root-id: :root height applies to root");
	expectEqual(specialNode.style.width, 45, "root-id: #special width applies");
	expectEqual(specialNode.style.height, 20, "root-id: #special height applies");
	expectEqual(otherNode.style.display, kDisplayNone, "root-id: #other display applies");
}

void testCandidateCacheKeepsAncestorSelectorsDynamic()
{
	resetNativeHost();
	StyleSheet::instance().clear();
	setViewportMetrics(160, 100, 1.0);

	const int root = makeDiv(-1, nullptr);
	const int activeGroup = makeDiv(root, "active");
	const int activeItem = makeDiv(activeGroup, "item");
	const int plainGroup = makeDiv(root, nullptr);
	const int plainItem = makeDiv(plainGroup, "item");
	Tree::instance().mount(root, 160, 100);

	StyleSheet::instance().registerRule("item", "width", "30");
	StyleSheet::instance().registerRule("item", "height", "10");
	StyleSheet::instance().registerSelectorRule(".active .item", "width", "90");
	StyleSheet::instance().recomputeSubtree(root);

	expectEqual(Tree::instance().node(activeItem).style.width, 90, "candidate-cache: ancestor selector applies under .active");
	expectEqual(Tree::instance().node(plainItem).style.width, 30, "candidate-cache: ancestor selector rejected outside .active");

	NodeHandle(activeGroup).classList().set("");
	NodeHandle(plainGroup).classList().set("active");

	expectEqual(Tree::instance().node(activeItem).style.width, 30, "candidate-cache: removed ancestor stops matching");
	expectEqual(Tree::instance().node(plainItem).style.width, 90, "candidate-cache: added ancestor starts matching");
}

void testCachedGridTemplateRulesApply()
{
	resetNativeHost();
	StyleSheet::instance().clear();
	setViewportMetrics(200, 100, 1.0);

	const int root = makeDiv(-1, nullptr);
	const int first = makeDiv(root, nullptr);
	const int second = makeDiv(root, nullptr);
	Tree::instance().mount(root, 200, 100);

	StyleSheet::instance().registerRule("gridy", "display", "grid");
	StyleSheet::instance().registerRule("gridy", "grid-template-columns", "repeat(2, 20px) 1fr auto");
	StyleSheet::instance().registerRule("gridy", "grid-template-rows", "12px minmax(4px, 2fr)");
	NodeHandle(first).classList().set("gridy");
	NodeHandle(second).classList().set("gridy");

	const RareStyle &rs = rstyle(Tree::instance().node(second).style);
	expectEqual(Tree::instance().node(second).style.display, kDisplayGrid, "grid-template-cache: display applies");
	expectEqual(rs.grid_column_count, 4, "grid-template-cache: column count");
	expectEqual(rs.grid_column_type[0], 1, "grid-template-cache: column 0 fixed");
	expectEqual(rs.grid_column_type[1], 1, "grid-template-cache: column 1 fixed");
	expectEqual(rs.grid_column_type[2], 2, "grid-template-cache: column 2 fr");
	expectEqual(rs.grid_column_type[3], 0, "grid-template-cache: column 3 auto");
	expectEqual(rs.grid_column_value[0], 20, "grid-template-cache: column 0 px");
	expectEqual(rs.grid_column_value[1], 20, "grid-template-cache: column 1 px");
	expectEqual(rs.grid_column_value[2], 1, "grid-template-cache: column 2 fr value");
	expectEqual(rs.grid_row_count, 2, "grid-template-cache: row count");
	expectEqual(rs.grid_row_type[0], 1, "grid-template-cache: row 0 fixed");
	expectEqual(rs.grid_row_type[1], 2, "grid-template-cache: row 1 fr");
	expectEqual(rs.grid_row_value[0], 12, "grid-template-cache: row 0 px");
	expectEqual(rs.grid_row_value[1], 2, "grid-template-cache: row 1 fr value");
}

void testStaticCustomLengthExpressionPreResolves()
{
	resetNativeHost();
	StyleSheet::instance().clear();
	setViewportMetrics(200, 100, 1.0);

	const int box = makeDiv(-1, "cube-wrap");
	Tree::instance().mount(box, 200, 100);

	StyleSheet::instance().registerRule("cube-wrap", "--cube-size", "clamp(20px, 50vw, 80px)");
	StyleSheet::instance().registerRule("cube-wrap", "width", "var(--cube-size)");
	StyleSheet::instance().registerRule("cube-wrap", "height", "calc(var(--cube-size) / 2)");
	NodeHandle(box).classList().set("cube-wrap");
	Tree::instance().computeLayout(box, 200, 100);

	const NodeCustomProperty *entry = customPropertyForNode(box, "--cube-size");
	expectTrue(entry && entry->hasLength(), "custom-length: clamp custom prop stored as length");
	if (entry && entry->hasLength()) {
		expectEqual(entry->lengthUnit, 1, "custom-length: clamp pre-resolved to raw internal px");
		expectEqual(static_cast<int>(entry->lengthValue), 80, "custom-length: initial resolved raw value");
	}
	expectEqual(Tree::instance().node(box).style.width, 80, "custom-length: var(width) uses pre-resolved clamp");
	expectEqual(Tree::instance().node(box).style.height, 40, "custom-length: calc(var / 2) uses pre-resolved clamp");

	setViewportMetrics(60, 100, 1.0);
	Tree::instance().computeLayout(box, 60, 100);
	entry = customPropertyForNode(box, "--cube-size");
	expectTrue(entry && entry->hasLength(), "custom-length: clamp survives viewport recompute");
	if (entry && entry->hasLength()) {
		expectEqual(entry->lengthUnit, 1, "custom-length: viewport recompute remains raw");
		expectEqual(static_cast<int>(entry->lengthValue), 30, "custom-length: viewport recompute updates raw value");
	}
	expectEqual(Tree::instance().node(box).style.width, 30, "custom-length: viewport recompute updates width");
	expectEqual(Tree::instance().node(box).style.height, 15, "custom-length: viewport recompute updates calc");
}

void testPercentCustomLengthStaysDynamic()
{
	resetNativeHost();
	StyleSheet::instance().clear();
	setViewportMetrics(200, 100, 1.0);

	const int parent = makeDiv(-1, "parent");
	const int child = makeDiv(parent, "pct");
	Tree::instance().mount(parent, 200, 100);

	StyleSheet::instance().registerRule("parent", "width", "120");
	StyleSheet::instance().registerRule("parent", "height", "40");
	StyleSheet::instance().registerRule("pct", "--w", "50%");
	StyleSheet::instance().registerRule("pct", "width", "var(--w)");
	StyleSheet::instance().registerRule("pct", "height", "10");
	NodeHandle(parent).classList().set("parent");
	NodeHandle(child).classList().set("pct");
	Tree::instance().computeLayout(parent, 200, 100);

	const NodeCustomProperty *entry = customPropertyForNode(child, "--w");
	expectTrue(entry && entry->hasLength(), "custom-length: percent custom prop stored as length");
	if (entry && entry->hasLength()) {
		expectEqual(entry->lengthUnit, 3, "custom-length: percent custom prop stays percent");
	}
	expectEqual(Tree::instance().node(child).style.width_percent, 500, "custom-length: percent var uses percent style slot");
	expectEqual(Tree::instance().node(child).layout.width, 60, "custom-length: percent var resolves against parent width in layout");
}

void testStaticLengthExpressionCacheInvalidatesWithViewport()
{
	resetNativeHost();
	StyleSheet::instance().clear();
	setViewportMetrics(200, 100, 1.0);

	const int root = makeDiv(-1, nullptr);
	const int first = makeDiv(root, "sized");
	const int second = makeDiv(root, "sized");
	Tree::instance().mount(root, 200, 100);

	StyleSheet::instance().registerRule("sized", "width", "clamp(20px, 50vw, 80px)");
	StyleSheet::instance().registerRule("sized", "height", "10");
	NodeHandle(first).classList().set("sized");
	NodeHandle(second).classList().set("sized");
	Tree::instance().computeLayout(root, 200, 100);

	expectEqual(Tree::instance().node(first).style.width, 80, "static-length-cache: initial first width");
	expectEqual(Tree::instance().node(second).style.width, 80, "static-length-cache: initial sibling width");

	setViewportMetrics(60, 100, 1.0);
	Tree::instance().computeLayout(root, 60, 100);

	expectEqual(Tree::instance().node(first).style.width, 30, "static-length-cache: viewport updates first width");
	expectEqual(Tree::instance().node(second).style.width, 30, "static-length-cache: viewport updates sibling width");
}

void testActiveRuleCollapseKeepsCascadeSemantics()
{
	resetNativeHost();
	StyleSheet::instance().clear();
	setViewportMetrics(200, 100, 1.0);

	const int root = makeDiv(-1, nullptr);
	const int box = makeDiv(root, "box override scrollx");
	Tree::instance().mount(root, 200, 100);

	StyleSheet::instance().registerRule("box", "width", "10");
	StyleSheet::instance().registerRule("box", "height", "12");
	StyleSheet::instance().registerRule("box", "overflow", "hidden");
	StyleSheet::instance().registerRule("box", "transform", "rotateX(30deg) translateZ(8px) scale(2)");
	StyleSheet::instance().registerSelectorRule(".box.override", "width", "50%");
	StyleSheet::instance().registerSelectorRule(".box.override", "transform", "translateZ(4px)");
	StyleSheet::instance().registerSelectorRule(".box.scrollx", "overflow-x", "scroll");
	NodeHandle(box).classList().set("box override scrollx");
	Tree::instance().computeLayout(root, 200, 100);

	const auto &node = Tree::instance().node(box);
	expectEqual(node.style.width, kUnset, "rule-collapse: later percent width clears earlier px width");
	expectEqual(node.style.width_percent, 500, "rule-collapse: later percent width wins");
	expectEqual(node.style.height, 12, "rule-collapse: unrelated earlier height remains");
	expectEqual(node.style.overflow_x, 2, "rule-collapse: later overflow-x wins");
	expectEqual(node.style.overflow_y, 1, "rule-collapse: earlier overflow-y from shorthand remains");
	expectEqual(node.style.overflow, 2, "rule-collapse: aggregate overflow sees both axes");
	expectEqual(rstyle(node.style).transform_rotate_x, 0, "rule-collapse: later transform resets earlier rotateX");
	expectEqual(rstyle(node.style).transform_translate_z, 4, "rule-collapse: later transform translateZ wins");
	expectEqual(rstyle(node.style).transform_scale_x, 1000, "rule-collapse: later transform resets earlier scaleX");
	expectEqual(rstyle(node.style).transform_scale_y, 1000, "rule-collapse: later transform resets earlier scaleY");
}

void testCustomPropertyLookupCacheInvalidates()
{
	resetNativeHost();
	StyleSheet::instance().clear();
	setViewportMetrics(200, 100, 1.0);

	const int parent = makeDiv(-1, "theme small");
	const int child = makeDiv(parent, "uses");
	const int sibling = makeDiv(parent, "uses");
	const int overrideBranch = makeDiv(parent, "override");
	const int overrideChild = makeDiv(overrideBranch, "uses");
	const int missingFallback = makeDiv(parent, "missing-fallback");
	const int missingNoFallback = makeDiv(parent, "missing-no-fallback");
	const int invalidBranch = makeDiv(parent, "invalid");
	const int invalidChild = makeDiv(invalidBranch, "invalid-color");
	Tree::instance().mount(parent, 200, 100);

	StyleSheet::instance().registerRule("small", "--s", "30px");
	StyleSheet::instance().registerRule("small", "--c", "#ff0000");
	StyleSheet::instance().registerRule("large", "--s", "70px");
	StyleSheet::instance().registerRule("large", "--c", "#0000ff");
	StyleSheet::instance().registerRule("override", "--s", "45px");
	StyleSheet::instance().registerRule("override", "--c", "#00ff00");
	StyleSheet::instance().registerRule("uses", "width", "var(--s)");
	StyleSheet::instance().registerRule("uses", "height", "var(--s)");
	StyleSheet::instance().registerRule("uses", "color", "var(--c, #000000)");
	StyleSheet::instance().registerRule("missing-fallback", "color", "var(--missing-c, #00ff00)");
	StyleSheet::instance().registerRule("missing-no-fallback", "color", "var(--missing-c)");
	StyleSheet::instance().registerRule("invalid", "--bad-c", "not-a-color");
	StyleSheet::instance().registerRule("invalid-color", "color", "var(--bad-c, #ff0000)");
	NodeHandle(parent).classList().set("theme small");
	NodeHandle(child).classList().set("uses");
	NodeHandle(sibling).classList().set("uses");
	NodeHandle(overrideBranch).classList().set("override");
	NodeHandle(overrideChild).classList().set("uses");
	NodeHandle(missingFallback).classList().set("missing-fallback");
	NodeHandle(missingNoFallback).classList().set("missing-no-fallback");
	NodeHandle(invalidBranch).classList().set("invalid");
	NodeHandle(invalidChild).classList().set("invalid-color");
	Tree::instance().computeLayout(parent, 200, 100);

	expectEqual(Tree::instance().node(child).style.width, 30, "custom-cache: first inherited width");
	expectEqual(Tree::instance().node(child).style.height, 30, "custom-cache: first inherited height");
	expectEqual(Tree::instance().node(sibling).style.width, 30, "custom-cache: sibling inherited width");
	expectEqual(Tree::instance().node(overrideChild).style.width, 45, "custom-cache: child override shadows inherited width");
	expectEqual(Tree::instance().node(child).style.text_color, nativeRgb565(0xf800), "custom-cache: inherited color var resolves red");
	expectEqual(Tree::instance().node(sibling).style.text_color, nativeRgb565(0xf800), "custom-cache: sibling inherited color var resolves red");
	expectEqual(Tree::instance().node(overrideChild).style.text_color, nativeRgb565(0x07e0), "custom-cache: child override color resolves green");
	expectEqual(Tree::instance().node(missingFallback).style.text_color, nativeRgb565(0x07e0), "custom-cache: missing color var uses fallback");
	expectEqual(Tree::instance().node(missingNoFallback).style.text_color, nativeRgb565(0xffff), "custom-cache: missing color var without fallback resolves white");
	expectEqual(Tree::instance().node(invalidChild).style.text_color, nativeRgb565(0xffff), "custom-cache: invalid inherited color var resolves white");

	NodeHandle(parent).classList().set("theme large");
	Tree::instance().computeLayout(parent, 200, 100);

	expectEqual(Tree::instance().node(child).style.width, 70, "custom-cache: inherited width updates after parent custom change");
	expectEqual(Tree::instance().node(child).style.height, 70, "custom-cache: inherited height updates after parent custom change");
	expectEqual(Tree::instance().node(sibling).style.width, 70, "custom-cache: sibling inherited width updates");
	expectEqual(Tree::instance().node(overrideChild).style.width, 45, "custom-cache: override still shadows parent change");
	expectEqual(Tree::instance().node(child).style.text_color, nativeRgb565(0x001f), "custom-cache: inherited color updates blue");
	expectEqual(Tree::instance().node(sibling).style.text_color, nativeRgb565(0x001f), "custom-cache: sibling inherited color updates blue");
	expectEqual(Tree::instance().node(overrideChild).style.text_color, nativeRgb565(0x07e0), "custom-cache: override still shadows parent color");
	expectEqual(Tree::instance().node(missingFallback).style.text_color, nativeRgb565(0x07e0), "custom-cache: missing fallback color remains green");
	expectEqual(Tree::instance().node(missingNoFallback).style.text_color, nativeRgb565(0xffff), "custom-cache: missing no-fallback color remains white");
	expectEqual(Tree::instance().node(invalidChild).style.text_color, nativeRgb565(0xffff), "custom-cache: invalid color remains white");
}

void testStaticCustomColorPrecompiles()
{
	resetNativeHost();
	StyleSheet::instance().clear();
	setViewportMetrics(200, 100, 1.0);

	const int parent = makeDiv(-1, "theme-red");
	const int child = makeDiv(parent, "uses-color");
	Tree::instance().mount(parent, 200, 100);

	StyleSheet::instance().registerStaticCustomColorRule(StaticStyleSelectorKind::Class,
	                                                     "theme-red",
	                                                     "--ink",
	                                                     255,
	                                                     0,
	                                                     0,
	                                                     255);
	StyleSheet::instance().registerStaticCustomColorRule(StaticStyleSelectorKind::Class,
	                                                     "theme-blue",
	                                                     "--ink",
	                                                     0,
	                                                     0,
	                                                     255,
	                                                     255);
	StyleSheet::instance().registerRule("uses-color", "color", "var(--ink)");
	NodeHandle(parent).classList().set("theme-red");
	NodeHandle(child).classList().set("uses-color");
	Tree::instance().computeLayout(parent, 200, 100);

	const NodeCustomProperty *entry = customPropertyForNode(parent, "--ink");
	expectTrue(entry && entry->hasColor(), "static-custom-color: custom prop stored as color");
	if (entry && entry->hasColor()) {
		expectEqual(static_cast<int>(entry->colorNative), nativeRgb565(0xf800), "static-custom-color: stored red native color");
		expectEqual(entry->colorAlpha, 255, "static-custom-color: stored alpha");
	}
	expectEqual(Tree::instance().node(child).style.text_color, nativeRgb565(0xf800), "static-custom-color: var resolves red");

	NodeHandle(parent).classList().set("theme-blue");
	Tree::instance().computeLayout(parent, 200, 100);
	entry = customPropertyForNode(parent, "--ink");
	expectTrue(entry && entry->hasColor(), "static-custom-color: color survives class update");
	if (entry && entry->hasColor()) {
		expectEqual(static_cast<int>(entry->colorNative), nativeRgb565(0x001f), "static-custom-color: stored blue native color");
	}
	expectEqual(Tree::instance().node(child).style.text_color, nativeRgb565(0x001f), "static-custom-color: var resolves blue");
}

void testCompiledGradientColorVarFallbacks()
{
	resetNativeHost();
	StyleSheet::instance().clear();
	setViewportMetrics(200, 100, 1.0);

	const int box = makeDiv(-1, "gradient-theme gradient-var");
	const int noFallback = makeDiv(box, "gradient-theme gradient-no-fallback");
	const int missingNoFallback = makeDiv(box, "gradient-missing-no-fallback");
	Tree::instance().mount(box, 200, 100);

	StyleSheet::instance().registerStaticCustomColorRule(StaticStyleSelectorKind::Class,
	                                                     "gradient-theme",
	                                                     "--to",
	                                                     255,
	                                                     0,
	                                                     0,
	                                                     255);
	StyleSheet::instance().registerRule("gradient-var",
	                                    "background",
	                                    "linear-gradient(90deg, var(--missing, #00ff00), var(--to, #0000ff))");
	StyleSheet::instance().registerRule("gradient-no-fallback",
	                                    "background",
	                                    "linear-gradient(90deg, #000000, var(--to))");
	StyleSheet::instance().registerRule("gradient-missing-no-fallback",
	                                    "background",
	                                    "linear-gradient(90deg, #000000, var(--missing-gradient-stop))");
	NodeHandle(box).classList().set("gradient-theme gradient-var");
	NodeHandle(noFallback).classList().set("gradient-theme gradient-no-fallback");
	NodeHandle(missingNoFallback).classList().set("gradient-missing-no-fallback");
	Tree::instance().computeLayout(box, 200, 100);

	const auto &style = Tree::instance().node(box).style;
	expectEqual(style.has_bg, 1, "gradient-var: compiled background applies");
	expectEqual(style.bg_fill, 1, "gradient-var: compiled background fill");
	expectEqual(static_cast<int>(rstyle(style).bg_gradient_from_color), nativeRgb565(0x07e0), "gradient-var: missing from stop uses fallback");
	expectEqual(static_cast<int>(rstyle(style).bg_gradient_to_color), nativeRgb565(0xf800), "gradient-var: custom to stop resolves red");
	expectEqual(static_cast<int>(rstyle(Tree::instance().node(noFallback).style).bg_gradient_to_color),
	            nativeRgb565(0xf800),
	            "gradient-var: no-fallback custom stop resolves red");
	expectEqual(Tree::instance().node(missingNoFallback).style.has_bg,
	            0,
	            "gradient-var: missing no-fallback stop leaves background unapplied");

	NodeHandle(box).classList().set("gradient-var");
	Tree::instance().computeLayout(box, 200, 100);
	expectEqual(static_cast<int>(rstyle(Tree::instance().node(box).style).bg_gradient_to_color),
	            nativeRgb565(0x001f),
	            "gradient-var: missing to stop uses fallback");
}

void testPseudoSelectorMaterializeAndRemove()
{
	resetNativeHost();
	StyleSheet::instance().clear();
	setViewportMetrics(120, 80, 1.0);

	const int chipId = makeDiv(-1, "chip active");
	Tree::instance().mount(chipId, 120, 80);
	StyleSheet::instance().registerRule("chip", "width", "60");
	StyleSheet::instance().registerRule("chip", "height", "20");
	StyleSheet::instance().registerSelectorRule(".chip.active::after", "content", "\"\"");
	StyleSheet::instance().registerSelectorRule(".chip.active::after", "position", "absolute");
	StyleSheet::instance().registerSelectorRule(".chip.active::after", "left", "0");
	StyleSheet::instance().registerSelectorRule(".chip.active::after", "right", "0");
	StyleSheet::instance().registerSelectorRule(".chip.active::after", "bottom", "0");
	StyleSheet::instance().registerSelectorRule(".chip.active::after", "height", "2");
	StyleSheet::instance().registerSelectorRule(".chip.active::after", "background", "#ffffff");
	NodeHandle(chipId).classList().set("chip active");

	const int afterId = findDirectChildByTag(chipId, "::after");
	expectTrue(afterId >= 0, "pseudo: active chip materializes ::after");
	if (afterId >= 0) {
		const auto &after = Tree::instance().node(afterId);
		expectEqual(after.style.height, 2, "pseudo: ::after height");
		expectEqual(after.style.has_bg, 1, "pseudo: ::after background");
	}

	NodeHandle(chipId).classList().set("chip");
	expectEqual(findDirectChildByTag(chipId, "::after") >= 0 ? 1 : 0, 0, "pseudo: inactive chip removes ::after");
}

}  // namespace

int main()
{
	testDisplayBlockStacks();
	testDefaultSpansStillRow();
	testVerticalMarginSpanStacks();
	testFlexBasisFixedWidth();
	testSpecificityDescendantBeatsBase();
	testThreePartDescendantSelector();
	testRootAndIdSelectors();
	testCandidateCacheKeepsAncestorSelectorsDynamic();
	testCachedGridTemplateRulesApply();
	testStaticCustomLengthExpressionPreResolves();
	testPercentCustomLengthStaysDynamic();
	testStaticLengthExpressionCacheInvalidatesWithViewport();
	testActiveRuleCollapseKeepsCascadeSemantics();
	testCustomPropertyLookupCacheInvalidates();
	testStaticCustomColorPrecompiles();
	testCompiledGradientColorVarFallbacks();
	testPseudoSelectorMaterializeAndRemove();
	if (gFailures == 0) {
		std::fprintf(stderr, "[css_block_flexbasis] ALL PASS\n");
		return 0;
	}
	std::fprintf(stderr, "[css_block_flexbasis] %d FAILURE(S)\n", gFailures);
	return 1;
}

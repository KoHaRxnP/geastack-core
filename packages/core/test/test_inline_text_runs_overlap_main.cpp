// Repro for the device-only "leading {dynamic} + adjacent text overlap" bug.
// Web flows separate adjacent text runs in a <span> left-to-right (browser inline
// layout). The native renderer builds the same separate runs; this test checks
// that they lay out side-by-side (no overlap), including when a LEADING reactive
// run starts empty at mount and is populated afterward.
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

struct Rect {
	int x, y, w, h;
};

Rect rectOf(int id)
{
	const Node &n = Tree::instance().node(id);
	return {n.layout.x, n.layout.y, n.layout.width, n.layout.height};
}

void dump(const char *label, int id)
{
	const Node &n = Tree::instance().node(id);
	std::printf("  %-8s x=%-4d w=%-4d y=%-4d h=%-4d text='%s'\n",
	            label, n.layout.x, n.layout.width, n.layout.y, n.layout.height, n.text.c_str());
}

// Returns true if [a] and [b] overlap horizontally while sharing a row (same y band).
bool overlapsX(const Rect &a, const Rect &b)
{
	if (a.w == 0 || b.w == 0) return false;
	const int aRight = a.x + a.w;
	const int bRight = b.x + b.w;
	const bool sameRow = !(a.y + a.h <= b.y || b.y + b.h <= a.y);
	const bool xOverlap = a.x < bRight && b.x < aRight;
	return sameRow && xOverlap;
}

}  // namespace

int main()
{
	using namespace gea::embedded::test;

	resetNativeHost();
	setNativeDisplaySize(220, 220);
	gea::embedded::ui::setViewportMetrics(220, 220, 1.0);

	auto root = Document::instance().createView();
	root.style().width(220);
	root.style().height(220);
	root.style().backgroundColor(0xffff);
	root.style().set(Property::FontSize, 20);
	root.style().set(Property::LineHeight, 24);
	// Parent matches .vn-screen.vn-sync: flex column, centered.
	root.style().display(kDisplayFlex);
	root.style().set(Property::FlexDirection, 1);  // column
	root.style().set(Property::AlignItems, 1);     // center
	root.style().set(Property::JustifyContent, 1); // center

	// <span class="vn-sync-label">{a} / {b}</span>  — leading dynamic, empty at mount.
	// CSS sets `display: block` EXPLICITLY on this span (the real failing case).
	auto span = Document::instance().createView();
	span.setTagName("span");
	span.style().display(kDisplayBlock);
	span.style().set(Property::FontSize, 22);
	span.style().set(Property::LineHeight, 26);
	auto runA = Document::instance().createText("");
	auto sep = Document::instance().createText(" / ");
	auto runB = Document::instance().createText("");
	root.appendChild(span);
	span.appendChild(runA);
	span.appendChild(sep);
	span.appendChild(runB);

	Document::instance().mount(root, 220, 220);
	std::printf("[after mount, runs still empty]\n");
	dump("runA", runA.id());
	dump("sep", sep.id());
	dump("runB", runB.id());

	// Reactive populate (mirrors reactiveTextValue firing after the empty mount).
	runA.setText("0");
	runB.setText("2");
	Document::instance().refresh(root, 220, 220);

	std::printf("[after reactive populate '0' / '2']\n");
	dump("span", span.id());
	dump("runA", runA.id());
	dump("sep", sep.id());
	dump("runB", runB.id());

	const Rect a = rectOf(runA.id());
	const Rect s = rectOf(sep.id());
	const Rect b = rectOf(runB.id());

	int rc = 0;
	if (overlapsX(a, s)) {
		std::fprintf(stderr, "[FAIL] runA overlaps separator (leading dynamic did not push siblings right)\n");
		rc = 1;
	}
	if (overlapsX(s, b)) {
		std::fprintf(stderr, "[FAIL] separator overlaps runB\n");
		rc = 1;
	}
	if (overlapsX(a, b)) {
		std::fprintf(stderr, "[FAIL] runA overlaps runB\n");
		rc = 1;
	}
	// Order sanity: a left of s left of b on the same row.
	if (!(a.x <= s.x && s.x <= b.x)) {
		std::fprintf(stderr, "[FAIL] runs out of left-to-right order: a.x=%d s.x=%d b.x=%d\n", a.x, s.x, b.x);
		rc = 1;
	}

	if (rc == 0) std::printf("[PASS] inline text runs flow left-to-right without overlap\n");
	return rc;
}

// Deterministic reproduction for "the right-edge fade gradient scrolls with the
// content instead of staying pinned to the edge". The hour-row carries a
// `mask-image: linear-gradient(..., transparent)` (mask_right_fade_width). When it
// scrolls, RootScrollOnlyRefresh used to scrollRect-blit the already-faded pixels
// and only repaint the freshly revealed edge strip, dragging the faded band across
// the content. This test scrolls the row via the blit fast path, then forces a full
// re-render at the same scroll offset and compares the row band: a correct, edge-
// anchored fade makes the two identical; the blit-dragged fade makes them differ.

#include "native_test_harness.h"

#include "app.h"
#include "graphics/font.h"
#include "host/backends.h"
#include "ui/document.h"
#include "ui/internal.h"
#include "ui/tree_internal.h"

#include <cstdio>
#include <cstdlib>
#include <vector>

extern void __gea_top_level();

namespace gea::framework::input {
bool InputBackend::consumeBackButton()
{
	return false;
}
}  // namespace gea::framework::input

namespace {
constexpr int kViewportWidth = 410;
constexpr int kViewportHeight = 502;
constexpr double kDevicePixelRatio = 1.5;

int lumaAt(int x, int y)
{
	const std::uint16_t p = gea::embedded::test::displayPixelAt(x, y);
	const int r = ((p >> 11) & 0x1f) * 255 / 31;
	const int g = ((p >> 5) & 0x3f) * 255 / 63;
	const int b = (p & 0x1f) * 255 / 31;
	return (r * 299 + g * 587 + b * 114) / 1000;
}

void markSubtreeFullyDirty(int id)
{
	using gea::embedded::ui::Tree;
	auto &tree = Tree::instance();
	if (id < 0 || id >= tree.nodeCount()) return;
	auto &n = tree.node(id);
	n.render.dirty = 1;
	n.render.non_scroll_dirty = 1;
	tree.markNodeDisplayCommandsDirty(id);
	for (int c = n.first_child; c >= 0; c = tree.node(c).next_sibling) markSubtreeFullyDirty(c);
}
}  // namespace

int main()
{
	using namespace gea::embedded::test;
	using gea::embedded::ui::Tree;

	resetNativeHost();
	setNativeDisplaySize(kViewportWidth, kViewportHeight);
	gea::embedded::ui::Document::setPreferredMountSize(kViewportWidth, kViewportHeight);
	gea::framework::app::Application::init(kViewportWidth, kViewportHeight, kDevicePixelRatio);
	__gea_top_level();
	refresh();
	pumpFrame(16);
	refresh();

	const auto rows = nodesWithClass("hour-row");
	if (rows.empty()) {
		std::fprintf(stderr, "[mask] no hour-row\n");
		return 1;
	}
	const int rowId = rows[0];
	{
		const auto &row = Tree::instance().node(rowId);
		if (row.style.mask_right_fade_width <= 0) {
			std::fprintf(stderr, "[mask] hour-row has no right-fade mask (%d)\n", row.style.mask_right_fade_width);
			return 1;
		}
		if (row.layout.scroll_content_width <= row.layout.width) {
			std::fprintf(stderr, "[mask] hour-row not scrollable contentW=%d w=%d\n",
			             row.layout.scroll_content_width, row.layout.width);
			return 1;
		}
	}

	// Drag the row a little so it scrolls through the blit fast path. Keep the drag
	// well inside the range so the fade band has interior content to (incorrectly)
	// drag onto. No pumpFrame after pointerUp => momentum never ticks => scroll frozen.
	const int rx = Tree::instance().node(rowId).layout.x;
	const int ry = Tree::instance().node(rowId).layout.y;
	const int rw = Tree::instance().node(rowId).layout.width;
	const int rh = Tree::instance().node(rowId).layout.height;
	const int maxScrollX = Tree::instance().node(rowId).layout.scroll_content_width - rw;
	const int delta = maxScrollX / 3 > 8 ? maxScrollX / 3 : 8;
	const int cx = rx + rw / 2;
	const int cy = ry + rh / 2;

	int t = 1000;
	setNativeNowMs(t);
	Tree::instance().pointerDown(cx, cy);
	t += 16;
	setNativeNowMs(t);
	Tree::instance().pointerMove(cx - delta / 2, cy);
	t += 16;
	setNativeNowMs(t);
	Tree::instance().pointerMove(cx - delta, cy);
	Tree::instance().pointerUp();
	refresh();  // renders the scrolled row through the scrollRect fast path

	const int scrollX = Tree::instance().node(rowId).layout.scroll_x;
	if (scrollX <= 0) {
		std::fprintf(stderr, "[mask] drag did not scroll the row (scroll_x=%d)\n", scrollX);
		return 1;
	}

	// Capture the row band as rendered by the fast path.
	const int bandTop = ry + 2;
	const int bandH = rh - 4 > 1 ? rh - 4 : rh;
	std::vector<int> blit(static_cast<std::size_t>(rw) * bandH);
	for (int yy = 0; yy < bandH; yy++)
		for (int xx = 0; xx < rw; xx++) blit[static_cast<std::size_t>(yy) * rw + xx] = lumaAt(rx + xx, bandTop + yy);

	// Force a full, non-scroll re-render at the SAME scroll offset (this re-records
	// every child with the per-child fade recomputed at its current position), then
	// capture the same band.
	markSubtreeFullyDirty(rowId);
	refresh();
	if (Tree::instance().node(rowId).layout.scroll_x != scrollX) {
		std::fprintf(stderr, "[mask] full re-render changed scroll_x %d -> %d\n", scrollX,
		             Tree::instance().node(rowId).layout.scroll_x);
		return 1;
	}

	int diffPixels = 0;
	int maxDelta = 0;
	int worstX = -1;
	for (int yy = 0; yy < bandH; yy++) {
		for (int xx = 0; xx < rw; xx++) {
			const int a = blit[static_cast<std::size_t>(yy) * rw + xx];
			const int b = lumaAt(rx + xx, bandTop + yy);
			const int d = a > b ? a - b : b - a;
			if (d > 12) {
				diffPixels++;
				if (d > maxDelta) {
					maxDelta = d;
					worstX = xx;
				}
			}
		}
	}

	std::fprintf(stderr,
	             "[mask] scroll_x=%d band=%dx%d diffPixels=%d maxDelta=%d worstColX=%d (rw=%d fadeW=%d)\n",
	             scrollX, rw, bandH, diffPixels, maxDelta, worstX, rw,
	             Tree::instance().node(rowId).style.mask_right_fade_width);

	// A handful of edge pixels can legitimately differ by AA rounding; a dragged
	// fade band lights up hundreds of interior pixels. Threshold generously.
	if (diffPixels > 40) {
		std::fprintf(stderr,
		             "[mask] FAIL: blit-rendered row differs from full re-render at the same scroll — fade not edge-anchored\n");
		return 1;
	}
	std::fprintf(stderr, "[mask] PASS: blit-scrolled row matches full re-render (fade stays edge-anchored)\n");
	return 0;
}

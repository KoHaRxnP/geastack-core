// Focused reproduction for "weather side-scrolls have no momentum": mounts the
// weather app and drives a realistic horizontal flick on the hour-row, then pumps
// frames and checks scroll_x keeps coasting after pointerUp. Standalone from
// test_gea_weather_main.cpp so it does NOT depend on live-forecast text (the full
// weather test asserts fetched data the native harness has no network for, and
// bails before its own momentum block ever runs).

#include "native_test_harness.h"

#include "app.h"
#include "graphics/font.h"
#include "host/backends.h"
#include "ui/document.h"
#include "ui/internal.h"
#include "ui/tree_internal.h"

#include <cstdio>
#include <cstdlib>

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

int testRowMomentum(const char *rowClass)
{
	using namespace gea::embedded::test;
	using gea::embedded::ui::Tree;

	const auto rows = nodesWithClass(rowClass);
	if (rows.empty()) {
		std::fprintf(stderr, "[momentum] no %s found\n", rowClass);
		return 1;
	}
	const int rowId = rows[0];
	const auto &row = Tree::instance().node(rowId);
	const int maxScrollX = row.layout.scroll_content_width - row.layout.width;
	std::fprintf(stderr,
	             "[momentum] %s overflow=%d ox=%d oy=%d contentW=%d w=%d mask=%d maxScrollX=%d momentumAttr=%d\n",
	             rowClass, static_cast<int>(row.style.overflow), static_cast<int>(row.style.overflow_x),
	             static_cast<int>(row.style.overflow_y), row.layout.scroll_content_width, row.layout.width,
	             row.style.mask_right_fade_width, maxScrollX,
	             Tree::instance().hasAttribute(rowId, "momentum") ? 1 : 0);
	if (maxScrollX <= 0) {
		std::fprintf(stderr, "[momentum] %s not horizontally scrollable\n", rowClass);
		return 1;
	}

	const int cx = row.layout.x + row.layout.width / 2;
	const int cy = row.layout.y + row.layout.height / 2;

	// Realistic leftward flick: 5 moves over advancing time (~1.1 px/ms).
	int t = 1000;
	setNativeNowMs(t);
	Tree::instance().pointerDown(cx, cy);
	int x = cx;
	for (int i = 0; i < 5; i++) {
		t += 16;
		setNativeNowMs(t);
		x -= 18;
		Tree::instance().pointerMove(x, cy);
	}
	const int scrollAfterDrag = Tree::instance().node(rowId).layout.scroll_x;
	Tree::instance().pointerUp();
	const int scrollAtUp = Tree::instance().node(rowId).layout.scroll_x;

	int maxSeen = scrollAtUp;
	for (int i = 0; i < 16; i++) {
		t += 16;
		pumpFrame(t);
		const int s = Tree::instance().node(rowId).layout.scroll_x;
		std::fprintf(stderr, "[momentum] %s frame t=%d scroll_x=%d\n", rowClass, t, s);
		if (s > maxSeen) maxSeen = s;
	}
	std::fprintf(stderr, "[momentum] %s scrollAfterDrag=%d scrollAtUp=%d maxSeen=%d coast=%d\n", rowClass,
	             scrollAfterDrag, scrollAtUp, maxSeen, maxSeen - scrollAtUp);
	if (maxSeen <= scrollAtUp) {
		std::fprintf(stderr, "[momentum] FAIL %s: no momentum coast after release\n", rowClass);
		return 1;
	}
	std::fprintf(stderr, "[momentum] PASS %s: coasted %d px after release\n", rowClass, maxSeen - scrollAtUp);
	return 0;
}
}  // namespace

int main()
{
	using namespace gea::embedded::test;

	resetNativeHost();
	setNativeDisplaySize(kViewportWidth, kViewportHeight);
	gea::embedded::ui::Document::setPreferredMountSize(kViewportWidth, kViewportHeight);
	gea::framework::app::Application::init(kViewportWidth, kViewportHeight, kDevicePixelRatio);
	__gea_top_level();
	refresh();
	pumpFrame(16);
	refresh();

	return testRowMomentum("hour-row");
}

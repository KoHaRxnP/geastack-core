#include "native_test_harness.h"

#include "display.h"
#include "ui/internal.h"
#include "ui/tree_internal.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>

namespace gea::framework::app::generated {
void drainMicrotasks() {}
}  // namespace gea::framework::app::generated

namespace gea::framework::graphics::generated {
void ensureLinked() {}
}  // namespace gea::framework::graphics::generated

namespace {
constexpr const char *kTestName = "test_transformed_rounded_rect_main";
constexpr double kPi = 3.14159265358979323846;

bool is_partial_pixel(std::uint16_t pixel, std::uint16_t bg, std::uint16_t fg)
{
	return pixel != bg && pixel != fg;
}

int count_partial_pixels(std::uint16_t bg, std::uint16_t fg)
{
	int count = 0;
	for (int y = 0; y < 220; ++y) {
		for (int x = 0; x < 220; ++x) {
			if (is_partial_pixel(gea::embedded::test::displayPixelAt(x, y), bg, fg)) ++count;
		}
	}
	return count;
}

int count_distinct_partial_colors(std::uint16_t bg, std::uint16_t fg)
{
	std::uint16_t colors[128] = {};
	int count = 0;
	for (int y = 0; y < 220; ++y) {
		for (int x = 0; x < 220; ++x) {
			const std::uint16_t pixel = gea::embedded::test::displayPixelAt(x, y);
			if (!is_partial_pixel(pixel, bg, fg)) continue;
			bool found = false;
			for (int i = 0; i < count; ++i) {
				if (colors[i] == pixel) {
					found = true;
					break;
				}
			}
			if (!found && count < static_cast<int>(sizeof(colors) / sizeof(colors[0]))) colors[count++] = pixel;
		}
	}
	return count;
}

bool has_partial_pixel_in_rect(int x0, int y0, int x1, int y1, std::uint16_t bg, std::uint16_t fg)
{
	for (int y = y0; y <= y1; ++y) {
		for (int x = x0; x <= x1; ++x) {
			if (is_partial_pixel(gea::embedded::test::displayPixelAt(x, y), bg, fg)) return true;
		}
	}
	return false;
}

template <int W, int H>
std::array<std::uint16_t, W * H> snapshotPixels()
{
	std::array<std::uint16_t, W * H> pixels{};
	for (int y = 0; y < H; ++y) {
		for (int x = 0; x < W; ++x)
			pixels[static_cast<std::size_t>(y * W + x)] = gea::embedded::test::displayPixelAt(x, y);
	}
	return pixels;
}

bool screen_to_local(const gea::embedded::ui::Node &node, double screenX, double screenY, double *localX, double *localY)
{
	const double ox = static_cast<double>(node.layout.x) + static_cast<double>(node.layout.width) * 0.5;
	const double oy = static_cast<double>(node.layout.y) + static_cast<double>(node.layout.height) * 0.5;
	const double angle = static_cast<double>(rstyle(node.style).transform_rotate) * kPi / 1800.0;
	const double c = std::cos(angle);
	const double s = std::sin(angle);
	const double dx = screenX - ox;
	const double dy = screenY - oy;
	*localX = ox + dx * c + dy * s;
	*localY = oy - dx * s + dy * c;
	return std::isfinite(*localX) && std::isfinite(*localY);
}

bool safely_inside_rounded_rect(const gea::embedded::ui::Node &node, double localX, double localY)
{
	const double margin = 1.4;
	const double left = static_cast<double>(node.layout.x);
	const double top = static_cast<double>(node.layout.y);
	const double right = left + static_cast<double>(node.layout.width);
	const double bottom = top + static_cast<double>(node.layout.height);
	if (localX < left + margin || localX >= right - margin || localY < top + margin || localY >= bottom - margin) return false;

	const double radius = static_cast<double>(node.style.border_radius[0]) - margin;
	if (radius <= 0.0) return true;
	const double capLeft = left + static_cast<double>(node.style.border_radius[0]);
	const double capRight = right - static_cast<double>(node.style.border_radius[0]);
	const double capTop = top + static_cast<double>(node.style.border_radius[0]);
	const double capBottom = bottom - static_cast<double>(node.style.border_radius[0]);
	const double cx = std::min(std::max(localX, capLeft), capRight);
	const double cy = std::min(std::max(localY, capTop), capBottom);
	const double dx = localX - cx;
	const double dy = localY - cy;
	return dx * dx + dy * dy <= radius * radius;
}

bool expect_transformed_rounded_pill_clips_corners()
{
	using namespace gea::embedded::ui;
	using namespace gea::embedded::test;

	resetNativeHost();
	setNativeDisplaySize(120, 80);
	gea::platform::display::Display::clearNoFlush();
	DisplayList::instance().clear();

	Tree &tree = Tree::instance();
	const int pill = tree.createView();
	Node &node = tree.node(pill);
	node.layout.x = 30;
	node.layout.y = 24;
	node.layout.width = 52;
	node.layout.height = 13;
	node.style.has_bg = 1;
	node.style.bg_color = 0xffff;
	node.style.bg_alpha = 255;
	rstyleMut(node.style).transform_rotate = 1;
	for (int i = 0; i < 4; i++) node.style.border_radius[i] = 6;

	ViewRenderer::recordBox(node);
	DisplayList::instance().replay();

	const auto cornerPixel = displayPixelAt(30, 24);
	const auto centerPixel = displayPixelAt(56, 30);
	if (cornerPixel != 0x0000 || centerPixel != 0xffff) {
		std::fprintf(stderr,
		             "[%s] transformed rounded rect should clip its corners, corner=0x%04x center=0x%04x\n",
		             kTestName,
		             cornerPixel,
		             centerPixel);
		return false;
	}
	return true;
}

bool expect_rotated_fill_quad_antialiases_edges()
{
	using namespace gea::embedded::ui;
	using namespace gea::embedded::test;

	resetNativeHost();
	setNativeDisplaySize(120, 120);
	gea::platform::display::Display::setAA(2);
	gea::platform::display::Display::clearNoFlush();
	DisplayList::instance().clear();

	DisplayCommand *quad = DisplayList::instance().append();
	quad->type = DisplayCommandType::FillQuad;
	quad->bx = 34;
	quad->by = 28;
	quad->bw = 52;
	quad->bh = 64;
	quad->quad.x0 = 58; quad->quad.y0 = 29;
	quad->quad.x1 = 84; quad->quad.y1 = 47;
	quad->quad.x2 = 60; quad->quad.y2 = 91;
	quad->quad.x3 = 34; quad->quad.y3 = 73;
	quad->quad.color = 0xffff;

	DisplayList::instance().replay();

	const int partial = count_partial_pixels(0x0000, 0xffff);
	if (partial <= 0) {
		std::fprintf(stderr,
		             "[%s] rotated FillQuad should produce antialiased edge pixels\n",
		             kTestName);
		return false;
	}
	return true;
}

bool expect_tiny_transformed_rounded_rect_keeps_compact_aa()
{
	using namespace gea::embedded::ui;
	using namespace gea::embedded::test;

	resetNativeHost();
	setNativeDisplaySize(40, 24);
	gea::platform::display::Display::setAA(4);
	gea::platform::display::Display::clearNoFlush();
	DisplayList::instance().clear();

	DisplayCommand *rect = DisplayList::instance().append();
	rect->type = DisplayCommandType::FillTransformedRoundedRect;
	rect->bx = 9;
	rect->by = 9;
	rect->bw = 15;
	rect->bh = 5;
	rect->transformedRoundedRect.x0 = 10; rect->transformedRoundedRect.y0 = 10;
	rect->transformedRoundedRect.x1 = 23; rect->transformedRoundedRect.y1 = 10;
	rect->transformedRoundedRect.x2 = 23; rect->transformedRoundedRect.y2 = 13;
	rect->transformedRoundedRect.x3 = 10; rect->transformedRoundedRect.y3 = 13;
	rect->transformedRoundedRect.lx = 10;
	rect->transformedRoundedRect.ly = 10;
	rect->transformedRoundedRect.lw = 13;
	rect->transformedRoundedRect.lh = 3;
	rect->transformedRoundedRect.tlRx8 = 8; rect->transformedRoundedRect.tlRy8 = 8;
	rect->transformedRoundedRect.trRx8 = 8; rect->transformedRoundedRect.trRy8 = 8;
	rect->transformedRoundedRect.brRx8 = 8; rect->transformedRoundedRect.brRy8 = 8;
	rect->transformedRoundedRect.blRx8 = 8; rect->transformedRoundedRect.blRy8 = 8;
	rect->transformedRoundedRect.color = 0xffff;
	rect->transformedRoundedRect.backfaceHidden = 0;

	DisplayList::instance().replay();

	const auto outsideTop = displayPixelAt(15, 9);
	if (outsideTop != 0x0000) {
		std::fprintf(stderr,
		             "[%s] tiny transformed rounded rect AA should not spill into extra rows, pixel=0x%04x\n",
		             kTestName,
		             outsideTop);
		return false;
	}
	return true;
}

bool expect_axis_aligned_transformed_minus_matches_direct_rounded_rect()
{
	using namespace gea::embedded::ui;
	using namespace gea::embedded::test;

	constexpr int kW = 40;
	constexpr int kH = 24;
	const std::uint16_t bg = 0xe6c0;
	const std::uint16_t fg = 0x0026;

	resetNativeHost();
	setNativeDisplaySize(kW, kH);
	gea::platform::display::Display::setAA(4);
	gea::platform::display::Display::clearNoFlush();
	gea::platform::display::Display::fillRect(0, 0, kW, kH, bg);
	DisplayList::instance().clear();

	DisplayCommand *rect = DisplayList::instance().append();
	rect->type = DisplayCommandType::FillTransformedRoundedRect;
	rect->bx = 9;
	rect->by = 9;
	rect->bw = 15;
	rect->bh = 5;
	rect->transformedRoundedRect.x0 = 10; rect->transformedRoundedRect.y0 = 10;
	rect->transformedRoundedRect.x1 = 23; rect->transformedRoundedRect.y1 = 10;
	rect->transformedRoundedRect.x2 = 23; rect->transformedRoundedRect.y2 = 13;
	rect->transformedRoundedRect.x3 = 10; rect->transformedRoundedRect.y3 = 13;
	rect->transformedRoundedRect.lx = 10;
	rect->transformedRoundedRect.ly = 10;
	rect->transformedRoundedRect.lw = 13;
	rect->transformedRoundedRect.lh = 3;
	rect->transformedRoundedRect.tlRx8 = 8; rect->transformedRoundedRect.tlRy8 = 8;
	rect->transformedRoundedRect.trRx8 = 8; rect->transformedRoundedRect.trRy8 = 8;
	rect->transformedRoundedRect.brRx8 = 8; rect->transformedRoundedRect.brRy8 = 8;
	rect->transformedRoundedRect.blRx8 = 8; rect->transformedRoundedRect.blRy8 = 8;
	rect->transformedRoundedRect.color = fg;
	rect->transformedRoundedRect.backfaceHidden = 0;
	DisplayList::instance().replay();
	const auto transformed = snapshotPixels<kW, kH>();

	resetNativeHost();
	setNativeDisplaySize(kW, kH);
	gea::platform::display::Display::setAA(4);
	gea::platform::display::Display::clearNoFlush();
	gea::platform::display::Display::fillRect(0, 0, kW, kH, bg);
	gea::platform::display::Display::fillRoundedRect(10, 10, 13, 3, 1, 1, 1, 1, fg);
	const auto direct = snapshotPixels<kW, kH>();

	for (std::size_t i = 0; i < transformed.size(); ++i) {
		if (transformed[i] == direct[i]) continue;
		const int x = static_cast<int>(i % kW);
		const int y = static_cast<int>(i / kW);
		std::fprintf(stderr,
		             "[%s] translated minus glyph should use direct rounded-rect rasterizer at (%d,%d): transformed=0x%04x direct=0x%04x\n",
		             kTestName,
		             x,
		             y,
		             transformed[i],
		             direct[i]);
		return false;
	}
	return true;
}

bool expect_axis_aligned_rounded_rect_antialiases_corners()
{
	using namespace gea::embedded::test;

	resetNativeHost();
	setNativeDisplaySize(90, 90);
	gea::platform::display::Display::setAA(2);
	gea::platform::display::Display::clearNoFlush();
	gea::platform::display::Display::fillRoundedRect(20, 16, 49, 49, 24, 24, 24, 24, 0xffff);

	const int partial = count_partial_pixels(0x0000, 0xffff);
	if (partial <= 0) {
		std::fprintf(stderr,
		             "[%s] axis-aligned rounded rect should produce antialiased corner pixels\n",
		             kTestName);
		return false;
	}
	return true;
}

bool expect_large_axis_aligned_circle_uses_2x_antialiasing()
{
	using namespace gea::embedded::test;

	resetNativeHost();
	setNativeDisplaySize(480, 480);
	gea::platform::display::Display::setAA(2);
	gea::platform::display::Display::clearNoFlush();

	const std::uint16_t bg = 0x0000;
	const std::uint16_t fill = 0xe6c0;
	gea::platform::display::Display::fillRect(0, 0, 480, 480, bg);
	gea::platform::display::Display::fillRoundedRect(14, 14, 452, 452, 226, 226, 226, 226, fill);

	const auto outerTopShoulder = displayPixelAt(240, 13);
	if (outerTopShoulder != bg) {
		std::fprintf(stderr,
		             "[%s] large dial circle AA should not paint outside its top edge, pixel=0x%04x\n",
		             kTestName,
		             outerTopShoulder);
		return false;
	}
	const auto topCrest = displayPixelAt(240, 14);
	if (topCrest != fill) {
		std::fprintf(stderr,
		             "[%s] large dial circle top crest should stay solid without a fake AA border, pixel=0x%04x\n",
		             kTestName,
		             topCrest);
		return false;
	}
	if (!has_partial_pixel_in_rect(72, 72, 94, 94, bg, fill)) {
		std::fprintf(stderr,
		             "[%s] large dial circle should still antialias its sloped shoulder with Display.setAA(2)\n",
		             kTestName);
		return false;
	}
	const auto center = displayPixelAt(240, 240);
	if (center != fill) {
		std::fprintf(stderr,
		             "[%s] large dial circle center should stay solid, pixel=0x%04x\n",
		             kTestName,
		             center);
		return false;
	}

	gea::platform::display::Display::clearNoFlush();
	gea::platform::display::Display::fillRect(0, 0, 480, 480, bg);
	gea::platform::display::Display::fillRoundedRect(22, 22, 434, 434, 216, 216, 216, 216, fill);
	const auto evenOuterTopShoulder = displayPixelAt(239, 21);
	if (evenOuterTopShoulder != bg) {
		std::fprintf(stderr,
		             "[%s] even-sized 50%% circle AA should not paint outside its top edge, pixel=0x%04x\n",
		             kTestName,
		             evenOuterTopShoulder);
		return false;
	}
	const auto evenTopCrest = displayPixelAt(239, 22);
	if (evenTopCrest != fill) {
		std::fprintf(stderr,
		             "[%s] even-sized 50%% circle top crest should stay solid without a fake AA border, pixel=0x%04x\n",
		             kTestName,
		             evenTopCrest);
		return false;
	}
	if (!has_partial_pixel_in_rect(77, 77, 99, 99, bg, fill)) {
		std::fprintf(stderr,
		             "[%s] even-sized 50%% circle should still antialias its sloped shoulder\n",
		             kTestName);
		return false;
	}
	return true;
}

bool expect_tiny_axis_aligned_rounded_rect_antialiases_corners()
{
	using namespace gea::embedded::test;

	resetNativeHost();
	setNativeDisplaySize(40, 24);
	gea::platform::display::Display::setAA(4);
	gea::platform::display::Display::clearNoFlush();
	gea::platform::display::Display::fillRoundedRect(10, 10, 13, 3, 1, 1, 1, 1, 0xffff);

	const int partial = count_partial_pixels(0x0000, 0xffff);
	if (partial <= 0) {
		std::fprintf(stderr,
		             "[%s] tiny rounded rect should produce antialiased corner pixels\n",
		             kTestName);
		return false;
	}
	return true;
}

bool expect_percent_circle_background_antialiases_extrema()
{
	using namespace gea::embedded::ui;
	using namespace gea::embedded::test;

	resetNativeHost();
	setNativeDisplaySize(120, 120);
	gea::platform::display::Display::setAA(4);
	gea::platform::display::Display::clearNoFlush();
	DisplayList::instance().clear();

	Tree &tree = Tree::instance();
	const int circle = tree.createView();
	Node &node = tree.node(circle);
	node.layout.x = 10;
	node.layout.y = 10;
	node.layout.width = 91;
	node.layout.height = 91;
	node.style.has_bg = 1;
	node.style.bg_color = 0xffff;
	node.style.bg_alpha = 255;
	for (int i = 0; i < 4; ++i) node.style.border_radius_percent[i] = 500;

	ViewRenderer::recordBox(node);
	DisplayList::instance().replay();

	const auto topCrest = displayPixelAt(55, 10);
	if (topCrest == 0x0000) {
		std::fprintf(stderr,
		             "[%s] percent circle background should cover its top crest\n",
		             kTestName);
		return false;
	}
	if (topCrest != 0xffff) {
		std::fprintf(stderr,
		             "[%s] percent circle top crest should stay solid without a fake AA border, pixel=0x%04x\n",
		             kTestName,
		             topCrest);
		return false;
	}
	const auto outerTopShoulder = displayPixelAt(55, 9);
	if (outerTopShoulder != 0x0000) {
		std::fprintf(stderr,
		             "[%s] percent circle background should not antialias outside the original top edge, pixel=0x%04x\n",
		             kTestName,
		             outerTopShoulder);
		return false;
	}
	if (!has_partial_pixel_in_rect(20, 20, 34, 34, 0x0000, 0xffff)) {
		std::fprintf(stderr,
		             "[%s] percent circle background should still antialias its sloped shoulder\n",
		             kTestName);
		return false;
	}
	return true;
}

bool expect_percent_circle_background_antialiases_diagonal_edges()
{
	using namespace gea::embedded::ui;
	using namespace gea::embedded::test;

	resetNativeHost();
	setNativeDisplaySize(120, 120);
	gea::platform::display::Display::setAA(4);
	gea::platform::display::Display::clearNoFlush();
	DisplayList::instance().clear();

	Tree &tree = Tree::instance();
	const int circle = tree.createView();
	Node &node = tree.node(circle);
	node.layout.x = 10;
	node.layout.y = 10;
	node.layout.width = 91;
	node.layout.height = 91;
	node.style.has_bg = 1;
	node.style.bg_color = 0xffe0;
	node.style.bg_alpha = 255;
	for (int i = 0; i < 4; ++i) node.style.border_radius_percent[i] = 500;

	ViewRenderer::recordBox(node);
	DisplayList::instance().replay();

	const auto diagonal = displayPixelAt(23, 23);
	if (diagonal == 0x0000) {
		std::fprintf(stderr,
		             "[%s] percent circle background should cover diagonal antialias pixels\n",
		             kTestName);
		return false;
	}
	if (!is_partial_pixel(diagonal, 0x0000, 0xffe0)) {
		std::fprintf(stderr,
		             "[%s] percent circle diagonal edge should be antialiased, pixel=0x%04x\n",
		             kTestName,
		             diagonal);
		return false;
	}
	const auto outerDiagonal = displayPixelAt(22, 22);
	if (outerDiagonal != 0x0000) {
		std::fprintf(stderr,
		             "[%s] circle antialiasing should not bloom beyond the true diagonal edge, pixel=0x%04x\n",
		             kTestName,
		             outerDiagonal);
		return false;
	}
	return true;
}

bool expect_small_control_circle_uses_smooth_edge_antialiasing()
{
	using namespace gea::embedded::test;

	resetNativeHost();
	setNativeDisplaySize(70, 70);
	gea::platform::display::Display::setAA(4);
	gea::platform::display::Display::clearNoFlush();

	const std::uint16_t yellow = 0xe6c0;
	const std::uint16_t white = 0xffff;
	gea::platform::display::Display::fillRect(0, 0, 70, 70, yellow);
	gea::platform::display::Display::fillRoundedRect(20, 20, 27, 27, 13, 13, 13, 13, white);

	const int partial = count_partial_pixels(yellow, white);
	if (partial <= 0) {
		std::fprintf(stderr,
		             "[%s] small control circle should still antialias its edge\n",
		             kTestName);
		return false;
	}
	const auto topCenter = displayPixelAt(33, 20);
	if (topCenter != white) {
		std::fprintf(stderr,
		             "[%s] small control circle top center should stay solid without an outside halo, pixel=0x%04x\n",
		             kTestName,
		             topCenter);
		return false;
	}
	const auto diagonalEdge = displayPixelAt(23, 23);
	if (!is_partial_pixel(diagonalEdge, yellow, white)) {
		std::fprintf(stderr,
		             "[%s] small control circle diagonal edge should be visibly antialiased, pixel=0x%04x\n",
		             kTestName,
		             diagonalEdge);
		return false;
	}
	const auto outerTopCenter = displayPixelAt(33, 19);
	if (outerTopCenter != yellow) {
		std::fprintf(stderr,
		             "[%s] small control circle antialiasing should not halo outside its top edge, pixel=0x%04x\n",
		             kTestName,
		             outerTopCenter);
		return false;
	}
	const auto farOuterTopCenter = displayPixelAt(33, 18);
	if (farOuterTopCenter != yellow) {
		std::fprintf(stderr,
		             "[%s] small control circle antialiasing should not bloom two pixels outside its top edge, pixel=0x%04x\n",
		             kTestName,
		             farOuterTopCenter);
		return false;
	}
	const auto leftCenter = displayPixelAt(20, 33);
	if (leftCenter != white) {
		std::fprintf(stderr,
		             "[%s] small control circle side center should stay solid without an outside halo, pixel=0x%04x\n",
		             kTestName,
		             leftCenter);
		return false;
	}
	const auto outerLeftCenter = displayPixelAt(19, 33);
	if (outerLeftCenter != yellow) {
		std::fprintf(stderr,
		             "[%s] small control circle antialiasing should not halo outside its left edge, pixel=0x%04x\n",
		             kTestName,
		             outerLeftCenter);
		return false;
	}
	const auto farOuterLeftCenter = displayPixelAt(18, 33);
	if (farOuterLeftCenter != yellow) {
		std::fprintf(stderr,
		             "[%s] small control circle antialiasing should not bloom two pixels outside its left edge, pixel=0x%04x\n",
		             kTestName,
		             farOuterLeftCenter);
		return false;
	}
	const auto center = displayPixelAt(33, 33);
	if (center != white) {
		std::fprintf(stderr,
		             "[%s] small control circle center should stay solid, pixel=0x%04x\n",
		             kTestName,
		             center);
		return false;
	}
	return true;
}

bool expect_translated_control_circle_matches_direct_aa()
{
	using namespace gea::embedded::ui;
	using namespace gea::embedded::test;

	constexpr int kW = 80;
	constexpr int kH = 80;
	const std::uint16_t yellow = 0xe6c0;
	const std::uint16_t white = 0xffff;

	resetNativeHost();
	setNativeDisplaySize(kW, kH);
	gea::platform::display::Display::setAA(4);
	gea::platform::display::Display::clearNoFlush();
	gea::platform::display::Display::fillRect(0, 0, kW, kH, yellow);
	DisplayList::instance().clear();

	Tree &tree = Tree::instance();
	const int button = tree.createButton();
	Node &node = tree.node(button);
	node.layout.x = 34;
	node.layout.y = 34;
	node.layout.width = 27;
	node.layout.height = 27;
	node.style.has_bg = 1;
	node.style.bg_color = white;
	node.style.bg_alpha = 255;
	rstyleMut(node.style).transform_translate_x_percent = -500;
	rstyleMut(node.style).transform_translate_y_percent = -500;
	for (int i = 0; i < 4; ++i) node.style.border_radius[i] = 14;

	ViewRenderer::recordBox(node);
	DisplayList::instance().replay();
	const auto translated = snapshotPixels<kW, kH>();

	resetNativeHost();
	setNativeDisplaySize(kW, kH);
	gea::platform::display::Display::setAA(4);
	gea::platform::display::Display::clearNoFlush();
	gea::platform::display::Display::fillRect(0, 0, kW, kH, yellow);
	gea::platform::display::Display::fillRoundedRect(21, 21, 27, 27, 13, 13, 13, 13, white);
	const auto direct = snapshotPixels<kW, kH>();

	for (std::size_t i = 0; i < translated.size(); ++i) {
		if (translated[i] == direct[i]) continue;
		const int x = static_cast<int>(i % kW);
		const int y = static_cast<int>(i / kW);
		std::fprintf(stderr,
		             "[%s] translated control circle should keep direct 4x4 rounded-rect AA at (%d,%d): transformed=0x%04x direct=0x%04x\n",
		             kTestName,
		             x,
		             y,
		             translated[i],
		             direct[i]);
		return false;
	}
	return true;
}

bool expect_same_color_button_border_preserves_fill_antialiasing()
{
	using namespace gea::embedded::ui;
	using namespace gea::embedded::test;

	resetNativeHost();
	setNativeDisplaySize(80, 80);
	gea::platform::display::Display::setAA(4);
	gea::platform::display::Display::clearNoFlush();
	DisplayList::instance().clear();

	Tree &tree = Tree::instance();
	const int button = tree.createButton();
	Node &node = tree.node(button);
	node.layout.x = 20;
	node.layout.y = 20;
	node.layout.width = 27;
	node.layout.height = 27;
	node.style.has_bg = 1;
	node.style.bg_color = 0xffff;
	node.style.bg_alpha = 255;
	node.style.border_width = 1;
	node.style.border_color = 0xffff;
	node.style.border_alpha = 255;
	for (int i = 0; i < 4; ++i) node.style.border_radius[i] = 14;

	ViewRenderer::recordBox(node);
	DisplayList::instance().replay();

	const int partial = count_partial_pixels(0x0000, 0xffff);
	if (partial <= 0) {
		std::fprintf(stderr,
		             "[%s] same-color button border should not overwrite rounded fill antialiasing\n",
		             kTestName);
		return false;
	}
	return true;
}

bool expect_fan_sized_pill_fill_and_border_antialias()
{
	using namespace gea::embedded::test;

	resetNativeHost();
	setNativeDisplaySize(120, 60);
	gea::platform::display::Display::setAA(4);
	gea::platform::display::Display::clearNoFlush();

	const std::uint16_t yellow = 0xe6c0;
	const std::uint16_t fill = 0x10a6;
	const std::uint16_t border = 0x322b;
	gea::platform::display::Display::fillRect(0, 0, 120, 60, yellow);
	gea::platform::display::Display::fillRoundedRect(15, 16, 90, 26, 13, 13, 13, 13, fill);
	gea::platform::display::Display::strokeRoundedRect(15, 16, 90, 26, 13, 13, 13, 13, 1, border);

	const auto outerEdge = displayPixelAt(24, 16);
	if (outerEdge == yellow || outerEdge == fill || outerEdge == border) {
		std::fprintf(stderr,
		             "[%s] fan-sized pill outer border edge should be antialiased, pixel=0x%04x\n",
		             kTestName,
		             outerEdge);
		return false;
	}
	return true;
}

bool expect_rounded_stroke_antialiases_outer_corners()
{
	using namespace gea::embedded::ui;
	using namespace gea::embedded::test;

	resetNativeHost();
	setNativeDisplaySize(80, 80);
	gea::platform::display::Display::setAA(4);
	gea::platform::display::Display::clearNoFlush();
	DisplayList::instance().clear();

	gea::platform::display::Display::strokeRoundedRect(20, 20, 27, 27, 13, 13, 13, 13, 1, 0xffff);

	const auto topShoulder = displayPixelAt(31, 20);
	if (topShoulder == 0x0000) {
		std::fprintf(stderr,
		             "[%s] rounded stroke should cover antialiased top shoulder pixels\n",
		             kTestName);
		return false;
	}
	if (!is_partial_pixel(topShoulder, 0x0000, 0xffff)) {
		std::fprintf(stderr,
		             "[%s] rounded stroke top shoulder should be antialiased, pixel=0x%04x\n",
		             kTestName,
		             topShoulder);
		return false;
	}
	return true;
}

bool expect_transformed_rounded_rect_antialiases_edges()
{
	using namespace gea::embedded::ui;
	using namespace gea::embedded::test;

	resetNativeHost();
	setNativeDisplaySize(120, 120);
	gea::platform::display::Display::setAA(2);
	gea::platform::display::Display::clearNoFlush();
	DisplayList::instance().clear();

	Tree &tree = Tree::instance();
	const int tick = tree.createView();
	Node &node = tree.node(tick);
	node.layout.x = 54;
	node.layout.y = 32;
	node.layout.width = 8;
	node.layout.height = 52;
	node.style.has_bg = 1;
	node.style.bg_color = 0xffff;
	node.style.bg_alpha = 255;
	rstyleMut(node.style).transform_rotate = 285;
	for (int i = 0; i < 4; ++i) node.style.border_radius[i] = 3;

	ViewRenderer::recordBox(node);
	DisplayList::instance().replay();

	const int partial = count_partial_pixels(0x0000, 0xffff);
	if (partial <= 0) {
		std::fprintf(stderr,
		             "[%s] transformed rounded rect should produce antialiased edge pixels\n",
		             kTestName);
		return false;
	}
	return true;
}

bool expect_display_antialiasing_quality_gate()
{
	using namespace gea::embedded::test;

	resetNativeHost();
	setNativeDisplaySize(90, 90);
	gea::platform::display::Display::setAA(0);
	gea::platform::display::Display::clearNoFlush();
	gea::platform::display::Display::fillRoundedRect(20, 16, 49, 49, 24, 24, 24, 24, 0xffff);
	const int noAAPartial = count_partial_pixels(0x0000, 0xffff);
	if (noAAPartial != 0) {
		std::fprintf(stderr,
		             "[%s] Display.setAA(0) should leave rounded div edges un-antialiased, partial=%d\n",
		             kTestName,
		             noAAPartial);
		return false;
	}

	resetNativeHost();
	setNativeDisplaySize(90, 90);
	gea::platform::display::Display::setAA(2);
	gea::platform::display::Display::clearNoFlush();
	gea::platform::display::Display::fillRoundedRect(20, 16, 49, 49, 24, 24, 24, 24, 0xffff);
	const int aa2Partial = count_partial_pixels(0x0000, 0xffff);
	const int aa2Colors = count_distinct_partial_colors(0x0000, 0xffff);
	if (aa2Partial <= 0) {
		std::fprintf(stderr,
		             "[%s] Display.setAA(2) should antialias rounded div edges\n",
		             kTestName);
		return false;
	}

	resetNativeHost();
	setNativeDisplaySize(90, 90);
	gea::platform::display::Display::setAA(4);
	gea::platform::display::Display::clearNoFlush();
	gea::platform::display::Display::fillRoundedRect(20, 16, 49, 49, 24, 24, 24, 24, 0xffff);
	const int aa4Colors = count_distinct_partial_colors(0x0000, 0xffff);
	if (aa4Colors <= aa2Colors) {
		std::fprintf(stderr,
		             "[%s] Display.setAA(4) should produce finer rounded div edge coverage than 2x2, aa2Colors=%d aa4Colors=%d\n",
		             kTestName,
		             aa2Colors,
		             aa4Colors);
		return false;
	}
	return true;
}

bool expect_tall_transformed_pill_keeps_straight_side()
{
	using namespace gea::embedded::ui;
	using namespace gea::embedded::test;

	resetNativeHost();
	setNativeDisplaySize(140, 200);
	gea::platform::display::Display::clearNoFlush();
	DisplayList::instance().clear();

	Tree &tree = Tree::instance();
	const int pill = tree.createView();
	Node &node = tree.node(pill);
	node.layout.x = 70;
	node.layout.y = 40;
	node.layout.width = 12;
	node.layout.height = 120;
	node.style.has_bg = 1;
	node.style.bg_color = 0xffff;
	node.style.bg_alpha = 255;
	rstyleMut(node.style).transform_rotate = 1;
	for (int i = 0; i < 4; i++) node.style.border_radius[i] = 6;

	ViewRenderer::recordBox(node);
	DisplayList::instance().replay();

	const auto edgePixel = displayPixelAt(71, 50);
	if (edgePixel != 0xffff) {
		std::fprintf(stderr,
		             "[%s] tall transformed pill should keep its straight side, edge=0x%04x\n",
		             kTestName,
		             edgePixel);
		return false;
	}
	return true;
}

bool expect_steep_transformed_pill_has_no_internal_gaps()
{
	using namespace gea::embedded::ui;
	using namespace gea::embedded::test;

	resetNativeHost();
	setNativeDisplaySize(120, 80);
	gea::platform::display::Display::clearNoFlush();
	DisplayList::instance().clear();

	Tree &tree = Tree::instance();
	const int pill = tree.createView();
	Node &node = tree.node(pill);
	node.layout.x = 34;
	node.layout.y = 24;
	node.layout.width = 52;
	node.layout.height = 13;
	node.style.has_bg = 1;
	node.style.bg_color = 0xffff;
	node.style.bg_alpha = 255;
	rstyleMut(node.style).transform_rotate = -600;
	for (int i = 0; i < 4; i++) node.style.border_radius[i] = 6;

	ViewRenderer::recordBox(node);
	DisplayList::instance().replay();

	int interiorPixels = 0;
	int holes = 0;
	int firstHoleX = -1;
	int firstHoleY = -1;
	double firstLocalX = 0.0;
	double firstLocalY = 0.0;
	for (int y = 0; y < 80; ++y) {
		for (int x = 0; x < 120; ++x) {
			double localX = 0.0;
			double localY = 0.0;
			if (!screen_to_local(node, static_cast<double>(x) + 0.5, static_cast<double>(y) + 0.5, &localX, &localY)) continue;
			if (!safely_inside_rounded_rect(node, localX, localY)) continue;
			++interiorPixels;
			if (displayPixelAt(x, y) == 0x0000) {
				if (holes == 0) {
					firstHoleX = x;
					firstHoleY = y;
					firstLocalX = localX;
					firstLocalY = localY;
				}
				++holes;
			}
		}
	}

	if (interiorPixels <= 0 || holes > 0) {
		std::fprintf(stderr,
		             "[%s] steep transformed rounded rect should not expose background inside, interior=%d holes=%d first=(%d,%d local %.2f,%.2f)\n",
		             kTestName,
		             interiorPixels,
		             holes,
		             firstHoleX,
		             firstHoleY,
		             firstLocalX,
		             firstLocalY);
		return false;
	}
	return true;
}

bool expect_percent_radius_draws_ellipse_not_capsule()
{
	using namespace gea::embedded::ui;
	using namespace gea::embedded::test;

	resetNativeHost();
	setNativeDisplaySize(120, 80);
	gea::platform::display::Display::clearNoFlush();
	DisplayList::instance().clear();

	Tree &tree = Tree::instance();
	const int pill = tree.createView();
	Node &node = tree.node(pill);
	node.layout.x = 34;
	node.layout.y = 24;
	node.layout.width = 52;
	node.layout.height = 13;
	node.style.has_bg = 1;
	node.style.bg_color = 0xffff;
	node.style.bg_alpha = 255;
	rstyleMut(node.style).transform_rotate = 1;
	for (int i = 0; i < 4; i++) {
		node.style.border_radius[i] = 0;
		node.style.border_radius_percent[i] = 500;
	}

	ViewRenderer::recordBox(node);
	DisplayList::instance().replay();

	const auto capsuleOnlyPixel = displayPixelAt(37, 26);
	const auto centerPixel = displayPixelAt(60, 30);
	if (capsuleOnlyPixel != 0x0000 || centerPixel != 0xffff) {
		std::fprintf(stderr,
		             "[%s] percent border-radius should draw an ellipse, capsuleOnly=0x%04x center=0x%04x\n",
		             kTestName,
		             capsuleOnlyPixel,
		             centerPixel);
		return false;
	}
	return true;
}

bool expect_axis_aligned_percent_radius_draws_ellipse()
{
	using namespace gea::embedded::ui;
	using namespace gea::embedded::test;

	resetNativeHost();
	setNativeDisplaySize(120, 80);
	gea::platform::display::Display::clearNoFlush();
	DisplayList::instance().clear();

	Tree &tree = Tree::instance();
	const int pill = tree.createView();
	Node &node = tree.node(pill);
	node.layout.x = 34;
	node.layout.y = 24;
	node.layout.width = 52;
	node.layout.height = 13;
	node.style.has_bg = 1;
	node.style.bg_color = 0xffff;
	node.style.bg_alpha = 255;
	for (int i = 0; i < 4; i++) {
		node.style.border_radius[i] = 0;
		node.style.border_radius_percent[i] = 500;
	}

	ViewRenderer::recordBox(node);
	DisplayList::instance().replay();

	const auto capsuleOnlyPixel = displayPixelAt(37, 26);
	const auto centerPixel = displayPixelAt(60, 30);
	if (capsuleOnlyPixel != 0x0000 || centerPixel != 0xffff) {
		std::fprintf(stderr,
		             "[%s] axis-aligned percent border-radius should draw an ellipse, capsuleOnly=0x%04x center=0x%04x\n",
		             kTestName,
		             capsuleOnlyPixel,
		             centerPixel);
		return false;
	}
	return true;
}

bool expect_temperature_dial_ticks_replay_quickly()
{
	using namespace gea::embedded::ui;
	using namespace gea::embedded::test;

	resetNativeHost();
	setNativeDisplaySize(480, 480);
	gea::platform::display::Display::clearNoFlush();
	DisplayList::instance().clear();

	Tree &tree = Tree::instance();
	constexpr int kTickCount = 56;
	constexpr double kCenter = 240.0;
	constexpr double kRadius = 174.0;
	constexpr double kWidth = 7.0;
	constexpr double kHeight = 36.0;
	constexpr double kStartAngle = -135.0;
	constexpr double kArc = 270.0;
	for (int i = 0; i < kTickCount; ++i) {
		const double progress = kTickCount <= 1 ? 0.0 : static_cast<double>(i) / static_cast<double>(kTickCount - 1);
		const double angle = kStartAngle + progress * kArc;
		const double radians = angle * kPi / 180.0;
		const int tick = tree.createView();
		Node &node = tree.node(tick);
		node.layout.width = static_cast<int>(std::lround(kWidth));
		node.layout.height = static_cast<int>(std::lround(kHeight));
		node.layout.x = static_cast<int>(std::lround(kCenter + std::sin(radians) * kRadius - kWidth * 0.5));
		node.layout.y = static_cast<int>(std::lround(kCenter - std::cos(radians) * kRadius - kHeight * 0.5));
		node.style.has_bg = 1;
		node.style.bg_color = i < 28 ? 0xffff : 0x000f;
		node.style.bg_alpha = 255;
		rstyleMut(node.style).transform_rotate = static_cast<int16_t>(std::lround(angle * 10.0));
		for (int corner = 0; corner < 4; ++corner) node.style.border_radius[corner] = 3;
		ViewRenderer::recordBox(node);
	}

	const auto start = std::chrono::steady_clock::now();
	DisplayList::instance().replay();
	const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
	    std::chrono::steady_clock::now() - start).count();
	if (elapsed > 3) {
		std::fprintf(stderr,
		             "[%s] 56 tiny transformed rounded ticks replayed too slowly: %lldms\n",
		             kTestName,
		             static_cast<long long>(elapsed));
		return false;
	}
	if (displayNonzeroPixelCount() <= 0) {
		std::fprintf(stderr, "[%s] expected transformed rounded ticks to draw pixels\n", kTestName);
		return false;
	}
	return true;
}
}  // namespace

int main()
{
	if (!expect_display_antialiasing_quality_gate()) return 1;
	if (!expect_rotated_fill_quad_antialiases_edges()) return 1;
	if (!expect_tiny_transformed_rounded_rect_keeps_compact_aa()) return 1;
	if (!expect_axis_aligned_transformed_minus_matches_direct_rounded_rect()) return 1;
	if (!expect_axis_aligned_rounded_rect_antialiases_corners()) return 1;
	if (!expect_large_axis_aligned_circle_uses_2x_antialiasing()) return 1;
	if (!expect_tiny_axis_aligned_rounded_rect_antialiases_corners()) return 1;
	if (!expect_percent_circle_background_antialiases_extrema()) return 1;
	if (!expect_percent_circle_background_antialiases_diagonal_edges()) return 1;
	if (!expect_small_control_circle_uses_smooth_edge_antialiasing()) return 1;
	if (!expect_translated_control_circle_matches_direct_aa()) return 1;
	if (!expect_same_color_button_border_preserves_fill_antialiasing()) return 1;
	if (!expect_fan_sized_pill_fill_and_border_antialias()) return 1;
	if (!expect_rounded_stroke_antialiases_outer_corners()) return 1;
	if (!expect_transformed_rounded_rect_antialiases_edges()) return 1;
	if (!expect_transformed_rounded_pill_clips_corners()) return 1;
	if (!expect_tall_transformed_pill_keeps_straight_side()) return 1;
	if (!expect_steep_transformed_pill_has_no_internal_gaps()) return 1;
	if (!expect_percent_radius_draws_ellipse_not_capsule()) return 1;
	if (!expect_axis_aligned_percent_radius_draws_ellipse()) return 1;
	if (!expect_temperature_dial_ticks_replay_quickly()) return 1;
	return 0;
}

#include "native_test_harness.h"

#include "canvas.h"
#include "display.h"
#include "pixel.h"
#include "ui/internal.h"
#include "ui/refresh_perf.h"
#include "ui/style.h"
#include "ui/tree_internal.h"

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <string>
#include <vector>

#include <unistd.h>

extern void __gea_top_level();
namespace gea::framework::app::generated {
void drainMicrotasks();
}  // namespace gea::framework::app::generated

namespace {
constexpr const char *kTestName = "test_gea_temperature_dial_local_refresh";
std::vector<std::uint16_t> gBackdropPixels;

extern "C" std::uint16_t *gea_backdrop_cache(int *cap_px)
{
	constexpr int kCapacity = 480 * 480;
	if (gBackdropPixels.size() < kCapacity) gBackdropPixels.assign(kCapacity, 0);
	if (cap_px) *cap_px = kCapacity;
	return gBackdropPixels.data();
}

void fail_on_alarm(int /*signal*/)
{
	std::fputs("[test_gea_temperature_dial_local_refresh] timed out while driving app frame\n", stderr);
	std::_Exit(124);
}

void drain_refresh()
{
	gea::framework::app::generated::drainMicrotasks();
	gea::embedded::test::refresh();
}

bool mount_temperature_dial()
{
	gea::embedded::test::resetNativeHost();
	gea::embedded::test::setNativeDisplaySize(480, 480);
	gea::embedded::ui::setViewportMetrics(480, 480, 1.5);
	__gea_top_level();
	drain_refresh();

	auto &tree = gea::embedded::ui::Tree::instance();
	const int initialCommandCount = gea::embedded::ui::DisplayList::instance().commandCount();
	if (tree.nodeCount() >= 50 && initialCommandCount > 0) return true;
	std::fprintf(stderr,
	             "[%s] unexpected initial app shape nodes=%d commands=%d\n",
	             kTestName,
	             tree.nodeCount(),
	             initialCommandCount);
	gea::embedded::test::dumpTree(kTestName);
	return false;
}

bool expect_square_box(const char *className)
{
	const std::vector<int> nodes = gea::embedded::test::nodesWithClass(className);
	if (nodes.empty()) {
		std::fprintf(stderr, "[%s] expected a node with class %s\n", kTestName, className);
		gea::embedded::test::dumpTree(kTestName);
		return false;
	}
	auto &tree = gea::embedded::ui::Tree::instance();
	const auto &node = tree.node(nodes.front());
	if (node.layout.width != node.layout.height) {
		std::fprintf(stderr,
		             "[%s] expected %s to lay out square, got layout=(%d,%d %dx%d) style=%dx%d\n",
		             kTestName,
		             className,
		             node.layout.x,
		             node.layout.y,
		             node.layout.width,
		             node.layout.height,
		             node.style.width,
		             node.style.height);
		gea::embedded::test::dumpTree(kTestName);
		return false;
	}
	return true;
}

const char *command_type_name(gea::embedded::ui::DisplayCommandType type)
{
	switch (type) {
	case gea::embedded::ui::DisplayCommandType::FillRoundedRect:
		return "FillRoundedRect";
	case gea::embedded::ui::DisplayCommandType::StrokeRoundedRect:
		return "StrokeRoundedRect";
	case gea::embedded::ui::DisplayCommandType::FillTransformedRoundedRect:
		return "FillTransformedRoundedRect";
	case gea::embedded::ui::DisplayCommandType::FillRect:
		return "FillRect";
	case gea::embedded::ui::DisplayCommandType::DrawText:
		return "DrawText";
	case gea::embedded::ui::DisplayCommandType::DrawProjectedText:
		return "DrawProjectedText";
	default:
		return "other";
	}
}

bool expect_solid_background_command(const char *className, gea::embedded::ui::DisplayCommandType expected)
{
	const std::vector<int> nodes = gea::embedded::test::nodesWithClass(className);
	if (nodes.empty()) {
		std::fprintf(stderr, "[%s] expected a node with class %s\n", kTestName, className);
		gea::embedded::test::dumpTree(kTestName);
		return false;
	}
	auto &list = gea::embedded::ui::DisplayList::instance();
	const int node = nodes.front();
	const int count = list.nodeCommandCount(node);
	for (int i = 0; i < count; ++i) {
		const auto *command = list.nodeCommandAt(node, i);
		if (!command) continue;
		if (command->type == expected) return true;
		if (command->type == gea::embedded::ui::DisplayCommandType::FillRoundedRect ||
		    command->type == gea::embedded::ui::DisplayCommandType::FillTransformedRoundedRect ||
		    command->type == gea::embedded::ui::DisplayCommandType::FillRect) {
			std::fprintf(stderr,
			             "[%s] expected %s background to record as %s, got %s\n",
			             kTestName,
			             className,
			             command_type_name(expected),
			             command_type_name(command->type));
			gea::embedded::test::dumpTree(kTestName);
			return false;
		}
	}
	std::fprintf(stderr,
	             "[%s] expected %s to have a solid background command, count=%d\n",
	             kTestName,
	             className,
	             count);
	gea::embedded::test::dumpTree(kTestName);
	return false;
}

bool expect_temperature_dial_geometry()
{
	return expect_square_box("temperature-dial-shell") &&
	       expect_square_box("temperature-dial-orbit") &&
	       expect_square_box("temperature-dial-climate") &&
	       expect_solid_background_command("temperature-dial-climate",
	                                       gea::embedded::ui::DisplayCommandType::FillRoundedRect);
}

void dump_node_commands(const char *phase, const char *className);

bool expect_range_labels_use_direct_text_commands()
{
	const std::vector<int> nodes = gea::embedded::test::nodesWithClass("temperature-dial-range-label");
	if (nodes.size() < 2) {
		std::fprintf(stderr, "[%s] expected two temperature range label nodes, got %zu\n", kTestName, nodes.size());
		gea::embedded::test::dumpTree(kTestName);
		return false;
	}

	auto &tree = gea::embedded::ui::Tree::instance();
	auto &list = gea::embedded::ui::DisplayList::instance();
	int directTextLabels = 0;
	for (int node : nodes) {
		const auto &n = tree.node(node);
		if (n.type != gea::embedded::ui::NodeType::Text) continue;

		bool sawText = false;
		for (int i = 0; i < list.nodeCommandCount(node); ++i) {
			const auto *command = list.nodeCommandAt(node, i);
			if (!command) continue;
			if (command->type == gea::embedded::ui::DisplayCommandType::DrawProjectedText) {
				std::fprintf(stderr,
				             "[%s] pure-translated range label '%s' should use DrawText, got DrawProjectedText at command %d\n",
				             kTestName,
				             n.text.c_str(),
				             i);
				dump_node_commands("range-label", "temperature-dial-range-label");
				return false;
			}
			if (command->type == gea::embedded::ui::DisplayCommandType::DrawText) sawText = true;
		}
		if (sawText) ++directTextLabels;
	}

	if (directTextLabels < 2) {
		std::fprintf(stderr,
		             "[%s] expected both range labels to emit DrawText commands, got %d\n",
		             kTestName,
		             directTextLabels);
		gea::embedded::test::dumpTree(kTestName);
		return false;
	}
	return true;
}

bool is_partial_pixel(std::uint16_t pixel, std::uint16_t bg, std::uint16_t fill)
{
	return pixel != bg && pixel != fill;
}

bool has_partial_pixel_in_rect(int x0, int y0, int x1, int y1, std::uint16_t bg, std::uint16_t fill)
{
	for (int y = y0; y <= y1; ++y) {
		for (int x = x0; x <= x1; ++x) {
			if (is_partial_pixel(gea::embedded::test::displayPixelAt(x, y), bg, fill)) return true;
		}
	}
	return false;
}

int rgb_distance(std::uint16_t a, std::uint16_t b)
{
	a = gea::framework::graphics::pixel::toRgb565(a);
	b = gea::framework::graphics::pixel::toRgb565(b);
	const int ar = (a >> 11) & 0x1f;
	const int ag = (a >> 5) & 0x3f;
	const int ab = a & 0x1f;
	const int br = (b >> 11) & 0x1f;
	const int bg = (b >> 5) & 0x3f;
	const int bb = b & 0x1f;
	return std::abs(ar - br) * 8 + std::abs(ag - bg) * 4 + std::abs(ab - bb) * 8;
}

bool expect_climate_circle_antialiases_without_top_halo(const char *phase)
{
	if (gea::framework::graphics::Canvas::antialiasSamples() < 2) {
		std::fprintf(stderr,
		             "[%s] temperature dial should enable Display.setAA(2), samples=%d\n",
		             kTestName,
		             gea::framework::graphics::Canvas::antialiasSamples());
		return false;
	}

	const std::vector<int> nodes = gea::embedded::test::nodesWithClass("temperature-dial-climate");
	if (nodes.empty()) {
		std::fprintf(stderr, "[%s] expected a climate circle node\n", kTestName);
		gea::embedded::test::dumpTree(kTestName);
		return false;
	}

	const auto &node = gea::embedded::ui::Tree::instance().node(nodes.front());
	const int cx = node.layout.x + node.layout.width / 2;
	const int outsideY = node.layout.y - 1;
	const std::uint16_t bg = gea::embedded::test::displayPixelAt(cx, outsideY - 1);
	const std::uint16_t outside = gea::embedded::test::displayPixelAt(cx, outsideY);
	const std::uint16_t topCrest = gea::embedded::test::displayPixelAt(cx, node.layout.y);
	const std::uint16_t fill = gea::embedded::test::displayPixelAt(cx, node.layout.y + 4);
	if (outside != bg) {
		std::fprintf(stderr,
		             "[%s] climate circle AA must not paint a halo outside its box after %s, layout=(%d,%d %dx%d) sample=(%d,%d) bg=0x%04x outside=0x%04x fill=0x%04x\n",
		             kTestName,
		             phase,
		             node.layout.x,
		             node.layout.y,
		             node.layout.width,
		             node.layout.height,
		             cx,
		             outsideY,
		             bg,
		             outside,
		             fill);
		return false;
	}
	if (topCrest != fill) {
		std::fprintf(stderr,
		             "[%s] climate circle top crest should stay solid without a fake AA border after %s, layout=(%d,%d %dx%d) sample=(%d,%d) bg=0x%04x crest=0x%04x fill=0x%04x\n",
		             kTestName,
		             phase,
		             node.layout.x,
		             node.layout.y,
		             node.layout.width,
		             node.layout.height,
		             cx,
		             node.layout.y,
		             bg,
		             topCrest,
		             fill);
		return false;
	}
	const int shoulderLeft = cx - node.layout.width / 4;
	const int shoulderRight = cx - node.layout.width / 16;
	const int shoulderTop = node.layout.y + 4;
	const int shoulderBottom = node.layout.y + 32;
	if (!has_partial_pixel_in_rect(shoulderLeft, shoulderTop, shoulderRight, shoulderBottom, bg, fill)) {
		std::fprintf(stderr,
		             "[%s] climate circle should still antialias its sloped shoulder after %s, layout=(%d,%d %dx%d) sampleRect=(%d,%d)-(%d,%d) bg=0x%04x fill=0x%04x\n",
		             kTestName,
		             phase,
		             node.layout.x,
		             node.layout.y,
		             node.layout.width,
		             node.layout.height,
		             shoulderLeft,
		             shoulderTop,
		             shoulderRight,
		             shoulderBottom,
		             bg,
		             fill);
		return false;
	}
	return true;
}

bool expect_idle_frames_do_not_accumulate_climate_antialias()
{
	const std::vector<int> nodes = gea::embedded::test::nodesWithClass("temperature-dial-climate");
	if (nodes.empty()) {
		std::fprintf(stderr, "[%s] expected a climate circle node\n", kTestName);
		gea::embedded::test::dumpTree(kTestName);
		return false;
	}
	const auto &node = gea::embedded::ui::Tree::instance().node(nodes.front());
	const int cx = node.layout.x + node.layout.width / 2;
	const int edgeY = node.layout.y - 1;
	const std::uint16_t before = gea::embedded::test::displayPixelAt(cx, edgeY);
	for (int frame = 0; frame < 8; ++frame)
		gea::embedded::test::pumpFrame(32 + frame * 16);
	const std::uint16_t after = gea::embedded::test::displayPixelAt(cx, edgeY);
	if (after != before) {
		std::fprintf(stderr,
		             "[%s] idle frames must not accumulate rounded-rect AA edge alpha, before=0x%04x after=0x%04x sample=(%d,%d)\n",
		             kTestName,
		             before,
		             after,
		             cx,
		             edgeY);
		return false;
	}
	return true;
}

bool expect_initial_refresh_clears_display_dirty()
{
	if (gea::embedded::ui::Tree::instance().refreshRequired()) {
		std::fprintf(stderr,
		             "[%s] initial refresh should clear display framebuffer dirty state so idle frames do not repaint static AA edges\n",
		             kTestName);
		return false;
	}
	return true;
}

bool press_first_class_center(const char *className)
{
	const std::vector<int> nodes = gea::embedded::test::nodesWithClass(className);
	if (nodes.empty()) {
		std::fprintf(stderr, "[%s] expected a node with class %s\n", kTestName, className);
		gea::embedded::test::dumpTree(kTestName);
		return false;
	}
	const auto &node = gea::embedded::ui::Tree::instance().node(nodes.front());
	const int x = node.layout.x + node.layout.width / 2;
	const int y = node.layout.y + node.layout.height / 2;
	gea::embedded::test::dispatchTouch(gea::framework::events::TouchPhase::Down, true, x, y);
	gea::embedded::test::dispatchTouch(gea::framework::events::TouchPhase::Up, false, x, y);
	return true;
}

void dump_refresh_perf(const char *phase, const gea::embedded::ui::RefreshPerfStats &perf)
{
	if (!std::getenv("GEA_DUMP_TEMPERATURE_PERF")) return;
	auto &tree = gea::embedded::ui::Tree::instance();
	std::fprintf(stderr,
	             "[%s] %s replayUs=%lld regions=%d originRegions=%d commandChecks=%d draw(fill=%d rounded=%d transformedRounded=%d text=%d other=%d) bgRecolor=%d/%dpx fastBgPx=%d flushPixels=%d\n",
	             kTestName,
	             phase,
	             static_cast<long long>(perf.treeReplayUs),
	             perf.treeReplayRegions,
	             perf.treeReplayOriginRegions,
	             perf.treeReplayCommandChecks,
	             perf.treeReplayFillRectCommands,
	             perf.treeReplayRoundedRectCommands,
	             perf.treeReplayTransformedRoundedRectCommands,
	             perf.treeReplayTextCommands,
	             perf.treeReplayOtherCommands,
	             perf.treeBgRecolorCalls,
	             perf.treeBgRecolorPixels,
	             perf.treeBgRecolorFastPixels,
	             gea::embedded::test::flushPixelCount());
	for (int i = 0; i < perf.treeReplayRegionSampleCount; ++i) {
		const int origin = perf.treeReplayRegionOrigin[i];
		std::string originClass;
		if (origin >= 0 && origin < tree.nodeCount()) originClass = tree.className(origin);
		std::fprintf(stderr,
		             "[%s] %s region[%d]=(%d,%d)-(%d,%d) origin=%d class=%s\n",
		             kTestName,
		             phase,
		             i,
		             perf.treeReplayRegionX0[i],
		             perf.treeReplayRegionY0[i],
		             perf.treeReplayRegionX1[i],
		             perf.treeReplayRegionY1[i],
		             origin,
		             originClass.c_str());
	}
}

bool expect_no_large_climate_recolor(const char *phase, const gea::embedded::ui::RefreshPerfStats &perf)
{
	constexpr int kMaxImmediateRecolorPixels = 50000;
	if (perf.treeBgRecolorPixels <= kMaxImmediateRecolorPixels) return true;
	std::fprintf(stderr,
	             "[%s] %s should only retained-recolor small changed controls/ticks, not the full climate circle; recolorCalls=%d recolorPixels=%d fastBgPx=%d replayUs=%lld flushPixels=%d\n",
	             kTestName,
	             phase,
	             perf.treeBgRecolorCalls,
	             perf.treeBgRecolorPixels,
	             perf.treeBgRecolorFastPixels,
	             static_cast<long long>(perf.treeReplayUs),
	             gea::embedded::test::flushPixelCount());
	return false;
}

bool copy_display_region(int x0, int y0, int x1, int y1, std::vector<std::uint16_t> &out)
{
	auto *canvas = gea::platform::display::Display::canvas();
	if (!canvas || !canvas->pixels() || canvas->width() <= 0 || canvas->height() <= 0) return false;
	if (x0 < 0) x0 = 0;
	if (y0 < 0) y0 = 0;
	if (x1 >= canvas->width()) x1 = canvas->width() - 1;
	if (y1 >= canvas->height()) y1 = canvas->height() - 1;
	if (x0 > x1 || y0 > y1) return false;
	const int w = x1 - x0 + 1;
	const int h = y1 - y0 + 1;
	const int stride = canvas->strideBytes() / static_cast<int>(sizeof(std::uint16_t));
	out.assign(static_cast<std::size_t>(w * h), 0);
	for (int y = 0; y < h; ++y) {
		const std::uint16_t *row = canvas->pixels() + canvas->rowToPhysical(y0 + y) * stride + x0;
		std::copy(row, row + w, out.begin() + static_cast<std::size_t>(y * w));
	}
	return true;
}

bool copy_presented_region(int x0, int y0, int x1, int y1, std::vector<std::uint16_t> &out)
{
	auto *canvas = gea::platform::display::Display::canvas();
	if (!canvas || canvas->width() <= 0 || canvas->height() <= 0) return false;
	if (x0 < 0) x0 = 0;
	if (y0 < 0) y0 = 0;
	if (x1 >= canvas->width()) x1 = canvas->width() - 1;
	if (y1 >= canvas->height()) y1 = canvas->height() - 1;
	if (x0 > x1 || y0 > y1) return false;
	const int w = x1 - x0 + 1;
	const int h = y1 - y0 + 1;
	out.assign(static_cast<std::size_t>(w * h), 0);
	for (int y = 0; y < h; ++y) {
		for (int x = 0; x < w; ++x)
			out[static_cast<std::size_t>(y * w + x)] = gea::embedded::test::presentedPixelAt(x0 + x, y0 + y);
	}
	return true;
}

bool expect_dirty_frame_matches_full_replay(int x0, int y0, int x1, int y1, const char *phase)
{
	auto *canvas = gea::platform::display::Display::canvas();
	if (!canvas || !canvas->pixels()) return false;
	if (x0 < 0) x0 = 0;
	if (y0 < 0) y0 = 0;
	if (x1 >= canvas->width()) x1 = canvas->width() - 1;
	if (y1 >= canvas->height()) y1 = canvas->height() - 1;
	if (x0 > x1 || y0 > y1) return false;
	const int w = x1 - x0 + 1;
	std::vector<std::uint16_t> dirty;
	std::vector<std::uint16_t> full;
	if (!copy_display_region(x0, y0, x1, y1, dirty)) return false;
	gea::platform::display::Display::clearNoFlush();
	gea::platform::display::Display::resetClip();
	gea::platform::display::Display::setAlpha(255);
	gea::embedded::ui::DisplayList::instance().replay();
	gea::platform::display::Display::resetClip();
	gea::platform::display::Display::setAlpha(255);
	if (!copy_display_region(x0, y0, x1, y1, full)) return false;

	int mismatchCount = 0;
	int maxDistance = 0;
	int firstX = -1;
	int firstY = -1;
	std::uint16_t firstDirty = 0;
	std::uint16_t firstFull = 0;
	constexpr int kMaxReverseBlendDistance = 24;
	for (std::size_t i = 0; i < dirty.size(); ++i) {
		const int distance = rgb_distance(dirty[i], full[i]);
		if (distance > maxDistance) maxDistance = distance;
		if (distance <= kMaxReverseBlendDistance) continue;
		++mismatchCount;
		if (firstX < 0) {
			firstX = x0 + static_cast<int>(i % static_cast<std::size_t>(w));
			firstY = y0 + static_cast<int>(i / static_cast<std::size_t>(w));
			firstDirty = dirty[i];
			firstFull = full[i];
		}
	}
	if (mismatchCount == 0) {
		gea::platform::display::Display::flush();
		gea::platform::display::Display::flushStatsReset();
		return true;
	}
	std::fprintf(stderr,
	             "[%s] dirty retained frame must match a full replay after %s in region=(%d,%d)-(%d,%d), mismatches=%d maxDist=%d tolerance=%d first=(%d,%d) dirty=0x%04x full=0x%04x dist=%d\n",
	             kTestName,
	             phase,
	             x0,
	             y0,
	             x1,
	             y1,
	             mismatchCount,
	             maxDistance,
	             kMaxReverseBlendDistance,
	             firstX,
	             firstY,
	             firstDirty,
	             firstFull,
	             rgb_distance(firstDirty, firstFull));
	return false;
}

bool expect_presented_frame_matches_full_replay(int x0, int y0, int x1, int y1, const char *phase)
{
	auto *canvas = gea::platform::display::Display::canvas();
	if (!canvas || !canvas->pixels()) return false;
	if (x0 < 0) x0 = 0;
	if (y0 < 0) y0 = 0;
	if (x1 >= canvas->width()) x1 = canvas->width() - 1;
	if (y1 >= canvas->height()) y1 = canvas->height() - 1;
	if (x0 > x1 || y0 > y1) return false;
	const int w = x1 - x0 + 1;
	std::vector<std::uint16_t> presented;
	std::vector<std::uint16_t> full;
	if (!copy_presented_region(x0, y0, x1, y1, presented)) return false;
	gea::platform::display::Display::clearNoFlush();
	gea::platform::display::Display::resetClip();
	gea::platform::display::Display::setAlpha(255);
	gea::embedded::ui::DisplayList::instance().replay();
	gea::platform::display::Display::resetClip();
	gea::platform::display::Display::setAlpha(255);
	if (!copy_display_region(x0, y0, x1, y1, full)) return false;

	int mismatchCount = 0;
	int maxDistance = 0;
	int firstX = -1;
	int firstY = -1;
	std::uint16_t firstPresented = 0;
	std::uint16_t firstFull = 0;
	constexpr int kMaxPresentedDistance = 8;
	for (std::size_t i = 0; i < presented.size(); ++i) {
		const int distance = rgb_distance(presented[i], full[i]);
		if (distance > maxDistance) maxDistance = distance;
		if (distance <= kMaxPresentedDistance) continue;
		++mismatchCount;
		if (firstX < 0) {
			firstX = x0 + static_cast<int>(i % static_cast<std::size_t>(w));
			firstY = y0 + static_cast<int>(i / static_cast<std::size_t>(w));
			firstPresented = presented[i];
			firstFull = full[i];
		}
	}
	if (mismatchCount == 0) {
		gea::platform::display::Display::flush();
		gea::platform::display::Display::flushStatsReset();
		return true;
	}
	std::fprintf(stderr,
	             "[%s] presented panel pixels must match a full replay after %s in region=(%d,%d)-(%d,%d), mismatches=%d maxDist=%d tolerance=%d first=(%d,%d) presented=0x%04x full=0x%04x dist=%d\n",
	             kTestName,
	             phase,
	             x0,
	             y0,
	             x1,
	             y1,
	             mismatchCount,
	             maxDistance,
	             kMaxPresentedDistance,
	             firstX,
	             firstY,
	             firstPresented,
	             firstFull,
	             rgb_distance(firstPresented, firstFull));
	return false;
}

bool expect_temperature_tick_matches_full_replay(int tickIndex, const char *phase)
{
	const std::vector<int> tickNodes = gea::embedded::test::nodesWithClass("temperature-dial-tick");
	if (tickIndex < 0 || tickIndex >= static_cast<int>(tickNodes.size())) {
		std::fprintf(stderr,
		             "[%s] expected tick index %d for %s, got %zu tick nodes\n",
		             kTestName,
		             tickIndex,
		             phase,
		             tickNodes.size());
		gea::embedded::test::dumpTree(kTestName);
		return false;
	}
	int x0 = 0;
	int y0 = 0;
	int x1 = -1;
	int y1 = -1;
	if (!gea::embedded::ui::DisplayList::instance().nodeCommandBounds(tickNodes[static_cast<std::size_t>(tickIndex)],
	                                                                  &x0,
	                                                                  &y0,
	                                                                  &x1,
	                                                                  &y1)) {
		std::fprintf(stderr, "[%s] expected tick %d to have command bounds for %s\n", kTestName, tickIndex, phase);
		return false;
	}
	constexpr int kPad = 2;
	return expect_dirty_frame_matches_full_replay(x0 - kPad, y0 - kPad, x1 + kPad, y1 + kPad, phase);
}

bool expect_temperature_tick_presented_matches_full_replay(int tickIndex, const char *phase)
{
	const std::vector<int> tickNodes = gea::embedded::test::nodesWithClass("temperature-dial-tick");
	if (tickIndex < 0 || tickIndex >= static_cast<int>(tickNodes.size())) {
		std::fprintf(stderr,
		             "[%s] expected tick index %d for %s, got %zu tick nodes\n",
		             kTestName,
		             tickIndex,
		             phase,
		             tickNodes.size());
		gea::embedded::test::dumpTree(kTestName);
		return false;
	}
	int x0 = 0;
	int y0 = 0;
	int x1 = -1;
	int y1 = -1;
	if (!gea::embedded::ui::DisplayList::instance().nodeCommandBounds(tickNodes[static_cast<std::size_t>(tickIndex)],
	                                                                  &x0,
	                                                                  &y0,
	                                                                  &x1,
	                                                                  &y1)) {
		std::fprintf(stderr, "[%s] expected tick %d to have command bounds for %s\n", kTestName, tickIndex, phase);
		return false;
	}
	constexpr int kPad = 2;
	return expect_presented_frame_matches_full_replay(x0 - kPad, y0 - kPad, x1 + kPad, y1 + kPad, phase);
}

bool expect_first_node_matches_full_replay(const char *className, const char *phase, int pad = 2)
{
	const std::vector<int> nodes = gea::embedded::test::nodesWithClass(className);
	if (nodes.empty()) {
		std::fprintf(stderr, "[%s] expected node with class %s for %s\n", kTestName, className, phase);
		gea::embedded::test::dumpTree(kTestName);
		return false;
	}
	int x0 = 0;
	int y0 = 0;
	int x1 = -1;
	int y1 = -1;
	if (!gea::embedded::ui::DisplayList::instance().nodeCommandBounds(nodes.front(), &x0, &y0, &x1, &y1)) {
		std::fprintf(stderr, "[%s] expected .%s to have command bounds for %s\n", kTestName, className, phase);
		return false;
	}
	return expect_dirty_frame_matches_full_replay(x0 - pad, y0 - pad, x1 + pad, y1 + pad, phase);
}

bool expect_first_node_presented_matches_full_replay(const char *className, const char *phase, int pad = 2)
{
	const std::vector<int> nodes = gea::embedded::test::nodesWithClass(className);
	if (nodes.empty()) {
		std::fprintf(stderr, "[%s] expected node with class %s for %s\n", kTestName, className, phase);
		gea::embedded::test::dumpTree(kTestName);
		return false;
	}
	int x0 = 0;
	int y0 = 0;
	int x1 = -1;
	int y1 = -1;
	if (!gea::embedded::ui::DisplayList::instance().nodeCommandBounds(nodes.front(), &x0, &y0, &x1, &y1)) {
		std::fprintf(stderr, "[%s] expected .%s to have command bounds for %s\n", kTestName, className, phase);
		return false;
	}
	return expect_presented_frame_matches_full_replay(x0 - pad, y0 - pad, x1 + pad, y1 + pad, phase);
}

void dump_node_commands(const char *phase, const char *className)
{
	if (!std::getenv("GEA_DUMP_TEMPERATURE_PERF")) return;
	const std::vector<int> nodes = gea::embedded::test::nodesWithClass(className);
	if (nodes.empty()) return;
	auto &list = gea::embedded::ui::DisplayList::instance();
	const int node = nodes.front();
	int bx0 = 0;
	int by0 = 0;
	int bx1 = -1;
	int by1 = -1;
	const bool hasBounds = list.nodeCommandBounds(node, &bx0, &by0, &bx1, &by1);
	std::fprintf(stderr,
	             "[%s] %s .%s node=%d commandCount=%d bounds=%s(%d,%d)-(%d,%d)\n",
	             kTestName,
	             phase,
	             className,
	             node,
	             list.nodeCommandCount(node),
	             hasBounds ? "" : "none ",
	             bx0,
	             by0,
	             bx1,
	             by1);
	for (int i = 0; i < list.nodeCommandCount(node); ++i) {
		const auto *command = list.nodeCommandAt(node, i);
		if (!command) continue;
		std::fprintf(stderr,
		             "[%s] %s .%s command[%d]=%s bbox=(%d,%d %dx%d)\n",
		             kTestName,
		             phase,
		             className,
		             i,
		             command_type_name(command->type),
		             command->bx,
		             command->by,
		             command->bw,
		             command->bh);
	}
}

}  // namespace

int main()
{
	std::signal(SIGALRM, fail_on_alarm);
	alarm(8);

	if (!mount_temperature_dial()) return 1;
	dump_node_commands("mount", "temperature-dial-value");
	if (!expect_temperature_dial_geometry()) return 1;
	if (!expect_range_labels_use_direct_text_commands()) return 1;
	if (!expect_climate_circle_antialiases_without_top_halo("mount")) return 1;
	if (!expect_initial_refresh_clears_display_dirty()) return 1;
	if (!expect_idle_frames_do_not_accumulate_climate_antialias()) return 1;

	if (!press_first_class_center("temperature-dial-control-plus")) {
		std::fprintf(stderr, "[%s] failed to press the plus control for retained-AA regression\n", kTestName);
		gea::embedded::test::dumpTree(kTestName);
		return 1;
	}
	gea::embedded::test::pumpFrame(16);
	if (!expect_temperature_tick_presented_matches_full_replay(12, "first tick presented flush")) return 1;
	if (!expect_temperature_tick_matches_full_replay(12, "first tick retained recolor")) return 1;
	gea::embedded::ui::refreshPerfStatsReset();
	gea::platform::display::Display::flushStatsReset();
	gea::embedded::test::pumpFrame(32);
	if (!expect_first_node_presented_matches_full_replay("temperature-dial-climate", "first climate presented flush")) return 1;
	if (!expect_temperature_tick_matches_full_replay(12, "first climate retained recolor over tick")) return 1;
	if (!expect_first_node_matches_full_replay("temperature-dial-fan", "first climate retained recolor over fan")) return 1;
	if (!expect_first_node_matches_full_replay("temperature-dial-climate", "first climate retained recolor")) return 1;

	if (!mount_temperature_dial()) return 1;
	auto &tree = gea::embedded::ui::Tree::instance();
	const int initialCommandCount = gea::embedded::ui::DisplayList::instance().commandCount();

	gea::embedded::ui::refreshPerfStatsReset();
	gea::platform::display::Display::flushStatsReset();
	if (!press_first_class_center("temperature-dial-control-plus")) {
		std::fprintf(stderr, "[%s] failed to press the plus control\n", kTestName);
		gea::embedded::test::dumpTree(kTestName);
		return 1;
	}
		gea::embedded::test::pumpFrame(16);
		const auto pressPerf = gea::embedded::ui::refreshPerfStatsRead();
		dump_refresh_perf("plus", pressPerf);

	if (!gea::embedded::test::expectContains(gea::embedded::test::rootTextContent(), "22.5", "updated temperature", kTestName)) {
		gea::embedded::test::dumpTree(kTestName);
		return 1;
	}
	if (!expect_climate_circle_antialiases_without_top_halo("plus press")) return 1;
	if (pressPerf.treeRecordCalls != 0 || pressPerf.treeRecordedNodes != 0 ||
	    gea::embedded::ui::DisplayList::instance().commandCount() != initialCommandCount) {
		std::fprintf(stderr,
		             "[%s] plus press should direct-replay local temperature changes, recordCalls=%d recordedNodes=%d commands=%d initialCommands=%d direct=%d refreshes=%d\n",
		             kTestName,
		             pressPerf.treeRecordCalls,
		             pressPerf.treeRecordedNodes,
		             gea::embedded::ui::DisplayList::instance().commandCount(),
		             initialCommandCount,
		             pressPerf.treeDirectReplayCalls,
		             pressPerf.treeRefreshCalls);
		gea::embedded::test::dumpTree(kTestName);
		return 1;
	}
	if (pressPerf.treeDirectReplayCalls <= 0) {
		std::fprintf(stderr,
		             "[%s] plus press should use direct dirty-region replay, direct=%d refreshes=%d\n",
		             kTestName,
		             pressPerf.treeDirectReplayCalls,
		             pressPerf.treeRefreshCalls);
		gea::embedded::test::dumpTree(kTestName);
		return 1;
	}
	if (!expect_no_large_climate_recolor("plus press", pressPerf)) {
		gea::embedded::test::dumpTree(kTestName);
		return 1;
	}
	if (gea::embedded::ui::DisplayList::instance().staticBackdropActive()) {
		std::fprintf(stderr,
		             "[%s] static transformed tick/button paint changes must not arm the full-surface backdrop cache\n",
		             kTestName);
		return 1;
	}

	gea::embedded::ui::refreshPerfStatsReset();
	gea::platform::display::Display::flushStatsReset();
	if (!press_first_class_center("temperature-dial-control-plus")) {
		std::fprintf(stderr, "[%s] failed to press the plus control a second time\n", kTestName);
		gea::embedded::test::dumpTree(kTestName);
		return 1;
	}
	gea::embedded::test::pumpFrame(32);
	const auto secondPressPerf = gea::embedded::ui::refreshPerfStatsRead();
	if (!gea::embedded::test::expectContains(gea::embedded::test::rootTextContent(), "23C", "second updated temperature", kTestName)) {
		gea::embedded::test::dumpTree(kTestName);
		return 1;
	}
	if (secondPressPerf.treeRecordCalls != 0 || secondPressPerf.treeRecordedNodes != 0) {
		std::fprintf(stderr,
		             "[%s] second plus press should not trigger a full display-list record, recordCalls=%d recordedNodes=%d\n",
		             kTestName,
		             secondPressPerf.treeRecordCalls,
		             secondPressPerf.treeRecordedNodes);
		gea::embedded::test::dumpTree(kTestName);
		return 1;
	}
	if (gea::embedded::ui::DisplayList::instance().staticBackdropActive()) {
		std::fprintf(stderr,
		             "[%s] repeated temperature changes must not arm the transform-only static backdrop cache\n",
		             kTestName);
		return 1;
	}

	for (int step = 0; step < 14; ++step) {
		gea::embedded::ui::refreshPerfStatsReset();
		if (!press_first_class_center("temperature-dial-control-plus")) {
			std::fprintf(stderr, "[%s] failed to press the plus control during boundary sweep step=%d\n", kTestName, step);
			gea::embedded::test::dumpTree(kTestName);
			return 1;
		}
		gea::embedded::test::pumpFrame(48 + step * 16);
		const auto sweepPerf = gea::embedded::ui::refreshPerfStatsRead();
		if (sweepPerf.treeRecordCalls != 0 || sweepPerf.treeRecordedNodes != 0) {
			std::fprintf(stderr,
			             "[%s] temperature boundary sweep should not trigger a full display-list record, step=%d recordCalls=%d recordedNodes=%d\n",
			             kTestName,
			             step,
			             sweepPerf.treeRecordCalls,
			             sweepPerf.treeRecordedNodes);
			gea::embedded::test::dumpTree(kTestName);
			return 1;
		}
		if (gea::embedded::ui::DisplayList::instance().staticBackdropActive()) {
			std::fprintf(stderr,
			             "[%s] temperature boundary sweep must not arm the transform-only static backdrop cache, step=%d\n",
			             kTestName,
			             step);
			return 1;
		}
	}
	if (!gea::embedded::test::expectContains(gea::embedded::test::rootTextContent(), "30C", "swept max temperature", kTestName)) {
		gea::embedded::test::dumpTree(kTestName);
		return 1;
	}

	if (!mount_temperature_dial()) return 1;
	gea::embedded::ui::refreshPerfStatsReset();
	gea::platform::display::Display::flushStatsReset();
	for (int step = 0; step < 20; ++step) {
		if (!press_first_class_center("temperature-dial-control-plus")) {
			std::fprintf(stderr, "[%s] failed to queue plus control burst step=%d\n", kTestName, step);
			gea::embedded::test::dumpTree(kTestName);
			return 1;
		}
	}
	gea::embedded::test::pumpFrame(64);
	const auto burstPerf = gea::embedded::ui::refreshPerfStatsRead();
	dump_refresh_perf("plus-burst", burstPerf);
	if (!gea::embedded::test::expectContains(gea::embedded::test::rootTextContent(), "30C", "burst max temperature", kTestName)) {
		gea::embedded::test::dumpTree(kTestName);
		return 1;
	}
	if (burstPerf.treeRecordCalls != 0 || burstPerf.treeRecordedNodes != 0) {
		std::fprintf(stderr,
		             "[%s] burst temperature update should stay on direct replay, recordCalls=%d recordedNodes=%d\n",
		             kTestName,
		             burstPerf.treeRecordCalls,
		             burstPerf.treeRecordedNodes);
		gea::embedded::test::dumpTree(kTestName);
		return 1;
	}
	if (!expect_no_large_climate_recolor("queued burst", burstPerf)) {
		gea::embedded::test::dumpTree(kTestName);
		return 1;
	}
	gea::embedded::ui::refreshPerfStatsReset();
	gea::platform::display::Display::flushStatsReset();
	for (int frame = 0; frame < 20; ++frame) {
		gea::embedded::test::pumpFrame(80 + frame * 16);
	}
	const auto settlePerf = gea::embedded::ui::refreshPerfStatsRead();
	dump_refresh_perf("plus-burst-settle", settlePerf);
	if (settlePerf.treeBgRecolorCalls <= 0 || settlePerf.treeBgRecolorFastPixels <= 0) {
		std::fprintf(stderr,
		             "[%s] settled burst should repaint the climate circle exactly on the delayed color frame, recolorCalls=%d recolorPixels=%d fastBgPx=%d replayUs=%lld flushPixels=%d\n",
		             kTestName,
		             settlePerf.treeBgRecolorCalls,
		             settlePerf.treeBgRecolorPixels,
		             settlePerf.treeBgRecolorFastPixels,
		             static_cast<long long>(settlePerf.treeReplayUs),
		             gea::embedded::test::flushPixelCount());
		gea::embedded::test::dumpTree(kTestName);
		return 1;
	}
	if (gea::embedded::test::flushPixelCount() >= 170000) {
		std::fprintf(stderr,
		             "[%s] settled burst background recolor should preserve circular flush bands instead of coalescing them with steady foreground/text regions; flushPixels=%d recolorPixels=%d\n",
		             kTestName,
		             gea::embedded::test::flushPixelCount(),
		             settlePerf.treeBgRecolorPixels);
		gea::embedded::test::dumpTree(kTestName);
		return 1;
	}
	if (!expect_first_node_matches_full_replay("temperature-dial-fan", "settled climate retained recolor over fan")) return 1;
	if (!expect_first_node_presented_matches_full_replay("temperature-dial-fan", "settled climate retained recolor over fan")) return 1;

	if (!mount_temperature_dial()) return 1;
	const int touchInitialCommandCount = gea::embedded::ui::DisplayList::instance().commandCount();

	gea::embedded::ui::refreshPerfStatsReset();
	gea::platform::display::Display::flushStatsReset();
	gea::embedded::test::dispatchTouch(gea::framework::events::TouchPhase::Down, true, 344, 396);
	const auto downPerf = gea::embedded::ui::refreshPerfStatsRead();
	if (downPerf.treeRefreshCalls != 0 || downPerf.treeRecordCalls != 0 || downPerf.treeRecordedNodes != 0 ||
	    gea::embedded::ui::DisplayList::instance().commandCount() != touchInitialCommandCount) {
		std::fprintf(stderr,
		             "[%s] touch down should only mark active-state dirty; it must not synchronously refresh, recordCalls=%d recordedNodes=%d commands=%d initialCommands=%d direct=%d refreshes=%d\n",
		             kTestName,
		             downPerf.treeRecordCalls,
		             downPerf.treeRecordedNodes,
		             gea::embedded::ui::DisplayList::instance().commandCount(),
		             touchInitialCommandCount,
		             downPerf.treeDirectReplayCalls,
		             downPerf.treeRefreshCalls);
		gea::embedded::test::dumpTree(kTestName);
		return 1;
	}

	gea::embedded::ui::refreshPerfStatsReset();
	gea::platform::display::Display::flushStatsReset();
	gea::embedded::test::pumpFrame(16);
	const auto downFramePerf = gea::embedded::ui::refreshPerfStatsRead();
	if (downFramePerf.treeRecordCalls != 0 || downFramePerf.treeRecordedNodes != 0 ||
	    gea::embedded::ui::DisplayList::instance().commandCount() != touchInitialCommandCount) {
		std::fprintf(stderr,
		             "[%s] touch down active-state frame should direct-replay locally, recordCalls=%d recordedNodes=%d commands=%d initialCommands=%d direct=%d refreshes=%d\n",
		             kTestName,
		             downFramePerf.treeRecordCalls,
		             downFramePerf.treeRecordedNodes,
		             gea::embedded::ui::DisplayList::instance().commandCount(),
		             touchInitialCommandCount,
		             downFramePerf.treeDirectReplayCalls,
		             downFramePerf.treeRefreshCalls);
		gea::embedded::test::dumpTree(kTestName);
		return 1;
	}
	if (downFramePerf.treeDirectReplayCalls <= 0) {
		std::fprintf(stderr,
		             "[%s] touch down active-state frame should use direct dirty-region replay, direct=%d refreshes=%d\n",
		             kTestName,
		             downFramePerf.treeDirectReplayCalls,
		             downFramePerf.treeRefreshCalls);
		gea::embedded::test::dumpTree(kTestName);
		return 1;
	}

	gea::embedded::ui::refreshPerfStatsReset();
	gea::platform::display::Display::flushStatsReset();
	gea::embedded::test::dispatchTouch(gea::framework::events::TouchPhase::Up, false, 344, 396);
		const auto upDispatchPerf = gea::embedded::ui::refreshPerfStatsRead();
		if (upDispatchPerf.treeRefreshCalls != 0 || upDispatchPerf.treeRecordCalls != 0 || upDispatchPerf.treeRecordedNodes != 0) {
			std::fprintf(stderr,
			             "[%s] touch up should coalesce active-state cleanup with the next app frame, refreshes=%d recordCalls=%d recordedNodes=%d\n",
			             kTestName,
			             upDispatchPerf.treeRefreshCalls,
			             upDispatchPerf.treeRecordCalls,
			             upDispatchPerf.treeRecordedNodes);
			gea::embedded::test::dumpTree(kTestName);
			return 1;
		}
		gea::embedded::test::pumpFrame(32);
		const auto upPerf = gea::embedded::ui::refreshPerfStatsRead();
		dump_refresh_perf("touch-up", upPerf);
	if (!gea::embedded::test::expectContains(gea::embedded::test::rootTextContent(), "22.5", "touch-up temperature", kTestName)) {
		gea::embedded::test::dumpTree(kTestName);
		return 1;
	}
	if (upPerf.treeRecordCalls != 0 || upPerf.treeRecordedNodes != 0 ||
	    gea::embedded::ui::DisplayList::instance().commandCount() != touchInitialCommandCount) {
		std::fprintf(stderr,
		             "[%s] touch up plus update should direct-replay local changes, recordCalls=%d recordedNodes=%d commands=%d initialCommands=%d direct=%d refreshes=%d\n",
		             kTestName,
		             upPerf.treeRecordCalls,
		             upPerf.treeRecordedNodes,
		             gea::embedded::ui::DisplayList::instance().commandCount(),
		             touchInitialCommandCount,
		             upPerf.treeDirectReplayCalls,
		             upPerf.treeRefreshCalls);
		gea::embedded::test::dumpTree(kTestName);
		return 1;
	}
	if (!expect_no_large_climate_recolor("touch-up temperature update", upPerf)) {
		gea::embedded::test::dumpTree(kTestName);
		return 1;
	}
	if (gea::embedded::ui::DisplayList::instance().staticBackdropActive()) {
		std::fprintf(stderr,
		             "[%s] touch-up static transformed paint changes must not arm the full-surface backdrop cache\n",
		             kTestName);
		return 1;
	}

	alarm(0);
	return 0;
}

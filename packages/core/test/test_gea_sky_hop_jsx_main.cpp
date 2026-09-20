#include "native_test_harness.h"

#include "display.h"
#include "ui/refresh_perf.h"
#include "ui/tree_internal.h"

#include <cstdint>
#include <cstdio>
#include <vector>

extern void __gea_top_level();

namespace {

int findPlayerImage()
{
	auto &tree = gea::embedded::ui::Tree::instance();
	const auto imageNodes = gea::embedded::test::nodesWithType(gea::embedded::ui::NodeType::Image);
	for (int nodeId : imageNodes) {
		const auto &node = tree.node(nodeId);
		if ((node.style.width == 38 || node.layout.width == 38) &&
		    (node.style.height == 38 || node.layout.height == 38)) {
			return nodeId;
		}
	}
	return -1;
}

int findCameraWorld()
{
	auto &tree = gea::embedded::ui::Tree::instance();
	for (int nodeId = 0; nodeId < tree.nodeCount(); ++nodeId) {
		const auto &node = tree.node(nodeId);
		if (node.type == gea::embedded::ui::NodeType::View &&
		    (node.style.width == 2196 || node.layout.width == 2196)) {
			return nodeId;
		}
	}
	return -1;
}

}  // namespace

int main()
{
	using namespace gea::embedded::test;
	// The fast path ships gated off by default; this test exercises and verifies it,
	// so enable it for the whole run. The per-frame correctness block toggles it
	// per-run to compare against the full-replay reference.
	gea::embedded::ui::gHorizontalPanFastPathDisabled = false;
	resetNativeHost();
	__gea_top_level();
	refresh();

	if (imageLoadCount() != 8) {
		std::fprintf(stderr, "[test_gea_sky_hop_jsx_main] expected 8 image loads, got %d\n", imageLoadCount());
		dumpTree("test_gea_sky_hop_jsx_main");
		return 1;
	}

	auto text = rootTextContent();
	if (!expectContains(text, "Sky Hop", "initial text", "test_gea_sky_hop_jsx_main")) return 1;
	if (!expectContains(text, "Coins 0/8", "initial text", "test_gea_sky_hop_jsx_main")) return 1;
	if (!expectContains(text, "Lives 3", "initial text", "test_gea_sky_hop_jsx_main")) return 1;
	if (!expectContains(text, "L", "initial text", "test_gea_sky_hop_jsx_main")) return 1;
	if (!expectContains(text, "R", "initial text", "test_gea_sky_hop_jsx_main")) return 1;
	if (!expectContains(text, "JUMP", "initial text", "test_gea_sky_hop_jsx_main")) return 1;

	auto &tree = gea::embedded::ui::Tree::instance();
	const int leftText = firstNodeWithText("L", true);
	if (leftText < 0) return 1;
	const int leftButton = tree.node(leftText).parent;
	if (leftButton < 0) return 1;
	const auto inactiveLeftBg = tree.node(leftButton).style.bg_color;
	dispatchTouch(gea::framework::events::TouchPhase::Down, true, 43, 457);
	refresh();
	if (tree.node(leftButton).style.bg_color == inactiveLeftBg) {
		std::fprintf(stderr, "[test_gea_sky_hop_jsx_main] left control did not enter pressed state after touch down\n");
		dumpTree("test_gea_sky_hop_jsx_main");
		return 1;
	}
	dispatchTouch(gea::framework::events::TouchPhase::Up, false, 43, 457);
	refresh();
	if (tree.node(leftButton).style.bg_color != inactiveLeftBg) {
		std::fprintf(stderr, "[test_gea_sky_hop_jsx_main] left control did not leave pressed state after touch up\n");
		dumpTree("test_gea_sky_hop_jsx_main");
		return 1;
	}

	if (nodesWithType(gea::embedded::ui::NodeType::Image).size() < 20) {
		std::fprintf(stderr, "[test_gea_sky_hop_jsx_main] expected world images for background, player, and tiles\n");
		dumpTree("test_gea_sky_hop_jsx_main");
		return 1;
	}
	if (nodesWithBox(16, 16).size() < 8 || nodesWithBox(36, 36).size() < 20) {
		std::fprintf(stderr, "[test_gea_sky_hop_jsx_main] expected coin and tile nodes in the world\n");
		dumpTree("test_gea_sky_hop_jsx_main");
		return 1;
	}

	pumpFrame(16);
	pumpFrame(48);
	text = rootTextContent();
	if (!expectContains(text, "Sky Hop", "frame text", "test_gea_sky_hop_jsx_main")) return 1;
	if (!expectContains(text, "Lives 3", "frame text", "test_gea_sky_hop_jsx_main")) return 1;

	int playerImage = -1;
	int stableLeft = 0;
	int stableTop = 0;
	for (int frame = 0; frame < 12; ++frame) {
		pumpFrame(64 + frame * 16);
		playerImage = findPlayerImage();
		if (playerImage < 0) {
			std::fprintf(stderr, "[test_gea_sky_hop_jsx_main] expected visible 38x38 player image\n");
			dumpTree("test_gea_sky_hop_jsx_main");
			return 1;
		}
		const int left = tree.node(playerImage).style.pos_offsets[3];
		const int top = tree.node(playerImage).style.pos_offsets[0];
		if (frame == 0) {
			stableLeft = left;
			stableTop = top;
		} else if (left != stableLeft) {
			std::fprintf(stderr,
			             "[test_gea_sky_hop_jsx_main] standing player left jittered from %d to %d on idle frame %d\n",
			             stableLeft,
			             left,
			             frame);
			dumpTree("test_gea_sky_hop_jsx_main");
			return 1;
		} else if (top != stableTop) {
			std::fprintf(stderr,
			             "[test_gea_sky_hop_jsx_main] standing player top jittered from %d to %d on idle frame %d\n",
			             stableTop,
			             top,
			             frame);
			dumpTree("test_gea_sky_hop_jsx_main");
			return 1;
		}
	}

	refresh();

	text = rootTextContent();
	if (!expectContains(text, "Coins 0/8", "post-frame text", "test_gea_sky_hop_jsx_main")) return 1;
	if (!expectContains(text, "Lives 3", "post-frame text", "test_gea_sky_hop_jsx_main")) return 1;

	const int cameraWorld = findCameraWorld();
	if (cameraWorld < 0) {
		std::fprintf(stderr, "[test_gea_sky_hop_jsx_main] expected retained camera/world node\n");
		dumpTree("test_gea_sky_hop_jsx_main");
		return 1;
	}
	dispatchTouch(gea::framework::events::TouchPhase::Down, true, 117, 457);
	refresh();
	const int cameraBeforeRun = tree.node(cameraWorld).layout.x;
	gea::embedded::ui::refreshPerfStatsReset();
	gea::platform::display::Display::flushStatsReset();
	for (int frame = 0; frame < 90; ++frame) {
		pumpFrame(288 + frame * 16);
	}
	const auto motionPerf = gea::embedded::ui::refreshPerfStatsRead();
	const auto motionFlush = gea::platform::display::Display::flushPerfStatsRead();
	std::fprintf(stderr,
	             "[skyhop-perf] frames=90 flushCalls=%d (%.2f/frame) flushPixels=%d (%d/frame) "
	             "refreshes=%d directReplay=%d panReplay=%d panRepaintArea=%d (%d/frame) recordCalls=%d recordedNodes=%d recordedCommands=%d "
	             "replayUs=%lld flushRectsUs=%lld dirtyCollectUs=%lld\n",
	             motionFlush.callCount,
	             motionFlush.callCount / 90.0,
	             motionFlush.pixelCount,
	             motionFlush.pixelCount / 90,
	             motionPerf.treeRefreshCalls,
	             motionPerf.treeDirectReplayCalls,
	             motionPerf.treePanReplayCalls,
	             motionPerf.treePanRepaintArea,
	             motionPerf.treePanRepaintArea / 90,
	             motionPerf.treeRecordCalls,
	             motionPerf.treeRecordedNodes,
	             motionPerf.treeRecordedCommands,
	             static_cast<long long>(motionPerf.treeReplayUs),
	             static_cast<long long>(motionPerf.treeFlushRectsUs),
	             static_cast<long long>(motionPerf.treeDirtyCollectUs));
	if (tree.node(cameraWorld).layout.x == cameraBeforeRun) {
		std::fprintf(stderr,
		             "[test_gea_sky_hop_jsx_main] expected right movement to shift camera world, camera=%d\n",
		             tree.node(cameraWorld).layout.x);
		dumpTree("test_gea_sky_hop_jsx_main");
		return 1;
	}
	const int recordCallLimit = motionPerf.treeRefreshCalls / 4;
	if (motionPerf.treeRecordCalls > recordCallLimit) {
		std::fprintf(stderr,
		             "[test_gea_sky_hop_jsx_main] moving camera/player should not full-record most frames, recordCalls=%d limit=%d recordedNodes=%d direct=%d refreshes=%d\n",
		             motionPerf.treeRecordCalls,
		             recordCallLimit,
		             motionPerf.treeRecordedNodes,
		             motionPerf.treeDirectReplayCalls,
		             motionPerf.treeRefreshCalls);
		dumpTree("test_gea_sky_hop_jsx_main");
		return 1;
	}
	// The moving camera/world must take the horizontal-pan fast path (memcpy-shift +
	// strip/sprite repaint) on most frames instead of re-replaying the whole viewport.
	// A handful of frames legitimately fall back (the ~every-36px re-record that brings
	// newly-revealed tiles on screen, or a control press/release), so require the fast
	// path to dominate the motion run rather than asserting it on every single frame.
	if (motionPerf.treePanReplayCalls <= motionPerf.treeRefreshCalls / 2) {
		std::fprintf(stderr,
		             "[test_gea_sky_hop_jsx_main] moving camera/player should use the horizontal-pan fast path on most frames, pan=%d direct=%d refreshes=%d\n",
		             motionPerf.treePanReplayCalls,
		             motionPerf.treeDirectReplayCalls,
		             motionPerf.treeRefreshCalls);
		dumpTree("test_gea_sky_hop_jsx_main");
		return 1;
	}
	// The pan fast path must dramatically cut per-frame *raster* work: the old path
	// re-replayed the whole ~410x466 (~191k px) game viewport every frame. With the
	// memcpy-shift only the revealed strip + sprites + smeared hill band are repainted,
	// so the averaged repaint area must stay well under the full viewport. (The flush
	// area stays full-viewport — a real panel must re-send the scrolled pixels — so the
	// win is in replay/raster, measured here as repainted area, not flush pixels.)
	if (motionPerf.treePanReplayCalls > 0 && motionPerf.treePanRepaintArea / motionPerf.treePanReplayCalls >= 130000) {
		std::fprintf(stderr,
		             "[test_gea_sky_hop_jsx_main] pan fast path should cut repaint area, repaintArea/panFrame=%d viewport~191k\n",
		             motionPerf.treePanRepaintArea / motionPerf.treePanReplayCalls);
		dumpTree("test_gea_sky_hop_jsx_main");
		return 1;
	}
	dispatchTouch(gea::framework::events::TouchPhase::Up, false, 117, 457);
	refresh();

	// --- Pixel correctness ---------------------------------------------------
	// The horizontal-pan fast path (memcpy-shift + partial repaint) must produce
	// the byte-for-byte same framebuffer as the normal full dirty-region replay on
	// EVERY frame — not just a steady pan, but through the gameplay events that
	// stress it: walking, falling, enemy collisions, coin collection, losing a life,
	// the respawn camera teleport, and the hurt-blink that toggles the camera-locked
	// player's visibility. Run the identical deterministic input twice (fast path on,
	// then forced off) and compare a per-frame hash; any transient divergence (a
	// smeared backdrop, a stale sprite ghost, a dropped control) fails the test.
	{
		constexpr int kPanFrames = 520;
		auto frameHash = []() -> std::uint64_t {
			auto *c = gea::platform::display::Display::canvas();
			const std::size_t count = static_cast<std::size_t>(c->width()) * c->height();
			const std::uint16_t *px = c->pixels();
			std::uint64_t h = 1469598103934665603ULL;  // FNV-1a
			for (std::size_t i = 0; i < count; ++i) { h ^= px[i]; h *= 1099511628211ULL; }
			return h;
		};
		auto runPan = [&](bool panEnabled, std::vector<std::uint64_t> &hashes) {
			gea::embedded::ui::gHorizontalPanFastPathDisabled = !panEnabled;
			resetNativeHost();
			__gea_top_level();
			refresh();
			dispatchTouch(gea::framework::events::TouchPhase::Down, true, 117, 457);
			refresh();
			hashes.clear();
			for (int f = 0; f < kPanFrames; ++f) {
				pumpFrame(16 + f * 16);
				hashes.push_back(frameHash());
			}
			dispatchTouch(gea::framework::events::TouchPhase::Up, false, 117, 457);
			refresh();
			gea::embedded::ui::gHorizontalPanFastPathDisabled = false;
		};
		std::vector<std::uint64_t> fastHashes;
		std::vector<std::uint64_t> refHashes;
		runPan(true, fastHashes);
		runPan(false, refHashes);
		if (fastHashes.size() != refHashes.size()) {
			std::fprintf(stderr, "[test_gea_sky_hop_jsx_main] frame count mismatch %zu vs %zu\n",
			             fastHashes.size(), refHashes.size());
			return 1;
		}
		int firstDivergedFrame = -1;
		int divergedFrames = 0;
		for (std::size_t i = 0; i < fastHashes.size(); ++i) {
			if (fastHashes[i] != refHashes[i]) {
				divergedFrames++;
				if (firstDivergedFrame < 0) firstDivergedFrame = static_cast<int>(i);
			}
		}
		if (divergedFrames != 0) {
			std::fprintf(stderr,
			             "[test_gea_sky_hop_jsx_main] pan fast path diverges from full replay on %d/%d frames (first at frame %d)\n",
			             divergedFrames, static_cast<int>(fastHashes.size()), firstDivergedFrame);
			return 1;
		}
	}

	return 0;
}

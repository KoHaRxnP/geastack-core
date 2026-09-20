// Loop-boundary perf probe for the css-3d-cube app: pumps deterministic 16ms
// frames across two cube-spin iterations (10s loop) and prints every frame
// where the engine falls off its fast paths — a full display-list record
// instead of the transform reproject, a static-backdrop drop, or an outlier
// frame wall time. Diagnoses the on-device fps dip reported at the animation
// wrap. Not a pass/fail test; it always exits 0 so the pipeline shows the log.
#include "native_test_harness.h"
#include "ui/refresh_perf.h"
#include "ui/internal.h"
#include "ui/node.h"
#include "ui/tree_internal.h"

#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <vector>

extern void __gea_top_level();

// The default gea_backdrop_cache is a weak nullptr (no backdrop cache), which
// silently disables the static-backdrop bake — and with it the reproject fast
// path — so the host run wouldn't model the device's bake/drop lifecycle at
// the loop wrap. Provide a real full-screen buffer like the esp32 targets do.
extern "C" gea::framework::graphics::pixel::native_t *gea_backdrop_cache(int *cap_px)
{
	static std::vector<gea::framework::graphics::pixel::native_t> buffer(480 * 480);
	if (cap_px)
		*cap_px = static_cast<int>(buffer.size());
	return buffer.data();
}

int main()
{
	using namespace gea::embedded::test;
	using namespace gea::embedded::ui;

	resetNativeHost();
	__gea_top_level();
	refresh();
	StyleSheet::instance().startCssAnimations(0);
	pumpFrame(0);

	bool prevBackdrop = DisplayList::instance().staticBackdropActive();
	std::int64_t typWall = 0; // EMA of frame wall time, for outlier detection
	// 21ms steps ≈ the device's ~45fps cadence: near the eased keyframe pauses the
	// per-frame transform delta quantizes to zero, which fixed 16ms host steps can
	// land differently — match the device to reproduce its quiet frames.
	const double stepMs = std::getenv("GEA_PROBE_STEP_MS") ? std::atof(std::getenv("GEA_PROBE_STEP_MS")) : 16.0;
	// Deterministic frame-time jitter (LCG) — on the device the measured fps
	// varies tick to tick, so the FpsBadge text CHANGES every second and its
	// setText dirties a node at arbitrary animation phases (including the eased
	// keyframe pauses). A fixed step makes the fps text constant and hides that.
	const bool jitter = std::getenv("GEA_PROBE_JITTER") != nullptr;
	std::uint32_t lcg = 12345;
	const int frames = static_cast<int>(22000.0 / stepMs); // 22s — covers two 10s wraps
	double t = 0.0;
	for (int f = 1; f <= frames; f++)
	{
		double step = stepMs;
		if (jitter)
		{
			lcg = lcg * 1664525u + 1013904223u;
			step = stepMs - 4.0 + static_cast<double>(lcg % 9000u) / 1000.0; // ±4ms around stepMs
		}
		t += step;
		// Force a badge-style text tick at a chosen time — models the device's fps
		// badge changing DURING a keyframe pause (frames go cheap there, measured fps
		// rises, the text changes). Pre-fix this dropped the live backdrop cache and
		// the resume re-bake + full-viewport sync was the loop-wrap stall.
		static const double forceTickAtMs =
				std::getenv("GEA_PROBE_FORCE_TICK_MS") ? std::atof(std::getenv("GEA_PROBE_FORCE_TICK_MS")) : -1.0;
		static bool forcedTick = false;
		if (forceTickAtMs > 0 && !forcedTick && t >= forceTickAtMs)
		{
			forcedTick = true;
			Tree::instance().setText(2, "FPS 99"); // node 2 = the fps badge span
			std::printf("[force-tick] t=%6.0fms setText on badge\n", t);
		}
		refreshPerfStatsReset();
		const std::int64_t t0 = refreshPerfNowUs();
		pumpFrame(t);
		const std::int64_t wall = refreshPerfNowUs() - t0;
		const auto p = refreshPerfStatsRead();
		const bool backdrop = DisplayList::instance().staticBackdropActive();
		const bool outlier = typWall > 0 && wall > typWall * 2 && wall > 2000;
		// Quiet frame: no display-list pass ran at all this pump (nothing dirty —
		// the eased animations hit a quantization standstill). These are the frames
		// whose phase the badge-tick drop theory cares about.
		if (std::getenv("GEA_PROBE_QUIET") && p.treeDisplayListUs == 0 && p.treeReplayUs == 0)
			std::printf("[quiet] t=%6.0fms\n", t);
		if (p.treeFullRecords > 0 || backdrop != prevBackdrop || outlier)
			std::printf(
					"[probe] t=%6.0fms wall=%6lldus full=%d reproj=%d records=%d cmds=%d direct=%d recordUs=%lld replayUs=%lld backdrop=%d->%d\n",
					t,
					static_cast<long long>(wall),
					p.treeFullRecords,
					p.treeReprojects,
					p.treeRecordCalls,
					p.treeRecordedCommands,
					p.treeDirectReplayCalls,
					static_cast<long long>(p.treeRecordNodeUs),
					static_cast<long long>(p.treeReplayUs),
					prevBackdrop ? 1 : 0,
					backdrop ? 1 : 0);
		prevBackdrop = backdrop;
		typWall = typWall == 0 ? wall : (typWall * 7 + wall) / 8;
	}
	return 0;
}

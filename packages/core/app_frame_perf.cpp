// SPDX-License-Identifier: Apache-2.0
#include "app.h"

#include <atomic>

namespace gea::framework::app {

namespace {

ApplicationFramePerfStats gFramePerfStats;
std::atomic<int> gFramePhase{static_cast<int>(ApplicationFramePhase::Idle)};

}  // namespace

void applicationFramePhaseSet(ApplicationFramePhase phase)
{
	gFramePhase.store(static_cast<int>(phase), std::memory_order_relaxed);
}

ApplicationFramePhase applicationFramePhaseRead()
{
	return static_cast<ApplicationFramePhase>(gFramePhase.load(std::memory_order_relaxed));
}

const char *applicationFramePhaseName(ApplicationFramePhase phase)
{
	switch (phase) {
		case ApplicationFramePhase::DrainMicrotasks: return "drain_microtasks";
		case ApplicationFramePhase::AnimationFrameCallbacks: return "animation_frame_callbacks";
		case ApplicationFramePhase::StyleRecompute: return "style_recompute";
		case ApplicationFramePhase::DocumentFrame: return "document_frame";
		case ApplicationFramePhase::RefreshMounted: return "refresh_mounted";
		case ApplicationFramePhase::Idle: return "idle";
	}
	return "idle";
}

void applicationFramePerfStatsReset()
{
	gFramePerfStats = {};
}

void applicationFramePerfStatsAdd(ApplicationFramePerfStats &stats, ApplicationFramePhase phase,
                                  std::int64_t durationUs)
{
	switch (phase) {
		case ApplicationFramePhase::DrainMicrotasks:
			stats.drainMicrotasksUs += durationUs;
			break;
		case ApplicationFramePhase::AnimationFrameCallbacks:
			stats.animationFrameUs += durationUs;
			break;
		case ApplicationFramePhase::StyleRecompute:
			stats.styleRecomputeUs += durationUs;
			break;
		case ApplicationFramePhase::DocumentFrame:
			stats.documentFrameUs += durationUs;
			break;
		case ApplicationFramePhase::RefreshMounted:
			stats.refreshMountedUs += durationUs;
			break;
		case ApplicationFramePhase::Idle:
			break;
	}
}

void applicationFramePerfStatsAdd(ApplicationFramePhase phase, std::int64_t durationUs)
{
	applicationFramePerfStatsAdd(gFramePerfStats, phase, durationUs);
}

ApplicationFramePerfStats applicationFramePerfStatsRead()
{
	return gFramePerfStats;
}

}  // namespace gea::framework::app

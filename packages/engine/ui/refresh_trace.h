// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <atomic>

namespace gea::embedded::ui {

enum class RefreshTraceStage : int {
	Idle,
	Begin,
	RootScroll,
	LayoutMode,
	Layout,
	DirectPrepare,
	DisplayListRebuild,
	DirtyCollect,
	DirtyCoalesce,
	RectReplay,
	RectFlush,
	Snapshot,
	CanvasReset,
};

inline std::atomic<int> gRefreshTraceStage{static_cast<int>(RefreshTraceStage::Idle)};
inline std::atomic<int> gRefreshTraceIndex{0};

inline void refreshTraceSet(RefreshTraceStage stage, int index = 0)
{
	gRefreshTraceIndex.store(index, std::memory_order_relaxed);
	gRefreshTraceStage.store(static_cast<int>(stage), std::memory_order_release);
}

inline RefreshTraceStage refreshTraceStage()
{
	return static_cast<RefreshTraceStage>(gRefreshTraceStage.load(std::memory_order_acquire));
}

inline int refreshTraceIndex()
{
	return gRefreshTraceIndex.load(std::memory_order_relaxed);
}

inline const char *refreshTraceStageName(RefreshTraceStage stage)
{
	switch (stage) {
		case RefreshTraceStage::Begin: return "begin";
		case RefreshTraceStage::RootScroll: return "root_scroll";
		case RefreshTraceStage::LayoutMode: return "layout_mode";
		case RefreshTraceStage::Layout: return "layout";
		case RefreshTraceStage::DirectPrepare: return "direct_prepare";
		case RefreshTraceStage::DisplayListRebuild: return "display_list_rebuild";
		case RefreshTraceStage::DirtyCollect: return "dirty_collect";
		case RefreshTraceStage::DirtyCoalesce: return "dirty_coalesce";
		case RefreshTraceStage::RectReplay: return "rect_replay";
		case RefreshTraceStage::RectFlush: return "rect_flush";
		case RefreshTraceStage::Snapshot: return "snapshot";
		case RefreshTraceStage::CanvasReset: return "canvas_reset";
		case RefreshTraceStage::Idle: return "idle";
	}
	return "idle";
}

inline const char *refreshTraceStageName()
{
	return refreshTraceStageName(refreshTraceStage());
}

}  // namespace gea::embedded::ui

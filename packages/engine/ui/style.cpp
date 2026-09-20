// SPDX-License-Identifier: Apache-2.0
#include "style.h"
#include "state_init.h"

#include "css_atom.h"
#include "graphics/font.h"
#include "node.h"
#include "pixel.h"
#include "style_values.h"
#include "tree_state.h"
#include "tree_internal.h"
#include "css/engine.h"
#include "refresh_perf.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <initializer_list>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#ifdef ESP_PLATFORM
#include "esp_timer.h"
#include "esp_log.h"
#define GEA_STYLE_LOGW(tag, fmt, ...) ESP_LOGW(tag, fmt, ##__VA_ARGS__)
#else
#define GEA_STYLE_LOGW(tag, fmt, ...) std::fprintf(stderr, "[%s] " fmt "\n", tag, ##__VA_ARGS__)
#endif

// Per-recompute-batch profiling that decomposes a recompute into per-node/per-parser
// sub-phases (dumped by endStyleMountBatch when a batch exceeds 50ms). The timers sit
// on hot paths (parseLengthForNode, applyPropertyWithSource, ...) so it's off by
// default; flip to 1 (ESP only — it uses esp_timer) to investigate recompute cost.
#ifndef GEA_RECPROF
#define GEA_RECPROF 0
#endif

// When 1, endStyleMountBatch runs the incremental recompute, then re-runs the full
// recompute and asserts every node's computed style is byte-identical — proving the
// incremental result on-device (where visual inspection over serial isn't possible).
// Costs the full recompute too, so it's a verification build, not a perf build.
#ifndef GEA_INCREMENTAL_VERIFY
#define GEA_INCREMENTAL_VERIFY 0
#endif

#ifndef GEA_CSS_CACHED_STYLE_APPLY_INLINE_OPS
#define GEA_CSS_CACHED_STYLE_APPLY_INLINE_OPS 64
#endif

#ifndef GEA_CSS_ACTIVE_RULE_PLAN_INLINE_CACHE_ENTRIES
#define GEA_CSS_ACTIVE_RULE_PLAN_INLINE_CACHE_ENTRIES 8
#endif

#ifndef GEA_CSS_RULE_CANDIDATE_INLINE_RULES
#define GEA_CSS_RULE_CANDIDATE_INLINE_RULES 32
#endif

#ifndef GEA_CSS_RULE_CANDIDATE_INLINE_CACHE_ENTRIES
#define GEA_CSS_RULE_CANDIDATE_INLINE_CACHE_ENTRIES 16
#endif

namespace gea::embedded::ui {

namespace {

#if GEA_RECPROF
// TEMP per-recompute-batch profiling: decompose a city/theme switch's ~400ms
// recompute into its per-node sub-phases. Reset + dumped by endStyleMountBatch.
int64_t g_profResetUs = 0, g_profCandUs = 0, g_profApplyUs = 0, g_profVarUs = 0,
        g_profMiscUs = 0, g_profPseudoUs = 0, g_profBodyUs = 0, g_profBgUs = 0, g_profXformUs = 0,
        g_profLenUs = 0, g_profColorUs = 0;
int g_profNodes = 0, g_profApplyCalls = 0, g_profBgCalls = 0, g_profXformCalls = 0,
    g_profLenCalls = 0, g_profColorCalls = 0;
inline int64_t recNow() { return gea::embedded::ui::refreshPerfNowUs(); }
// Last dump line, queryable after the fact (the mount-batch dump fires before a
// USB console can attach on embedded targets). See gea_recprof_last().
char g_recprofLast[420];
#endif

// --- Incremental class-style recompute: dependency tracking ---------------
// During a node's class-style recompute we record every custom-property name it
// resolves (via lookupCustomProperty, which is reached for var() in any property,
// including the node's pseudo-elements and transitively through var()-valued
// custom properties). On a later recompute triggered by a near-root class change
// (e.g. a weather theme switch flipping is-cloud->is-rain, which only mutates the
// shell's --base-*/--accent custom props), endStyleMountBatch walks the subtree and
// recomputes a node only if it was directly mutated, inherits a changed value, or
// references a custom property whose value changed. Nodes that depend on none of
// those keep their existing computed style (identical to a full recompute). A
// GEA_INCREMENTAL_VERIFY build additionally runs the full recompute afterwards and
// asserts every node's style matches, proving equivalence on-device.
std::vector<CssAtomId> g_nodeRefOverflow;

struct CssAtomSmallList {
	static constexpr std::uint8_t kInlineCount = 16;
	CssAtomId inlineValues[kInlineCount]{};
	std::vector<CssAtomId> spillValues;
	std::uint8_t inlineCount = 0;
	bool spilled = false;

	std::size_t size() const { return spilled ? spillValues.size() : inlineCount; }
	bool empty() const { return size() == 0; }

	CssAtomId at(std::size_t index) const
	{
		return spilled ? spillValues[index] : inlineValues[index];
	}

	bool contains(CssAtomId value) const
	{
		for (std::size_t i = 0, n = size(); i < n; ++i)
			if (at(i) == value) return true;
		return false;
	}

	void insertUnique(CssAtomId value)
	{
		if (value == kInvalidCssAtom || contains(value)) return;
		if (!spilled && inlineCount < kInlineCount) {
			inlineValues[inlineCount++] = value;
			return;
		}
		if (!spilled) {
			spillValues.assign(inlineValues, inlineValues + inlineCount);
			spilled = true;
		}
		spillValues.push_back(value);
	}

	void erase(CssAtomId value)
	{
		if (spilled) {
			for (auto it = spillValues.begin(); it != spillValues.end(); ++it)
				if (*it == value) {
					spillValues.erase(it);
					return;
				}
			return;
		}
		for (std::uint8_t i = 0; i < inlineCount; ++i) {
			if (inlineValues[i] != value) continue;
			for (std::uint8_t j = i + 1; j < inlineCount; ++j)
				inlineValues[j - 1] = inlineValues[j];
			--inlineCount;
			return;
		}
	}
};

struct NodeCustomPropRefs {
	static constexpr std::uint8_t kInlineCount = 4;
	static constexpr std::uint16_t kNoOverflow = 0xFFFFu;
	CssAtomId inlineRefs[kInlineCount]{};
	std::uint16_t overflowStart = kNoOverflow;
	std::uint16_t overflowCount = 0;
	std::uint8_t count = 0;
	bool tracked = false;

#if GEA_EMBEDDED_UI_STATE_DYNAMIC_INIT
	// Runtime initialization keeps the overflow sentinel out of .data (see
	// state_init.h).
	NodeCustomPropRefs() { reset(); }
#endif

	void clearForRecompute()
	{
		tracked = true;
		count = 0;
		overflowStart = kNoOverflow;
		overflowCount = 0;
	}

	void reset()
	{
		tracked = false;
		count = 0;
		overflowStart = kNoOverflow;
		overflowCount = 0;
	}

	bool contains(CssAtomId name) const
	{
		for (std::uint8_t i = 0; i < count; ++i)
			if (inlineRefs[i] == name) return true;
		if (overflowStart != kNoOverflow) {
			const std::size_t start = overflowStart;
			const std::size_t end = start + overflowCount;
			for (std::size_t i = start; i < end && i < g_nodeRefOverflow.size(); ++i)
				if (g_nodeRefOverflow[i] == name) return true;
		}
		return false;
	}

	void insert(CssAtomId name)
	{
		if (name == kInvalidCssAtom || contains(name)) return;
		if (count < kInlineCount) {
			inlineRefs[count++] = name;
			return;
		}
		if (overflowStart == kNoOverflow) {
			const std::size_t start = g_nodeRefOverflow.size();
			if (start >= kNoOverflow) return;
			overflowStart = static_cast<std::uint16_t>(start);
			overflowCount = 0;
		}
		g_nodeRefOverflow.push_back(name);
		++overflowCount;
	}

	bool touches(const CssAtomSmallList &changed) const
	{
		if (!tracked || changed.empty()) return false;
		for (std::uint8_t i = 0; i < count; ++i)
			for (std::size_t c = 0, n = changed.size(); c < n; ++c) {
				const CssAtomId changedName = changed.at(c);
				if (changedName == inlineRefs[i]) return true;
			}
		if (overflowStart != kNoOverflow) {
			const std::size_t start = overflowStart;
			const std::size_t end = start + overflowCount;
			for (std::size_t i = start; i < end && i < g_nodeRefOverflow.size(); ++i)
				for (std::size_t c = 0, n = changed.size(); c < n; ++c) {
					const CssAtomId changedName = changed.at(c);
					if (changedName == g_nodeRefOverflow[i]) return true;
				}
		}
		return false;
	}

	std::string debugString() const
	{
		std::string out;
		for (std::uint8_t i = 0; i < count; ++i) {
			out += cssAtomText(inlineRefs[i]);
			out.push_back(' ');
		}
		if (overflowStart != kNoOverflow) {
			const std::size_t start = overflowStart;
			const std::size_t end = start + overflowCount;
			for (std::size_t i = start; i < end && i < g_nodeRefOverflow.size(); ++i) {
				out += cssAtomText(g_nodeRefOverflow[i]);
				out.push_back(' ');
			}
		}
		return out;
	}
};

NodeCustomPropRefs g_nodeRefs[kMaxNodes];
int g_recordingNode = -1;
struct CustomPropertyLookupCache {
	static constexpr std::uint8_t kCapacity = 16;
	int nodeIds[kCapacity]{};
	CssAtomId names[kCapacity]{};
	const NodeCustomProperty *entries[kCapacity]{};
	std::uint8_t hits[kCapacity]{};
	std::uint8_t count = 0;

	void clear()
	{
		count = 0;
	}

	bool lookup(int nodeId, CssAtomId name, const NodeCustomProperty *&out) const
	{
		for (std::uint8_t i = 0; i < count; ++i) {
			if (nodeIds[i] != nodeId || names[i] != name) continue;
			out = hits[i] ? entries[i] : nullptr;
			return true;
		}
		return false;
	}

	void store(int nodeId, CssAtomId name, const NodeCustomProperty *entry)
	{
		if (name == kInvalidCssAtom) return;
		for (std::uint8_t i = 0; i < count; ++i) {
			if (nodeIds[i] != nodeId || names[i] != name) continue;
			entries[i] = entry;
			hits[i] = entry ? 1 : 0;
			return;
		}
		if (count >= kCapacity) return;
		nodeIds[count] = nodeId;
		names[count] = name;
		entries[count] = entry;
		hits[count] = entry ? 1 : 0;
		++count;
	}
};

CustomPropertyLookupCache g_customPropertyLookupCache;

void clearDynamicLengthExpressionResolutionCache();

inline void clearCustomPropertyLookupCache()
{
	g_customPropertyLookupCache.clear();
	clearDynamicLengthExpressionResolutionCache();
}

struct DenseNodeMark {
#if GEA_EMBEDDED_UI_STATE_DYNAMIC_INIT
	std::uint16_t marks[kMaxNodes];
	std::uint16_t serial;

	// Runtime initialization keeps the nonzero serial out of .data (see
	// state_init.h); noinline stops the compiler folding it back in.
	__attribute__((noinline)) DenseNodeMark()
	{
		for (std::uint16_t &mark : marks) mark = 0;
		serial = 1;
	}
#else
	std::uint16_t marks[kMaxNodes]{};
	std::uint16_t serial = 1;
#endif

	void clear()
	{
		++serial;
		if (serial == 0) {
			for (std::uint16_t &mark : marks) mark = 0;
			serial = 1;
		}
	}

	void insert(int node)
	{
		if (node < 0 || node >= kMaxNodes) return;
		marks[node] = serial;
	}

	bool contains(int node) const
	{
		return node >= 0 && node < kMaxNodes && marks[node] == serial;
	}
};

DenseNodeMark g_pendingRecomputeMark;
// Nodes whose class change touched a class/tag used as an ancestor matcher in some
// complex selector (e.g. toggling `.open` where `.open .panel {}` exists) — captured
// at setClassName time so both the OLD and NEW class sets are visible. Their entire
// subtree must be recomputed in full (descendant selector matches may have flipped),
// which a custom-property/inheritance diff alone wouldn't catch. Consumed + cleared
// by endStyleMountBatch.
DenseNodeMark g_forceFullSubtreeMark;
#if GEA_INCREMENTAL_VERIFY
std::unordered_set<int> g_incrRecomputedNodes;  // nodes the incremental pass actually recomputed
#endif

inline void recordCustomPropRef(CssAtomId name)
{
	if (g_recordingNode < 0 || g_recordingNode >= kMaxNodes || name == kInvalidCssAtom) return;
	g_nodeRefs[g_recordingNode].insert(name);
}

bool nodeParticipatesInMountedTree(const TreeState &state, int node)
{
	const int root = Tree::instance().mountedRoot();
	if (root < 0) return false;
	for (int current = node; current >= 0 && current < state.nodeCount; current = state.nodes[current].parent) {
		if (current == root) return true;
	}
	return false;
}

bool styleEqualExceptTextPaint(const ComputedStyle &a, const ComputedStyle &b)
{
	const RareStyle &ar = rstyle(a);
	const RareStyle &br = rstyle(b);
	if (a.display != b.display ||
	    a.flex_direction != b.flex_direction ||
	    a.flex_direction_explicit != b.flex_direction_explicit ||
	    a.display_explicit != b.display_explicit ||
	    a.flex_wrap != b.flex_wrap ||
	    a.justify_content != b.justify_content ||
	    a.align_items != b.align_items ||
	    a.justify_items != b.justify_items ||
	    a.align_content != b.align_content ||
	    a.align_self != b.align_self ||
	    a.gap != b.gap ||
	    ar.grid_column_count != br.grid_column_count ||
	    ar.grid_row_count != br.grid_row_count) return false;
	for (int i = 0; i < kMaxGridTracks; ++i) {
		if (ar.grid_column_type[i] != br.grid_column_type[i] ||
		    ar.grid_row_type[i] != br.grid_row_type[i] ||
		    ar.grid_column_value[i] != br.grid_column_value[i] ||
		    ar.grid_row_value[i] != br.grid_row_value[i]) return false;
	}
	if (a.width != b.width ||
	    a.height != b.height ||
	    a.width_percent != b.width_percent ||
	    a.height_percent != b.height_percent ||
	    a.min_width != b.min_width ||
	    a.min_height != b.min_height ||
	    a.max_width != b.max_width ||
	    a.max_height != b.max_height ||
	    a.flex != b.flex ||
	    a.flex_shrink != b.flex_shrink ||
	    a.flex_basis != b.flex_basis) return false;
	for (int i = 0; i < 4; ++i) {
		if (a.padding[i] != b.padding[i] ||
		    a.margin[i] != b.margin[i] ||
		    a.pos_offsets[i] != b.pos_offsets[i] ||
		    a.pos_offset_percent[i] != b.pos_offset_percent[i] ||
		    ar.border_side_width[i] != br.border_side_width[i] ||
		    ar.border_side_color[i] != br.border_side_color[i] ||
		    ar.border_side_alpha[i] != br.border_side_alpha[i] ||
		    a.border_radius[i] != b.border_radius[i] ||
		    a.border_radius_percent[i] != b.border_radius_percent[i]) return false;
	}
	if (a.position != b.position ||
	    a.z_index != b.z_index ||
	    a.bg_color != b.bg_color ||
	    a.has_bg != b.has_bg ||
	    a.bg_alpha != b.bg_alpha ||
	    a.bg_fill != b.bg_fill ||
	    ar.bg_gradient_from_color != br.bg_gradient_from_color ||
	    ar.bg_gradient_mid_color != br.bg_gradient_mid_color ||
	    ar.bg_gradient_to_color != br.bg_gradient_to_color ||
	    ar.bg_gradient_from_alpha != br.bg_gradient_from_alpha ||
	    ar.bg_gradient_mid_alpha != br.bg_gradient_mid_alpha ||
	    ar.bg_gradient_to_alpha != br.bg_gradient_to_alpha ||
	    ar.bg_gradient_mid_stop != br.bg_gradient_mid_stop ||
	    ar.bg_gradient_to_stop != br.bg_gradient_to_stop ||
	    ar.bg_gradient_has_mid != br.bg_gradient_has_mid ||
	    ar.bg_gradient_angle != br.bg_gradient_angle ||
	    ar.bg_overlay_gradient != br.bg_overlay_gradient ||
	    ar.bg_overlay_gradient_from_color != br.bg_overlay_gradient_from_color ||
	    ar.bg_overlay_gradient_mid_color != br.bg_overlay_gradient_mid_color ||
	    ar.bg_overlay_gradient_to_color != br.bg_overlay_gradient_to_color ||
	    ar.bg_overlay_gradient_from_alpha != br.bg_overlay_gradient_from_alpha ||
	    ar.bg_overlay_gradient_mid_alpha != br.bg_overlay_gradient_mid_alpha ||
	    ar.bg_overlay_gradient_to_alpha != br.bg_overlay_gradient_to_alpha ||
	    ar.bg_overlay_gradient_mid_stop != br.bg_overlay_gradient_mid_stop ||
	    ar.bg_overlay_gradient_to_stop != br.bg_overlay_gradient_to_stop ||
	    ar.bg_overlay_gradient_has_mid != br.bg_overlay_gradient_has_mid ||
	    ar.bg_overlay_gradient_angle != br.bg_overlay_gradient_angle ||
	    ar.bg_radial_gradient != br.bg_radial_gradient ||
	    ar.bg_radial_gradient_from_color != br.bg_radial_gradient_from_color ||
	    ar.bg_radial_gradient_to_color != br.bg_radial_gradient_to_color ||
	    ar.bg_radial_gradient_from_alpha != br.bg_radial_gradient_from_alpha ||
	    ar.bg_radial_gradient_to_alpha != br.bg_radial_gradient_to_alpha ||
	    ar.bg_radial_gradient_stop != br.bg_radial_gradient_stop ||
	    ar.bg_radial_gradient_cx != br.bg_radial_gradient_cx ||
	    ar.bg_radial_gradient_cy != br.bg_radial_gradient_cy ||
	    ar.bg_radial_gradient_rx != br.bg_radial_gradient_rx ||
	    ar.bg_radial_gradient_ry != br.bg_radial_gradient_ry ||
	    ar.bg_grid_axes != br.bg_grid_axes ||
	    ar.bg_grid_color != br.bg_grid_color ||
	    ar.bg_grid_alpha != br.bg_grid_alpha ||
	    ar.bg_grid_step_x != br.bg_grid_step_x ||
	    ar.bg_grid_step_y != br.bg_grid_step_y ||
	    ar.bg_grid_line_x != br.bg_grid_line_x ||
	    ar.bg_grid_line_y != br.bg_grid_line_y ||
	    a.active_bg_color != b.active_bg_color ||
	    a.has_active_bg != b.has_active_bg ||
	    a.opacity != b.opacity ||
	    a.blink_interval_ms != b.blink_interval_ms ||
	    a.blink_started_ms != b.blink_started_ms ||
	    a.blink_visible != b.blink_visible ||
	    a.border_width != b.border_width ||
	    a.border_color != b.border_color ||
	    a.border_alpha != b.border_alpha ||
	    ar.transform_rotate != br.transform_rotate ||
	    ar.transform_rotate_x != br.transform_rotate_x ||
	    ar.transform_rotate_y != br.transform_rotate_y ||
	    ar.transform_translate_x != br.transform_translate_x ||
	    ar.transform_translate_y != br.transform_translate_y ||
	    ar.transform_translate_z != br.transform_translate_z ||
	    ar.transform_translate_x_percent != br.transform_translate_x_percent ||
	    ar.transform_translate_y_percent != br.transform_translate_y_percent ||
	    ar.transform_scale_x != br.transform_scale_x ||
	    ar.transform_scale_y != br.transform_scale_y ||
	    ar.transform_origin_x != br.transform_origin_x ||
	    ar.transform_origin_y != br.transform_origin_y ||
	    ar.perspective != br.perspective ||
	    ar.perspective_origin_x != br.perspective_origin_x ||
	    ar.perspective_origin_y != br.perspective_origin_y ||
	    ar.filter_blur_radius != br.filter_blur_radius ||
	    ar.box_shadow_inset != br.box_shadow_inset ||
	    ar.box_shadow_offset_x != br.box_shadow_offset_x ||
	    ar.box_shadow_offset_y != br.box_shadow_offset_y ||
	    ar.box_shadow_blur_radius != br.box_shadow_blur_radius ||
	    ar.box_shadow_spread != br.box_shadow_spread ||
	    ar.box_shadow_color != br.box_shadow_color ||
	    ar.box_shadow_alpha != br.box_shadow_alpha ||
	    a.font_id != b.font_id ||
	    a.font_size != b.font_size ||
	    a.font_weight != b.font_weight ||
	    a.line_height != b.line_height ||
	    a.text_align != b.text_align ||
	    a.overflow != b.overflow ||
	    a.overflow_x != b.overflow_x ||
	    a.overflow_y != b.overflow_y ||
	    a.mask_right_fade_width != b.mask_right_fade_width ||
	    a.image_fit != b.image_fit ||
	    a.backface_hidden != b.backface_hidden ||
	    a.text_decoration != b.text_decoration ||
	    a.text_transform != b.text_transform ||
	    a.white_space != b.white_space ||
	    a.text_overflow != b.text_overflow) return false;
	return true;
}

bool styleEqualExceptLocalDisplayCommands(const ComputedStyle &a, const ComputedStyle &b)
{
	const RareStyle &ar = rstyle(a);
	const RareStyle &br = rstyle(b);
	if (a.display != b.display ||
	    a.flex_direction != b.flex_direction ||
	    a.flex_direction_explicit != b.flex_direction_explicit ||
	    a.display_explicit != b.display_explicit ||
	    a.flex_wrap != b.flex_wrap ||
	    a.justify_content != b.justify_content ||
	    a.align_items != b.align_items ||
	    a.justify_items != b.justify_items ||
	    a.align_content != b.align_content ||
	    a.align_self != b.align_self ||
	    a.gap != b.gap ||
	    ar.grid_column_count != br.grid_column_count ||
	    ar.grid_row_count != br.grid_row_count) return false;
	for (int i = 0; i < kMaxGridTracks; ++i) {
		if (ar.grid_column_type[i] != br.grid_column_type[i] ||
		    ar.grid_row_type[i] != br.grid_row_type[i] ||
		    ar.grid_column_value[i] != br.grid_column_value[i] ||
		    ar.grid_row_value[i] != br.grid_row_value[i]) return false;
	}
	if (a.width != b.width ||
	    a.height != b.height ||
	    a.width_percent != b.width_percent ||
	    a.height_percent != b.height_percent ||
	    a.min_width != b.min_width ||
	    a.min_height != b.min_height ||
	    a.max_width != b.max_width ||
	    a.max_height != b.max_height ||
	    a.flex != b.flex ||
	    a.flex_shrink != b.flex_shrink ||
	    a.flex_basis != b.flex_basis) return false;
	for (int i = 0; i < 4; ++i) {
		if (a.padding[i] != b.padding[i] ||
		    a.margin[i] != b.margin[i] ||
		    a.pos_offsets[i] != b.pos_offsets[i] ||
		    a.pos_offset_percent[i] != b.pos_offset_percent[i]) return false;
	}
	if (a.position != b.position ||
	    a.z_index != b.z_index ||
	    a.opacity != b.opacity ||
	    a.blink_interval_ms != b.blink_interval_ms ||
	    a.blink_started_ms != b.blink_started_ms ||
	    a.blink_visible != b.blink_visible ||
	    ar.filter_blur_radius != br.filter_blur_radius ||
	    ar.box_shadow_inset != br.box_shadow_inset ||
	    ar.box_shadow_offset_x != br.box_shadow_offset_x ||
	    ar.box_shadow_offset_y != br.box_shadow_offset_y ||
	    ar.box_shadow_blur_radius != br.box_shadow_blur_radius ||
	    ar.box_shadow_spread != br.box_shadow_spread ||
	    ar.box_shadow_color != br.box_shadow_color ||
	    ar.box_shadow_alpha != br.box_shadow_alpha ||
	    a.font_id != b.font_id ||
	    a.font_size != b.font_size ||
	    a.font_weight != b.font_weight ||
	    a.line_height != b.line_height ||
	    a.text_align != b.text_align ||
	    a.overflow != b.overflow ||
	    a.overflow_x != b.overflow_x ||
	    a.overflow_y != b.overflow_y ||
	    a.mask_right_fade_width != b.mask_right_fade_width ||
	    a.image_fit != b.image_fit ||
	    a.text_decoration != b.text_decoration ||
	    a.text_transform != b.text_transform ||
	    a.white_space != b.white_space ||
	    a.text_overflow != b.text_overflow) return false;
	return true;
}

bool styleExactlyEqual(const ComputedStyle &a, const ComputedStyle &b)
{
	return a.text_color == b.text_color &&
	       a.text_alpha == b.text_alpha &&
	       styleEqualExceptTextPaint(a, b);
}

bool isPlainOpaqueBackground(const ComputedStyle &style)
{
	const RareStyle &rs = rstyle(style);
	return style.has_bg &&
	       style.bg_alpha == 255 &&
	       style.bg_fill == 0 &&
	       rs.bg_gradient_has_mid == 0 &&
	       rs.bg_overlay_gradient == 0 &&
	       rs.bg_radial_gradient == 0 &&
	       rs.bg_grid_axes == 0;
}

bool styleEqualExceptPlainBackgroundColor(const ComputedStyle &a, const ComputedStyle &b)
{
	if (a.bg_color == b.bg_color) return false;
	if (!isPlainOpaqueBackground(a) || !isPlainOpaqueBackground(b)) return false;
	ComputedStyle patched = b;
	patched.bg_color = a.bg_color;
	return styleExactlyEqual(a, patched);
}

void markClassRecomputeStyleDiff(int node, const ComputedStyle &beforeStyle, int beforeImageId)
{
	auto &state = treeState();
	if (node < 0 || node >= state.nodeCount) return;
	Node &target = state.nodes[node];
	if (beforeImageId == target.image_id && styleExactlyEqual(beforeStyle, target.style)) return;
	if (!nodeParticipatesInMountedTree(state, node)) return;

	if (beforeImageId == target.image_id && styleEqualExceptTextPaint(beforeStyle, target.style)) {
		if (target.type == NodeType::Text) {
			target.render.dirty = 1;  // paint-only: text color; geometry untouched
			target.render.non_scroll_dirty = 1;
			Tree::instance().markNodeDisplayCommandsDirty(node);
		}
		return;
	}

	if (beforeImageId == target.image_id && styleEqualExceptPlainBackgroundColor(beforeStyle, target.style)) {
		// Preserve `bg_recolor_from` across coalesced changes. The in-place recolor
		// rewrites framebuffer pixels equal to `from`. If several background-color
		// changes land between renders (e.g. fast rotary spins A->B->C), overwriting
		// `from` each time leaves it at an intermediate color (B/C) while the framebuffer
		// is still the original (A) from the last render — so almost nothing matches and
		// the background only partially recolors, leaving a patchwork that never
		// converges. Capture `from` only on the first change since the last render (when
		// the framebuffer still matches the current style), and just advance `to`.
		if (!target.render.bg_recolor_pending) {
			target.render.bg_recolor_pending = 1;
			target.render.bg_recolor_from = beforeStyle.bg_color;
		}
		target.render.bg_recolor_to = target.style.bg_color;
		target.render.dirty = 1;  // paint-only: background recolor; geometry untouched
		target.render.non_scroll_dirty = 1;
		Tree::instance().markNodeDisplayCommandsDirty(node);
		return;
	}

	target.render.dirty = 1;
	target.render.non_scroll_dirty = 1;
	if (beforeImageId == target.image_id && styleEqualExceptLocalDisplayCommands(beforeStyle, target.style)) {
		// Local display-command diff: geometry equal by the predicate's
		// construction (transforms ride transform_dirty), so no layout pass.
		const RareStyle &beforeRare = rstyle(beforeStyle);
		const RareStyle &targetRare = rstyle(target.style);
		if (beforeRare.transform_rotate != targetRare.transform_rotate ||
		    beforeRare.transform_rotate_x != targetRare.transform_rotate_x ||
		    beforeRare.transform_rotate_y != targetRare.transform_rotate_y ||
		    beforeRare.transform_translate_x != targetRare.transform_translate_x ||
		    beforeRare.transform_translate_y != targetRare.transform_translate_y ||
		    beforeRare.transform_translate_z != targetRare.transform_translate_z ||
		    beforeRare.transform_translate_x_percent != targetRare.transform_translate_x_percent ||
		    beforeRare.transform_translate_y_percent != targetRare.transform_translate_y_percent ||
		    beforeRare.transform_scale_x != targetRare.transform_scale_x ||
		    beforeRare.transform_scale_y != targetRare.transform_scale_y ||
		    beforeRare.transform_origin_x != targetRare.transform_origin_x ||
		    beforeRare.transform_origin_y != targetRare.transform_origin_y ||
		    beforeRare.perspective != targetRare.perspective ||
		    beforeRare.perspective_origin_x != targetRare.perspective_origin_x ||
		    beforeRare.perspective_origin_y != targetRare.perspective_origin_y) {
			target.render.transform_dirty = 1;
			state.transformScanSerial = ~0ull;
			state.transformScanValid = false;  // a transform was added/changed → drop durable no-transform cache
		}
		Tree::instance().markNodeDisplayCommandsDirty(node);
		// CSS flattening: descendant text paints into this node's plane, and its
		// recorded DrawProjectedText carries the ancestor-OR of
		// backface-visibility captured at record time (TextRenderer's flattening
		// walk). The node-local re-record above refreshes only THIS node's
		// commands, so a backface toggle here would leave descendants' retained
		// projected-text commands with the stale flag — replay keeps culling a
		// label whose face just became double-sided (or vice versa). Mark
		// descendant text nodes for their own in-place re-record.
		if (beforeStyle.backface_hidden != target.style.backface_hidden &&
		    target.first_child >= 0) {
			for (int i = 0; i < state.nodeCount; i++) {
				if (state.nodes[i].type != NodeType::Text) continue;
				bool underToggledNode = false;
				for (int a = state.nodes[i].parent; a >= 0 && a < state.nodeCount;
				     a = state.nodes[a].parent) {
					if (a == node) { underToggledNode = true; break; }
				}
				if (!underToggledNode) continue;
				state.nodes[i].render.dirty = 1;
				state.nodes[i].render.non_scroll_dirty = 1;
				Tree::instance().markNodeDisplayCommandsDirty(i);
			}
		}
		return;
	}
	target.render.layout_dirty = 1;
	Tree::instance().markDisplayListDirty();
}

struct CssText {
	const char *data = "";
	std::uint16_t length = 0;
	bool owned = false;
	bool hasVar = false;
	bool trimClean = true;

	CssText() = default;

	~CssText()
	{
		reset();
	}

	CssText(const CssText &other)
	{
		assign(other.data, other.length, other.owned);
	}

	CssText &operator=(const CssText &other)
	{
		if (this == &other) return *this;
		reset();
		assign(other.data, other.length, other.owned);
		return *this;
	}

	CssText(CssText &&other) noexcept
	    : data(other.data),
	      length(other.length),
	      owned(other.owned),
	      hasVar(other.hasVar),
	      trimClean(other.trimClean)
	{
		other.data = "";
		other.length = 0;
		other.owned = false;
		other.hasVar = false;
		other.trimClean = true;
	}

	CssText &operator=(CssText &&other) noexcept
	{
		if (this == &other) return *this;
		reset();
		data = other.data;
		length = other.length;
		owned = other.owned;
		hasVar = other.hasVar;
		trimClean = other.trimClean;
		other.data = "";
		other.length = 0;
		other.owned = false;
		other.hasVar = false;
		other.trimClean = true;
		return *this;
	}

	static CssText literal(const char *text)
	{
		CssText out;
		out.data = text ? text : "";
		out.length = clampLength(std::strlen(out.data));
		out.owned = false;
		out.refreshFlags();
		return out;
	}

	static CssText copy(const std::string &text)
	{
		CssText out;
		out.assignCopy(text.c_str(), text.size());
		return out;
	}

	static CssText view(const std::string &text)
	{
		CssText out;
		out.data = text.c_str();
		out.length = clampLength(text.size());
		out.owned = false;
		out.refreshFlags();
		return out;
	}

	static CssText view(const char *text, std::size_t size)
	{
		CssText out;
		out.data = text ? text : "";
		out.length = clampLength(size);
		out.owned = false;
		out.refreshFlags();
		return out;
	}

	bool empty() const { return length == 0; }
	const char *c_str() const { return data ? data : ""; }
	std::string str() const { return std::string(c_str(), length); }
	bool hasVarReference() const { return hasVar; }

	bool equals(const char *text) const
	{
		const char *rhs = text ? text : "";
		const std::size_t rhsLen = std::strlen(rhs);
		return rhsLen == length && std::memcmp(c_str(), rhs, length) == 0;
	}

	bool contains(const char *needle) const
	{
		return needle && std::strstr(c_str(), needle) != nullptr;
	}

	bool startsWith(const char *prefix) const
	{
		if (!prefix) return false;
		const std::size_t prefixLen = std::strlen(prefix);
		return prefixLen <= length && std::memcmp(c_str(), prefix, prefixLen) == 0;
	}

	std::string trimmedStr() const
	{
		if (trimClean) return str();
		std::size_t b = 0;
		std::size_t e = length;
		const char *text = c_str();
		while (b < e && static_cast<unsigned char>(text[b]) <= ' ') ++b;
		while (e > b && static_cast<unsigned char>(text[e - 1]) <= ' ') --e;
		return std::string(text + b, e - b);
	}

private:
	static std::uint16_t clampLength(std::size_t size)
	{
		return size > 0xFFFFu ? static_cast<std::uint16_t>(0xFFFFu) : static_cast<std::uint16_t>(size);
	}

	void reset()
	{
		if (owned) delete[] const_cast<char *>(data);
		data = "";
		length = 0;
		owned = false;
		hasVar = false;
		trimClean = true;
	}

	void assign(const char *text, std::uint16_t size, bool copyText)
	{
		if (copyText)
			assignCopy(text, size);
		else {
			data = text ? text : "";
			length = size;
			owned = false;
			refreshFlags();
		}
	}

	void assignCopy(const char *text, std::size_t size)
	{
		length = clampLength(size);
		char *copy = new char[static_cast<std::size_t>(length) + 1];
		if (length > 0 && text) std::memcpy(copy, text, length);
		copy[length] = '\0';
		data = copy;
		owned = true;
		refreshFlags();
	}

	void refreshFlags()
	{
		const char *text = c_str();
		hasVar = false;
		for (std::uint16_t i = 0; i + 4 <= length; ++i) {
			if (std::memcmp(text + i, "var(", 4) == 0) {
				hasVar = true;
				break;
			}
		}
		trimClean = length == 0 ||
		            (static_cast<unsigned char>(text[0]) > ' ' &&
		             static_cast<unsigned char>(text[length - 1]) > ' ');
	}
};

CssAtomId atomForText(const CssText &text)
{
	return text.empty() ? kInvalidCssAtom : internCssAtom(text.c_str(), text.length);
}

enum class CssRuleProperty : std::uint8_t {
	Other,
	Custom,
	Animation
};

using CssDeclarationId = StyleDeclaration;

CssDeclarationId classifyDeclaration(const char *property)
{
	if (!property || !*property) return CssDeclarationId::Unknown;
	if (property[0] == '-' && property[1] == '-') return CssDeclarationId::Custom;
	if (std::strcmp(property, "color-scheme") == 0 ||
	    std::strcmp(property, "background-position") == 0 ||
	    std::strcmp(property, "box-sizing") == 0 ||
	    std::strcmp(property, "font") == 0 ||
	    std::strcmp(property, "grid-column") == 0 ||
	    std::strcmp(property, "isolation") == 0 ||
	    std::strcmp(property, "letter-spacing") == 0 ||
	    std::strcmp(property, "outline") == 0 ||
	    std::strcmp(property, "scroll-snap-align") == 0 ||
	    std::strcmp(property, "scroll-snap-type") == 0 ||
	    std::strcmp(property, "scrollbar-width") == 0 ||
	    std::strcmp(property, "text-shadow") == 0 ||
	    std::strcmp(property, "transform-style") == 0 ||
	    std::strcmp(property, "transition") == 0 ||
	    std::strcmp(property, "cursor") == 0 ||
	    std::strcmp(property, "-webkit-tap-highlight-color") == 0)
		return CssDeclarationId::Ignored;
	if (std::strcmp(property, "animation") == 0) return CssDeclarationId::Animation;
	if (std::strcmp(property, "display") == 0) return CssDeclarationId::Display;
	if (std::strcmp(property, "flex-direction") == 0) return CssDeclarationId::FlexDirection;
	if (std::strcmp(property, "flex-wrap") == 0) return CssDeclarationId::FlexWrap;
	if (std::strcmp(property, "justify-content") == 0) return CssDeclarationId::JustifyContent;
	if (std::strcmp(property, "align-items") == 0) return CssDeclarationId::AlignItems;
	if (std::strcmp(property, "justify-items") == 0) return CssDeclarationId::JustifyItems;
	if (std::strcmp(property, "align-content") == 0) return CssDeclarationId::AlignContent;
	if (std::strcmp(property, "align-self") == 0) return CssDeclarationId::AlignSelf;
	if (std::strcmp(property, "place-items") == 0) return CssDeclarationId::PlaceItems;
	if (std::strcmp(property, "grid-template-columns") == 0) return CssDeclarationId::GridTemplateColumns;
	if (std::strcmp(property, "grid-template-rows") == 0) return CssDeclarationId::GridTemplateRows;
	if (std::strcmp(property, "content") == 0) return CssDeclarationId::Content;
	if (std::strcmp(property, "gap") == 0) return CssDeclarationId::Gap;
	if (std::strcmp(property, "width") == 0) return CssDeclarationId::Width;
	if (std::strcmp(property, "height") == 0) return CssDeclarationId::Height;
	if (std::strcmp(property, "min-width") == 0) return CssDeclarationId::MinWidth;
	if (std::strcmp(property, "min-height") == 0) return CssDeclarationId::MinHeight;
	if (std::strcmp(property, "max-width") == 0) return CssDeclarationId::MaxWidth;
	if (std::strcmp(property, "max-height") == 0) return CssDeclarationId::MaxHeight;
	if (std::strcmp(property, "flex") == 0) return CssDeclarationId::Flex;
	if (std::strcmp(property, "flex-grow") == 0) return CssDeclarationId::FlexGrow;
	if (std::strcmp(property, "flex-shrink") == 0) return CssDeclarationId::FlexShrink;
	if (std::strcmp(property, "flex-basis") == 0) return CssDeclarationId::FlexBasis;
	if (std::strcmp(property, "padding") == 0) return CssDeclarationId::Padding;
	if (std::strcmp(property, "padding-top") == 0) return CssDeclarationId::PaddingTop;
	if (std::strcmp(property, "padding-right") == 0) return CssDeclarationId::PaddingRight;
	if (std::strcmp(property, "padding-bottom") == 0) return CssDeclarationId::PaddingBottom;
	if (std::strcmp(property, "padding-left") == 0) return CssDeclarationId::PaddingLeft;
	if (std::strcmp(property, "margin") == 0) return CssDeclarationId::Margin;
	if (std::strcmp(property, "margin-top") == 0) return CssDeclarationId::MarginTop;
	if (std::strcmp(property, "margin-right") == 0) return CssDeclarationId::MarginRight;
	if (std::strcmp(property, "margin-bottom") == 0) return CssDeclarationId::MarginBottom;
	if (std::strcmp(property, "margin-left") == 0) return CssDeclarationId::MarginLeft;
	if (std::strcmp(property, "position") == 0) return CssDeclarationId::Position;
	if (std::strcmp(property, "inset") == 0) return CssDeclarationId::Inset;
	if (std::strcmp(property, "top") == 0) return CssDeclarationId::Top;
	if (std::strcmp(property, "right") == 0) return CssDeclarationId::Right;
	if (std::strcmp(property, "bottom") == 0) return CssDeclarationId::Bottom;
	if (std::strcmp(property, "left") == 0) return CssDeclarationId::Left;
	if (std::strcmp(property, "z-index") == 0) return CssDeclarationId::ZIndex;
	if (std::strcmp(property, "active-background-color") == 0 || std::strcmp(property, "active-background") == 0) return CssDeclarationId::ActiveBackgroundColor;
	if (std::strcmp(property, "background-color") == 0 || std::strcmp(property, "background") == 0 || std::strcmp(property, "background-image") == 0) return CssDeclarationId::Background;
	if (std::strcmp(property, "background-size") == 0) return CssDeclarationId::BackgroundSize;
	if (std::strcmp(property, "object-fit") == 0) return CssDeclarationId::ObjectFit;
	if (std::strcmp(property, "color") == 0) return CssDeclarationId::Color;
	if (std::strcmp(property, "opacity") == 0) return CssDeclarationId::Opacity;
	if (std::strcmp(property, "border-color") == 0) return CssDeclarationId::BorderColor;
	if (std::strcmp(property, "border") == 0) return CssDeclarationId::Border;
	if (std::strcmp(property, "border-width") == 0) return CssDeclarationId::BorderWidth;
	if (std::strcmp(property, "border-top") == 0) return CssDeclarationId::BorderTop;
	if (std::strcmp(property, "border-right") == 0) return CssDeclarationId::BorderRight;
	if (std::strcmp(property, "border-bottom") == 0) return CssDeclarationId::BorderBottom;
	if (std::strcmp(property, "border-left") == 0) return CssDeclarationId::BorderLeft;
	if (std::strcmp(property, "border-top-width") == 0) return CssDeclarationId::BorderTopWidth;
	if (std::strcmp(property, "border-right-width") == 0) return CssDeclarationId::BorderRightWidth;
	if (std::strcmp(property, "border-bottom-width") == 0) return CssDeclarationId::BorderBottomWidth;
	if (std::strcmp(property, "border-left-width") == 0) return CssDeclarationId::BorderLeftWidth;
	if (std::strcmp(property, "border-top-color") == 0) return CssDeclarationId::BorderTopColor;
	if (std::strcmp(property, "border-right-color") == 0) return CssDeclarationId::BorderRightColor;
	if (std::strcmp(property, "border-bottom-color") == 0) return CssDeclarationId::BorderBottomColor;
	if (std::strcmp(property, "border-left-color") == 0) return CssDeclarationId::BorderLeftColor;
	if (std::strcmp(property, "border-radius") == 0) return CssDeclarationId::BorderRadius;
	if (std::strcmp(property, "border-top-left-radius") == 0) return CssDeclarationId::BorderTopLeftRadius;
	if (std::strcmp(property, "border-top-right-radius") == 0) return CssDeclarationId::BorderTopRightRadius;
	if (std::strcmp(property, "border-bottom-right-radius") == 0) return CssDeclarationId::BorderBottomRightRadius;
	if (std::strcmp(property, "border-bottom-left-radius") == 0) return CssDeclarationId::BorderBottomLeftRadius;
	if (std::strcmp(property, "font-family") == 0) return CssDeclarationId::FontFamily;
	if (std::strcmp(property, "font-size") == 0) return CssDeclarationId::FontSize;
	if (std::strcmp(property, "font-weight") == 0) return CssDeclarationId::FontWeight;
	if (std::strcmp(property, "line-height") == 0) return CssDeclarationId::LineHeight;
	if (std::strcmp(property, "text-align") == 0) return CssDeclarationId::TextAlign;
	if (std::strcmp(property, "text-decoration") == 0 || std::strcmp(property, "text-decoration-line") == 0) return CssDeclarationId::TextDecoration;
	if (std::strcmp(property, "text-transform") == 0) return CssDeclarationId::TextTransform;
	if (std::strcmp(property, "white-space") == 0) return CssDeclarationId::WhiteSpace;
	if (std::strcmp(property, "text-overflow") == 0) return CssDeclarationId::TextOverflow;
	if (std::strcmp(property, "backface-visibility") == 0) return CssDeclarationId::BackfaceVisibility;
	if (std::strcmp(property, "pointer-events") == 0) return CssDeclarationId::PointerEvents;
	if (std::strcmp(property, "overflow") == 0) return CssDeclarationId::Overflow;
	if (std::strcmp(property, "overflow-x") == 0) return CssDeclarationId::OverflowX;
	if (std::strcmp(property, "overflow-y") == 0) return CssDeclarationId::OverflowY;
	if (std::strcmp(property, "mask-image") == 0 || std::strcmp(property, "-webkit-mask-image") == 0) return CssDeclarationId::MaskImage;
	if (std::strcmp(property, "transform") == 0) return CssDeclarationId::Transform;
	if (std::strcmp(property, "rotate") == 0) return CssDeclarationId::Rotate;
	if (std::strcmp(property, "scale") == 0) return CssDeclarationId::Scale;
	if (std::strcmp(property, "filter") == 0) return CssDeclarationId::Filter;
	if (std::strcmp(property, "box-shadow") == 0) return CssDeclarationId::BoxShadow;
	if (std::strcmp(property, "transform-origin") == 0) return CssDeclarationId::TransformOrigin;
	if (std::strcmp(property, "perspective") == 0) return CssDeclarationId::Perspective;
	if (std::strcmp(property, "perspective-origin") == 0) return CssDeclarationId::PerspectiveOrigin;
	return CssDeclarationId::Unknown;
}

CssDeclarationId classifyDeclaration(const CssText &property)
{
	return classifyDeclaration(property.c_str());
}

CssDeclarationId classifyDeclaration(const std::string &property)
{
	return classifyDeclaration(property.c_str());
}

CssRuleProperty rulePropertyKind(CssDeclarationId declaration)
{
	if (declaration == CssDeclarationId::Custom) return CssRuleProperty::Custom;
	if (declaration == CssDeclarationId::Animation) return CssRuleProperty::Animation;
	return CssRuleProperty::Other;
}

constexpr std::uint16_t kNoCompiledCssValue = 0xFFFFu;
constexpr std::uint16_t kNoCompiledCssAnimationSpec = 0xFFFFu;
constexpr std::uint16_t kNoCompiledCssBackground = 0xFFFFu;
constexpr std::uint16_t kNoCompiledCssGridTemplate = 0xFFFFu;
constexpr std::uint16_t kNoCompiledCssLengthExpression = 0xFFFFu;
constexpr std::uint16_t kNoCssRuleText = 0xFFFFu;
constexpr std::uint16_t kNoSelectorPlan = 0xFFFFu;
constexpr std::uint16_t kNoMediaConditionPlan = 0xFFFFu;

#ifndef GEA_COMPILED_CSS_VALUE_MASK
#define GEA_COMPILED_CSS_VALUE_MASK \
	(kCssCompiledFeatureKeyword | kCssCompiledFeatureColor | kCssCompiledFeatureOpacity | \
	 kCssCompiledFeatureSize | kCssCompiledFeaturePosition | kCssCompiledFeatureBox | \
	 kCssCompiledFeatureLength | kCssCompiledFeatureOrigin | kCssCompiledFeatureTransformScalar | \
	 kCssCompiledFeatureTransform | kCssCompiledFeatureBackground | kCssCompiledFeatureEffects | \
	 kCssCompiledFeatureGridTemplate | kCssCompiledFeatureNumber)
#endif
#ifndef GEA_INLINE_COMPILED_STYLE_CACHE_ENTRIES
#define GEA_INLINE_COMPILED_STYLE_CACHE_ENTRIES 32
#endif

constexpr std::uint32_t kCssCompiledFeatureKeyword = 1u << 0;
constexpr std::uint32_t kCssCompiledFeatureColor = 1u << 1;
constexpr std::uint32_t kCssCompiledFeatureOpacity = 1u << 2;
constexpr std::uint32_t kCssCompiledFeatureSize = 1u << 3;
constexpr std::uint32_t kCssCompiledFeaturePosition = 1u << 4;
constexpr std::uint32_t kCssCompiledFeatureBox = 1u << 5;
constexpr std::uint32_t kCssCompiledFeatureLength = 1u << 6;
constexpr std::uint32_t kCssCompiledFeatureOrigin = 1u << 7;
constexpr std::uint32_t kCssCompiledFeatureTransformScalar = 1u << 8;
constexpr std::uint32_t kCssCompiledFeatureTransform = 1u << 9;
constexpr std::uint32_t kCssCompiledFeatureBackground = 1u << 10;
constexpr std::uint32_t kCssCompiledFeatureEffects = 1u << 11;
constexpr std::uint32_t kCssCompiledFeatureGridTemplate = 1u << 12;
constexpr std::uint32_t kCssCompiledFeatureNumber = 1u << 13;
constexpr int kInlineCompiledStyleCacheEntries = GEA_INLINE_COMPILED_STYLE_CACHE_ENTRIES;
static_assert(kInlineCompiledStyleCacheEntries >= 0);

constexpr bool compiledCssValueFeatureEnabled(std::uint32_t feature)
{
	return (static_cast<std::uint32_t>(GEA_COMPILED_CSS_VALUE_MASK) & feature) != 0u;
}

enum class CssCompiledKind : std::uint8_t {
	None,
	Noop,
	DirectProperty,
	DirectPropertyGroup,
	Keyword,
	Number,
	Length,
	Size,
	PositionOffset,
	Box,
	Color,
	ColorVar,
	Opacity,
	Rotate,
	Scale,
	OriginPair,
	Transform,
	LineHeight,
	Background,
	BackgroundSize,
	FilterBlur,
	BoxShadow,
	GridTemplate,
	Flex,
	FlexBasis,
	BorderShorthand,
	BorderSideShorthand,
	BorderRadius
};

enum class CssLengthUnit : std::uint8_t {
	Invalid,
	Raw,
	Px,
	Percent,
	Vw,
	Vh,
	Vmin,
	Vmax,
	Dvw,
	Dvh,
	Auto,
	Expression
};

struct CssLengthSpec {
	float value = 0.0f;
	CssLengthUnit unit = CssLengthUnit::Invalid;
};

enum class CssLengthExpressionKind : std::uint8_t {
	Add,
	Subtract,
	Multiply,
	Divide,
	Min,
	Max,
	Clamp,
	Var
};

struct CssLengthExpression {
	CssLengthExpressionKind kind = CssLengthExpressionKind::Add;
	CssLengthSpec a;
	CssLengthSpec b;
	CssLengthSpec c;
	float scalar = 0.0f;
	CssAtomId nameAtom = kInvalidCssAtom;
	std::uint8_t hasFallback = 0;
};

struct ResolvedCssLength {
	int value = 0;
	bool isPercent = false;
	bool isAuto = false;
};

struct CachedCssColor {
	std::int32_t styleColor = 0;
	std::int32_t nativeColor = 0;
	std::uint8_t alpha = 255;
	bool valid = false;
};

struct CssCompiledValue {
	CssCompiledKind kind = CssCompiledKind::None;
	CssDeclarationId declaration = CssDeclarationId::Unknown;
	std::uint16_t flags = 0;
	std::uint8_t aux = 0;
	CssLengthSpec lengths[4];
	std::int32_t values[10]{};
};

CssLengthSpec cssLengthSpecForStatic(StaticStyleLengthSpec spec);
struct CssCompiledBackground;
std::vector<CssCompiledBackground> &compiledCssBackgrounds();

struct CssCompiledLinearGradient {
	std::int32_t fromStyleColor = 0;
	style_color_t fromNativeColor = 0;
	style_color_t midNativeColor = 0;
	style_color_t toNativeColor = 0;
	CssAtomId fromColorAtom = kInvalidCssAtom;
	CssAtomId midColorAtom = kInvalidCssAtom;
	CssAtomId toColorAtom = kInvalidCssAtom;
	std::uint8_t fromAlpha = 255;
	std::uint8_t midAlpha = 255;
	std::uint8_t toAlpha = 255;
	std::uint16_t midStopPermille = 500;
	std::uint16_t toStopPermille = 1000;
	std::uint8_t hasMid = 0;
	std::uint8_t fromColorHasFallback = 0;
	std::uint8_t midColorHasFallback = 0;
	std::uint8_t toColorHasFallback = 0;
	std::int16_t angleTenths = 1800;
};

struct CssCompiledRadialGradient {
	style_color_t fromNativeColor = 0;
	style_color_t toNativeColor = 0;
	CssAtomId fromColorAtom = kInvalidCssAtom;
	CssAtomId toColorAtom = kInvalidCssAtom;
	std::uint8_t fromAlpha = 255;
	std::uint8_t toAlpha = 255;
	std::uint8_t fromColorHasFallback = 0;
	std::uint8_t toColorHasFallback = 0;
	std::uint16_t stopPermille = 1000;
	std::int16_t cxPermille = 500;
	std::int16_t cyPermille = 500;
	std::int16_t rxPermille = 1000;
	std::int16_t ryPermille = 1000;
};

struct CssCompiledBackground {
	CssCompiledLinearGradient gradient;
	CssCompiledLinearGradient overlayGradient;
	CssCompiledRadialGradient radialGradient;
	CssLengthSpec gridLineX;
	CssLengthSpec gridLineY;
	style_color_t gridColor = 0;
	std::uint8_t gridAlpha = 255;
	std::uint8_t gridAxes = 0;
	std::uint8_t hasGridLineX = 0;
	std::uint8_t hasGridLineY = 0;
	std::uint8_t hasGradient = 0;
	std::uint8_t hasOverlayGradient = 0;
	std::uint8_t hasRadialGradient = 0;
};

struct CssCompiledGridTrack {
	std::int8_t type = 0;
	std::int16_t value = 0;
	CssLengthSpec length;
};

struct CssCompiledGridTemplate {
	std::uint8_t count = 0;
	CssCompiledGridTrack tracks[kMaxGridTracks];
};

std::uint16_t compileCssValue(CssDeclarationId declaration, const CssText &value);
std::uint16_t compileCustomPropertyValue(const CssText &value);
std::uint16_t compileCssAnimationSpec(const CssText &value);
std::uint16_t compileSelectorPlan(const CssText &selector);
std::uint16_t compileMediaConditionPlan(const CssText &condition);
void clearCompiledCssBackgrounds();
void clearCompiledCssGridTemplates();
void clearCompiledCssLengthExpressions();
void clearStaticLengthExpressionResolutionCache();

struct CssRule {
	enum class SelectorType {
		Class,
		Element,
		Selector
	};

	enum class PseudoElement {
		None,
		Before,
		After,
		Unsupported
	};

	SelectorType selectorType;
	PseudoElement pseudoElement;
	CssRuleProperty propertyKind;
	CssDeclarationId declaration;
	std::uint16_t compiledValue;
	std::uint16_t compiledAnimationSpec;
	CssAtomId selectorAtom;
	CssAtomId propertyAtom;
	int16_t selectorTagId;
	std::uint16_t selectorPlan;
	std::uint16_t mediaPlan;
	std::uint16_t propertyText;
	std::uint16_t valueText;
	std::uint16_t mediaText;
};

struct CssKeyframeRule {
	CssAtomId nameAtom;
	int offsetPermille;
	CssRuleProperty propertyKind;
	CssDeclarationId declaration;
	std::uint16_t compiledValue;
	std::uint16_t valueText;
};

struct SelectorTextSlice {
	const char *data = "";
	std::size_t length = 0;
	CssRule::PseudoElement pseudo = CssRule::PseudoElement::None;
};

bool asciiEqualsIgnoreCaseTrimmed(const char *text, std::size_t length, const char *expected)
{
	if (!text || !expected) return false;
	while (length > 0 && static_cast<unsigned char>(*text) <= ' ') {
		++text;
		--length;
	}
	while (length > 0 && static_cast<unsigned char>(text[length - 1]) <= ' ') --length;
	const std::size_t expectedLength = std::strlen(expected);
	if (length != expectedLength) return false;
	for (std::size_t i = 0; i < length; ++i) {
		const char lhs = static_cast<char>(std::tolower(static_cast<unsigned char>(text[i])));
		if (lhs != expected[i]) return false;
	}
	return true;
}

SelectorTextSlice selectorTextWithoutPseudo(const char *selectorText, std::size_t selectorLength)
{
	SelectorTextSlice out;
	out.data = selectorText ? selectorText : "";
	out.length = selectorText ? selectorLength : 0;
	for (std::size_t i = 0; i + 1 < out.length; ++i) {
		if (out.data[i] != ':' || out.data[i + 1] != ':') continue;
		std::size_t selectorEnd = i;
		while (selectorEnd > 0 && static_cast<unsigned char>(out.data[selectorEnd - 1]) <= ' ') --selectorEnd;
		const char *name = out.data + i + 2;
		const std::size_t nameLength = out.length - i - 2;
		out.length = selectorEnd;
		if (asciiEqualsIgnoreCaseTrimmed(name, nameLength, "before")) {
			out.pseudo = CssRule::PseudoElement::Before;
		} else if (asciiEqualsIgnoreCaseTrimmed(name, nameLength, "after")) {
			out.pseudo = CssRule::PseudoElement::After;
		} else {
			out.pseudo = CssRule::PseudoElement::Unsupported;
		}
		return out;
	}
	return out;
}

enum class StyleApplicationSource {
	Inline,
	ClassRule
};

enum class LengthAxis {
	None,
	Horizontal,
	Vertical
};

bool isInheritedStyleProperty(Property property);
void recomputeDescendantClassStyles(int node);
void recomputeSubtreeClassStyles(int node);
void setStyleValue(NodeHandle node, Property property, int value, StyleApplicationSource source);
struct ActiveRulePlan;
void primeCssAnimationsForNode(int node, const ActiveRulePlan *activePlan = nullptr);
int parseOriginPart(const std::string &part, int fallback);

// File-scope lazy pointer rather than a function-local static: the static-local
// guard is not inlined on this Xtensa toolchain, so a Meyers singleton pays a
// __cxa_guard_acquire CALL per access, and rules() is iterated per node during
// every class-style recompute. Single-threaded UI access, so no guard needed.
std::vector<CssRule> *g_rules = nullptr;

std::vector<CssRule> &rules()
{
	if (!g_rules) g_rules = new std::vector<CssRule>();
	return *g_rules;
}

std::vector<CssKeyframeRule> &keyframeRules()
{
	static std::vector<CssKeyframeRule> list;
	return list;
}

struct DenseRuleBucketSpan {
	const int *data = nullptr;
	std::size_t count = 0;

	bool empty() const { return count == 0; }
};

struct DenseRuleBuckets {
	struct PendingEntry {
		std::uint16_t key;
		int value;
	};

	std::vector<PendingEntry> pending;
	std::vector<int> values;
	std::vector<std::uint32_t> offsets;
	std::size_t nonEmpty = 0;
	std::uint16_t maxKey = 0;
	bool hasPending = false;

	void clear()
	{
		pending.clear();
		values.clear();
		offsets.clear();
		nonEmpty = 0;
		maxKey = 0;
		hasPending = false;
	}

	void add(int key, int value)
	{
		if (key < 0 || key > 0xFFFF) return;
		const std::size_t index = static_cast<std::size_t>(key);
		pending.push_back({static_cast<std::uint16_t>(index), value});
		if (index > maxKey) maxKey = static_cast<std::uint16_t>(index);
		hasPending = true;
	}

	void finalize()
	{
		values.clear();
		offsets.clear();
		nonEmpty = 0;
		if (pending.empty()) {
			hasPending = false;
			return;
		}
		offsets.assign(static_cast<std::size_t>(maxKey) + 2, 0);
		for (const PendingEntry &entry : pending)
			++offsets[static_cast<std::size_t>(entry.key) + 1];
		std::uint32_t running = 0;
		for (std::size_t key = 0; key + 1 < offsets.size(); ++key) {
			const std::uint32_t count = offsets[key + 1];
			if (count != 0) ++nonEmpty;
			offsets[key] = running;
			running += count;
		}
		offsets.back() = running;
		values.resize(pending.size());
		for (const PendingEntry &entry : pending) {
			const std::size_t index = static_cast<std::size_t>(entry.key);
			values[offsets[index]++] = entry.value;
		}
		for (std::size_t i = offsets.size() - 1; i > 0; --i)
			offsets[i] = offsets[i - 1];
		offsets[0] = 0;
		std::vector<PendingEntry>().swap(pending);
		hasPending = false;
	}

	DenseRuleBucketSpan get(int key) const
	{
		if (key < 0 || hasPending) return {};
		const std::size_t index = static_cast<std::size_t>(key);
		if (index + 1 >= offsets.size()) return {};
		const std::uint32_t begin = offsets[index];
		const std::uint32_t end = offsets[index + 1];
		if (begin == end) return {};
		return {values.data() + begin, static_cast<std::size_t>(end - begin)};
	}

	bool empty() const { return nonEmpty == 0; }
};

struct KeyframeRuleIndex {
	bool valid = false;
	DenseRuleBuckets byName;
};

KeyframeRuleIndex g_keyframeRuleIndex;

struct DenseIdSet {
	std::vector<std::uint8_t> bits;
	std::size_t count = 0;

	void clear()
	{
		std::fill(bits.begin(), bits.end(), 0);
		count = 0;
	}

	void insert(int key)
	{
		if (key < 0) return;
		const std::size_t index = static_cast<std::size_t>(key);
		if (index >= bits.size()) bits.resize(index + 1, 0);
		if (bits[index] == 0) {
			bits[index] = 1;
			++count;
		}
	}

	bool contains(int key) const
	{
		if (key < 0) return false;
		const std::size_t index = static_cast<std::size_t>(key);
		return index < bits.size() && bits[index] != 0;
	}

	bool empty() const { return count == 0; }
};

void invalidateKeyframeRuleIndex()
{
	g_keyframeRuleIndex.valid = false;
}

void clearKeyframeRuleIndex()
{
	g_keyframeRuleIndex.byName.clear();
	g_keyframeRuleIndex.valid = false;
}

void rebuildKeyframeRuleIndexIfNeeded()
{
	if (g_keyframeRuleIndex.valid) return;
	g_keyframeRuleIndex.byName.clear();
	const auto &list = keyframeRules();
	for (int i = 0; i < static_cast<int>(list.size()); ++i) {
		if (list[i].nameAtom == kInvalidCssAtom) continue;
		g_keyframeRuleIndex.byName.add(list[i].nameAtom, i);
	}
	g_keyframeRuleIndex.byName.finalize();
	g_keyframeRuleIndex.valid = true;
}

DenseRuleBucketSpan keyframeRuleIndicesForName(CssAtomId name)
{
	if (name == kInvalidCssAtom) return {};
	rebuildKeyframeRuleIndexIfNeeded();
	return g_keyframeRuleIndex.byName.get(name);
}

std::vector<CssCompiledValue> &compiledCssValues()
{
	static std::vector<CssCompiledValue> list;
	return list;
}

std::vector<CssText> &cssRuleTexts()
{
	static std::vector<CssText> list;
	return list;
}

void clearCssRuleTexts()
{
	cssRuleTexts().clear();
}

std::uint16_t storeCssRuleText(CssText text)
{
	if (text.empty()) return kNoCssRuleText;
	auto &list = cssRuleTexts();
	if (list.size() >= kNoCssRuleText) return kNoCssRuleText;
	list.push_back(std::move(text));
	return static_cast<std::uint16_t>(list.size() - 1);
}

const CssText &cssRuleTextForHandle(std::uint16_t handle)
{
	static const CssText empty;
	const auto &list = cssRuleTexts();
	return handle < list.size() ? list[handle] : empty;
}

std::uint16_t storeDirectPropertyCompiledValue(Property property, int value)
{
	auto &list = compiledCssValues();
	if (list.size() >= kNoCompiledCssValue) return kNoCompiledCssValue;
	CssCompiledValue compiled;
	compiled.kind = CssCompiledKind::DirectProperty;
	compiled.values[0] = static_cast<int>(property);
	compiled.values[1] = value;
	list.push_back(compiled);
	return static_cast<std::uint16_t>(list.size() - 1);
}

std::uint16_t storeDirectPropertyGroupCompiledValue(std::initializer_list<StaticStylePropertyValue> properties)
{
	auto &list = compiledCssValues();
	if (list.size() >= kNoCompiledCssValue) return kNoCompiledCssValue;
	CssCompiledValue compiled;
	compiled.kind = CssCompiledKind::DirectPropertyGroup;
	int count = 0;
	for (const StaticStylePropertyValue &entry : properties) {
		if (count >= 4) break;
		compiled.values[1 + count * 2] = static_cast<int>(entry.property);
		compiled.values[2 + count * 2] = entry.value;
		count++;
	}
	if (count == 0) return kNoCompiledCssValue;
	compiled.values[0] = count;
	list.push_back(compiled);
	return static_cast<std::uint16_t>(list.size() - 1);
}

int clampStaticCssColorChannel(int value)
{
	return std::clamp(value, 0, 255);
}

std::uint16_t storeStaticColorCompiledValue(CssDeclarationId declaration, int r, int g, int b, int a)
{
	auto &list = compiledCssValues();
	if (list.size() >= kNoCompiledCssValue) return kNoCompiledCssValue;
	const int cr = clampStaticCssColorChannel(r);
	const int cg = clampStaticCssColorChannel(g);
	const int cb = clampStaticCssColorChannel(b);
	CssCompiledValue compiled;
	compiled.kind = CssCompiledKind::Color;
	compiled.declaration = declaration;
	compiled.values[0] = static_cast<std::int32_t>(gea::framework::graphics::pixel::nativeStyleValue(cr, cg, cb));
	compiled.values[1] = static_cast<std::int32_t>(gea::framework::graphics::pixel::nativeColor(cr, cg, cb));
	compiled.values[2] = clampStaticCssColorChannel(a);
	list.push_back(compiled);
	return static_cast<std::uint16_t>(list.size() - 1);
}

std::uint16_t storeStaticColorVarCompiledValue(CssDeclarationId declaration,
                                               const char *name,
                                               bool hasFallback,
                                               int r,
                                               int g,
                                               int b,
                                               int a)
{
	auto &list = compiledCssValues();
	if (list.size() >= kNoCompiledCssValue) return kNoCompiledCssValue;
	const int cr = clampStaticCssColorChannel(r);
	const int cg = clampStaticCssColorChannel(g);
	const int cb = clampStaticCssColorChannel(b);
	CssCompiledValue compiled;
	compiled.kind = CssCompiledKind::ColorVar;
	compiled.declaration = declaration;
	compiled.values[0] = static_cast<std::int32_t>(internCssAtom(name ? name : ""));
	compiled.aux = hasFallback ? 1 : 0;
	if (hasFallback) {
		compiled.values[1] = static_cast<std::int32_t>(gea::framework::graphics::pixel::nativeStyleValue(cr, cg, cb));
		compiled.values[2] = static_cast<std::int32_t>(gea::framework::graphics::pixel::nativeColor(cr, cg, cb));
		compiled.values[3] = clampStaticCssColorChannel(a);
	}
	list.push_back(compiled);
	return static_cast<std::uint16_t>(list.size() - 1);
}

std::uint16_t storeStaticFontFamilyCompiledValue(const char *family)
{
	auto &list = compiledCssValues();
	if (list.size() >= kNoCompiledCssValue) return kNoCompiledCssValue;
	CssCompiledValue compiled;
	compiled.kind = CssCompiledKind::Keyword;
	compiled.declaration = CssDeclarationId::FontFamily;
	compiled.values[0] = gea::framework::graphics::FontRegistry::familyId(family ? family : "");
	list.push_back(compiled);
	return static_cast<std::uint16_t>(list.size() - 1);
}

std::uint16_t storeStaticLineHeightCompiledValue(StaticStyleLineHeightKind kind,
                                                 StaticStyleLengthSpec value)
{
	auto &list = compiledCssValues();
	if (list.size() >= kNoCompiledCssValue) return kNoCompiledCssValue;
	CssCompiledValue compiled;
	compiled.kind = CssCompiledKind::LineHeight;
	compiled.declaration = CssDeclarationId::LineHeight;
	compiled.lengths[0] = cssLengthSpecForStatic(value);
	switch (kind) {
	case StaticStyleLineHeightKind::Normal:
		compiled.aux = 0;
		break;
	case StaticStyleLineHeightKind::Scalar:
		compiled.aux = 1;
		compiled.lengths[0].unit = CssLengthUnit::Raw;
		break;
	case StaticStyleLineHeightKind::Percent:
		compiled.aux = 2;
		compiled.lengths[0].unit = CssLengthUnit::Percent;
		break;
	case StaticStyleLineHeightKind::Length:
		compiled.aux = 3;
		break;
	}
	list.push_back(compiled);
	return static_cast<std::uint16_t>(list.size() - 1);
}

std::uint16_t storeStaticFlexCompiledValue(int grow, StaticStyleLengthSpec basis, bool hasBasis)
{
	auto &list = compiledCssValues();
	if (list.size() >= kNoCompiledCssValue) return kNoCompiledCssValue;
	CssCompiledValue compiled;
	compiled.kind = CssCompiledKind::Flex;
	compiled.declaration = CssDeclarationId::Flex;
	compiled.values[0] = grow;
	compiled.values[1] = hasBasis && grow == 0 ? 0 : 1;
	compiled.aux = hasBasis ? 1 : 0;
	if (hasBasis) compiled.lengths[0] = cssLengthSpecForStatic(basis);
	list.push_back(compiled);
	return static_cast<std::uint16_t>(list.size() - 1);
}

std::uint16_t storeStaticBorderCompiledValue(StaticStyleLengthSpec width,
                                             int r,
                                             int g,
                                             int b,
                                             int a)
{
	auto &list = compiledCssValues();
	if (list.size() >= kNoCompiledCssValue) return kNoCompiledCssValue;
	const int cr = clampStaticCssColorChannel(r);
	const int cg = clampStaticCssColorChannel(g);
	const int cb = clampStaticCssColorChannel(b);
	CssCompiledValue compiled;
	compiled.kind = CssCompiledKind::BorderShorthand;
	compiled.declaration = CssDeclarationId::Border;
	compiled.aux = 1;
	compiled.lengths[0] = cssLengthSpecForStatic(width);
	compiled.values[0] = static_cast<std::int32_t>(gea::framework::graphics::pixel::nativeStyleValue(cr, cg, cb));
	compiled.values[1] = clampStaticCssColorChannel(a);
	list.push_back(compiled);
	return static_cast<std::uint16_t>(list.size() - 1);
}

style_color_t staticNativeColor(StaticStyleColor color)
{
	return gea::framework::graphics::pixel::nativeColor(clampStaticCssColorChannel(color.r),
	                                                    clampStaticCssColorChannel(color.g),
	                                                    clampStaticCssColorChannel(color.b));
}

std::int32_t staticStyleColorValue(StaticStyleColor color)
{
	return static_cast<std::int32_t>(gea::framework::graphics::pixel::nativeStyleValue(
	    clampStaticCssColorChannel(color.r),
	    clampStaticCssColorChannel(color.g),
	    clampStaticCssColorChannel(color.b)));
}

std::uint16_t staticStopPermille(int value, int fallback, int maxValue)
{
	if (value < 0) value = fallback;
	if (value < 0) value = 0;
	if (value > maxValue) value = maxValue;
	return static_cast<std::uint16_t>(value);
}

CssCompiledLinearGradient staticLinearGradient(StaticStyleLinearGradient gradient)
{
	CssCompiledLinearGradient out;
	out.fromStyleColor = staticStyleColorValue(gradient.from);
	out.fromNativeColor = staticNativeColor(gradient.from);
	out.fromAlpha = static_cast<std::uint8_t>(clampStaticCssColorChannel(gradient.from.a));
	out.toNativeColor = staticNativeColor(gradient.to);
	out.toAlpha = static_cast<std::uint8_t>(clampStaticCssColorChannel(gradient.to.a));
	out.hasMid = gradient.hasMid ? 1 : 0;
	out.midStopPermille = staticStopPermille(gradient.midStopPermille, 500, 1000);
	int toStop = gradient.toStopPermille;
	if (toStop <= 0) toStop = 1;
	if (gradient.hasMid && toStop <= static_cast<int>(out.midStopPermille))
		toStop = static_cast<int>(out.midStopPermille) + 1;
	out.toStopPermille = staticStopPermille(toStop, 1000, 60000);
	if (gradient.hasMid) {
		out.midNativeColor = staticNativeColor(gradient.mid);
		out.midAlpha = static_cast<std::uint8_t>(clampStaticCssColorChannel(gradient.mid.a));
	} else {
		out.midNativeColor = 0;
		out.midAlpha = 255;
	}
	out.angleTenths = static_cast<std::int16_t>(std::clamp(gradient.angleTenths, -32768, 32767));
	return out;
}

CssAtomId staticColorRefAtom(StaticStyleColorRef color)
{
	if (!color.varName || color.varName[0] == '\0') return kInvalidCssAtom;
	return internCssAtom(color.varName);
}

void applyStaticLinearGradientColorRef(CssCompiledLinearGradient &out,
                                       StaticStyleColorRef color,
                                       int slot)
{
	const CssAtomId atom = staticColorRefAtom(color);
	const style_color_t nativeColor = staticNativeColor(color.fallback);
	const std::uint8_t alpha = atom == kInvalidCssAtom || color.hasFallback
	    ? static_cast<std::uint8_t>(clampStaticCssColorChannel(color.fallback.a))
	    : 255;
	const std::uint8_t hasFallback = color.hasFallback ? 1 : 0;
	if (slot == 0) {
		out.fromStyleColor = staticStyleColorValue(color.fallback);
		out.fromNativeColor = nativeColor;
		out.fromAlpha = alpha;
		out.fromColorAtom = atom;
		out.fromColorHasFallback = hasFallback;
	} else if (slot == 1) {
		out.midNativeColor = nativeColor;
		out.midAlpha = alpha;
		out.midColorAtom = atom;
		out.midColorHasFallback = hasFallback;
	} else {
		out.toNativeColor = nativeColor;
		out.toAlpha = alpha;
		out.toColorAtom = atom;
		out.toColorHasFallback = hasFallback;
	}
}

CssCompiledLinearGradient staticLinearGradientRef(StaticStyleLinearGradientRef gradient)
{
	CssCompiledLinearGradient out;
	applyStaticLinearGradientColorRef(out, gradient.from, 0);
	applyStaticLinearGradientColorRef(out, gradient.to, 2);
	out.hasMid = gradient.hasMid ? 1 : 0;
	out.midStopPermille = staticStopPermille(gradient.midStopPermille, 500, 1000);
	int toStop = gradient.toStopPermille;
	if (toStop <= 0) toStop = 1;
	if (gradient.hasMid && toStop <= static_cast<int>(out.midStopPermille))
		toStop = static_cast<int>(out.midStopPermille) + 1;
	out.toStopPermille = staticStopPermille(toStop, 1000, 60000);
	if (gradient.hasMid) {
		applyStaticLinearGradientColorRef(out, gradient.mid, 1);
	} else {
		out.midNativeColor = 0;
		out.midAlpha = 255;
	}
	out.angleTenths = static_cast<std::int16_t>(std::clamp(gradient.angleTenths, -32768, 32767));
	return out;
}

void applyStaticRadialGradientColorRef(CssCompiledRadialGradient &out,
                                       StaticStyleColorRef color,
                                       bool from)
{
	const CssAtomId atom = staticColorRefAtom(color);
	const style_color_t nativeColor = staticNativeColor(color.fallback);
	const std::uint8_t alpha = atom == kInvalidCssAtom || color.hasFallback
	    ? static_cast<std::uint8_t>(clampStaticCssColorChannel(color.fallback.a))
	    : 255;
	if (from) {
		out.fromNativeColor = nativeColor;
		out.fromAlpha = alpha;
		out.fromColorAtom = atom;
		out.fromColorHasFallback = color.hasFallback ? 1 : 0;
	} else {
		out.toNativeColor = nativeColor;
		out.toAlpha = alpha;
		out.toColorAtom = atom;
		out.toColorHasFallback = color.hasFallback ? 1 : 0;
	}
}

CssCompiledRadialGradient staticRadialGradientRef(StaticStyleRadialGradientRef gradient)
{
	CssCompiledRadialGradient out;
	applyStaticRadialGradientColorRef(out, gradient.from, true);
	applyStaticRadialGradientColorRef(out, gradient.to, false);
	int stop = gradient.stopPermille;
	if (stop <= 0) stop = 1;
	out.stopPermille = staticStopPermille(stop, 1000, 1000);
	out.cxPermille = static_cast<std::int16_t>(std::clamp(gradient.cxPermille, -32768, 32767));
	out.cyPermille = static_cast<std::int16_t>(std::clamp(gradient.cyPermille, -32768, 32767));
	out.rxPermille = static_cast<std::int16_t>(std::clamp(gradient.rxPermille, -32768, 32767));
	out.ryPermille = static_cast<std::int16_t>(std::clamp(gradient.ryPermille, -32768, 32767));
	return out;
}

void applyStaticBackgroundGridLine(CssCompiledBackground &background,
                                   StaticStyleBackgroundGridLine line,
                                   bool xAxis)
{
	if (!line.enabled) return;
	background.gridAxes |= xAxis ? 1 : 2;
	background.gridColor = staticNativeColor(line.color);
	background.gridAlpha = static_cast<std::uint8_t>(clampStaticCssColorChannel(line.color.a));
	if (xAxis) {
		background.gridLineX = cssLengthSpecForStatic(line.width);
		background.hasGridLineX = 1;
	} else {
		background.gridLineY = cssLengthSpecForStatic(line.width);
		background.hasGridLineY = 1;
	}
}

std::uint16_t storeStaticBackgroundCompiledValue(StaticStyleLinearGradient gradient,
                                                 StaticStyleLinearGradient overlayGradient,
                                                 bool hasOverlayGradient,
                                                 StaticStyleBackgroundGridLine gridX,
                                                 StaticStyleBackgroundGridLine gridY)
{
	auto &values = compiledCssValues();
	auto &backgrounds = compiledCssBackgrounds();
	if (values.size() >= kNoCompiledCssValue || backgrounds.size() >= kNoCompiledCssBackground)
		return kNoCompiledCssValue;

	CssCompiledBackground background;
	background.gradient = staticLinearGradient(gradient);
	background.hasGradient = 1;
	if (hasOverlayGradient) {
		background.overlayGradient = staticLinearGradient(overlayGradient);
		background.hasOverlayGradient = 1;
	}
	applyStaticBackgroundGridLine(background, gridX, true);
	applyStaticBackgroundGridLine(background, gridY, false);

	backgrounds.push_back(background);
	CssCompiledValue compiled;
	compiled.kind = CssCompiledKind::Background;
	compiled.declaration = CssDeclarationId::Background;
	compiled.values[0] = static_cast<std::int32_t>(backgrounds.size() - 1);
	values.push_back(compiled);
	return static_cast<std::uint16_t>(values.size() - 1);
}

std::uint16_t storeStaticBackgroundFullCompiledValue(StaticStyleLinearGradientRef gradient,
                                                     StaticStyleLinearGradientRef overlayGradient,
                                                     bool hasOverlayGradient,
                                                     StaticStyleRadialGradientRef radialGradient,
                                                     StaticStyleBackgroundGridLine gridX,
                                                     StaticStyleBackgroundGridLine gridY)
{
	auto &values = compiledCssValues();
	auto &backgrounds = compiledCssBackgrounds();
	if (values.size() >= kNoCompiledCssValue || backgrounds.size() >= kNoCompiledCssBackground)
		return kNoCompiledCssValue;

	CssCompiledBackground background;
	background.gradient = staticLinearGradientRef(gradient);
	background.hasGradient = 1;
	if (hasOverlayGradient) {
		background.overlayGradient = staticLinearGradientRef(overlayGradient);
		background.hasOverlayGradient = 1;
	}
	if (radialGradient.enabled) {
		background.radialGradient = staticRadialGradientRef(radialGradient);
		background.hasRadialGradient = 1;
	}
	applyStaticBackgroundGridLine(background, gridX, true);
	applyStaticBackgroundGridLine(background, gridY, false);

	backgrounds.push_back(background);
	CssCompiledValue compiled;
	compiled.kind = CssCompiledKind::Background;
	compiled.declaration = CssDeclarationId::Background;
	compiled.values[0] = static_cast<std::int32_t>(backgrounds.size() - 1);
	values.push_back(compiled);
	return static_cast<std::uint16_t>(values.size() - 1);
}

std::uint16_t storeStaticBackgroundSizeCompiledValue(StaticStyleLengthSpec stepX,
                                                     StaticStyleLengthSpec stepY)
{
	auto &list = compiledCssValues();
	if (list.size() >= kNoCompiledCssValue) return kNoCompiledCssValue;
	CssCompiledValue compiled;
	compiled.kind = CssCompiledKind::BackgroundSize;
	compiled.declaration = CssDeclarationId::BackgroundSize;
	compiled.lengths[0] = cssLengthSpecForStatic(stepX);
	compiled.lengths[1] = cssLengthSpecForStatic(stepY);
	list.push_back(compiled);
	return static_cast<std::uint16_t>(list.size() - 1);
}

std::uint16_t storeStaticCustomLengthCompiledValue(StaticStyleLengthSpec length)
{
	auto &list = compiledCssValues();
	if (list.size() >= kNoCompiledCssValue) return kNoCompiledCssValue;
	CssCompiledValue compiled;
	compiled.kind = CssCompiledKind::Length;
	compiled.declaration = CssDeclarationId::Custom;
	compiled.lengths[0] = cssLengthSpecForStatic(length);
	list.push_back(compiled);
	return static_cast<std::uint16_t>(list.size() - 1);
}

std::vector<CssCompiledBackground> &compiledCssBackgrounds()
{
	static std::vector<CssCompiledBackground> list;
	return list;
}

void clearCompiledCssBackgrounds()
{
	compiledCssBackgrounds().clear();
}

std::vector<CssCompiledGridTemplate> &compiledCssGridTemplates()
{
	static std::vector<CssCompiledGridTemplate> list;
	return list;
}

void clearCompiledCssGridTemplates()
{
	compiledCssGridTemplates().clear();
}

std::vector<CssLengthExpression> &compiledCssLengthExpressions()
{
	static std::vector<CssLengthExpression> list;
	return list;
}

struct StaticLengthExpressionResolution {
	ResolvedCssLength value;
	std::uint8_t status = 0;  // 0 unknown, 1 dynamic, 2 cacheable
	std::uint8_t valid = 0;
};

struct DynamicLengthExpressionResolution {
#if GEA_EMBEDDED_UI_STATE_DYNAMIC_INIT
	ResolvedCssLength value;
	std::uint16_t handle;
	std::int16_t nodeId;
	std::uint8_t axis;
	std::uint8_t valid;

	// Runtime initialization keeps the nonzero sentinels out of .data (see
	// state_init.h); noinline stops the compiler folding them back in.
	__attribute__((noinline)) DynamicLengthExpressionResolution()
	{
		value = ResolvedCssLength{};
		handle = kNoCompiledCssLengthExpression;
		nodeId = -1;
		axis = 0;
		valid = 0;
	}
#else
	ResolvedCssLength value;
	std::uint16_t handle = kNoCompiledCssLengthExpression;
	std::int16_t nodeId = -1;
	std::uint8_t axis = 0;
	std::uint8_t valid = 0;
#endif
};

constexpr std::uint8_t kDynamicLengthExpressionResolutionCacheSize = 64;

std::vector<StaticLengthExpressionResolution> &staticLengthExpressionResolutionCache()
{
	static std::vector<StaticLengthExpressionResolution> list;
	return list;
}

DynamicLengthExpressionResolution (&dynamicLengthExpressionResolutionCache())[kDynamicLengthExpressionResolutionCacheSize]
{
	static DynamicLengthExpressionResolution cache[kDynamicLengthExpressionResolutionCacheSize];
	return cache;
}

std::uint8_t &dynamicLengthExpressionResolutionCacheCursor()
{
	static std::uint8_t cursor = 0;
	return cursor;
}

void clearDynamicLengthExpressionResolutionCache()
{
	for (DynamicLengthExpressionResolution &entry : dynamicLengthExpressionResolutionCache())
		entry.valid = 0;
	dynamicLengthExpressionResolutionCacheCursor() = 0;
}

void clearStaticLengthExpressionResolutionCache()
{
	staticLengthExpressionResolutionCache().clear();
	clearDynamicLengthExpressionResolutionCache();
}

void clearCompiledCssLengthExpressions()
{
	compiledCssLengthExpressions().clear();
	clearStaticLengthExpressionResolutionCache();
}

std::unordered_map<std::string, CssLengthSpec> &compiledCssLengthCache()
{
	static std::unordered_map<std::string, CssLengthSpec> cache;
	return cache;
}

void clearCompiledCssLengthCache()
{
	compiledCssLengthCache().clear();
}

std::unordered_map<std::string, CachedCssColor> &compiledCssColorCache()
{
	static std::unordered_map<std::string, CachedCssColor> cache;
	return cache;
}

void clearCompiledCssColorCache()
{
	compiledCssColorCache().clear();
}

bool compiledLinearGradientNeedsTextFallback(const CssCompiledLinearGradient &gradient)
{
	(void)gradient;
	return false;
}

bool compiledRadialGradientNeedsTextFallback(const CssCompiledRadialGradient &gradient)
{
	(void)gradient;
	return false;
}

bool compiledBackgroundNeedsTextFallback(const CssCompiledBackground &background)
{
	return compiledLinearGradientNeedsTextFallback(background.gradient) ||
	       (background.hasOverlayGradient &&
	        compiledLinearGradientNeedsTextFallback(background.overlayGradient)) ||
	       (background.hasRadialGradient &&
	        compiledRadialGradientNeedsTextFallback(background.radialGradient));
}

bool compiledCssValueCanSkipRuleText(std::uint16_t handle)
{
	const auto &values = compiledCssValues();
	if (handle >= values.size()) return false;
	const CssCompiledValue &compiled = values[handle];
	switch (compiled.kind) {
	case CssCompiledKind::None:
		return false;
	case CssCompiledKind::Background: {
		const auto &backgrounds = compiledCssBackgrounds();
		const std::uint16_t backgroundHandle = static_cast<std::uint16_t>(compiled.values[0]);
		if (backgroundHandle >= backgrounds.size()) return false;
		return !compiledBackgroundNeedsTextFallback(backgrounds[backgroundHandle]);
	}
	default:
		return true;
	}
}

bool compiledCssValueCanSkipKeyframeText(std::uint16_t handle)
{
	const auto &values = compiledCssValues();
	if (handle >= values.size()) return false;
	const CssCompiledValue &compiled = values[handle];
	switch (compiled.kind) {
	case CssCompiledKind::DirectProperty:
	case CssCompiledKind::Noop:
	case CssCompiledKind::Opacity:
	case CssCompiledKind::Transform:
	case CssCompiledKind::FilterBlur:
		return true;
	case CssCompiledKind::Rotate:
		return compiled.declaration == CssDeclarationId::Rotate;
	case CssCompiledKind::Scale:
		return compiled.declaration == CssDeclarationId::Scale;
	case CssCompiledKind::Color:
		return compiled.declaration == CssDeclarationId::Background ||
		       compiled.declaration == CssDeclarationId::Color;
	case CssCompiledKind::ColorVar:
		return compiled.declaration == CssDeclarationId::Background ||
		       compiled.declaration == CssDeclarationId::Color;
	case CssCompiledKind::Length:
	case CssCompiledKind::Size:
	case CssCompiledKind::PositionOffset:
		return compiled.declaration == CssDeclarationId::Width ||
		       compiled.declaration == CssDeclarationId::Height ||
		       compiled.declaration == CssDeclarationId::Left ||
		       compiled.declaration == CssDeclarationId::Top;
	default:
		return false;
	}
}

CssRule makeCssRule(CssRule::SelectorType selectorType,
                    CssRule::PseudoElement pseudoElement,
                    CssText selector,
                    CssText property,
	CssText value,
	CssText media)
{
	const CssDeclarationId declaration = classifyDeclaration(property);
	const CssAtomId selectorAtom = selectorType == CssRule::SelectorType::Class ? atomForText(selector) : kInvalidCssAtom;
	const int16_t selectorTagId = selectorType == CssRule::SelectorType::Element ? internTag(selector.c_str()) : -1;
	const std::uint16_t selectorPlan = selectorType == CssRule::SelectorType::Selector ? compileSelectorPlan(selector) : kNoSelectorPlan;
	const CssAtomId propertyAtom = declaration == CssDeclarationId::Custom
	    ? atomForText(property)
	    : kInvalidCssAtom;
	const std::uint16_t compiledValue = declaration == CssDeclarationId::Custom
	    ? compileCustomPropertyValue(value)
	    : compileCssValue(declaration, value);
	const std::uint16_t compiledAnimationSpec = declaration == CssDeclarationId::Animation
	    ? compileCssAnimationSpec(value)
	    : kNoCompiledCssAnimationSpec;
	const std::uint16_t mediaPlan = compileMediaConditionPlan(media);
	const bool keepValueText =
	    declaration == CssDeclarationId::Custom ||
	    (declaration == CssDeclarationId::Animation && compiledAnimationSpec == kNoCompiledCssAnimationSpec) ||
	    (declaration != CssDeclarationId::Animation &&
	     !compiledCssValueCanSkipRuleText(compiledValue));
	const bool keepPropertyText =
	    keepValueText &&
	    declaration != CssDeclarationId::Custom &&
	    declaration != CssDeclarationId::Animation;
	const std::uint16_t propertyText = keepPropertyText
	    ? storeCssRuleText(std::move(property))
	    : kNoCssRuleText;
	const std::uint16_t valueText = keepValueText
	    ? storeCssRuleText(std::move(value))
	    : kNoCssRuleText;
	const std::uint16_t mediaText = mediaPlan == kNoMediaConditionPlan
	    ? storeCssRuleText(std::move(media))
	    : kNoCssRuleText;
	CssRule rule{selectorType, pseudoElement, rulePropertyKind(declaration), declaration, compiledValue,
	             compiledAnimationSpec,
	             selectorAtom, propertyAtom, selectorTagId, selectorPlan, mediaPlan,
	             propertyText, valueText, mediaText};
	return rule;
}

CssRule::SelectorType cssRuleSelectorTypeForStaticKind(StaticStyleSelectorKind kind)
{
	switch (kind) {
	case StaticStyleSelectorKind::Class: return CssRule::SelectorType::Class;
	case StaticStyleSelectorKind::Element: return CssRule::SelectorType::Element;
	case StaticStyleSelectorKind::Selector: return CssRule::SelectorType::Selector;
	}
	return CssRule::SelectorType::Selector;
}

CssDeclarationId declarationForStaticColorProperty(StaticStyleColorProperty property)
{
	switch (property) {
	case StaticStyleColorProperty::Color: return CssDeclarationId::Color;
	case StaticStyleColorProperty::Background: return CssDeclarationId::Background;
	case StaticStyleColorProperty::ActiveBackground: return CssDeclarationId::ActiveBackgroundColor;
	case StaticStyleColorProperty::Border: return CssDeclarationId::BorderColor;
	case StaticStyleColorProperty::BorderTop: return CssDeclarationId::BorderTopColor;
	case StaticStyleColorProperty::BorderRight: return CssDeclarationId::BorderRightColor;
	case StaticStyleColorProperty::BorderBottom: return CssDeclarationId::BorderBottomColor;
	case StaticStyleColorProperty::BorderLeft: return CssDeclarationId::BorderLeftColor;
	}
	return CssDeclarationId::Color;
}

CssLengthUnit cssLengthUnitForStatic(StaticStyleLengthUnit unit)
{
	switch (unit) {
	case StaticStyleLengthUnit::Raw: return CssLengthUnit::Raw;
	case StaticStyleLengthUnit::Px: return CssLengthUnit::Px;
	case StaticStyleLengthUnit::Percent: return CssLengthUnit::Percent;
	case StaticStyleLengthUnit::Vw: return CssLengthUnit::Vw;
	case StaticStyleLengthUnit::Vh: return CssLengthUnit::Vh;
	case StaticStyleLengthUnit::Vmin: return CssLengthUnit::Vmin;
	case StaticStyleLengthUnit::Vmax: return CssLengthUnit::Vmax;
	case StaticStyleLengthUnit::Dvw: return CssLengthUnit::Dvw;
	case StaticStyleLengthUnit::Dvh: return CssLengthUnit::Dvh;
	case StaticStyleLengthUnit::Auto: return CssLengthUnit::Auto;
	case StaticStyleLengthUnit::Expression: return CssLengthUnit::Expression;
	}
	return CssLengthUnit::Invalid;
}

CssLengthSpec cssLengthSpecForStatic(StaticStyleLengthSpec spec)
{
	CssLengthSpec out;
	out.unit = cssLengthUnitForStatic(spec.unit);
	out.value = spec.value;
	return out;
}

CssLengthExpressionKind cssLengthExpressionKindForStatic(StaticStyleLengthExpressionKind kind)
{
	switch (kind) {
	case StaticStyleLengthExpressionKind::Add: return CssLengthExpressionKind::Add;
	case StaticStyleLengthExpressionKind::Subtract: return CssLengthExpressionKind::Subtract;
	case StaticStyleLengthExpressionKind::Multiply: return CssLengthExpressionKind::Multiply;
	case StaticStyleLengthExpressionKind::Divide: return CssLengthExpressionKind::Divide;
	case StaticStyleLengthExpressionKind::Min: return CssLengthExpressionKind::Min;
	case StaticStyleLengthExpressionKind::Max: return CssLengthExpressionKind::Max;
	case StaticStyleLengthExpressionKind::Clamp: return CssLengthExpressionKind::Clamp;
	case StaticStyleLengthExpressionKind::Var: return CssLengthExpressionKind::Var;
	}
	return CssLengthExpressionKind::Add;
}

CssDeclarationId declarationForStaticLengthProperty(StaticStyleLengthProperty property)
{
	switch (property) {
	case StaticStyleLengthProperty::Gap: return CssDeclarationId::Gap;
	case StaticStyleLengthProperty::Width: return CssDeclarationId::Width;
	case StaticStyleLengthProperty::Height: return CssDeclarationId::Height;
	case StaticStyleLengthProperty::MinWidth: return CssDeclarationId::MinWidth;
	case StaticStyleLengthProperty::MinHeight: return CssDeclarationId::MinHeight;
	case StaticStyleLengthProperty::MaxWidth: return CssDeclarationId::MaxWidth;
	case StaticStyleLengthProperty::MaxHeight: return CssDeclarationId::MaxHeight;
	case StaticStyleLengthProperty::FlexBasis: return CssDeclarationId::FlexBasis;
	case StaticStyleLengthProperty::PaddingTop: return CssDeclarationId::PaddingTop;
	case StaticStyleLengthProperty::PaddingRight: return CssDeclarationId::PaddingRight;
	case StaticStyleLengthProperty::PaddingBottom: return CssDeclarationId::PaddingBottom;
	case StaticStyleLengthProperty::PaddingLeft: return CssDeclarationId::PaddingLeft;
	case StaticStyleLengthProperty::MarginTop: return CssDeclarationId::MarginTop;
	case StaticStyleLengthProperty::MarginRight: return CssDeclarationId::MarginRight;
	case StaticStyleLengthProperty::MarginBottom: return CssDeclarationId::MarginBottom;
	case StaticStyleLengthProperty::MarginLeft: return CssDeclarationId::MarginLeft;
	case StaticStyleLengthProperty::BorderWidth: return CssDeclarationId::BorderWidth;
	case StaticStyleLengthProperty::BorderTopWidth: return CssDeclarationId::BorderTopWidth;
	case StaticStyleLengthProperty::BorderRightWidth: return CssDeclarationId::BorderRightWidth;
	case StaticStyleLengthProperty::BorderBottomWidth: return CssDeclarationId::BorderBottomWidth;
	case StaticStyleLengthProperty::BorderLeftWidth: return CssDeclarationId::BorderLeftWidth;
	case StaticStyleLengthProperty::FontSize: return CssDeclarationId::FontSize;
	case StaticStyleLengthProperty::Perspective: return CssDeclarationId::Perspective;
	case StaticStyleLengthProperty::MaskImage: return CssDeclarationId::MaskImage;
	case StaticStyleLengthProperty::Top: return CssDeclarationId::Top;
	case StaticStyleLengthProperty::Right: return CssDeclarationId::Right;
	case StaticStyleLengthProperty::Bottom: return CssDeclarationId::Bottom;
	case StaticStyleLengthProperty::Left: return CssDeclarationId::Left;
	}
	return CssDeclarationId::Unknown;
}

CssDeclarationId declarationForStaticBorderRadiusCorner(StaticStyleBorderRadiusCorner corner)
{
	switch (corner) {
	case StaticStyleBorderRadiusCorner::TopLeft: return CssDeclarationId::BorderTopLeftRadius;
	case StaticStyleBorderRadiusCorner::TopRight: return CssDeclarationId::BorderTopRightRadius;
	case StaticStyleBorderRadiusCorner::BottomRight: return CssDeclarationId::BorderBottomRightRadius;
	case StaticStyleBorderRadiusCorner::BottomLeft: return CssDeclarationId::BorderBottomLeftRadius;
	}
	return CssDeclarationId::Unknown;
}

CssCompiledKind compiledKindForStaticLengthProperty(StaticStyleLengthProperty property)
{
	switch (property) {
	case StaticStyleLengthProperty::Width:
	case StaticStyleLengthProperty::Height:
		return CssCompiledKind::Size;
	case StaticStyleLengthProperty::Top:
	case StaticStyleLengthProperty::Right:
	case StaticStyleLengthProperty::Bottom:
	case StaticStyleLengthProperty::Left:
		return CssCompiledKind::PositionOffset;
	case StaticStyleLengthProperty::FlexBasis:
		return CssCompiledKind::FlexBasis;
	default:
		return CssCompiledKind::Length;
	}
}

std::uint16_t storeStaticLengthCompiledValue(StaticStyleLengthProperty property,
                                             StaticStyleLengthSpec length)
{
	auto &list = compiledCssValues();
	if (list.size() >= kNoCompiledCssValue) return kNoCompiledCssValue;
	const CssDeclarationId declaration = declarationForStaticLengthProperty(property);
	const CssLengthUnit lengthUnit = cssLengthUnitForStatic(length.unit);
	if (declaration == CssDeclarationId::Unknown || lengthUnit == CssLengthUnit::Invalid)
		return kNoCompiledCssValue;
	CssCompiledValue compiled;
	compiled.kind = compiledKindForStaticLengthProperty(property);
	compiled.declaration = declaration;
	compiled.lengths[0] = cssLengthSpecForStatic(length);
	if (compiled.kind == CssCompiledKind::FlexBasis)
		compiled.aux = lengthUnit == CssLengthUnit::Percent ? 0 : 1;
	list.push_back(compiled);
	return static_cast<std::uint16_t>(list.size() - 1);
}

std::uint16_t storeStaticBorderRadiusCompiledValue(CssDeclarationId declaration,
                                                   StaticStyleLengthSpec topLeft,
                                                   StaticStyleLengthSpec topRight,
                                                   StaticStyleLengthSpec bottomRight,
                                                   StaticStyleLengthSpec bottomLeft)
{
	auto &list = compiledCssValues();
	if (list.size() >= kNoCompiledCssValue) return kNoCompiledCssValue;
	if (declaration == CssDeclarationId::Unknown) return kNoCompiledCssValue;
	if (cssLengthUnitForStatic(topLeft.unit) == CssLengthUnit::Invalid ||
	    cssLengthUnitForStatic(topRight.unit) == CssLengthUnit::Invalid ||
	    cssLengthUnitForStatic(bottomRight.unit) == CssLengthUnit::Invalid ||
	    cssLengthUnitForStatic(bottomLeft.unit) == CssLengthUnit::Invalid)
		return kNoCompiledCssValue;
	CssCompiledValue compiled;
	compiled.kind = CssCompiledKind::BorderRadius;
	compiled.declaration = declaration;
	compiled.lengths[0] = cssLengthSpecForStatic(topLeft);
	compiled.lengths[1] = cssLengthSpecForStatic(topRight);
	compiled.lengths[2] = cssLengthSpecForStatic(bottomRight);
	compiled.lengths[3] = cssLengthSpecForStatic(bottomLeft);
	list.push_back(compiled);
	return static_cast<std::uint16_t>(list.size() - 1);
}

std::uint16_t storeStaticLengthCompiledValue(StaticStyleLengthProperty property,
                                             StaticStyleLengthUnit unit,
                                             float value)
{
	return storeStaticLengthCompiledValue(property, StaticStyleLengthSpec{unit, value});
}

std::uint16_t storeStaticTransformCompiledValue(std::uint16_t flags,
                                                int rotateX,
                                                int rotateY,
                                                int rotateZ,
                                                StaticStyleLengthSpec translateX,
                                                StaticStyleLengthSpec translateY,
                                                StaticStyleLengthSpec translateZ,
                                                int scaleX,
                                                int scaleY)
{
	auto &list = compiledCssValues();
	if (list.size() >= kNoCompiledCssValue) return kNoCompiledCssValue;
	CssCompiledValue compiled;
	compiled.kind = CssCompiledKind::Transform;
	compiled.declaration = CssDeclarationId::Transform;
	compiled.flags = flags;
	compiled.values[0] = rotateX;
	compiled.values[1] = rotateY;
	compiled.values[2] = rotateZ;
	compiled.lengths[0] = cssLengthSpecForStatic(translateX);
	compiled.lengths[1] = cssLengthSpecForStatic(translateY);
	compiled.lengths[2] = cssLengthSpecForStatic(translateZ);
	compiled.values[8] = scaleX;
	compiled.values[9] = scaleY;
	list.push_back(compiled);
	return static_cast<std::uint16_t>(list.size() - 1);
}

std::uint16_t storeStaticFilterBlurCompiledValue(StaticStyleLengthSpec radius)
{
	auto &list = compiledCssValues();
	if (list.size() >= kNoCompiledCssValue) return kNoCompiledCssValue;
	CssCompiledValue compiled;
	compiled.kind = CssCompiledKind::FilterBlur;
	compiled.declaration = CssDeclarationId::Filter;
	compiled.aux = 1;
	compiled.lengths[0] = cssLengthSpecForStatic(radius);
	list.push_back(compiled);
	return static_cast<std::uint16_t>(list.size() - 1);
}

std::uint16_t storeStaticBoxShadowNoneCompiledValue()
{
	auto &list = compiledCssValues();
	if (list.size() >= kNoCompiledCssValue) return kNoCompiledCssValue;
	CssCompiledValue compiled;
	compiled.kind = CssCompiledKind::BoxShadow;
	compiled.declaration = CssDeclarationId::BoxShadow;
	compiled.aux = 0;
	list.push_back(compiled);
	return static_cast<std::uint16_t>(list.size() - 1);
}

CssDeclarationId declarationForStaticOriginProperty(StaticStyleOriginProperty property)
{
	switch (property) {
	case StaticStyleOriginProperty::TransformOrigin: return CssDeclarationId::TransformOrigin;
	case StaticStyleOriginProperty::PerspectiveOrigin: return CssDeclarationId::PerspectiveOrigin;
	}
	return CssDeclarationId::Unknown;
}

std::uint16_t storeStaticOriginCompiledValue(CssDeclarationId declaration, int xPermille, int yPermille)
{
	auto &list = compiledCssValues();
	if (list.size() >= kNoCompiledCssValue) return kNoCompiledCssValue;
	if (declaration == CssDeclarationId::Unknown) return kNoCompiledCssValue;
	CssCompiledValue compiled;
	compiled.kind = CssCompiledKind::OriginPair;
	compiled.declaration = declaration;
	compiled.values[0] = xPermille;
	compiled.values[1] = yPermille;
	list.push_back(compiled);
	return static_cast<std::uint16_t>(list.size() - 1);
}

CssDeclarationId declarationForStaticGridTemplateProperty(StaticStyleGridTemplateProperty property)
{
	switch (property) {
	case StaticStyleGridTemplateProperty::Columns: return CssDeclarationId::GridTemplateColumns;
	case StaticStyleGridTemplateProperty::Rows: return CssDeclarationId::GridTemplateRows;
	}
	return CssDeclarationId::Unknown;
}

std::uint16_t storeStaticGridTemplateCompiledValue(CssDeclarationId declaration,
                                                   std::initializer_list<StaticStyleGridTemplateTrack> tracks)
{
	auto &values = compiledCssValues();
	auto &grids = compiledCssGridTemplates();
	if (values.size() >= kNoCompiledCssValue || grids.size() >= kNoCompiledCssGridTemplate)
		return kNoCompiledCssValue;
	if (declaration == CssDeclarationId::Unknown) return kNoCompiledCssValue;

	CssCompiledGridTemplate grid;
	for (const StaticStyleGridTemplateTrack &staticTrack : tracks) {
		if (grid.count >= kMaxGridTracks) break;
		CssCompiledGridTrack track;
		track.type = static_cast<std::int8_t>(std::clamp(staticTrack.type, 0, 2));
		track.value = static_cast<std::int16_t>(std::clamp(staticTrack.value, -32768, 32767));
		track.length = cssLengthSpecForStatic(staticTrack.length);
		grid.tracks[grid.count++] = track;
	}

	grids.push_back(grid);
	CssCompiledValue compiled;
	compiled.kind = CssCompiledKind::GridTemplate;
	compiled.declaration = declaration;
	compiled.values[0] = static_cast<std::int32_t>(grids.size() - 1);
	values.push_back(compiled);
	return static_cast<std::uint16_t>(values.size() - 1);
}

CssRule makeStaticCompiledCssRule(StaticStyleSelectorKind selectorKind,
                                  const char *selector,
                                  CssRuleProperty propertyKind,
                                  CssDeclarationId declaration,
                                  std::uint16_t compiledValue,
                                  const char *media)
{
	const CssRule::SelectorType selectorType = cssRuleSelectorTypeForStaticKind(selectorKind);
	CssRule::PseudoElement pseudo = CssRule::PseudoElement::None;
	CssText selectorText = CssText::literal(selector);
	if (selectorType == CssRule::SelectorType::Selector) {
		const char *raw = selector ? selector : "";
		const SelectorTextSlice selectorSlice = selectorTextWithoutPseudo(raw, std::strlen(raw));
		pseudo = selectorSlice.pseudo;
		selectorText = CssText::view(selectorSlice.data, selectorSlice.length);
	}
	const CssAtomId selectorAtom = selectorType == CssRule::SelectorType::Class
	    ? atomForText(selectorText)
	    : kInvalidCssAtom;
	const int16_t selectorTagId = selectorType == CssRule::SelectorType::Element
	    ? internTag(selectorText.c_str())
	    : -1;
	const std::uint16_t selectorPlan = selectorType == CssRule::SelectorType::Selector
	    ? compileSelectorPlan(selectorText)
	    : kNoSelectorPlan;
	CssText mediaTextValue = CssText::literal(media);
	const std::uint16_t mediaPlan = compileMediaConditionPlan(mediaTextValue);
	const std::uint16_t storedMediaText = mediaPlan == kNoMediaConditionPlan
	    ? storeCssRuleText(std::move(mediaTextValue))
	    : kNoCssRuleText;
	CssRule rule{selectorType,
	             pseudo,
	             propertyKind,
	             declaration,
	             compiledValue,
	             kNoCompiledCssAnimationSpec,
	             selectorAtom,
	             kInvalidCssAtom,
	             selectorTagId,
	             selectorPlan,
	             mediaPlan,
	             kNoCssRuleText,
	             kNoCssRuleText,
	             storedMediaText};
	return rule;
}

CssRule makeDirectPropertyCssRule(StaticStyleSelectorKind selectorKind,
                                  const char *selector,
                                  Property property,
                                  int value,
                                  const char *media)
{
	return makeStaticCompiledCssRule(selectorKind,
	                                 selector,
	                                 CssRuleProperty::Other,
	                                 CssDeclarationId::Ignored,
	                                 storeDirectPropertyCompiledValue(property, value),
	                                 media);
}

CssRule makeDirectPropertyGroupCssRule(StaticStyleSelectorKind selectorKind,
                                       const char *selector,
                                       std::initializer_list<StaticStylePropertyValue> properties,
                                       const char *media)
{
	return makeStaticCompiledCssRule(selectorKind,
	                                 selector,
	                                 CssRuleProperty::Other,
	                                 CssDeclarationId::Ignored,
	                                 storeDirectPropertyGroupCompiledValue(properties),
	                                 media);
}

CssRule makeStaticColorCssRule(StaticStyleSelectorKind selectorKind,
                               const char *selector,
                               StaticStyleColorProperty property,
                               int r,
                               int g,
                               int b,
                               int a,
                               const char *media)
{
	const CssDeclarationId declaration = declarationForStaticColorProperty(property);
	return makeStaticCompiledCssRule(selectorKind,
	                                 selector,
	                                 rulePropertyKind(declaration),
	                                 declaration,
	                                 storeStaticColorCompiledValue(declaration, r, g, b, a),
	                                 media);
}

CssRule makeStaticColorVarCssRule(StaticStyleSelectorKind selectorKind,
                                  const char *selector,
                                  StaticStyleColorProperty property,
                                  const char *name,
                                  bool hasFallback,
                                  int r,
                                  int g,
                                  int b,
                                  int a,
                                  const char *media)
{
	const CssDeclarationId declaration = declarationForStaticColorProperty(property);
	return makeStaticCompiledCssRule(selectorKind,
	                                 selector,
	                                 rulePropertyKind(declaration),
	                                 declaration,
	                                 storeStaticColorVarCompiledValue(declaration,
	                                                                  name,
	                                                                  hasFallback,
	                                                                  r,
	                                                                  g,
	                                                                  b,
	                                                                  a),
	                                 media);
}

CssRule makeStaticLengthCssRule(StaticStyleSelectorKind selectorKind,
                                const char *selector,
                                StaticStyleLengthProperty property,
                                StaticStyleLengthUnit unit,
                                float value,
                                const char *media)
{
	const CssDeclarationId declaration = declarationForStaticLengthProperty(property);
	return makeStaticCompiledCssRule(selectorKind,
	                                 selector,
	                                 rulePropertyKind(declaration),
	                                 declaration,
	                                 storeStaticLengthCompiledValue(property, unit, value),
	                                 media);
}

CssRule makeStaticLengthSpecCssRule(StaticStyleSelectorKind selectorKind,
                                    const char *selector,
                                    StaticStyleLengthProperty property,
                                    StaticStyleLengthSpec length,
                                    const char *media)
{
	const CssDeclarationId declaration = declarationForStaticLengthProperty(property);
	return makeStaticCompiledCssRule(selectorKind,
	                                 selector,
	                                 rulePropertyKind(declaration),
	                                 declaration,
	                                 storeStaticLengthCompiledValue(property, length),
	                                 media);
}

CssRule makeStaticFontFamilyCssRule(StaticStyleSelectorKind selectorKind,
                                    const char *selector,
                                    const char *family,
                                    const char *media)
{
	return makeStaticCompiledCssRule(selectorKind,
	                                 selector,
	                                 rulePropertyKind(CssDeclarationId::FontFamily),
	                                 CssDeclarationId::FontFamily,
	                                 storeStaticFontFamilyCompiledValue(family),
	                                 media);
}

CssRule makeStaticLineHeightCssRule(StaticStyleSelectorKind selectorKind,
                                    const char *selector,
                                    StaticStyleLineHeightKind kind,
                                    StaticStyleLengthSpec value,
                                    const char *media)
{
	return makeStaticCompiledCssRule(selectorKind,
	                                 selector,
	                                 rulePropertyKind(CssDeclarationId::LineHeight),
	                                 CssDeclarationId::LineHeight,
	                                 storeStaticLineHeightCompiledValue(kind, value),
	                                 media);
}

CssRule makeStaticFlexCssRule(StaticStyleSelectorKind selectorKind,
                              const char *selector,
                              int grow,
                              StaticStyleLengthSpec basis,
                              bool hasBasis,
                              const char *media)
{
	return makeStaticCompiledCssRule(selectorKind,
	                                 selector,
	                                 rulePropertyKind(CssDeclarationId::Flex),
	                                 CssDeclarationId::Flex,
	                                 storeStaticFlexCompiledValue(grow, basis, hasBasis),
	                                 media);
}

CssRule makeStaticBorderCssRule(StaticStyleSelectorKind selectorKind,
                                const char *selector,
                                StaticStyleLengthSpec width,
                                int r,
                                int g,
                                int b,
                                int a,
                                const char *media)
{
	return makeStaticCompiledCssRule(selectorKind,
	                                 selector,
	                                 rulePropertyKind(CssDeclarationId::Border),
	                                 CssDeclarationId::Border,
	                                 storeStaticBorderCompiledValue(width, r, g, b, a),
	                                 media);
}

CssRule makeStaticBorderRadiusCssRule(StaticStyleSelectorKind selectorKind,
                                      const char *selector,
                                      StaticStyleLengthSpec topLeft,
                                      StaticStyleLengthSpec topRight,
                                      StaticStyleLengthSpec bottomRight,
                                      StaticStyleLengthSpec bottomLeft,
                                      const char *media)
{
	return makeStaticCompiledCssRule(selectorKind,
	                                 selector,
	                                 rulePropertyKind(CssDeclarationId::BorderRadius),
	                                 CssDeclarationId::BorderRadius,
	                                 storeStaticBorderRadiusCompiledValue(CssDeclarationId::BorderRadius,
	                                                                      topLeft,
	                                                                      topRight,
	                                                                      bottomRight,
	                                                                      bottomLeft),
	                                 media);
}

CssRule makeStaticBorderRadiusCornerCssRule(StaticStyleSelectorKind selectorKind,
                                            const char *selector,
                                            StaticStyleBorderRadiusCorner corner,
                                            StaticStyleLengthSpec radius,
                                            const char *media)
{
	const CssDeclarationId declaration = declarationForStaticBorderRadiusCorner(corner);
	return makeStaticCompiledCssRule(selectorKind,
	                                 selector,
	                                 rulePropertyKind(declaration),
	                                 declaration,
	                                 storeStaticBorderRadiusCompiledValue(declaration,
	                                                                      radius,
	                                                                      radius,
	                                                                      radius,
	                                                                      radius),
	                                 media);
}

CssRule makeStaticFilterBlurCssRule(StaticStyleSelectorKind selectorKind,
                                    const char *selector,
                                    StaticStyleLengthSpec radius,
                                    const char *media)
{
	return makeStaticCompiledCssRule(selectorKind,
	                                 selector,
	                                 rulePropertyKind(CssDeclarationId::Filter),
	                                 CssDeclarationId::Filter,
	                                 storeStaticFilterBlurCompiledValue(radius),
	                                 media);
}

CssRule makeStaticBoxShadowNoneCssRule(StaticStyleSelectorKind selectorKind,
                                       const char *selector,
                                       const char *media)
{
	return makeStaticCompiledCssRule(selectorKind,
	                                 selector,
	                                 rulePropertyKind(CssDeclarationId::BoxShadow),
	                                 CssDeclarationId::BoxShadow,
	                                 storeStaticBoxShadowNoneCompiledValue(),
	                                 media);
}

CssRule makeStaticBackgroundCssRule(StaticStyleSelectorKind selectorKind,
                                    const char *selector,
                                    StaticStyleLinearGradient gradient,
                                    StaticStyleLinearGradient overlayGradient,
                                    bool hasOverlayGradient,
                                    StaticStyleBackgroundGridLine gridX,
                                    StaticStyleBackgroundGridLine gridY,
                                    const char *media)
{
	return makeStaticCompiledCssRule(selectorKind,
	                                 selector,
	                                 rulePropertyKind(CssDeclarationId::Background),
	                                 CssDeclarationId::Background,
	                                 storeStaticBackgroundCompiledValue(gradient,
	                                                                    overlayGradient,
	                                                                    hasOverlayGradient,
	                                                                    gridX,
	                                                                    gridY),
	                                 media);
}

CssRule makeStaticBackgroundFullCssRule(StaticStyleSelectorKind selectorKind,
                                        const char *selector,
                                        StaticStyleLinearGradientRef gradient,
                                        StaticStyleLinearGradientRef overlayGradient,
                                        bool hasOverlayGradient,
                                        StaticStyleRadialGradientRef radialGradient,
                                        StaticStyleBackgroundGridLine gridX,
                                        StaticStyleBackgroundGridLine gridY,
                                        const char *media)
{
	return makeStaticCompiledCssRule(selectorKind,
	                                 selector,
	                                 rulePropertyKind(CssDeclarationId::Background),
	                                 CssDeclarationId::Background,
	                                 storeStaticBackgroundFullCompiledValue(gradient,
	                                                                        overlayGradient,
	                                                                        hasOverlayGradient,
	                                                                        radialGradient,
	                                                                        gridX,
	                                                                        gridY),
	                                 media);
}

CssRule makeStaticBackgroundSizeCssRule(StaticStyleSelectorKind selectorKind,
                                        const char *selector,
                                        StaticStyleLengthSpec stepX,
                                        StaticStyleLengthSpec stepY,
                                        const char *media)
{
	return makeStaticCompiledCssRule(selectorKind,
	                                 selector,
	                                 rulePropertyKind(CssDeclarationId::BackgroundSize),
	                                 CssDeclarationId::BackgroundSize,
	                                 storeStaticBackgroundSizeCompiledValue(stepX, stepY),
	                                 media);
}

CssRule makeStaticCustomLengthCssRule(StaticStyleSelectorKind selectorKind,
                                      const char *selector,
                                      const char *name,
                                      StaticStyleLengthSpec length,
                                      const char *media)
{
	CssRule rule = makeStaticCompiledCssRule(selectorKind,
	                                        selector,
	                                        CssRuleProperty::Custom,
	                                        CssDeclarationId::Custom,
	                                        storeStaticCustomLengthCompiledValue(length),
	                                        media);
	rule.propertyAtom = internCssAtom(name ? name : "");
	return rule;
}

CssRule makeStaticCustomColorCssRule(StaticStyleSelectorKind selectorKind,
                                     const char *selector,
                                     const char *name,
                                     int r,
                                     int g,
                                     int b,
                                     int a,
                                     const char *media)
{
	CssRule rule = makeStaticCompiledCssRule(selectorKind,
	                                        selector,
	                                        CssRuleProperty::Custom,
	                                        CssDeclarationId::Custom,
	                                        storeStaticColorCompiledValue(CssDeclarationId::Custom, r, g, b, a),
	                                        media);
	rule.propertyAtom = internCssAtom(name ? name : "");
	return rule;
}

CssRule makeStaticTransformCssRule(StaticStyleSelectorKind selectorKind,
                                   const char *selector,
                                   std::uint16_t flags,
                                   int rotateX,
                                   int rotateY,
                                   int rotateZ,
                                   StaticStyleLengthSpec translateX,
                                   StaticStyleLengthSpec translateY,
                                   StaticStyleLengthSpec translateZ,
                                   int scaleX,
                                   int scaleY,
                                   const char *media)
{
	return makeStaticCompiledCssRule(selectorKind,
	                                 selector,
	                                 rulePropertyKind(CssDeclarationId::Transform),
	                                 CssDeclarationId::Transform,
	                                 storeStaticTransformCompiledValue(flags,
	                                                                   rotateX,
	                                                                   rotateY,
	                                                                   rotateZ,
	                                                                   translateX,
	                                                                   translateY,
	                                                                   translateZ,
	                                                                   scaleX,
	                                                                   scaleY),
	                                 media);
}

CssRule makeStaticOriginCssRule(StaticStyleSelectorKind selectorKind,
                                const char *selector,
                                StaticStyleOriginProperty property,
                                int xPermille,
                                int yPermille,
                                const char *media)
{
	const CssDeclarationId declaration = declarationForStaticOriginProperty(property);
	return makeStaticCompiledCssRule(selectorKind,
	                                 selector,
	                                 rulePropertyKind(declaration),
	                                 declaration,
	                                 storeStaticOriginCompiledValue(declaration, xPermille, yPermille),
	                                 media);
}

CssRule makeStaticGridTemplateCssRule(StaticStyleSelectorKind selectorKind,
                                      const char *selector,
                                      StaticStyleGridTemplateProperty property,
                                      std::initializer_list<StaticStyleGridTemplateTrack> tracks,
                                      const char *media)
{
	const CssDeclarationId declaration = declarationForStaticGridTemplateProperty(property);
	return makeStaticCompiledCssRule(selectorKind,
	                                 selector,
	                                 rulePropertyKind(declaration),
	                                 declaration,
	                                 storeStaticGridTemplateCompiledValue(declaration, tracks),
	                                 media);
}

CssKeyframeRule makeCssKeyframeRule(CssText name, int offsetPermille, CssText property, CssText value)
{
	const CssDeclarationId declaration = classifyDeclaration(property);
	const CssAtomId nameAtom = atomForText(name);
	const std::uint16_t compiledValue = compileCssValue(declaration, value);
	const std::uint16_t valueText = compiledCssValueCanSkipKeyframeText(compiledValue)
	    ? kNoCssRuleText
	    : storeCssRuleText(std::move(value));
	CssKeyframeRule rule{nameAtom, offsetPermille, rulePropertyKind(declaration),
	                     declaration, compiledValue, valueText};
	return rule;
}

CssKeyframeRule makeStaticCompiledKeyframeRule(const char *name,
                                               int offsetPermille,
                                               CssRuleProperty propertyKind,
                                               CssDeclarationId declaration,
                                               std::uint16_t compiledValue)
{
	return CssKeyframeRule{internCssAtom(name ? name : ""),
	                       offsetPermille,
	                       propertyKind,
	                       declaration,
	                       compiledValue,
	                       kNoCssRuleText};
}

CssKeyframeRule makeStaticPropertyKeyframeRule(const char *name,
                                               int offsetPermille,
                                               Property property,
                                               int value)
{
	return makeStaticCompiledKeyframeRule(name,
	                                      offsetPermille,
	                                      CssRuleProperty::Other,
	                                      CssDeclarationId::Ignored,
	                                      storeDirectPropertyCompiledValue(property, value));
}

CssKeyframeRule makeStaticColorKeyframeRule(const char *name,
                                            int offsetPermille,
                                            StaticStyleColorProperty property,
                                            int r,
                                            int g,
                                            int b,
                                            int a)
{
	const CssDeclarationId declaration = declarationForStaticColorProperty(property);
	return makeStaticCompiledKeyframeRule(name,
	                                      offsetPermille,
	                                      rulePropertyKind(declaration),
	                                      declaration,
	                                      storeStaticColorCompiledValue(declaration, r, g, b, a));
}

CssKeyframeRule makeStaticColorVarKeyframeRule(const char *name,
                                               int offsetPermille,
                                               StaticStyleColorProperty property,
                                               const char *varName,
                                               bool hasFallback,
                                               int r,
                                               int g,
                                               int b,
                                               int a)
{
	const CssDeclarationId declaration = declarationForStaticColorProperty(property);
	return makeStaticCompiledKeyframeRule(name,
	                                      offsetPermille,
	                                      rulePropertyKind(declaration),
	                                      declaration,
	                                      storeStaticColorVarCompiledValue(declaration,
	                                                                       varName,
	                                                                       hasFallback,
	                                                                       r,
	                                                                       g,
	                                                                       b,
	                                                                       a));
}

CssKeyframeRule makeStaticLengthKeyframeRule(const char *name,
                                             int offsetPermille,
                                             StaticStyleLengthProperty property,
                                             StaticStyleLengthSpec length)
{
	const CssDeclarationId declaration = declarationForStaticLengthProperty(property);
	return makeStaticCompiledKeyframeRule(name,
	                                      offsetPermille,
	                                      rulePropertyKind(declaration),
	                                      declaration,
	                                      storeStaticLengthCompiledValue(property, length));
}

CssKeyframeRule makeStaticFilterBlurKeyframeRule(const char *name,
                                                 int offsetPermille,
                                                 StaticStyleLengthSpec radius)
{
	return makeStaticCompiledKeyframeRule(name,
	                                      offsetPermille,
	                                      CssRuleProperty::Other,
	                                      CssDeclarationId::Filter,
	                                      storeStaticFilterBlurCompiledValue(radius));
}

CssKeyframeRule makeStaticTransformKeyframeRule(const char *name,
                                                int offsetPermille,
                                                std::uint16_t flags,
                                                int rotateX,
                                                int rotateY,
                                                int rotateZ,
                                                StaticStyleLengthSpec translateX,
                                                StaticStyleLengthSpec translateY,
                                                StaticStyleLengthSpec translateZ,
                                                int scaleX,
                                                int scaleY)
{
	return CssKeyframeRule{internCssAtom(name ? name : ""),
	                       offsetPermille,
	                       rulePropertyKind(CssDeclarationId::Transform),
	                       CssDeclarationId::Transform,
	                       storeStaticTransformCompiledValue(flags,
	                                                        rotateX,
	                                                        rotateY,
	                                                        rotateZ,
	                                                        translateX,
	                                                        translateY,
	                                                        translateZ,
	                                                        scaleX,
	                                                        scaleY),
	                       kNoCssRuleText};
}

bool isCustomRuleProperty(const CssRule &rule)
{
	return rule.propertyKind == CssRuleProperty::Custom;
}

// Viewport dimensions are physical framebuffer pixels. CSS `vw`/`vh`
// resolve against those dimensions, while explicit CSS `px` lengths are
// logical pixels scaled by the device pixel ratio below. Unitless values
// remain physical pixels for compatibility with older embedded examples.
int g_viewport_width = 0;
int g_viewport_height = 0;
double g_device_pixel_ratio = 1.0;
// Physical-pixel height reserved at the bottom of the panel for a
// platform overlay drawn outside the document tree (e.g. the geaos
// home button). UI that wants to stay clear of that zone — the
// built-in virtual keyboard — reads this and offsets itself upward.
// Defaults to 0, so targets without such an overlay (ESP32) are
// unaffected.
int g_safe_area_inset_bottom = 0;

double sanitizedDevicePixelRatio(double value)
{
	return std::isfinite(value) && value > 0.0 ? value : 1.0;
}

int roundToInt(double value)
{
	if (!std::isfinite(value)) return 0;
	return static_cast<int>(std::round(value));
}

int cssPixelLength(double value)
{
	return roundToInt(value * g_device_pixel_ratio);
}

int rawNumber(double value)
{
	return roundToInt(value);
}

std::string trimCssValue(const std::string &value)
{
	std::size_t start = 0;
	while (start < value.size() && static_cast<unsigned char>(value[start]) <= ' ') ++start;
	std::size_t end = value.size();
	while (end > start && static_cast<unsigned char>(value[end - 1]) <= ' ') --end;
	return value.substr(start, end - start);
}

bool isTrimmedCssValue(const std::string &value)
{
	return value.empty() ||
	       (static_cast<unsigned char>(value.front()) > ' ' &&
	        static_cast<unsigned char>(value.back()) > ' ');
}

std::string toLowerAscii(std::string value)
{
	for (char &c : value) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
	return value;
}

bool startsWith(const std::string &value, const char *prefix)
{
	return value.rfind(prefix, 0) == 0;
}

std::string functionInner(const std::string &value, const char *name)
{
	const std::string text = trimCssValue(value);
	const std::string prefix = std::string(name) + "(";
	if (!startsWith(text, prefix.c_str()) || text.empty() || text.back() != ')') return std::string();
	return text.substr(prefix.size(), text.size() - prefix.size() - 1);
}

std::vector<std::string> splitTopLevel(const std::string &value, char delimiter)
{
	std::vector<std::string> out;
	std::size_t start = 0;
	int depth = 0;
	for (std::size_t i = 0; i < value.size(); ++i) {
		const char c = value[i];
		if (c == '(') depth++;
		else if (c == ')' && depth > 0) depth--;
		else if (c == delimiter && depth == 0) {
			out.push_back(trimCssValue(value.substr(start, i - start)));
			start = i + 1;
		}
	}
	out.push_back(trimCssValue(value.substr(start)));
	return out;
}

const NodeCustomProperty *lookupCustomPropertyEntry(int nodeId, CssAtomId name)
{
	// Record the dependency against the node currently being recomputed (not nodeId:
	// pseudo-element resolution looks up from the pseudo node but the dependency
	// belongs to its owner, which is g_recordingNode). Record on every lookup, hit or
	// miss — a miss that later becomes a hit is also a dependency.
	recordCustomPropRef(name);
	if (name == kInvalidCssAtom) return nullptr;
	const NodeCustomProperty *cached = nullptr;
	if (g_customPropertyLookupCache.lookup(nodeId, name, cached)) return cached;
	auto &state = treeState();
	int visited[CustomPropertyLookupCache::kCapacity]{};
	std::uint8_t visitedCount = 0;
	auto rememberVisited = [&](int id) {
		if (visitedCount >= CustomPropertyLookupCache::kCapacity) return;
		visited[visitedCount++] = id;
	};
	auto storeVisited = [&](const NodeCustomProperty *entry) {
		for (std::uint8_t i = 0; i < visitedCount; ++i)
			g_customPropertyLookupCache.store(visited[i], name, entry);
	};
	for (int id = nodeId; id >= 0 && id < state.nodeCount; id = state.nodes[id].parent) {
		if (id != nodeId && g_customPropertyLookupCache.lookup(id, name, cached)) {
			storeVisited(cached);
			return cached;
		}
		rememberVisited(id);
		if (const NodeRareData *rd = rareDataFor(id)) {
			if (const auto *entry = rd->customProperties.getEntry(name)) {
				storeVisited(entry);
				return entry;
			}
		}
	}
	storeVisited(nullptr);
	return nullptr;
}

const std::string *lookupCustomProperty(int nodeId, CssAtomId name)
{
	if (const NodeCustomProperty *entry = lookupCustomPropertyEntry(nodeId, name)) return &entry->value;
	return nullptr;
}

const std::string *lookupCustomProperty(int nodeId, const std::string &name)
{
	return lookupCustomProperty(nodeId, internCssAtom(name));
}

std::string resolveCssVarsForNode(const std::string &rawValue, int nodeId, int depth = 0)
{
	if (depth > 8) return rawValue;
	// Common case: no var() reference. Skip the working-copy allocation and the
	// scan loop; just hand back the trimmed value.
	if (rawValue.find("var(") == std::string::npos) return trimCssValue(rawValue);
	std::string value = rawValue;
	std::size_t search = 0;
	while (true) {
		const std::size_t start = value.find("var(", search);
		if (start == std::string::npos) break;
		std::size_t i = start + 4;
		int parenDepth = 1;
		while (i < value.size() && parenDepth > 0) {
			if (value[i] == '(') parenDepth++;
			else if (value[i] == ')') parenDepth--;
			++i;
		}
		if (parenDepth != 0) break;
		const std::string inner = value.substr(start + 4, i - start - 5);
		const auto parts = splitTopLevel(inner, ',');
		std::string replacement;
		if (!parts.empty()) {
			const std::string name = trimCssValue(parts[0]);
			if (const auto *resolved = lookupCustomProperty(nodeId, name)) {
				replacement = resolveCssVarsForNode(*resolved, nodeId, depth + 1);
			} else if (parts.size() > 1) {
				replacement = resolveCssVarsForNode(parts[1], nodeId, depth + 1);
			}
		}
		value.replace(start, i - start, replacement);
		search = start + replacement.size();
	}
	return trimCssValue(value);
}

std::string resolveCssVarsForNode(const CssText &rawValue, int nodeId, int depth = 0)
{
	if (depth > 8) return rawValue.trimmedStr();
	if (!rawValue.hasVarReference()) return rawValue.trimmedStr();
	return resolveCssVarsForNode(rawValue.str(), nodeId, depth);
}

int percentBasisForNode(int nodeId, LengthAxis axis)
{
	auto &state = treeState();
	int parent = nodeId >= 0 && nodeId < state.nodeCount ? state.nodes[nodeId].parent : -1;
	if (parent >= 0 && parent < state.nodeCount) {
		const auto &p = state.nodes[parent];
		const int padding = axis == LengthAxis::Vertical
			? p.style.padding[0] + p.style.padding[2]
			: p.style.padding[1] + p.style.padding[3];
		auto contentBasis = [padding](int value) {
			value -= padding;
			return value < 0 ? 0 : value;
		};
		if (axis == LengthAxis::Vertical) {
			if (p.layout.height > 0) return contentBasis(p.layout.height);
			if (p.style.height != kUnset) return contentBasis(p.style.height);
		} else {
			if (p.layout.width > 0) return contentBasis(p.layout.width);
			if (p.style.width != kUnset) return contentBasis(p.style.width);
		}
	}
	return axis == LengthAxis::Vertical ? g_viewport_height : g_viewport_width;
}

int parseLengthForNode(const std::string &rawValue, int nodeId, LengthAxis axis);
std::vector<std::string> splitWords(const std::string &value);
std::vector<std::string> splitFunctionAwareWords(const std::string &value);
int parseOriginPart(const std::string &part, int fallback);

int parseCalcExpression(const std::string &expr, int nodeId, LengthAxis axis)
{
	const std::string text = trimCssValue(expr);
	for (char op : {'/', '*', '+', '-'}) {
		int depth = 0;
		for (std::size_t i = 0; i < text.size(); ++i) {
			const char c = text[i];
			if (c == '(') depth++;
			else if (c == ')' && depth > 0) depth--;
			else if (c == op && depth == 0 && i > 0) {
				const int left = parseLengthForNode(text.substr(0, i), nodeId, axis);
				const double right = std::strtod(text.substr(i + 1).c_str(), nullptr);
				if (op == '/') return right == 0.0 ? 0 : roundToInt(static_cast<double>(left) / right);
				if (op == '*') return roundToInt(static_cast<double>(left) * right);
				const int rightLength = parseLengthForNode(text.substr(i + 1), nodeId, axis);
				return op == '+' ? left + rightLength : left - rightLength;
			}
		}
	}
	return parseLengthForNode(text, nodeId, axis);
}

int parseLengthForNode(const std::string &rawValue, int nodeId, LengthAxis axis)
{
#if GEA_RECPROF
	g_profLenCalls++;
	const int64_t _lt = recNow();
	struct LenTimer { int64_t s; ~LenTimer() { g_profLenUs += recNow() - s; } } _lenTimer{_lt};
#endif
	// Fast path: a plain number/length (NOT a var()/calc()/min()/max()/clamp()
	// function) parses without the trimmed + lowercased std::string temporaries the
	// general path below allocates. Functions start with a letter; numeric lengths
	// start with a digit, sign, or dot — so the cheap first-char test routes them.
	// This is the hot case during a full style recompute (px/%/vw values).
	{
		std::size_t b = 0, e = rawValue.size();
		while (b < e && static_cast<unsigned char>(rawValue[b]) <= ' ') ++b;
		while (e > b && static_cast<unsigned char>(rawValue[e - 1]) <= ' ') --e;
		if (b >= e) return 0;
		const char c0 = rawValue[b];
		const bool maybeFunction = (c0 >= 'A' && c0 <= 'Z') || (c0 >= 'a' && c0 <= 'z');
		if (!maybeFunction) {
			char *end = nullptr;
			const double v = std::strtod(rawValue.c_str() + b, &end);
			if (end) {
				while (*end == ' ') ++end;
				if (*end == '\0') return rawNumber(v);
				if (end[0] == 'p' && end[1] == 'x') return cssPixelLength(v);
				if (end[0] == '%') {
					const int basis = percentBasisForNode(nodeId, axis);
					return roundToInt(v * basis / 100.0);
				}
				if (end[0] == 'v' && end[1] == 'm' && end[2] == 'i' && end[3] == 'n' && g_viewport_width > 0 && g_viewport_height > 0) return roundToInt(v * std::min(g_viewport_width, g_viewport_height) / 100.0);
				if (end[0] == 'v' && end[1] == 'm' && end[2] == 'a' && end[3] == 'x' && g_viewport_width > 0 && g_viewport_height > 0) return roundToInt(v * std::max(g_viewport_width, g_viewport_height) / 100.0);
				if (end[0] == 'v' && end[1] == 'w' && g_viewport_width > 0) return roundToInt(v * g_viewport_width / 100.0);
				if (end[0] == 'v' && end[1] == 'h' && g_viewport_height > 0) return roundToInt(v * g_viewport_height / 100.0);
				if (end[0] == 'd' && end[1] == 'v' && end[2] == 'w' && g_viewport_width > 0) return roundToInt(v * g_viewport_width / 100.0);
				if (end[0] == 'd' && end[1] == 'v' && end[2] == 'h' && g_viewport_height > 0) return roundToInt(v * g_viewport_height / 100.0);
			}
			return rawNumber(v);
		}
	}
	const std::string value = trimCssValue(rawValue);
	if (value.empty()) return 0;
	const std::string lower = toLowerAscii(value);
	if (startsWith(lower, "var(")) {
		const auto inner = splitTopLevel(functionInner(value, "var"), ',');
		if (!inner.empty()) {
			if (const auto *resolved = lookupCustomProperty(nodeId, trimCssValue(inner[0])))
				return parseLengthForNode(*resolved, nodeId, axis);
			if (inner.size() > 1) return parseLengthForNode(inner[1], nodeId, axis);
		}
		return 0;
	}
	if (startsWith(lower, "calc(")) return parseCalcExpression(functionInner(value, "calc"), nodeId, axis);
	if (startsWith(lower, "min(") || startsWith(lower, "max(")) {
		const bool isMin = startsWith(lower, "min(");
		const auto parts = splitTopLevel(functionInner(value, isMin ? "min" : "max"), ',');
		if (parts.empty()) return 0;
		int result = parseLengthForNode(parts[0], nodeId, axis);
		for (std::size_t i = 1; i < parts.size(); ++i) {
			const int next = parseLengthForNode(parts[i], nodeId, axis);
			result = isMin ? std::min(result, next) : std::max(result, next);
		}
		return result;
	}
	if (startsWith(lower, "clamp(")) {
		const auto parts = splitTopLevel(functionInner(value, "clamp"), ',');
		if (parts.size() < 3) return parts.empty() ? 0 : parseLengthForNode(parts[0], nodeId, axis);
		const int minValue = parseLengthForNode(parts[0], nodeId, axis);
		const int preferred = parseLengthForNode(parts[1], nodeId, axis);
		const int maxValue = parseLengthForNode(parts[2], nodeId, axis);
		return std::max(minValue, std::min(preferred, maxValue));
	}
	char *end = nullptr;
	double v = std::strtod(value.c_str(), &end);
	if (end) {
		while (*end == ' ') ++end;
		if (*end == '\0') return rawNumber(v);
		if (end[0] == 'p' && end[1] == 'x') return cssPixelLength(v);
		if (end[0] == '%') {
			const int basis = percentBasisForNode(nodeId, axis);
			return roundToInt(v * basis / 100.0);
		}
		if (end[0] == 'v' && end[1] == 'm' && end[2] == 'i' && end[3] == 'n' && g_viewport_width > 0 && g_viewport_height > 0) {
			return roundToInt(v * std::min(g_viewport_width, g_viewport_height) / 100.0);
		}
		if (end[0] == 'v' && end[1] == 'm' && end[2] == 'a' && end[3] == 'x' && g_viewport_width > 0 && g_viewport_height > 0) {
			return roundToInt(v * std::max(g_viewport_width, g_viewport_height) / 100.0);
		}
		if (end[0] == 'v' && end[1] == 'w' && g_viewport_width > 0) {
			return roundToInt(v * g_viewport_width / 100.0);
		}
		if (end[0] == 'v' && end[1] == 'h' && g_viewport_height > 0) {
			return roundToInt(v * g_viewport_height / 100.0);
		}
		if (end[0] == 'd' && end[1] == 'v' && end[2] == 'w' && g_viewport_width > 0) {
			return roundToInt(v * g_viewport_width / 100.0);
		}
		if (end[0] == 'd' && end[1] == 'v' && end[2] == 'h' && g_viewport_height > 0) {
			return roundToInt(v * g_viewport_height / 100.0);
		}
	}
	return rawNumber(v);
}

int parseRightFadeMaskWidth(const std::string &rawValue, int nodeId)
{
	const std::string value = trimCssValue(rawValue);
	const std::string lower = toLowerAscii(value);
	if (lower.empty() || lower == "none") return 0;
	if (lower.find("linear-gradient") == std::string::npos ||
	    lower.find("to right") == std::string::npos ||
	    lower.find("transparent") == std::string::npos) {
		return 0;
	}

	const std::size_t calcStart = lower.find("calc(");
	if (calcStart == std::string::npos) return 0;
	const std::size_t innerStart = calcStart + 5;
	int depth = 1;
	std::size_t end = innerStart;
	for (; end < value.size(); ++end) {
		if (value[end] == '(') depth++;
		else if (value[end] == ')') {
			if (--depth == 0) break;
		}
	}
	if (end <= innerStart || end >= value.size()) return 0;

	const std::string inner = trimCssValue(value.substr(innerStart, end - innerStart));
	const std::string innerLower = toLowerAscii(inner);
	if (innerLower.rfind("100%", 0) != 0) return 0;
	depth = 0;
	for (std::size_t i = 0; i < inner.size(); ++i) {
		const char c = inner[i];
		if (c == '(') depth++;
		else if (c == ')' && depth > 0) depth--;
		else if (c == '-' && depth == 0) {
			return std::max(0, parseLengthForNode(inner.substr(i + 1), nodeId, LengthAxis::Horizontal));
		}
	}
	return 0;
}

int parseLength(const std::string &value)
{
	return parseLengthForNode(value, -1, LengthAxis::None);
}

int currentFontSizeForNode(int nodeId)
{
	auto &state = treeState();
	if (nodeId >= 0 && nodeId < state.nodeCount && state.nodes[nodeId].style.font_size > 0)
		return state.nodes[nodeId].style.font_size;
	return cssPixelLength(16.0);
}

int parseLineHeightForNode(const std::string &rawValue, int nodeId)
{
	const std::string value = trimCssValue(rawValue);
	if (value.empty()) return 0;
	const std::string lower = toLowerAscii(value);
	if (lower == "normal") return 0;
	if (startsWith(lower, "var(")) {
		const auto inner = splitTopLevel(functionInner(value, "var"), ',');
		if (!inner.empty()) {
			if (const auto *resolved = lookupCustomProperty(nodeId, trimCssValue(inner[0])))
				return parseLineHeightForNode(*resolved, nodeId);
			if (inner.size() > 1) return parseLineHeightForNode(inner[1], nodeId);
		}
		return 0;
	}

	char *end = nullptr;
	const double scalar = std::strtod(value.c_str(), &end);
	if (end) {
		while (*end == ' ') ++end;
		if (*end == '\0') return roundToInt(static_cast<double>(currentFontSizeForNode(nodeId)) * scalar);
		if (end[0] == '%') return roundToInt(static_cast<double>(currentFontSizeForNode(nodeId)) * scalar / 100.0);
	}
	return parseLengthForNode(value, nodeId, LengthAxis::Vertical);
}

std::string primaryFontFamily(const std::string &value)
{
	std::string text = trimCssValue(value);
	if (text.empty()) return text;
	const char quote = text[0];
	if (quote == '\'' || quote == '"') {
		const std::size_t end = text.find(quote, 1);
		return end == std::string::npos ? std::string() : text.substr(1, end - 1);
	}
	const std::size_t comma = text.find(',');
	if (comma != std::string::npos) text = text.substr(0, comma);
	return trimCssValue(text);
}

int fontWeightValue(const std::string &rawValue)
{
	const std::string value = toLowerAscii(trimCssValue(rawValue));
	if (value.empty() || value == "normal") return 400;
	if (value == "bold" || value == "bolder") return 700;
	if (value == "lighter") return 300;
	char *end = nullptr;
	const double number = std::strtod(value.c_str(), &end);
	if (!end || end == value.c_str()) return 400;
	while (*end == ' ') ++end;
	if (*end != '\0') return 400;
	return std::clamp(roundToInt(number), 1, 1000);
}

int clampColorChannel(int value)
{
	if (value < 0) return 0;
	if (value > 255) return 255;
	return value;
}

double parseAngleDegrees(const std::string &value);
int numericRotateTenths(double degrees);

int clampAlphaChannel(double value)
{
	if (value <= 1.0) value *= 255.0;
	if (value < 0.0) return 0;
	if (value > 255.0) return 255;
	return static_cast<int>(value + 0.5);
}

struct ParsedCssColor {
	int r = 255;
	int g = 255;
	int b = 255;
	int a = 255;
	bool valid = false;
};

int hexDigitValue(char c)
{
	if (c >= '0' && c <= '9') return c - '0';
	if (c >= 'a' && c <= 'f') return c - 'a' + 10;
	if (c >= 'A' && c <= 'F') return c - 'A' + 10;
	return -1;
}

bool parseHexPair(const std::string &value, std::size_t index, int &out)
{
	if (index + 1 >= value.size()) return false;
	const int hi = hexDigitValue(value[index]);
	const int lo = hexDigitValue(value[index + 1]);
	if (hi < 0 || lo < 0) return false;
	out = (hi << 4) | lo;
	return true;
}

bool parseHexCssColor(const std::string &value, ParsedCssColor &out)
{
	if (value.empty() || value[0] != '#') return false;
	if (value.size() == 4 || value.size() == 5) {
		const int r = hexDigitValue(value[1]);
		const int g = hexDigitValue(value[2]);
		const int b = hexDigitValue(value[3]);
		const int a = value.size() == 5 ? hexDigitValue(value[4]) : 15;
		if (r < 0 || g < 0 || b < 0 || a < 0) return false;
		out.r = (r << 4) | r;
		out.g = (g << 4) | g;
		out.b = (b << 4) | b;
		out.a = (a << 4) | a;
		out.valid = true;
		return true;
	}
	if (value.size() == 7 || value.size() == 9) {
		if (!parseHexPair(value, 1, out.r) ||
		    !parseHexPair(value, 3, out.g) ||
		    !parseHexPair(value, 5, out.b))
			return false;
		if (value.size() == 9 && !parseHexPair(value, 7, out.a)) return false;
		if (value.size() == 7) out.a = 255;
		out.valid = true;
		return true;
	}
	return false;
}

bool parseRgbFunction(const std::string &value, ParsedCssColor &out)
{
	const std::size_t open = value.find('(');
	const std::size_t close = value.find(')', open == std::string::npos ? 0 : open);
	if (open == std::string::npos || close == std::string::npos) return false;
	const std::string name = toLowerAscii(trimCssValue(value.substr(0, open)));
	if (name != "rgb" && name != "rgba") return false;
	const auto parts = splitTopLevel(value.substr(open + 1, close - open - 1), ',');
	if (parts.size() < 3) return false;
	out.r = clampColorChannel(static_cast<int>(std::strtod(parts[0].c_str(), nullptr)));
	out.g = clampColorChannel(static_cast<int>(std::strtod(parts[1].c_str(), nullptr)));
	out.b = clampColorChannel(static_cast<int>(std::strtod(parts[2].c_str(), nullptr)));
	out.a = parts.size() >= 4 ? clampAlphaChannel(std::strtod(parts[3].c_str(), nullptr)) : 255;
	out.valid = true;
	return true;
}

std::string firstColorToken(const std::string &value)
{
	std::size_t rgb = value.find("rgb(");
	std::size_t rgba = value.find("rgba(");
	if (rgba != std::string::npos && (rgb == std::string::npos || rgba < rgb)) rgb = rgba;
	std::size_t hex = value.find('#');
	if (rgb != std::string::npos && (hex == std::string::npos || rgb < hex)) {
		int depth = 0;
		for (std::size_t i = rgb; i < value.size(); ++i) {
			if (value[i] == '(') depth++;
			else if (value[i] == ')' && --depth == 0) return value.substr(rgb, i - rgb + 1);
		}
		return value.substr(rgb);
	}
	if (hex != std::string::npos) {
		std::size_t end = hex + 1;
		while (end < value.size() && std::isxdigit(static_cast<unsigned char>(value[end]))) ++end;
		return value.substr(hex, end - hex);
	}
	return trimCssValue(value);
}

// Parse to a RAW (pre-panel-swap) native style-value int. Callers route through
// setStyleValue/pixelFromStyleValue (border fallback, keyframe colours), which
// applies the panel byte-swap on 16-bit boards; identity on full-colour boards.
int parseColorStyleValue(const std::string &value)
{
#if GEA_RECPROF
	g_profColorCalls++;
	const int64_t _ct = recNow();
	struct ColorTimer { int64_t s; ~ColorTimer() { g_profColorUs += recNow() - s; } } _colorTimer{_ct};
#endif
	const std::string colorValue = firstColorToken(value);
	ParsedCssColor parsed;
	if (parseRgbFunction(colorValue, parsed)) return static_cast<int>(gea::framework::graphics::pixel::nativeStyleValue(parsed.r, parsed.g, parsed.b));
	if (parseHexCssColor(colorValue, parsed)) return static_cast<int>(gea::framework::graphics::pixel::nativeStyleValue(parsed.r, parsed.g, parsed.b));
	return static_cast<int>(gea::framework::graphics::pixel::nativeStyleValue(255, 255, 255));
}

ParsedCssColor parseCssColor(const std::string &value)
{
	const std::string colorValue = trimCssValue(value);
	if (toLowerAscii(colorValue) == "transparent") {
		ParsedCssColor parsed;
		parsed.r = 0;
		parsed.g = 0;
		parsed.b = 0;
		parsed.a = 0;
		parsed.valid = true;
		return parsed;
	}
	ParsedCssColor parsed;
	if (parseRgbFunction(colorValue, parsed)) return parsed;
	if (parseHexCssColor(colorValue, parsed)) return parsed;
	return parsed;
}

// RAW (pre-panel-swap) native style-value for colours applied via setStyleValue()
// -> Tree::setStyle() -> StyleValues::pixelFromStyleValue(), which applies the
// panel byte-swap once on 16-bit boards (must be raw here or colours double-swap,
// e.g. cream text rendered periwinkle). Colours written straight into node.style
// (gradient stops, grid lines) bypass that swap and use cssColorNative() instead.
// On full-colour boards there is no panel concept, so both are identical RGBA8888.
gea::framework::graphics::pixel::native_t cssColorStyleValue(const ParsedCssColor &color)
{
	return gea::framework::graphics::pixel::nativeStyleValue(color.r, color.g, color.b);
}

// Final native pixel for colours written DIRECTLY into node.style (gradient stops,
// grid colours) which skip setStyleValue/pixelFromStyleValue and so must already be
// in the board's framebuffer form (panel-order RGB565 / RGBA8888).
gea::framework::graphics::pixel::native_t cssColorNative(const ParsedCssColor &color)
{
	return gea::framework::graphics::pixel::nativeColor(color.r, color.g, color.b);
}

CachedCssColor cachedCssColorForValue(const std::string &raw)
{
	const std::string key = trimCssValue(raw);
	if (key.empty()) return {};
	auto &cache = compiledCssColorCache();
	const auto it = cache.find(key);
	if (it != cache.end()) return it->second;
	CachedCssColor out;
	const ParsedCssColor color = parseCssColor(firstColorToken(key));
	if (color.valid) {
		out.styleColor = static_cast<std::int32_t>(cssColorStyleValue(color));
		out.nativeColor = static_cast<std::int32_t>(cssColorNative(color));
		out.alpha = static_cast<std::uint8_t>(color.a);
		out.valid = true;
	}
	cache.emplace(key, out);
	return out;
}

const CssLengthSpec *cachedCompiledCssLengthSpec(const std::string &raw);
bool tryPreResolveStaticCustomLengthSpec(const CssLengthSpec &length, int nodeId, CssLengthSpec &out);

void setCustomPropertyValue(NodeCustomPropertyStore &store, CssAtomId name, const std::string &value)
{
	clearCustomPropertyLookupCache();
	const CachedCssColor color = cachedCssColorForValue(value);
	if (color.valid) {
		store.setColor(name,
		               value,
		               color.styleColor,
		               color.nativeColor,
		               color.alpha);
		return;
	}
	if (const CssLengthSpec *length = cachedCompiledCssLengthSpec(value)) {
		store.setLength(name, value, length->value, static_cast<std::uint8_t>(length->unit));
		return;
	}
	store.set(name, value);
}

void setCustomPropertyRuleValue(NodeCustomPropertyStore &store,
                                CssAtomId name,
                                const CssText &text,
                                int nodeId,
                                std::uint16_t compiledHandle)
{
	clearCustomPropertyLookupCache();
	const std::string value = text.str();
	const auto &compiledValues = compiledCssValues();
	if (compiledHandle < compiledValues.size()) {
		const CssCompiledValue &compiled = compiledValues[compiledHandle];
		if (compiled.declaration == CssDeclarationId::Custom) {
			if (compiled.kind == CssCompiledKind::Color) {
				store.setColor(name,
				               value,
				               compiled.values[0],
				               compiled.values[1],
				               static_cast<std::uint8_t>(compiled.values[2]));
				return;
			}
			if (compiled.kind == CssCompiledKind::Length) {
				CssLengthSpec storedLength;
				if (tryPreResolveStaticCustomLengthSpec(compiled.lengths[0], nodeId, storedLength)) {
					store.setLength(name,
					                value,
					                storedLength.value,
					                static_cast<std::uint8_t>(storedLength.unit));
					return;
				}
				store.setLength(name,
				                value,
				                compiled.lengths[0].value,
				                static_cast<std::uint8_t>(compiled.lengths[0].unit));
				return;
			}
		}
	}
	setCustomPropertyValue(store, name, value);
}

ParsedCssColor parseGradientColorStop(const std::string &value)
{
	const std::string token = trimCssValue(value);
	if (token.rfind("rgb", 0) == 0 || token.rfind("rgba", 0) == 0 || token.find('#') != std::string::npos)
		return parseCssColor(firstColorToken(token));
	const std::size_t end = token.find_first_of(" \t\r\n");
	return parseCssColor(end == std::string::npos ? token : token.substr(0, end));
}

std::string gradientColorTokenForStop(const std::string &stop)
{
	const auto words = splitFunctionAwareWords(stop);
	if (!words.empty()) return words[0];
	const std::string token = firstColorToken(stop);
	const std::string trimmed = trimCssValue(stop);
	if (token == trimmed) {
		const std::size_t end = token.find_first_of(" \t\r\n");
		if (end != std::string::npos) return token.substr(0, end);
	}
	return token;
}

struct ParsedLinearGradient {
	bool valid = false;
	int angleTenths = 1800;
	ParsedCssColor from;
	ParsedCssColor mid;
	ParsedCssColor to;
	int midStopPermille = 500;
	int toStopPermille = 1000;
	bool hasMid = false;
};

struct ParsedRadialGradient {
	bool valid = false;
	ParsedCssColor from;
	ParsedCssColor to;
	int stopPermille = 1000;
	int cxPermille = 500;
	int cyPermille = 500;
	int rxPermille = 1000;
	int ryPermille = 1000;
};

struct ParsedGradientLineLayer {
	bool valid = false;
	bool vertical = false;
	ParsedCssColor color;
	int lineWidth = 1;
};

std::string lastFunctionCall(const std::string &value, const std::string &name)
{
	const std::string needle = name + "(";
	std::string found;
	std::size_t search = 0;
	while (true) {
		const std::size_t start = value.find(needle, search);
		if (start == std::string::npos) break;
		std::size_t i = start + needle.size();
		int depth = 1;
		while (i < value.size() && depth > 0) {
			if (value[i] == '(') depth++;
			else if (value[i] == ')') depth--;
			++i;
		}
		if (depth == 0) found = value.substr(start, i - start);
		search = start + needle.size();
	}
	return found;
}

int colorStopLength(const std::string &stop, int nodeId)
{
	const std::string token = gradientColorTokenForStop(stop);
	const std::size_t tokenStart = stop.find(token);
	const std::string rest = tokenStart == std::string::npos
	    ? std::string()
	    : trimCssValue(stop.substr(tokenStart + token.size()));
	if (rest.empty()) return 0;
	const auto parts = splitWords(rest);
	if (parts.empty()) return 0;
	return parseLengthForNode(parts[0], nodeId, LengthAxis::None);
}

int colorStopPermilleWithLimit(const std::string &stop, int fallback, bool clampToGradientBox)
{
	const std::string token = gradientColorTokenForStop(stop);
	const std::size_t tokenStart = stop.find(token);
	const std::string rest = tokenStart == std::string::npos
	    ? std::string()
	    : trimCssValue(stop.substr(tokenStart + token.size()));
	if (rest.empty()) return fallback;
	const auto parts = splitWords(rest);
	if (parts.empty()) return fallback;
	const std::string first = trimCssValue(parts[0]);
	if (first.find('%') == std::string::npos) return fallback;
	const double percent = std::strtod(first.c_str(), nullptr);
	if (!std::isfinite(percent)) return fallback;
	int permille = static_cast<int>(percent * 10.0 + (percent >= 0.0 ? 0.5 : -0.5));
	if (permille < 0) permille = 0;
	if (clampToGradientBox && permille > 1000) permille = 1000;
	else if (!clampToGradientBox && permille > 60000) permille = 60000;
	return permille;
}

int colorStopPermille(const std::string &stop, int fallback)
{
	return colorStopPermilleWithLimit(stop, fallback, true);
}

int colorStopPermilleUnclamped(const std::string &stop, int fallback)
{
	return colorStopPermilleWithLimit(stop, fallback, false);
}

int parsePercentPermille(const std::string &part, int fallback)
{
	const std::string value = trimCssValue(part);
	if (value.find('%') == std::string::npos) return fallback;
	const double percent = std::strtod(value.c_str(), nullptr);
	if (!std::isfinite(percent)) return fallback;
	return static_cast<int>(percent * 10.0 + (percent >= 0.0 ? 0.5 : -0.5));
}

bool gradientLineIsVertical(int angleTenths)
{
	const double angleRadians = (static_cast<double>(angleTenths) * 3.14159265358979323846) / 1800.0;
	const double dx = std::sin(angleRadians);
	const double dy = -std::cos(angleRadians);
	return std::fabs(dx) >= std::fabs(dy);
}

ParsedGradientLineLayer parseGradientLineLayer(const std::string &value, int nodeId)
{
	ParsedGradientLineLayer layer;
	const std::string call = lastFunctionCall(value, "linear-gradient");
	if (call.empty()) return layer;
	const std::string inner = functionInner(call, "linear-gradient");
	const auto parts = splitTopLevel(inner, ',');
	if (parts.size() < 2) return layer;

	std::size_t colorStart = 0;
	int angleTenths = 1800;
	const std::string first = trimCssValue(parts[0]);
	if (first.find("deg") != std::string::npos || first.find("turn") != std::string::npos || first.find("rad") != std::string::npos) {
		angleTenths = numericRotateTenths(parseAngleDegrees(first));
		colorStart = 1;
	} else if (first.rfind("to ", 0) == 0) {
		const std::string direction = toLowerAscii(first);
		if (direction.find("right") != std::string::npos) angleTenths = 900;
		else if (direction.find("left") != std::string::npos) angleTenths = 2700;
		else if (direction.find("top") != std::string::npos) angleTenths = 0;
		else angleTenths = 1800;
		colorStart = 1;
	}
	if (colorStart + 1 >= parts.size()) return layer;

	const ParsedCssColor firstColor = parseGradientColorStop(parts[colorStart]);
	const ParsedCssColor secondColor = parseGradientColorStop(parts[colorStart + 1]);
	const int firstLength = colorStopLength(parts[colorStart], nodeId);
	const int secondLength = colorStopLength(parts[colorStart + 1], nodeId);
	if (!firstColor.valid || !secondColor.valid || firstColor.a == 0 || secondColor.a != 0 || firstLength <= 0) return layer;
	if (secondLength > 0 && secondLength != firstLength) return layer;

	layer.valid = true;
	layer.vertical = gradientLineIsVertical(angleTenths);
	layer.color = firstColor;
	layer.lineWidth = firstLength;
	return layer;
}

ParsedLinearGradient parseLinearGradient(const std::string &value)
{
	ParsedLinearGradient gradient;
	const std::string call = lastFunctionCall(value, "linear-gradient");
	if (call.empty()) return gradient;
	const std::string inner = functionInner(call, "linear-gradient");
	const auto parts = splitTopLevel(inner, ',');
	if (parts.size() < 2) return gradient;

	std::size_t colorStart = 0;
	const std::string first = trimCssValue(parts[0]);
	if (first.find("deg") != std::string::npos || first.find("turn") != std::string::npos || first.find("rad") != std::string::npos) {
		gradient.angleTenths = numericRotateTenths(parseAngleDegrees(first));
		colorStart = 1;
	} else if (first.rfind("to ", 0) == 0) {
		const std::string direction = toLowerAscii(first);
		if (direction.find("right") != std::string::npos) gradient.angleTenths = 900;
		else if (direction.find("left") != std::string::npos) gradient.angleTenths = 2700;
		else if (direction.find("top") != std::string::npos) gradient.angleTenths = 0;
		else gradient.angleTenths = 1800;
		colorStart = 1;
	}
	if (colorStart >= parts.size()) return gradient;

	gradient.from = parseGradientColorStop(parts[colorStart]);
	if (parts.size() == colorStart + 2) {
		const int fromStop = colorStopPermille(parts[colorStart], -1);
		if (fromStop > 0 && fromStop < 1000) {
			gradient.mid = gradient.from;
			gradient.midStopPermille = fromStop;
			gradient.hasMid = gradient.mid.valid;
		}
	} else if (parts.size() > colorStart + 2) {
		gradient.mid = parseGradientColorStop(parts[colorStart + 1]);
		gradient.midStopPermille = colorStopPermille(parts[colorStart + 1], 500);
		gradient.hasMid = gradient.mid.valid;
	}
	gradient.to = parseGradientColorStop(parts.back());
	gradient.toStopPermille = colorStopPermilleUnclamped(parts.back(), 1000);
	if (gradient.toStopPermille <= 0) gradient.toStopPermille = 1;
	if (gradient.hasMid && gradient.toStopPermille <= gradient.midStopPermille)
		gradient.toStopPermille = gradient.midStopPermille + 1;
	gradient.valid = gradient.from.valid && gradient.to.valid;
	return gradient;
}

ParsedRadialGradient parseRadialGradient(const std::string &value)
{
	ParsedRadialGradient gradient;
	const std::string call = lastFunctionCall(value, "radial-gradient");
	if (call.empty()) return gradient;
	const std::string inner = functionInner(call, "radial-gradient");
	const auto parts = splitTopLevel(inner, ',');
	if (parts.size() < 2) return gradient;

	std::size_t colorStart = 0;
	const ParsedCssColor firstMaybeColor = parseGradientColorStop(parts[0]);
	if (!firstMaybeColor.valid) {
		colorStart = 1;
		const auto words = splitWords(parts[0]);
		std::size_t atIndex = words.size();
		for (std::size_t i = 0; i < words.size(); ++i) {
			if (toLowerAscii(words[i]) == "at") {
				atIndex = i;
				break;
			}
		}

		std::vector<std::string> sizeWords;
		for (std::size_t i = 0; i < atIndex; ++i) {
			const std::string lower = toLowerAscii(words[i]);
			if (lower == "circle" || lower == "ellipse" || lower == "closest-side" ||
			    lower == "closest-corner" || lower == "farthest-side" || lower == "farthest-corner")
				continue;
			sizeWords.push_back(words[i]);
		}
		if (!sizeWords.empty()) gradient.rxPermille = parsePercentPermille(sizeWords[0], gradient.rxPermille);
		if (sizeWords.size() > 1) gradient.ryPermille = parsePercentPermille(sizeWords[1], gradient.ryPermille);
		else if (!sizeWords.empty()) gradient.ryPermille = gradient.rxPermille;

		if (atIndex < words.size()) {
			std::vector<std::string> centerWords;
			for (std::size_t i = atIndex + 1; i < words.size(); ++i) centerWords.push_back(words[i]);
			if (!centerWords.empty()) gradient.cxPermille = parseOriginPart(centerWords[0], gradient.cxPermille);
			if (centerWords.size() > 1) gradient.cyPermille = parseOriginPart(centerWords[1], gradient.cyPermille);
		}
	}
	if (colorStart + 1 >= parts.size()) return gradient;

	gradient.from = parseGradientColorStop(parts[colorStart]);
	gradient.to = parseGradientColorStop(parts.back());
	gradient.stopPermille = colorStopPermille(parts.back(), 1000);
	if (gradient.stopPermille <= 0) gradient.stopPermille = 1;
	gradient.valid = gradient.from.valid && gradient.to.valid;
	return gradient;
}

int flexAlignValue(const std::string &value)
{
	if (value == "center") return 1;
	if (value == "flex-end" || value == "end") return 2;
	if (value == "space-between") return 3;
	if (value == "space-around") return 4;
	if (value == "baseline") return 5;
	return 0;
}

int displayValue(const std::string &value)
{
	if (value == "none") return kDisplayNone;
	if (value == "grid" || value == "inline-grid") return kDisplayGrid;
	if (value == "flex" || value == "inline-flex") return kDisplayFlex;
	return kDisplayBlock;
}

int imageFitValue(const std::string &value)
{
	const std::string lower = toLowerAscii(trimCssValue(value));
	if (lower == "contain") return 1;
	if (lower == "cover") return 2;
	if (lower == "none") return 3;
	if (lower == "scale-down") return 4;
	return 0;
}

int flexDirectionValue(const std::string &value) { return value == "row" ? 1 : 0; }
int flexWrapValue(const std::string &value) { return value == "wrap" ? 1 : 0; }
int alignSelfValue(const std::string &value) { return value == "auto" ? -1 : flexAlignValue(value); }
int positionValue(const std::string &value)
{
	if (value == "absolute") return 1;
	if (value == "relative") return 2;
	return 0;
}

int textAlignValue(const std::string &value)
{
	if (value == "center") return 1;
	if (value == "right" || value == "end") return 2;
	return 0;
}

// CSS `text-decoration` — only the values relevant to embedded UI are
// modeled: `none` (default), `underline`, and `line-through`. The
// renderer paints a 1-pixel horizontal line at a y-offset chosen per
// decoration kind; the value enum is what TextRenderer consults.
int textDecorationValue(const std::string &value)
{
	if (value == "line-through" || value == "strikethrough") return 2;
	if (value == "underline") return 1;
	return 0;
}

int textTransformValue(const std::string &value)
{
	const std::string lower = toLowerAscii(trimCssValue(value));
	if (lower == "uppercase") return 1;
	if (lower == "lowercase") return 2;
	if (lower == "capitalize") return 3;
	return 0;
}

// CSS `backface-visibility`: hidden => 1 (cull a face/text whose projected winding
// points away from the viewer), visible/default => 0.
int backfaceValue(const std::string &value)
{
	return toLowerAscii(trimCssValue(value)) == "hidden" ? 1 : 0;
}

int overflowValue(const std::string &value)
{
	if (value == "hidden") return 1;
	if (value == "scroll" || value == "auto") return 2;
	return 0;
}

// CSS `white-space`: 1 (nowrap) for any value that suppresses width-based line
// breaking (`nowrap`, `pre`), 0 (normal — wrap) otherwise. `pre`/`pre-line`
// keep authored newlines, which the renderer already honours, so they map to
// the same no-auto-wrap behaviour as `nowrap` here.
int whiteSpaceValue(const std::string &value)
{
	const std::string lower = toLowerAscii(trimCssValue(value));
	if (lower == "nowrap" || lower == "pre") return 1;
	return 0;
}

// CSS `text-overflow`: 1 (ellipsis) when an overflowing nowrap line should be
// truncated with a trailing "..."; 0 (clip — the default) otherwise.
int textOverflowValue(const std::string &value)
{
	return toLowerAscii(trimCssValue(value)) == "ellipsis" ? 1 : 0;
}

// CSS `pointer-events`: 1 (none — the node and its subtree are skipped during
// hit-testing so clicks fall through to whatever is behind) when "none";
// 0 (auto — the default) otherwise.
int pointerEventsValue(const std::string &value)
{
	return toLowerAscii(trimCssValue(value)) == "none" ? 1 : 0;
}

int parseOpacity(const std::string &value)
{
	double v = std::strtod(value.c_str(), nullptr);
	if (v <= 1.0) v *= 255.0;
	if (v < 0.0) v = 0.0;
	if (v > 255.0) v = 255.0;
	return static_cast<int>(v + 0.5);
}

int parseFilterBlurRadius(const std::string &value, int nodeId)
{
	const std::string text = trimCssValue(value);
	const std::string lower = toLowerAscii(text);
	if (lower.empty() || lower == "none") return 0;
	const std::size_t blur = lower.find("blur(");
	if (blur == std::string::npos) return 0;
	std::size_t i = blur + 5;
	int depth = 1;
	const std::size_t argStart = i;
	while (i < text.size() && depth > 0) {
		if (text[i] == '(') depth++;
		else if (text[i] == ')') depth--;
		++i;
	}
	if (depth != 0 || i <= argStart) return 0;
	int radius = parseLengthForNode(text.substr(argStart, i - argStart - 1), nodeId, LengthAxis::None);
	if (radius < 0) radius = 0;
	if (radius > 64) radius = 64;
	return radius;
}

std::vector<std::string> splitFunctionAwareWords(const std::string &value)
{
	std::vector<std::string> out;
	std::size_t i = 0;
	while (i < value.size()) {
		while (i < value.size() && static_cast<unsigned char>(value[i]) <= ' ') ++i;
		const std::size_t start = i;
		int depth = 0;
		while (i < value.size()) {
			const char c = value[i];
			if (c == '(') depth++;
			else if (c == ')' && depth > 0) depth--;
			else if (static_cast<unsigned char>(c) <= ' ' && depth == 0) break;
			++i;
		}
		if (i > start) out.push_back(value.substr(start, i - start));
	}
	return out;
}

bool isBoxShadowColorToken(const std::string &token)
{
	const std::string lower = toLowerAscii(trimCssValue(token));
	return lower == "transparent" ||
	       lower.rfind("rgb(", 0) == 0 ||
	       lower.rfind("rgba(", 0) == 0 ||
	       (!lower.empty() && lower[0] == '#');
}

struct ParsedBoxShadow {
	bool valid = false;
	bool inset = false;
	int offsetX = 0;
	int offsetY = 0;
	int blur = 0;
	int spread = 0;
	ParsedCssColor color;
};

ParsedBoxShadow parseInsetBoxShadow(const std::string &value, int nodeId)
{
	ParsedBoxShadow out;
	out.color.r = 0;
	out.color.g = 0;
	out.color.b = 0;
	out.color.a = 255;
	out.color.valid = true;

	const std::string text = trimCssValue(value);
	const std::string lowerText = toLowerAscii(text);
	if (text.empty() || lowerText == "none") {
		out.valid = true;
		out.inset = false;
		out.color.a = 0;
		return out;
	}

	for (const auto &rawLayer : splitTopLevel(text, ',')) {
		const auto tokens = splitFunctionAwareWords(rawLayer);
		bool inset = false;
		std::vector<std::string> lengths;
		ParsedCssColor color = out.color;
		for (const auto &token : tokens) {
			const std::string lower = toLowerAscii(trimCssValue(token));
			if (lower == "inset") {
				inset = true;
				continue;
			}
			if (isBoxShadowColorToken(token)) {
				const ParsedCssColor parsed = parseCssColor(firstColorToken(token));
				if (parsed.valid) color = parsed;
				continue;
			}
			if (lower == "outset") continue;
			lengths.push_back(token);
		}
		if (!inset) continue;
		out.valid = true;
		out.inset = true;
		out.color = color;
		if (!lengths.empty()) out.offsetX = parseLengthForNode(lengths[0], nodeId, LengthAxis::Horizontal);
		if (lengths.size() > 1) out.offsetY = parseLengthForNode(lengths[1], nodeId, LengthAxis::Vertical);
		if (lengths.size() > 2) out.blur = parseLengthForNode(lengths[2], nodeId, LengthAxis::None);
		if (lengths.size() > 3) out.spread = parseLengthForNode(lengths[3], nodeId, LengthAxis::None);
		if (out.blur < 0) out.blur = 0;
		if (out.blur > 96) out.blur = 96;
		return out;
	}

	out.valid = true;
	out.inset = false;
	out.color.a = 0;
	return out;
}

void applyBoxShadowValue(NodeHandle node, const std::string &value, StyleApplicationSource source)
{
	const ParsedBoxShadow shadow = parseInsetBoxShadow(value, node.id());
	if (!shadow.valid || !shadow.inset || shadow.color.a == 0) {
		setStyleValue(node, Property::BoxShadowInset, 0, source);
		setStyleValue(node, Property::BoxShadowOffsetX, 0, source);
		setStyleValue(node, Property::BoxShadowOffsetY, 0, source);
		setStyleValue(node, Property::BoxShadowBlur, 0, source);
		setStyleValue(node, Property::BoxShadowSpread, 0, source);
		setStyleValue(node, Property::BoxShadowColor, 0, source);
		setStyleValue(node, Property::BoxShadowAlpha, 0, source);
		return;
	}
	setStyleValue(node, Property::BoxShadowOffsetX, shadow.offsetX, source);
	setStyleValue(node, Property::BoxShadowOffsetY, shadow.offsetY, source);
	setStyleValue(node, Property::BoxShadowBlur, shadow.blur, source);
	setStyleValue(node, Property::BoxShadowSpread, shadow.spread, source);
	setStyleValue(node, Property::BoxShadowColor, cssColorStyleValue(shadow.color), source);
	setStyleValue(node, Property::BoxShadowAlpha, shadow.color.a, source);
	setStyleValue(node, Property::BoxShadowInset, 1, source);
}

int numericLength(double value)
{
	return rawNumber(value);
}

int numericOpacity(double value)
{
	if (!std::isfinite(value)) return 0;
	if (value <= 1.0) value *= 255.0;
	if (value < 0.0) value = 0.0;
	if (value > 255.0) value = 255.0;
	return static_cast<int>(value + 0.5);
}

int numericRotateTenths(double degrees)
{
	if (!std::isfinite(degrees)) return 0;
	return static_cast<int>(degrees * 10.0 + (degrees >= 0 ? 0.5 : -0.5));
}

int numericScalePermille(double value)
{
	if (!std::isfinite(value)) return 1000;
	if (value < 0.0) value = 0.0;
	if (value > 16.0) value = 16.0;
	return static_cast<int>(value * 1000.0 + 0.5);
}

int parseRotateTenths(const std::string &value)
{
	const auto rotate = value.find("rotate(");
	const char *start = value.c_str();
	if (rotate != std::string::npos) start = value.c_str() + rotate + 7;
	double degrees = std::strtod(start, nullptr);
	if (value.find("rad", static_cast<std::size_t>(start - value.c_str())) != std::string::npos) {
		degrees = degrees * 180.0 / 3.14159265358979323846;
	}
	return numericRotateTenths(degrees);
}

double parseAngleDegrees(const std::string &value)
{
	char *end = nullptr;
	double degrees = std::strtod(value.c_str(), &end);
	const std::string unit = end ? trimCssValue(std::string(end)) : std::string();
	if (unit.rfind("rad", 0) == 0) degrees = degrees * 180.0 / 3.14159265358979323846;
	if (unit.rfind("turn", 0) == 0) degrees = degrees * 360.0;
	return degrees;
}

int parseScalePermille(const std::string &value)
{
	const double scale = std::strtod(value.c_str(), nullptr);
	return roundToInt(scale * 1000.0);
}

struct TransformComponents {
	int rotateX = 0;
	int rotateY = 0;
	int rotateZ = 0;
	int translateX = 0;
	int translateY = 0;
	int translateZ = 0;
	int translateXPercent = 0;
	int translateYPercent = 0;
	int scaleX = 1000;
	int scaleY = 1000;
	bool hasRotateX = false;
	bool hasRotateY = false;
	bool hasRotateZ = false;
	bool hasTranslateX = false;
	bool hasTranslateY = false;
	bool hasTranslateZ = false;
	bool hasScaleX = false;
	bool hasScaleY = false;
};

bool parseSimplePercentPermille(const std::string &raw, int &out)
{
	const std::string value = trimCssValue(raw);
	if (value.empty()) return false;
	char *end = nullptr;
	const double percent = std::strtod(value.c_str(), &end);
	if (end == value.c_str() || !end || *end != '%') return false;
	++end;
	while (*end != '\0' && static_cast<unsigned char>(*end) <= ' ') ++end;
	if (*end != '\0') return false;
	int permille = roundToInt(percent * 10.0);
	if (permille < -32768) permille = -32768;
	if (permille > 32767) permille = 32767;
	out = permille;
	return true;
}

void parseTranslateComponent(const std::string &value,
                             int nodeId,
                             LengthAxis axis,
                             int &length,
                             int &percent,
                             bool &hasValue)
{
	hasValue = true;
	if (parseSimplePercentPermille(value, percent)) {
		length = 0;
		return;
	}
	length = parseLengthForNode(value, nodeId, axis);
	percent = 0;
}

TransformComponents parseTransformComponents(const std::string &value, int nodeId)
{
#if GEA_RECPROF
	g_profXformCalls++;
	const int64_t _xt = recNow();
	struct XformTimer { int64_t s; ~XformTimer() { g_profXformUs += recNow() - s; } } _xformTimer{_xt};
#endif
	TransformComponents out;
	std::size_t i = 0;
	while (i < value.size()) {
		while (i < value.size() && static_cast<unsigned char>(value[i]) <= ' ') ++i;
		const std::size_t nameStart = i;
		while (i < value.size() && (std::isalpha(static_cast<unsigned char>(value[i])) || value[i] == '3')) ++i;
		if (i == nameStart || i >= value.size() || value[i] != '(') {
			++i;
			continue;
		}
		const std::string name = toLowerAscii(value.substr(nameStart, i - nameStart));
		const std::size_t argStart = ++i;
		int depth = 1;
		while (i < value.size() && depth > 0) {
			if (value[i] == '(') depth++;
			else if (value[i] == ')') depth--;
			++i;
		}
		const std::string arg = value.substr(argStart, depth == 0 ? i - argStart - 1 : i - argStart);
		const auto args = splitTopLevel(arg, ',');
		if (name == "rotate" || name == "rotatez") {
			out.rotateZ = numericRotateTenths(parseAngleDegrees(arg));
			out.hasRotateZ = true;
		} else if (name == "rotatex") {
			out.rotateX = numericRotateTenths(parseAngleDegrees(arg));
			out.hasRotateX = true;
		} else if (name == "rotatey") {
			out.rotateY = numericRotateTenths(parseAngleDegrees(arg));
			out.hasRotateY = true;
		} else if (name == "translatex") {
			parseTranslateComponent(arg, nodeId, LengthAxis::Horizontal, out.translateX, out.translateXPercent, out.hasTranslateX);
		} else if (name == "translatey") {
			parseTranslateComponent(arg, nodeId, LengthAxis::Vertical, out.translateY, out.translateYPercent, out.hasTranslateY);
		} else if (name == "translatez") {
			out.translateZ = parseLengthForNode(arg, nodeId, LengthAxis::Horizontal);
			out.hasTranslateZ = true;
		} else if (name == "translate" || name == "translate3d") {
			if (!args.empty()) {
				parseTranslateComponent(args[0], nodeId, LengthAxis::Horizontal, out.translateX, out.translateXPercent, out.hasTranslateX);
			}
			if (args.size() > 1) {
				parseTranslateComponent(args[1], nodeId, LengthAxis::Vertical, out.translateY, out.translateYPercent, out.hasTranslateY);
			}
			if (args.size() > 2) {
				out.translateZ = parseLengthForNode(args[2], nodeId, LengthAxis::Horizontal);
				out.hasTranslateZ = true;
			}
		} else if (name == "scale") {
			const int sx = parseScalePermille(args.empty() ? arg : args[0]);
			const int sy = parseScalePermille(args.size() > 1 ? args[1] : (args.empty() ? arg : args[0]));
			out.scaleX = sx;
			out.scaleY = sy;
			out.hasScaleX = true;
			out.hasScaleY = true;
		} else if (name == "scalex") {
			out.scaleX = parseScalePermille(arg);
			out.hasScaleX = true;
		} else if (name == "scaley") {
			out.scaleY = parseScalePermille(arg);
			out.hasScaleY = true;
		}
	}
	return out;
}

int parseOriginPart(const std::string &part, int fallback)
{
	if (part.empty()) return fallback;
	if (part == "left" || part == "top") return 0;
	if (part == "center") return 500;
	if (part == "right" || part == "bottom") return 1000;
	double v = std::strtod(part.c_str(), nullptr);
	if (part.find('%') != std::string::npos) return static_cast<int>(v * 10.0 + (v >= 0.0 ? 0.5 : -0.5));
	return fallback;
}

std::vector<std::string> splitWords(const std::string &value)
{
	std::vector<std::string> out;
	std::size_t i = 0;
	while (i < value.size()) {
		while (i < value.size() && static_cast<unsigned char>(value[i]) <= ' ') ++i;
		const std::size_t start = i;
		int depth = 0;
		while (i < value.size()) {
			const char c = value[i];
			if (c == '(') depth++;
			else if (c == ')' && depth > 0) depth--;
			else if (static_cast<unsigned char>(c) <= ' ' && depth == 0) break;
			++i;
		}
		if (i > start) out.push_back(value.substr(start, i - start));
	}
	return out;
}

struct BoxLengths {
	int top = 0;
	int right = 0;
	int bottom = 0;
	int left = 0;
};

BoxLengths parseBoxLengths(const std::string &value, int nodeId = -1)
{
	const auto parts = splitWords(value);
	if (parts.empty()) return {};
	const int first = parseLengthForNode(parts[0], nodeId, LengthAxis::Vertical);
	const int second = parts.size() > 1 ? parseLengthForNode(parts[1], nodeId, LengthAxis::Horizontal) : first;
	const int third = parts.size() > 2 ? parseLengthForNode(parts[2], nodeId, LengthAxis::Vertical) : first;
	const int fourth = parts.size() > 3 ? parseLengthForNode(parts[3], nodeId, LengthAxis::Horizontal) : second;
	return {first, second, third, fourth};
}

bool hasDynamicCssValue(const std::string &value)
{
	const std::string lower = toLowerAscii(value);
	return lower.find("var(") != std::string::npos ||
	       lower.find("calc(") != std::string::npos ||
	       lower.find("min(") != std::string::npos ||
	       lower.find("max(") != std::string::npos ||
	       lower.find("clamp(") != std::string::npos;
}

bool parseCompiledLengthSpec(const std::string &raw, CssLengthSpec &out, bool allowAuto = false);
bool compileLengthExpressionSpec(const std::string &raw, CssLengthSpec &out);
int resolveCompiledLengthForNode(const CssLengthSpec &length, int nodeId, LengthAxis axis, int depth = 0);
ResolvedCssLength resolveCompiledLengthForNodeDetailed(const CssLengthSpec &length, int nodeId, LengthAxis axis, int depth = 0);

std::uint16_t storeCompiledCssLengthExpression(const CssLengthExpression &expression)
{
	auto &list = compiledCssLengthExpressions();
	if (list.size() >= kNoCompiledCssLengthExpression) return kNoCompiledCssLengthExpression;
	list.push_back(expression);
	return static_cast<std::uint16_t>(list.size() - 1);
}

bool storeCompiledCssLengthExpressionSpec(const CssLengthExpression &expression, CssLengthSpec &out)
{
	const std::uint16_t handle = storeCompiledCssLengthExpression(expression);
	if (handle == kNoCompiledCssLengthExpression) return false;
	out.unit = CssLengthUnit::Expression;
	out.value = static_cast<float>(handle);
	return true;
}

bool compileVarLengthSpec(const std::string &raw, CssLengthSpec &out)
{
	const std::string text = trimCssValue(raw);
	const std::string lower = toLowerAscii(text);
	if (!startsWith(lower, "var(") || text.empty() || text.back() != ')') return false;
	const std::size_t open = text.find('(');
	if (open == std::string::npos || open + 1 >= text.size()) return false;
	const std::string inner = text.substr(open + 1, text.size() - open - 2);
	const auto parts = splitTopLevel(inner, ',');
	if (parts.empty()) return false;
	const std::string name = trimCssValue(parts[0]);
	if (name.rfind("--", 0) != 0) return false;
	CssLengthExpression expression;
	expression.kind = CssLengthExpressionKind::Var;
	expression.nameAtom = internCssAtom(name);
	if (parts.size() > 1) {
		if (!parseCompiledLengthSpec(parts[1], expression.a)) return false;
		expression.hasFallback = 1;
	}
	return storeCompiledCssLengthExpressionSpec(expression, out);
}

bool parseCompiledLengthSpec(const std::string &raw, CssLengthSpec &out, bool allowAuto)
{
	const std::string value = toLowerAscii(trimCssValue(raw));
	if (value.empty()) return false;
	if (value == "auto") {
		if (!allowAuto) return false;
		out.unit = CssLengthUnit::Auto;
		out.value = 0.0f;
		return true;
	}
	if (startsWith(value, "var(")) return compileVarLengthSpec(raw, out);
	if (startsWith(value, "calc(") || startsWith(value, "min(") ||
	    startsWith(value, "max(") || startsWith(value, "clamp("))
		return compileLengthExpressionSpec(raw, out);
	char *end = nullptr;
	const double parsed = std::strtod(value.c_str(), &end);
	if (end == value.c_str()) return false;
	const std::string unit = end ? trimCssValue(std::string(end)) : std::string();
	if (unit.empty()) out.unit = CssLengthUnit::Raw;
	else if (unit == "px") out.unit = CssLengthUnit::Px;
	else if (unit == "%") out.unit = CssLengthUnit::Percent;
	else if (unit == "vw") out.unit = CssLengthUnit::Vw;
	else if (unit == "vh") out.unit = CssLengthUnit::Vh;
	else if (unit == "vmin") out.unit = CssLengthUnit::Vmin;
	else if (unit == "vmax") out.unit = CssLengthUnit::Vmax;
	else if (unit == "dvw") out.unit = CssLengthUnit::Dvw;
	else if (unit == "dvh") out.unit = CssLengthUnit::Dvh;
	else return false;
	out.value = static_cast<float>(parsed);
	return true;
}

bool parseCompiledCalcExpression(const std::string &expr, CssLengthSpec &out)
{
	const std::string text = trimCssValue(expr);
	for (char op : {'/', '*', '+', '-'}) {
		int depth = 0;
		for (std::size_t i = 0; i < text.size(); ++i) {
			const char c = text[i];
			if (c == '(') depth++;
			else if (c == ')' && depth > 0) depth--;
			else if (c == op && depth == 0 && i > 0) {
				CssLengthExpression expression;
				if (!parseCompiledLengthSpec(text.substr(0, i), expression.a)) return false;
				if (op == '/' || op == '*') {
					const std::string right = trimCssValue(text.substr(i + 1));
					char *end = nullptr;
					const double scalar = std::strtod(right.c_str(), &end);
					if (end == right.c_str()) return false;
					while (*end == ' ') ++end;
					if (*end != '\0') return false;
					expression.scalar = static_cast<float>(scalar);
					expression.kind = op == '/'
					    ? CssLengthExpressionKind::Divide
					    : CssLengthExpressionKind::Multiply;
				} else {
					if (!parseCompiledLengthSpec(text.substr(i + 1), expression.b)) return false;
					expression.kind = op == '+'
					    ? CssLengthExpressionKind::Add
					    : CssLengthExpressionKind::Subtract;
				}
				return storeCompiledCssLengthExpressionSpec(expression, out);
			}
		}
	}
	return parseCompiledLengthSpec(text, out);
}

bool compileLengthExpressionSpec(const std::string &raw, CssLengthSpec &out)
{
	const std::string text = trimCssValue(raw);
	const std::string lower = toLowerAscii(text);
	if (startsWith(lower, "calc(")) return parseCompiledCalcExpression(functionInner(text, "calc"), out);
	if (startsWith(lower, "min(") || startsWith(lower, "max(")) {
		const bool isMin = startsWith(lower, "min(");
		const auto parts = splitTopLevel(functionInner(text, isMin ? "min" : "max"), ',');
		if (parts.empty()) return false;
		CssLengthSpec current;
		if (!parseCompiledLengthSpec(parts[0], current)) return false;
		for (std::size_t i = 1; i < parts.size(); ++i) {
			CssLengthExpression expression;
			expression.kind = isMin ? CssLengthExpressionKind::Min : CssLengthExpressionKind::Max;
			expression.a = current;
			if (!parseCompiledLengthSpec(parts[i], expression.b)) return false;
			if (!storeCompiledCssLengthExpressionSpec(expression, current)) return false;
		}
		out = current;
		return true;
	}
	if (startsWith(lower, "clamp(")) {
		const auto parts = splitTopLevel(functionInner(text, "clamp"), ',');
		if (parts.size() < 3) return false;
		CssLengthExpression expression;
		expression.kind = CssLengthExpressionKind::Clamp;
		if (!parseCompiledLengthSpec(parts[0], expression.a)) return false;
		if (!parseCompiledLengthSpec(parts[1], expression.b)) return false;
		if (!parseCompiledLengthSpec(parts[2], expression.c)) return false;
		return storeCompiledCssLengthExpressionSpec(expression, out);
	}
	return false;
}

const CssLengthSpec *cachedCompiledCssLengthSpec(const std::string &raw)
{
	const std::string key = trimCssValue(raw);
	if (key.empty()) return nullptr;
	auto &cache = compiledCssLengthCache();
	const auto it = cache.find(key);
	if (it != cache.end()) return &it->second;
	CssLengthSpec spec;
	if (!parseCompiledLengthSpec(key, spec)) return nullptr;
	const auto inserted = cache.emplace(key, spec);
	return &inserted.first->second;
}

ResolvedCssLength resolveCustomPropertyLengthForNode(const std::string &raw, int nodeId, LengthAxis axis, int depth)
{
	if (depth > 8) return {0, false, false};
	if (const CssLengthSpec *compiled = cachedCompiledCssLengthSpec(raw))
		return resolveCompiledLengthForNodeDetailed(*compiled, nodeId, axis, depth + 1);
	return {parseLengthForNode(raw, nodeId, axis), false, false};
}

bool compiledLengthSpecHasCustomRuntimeInputs(const CssLengthSpec &length, int depth);

bool compiledLengthExpressionHasCustomRuntimeInputs(const CssLengthExpression &expression, int depth)
{
	if (depth > 8) return true;
	if (expression.kind == CssLengthExpressionKind::Var) return true;
	if (compiledLengthSpecHasCustomRuntimeInputs(expression.a, depth + 1)) return true;
	switch (expression.kind) {
	case CssLengthExpressionKind::Add:
	case CssLengthExpressionKind::Subtract:
	case CssLengthExpressionKind::Min:
	case CssLengthExpressionKind::Max:
		return compiledLengthSpecHasCustomRuntimeInputs(expression.b, depth + 1);
	case CssLengthExpressionKind::Clamp:
		return compiledLengthSpecHasCustomRuntimeInputs(expression.b, depth + 1) ||
		       compiledLengthSpecHasCustomRuntimeInputs(expression.c, depth + 1);
	case CssLengthExpressionKind::Multiply:
	case CssLengthExpressionKind::Divide:
		return false;
	case CssLengthExpressionKind::Var:
		return true;
	}
	return true;
}

bool compiledLengthSpecHasCustomRuntimeInputs(const CssLengthSpec &length, int depth)
{
	if (depth > 8) return true;
	if (length.unit == CssLengthUnit::Percent) return true;
	if (length.unit != CssLengthUnit::Expression) return false;
	const auto &list = compiledCssLengthExpressions();
	const std::uint16_t handle = static_cast<std::uint16_t>(length.value);
	if (handle >= list.size()) return true;
	return compiledLengthExpressionHasCustomRuntimeInputs(list[handle], depth + 1);
}

bool tryPreResolveStaticCustomLengthSpec(const CssLengthSpec &length, int nodeId, CssLengthSpec &out)
{
	if (length.unit != CssLengthUnit::Expression) return false;
	if (compiledLengthSpecHasCustomRuntimeInputs(length, 0)) return false;
	const ResolvedCssLength resolved = resolveCompiledLengthForNodeDetailed(length, nodeId, LengthAxis::None);
	if (resolved.isPercent || resolved.isAuto) return false;
	out.unit = CssLengthUnit::Raw;
	out.value = static_cast<float>(resolved.value);
	return true;
}

ResolvedCssLength resolveCompiledLengthExpressionForNode(const CssLengthExpression &expression,
                                                         int nodeId,
                                                         LengthAxis axis,
                                                         int depth);

bool tryResolveStaticLengthExpressionCached(std::uint16_t handle,
                                            const CssLengthExpression &expression,
                                            int nodeId,
                                            LengthAxis axis,
                                            int depth,
                                            ResolvedCssLength &out)
{
	auto &cache = staticLengthExpressionResolutionCache();
	if (cache.size() <= handle) cache.resize(static_cast<std::size_t>(handle) + 1);
	StaticLengthExpressionResolution &entry = cache[handle];
	if (entry.status == 0) {
		CssLengthSpec spec;
		spec.unit = CssLengthUnit::Expression;
		spec.value = static_cast<float>(handle);
		entry.status = compiledLengthSpecHasCustomRuntimeInputs(spec, 0) ? 1 : 2;
	}
	if (entry.status != 2) return false;
	if (entry.valid) {
		out = entry.value;
		return true;
	}
	entry.value = resolveCompiledLengthExpressionForNode(expression, nodeId, axis, depth);
	entry.valid = 1;
	out = entry.value;
	return true;
}

bool tryResolveDynamicLengthExpressionCached(std::uint16_t handle,
                                             int nodeId,
                                             LengthAxis axis,
                                             ResolvedCssLength &out)
{
	for (const DynamicLengthExpressionResolution &entry : dynamicLengthExpressionResolutionCache()) {
		if (!entry.valid ||
		    entry.handle != handle ||
		    entry.nodeId != nodeId ||
		    entry.axis != static_cast<std::uint8_t>(axis))
			continue;
		out = entry.value;
		return true;
	}
	return false;
}

void storeDynamicLengthExpressionCached(std::uint16_t handle,
                                        int nodeId,
                                        LengthAxis axis,
                                        ResolvedCssLength value)
{
	if (handle == kNoCompiledCssLengthExpression || nodeId < 0) return;
	std::uint8_t &cursor = dynamicLengthExpressionResolutionCacheCursor();
	DynamicLengthExpressionResolution &entry = dynamicLengthExpressionResolutionCache()[cursor];
	entry.value = value;
	entry.handle = handle;
	entry.nodeId = static_cast<std::int16_t>(std::min(nodeId, 32767));
	entry.axis = static_cast<std::uint8_t>(axis);
	entry.valid = 1;
	cursor = static_cast<std::uint8_t>((cursor + 1) % kDynamicLengthExpressionResolutionCacheSize);
}

ResolvedCssLength resolveCompiledLengthExpressionForNode(const CssLengthExpression &expression,
                                                         int nodeId,
                                                         LengthAxis axis,
                                                         int depth)
{
	if (expression.kind == CssLengthExpressionKind::Var) {
		if (const NodeCustomProperty *entry = lookupCustomPropertyEntry(nodeId, expression.nameAtom)) {
			if (entry->hasLength()) {
				CssLengthSpec spec;
				spec.value = entry->lengthValue;
				spec.unit = static_cast<CssLengthUnit>(entry->lengthUnit);
				return resolveCompiledLengthForNodeDetailed(spec, nodeId, axis, depth + 1);
			}
			return resolveCustomPropertyLengthForNode(entry->value, nodeId, axis, depth + 1);
		}
		if (expression.hasFallback)
			return resolveCompiledLengthForNodeDetailed(expression.a, nodeId, axis, depth + 1);
		return {0, false, false};
	}
	// A percent-homogeneous calc() -- `calc(50% - 1.2%)` -- IS a percentage. Resolving
	// it to px here uses percentBasisForNode() at style-apply time, which falls back to
	// the VIEWPORT whenever the node has no parent yet. Report it as a percent (permille)
	// so the caller can store it in the *Percent companion and let LAYOUT resolve it
	// against the real containing block, exactly as a bare `50%` already does.
	{
		const ResolvedCssLength da = resolveCompiledLengthForNodeDetailed(expression.a, nodeId, axis, depth + 1);
		if (da.isPercent && (expression.kind == CssLengthExpressionKind::Add ||
		                     expression.kind == CssLengthExpressionKind::Subtract)) {
			const ResolvedCssLength db =
			    resolveCompiledLengthForNodeDetailed(expression.b, nodeId, axis, depth + 1);
			if (db.isPercent)
				return {expression.kind == CssLengthExpressionKind::Add ? da.value + db.value
				                                                       : da.value - db.value,
				        true, false};
		}
	}
	const int a = resolveCompiledLengthForNode(expression.a, nodeId, axis, depth + 1);
	switch (expression.kind) {
	case CssLengthExpressionKind::Add:
		return {a + resolveCompiledLengthForNode(expression.b, nodeId, axis, depth + 1), false, false};
	case CssLengthExpressionKind::Subtract:
		return {a - resolveCompiledLengthForNode(expression.b, nodeId, axis, depth + 1), false, false};
	case CssLengthExpressionKind::Multiply:
		return {roundToInt(static_cast<double>(a) * static_cast<double>(expression.scalar)), false, false};
	case CssLengthExpressionKind::Divide:
		return {expression.scalar == 0.0f
		            ? 0
		            : roundToInt(static_cast<double>(a) / static_cast<double>(expression.scalar)),
		        false,
		        false};
	case CssLengthExpressionKind::Min:
		return {std::min(a, resolveCompiledLengthForNode(expression.b, nodeId, axis, depth + 1)), false, false};
	case CssLengthExpressionKind::Max:
		return {std::max(a, resolveCompiledLengthForNode(expression.b, nodeId, axis, depth + 1)), false, false};
	case CssLengthExpressionKind::Clamp: {
		const int preferred = resolveCompiledLengthForNode(expression.b, nodeId, axis, depth + 1);
		const int maxValue = resolveCompiledLengthForNode(expression.c, nodeId, axis, depth + 1);
		return {std::max(a, std::min(preferred, maxValue)), false, false};
	}
	case CssLengthExpressionKind::Var:
		return {0, false, false};
	}
	return {0, false, false};
}

ResolvedCssLength resolveCompiledLengthForNodeDetailed(const CssLengthSpec &length, int nodeId, LengthAxis axis, int depth)
{
	const double value = static_cast<double>(length.value);
	switch (length.unit) {
	case CssLengthUnit::Raw:
		return {rawNumber(value), false, false};
	case CssLengthUnit::Px:
		return {cssPixelLength(value), false, false};
	case CssLengthUnit::Percent:
		return {roundToInt(value * 10.0), true, false};
	case CssLengthUnit::Vw:
	case CssLengthUnit::Dvw:
		return {g_viewport_width > 0 ? roundToInt(value * g_viewport_width / 100.0) : 0, false, false};
	case CssLengthUnit::Vh:
	case CssLengthUnit::Dvh:
		return {g_viewport_height > 0 ? roundToInt(value * g_viewport_height / 100.0) : 0, false, false};
	case CssLengthUnit::Vmin:
		return {g_viewport_width > 0 && g_viewport_height > 0
		            ? roundToInt(value * std::min(g_viewport_width, g_viewport_height) / 100.0)
		            : 0,
		        false,
		        false};
	case CssLengthUnit::Vmax:
		return {g_viewport_width > 0 && g_viewport_height > 0
		            ? roundToInt(value * std::max(g_viewport_width, g_viewport_height) / 100.0)
		            : 0,
		        false,
		        false};
	case CssLengthUnit::Auto:
		return {kUnset, false, true};
	case CssLengthUnit::Expression:
		break;
	case CssLengthUnit::Invalid:
		return {0, false, false};
	}

	if (depth > 8) return {0, false, false};
	const auto &list = compiledCssLengthExpressions();
	const std::uint16_t handle = static_cast<std::uint16_t>(value);
	if (handle >= list.size()) return {0, false, false};
	const CssLengthExpression &expression = list[handle];
	ResolvedCssLength cached;
	if (tryResolveStaticLengthExpressionCached(handle, expression, nodeId, axis, depth, cached)) return cached;
	if (tryResolveDynamicLengthExpressionCached(handle, nodeId, axis, cached)) return cached;
	ResolvedCssLength resolved = resolveCompiledLengthExpressionForNode(expression, nodeId, axis, depth);
	storeDynamicLengthExpressionCached(handle, nodeId, axis, resolved);
	return resolved;
}

int resolveCompiledLengthForNode(const CssLengthSpec &length, int nodeId, LengthAxis axis, int depth)
{
	const ResolvedCssLength resolved = resolveCompiledLengthForNodeDetailed(length, nodeId, axis, depth);
	if (resolved.isPercent) {
		const int basis = percentBasisForNode(nodeId, axis);
		return roundToInt(static_cast<double>(resolved.value) * basis / 1000.0);
	}
	return resolved.value;
}

bool compileBoxLengthSpecs(const std::string &value, CssCompiledValue &compiled, bool allowAuto = false)
{
	const auto parts = splitWords(value);
	if (parts.empty() || parts.size() > 4) return false;
	CssLengthSpec first;
	if (!parseCompiledLengthSpec(parts[0], first, allowAuto)) return false;
	CssLengthSpec second = first;
	CssLengthSpec third = first;
	CssLengthSpec fourth = second;
	if (parts.size() > 1 && !parseCompiledLengthSpec(parts[1], second, allowAuto)) return false;
	if (parts.size() > 2 && !parseCompiledLengthSpec(parts[2], third, allowAuto)) return false;
	if (parts.size() > 3 && !parseCompiledLengthSpec(parts[3], fourth, allowAuto)) return false;
	if (parts.size() <= 2) third = first;
	if (parts.size() <= 3) fourth = second;
	compiled.lengths[0] = first;
	compiled.lengths[1] = second;
	compiled.lengths[2] = third;
	compiled.lengths[3] = fourth;
	return true;
}

bool compileSimpleColor(const std::string &value, CssCompiledValue &compiled)
{
	const std::string text = trimCssValue(value);
	const std::string lower = toLowerAscii(text);
	if (lower.empty() || lower.find("gradient(") != std::string::npos || lower.find("var(") != std::string::npos)
		return false;
	const ParsedCssColor color = parseCssColor(firstColorToken(text));
	if (!color.valid) return false;
	compiled.values[0] = static_cast<std::int32_t>(cssColorStyleValue(color));
	compiled.values[1] = static_cast<std::int32_t>(cssColorNative(color));
	compiled.values[2] = color.a;
	return true;
}

bool compileColorVarValue(const std::string &value, CssCompiledValue &compiled)
{
	const std::string text = trimCssValue(value);
	const std::string lower = toLowerAscii(text);
	if (!startsWith(lower, "var(") || text.empty() || text.back() != ')') return false;
	const std::size_t open = text.find('(');
	if (open == std::string::npos || open + 1 >= text.size()) return false;
	const auto parts = splitTopLevel(text.substr(open + 1, text.size() - open - 2), ',');
	if (parts.empty()) return false;
	const std::string name = trimCssValue(parts[0]);
	if (name.rfind("--", 0) != 0) return false;
	compiled.values[0] = static_cast<std::int32_t>(internCssAtom(name));
	compiled.aux = 0;
	if (parts.size() > 1) {
		const CachedCssColor fallback = cachedCssColorForValue(parts[1]);
		if (!fallback.valid) return false;
		compiled.values[1] = fallback.styleColor;
		compiled.values[2] = fallback.nativeColor;
		compiled.values[3] = fallback.alpha;
		compiled.aux = 1;
	}
	return true;
}

struct CompiledCssColorRef {
	CachedCssColor color;
	CssAtomId atom = kInvalidCssAtom;
	std::uint8_t hasFallback = 0;
	bool valid = false;
};

bool compileDirectColorVarRef(const std::string &value, CssAtomId &atom, CachedCssColor &fallback, std::uint8_t &hasFallback)
{
	const std::string text = trimCssValue(value);
	const std::string lower = toLowerAscii(text);
	if (!startsWith(lower, "var(") || text.empty() || text.back() != ')') return false;
	const std::size_t open = text.find('(');
	if (open == std::string::npos || open + 1 >= text.size()) return false;
	const auto parts = splitTopLevel(text.substr(open + 1, text.size() - open - 2), ',');
	if (parts.empty()) return false;
	const std::string name = trimCssValue(parts[0]);
	if (name.rfind("--", 0) != 0) return false;
	atom = internCssAtom(name);
	hasFallback = 0;
	fallback = {};
	if (parts.size() > 1) {
		fallback = cachedCssColorForValue(parts[1]);
		if (!fallback.valid) return false;
		hasFallback = 1;
	}
	return true;
}

bool compileGradientColorStopRef(const std::string &stop, CompiledCssColorRef &out)
{
	const std::string token = gradientColorTokenForStop(stop);
	if (token.empty()) return false;
	const std::string lower = toLowerAscii(trimCssValue(token));
	if (startsWith(lower, "var(")) {
		if (!compileDirectColorVarRef(token, out.atom, out.color, out.hasFallback)) return false;
		out.valid = true;
		return true;
	}
	const CachedCssColor color = cachedCssColorForValue(token);
	if (!color.valid) return false;
	out.color = color;
	out.valid = true;
	return true;
}

void setCompiledLinearGradientFrom(CssCompiledLinearGradient &gradient, const CompiledCssColorRef &color)
{
	gradient.fromStyleColor = color.color.styleColor;
	gradient.fromNativeColor = static_cast<style_color_t>(color.color.nativeColor);
	gradient.fromAlpha = color.hasFallback || color.atom == kInvalidCssAtom ? color.color.alpha : 255;
	gradient.fromColorAtom = color.atom;
	gradient.fromColorHasFallback = color.hasFallback;
}

void setCompiledLinearGradientMid(CssCompiledLinearGradient &gradient, const CompiledCssColorRef &color)
{
	gradient.midNativeColor = static_cast<style_color_t>(color.color.nativeColor);
	gradient.midAlpha = color.hasFallback || color.atom == kInvalidCssAtom ? color.color.alpha : 255;
	gradient.midColorAtom = color.atom;
	gradient.midColorHasFallback = color.hasFallback;
}

void setCompiledLinearGradientTo(CssCompiledLinearGradient &gradient, const CompiledCssColorRef &color)
{
	gradient.toNativeColor = static_cast<style_color_t>(color.color.nativeColor);
	gradient.toAlpha = color.hasFallback || color.atom == kInvalidCssAtom ? color.color.alpha : 255;
	gradient.toColorAtom = color.atom;
	gradient.toColorHasFallback = color.hasFallback;
}

bool compileLineHeightValue(const std::string &value, CssCompiledValue &compiled)
{
	const std::string text = trimCssValue(value);
	if (text.empty()) return false;
	const std::string lower = toLowerAscii(text);
	if (lower == "normal") {
		compiled.aux = 0;
		return true;
	}
	char *end = nullptr;
	const double scalar = std::strtod(text.c_str(), &end);
	if (end && end != text.c_str()) {
		while (*end == ' ') ++end;
		if (*end == '\0') {
			compiled.aux = 1;
			compiled.lengths[0].value = static_cast<float>(scalar);
			compiled.lengths[0].unit = CssLengthUnit::Raw;
			return true;
		}
		if (end[0] == '%' && end[1] == '\0') {
			compiled.aux = 2;
			compiled.lengths[0].value = static_cast<float>(scalar);
			compiled.lengths[0].unit = CssLengthUnit::Percent;
			return true;
		}
	}
	if (!parseCompiledLengthSpec(text, compiled.lengths[0])) return false;
	compiled.aux = 3;
	return true;
}

bool compileRightFadeMaskWidthValue(const std::string &rawValue, CssCompiledValue &compiled)
{
	const std::string value = trimCssValue(rawValue);
	const std::string lower = toLowerAscii(value);
	if (lower.empty() || lower == "none") {
		compiled.lengths[0].unit = CssLengthUnit::Raw;
		compiled.lengths[0].value = 0.0f;
		return true;
	}
	if (lower.find("linear-gradient") == std::string::npos ||
	    lower.find("to right") == std::string::npos ||
	    lower.find("transparent") == std::string::npos) {
		return false;
	}

	const std::size_t calcStart = lower.find("calc(");
	if (calcStart == std::string::npos) return false;
	const std::size_t innerStart = calcStart + 5;
	int depth = 1;
	std::size_t end = innerStart;
	for (; end < value.size(); ++end) {
		if (value[end] == '(') depth++;
		else if (value[end] == ')') {
			if (--depth == 0) break;
		}
	}
	if (end <= innerStart || end >= value.size()) return false;

	const std::string inner = trimCssValue(value.substr(innerStart, end - innerStart));
	const std::string innerLower = toLowerAscii(inner);
	if (innerLower.rfind("100%", 0) != 0) return false;
	depth = 0;
	for (std::size_t i = 0; i < inner.size(); ++i) {
		const char c = inner[i];
		if (c == '(') depth++;
		else if (c == ')' && depth > 0) depth--;
		else if (c == '-' && depth == 0)
			return parseCompiledLengthSpec(inner.substr(i + 1), compiled.lengths[0]);
	}
	return false;
}

bool isUnsetFlexBasisToken(const std::string &value)
{
	const std::string lower = toLowerAscii(trimCssValue(value));
	return lower == "auto" ||
	       lower == "content" ||
	       lower == "max-content" ||
	       lower == "min-content" ||
	       lower == "fit-content" ||
	       lower.find('%') != std::string::npos;
}

bool compileFlexBasisValue(const std::string &value, CssCompiledValue &compiled)
{
	const std::string text = trimCssValue(value);
	if (text.empty()) return false;
	compiled.aux = 0;
	if (isUnsetFlexBasisToken(text)) return true;
	if (!parseCompiledLengthSpec(text, compiled.lengths[0])) return false;
	compiled.aux = 1;
	return true;
}

bool compileFlexShorthandValue(const std::string &value, CssCompiledValue &compiled)
{
	const std::string text = trimCssValue(value);
	if (text.empty()) return false;
	double grow = 0.0;
	double shrink = 1.0;
	int numberIndex = 0;
	CssLengthSpec basis;
	bool hasBasis = false;

	for (const auto &tokenRaw : splitFunctionAwareWords(text)) {
		const std::string token = trimCssValue(tokenRaw);
		const std::string lower = toLowerAscii(token);
		if (token.empty()) continue;
		if (lower == "none") {
			grow = 0.0;
			shrink = 0.0;
			hasBasis = false;
			continue;
		}
		if (isUnsetFlexBasisToken(token)) {
			hasBasis = false;
			continue;
		}
		char *endp = nullptr;
		const double num = std::strtod(token.c_str(), &endp);
		const bool parsedNumber = endp && endp != token.c_str();
		const bool hasUnit = parsedNumber && *endp != '\0';
		if (hasUnit) {
			CssLengthSpec candidate;
			if (!parseCompiledLengthSpec(token, candidate)) return false;
			basis = candidate;
			hasBasis = true;
			continue;
		}
		if (parsedNumber) {
			if (numberIndex == 0) grow = num;
			else if (numberIndex == 1) shrink = num;
			numberIndex++;
			continue;
		}
		return false;
	}

	compiled.values[0] = rawNumber(grow);
	compiled.values[1] = rawNumber(shrink);
	compiled.aux = hasBasis ? 1 : 0;
	if (hasBasis) compiled.lengths[0] = basis;
	return true;
}

bool compileFilterBlurValue(const std::string &value, CssCompiledValue &compiled)
{
	const std::string text = trimCssValue(value);
	const std::string lower = toLowerAscii(text);
	if (lower.empty() || lower == "none") {
		compiled.aux = 0;
		return true;
	}
	const std::size_t blur = lower.find("blur(");
	if (blur == std::string::npos) {
		compiled.aux = 0;
		return true;
	}
	std::size_t i = blur + 5;
	int depth = 1;
	const std::size_t argStart = i;
	while (i < text.size() && depth > 0) {
		if (text[i] == '(') depth++;
		else if (text[i] == ')') depth--;
		++i;
	}
	if (depth != 0 || i <= argStart) return false;
	if (!parseCompiledLengthSpec(text.substr(argStart, i - argStart - 1), compiled.lengths[0])) return false;
	compiled.aux = 1;
	return true;
}

bool compileBoxShadowValue(const std::string &value, CssCompiledValue &compiled)
{
	const std::string text = trimCssValue(value);
	const std::string lowerText = toLowerAscii(text);
	compiled.aux = 0;
	compiled.values[0] = 0;
	compiled.values[1] = 0;
	if (text.empty() || lowerText == "none") return true;

	for (const auto &rawLayer : splitTopLevel(text, ',')) {
		const auto tokens = splitFunctionAwareWords(rawLayer);
		bool inset = false;
		std::vector<CssLengthSpec> lengths;
		ParsedCssColor color;
		color.r = 0;
		color.g = 0;
		color.b = 0;
		color.a = 255;
		color.valid = true;
		for (const auto &token : tokens) {
			const std::string lower = toLowerAscii(trimCssValue(token));
			if (lower == "inset") {
				inset = true;
				continue;
			}
			if (isBoxShadowColorToken(token)) {
				const ParsedCssColor parsed = parseCssColor(firstColorToken(token));
				if (parsed.valid) color = parsed;
				continue;
			}
			if (lower == "outset") continue;
			CssLengthSpec length;
			if (!parseCompiledLengthSpec(token, length)) return false;
			if (lengths.size() < 4) lengths.push_back(length);
		}
		if (!inset) continue;
		compiled.aux = 1;
		for (std::size_t i = 0; i < lengths.size(); ++i) compiled.lengths[i] = lengths[i];
		compiled.values[0] = static_cast<std::int32_t>(cssColorStyleValue(color));
		compiled.values[1] = color.a;
		return true;
	}
	return true;
}

bool boxShadowValueHasInsetLayer(const std::string &value)
{
	for (const auto &rawLayer : splitTopLevel(value, ',')) {
		for (const auto &token : splitFunctionAwareWords(rawLayer)) {
			if (toLowerAscii(trimCssValue(token)) == "inset") return true;
		}
	}
	return false;
}

bool compileBorderShorthandValue(const std::string &value, CssCompiledValue &compiled)
{
	const auto parts = splitWords(value);
	if (parts.empty()) return false;
	if (!parseCompiledLengthSpec(parts[0], compiled.lengths[0])) return false;
	compiled.aux = 0;
	for (const auto &part : parts) {
		if (part.empty()) continue;
		if (part[0] == '#' || part.find("rgb") != std::string::npos) {
			const ParsedCssColor color = parseCssColor(firstColorToken(value));
			if (!color.valid) return false;
			compiled.aux = 1;
			compiled.values[0] = static_cast<std::int32_t>(cssColorStyleValue(color));
			compiled.values[1] = color.a;
			return true;
		}
	}
	return true;
}

int borderSideForDeclaration(CssDeclarationId declaration)
{
	switch (declaration) {
	case CssDeclarationId::BorderTop: return 0;
	case CssDeclarationId::BorderRight: return 1;
	case CssDeclarationId::BorderBottom: return 2;
	case CssDeclarationId::BorderLeft: return 3;
	default: return -1;
	}
}

bool compileBorderRadiusValue(const std::string &value, CssCompiledValue &compiled)
{
	const auto axes = splitTopLevel(value, '/');
	const auto parts = splitWords(axes.empty() ? value : axes[0]);
	if (parts.empty() || parts.size() > 4) return false;
	if (!parseCompiledLengthSpec(parts[0], compiled.lengths[0])) return false;
	compiled.lengths[1] = compiled.lengths[0];
	compiled.lengths[2] = compiled.lengths[0];
	compiled.lengths[3] = compiled.lengths[1];
	if (parts.size() > 1 && !parseCompiledLengthSpec(parts[1], compiled.lengths[1])) return false;
	if (parts.size() > 2 && !parseCompiledLengthSpec(parts[2], compiled.lengths[2])) return false;
	if (parts.size() > 3 && !parseCompiledLengthSpec(parts[3], compiled.lengths[3])) return false;
	if (parts.size() <= 2) compiled.lengths[2] = compiled.lengths[0];
	if (parts.size() <= 3) compiled.lengths[3] = compiled.lengths[1];
	return true;
}

bool compileGridTrackSpec(const std::string &rawToken, CssCompiledGridTrack &track)
{
	const std::string token = trimCssValue(rawToken);
	const std::string lower = toLowerAscii(token);
	if (token.empty()) return false;
	if (lower == "auto") {
		track.type = 0;
		track.value = 0;
		track.length = {};
		return true;
	}
	if (startsWith(lower, "minmax(")) {
		const auto parts = splitTopLevel(functionInner(token, "minmax"), ',');
		if (parts.empty()) return false;
		const std::string preferred = parts.size() > 1 ? parts[1] : parts[0];
		return compileGridTrackSpec(preferred, track);
	}
	if (lower.size() > 2 && lower.substr(lower.size() - 2) == "fr") {
		char *end = nullptr;
		const double fr = std::strtod(lower.c_str(), &end);
		if (end == lower.c_str()) return false;
		while (*end == ' ') ++end;
		if (end[0] != 'f' || end[1] != 'r') return false;
		track.type = 2;
		track.value = static_cast<std::int16_t>(std::max(1, rawNumber(fr)));
		track.length = {};
		return true;
	}
	CssLengthSpec length;
	if (!parseCompiledLengthSpec(token, length)) return false;
	track.type = 1;
	track.value = 0;
	track.length = length;
	return true;
}

bool appendCompiledGridTrackToken(const std::string &token, CssCompiledGridTemplate &grid)
{
	if (grid.count >= kMaxGridTracks) return true;
	CssCompiledGridTrack track;
	if (!compileGridTrackSpec(token, track)) return false;
	grid.tracks[grid.count++] = track;
	return true;
}

std::uint16_t storeCompiledCssGridTemplate(const CssCompiledGridTemplate &grid)
{
	auto &list = compiledCssGridTemplates();
	if (list.size() >= kNoCompiledCssGridTemplate) return kNoCompiledCssGridTemplate;
	list.push_back(grid);
	return static_cast<std::uint16_t>(list.size() - 1);
}

bool compileGridTemplateValue(const std::string &value, CssCompiledValue &compiled)
{
	CssCompiledGridTemplate grid;
	for (const auto &token : splitFunctionAwareWords(value)) {
		const std::string lower = toLowerAscii(trimCssValue(token));
		if (startsWith(lower, "repeat(")) {
			const auto parts = splitTopLevel(functionInner(token, "repeat"), ',');
			if (parts.size() >= 2) {
				int repeatCount = rawNumber(std::strtod(parts[0].c_str(), nullptr));
				if (repeatCount < 0) repeatCount = 0;
				if (repeatCount > kMaxGridTracks) repeatCount = kMaxGridTracks;
				for (int i = 0; i < repeatCount && grid.count < kMaxGridTracks; ++i) {
					if (!appendCompiledGridTrackToken(parts[1], grid)) return false;
				}
			}
			continue;
		}
		if (!appendCompiledGridTrackToken(token, grid)) return false;
	}
	const std::uint16_t handle = storeCompiledCssGridTemplate(grid);
	if (handle == kNoCompiledCssGridTemplate) return false;
	compiled.values[0] = static_cast<std::int32_t>(handle);
	return true;
}

bool sameCompiledLengthSpec(const CssLengthSpec &a, const CssLengthSpec &b)
{
	return a.unit == b.unit && std::fabs(static_cast<double>(a.value) - static_cast<double>(b.value)) < 0.0001;
}

bool compileColorStopLengthSpec(const std::string &stop, CssLengthSpec &out)
{
	const std::string token = gradientColorTokenForStop(stop);
	const std::size_t tokenStart = stop.find(token);
	const std::string rest = tokenStart == std::string::npos
	    ? std::string()
	    : trimCssValue(stop.substr(tokenStart + token.size()));
	if (rest.empty()) return false;
	const auto parts = splitWords(rest);
	if (parts.empty()) return false;
	return parseCompiledLengthSpec(parts[0], out);
}

bool compiledGridLineLengthIsSafe(const CssLengthSpec &length)
{
	if (!std::isfinite(static_cast<double>(length.value)) || length.value < 1.0f) return false;
	return length.unit == CssLengthUnit::Raw || length.unit == CssLengthUnit::Px;
}

CssCompiledLinearGradient compileLinearGradientLayer(const ParsedLinearGradient &gradient)
{
	CssCompiledLinearGradient out;
	out.fromStyleColor = static_cast<std::int32_t>(cssColorStyleValue(gradient.from));
	out.fromNativeColor = cssColorNative(gradient.from);
	out.midNativeColor = gradient.hasMid ? cssColorNative(gradient.mid) : 0;
	out.toNativeColor = cssColorNative(gradient.to);
	out.fromAlpha = static_cast<std::uint8_t>(gradient.from.a);
	out.midAlpha = static_cast<std::uint8_t>(gradient.hasMid ? gradient.mid.a : 255);
	out.toAlpha = static_cast<std::uint8_t>(gradient.to.a);
	out.midStopPermille = static_cast<std::uint16_t>(gradient.midStopPermille);
	out.toStopPermille = static_cast<std::uint16_t>(gradient.toStopPermille);
	out.hasMid = gradient.hasMid ? 1 : 0;
	out.angleTenths = static_cast<std::int16_t>(gradient.angleTenths);
	return out;
}

bool compileLinearGradientLayerValue(const std::string &value, CssCompiledLinearGradient &out)
{
	const std::string call = lastFunctionCall(value, "linear-gradient");
	if (call.empty()) return false;
	const std::string inner = functionInner(call, "linear-gradient");
	const auto parts = splitTopLevel(inner, ',');
	if (parts.size() < 2) return false;

	std::size_t colorStart = 0;
	int angleTenths = 1800;
	const std::string first = trimCssValue(parts[0]);
	if (first.find("deg") != std::string::npos || first.find("turn") != std::string::npos || first.find("rad") != std::string::npos) {
		angleTenths = numericRotateTenths(parseAngleDegrees(first));
		colorStart = 1;
	} else if (first.rfind("to ", 0) == 0) {
		const std::string direction = toLowerAscii(first);
		if (direction.find("right") != std::string::npos) angleTenths = 900;
		else if (direction.find("left") != std::string::npos) angleTenths = 2700;
		else if (direction.find("top") != std::string::npos) angleTenths = 0;
		else angleTenths = 1800;
		colorStart = 1;
	}
	if (colorStart >= parts.size()) return false;

	CompiledCssColorRef from;
	if (!compileGradientColorStopRef(parts[colorStart], from)) return false;
	setCompiledLinearGradientFrom(out, from);

	if (parts.size() == colorStart + 2) {
		const int fromStop = colorStopPermille(parts[colorStart], -1);
		if (fromStop > 0 && fromStop < 1000) {
			setCompiledLinearGradientMid(out, from);
			out.midStopPermille = static_cast<std::uint16_t>(fromStop);
			out.hasMid = 1;
		}
	} else if (parts.size() > colorStart + 2) {
		CompiledCssColorRef mid;
		if (!compileGradientColorStopRef(parts[colorStart + 1], mid)) return false;
		setCompiledLinearGradientMid(out, mid);
		out.midStopPermille = static_cast<std::uint16_t>(colorStopPermille(parts[colorStart + 1], 500));
		out.hasMid = 1;
	}

	CompiledCssColorRef to;
	if (!compileGradientColorStopRef(parts.back(), to)) return false;
	setCompiledLinearGradientTo(out, to);
	int toStopPermille = colorStopPermilleUnclamped(parts.back(), 1000);
	if (toStopPermille <= 0) toStopPermille = 1;
	if (out.hasMid && toStopPermille <= out.midStopPermille) toStopPermille = out.midStopPermille + 1;
	out.toStopPermille = static_cast<std::uint16_t>(toStopPermille);
	out.angleTenths = static_cast<std::int16_t>(angleTenths);
	return true;
}

CssCompiledRadialGradient compileRadialGradientLayer(const ParsedRadialGradient &gradient)
{
	CssCompiledRadialGradient out;
	out.fromNativeColor = cssColorNative(gradient.from);
	out.toNativeColor = cssColorNative(gradient.to);
	out.fromAlpha = static_cast<std::uint8_t>(gradient.from.a);
	out.toAlpha = static_cast<std::uint8_t>(gradient.to.a);
	out.stopPermille = static_cast<std::uint16_t>(gradient.stopPermille);
	out.cxPermille = static_cast<std::int16_t>(gradient.cxPermille);
	out.cyPermille = static_cast<std::int16_t>(gradient.cyPermille);
	out.rxPermille = static_cast<std::int16_t>(gradient.rxPermille);
	out.ryPermille = static_cast<std::int16_t>(gradient.ryPermille);
	return out;
}

bool compileGradientLineLayer(const std::string &value, CssCompiledBackground &background, bool &matchedLineLayer)
{
	matchedLineLayer = false;
	const std::string call = lastFunctionCall(value, "linear-gradient");
	if (call.empty()) return false;
	const std::string inner = functionInner(call, "linear-gradient");
	const auto parts = splitTopLevel(inner, ',');
	if (parts.size() < 2) return false;

	std::size_t colorStart = 0;
	int angleTenths = 1800;
	const std::string first = trimCssValue(parts[0]);
	if (first.find("deg") != std::string::npos || first.find("turn") != std::string::npos || first.find("rad") != std::string::npos) {
		angleTenths = numericRotateTenths(parseAngleDegrees(first));
		colorStart = 1;
	} else if (first.rfind("to ", 0) == 0) {
		const std::string direction = toLowerAscii(first);
		if (direction.find("right") != std::string::npos) angleTenths = 900;
		else if (direction.find("left") != std::string::npos) angleTenths = 2700;
		else if (direction.find("top") != std::string::npos) angleTenths = 0;
		else angleTenths = 1800;
		colorStart = 1;
	}
	if (colorStart + 1 >= parts.size()) return false;

	const ParsedCssColor firstColor = parseGradientColorStop(parts[colorStart]);
	const ParsedCssColor secondColor = parseGradientColorStop(parts[colorStart + 1]);
	CssLengthSpec firstLength;
	const bool hasFirstLength = compileColorStopLengthSpec(parts[colorStart], firstLength);
	if (hasFirstLength && secondColor.valid && secondColor.a == 0) matchedLineLayer = true;
	if (!firstColor.valid || !secondColor.valid || firstColor.a == 0 || secondColor.a != 0 ||
	    !hasFirstLength)
		return false;

	matchedLineLayer = true;
	if (!compiledGridLineLengthIsSafe(firstLength)) return false;
	CssLengthSpec secondLength;
	if (compileColorStopLengthSpec(parts[colorStart + 1], secondLength)) {
		if (!compiledGridLineLengthIsSafe(secondLength)) return false;
		if (!sameCompiledLengthSpec(firstLength, secondLength)) return false;
	}

	const bool vertical = gradientLineIsVertical(angleTenths);
	background.gridAxes |= vertical ? 1 : 2;
	background.gridColor = cssColorNative(firstColor);
	background.gridAlpha = static_cast<std::uint8_t>(firstColor.a);
	if (vertical) {
		background.gridLineX = firstLength;
		background.hasGridLineX = 1;
	} else {
		background.gridLineY = firstLength;
		background.hasGridLineY = 1;
	}
	return true;
}

std::uint16_t storeCompiledCssBackground(const CssCompiledBackground &background)
{
	auto &list = compiledCssBackgrounds();
	if (list.size() >= kNoCompiledCssBackground) return kNoCompiledCssBackground;
	list.push_back(background);
	return static_cast<std::uint16_t>(list.size() - 1);
}

bool compileBackgroundValue(const std::string &value, CssCompiledValue &compiled)
{
	const std::string lower = toLowerAscii(value);
	if (lower.find("calc(") != std::string::npos ||
	    lower.find("min(") != std::string::npos ||
	    lower.find("max(") != std::string::npos ||
	    lower.find("clamp(") != std::string::npos)
		return false;

	CssCompiledBackground background;
	const auto layers = splitTopLevel(value, ',');
	for (const auto &layerValue : layers) {
		bool matchedLineLayer = false;
		if (compileGradientLineLayer(layerValue, background, matchedLineLayer)) continue;
		if (matchedLineLayer) return false;

		CssCompiledLinearGradient layerGradient;
		if (compileLinearGradientLayerValue(layerValue, layerGradient)) {
			if (background.hasGradient) {
				background.overlayGradient = background.gradient;
				background.hasOverlayGradient = 1;
			}
			background.gradient = layerGradient;
			background.hasGradient = 1;
			continue;
		}
		const ParsedRadialGradient layerRadial = parseRadialGradient(layerValue);
		if (layerRadial.valid) {
			background.radialGradient = compileRadialGradientLayer(layerRadial);
			background.hasRadialGradient = 1;
			continue;
		}
		if (toLowerAscii(layerValue).find("var(") != std::string::npos) return false;
	}
	if (!background.hasGradient) return false;

	const std::uint16_t handle = storeCompiledCssBackground(background);
	if (handle == kNoCompiledCssBackground) return false;
	compiled.values[0] = handle;
	return true;
}

bool compileBackgroundSizeValue(const std::string &value, CssCompiledValue &compiled)
{
	const std::string lower = toLowerAscii(value);
	if (hasDynamicCssValue(lower)) return false;
	const auto layers = splitTopLevel(value, ',');
	if (layers.empty()) return false;
	const auto parts = splitWords(layers[0]);
	if (parts.empty()) return false;
	if (!parseCompiledLengthSpec(parts[0], compiled.lengths[0])) return false;
	if (parts.size() > 1) {
		if (!parseCompiledLengthSpec(parts[1], compiled.lengths[1])) return false;
	} else {
		compiled.lengths[1] = compiled.lengths[0];
	}
	return true;
}

bool compileKeywordValue(CssDeclarationId declaration, const std::string &value, CssCompiledValue &compiled)
{
	switch (declaration) {
	case CssDeclarationId::Display:
		compiled.values[0] = displayValue(value);
		return true;
	case CssDeclarationId::ObjectFit:
		compiled.values[0] = imageFitValue(value);
		return true;
	case CssDeclarationId::FlexDirection:
		compiled.values[0] = flexDirectionValue(value);
		return true;
	case CssDeclarationId::FlexWrap:
		compiled.values[0] = flexWrapValue(value);
		return true;
	case CssDeclarationId::JustifyContent:
	case CssDeclarationId::AlignItems:
	case CssDeclarationId::JustifyItems:
	case CssDeclarationId::AlignContent:
	case CssDeclarationId::PlaceItems:
		compiled.values[0] = flexAlignValue(value);
		return true;
	case CssDeclarationId::AlignSelf:
		compiled.values[0] = alignSelfValue(value);
		return true;
	case CssDeclarationId::Position:
		compiled.values[0] = positionValue(value);
		return true;
	case CssDeclarationId::TextAlign:
		compiled.values[0] = textAlignValue(value);
		return true;
	case CssDeclarationId::TextDecoration:
		compiled.values[0] = textDecorationValue(value);
		return true;
	case CssDeclarationId::TextTransform:
		compiled.values[0] = textTransformValue(value);
		return true;
	case CssDeclarationId::WhiteSpace:
		compiled.values[0] = whiteSpaceValue(value);
		return true;
	case CssDeclarationId::TextOverflow:
		compiled.values[0] = textOverflowValue(value);
		return true;
	case CssDeclarationId::BackfaceVisibility:
		compiled.values[0] = backfaceValue(value);
		return true;
	case CssDeclarationId::PointerEvents:
		compiled.values[0] = pointerEventsValue(value);
		return true;
	case CssDeclarationId::Overflow:
	case CssDeclarationId::OverflowX:
	case CssDeclarationId::OverflowY:
		compiled.values[0] = overflowValue(value);
		return true;
	case CssDeclarationId::FontFamily: {
		const std::string family = primaryFontFamily(value);
		compiled.values[0] = gea::framework::graphics::FontRegistry::familyId(family.c_str());
		return true;
	}
	case CssDeclarationId::FontWeight:
		compiled.values[0] = fontWeightValue(value);
		return true;
	default:
		return false;
	}
}

bool compileTransformLength(const std::string &value, CssLengthSpec &out)
{
	return parseCompiledLengthSpec(value, out);
}

bool compileTransformValue(const std::string &value, CssCompiledValue &compiled)
{
	std::size_t i = 0;
	bool sawTransform = false;
	while (i < value.size()) {
		while (i < value.size() && static_cast<unsigned char>(value[i]) <= ' ') ++i;
		const std::size_t nameStart = i;
		while (i < value.size() && (std::isalpha(static_cast<unsigned char>(value[i])) || value[i] == '3')) ++i;
		if (i == nameStart) {
			while (i < value.size() && static_cast<unsigned char>(value[i]) <= ' ') ++i;
			if (i >= value.size()) break;
			return false;
		}
		while (i < value.size() && static_cast<unsigned char>(value[i]) <= ' ') ++i;
		if (i >= value.size() || value[i] != '(') return false;
		const std::string name = toLowerAscii(value.substr(nameStart, i - nameStart));
		const std::size_t argStart = ++i;
		int depth = 1;
		while (i < value.size() && depth > 0) {
			if (value[i] == '(') depth++;
			else if (value[i] == ')') depth--;
			++i;
		}
		if (depth != 0) return false;
		const std::string arg = value.substr(argStart, i - argStart - 1);
		const auto args = splitTopLevel(arg, ',');
		sawTransform = true;

		if (name == "rotate" || name == "rotatez") {
			if (hasDynamicCssValue(arg)) return false;
			compiled.values[2] = numericRotateTenths(parseAngleDegrees(arg));
			compiled.flags |= 1u << 2;
		} else if (name == "rotatex") {
			if (hasDynamicCssValue(arg)) return false;
			compiled.values[0] = numericRotateTenths(parseAngleDegrees(arg));
			compiled.flags |= 1u << 0;
		} else if (name == "rotatey") {
			if (hasDynamicCssValue(arg)) return false;
			compiled.values[1] = numericRotateTenths(parseAngleDegrees(arg));
			compiled.flags |= 1u << 1;
		} else if (name == "translatex") {
			if (!compileTransformLength(arg, compiled.lengths[0])) return false;
			compiled.flags |= 1u << 3;
		} else if (name == "translatey") {
			if (!compileTransformLength(arg, compiled.lengths[1])) return false;
			compiled.flags |= 1u << 4;
		} else if (name == "translatez") {
			if (!compileTransformLength(arg, compiled.lengths[2])) return false;
			compiled.flags |= 1u << 5;
		} else if (name == "translate" || name == "translate3d") {
			if (args.empty()) return false;
			if (!compileTransformLength(args[0], compiled.lengths[0])) return false;
			compiled.flags |= 1u << 3;
			if (args.size() > 1) {
				if (!compileTransformLength(args[1], compiled.lengths[1])) return false;
				compiled.flags |= 1u << 4;
			}
			if (args.size() > 2) {
				if (!compileTransformLength(args[2], compiled.lengths[2])) return false;
				compiled.flags |= 1u << 5;
			}
		} else if (name == "scale") {
			if (hasDynamicCssValue(arg)) return false;
			const int sx = parseScalePermille(args.empty() ? arg : args[0]);
			const int sy = parseScalePermille(args.size() > 1 ? args[1] : (args.empty() ? arg : args[0]));
			compiled.values[8] = sx;
			compiled.values[9] = sy;
			compiled.flags |= (1u << 8) | (1u << 9);
		} else if (name == "scalex") {
			if (hasDynamicCssValue(arg)) return false;
			compiled.values[8] = parseScalePermille(arg);
			compiled.flags |= 1u << 8;
		} else if (name == "scaley") {
			if (hasDynamicCssValue(arg)) return false;
			compiled.values[9] = parseScalePermille(arg);
			compiled.flags |= 1u << 9;
		} else {
			return false;
		}
	}
	return sawTransform;
}

std::uint16_t storeCompiledCssValue(const CssCompiledValue &compiled)
{
	if (compiled.kind == CssCompiledKind::None) return kNoCompiledCssValue;
	auto &list = compiledCssValues();
	if (list.size() >= kNoCompiledCssValue) return kNoCompiledCssValue;
	list.push_back(compiled);
	return static_cast<std::uint16_t>(list.size() - 1);
}

std::uint16_t compileCustomPropertyValue(const CssText &rawValue)
{
	if (rawValue.empty()) return kNoCompiledCssValue;
	const std::string value = rawValue.trimmedStr();
	if (value.empty()) return kNoCompiledCssValue;

	CssCompiledValue compiled;
	compiled.declaration = CssDeclarationId::Custom;

	const CachedCssColor color = cachedCssColorForValue(value);
	if (color.valid) {
		compiled.kind = CssCompiledKind::Color;
		compiled.values[0] = color.styleColor;
		compiled.values[1] = color.nativeColor;
		compiled.values[2] = color.alpha;
		return storeCompiledCssValue(compiled);
	}

	CssLengthSpec length;
	if (parseCompiledLengthSpec(value, length)) {
		compiled.kind = CssCompiledKind::Length;
		compiled.lengths[0] = length;
		return storeCompiledCssValue(compiled);
	}

	return kNoCompiledCssValue;
}

std::uint16_t compileCssValue(CssDeclarationId declaration, const CssText &rawValue)
{
	if (declaration == CssDeclarationId::Unknown || declaration == CssDeclarationId::Custom || rawValue.empty())
		return kNoCompiledCssValue;
	if (static_cast<std::uint32_t>(GEA_COMPILED_CSS_VALUE_MASK) == 0u) return kNoCompiledCssValue;

	const std::string value = rawValue.trimmedStr();
	if (value.empty()) return kNoCompiledCssValue;
	const bool hasVar = rawValue.hasVarReference();

	CssCompiledValue compiled;
	compiled.declaration = declaration;

	if (declaration == CssDeclarationId::Ignored ||
	    declaration == CssDeclarationId::Content ||
	    declaration == CssDeclarationId::Animation) {
		compiled.kind = CssCompiledKind::Noop;
		return storeCompiledCssValue(compiled);
	}

	if (!hasVar && compiledCssValueFeatureEnabled(kCssCompiledFeatureKeyword) && compileKeywordValue(declaration, value, compiled)) {
		compiled.kind = CssCompiledKind::Keyword;
		return storeCompiledCssValue(compiled);
	}

	switch (declaration) {
	case CssDeclarationId::Opacity:
		if (hasVar) return kNoCompiledCssValue;
		if (!compiledCssValueFeatureEnabled(kCssCompiledFeatureOpacity)) return kNoCompiledCssValue;
		compiled.kind = CssCompiledKind::Opacity;
		compiled.values[0] = parseOpacity(value);
		return storeCompiledCssValue(compiled);
	case CssDeclarationId::ZIndex:
	case CssDeclarationId::FlexGrow:
	case CssDeclarationId::FlexShrink:
	case CssDeclarationId::FontWeight:
		if (hasVar) return kNoCompiledCssValue;
		if (!compiledCssValueFeatureEnabled(kCssCompiledFeatureNumber)) return kNoCompiledCssValue;
		compiled.kind = CssCompiledKind::Number;
		compiled.values[0] = declaration == CssDeclarationId::FontWeight
		                         ? fontWeightValue(value)
		                         : rawNumber(std::strtod(value.c_str(), nullptr));
		return storeCompiledCssValue(compiled);
	case CssDeclarationId::Flex:
		if (hasVar) return kNoCompiledCssValue;
		if (!compiledCssValueFeatureEnabled(kCssCompiledFeatureNumber) ||
		    !compiledCssValueFeatureEnabled(kCssCompiledFeatureLength))
			return kNoCompiledCssValue;
		if (!compileFlexShorthandValue(value, compiled)) return kNoCompiledCssValue;
		compiled.kind = CssCompiledKind::Flex;
		return storeCompiledCssValue(compiled);
	case CssDeclarationId::FlexBasis:
		if (hasVar) return kNoCompiledCssValue;
		if (!compiledCssValueFeatureEnabled(kCssCompiledFeatureLength)) return kNoCompiledCssValue;
		if (!compileFlexBasisValue(value, compiled)) return kNoCompiledCssValue;
		compiled.kind = CssCompiledKind::FlexBasis;
		return storeCompiledCssValue(compiled);
	case CssDeclarationId::Color:
	case CssDeclarationId::ActiveBackgroundColor:
	case CssDeclarationId::BorderColor:
	case CssDeclarationId::BorderTopColor:
	case CssDeclarationId::BorderRightColor:
	case CssDeclarationId::BorderBottomColor:
	case CssDeclarationId::BorderLeftColor:
		if (!compiledCssValueFeatureEnabled(kCssCompiledFeatureColor)) return kNoCompiledCssValue;
		if (hasVar) {
			if (!compileColorVarValue(value, compiled)) return kNoCompiledCssValue;
			compiled.kind = CssCompiledKind::ColorVar;
		} else {
			if (!compileSimpleColor(value, compiled)) return kNoCompiledCssValue;
			compiled.kind = CssCompiledKind::Color;
		}
		return storeCompiledCssValue(compiled);
	case CssDeclarationId::Background:
		if (compiledCssValueFeatureEnabled(kCssCompiledFeatureColor)) {
			if (hasVar) {
				if (compileColorVarValue(value, compiled)) {
					compiled.kind = CssCompiledKind::ColorVar;
					return storeCompiledCssValue(compiled);
				}
			} else if (compileSimpleColor(value, compiled)) {
				compiled.kind = CssCompiledKind::Color;
				return storeCompiledCssValue(compiled);
			}
		}
		if (!compiledCssValueFeatureEnabled(kCssCompiledFeatureBackground)) return kNoCompiledCssValue;
		if (hasVar && !compiledCssValueFeatureEnabled(kCssCompiledFeatureColor)) return kNoCompiledCssValue;
		if (!compileBackgroundValue(value, compiled)) return kNoCompiledCssValue;
		compiled.kind = CssCompiledKind::Background;
		return storeCompiledCssValue(compiled);
	case CssDeclarationId::BackgroundSize:
		if (hasVar) return kNoCompiledCssValue;
		if (!compiledCssValueFeatureEnabled(kCssCompiledFeatureBackground)) return kNoCompiledCssValue;
		if (!compileBackgroundSizeValue(value, compiled)) return kNoCompiledCssValue;
		compiled.kind = CssCompiledKind::BackgroundSize;
		return storeCompiledCssValue(compiled);
	case CssDeclarationId::MaskImage:
		if (hasVar) return kNoCompiledCssValue;
		if (!compiledCssValueFeatureEnabled(kCssCompiledFeatureLength)) return kNoCompiledCssValue;
		if (!compileRightFadeMaskWidthValue(value, compiled)) return kNoCompiledCssValue;
		compiled.kind = CssCompiledKind::Length;
		return storeCompiledCssValue(compiled);
	case CssDeclarationId::Border:
		if (hasVar) return kNoCompiledCssValue;
		if (!compiledCssValueFeatureEnabled(kCssCompiledFeatureLength)) return kNoCompiledCssValue;
		if (!compileBorderShorthandValue(value, compiled)) return kNoCompiledCssValue;
		if (compiled.aux != 0 && !compiledCssValueFeatureEnabled(kCssCompiledFeatureColor))
			return kNoCompiledCssValue;
		compiled.kind = CssCompiledKind::BorderShorthand;
		return storeCompiledCssValue(compiled);
	case CssDeclarationId::BorderTop:
	case CssDeclarationId::BorderRight:
	case CssDeclarationId::BorderBottom:
	case CssDeclarationId::BorderLeft:
		if (hasVar) return kNoCompiledCssValue;
		if (!compiledCssValueFeatureEnabled(kCssCompiledFeatureLength)) return kNoCompiledCssValue;
		if (!compileBorderShorthandValue(value, compiled)) return kNoCompiledCssValue;
		if (compiled.aux != 0 && !compiledCssValueFeatureEnabled(kCssCompiledFeatureColor))
			return kNoCompiledCssValue;
		compiled.kind = CssCompiledKind::BorderSideShorthand;
		return storeCompiledCssValue(compiled);
	case CssDeclarationId::BorderRadius:
		if (hasVar) return kNoCompiledCssValue;
		if (!compiledCssValueFeatureEnabled(kCssCompiledFeatureLength)) return kNoCompiledCssValue;
		if (!compileBorderRadiusValue(value, compiled)) return kNoCompiledCssValue;
		compiled.kind = CssCompiledKind::BorderRadius;
		return storeCompiledCssValue(compiled);
	case CssDeclarationId::BorderTopLeftRadius:
	case CssDeclarationId::BorderTopRightRadius:
	case CssDeclarationId::BorderBottomRightRadius:
	case CssDeclarationId::BorderBottomLeftRadius:
		if (!compiledCssValueFeatureEnabled(kCssCompiledFeatureLength)) return kNoCompiledCssValue;
		if (!parseCompiledLengthSpec(value, compiled.lengths[0])) return kNoCompiledCssValue;
		compiled.kind = CssCompiledKind::BorderRadius;
		return storeCompiledCssValue(compiled);
	case CssDeclarationId::Width:
	case CssDeclarationId::Height:
		if (!compiledCssValueFeatureEnabled(kCssCompiledFeatureSize)) return kNoCompiledCssValue;
		if (!parseCompiledLengthSpec(value, compiled.lengths[0], true)) return kNoCompiledCssValue;
		compiled.kind = CssCompiledKind::Size;
		return storeCompiledCssValue(compiled);
	case CssDeclarationId::Top:
	case CssDeclarationId::Right:
	case CssDeclarationId::Bottom:
	case CssDeclarationId::Left:
		if (!compiledCssValueFeatureEnabled(kCssCompiledFeaturePosition)) return kNoCompiledCssValue;
		if (!parseCompiledLengthSpec(value, compiled.lengths[0])) return kNoCompiledCssValue;
		compiled.kind = CssCompiledKind::PositionOffset;
		return storeCompiledCssValue(compiled);
	case CssDeclarationId::Padding:
	case CssDeclarationId::Margin:
	case CssDeclarationId::Inset:
		if (hasVar) return kNoCompiledCssValue;
		if (!compiledCssValueFeatureEnabled(kCssCompiledFeatureBox)) return kNoCompiledCssValue;
		if (!compileBoxLengthSpecs(value, compiled)) return kNoCompiledCssValue;
		compiled.kind = CssCompiledKind::Box;
		return storeCompiledCssValue(compiled);
	case CssDeclarationId::Gap:
	case CssDeclarationId::MinWidth:
	case CssDeclarationId::MinHeight:
	case CssDeclarationId::MaxWidth:
	case CssDeclarationId::MaxHeight:
	case CssDeclarationId::PaddingTop:
	case CssDeclarationId::PaddingRight:
	case CssDeclarationId::PaddingBottom:
	case CssDeclarationId::PaddingLeft:
	case CssDeclarationId::MarginTop:
	case CssDeclarationId::MarginRight:
	case CssDeclarationId::MarginBottom:
	case CssDeclarationId::MarginLeft:
	case CssDeclarationId::BorderWidth:
	case CssDeclarationId::BorderTopWidth:
	case CssDeclarationId::BorderRightWidth:
	case CssDeclarationId::BorderBottomWidth:
	case CssDeclarationId::BorderLeftWidth:
	case CssDeclarationId::FontSize:
	case CssDeclarationId::Perspective:
		if (!compiledCssValueFeatureEnabled(kCssCompiledFeatureLength)) return kNoCompiledCssValue;
		if (!parseCompiledLengthSpec(value, compiled.lengths[0])) return kNoCompiledCssValue;
		compiled.kind = CssCompiledKind::Length;
		return storeCompiledCssValue(compiled);
	case CssDeclarationId::LineHeight:
		if (hasVar) return kNoCompiledCssValue;
		if (!compiledCssValueFeatureEnabled(kCssCompiledFeatureLength)) return kNoCompiledCssValue;
		if (!compileLineHeightValue(value, compiled)) return kNoCompiledCssValue;
		compiled.kind = CssCompiledKind::LineHeight;
		return storeCompiledCssValue(compiled);
	case CssDeclarationId::TransformOrigin:
	case CssDeclarationId::PerspectiveOrigin: {
		if (hasVar) return kNoCompiledCssValue;
		if (!compiledCssValueFeatureEnabled(kCssCompiledFeatureOrigin)) return kNoCompiledCssValue;
		const auto parts = splitWords(value);
		compiled.kind = CssCompiledKind::OriginPair;
		compiled.values[0] = parseOriginPart(parts.empty() ? "" : parts[0], 500);
		compiled.values[1] = parseOriginPart(parts.size() < 2 ? "" : parts[1], 500);
		return storeCompiledCssValue(compiled);
	}
	case CssDeclarationId::Rotate:
		if (hasVar) return kNoCompiledCssValue;
		if (!compiledCssValueFeatureEnabled(kCssCompiledFeatureTransformScalar)) return kNoCompiledCssValue;
		compiled.kind = CssCompiledKind::Rotate;
		compiled.values[0] = parseRotateTenths(value);
		return storeCompiledCssValue(compiled);
	case CssDeclarationId::Scale:
		if (hasVar) return kNoCompiledCssValue;
		if (!compiledCssValueFeatureEnabled(kCssCompiledFeatureTransformScalar)) return kNoCompiledCssValue;
		compiled.kind = CssCompiledKind::Scale;
		compiled.values[0] = parseScalePermille(value);
		return storeCompiledCssValue(compiled);
	case CssDeclarationId::Transform: {
		if (!compiledCssValueFeatureEnabled(kCssCompiledFeatureTransform)) return kNoCompiledCssValue;
		compiled.kind = CssCompiledKind::Transform;
		compiled.values[8] = 1000;
		compiled.values[9] = 1000;
		if (!compileTransformValue(value, compiled)) return kNoCompiledCssValue;
		return storeCompiledCssValue(compiled);
	}
	case CssDeclarationId::Filter:
		if (hasVar && toLowerAscii(value).find("blur(") == std::string::npos) return kNoCompiledCssValue;
		if (!compiledCssValueFeatureEnabled(kCssCompiledFeatureEffects)) return kNoCompiledCssValue;
		if (!compileFilterBlurValue(value, compiled)) return kNoCompiledCssValue;
		compiled.kind = CssCompiledKind::FilterBlur;
		return storeCompiledCssValue(compiled);
	case CssDeclarationId::BoxShadow:
		if (!compiledCssValueFeatureEnabled(kCssCompiledFeatureEffects)) return kNoCompiledCssValue;
		if (hasDynamicCssValue(value)) {
			if (boxShadowValueHasInsetLayer(value)) return kNoCompiledCssValue;
			compiled.kind = CssCompiledKind::BoxShadow;
			compiled.aux = 0;
			return storeCompiledCssValue(compiled);
		}
		if (!compileBoxShadowValue(value, compiled)) return kNoCompiledCssValue;
		compiled.kind = CssCompiledKind::BoxShadow;
		return storeCompiledCssValue(compiled);
	case CssDeclarationId::GridTemplateColumns:
	case CssDeclarationId::GridTemplateRows:
		if (hasVar) return kNoCompiledCssValue;
		if (!compiledCssValueFeatureEnabled(kCssCompiledFeatureGridTemplate)) return kNoCompiledCssValue;
		if (!compileGridTemplateValue(value, compiled)) return kNoCompiledCssValue;
		compiled.kind = CssCompiledKind::GridTemplate;
		return storeCompiledCssValue(compiled);
	default:
		return kNoCompiledCssValue;
	}
}

bool parseGridTrack(const std::string &rawToken, int nodeId, LengthAxis axis, int8_t &type, int16_t &trackValue)
{
	const std::string token = trimCssValue(rawToken);
	const std::string lower = toLowerAscii(token);
	if (token.empty()) return false;
	if (lower == "auto") {
		type = 0;
		trackValue = 0;
		return true;
	}
	if (startsWith(lower, "minmax(")) {
		const auto parts = splitTopLevel(functionInner(token, "minmax"), ',');
		if (parts.empty()) return false;
		const std::string preferred = parts.size() > 1 ? parts[1] : parts[0];
		return parseGridTrack(preferred, nodeId, axis, type, trackValue);
	}
	if (lower.size() > 2 && lower.substr(lower.size() - 2) == "fr") {
		type = 2;
		const double fr = std::strtod(lower.c_str(), nullptr);
		trackValue = static_cast<int16_t>(std::max(1, rawNumber(fr)));
		return true;
	}
	type = 1;
	trackValue = static_cast<int16_t>(parseLengthForNode(token, nodeId, axis));
	return true;
}

void appendGridTrackToken(const std::string &token,
                          int nodeId,
                          LengthAxis axis,
                          int8_t *types,
                          int16_t *values,
                          int &count)
{
	if (count >= kMaxGridTracks) return;
	int8_t type = 0;
	int16_t trackValue = 0;
	if (!parseGridTrack(token, nodeId, axis, type, trackValue)) return;
	types[count] = type;
	values[count] = trackValue;
	count++;
}

void parseGridTemplate(const std::string &value,
                       int nodeId,
                       LengthAxis axis,
                       int8_t *types,
                       int16_t *values,
                       int8_t &outCount)
{
	for (int i = 0; i < kMaxGridTracks; ++i) {
		types[i] = 0;
		values[i] = 0;
	}
	int count = 0;
	const auto tokens = splitFunctionAwareWords(value);
	for (const auto &token : tokens) {
		const std::string lower = toLowerAscii(trimCssValue(token));
		if (startsWith(lower, "repeat(")) {
			const auto parts = splitTopLevel(functionInner(token, "repeat"), ',');
			if (parts.size() >= 2) {
				int repeatCount = rawNumber(std::strtod(parts[0].c_str(), nullptr));
				if (repeatCount < 0) repeatCount = 0;
				if (repeatCount > kMaxGridTracks) repeatCount = kMaxGridTracks;
				for (int i = 0; i < repeatCount && count < kMaxGridTracks; ++i)
					appendGridTrackToken(parts[1], nodeId, axis, types, values, count);
			}
			continue;
		}
		appendGridTrackToken(token, nodeId, axis, types, values, count);
	}
	outCount = static_cast<int8_t>(count);
}

bool styleApplyInvalidationSuppressed()
{
	return treeState().styleInvalidationSuppressionDepth > 0;
}

void markNodeDisplayCommandsDirtyForStyleApply(int nodeId)
{
	if (styleApplyInvalidationSuppressed()) return;
	Tree::instance().markNodeDisplayCommandsDirty(nodeId);
}

void markDisplayListDirtyForStyleApply()
{
	if (styleApplyInvalidationSuppressed()) return;
	Tree::instance().markDisplayListDirty();
}

void applyGridTemplateValue(NodeHandle node, const std::string &value, bool columns)
{
	if (!node) return;
	Node &target = treeState().nodes[node.id()];
	RareStyle &rs = rstyleMut(target.style);
	if (columns) {
		parseGridTemplate(value,
		                  node.id(),
		                  LengthAxis::Horizontal,
		                  rs.grid_column_type,
		                  rs.grid_column_value,
		                  rs.grid_column_count);
	} else {
		parseGridTemplate(value,
		                  node.id(),
		                  LengthAxis::Vertical,
		                  rs.grid_row_type,
		                  rs.grid_row_value,
		                  rs.grid_row_count);
	}
	target.render.dirty = 1;
	target.render.layout_dirty = 1;
	target.render.non_scroll_dirty = 1;
	markDisplayListDirtyForStyleApply();
}

void applyCompiledGridTemplateValue(NodeHandle node, const CssCompiledGridTemplate &grid, bool columns)
{
	if (!node) return;
	const int nodeId = node.id();
	Node &target = treeState().nodes[nodeId];
	const LengthAxis axis = columns ? LengthAxis::Horizontal : LengthAxis::Vertical;
	RareStyle &rs = rstyleMut(target.style);
	int8_t *types = columns ? rs.grid_column_type : rs.grid_row_type;
	int16_t *values = columns ? rs.grid_column_value : rs.grid_row_value;
	int8_t &count = columns ? rs.grid_column_count : rs.grid_row_count;
	for (int i = 0; i < kMaxGridTracks; ++i) {
		types[i] = 0;
		values[i] = 0;
	}
	const int trackCount = std::min<int>(grid.count, kMaxGridTracks);
	for (int i = 0; i < trackCount; ++i) {
		const CssCompiledGridTrack &track = grid.tracks[i];
		types[i] = track.type;
		values[i] = track.type == 1
		    ? static_cast<std::int16_t>(resolveCompiledLengthForNode(track.length, nodeId, axis))
		    : track.value;
	}
	count = static_cast<std::int8_t>(trackCount);
	target.render.dirty = 1;
	target.render.layout_dirty = 1;
	target.render.non_scroll_dirty = 1;
	markDisplayListDirtyForStyleApply();
}

bool setClassRuleValueFastUnchecked(Node &target, Property property, int value)
{
	ComputedStyle &style = target.style;
	switch (property) {
	case Property::Display:
		style.display_explicit = 1;
		style.display = value;
		return true;
	case Property::FlexDirection:
		style.flex_direction_explicit = 1;
		style.flex_direction = value;
		return true;
	case Property::FlexWrap: style.flex_wrap = value; return true;
	case Property::JustifyContent: style.justify_content = value; return true;
	case Property::AlignItems: style.align_items = value; return true;
	case Property::JustifyItems: style.justify_items = value; return true;
	case Property::AlignContent: style.align_content = value; return true;
	case Property::AlignSelf: style.align_self = value; return true;
	case Property::Gap: style.gap = value; return true;
	case Property::Width:
		style.width = value;
		style.width_percent = kUnset;
		return true;
	case Property::Height:
		style.height = value;
		style.height_percent = kUnset;
		return true;
	case Property::WidthPercent:
		style.width_percent = value;
		style.width = kUnset;
		return true;
	case Property::HeightPercent:
		style.height_percent = value;
		style.height = kUnset;
		return true;
	case Property::MinWidth: style.min_width = value; return true;
	case Property::MinHeight: style.min_height = value; return true;
	case Property::MaxWidth: style.max_width = value; return true;
	case Property::MaxHeight: style.max_height = value; return true;
	case Property::Flex: style.flex = value; return true;
	case Property::FlexShrink: style.flex_shrink = value; return true;
	case Property::FlexBasis: style.flex_basis = value; return true;
	case Property::PaddingTop: style.padding[0] = value; return true;
	case Property::PaddingRight: style.padding[1] = value; return true;
	case Property::PaddingBottom: style.padding[2] = value; return true;
	case Property::PaddingLeft: style.padding[3] = value; return true;
	case Property::MarginTop: style.margin[0] = value; return true;
	case Property::MarginRight: style.margin[1] = value; return true;
	case Property::MarginBottom: style.margin[2] = value; return true;
	case Property::MarginLeft: style.margin[3] = value; return true;
	case Property::Position: style.position = value; return true;
	case Property::Top:
		style.pos_offsets[0] = value;
		style.pos_offset_percent[0] = kUnset;
		return true;
	case Property::Right:
		style.pos_offsets[1] = value;
		style.pos_offset_percent[1] = kUnset;
		return true;
	case Property::Bottom:
		style.pos_offsets[2] = value;
		style.pos_offset_percent[2] = kUnset;
		return true;
	case Property::Left:
		style.pos_offsets[3] = value;
		style.pos_offset_percent[3] = kUnset;
		return true;
	case Property::TopPercent:
		style.pos_offset_percent[0] = value;
		style.pos_offsets[0] = kUnset;
		return true;
	case Property::RightPercent:
		style.pos_offset_percent[1] = value;
		style.pos_offsets[1] = kUnset;
		return true;
	case Property::BottomPercent:
		style.pos_offset_percent[2] = value;
		style.pos_offsets[2] = kUnset;
		return true;
	case Property::LeftPercent:
		style.pos_offset_percent[3] = value;
		style.pos_offsets[3] = kUnset;
		return true;
	case Property::ZIndex: style.z_index = value; return true;
	case Property::BackgroundColor: {
		style.bg_color = StyleValues::pixelFromStyleValue(value);
		style.bg_alpha = 255;
		style.bg_fill = 0;
		RareStyle &rs = rstyleMut(style);
		rs.bg_gradient_has_mid = 0;
		rs.bg_overlay_gradient = 0;
		rs.bg_radial_gradient = 0;
		rs.bg_grid_axes = 0;
		return true;
	}
	case Property::HasBackground: style.has_bg = value; return true;
	case Property::ActiveBackgroundColor:
		style.active_bg_color = StyleValues::pixelFromStyleValue(value);
		return true;
	case Property::HasActiveBackground: style.has_active_bg = value; return true;
	case Property::Color:
		style.text_color = StyleValues::pixelFromStyleValue(value);
		style.text_alpha = 255;
		return true;
	case Property::Opacity:
		style.opacity = static_cast<std::uint8_t>(value < 0 ? 0 : value > 255 ? 255 : value);
		return true;
	case Property::BorderWidth: style.border_width = value; return true;
	case Property::BorderColor:
		style.border_color = StyleValues::pixelFromStyleValue(value);
		style.border_alpha = 255;
		return true;
	case Property::BorderTopWidth: rstyleMut(style).border_side_width[0] = value; return true;
	case Property::BorderRightWidth: rstyleMut(style).border_side_width[1] = value; return true;
	case Property::BorderBottomWidth: rstyleMut(style).border_side_width[2] = value; return true;
	case Property::BorderLeftWidth: rstyleMut(style).border_side_width[3] = value; return true;
	case Property::BorderTopColor:
		rstyleMut(style).border_side_color[0] = StyleValues::pixelFromStyleValue(value);
		rstyleMut(style).border_side_alpha[0] = 255;
		return true;
	case Property::BorderRightColor:
		rstyleMut(style).border_side_color[1] = StyleValues::pixelFromStyleValue(value);
		rstyleMut(style).border_side_alpha[1] = 255;
		return true;
	case Property::BorderBottomColor:
		rstyleMut(style).border_side_color[2] = StyleValues::pixelFromStyleValue(value);
		rstyleMut(style).border_side_alpha[2] = 255;
		return true;
	case Property::BorderLeftColor:
		rstyleMut(style).border_side_color[3] = StyleValues::pixelFromStyleValue(value);
		rstyleMut(style).border_side_alpha[3] = 255;
		return true;
	case Property::BorderRadiusTopLeft:
		style.border_radius[0] = value;
		style.border_radius_percent[0] = kUnset;
		return true;
	case Property::BorderRadiusTopRight:
		style.border_radius[1] = value;
		style.border_radius_percent[1] = kUnset;
		return true;
	case Property::BorderRadiusBottomRight:
		style.border_radius[2] = value;
		style.border_radius_percent[2] = kUnset;
		return true;
	case Property::BorderRadiusBottomLeft:
		style.border_radius[3] = value;
		style.border_radius_percent[3] = kUnset;
		return true;
	case Property::BorderRadiusTopLeftPercent:
		style.border_radius_percent[0] = value;
		style.border_radius[0] = 0;
		return true;
	case Property::BorderRadiusTopRightPercent:
		style.border_radius_percent[1] = value;
		style.border_radius[1] = 0;
		return true;
	case Property::BorderRadiusBottomRightPercent:
		style.border_radius_percent[2] = value;
		style.border_radius[2] = 0;
		return true;
	case Property::BorderRadiusBottomLeftPercent:
		style.border_radius_percent[3] = value;
		style.border_radius[3] = 0;
		return true;
	case Property::FontId: style.font_id = value; return true;
	case Property::FontSize: style.font_size = value; return true;
	case Property::FontWeight: style.font_weight = value; return true;
	case Property::LineHeight: style.line_height = value; return true;
	case Property::TextAlign: style.text_align = value; return true;
	case Property::TextDecoration: style.text_decoration = value; return true;
	case Property::TextTransform: style.text_transform = value; return true;
	case Property::WhiteSpace: style.white_space = static_cast<std::int8_t>(value); return true;
	case Property::TextOverflow: style.text_overflow = static_cast<std::int8_t>(value); return true;
	case Property::Backface: style.backface_hidden = static_cast<std::int8_t>(value); return true;
	case Property::PointerEvents: style.pointer_events = static_cast<std::int8_t>(value); return true;
	case Property::Overflow: {
		const auto next = static_cast<std::int8_t>(value);
		style.overflow = next;
		style.overflow_x = next;
		style.overflow_y = next;
		return true;
	}
	case Property::OverflowX: {
		const auto next = static_cast<std::int8_t>(value);
		style.overflow_x = next;
		style.overflow = aggregateOverflow(next, style.overflow_y);
		return true;
	}
	case Property::OverflowY: {
		const auto next = static_cast<std::int8_t>(value);
		style.overflow_y = next;
		style.overflow = aggregateOverflow(style.overflow_x, next);
		return true;
	}
	case Property::MaskRightFadeWidth:
		style.mask_right_fade_width = static_cast<std::int16_t>(value < 0 ? 0 : value > 32767 ? 32767 : value);
		return true;
	case Property::ImageId: target.image_id = value; return true;
	case Property::ImageFit: style.image_fit = value; return true;
	case Property::TransformRotate: rstyleMut(style).transform_rotate = value; return true;
	case Property::TransformRotateX: rstyleMut(style).transform_rotate_x = value; return true;
	case Property::TransformRotateY: rstyleMut(style).transform_rotate_y = value; return true;
	case Property::TransformTranslateX: rstyleMut(style).transform_translate_x = value; return true;
	case Property::TransformTranslateY: rstyleMut(style).transform_translate_y = value; return true;
	case Property::TransformTranslateZ: rstyleMut(style).transform_translate_z = value; return true;
	case Property::TransformTranslateXPercent: rstyleMut(style).transform_translate_x_percent = value; return true;
	case Property::TransformTranslateYPercent: rstyleMut(style).transform_translate_y_percent = value; return true;
	case Property::TransformScaleX: rstyleMut(style).transform_scale_x = value; return true;
	case Property::TransformScaleY: rstyleMut(style).transform_scale_y = value; return true;
	case Property::TransformOriginX: rstyleMut(style).transform_origin_x = value; return true;
	case Property::TransformOriginY: rstyleMut(style).transform_origin_y = value; return true;
	case Property::Perspective: rstyleMut(style).perspective = value; return true;
	case Property::PerspectiveOriginX: rstyleMut(style).perspective_origin_x = value; return true;
	case Property::PerspectiveOriginY: rstyleMut(style).perspective_origin_y = value; return true;
	case Property::FilterBlur: rstyleMut(style).filter_blur_radius = value; return true;
	case Property::BoxShadowInset:
		rstyleMut(style).box_shadow_inset = value != 0 ? 1 : 0;
		return true;
	case Property::BoxShadowOffsetX: rstyleMut(style).box_shadow_offset_x = value; return true;
	case Property::BoxShadowOffsetY: rstyleMut(style).box_shadow_offset_y = value; return true;
	case Property::BoxShadowBlur: rstyleMut(style).box_shadow_blur_radius = value; return true;
	case Property::BoxShadowSpread: rstyleMut(style).box_shadow_spread = value; return true;
	case Property::BoxShadowColor:
		rstyleMut(style).box_shadow_color = StyleValues::pixelFromStyleValue(value);
		return true;
	case Property::BoxShadowAlpha:
		rstyleMut(style).box_shadow_alpha = static_cast<std::uint8_t>(value < 0 ? 0 : value > 255 ? 255 : value);
		return true;
	default:
		return false;
	}
}

bool setClassRuleValueFast(NodeHandle node, Property property, int value)
{
	if (!node) return true;
	auto &state = treeState();
	if (state.styleInvalidationSuppressionDepth <= 0) return false;
	const int nodeId = node.id();
	if (nodeId < 0 || nodeId >= state.nodeCount) return true;
	Node &target = state.nodes[nodeId];
	return setClassRuleValueFastUnchecked(target, property, value);
}

void setStyleValue(NodeHandle node, Property property, int value, StyleApplicationSource source)
{
	if (!node) return;
	if (source == StyleApplicationSource::ClassRule) {
		if (setClassRuleValueFast(node, property, value)) return;
		Tree::instance().setStyleFromClass(node.id(), property, value);
	} else {
		node.style().set(property, value);
	}
}

void setStyleValueKnownTarget(NodeHandle node,
                              Node &target,
                              Property property,
                              int value,
                              StyleApplicationSource source)
{
	if (source == StyleApplicationSource::ClassRule &&
	    treeState().styleInvalidationSuppressionDepth > 0 &&
	    setClassRuleValueFastUnchecked(target, property, value))
		return;
	setStyleValue(node, property, value, source);
}

void setPositionOffsetValue(NodeHandle node,
                            Property lengthProperty,
                            Property percentProperty,
                            const std::string &value,
                            LengthAxis axis,
                            StyleApplicationSource source)
{
	int percent = 0;
	if (parseSimplePercentPermille(value, percent)) {
		setStyleValue(node, percentProperty, percent, source);
		return;
	}
	// parseSimplePercentPermille only recognises a BARE `NN%`. An inline
	// `left: calc(50% - 1.2%)` fell through to parseLengthForNode, which froze it to px
	// against percentBasisForNode() -- the VIEWPORT, because JSX styles a child before
	// its parent element exists -- and nothing ever re-resolved it after the append.
	if (const CssLengthSpec *compiled = cachedCompiledCssLengthSpec(value)) {
		const ResolvedCssLength resolved = resolveCompiledLengthForNodeDetailed(*compiled, node.id(), axis);
		if (resolved.isPercent) {
			setStyleValue(node, percentProperty, resolved.value, source);
			return;
		}
	}
	setStyleValue(node, lengthProperty, parseLengthForNode(value, node.id(), axis), source);
}

void setSizeValue(NodeHandle node,
                  Property lengthProperty,
                  Property percentProperty,
                  const std::string &value,
                  LengthAxis axis,
                  StyleApplicationSource source)
{
	// `width: auto` / `height: auto` is the CSS initial value: size the box to its
	// content (the same as not declaring width/height at all). Map it to kUnset on
	// BOTH the length and percent companions so the flex pass autosizes the box.
	// Without this, parseLengthForNode("auto") falls through to its 0 keyword
	// fallback, so an explicit `width: auto` collapsed the box to a 0px width
	// (e.g. topbar buttons rendered 0-wide, overlapping their labels) — diverging
	// from the browser, which content-sizes it.
	if (toLowerAscii(trimCssValue(value)) == "auto") {
		setStyleValue(node, lengthProperty, kUnset, source);
		setStyleValue(node, percentProperty, kUnset, source);
		return;
	}
	int percent = 0;
	if (parseSimplePercentPermille(value, percent))
		setStyleValue(node, percentProperty, percent, source);
	else
		setStyleValue(node, lengthProperty, parseLengthForNode(value, node.id(), axis), source);
}

void setAllPadding(NodeHandle node, int value, StyleApplicationSource source)
{
	setStyleValue(node, Property::PaddingTop, value, source);
	setStyleValue(node, Property::PaddingRight, value, source);
	setStyleValue(node, Property::PaddingBottom, value, source);
	setStyleValue(node, Property::PaddingLeft, value, source);
}

void setPaddingBox(NodeHandle node, const BoxLengths &box, StyleApplicationSource source)
{
	setStyleValue(node, Property::PaddingTop, box.top, source);
	setStyleValue(node, Property::PaddingRight, box.right, source);
	setStyleValue(node, Property::PaddingBottom, box.bottom, source);
	setStyleValue(node, Property::PaddingLeft, box.left, source);
}

void setAllMargin(NodeHandle node, int value, StyleApplicationSource source)
{
	setStyleValue(node, Property::MarginTop, value, source);
	setStyleValue(node, Property::MarginRight, value, source);
	setStyleValue(node, Property::MarginBottom, value, source);
	setStyleValue(node, Property::MarginLeft, value, source);
}

void setMarginBox(NodeHandle node, const BoxLengths &box, StyleApplicationSource source)
{
	setStyleValue(node, Property::MarginTop, box.top, source);
	setStyleValue(node, Property::MarginRight, box.right, source);
	setStyleValue(node, Property::MarginBottom, box.bottom, source);
	setStyleValue(node, Property::MarginLeft, box.left, source);
}

void setAllBorderRadius(NodeHandle node, int value, StyleApplicationSource source)
{
	setStyleValue(node, Property::BorderRadiusTopLeft, value, source);
	setStyleValue(node, Property::BorderRadiusTopRight, value, source);
	setStyleValue(node, Property::BorderRadiusBottomRight, value, source);
	setStyleValue(node, Property::BorderRadiusBottomLeft, value, source);
}

Property borderRadiusLengthProperty(int corner)
{
	switch (corner) {
	case 0: return Property::BorderRadiusTopLeft;
	case 1: return Property::BorderRadiusTopRight;
	case 2: return Property::BorderRadiusBottomRight;
	default: return Property::BorderRadiusBottomLeft;
	}
}

Property borderRadiusPercentProperty(int corner)
{
	switch (corner) {
	case 0: return Property::BorderRadiusTopLeftPercent;
	case 1: return Property::BorderRadiusTopRightPercent;
	case 2: return Property::BorderRadiusBottomRightPercent;
	default: return Property::BorderRadiusBottomLeftPercent;
	}
}

void setBorderRadiusCornerValue(NodeHandle node,
                                int corner,
                                const std::string &value,
                                StyleApplicationSource source)
{
	int percent = 0;
	if (parseSimplePercentPermille(value, percent))
		setStyleValue(node, borderRadiusPercentProperty(corner), percent, source);
	else
		setStyleValue(node, borderRadiusLengthProperty(corner), parseLengthForNode(value, node.id(), LengthAxis::None), source);
}

void applyBorderRadiusValue(NodeHandle node, const std::string &value, StyleApplicationSource source)
{
	if (!node) return;
	const auto axes = splitTopLevel(value, '/');
	const auto parts = splitWords(axes.empty() ? value : axes[0]);
	if (parts.empty()) return;
	const std::string &tl = parts[0];
	const std::string &tr = parts.size() > 1 ? parts[1] : tl;
	const std::string &br = parts.size() > 2 ? parts[2] : tl;
	const std::string &bl = parts.size() > 3 ? parts[3] : tr;
	setBorderRadiusCornerValue(node, 0, tl, source);
	setBorderRadiusCornerValue(node, 1, tr, source);
	setBorderRadiusCornerValue(node, 2, br, source);
	setBorderRadiusCornerValue(node, 3, bl, source);
}

void setTransformComponents(NodeHandle node, const TransformComponents &t, StyleApplicationSource source)
{
	{
		static const bool gTraceCube = std::getenv("GEA_DEBUG_CUBE") != nullptr;
		if (gTraceCube)
			std::printf("[cube] setTransformComponents node=%d src=%d rx=%d ry=%d rz=%d tz=%d sx=%d sy=%d\n",
			            node.id(), static_cast<int>(source), t.rotateX, t.rotateY, t.rotateZ, t.translateZ, t.scaleX,
			            t.scaleY);
	}
	setStyleValue(node, Property::TransformRotateX, t.rotateX, source);
	setStyleValue(node, Property::TransformRotateY, t.rotateY, source);
	setStyleValue(node, Property::TransformRotate, t.rotateZ, source);
	setStyleValue(node, Property::TransformTranslateX, t.translateX, source);
	setStyleValue(node, Property::TransformTranslateY, t.translateY, source);
	setStyleValue(node, Property::TransformTranslateZ, t.translateZ, source);
	setStyleValue(node, Property::TransformTranslateXPercent, t.translateXPercent, source);
	setStyleValue(node, Property::TransformTranslateYPercent, t.translateYPercent, source);
	setStyleValue(node, Property::TransformScaleX, t.scaleX, source);
	setStyleValue(node, Property::TransformScaleY, t.scaleY, source);
}

bool applyTransformComponentsFast(NodeHandle node, const TransformComponents &t, StyleApplicationSource source)
{
	{
		static const bool gTraceCube = std::getenv("GEA_DEBUG_CUBE") != nullptr;
		if (gTraceCube)
			std::printf("[cube] applyTransformComponentsFast node=%d src=%d rx=%d ry=%d rz=%d tz=%d sx=%d sy=%d\n",
			            node.id(), static_cast<int>(source), t.rotateX, t.rotateY, t.rotateZ, t.translateZ, t.scaleX,
			            t.scaleY);
	}
	if (!node) return true;
	if (source != StyleApplicationSource::ClassRule) return false;
	auto &state = treeState();
	const int nodeId = node.id();
	if (nodeId < 0 || nodeId >= state.nodeCount) return true;
	Node &target = state.nodes[nodeId];
	const RareStyle &current = rstyle(target.style);
	if (current.transform_rotate_x == static_cast<int16_t>(t.rotateX) &&
	    current.transform_rotate_y == static_cast<int16_t>(t.rotateY) &&
	    current.transform_rotate == static_cast<int16_t>(t.rotateZ) &&
	    current.transform_translate_x == static_cast<int16_t>(t.translateX) &&
	    current.transform_translate_y == static_cast<int16_t>(t.translateY) &&
	    current.transform_translate_z == static_cast<int16_t>(t.translateZ) &&
	    current.transform_translate_x_percent == static_cast<int16_t>(t.translateXPercent) &&
	    current.transform_translate_y_percent == static_cast<int16_t>(t.translateYPercent) &&
	    current.transform_scale_x == static_cast<int16_t>(t.scaleX) &&
	    current.transform_scale_y == static_cast<int16_t>(t.scaleY))
		return true;
	RareStyle &rs = rstyleMut(target.style);
	rs.transform_rotate_x = static_cast<int16_t>(t.rotateX);
	rs.transform_rotate_y = static_cast<int16_t>(t.rotateY);
	rs.transform_rotate = static_cast<int16_t>(t.rotateZ);
	rs.transform_translate_x = static_cast<int16_t>(t.translateX);
	rs.transform_translate_y = static_cast<int16_t>(t.translateY);
	rs.transform_translate_z = static_cast<int16_t>(t.translateZ);
	rs.transform_translate_x_percent = static_cast<int16_t>(t.translateXPercent);
	rs.transform_translate_y_percent = static_cast<int16_t>(t.translateYPercent);
	rs.transform_scale_x = static_cast<int16_t>(t.scaleX);
	rs.transform_scale_y = static_cast<int16_t>(t.scaleY);
	{
		static const bool gTraceCube = std::getenv("GEA_DEBUG_CUBE") != nullptr;
		if (gTraceCube)
			std::printf("[cube]   stored node=%d suppress=%d mounted=%d\n", nodeId,
			            state.styleInvalidationSuppressionDepth,
			            nodeParticipatesInMountedTree(state, nodeId) ? 1 : 0);
	}
	if (state.styleInvalidationSuppressionDepth > 0) return true;
	if (!nodeParticipatesInMountedTree(state, nodeId)) return true;
	target.render.dirty = 1;
	target.render.layout_dirty = 1;
	target.render.non_scroll_dirty = 1;
	target.render.transform_dirty = 1;
	state.transformScanSerial = ~0ull;
	state.transformScanValid = false;
	Tree::instance().markNodeDisplayCommandsDirty(nodeId);
	return true;
}

bool applyTransformSlotsFast(NodeHandle node, const std::int16_t *slots, StyleApplicationSource source)
{
	{
		static const bool gTraceCube = std::getenv("GEA_DEBUG_CUBE") != nullptr;
		if (gTraceCube)
			std::printf("[cube] applyTransformSlotsFast node=%d src=%d rx=%d ry=%d rz=%d tz=%d sx=%d sy=%d\n", node.id(),
			            static_cast<int>(source), slots[0], slots[1], slots[2], slots[5], slots[8], slots[9]);
	}
	if (!node) return true;
	if (source != StyleApplicationSource::ClassRule) return false;
	auto &state = treeState();
	const int nodeId = node.id();
	if (nodeId < 0 || nodeId >= state.nodeCount) return true;
	Node &target = state.nodes[nodeId];
	const RareStyle &current = rstyle(target.style);
	if (current.transform_rotate_x == slots[0] &&
	    current.transform_rotate_y == slots[1] &&
	    current.transform_rotate == slots[2] &&
	    current.transform_translate_x == slots[3] &&
	    current.transform_translate_y == slots[4] &&
	    current.transform_translate_z == slots[5] &&
	    current.transform_translate_x_percent == slots[6] &&
	    current.transform_translate_y_percent == slots[7] &&
	    current.transform_scale_x == slots[8] &&
	    current.transform_scale_y == slots[9])
		return true;
	RareStyle &rs = rstyleMut(target.style);
	rs.transform_rotate_x = slots[0];
	rs.transform_rotate_y = slots[1];
	rs.transform_rotate = slots[2];
	rs.transform_translate_x = slots[3];
	rs.transform_translate_y = slots[4];
	rs.transform_translate_z = slots[5];
	rs.transform_translate_x_percent = slots[6];
	rs.transform_translate_y_percent = slots[7];
	rs.transform_scale_x = slots[8];
	rs.transform_scale_y = slots[9];
	if (state.styleInvalidationSuppressionDepth > 0) return true;
	if (!nodeParticipatesInMountedTree(state, nodeId)) return true;
	target.render.dirty = 1;
	target.render.layout_dirty = 1;
	target.render.non_scroll_dirty = 1;
	target.render.transform_dirty = 1;
	state.transformScanSerial = ~0ull;
	state.transformScanValid = false;
	Tree::instance().markNodeDisplayCommandsDirty(nodeId);
	return true;
}

void applyBorderShorthand(NodeHandle node, const std::string &value, StyleApplicationSource source)
{
	const auto parts = splitWords(value);
	if (!parts.empty()) setStyleValue(node, Property::BorderWidth, parseLengthForNode(parts[0], node.id(), LengthAxis::None), source);
	for (const auto &part : parts) {
		if (part.empty()) continue;
		if (part[0] == '#' || part.find("rgb") != std::string::npos) {
			const ParsedCssColor color = parseCssColor(firstColorToken(value));
			setStyleValue(node, Property::BorderColor, color.valid ? cssColorStyleValue(color) : parseColorStyleValue(value), source);
			if (color.valid) {
				treeState().nodes[node.id()].style.border_alpha = static_cast<uint8_t>(color.a);
				markNodeDisplayCommandsDirtyForStyleApply(node.id());
			}
			return;
		}
	}
}

Property borderSideWidthProperty(int side)
{
	switch (side) {
	case 0: return Property::BorderTopWidth;
	case 1: return Property::BorderRightWidth;
	case 2: return Property::BorderBottomWidth;
	default: return Property::BorderLeftWidth;
	}
}

Property borderSideColorProperty(int side)
{
	switch (side) {
	case 0: return Property::BorderTopColor;
	case 1: return Property::BorderRightColor;
	case 2: return Property::BorderBottomColor;
	default: return Property::BorderLeftColor;
	}
}

int borderSideForProperty(const std::string &property, const char *suffix = "")
{
	const std::string tail = suffix ? suffix : "";
	if (property == std::string("border-top") + tail) return 0;
	if (property == std::string("border-right") + tail) return 1;
	if (property == std::string("border-bottom") + tail) return 2;
	if (property == std::string("border-left") + tail) return 3;
	return -1;
}

void applyBorderSideColorValue(NodeHandle node, int side, const std::string &value, StyleApplicationSource source)
{
	if (!node || side < 0 || side > 3) return;
	const ParsedCssColor color = parseCssColor(firstColorToken(value));
	setStyleValue(node, borderSideColorProperty(side), color.valid ? cssColorStyleValue(color) : parseColorStyleValue(value), source);
	rstyleMut(treeState().nodes[node.id()].style).border_side_alpha[side] = static_cast<uint8_t>(color.valid ? color.a : 255);
	markNodeDisplayCommandsDirtyForStyleApply(node.id());
}

void applyBorderSideShorthand(NodeHandle node, int side, const std::string &value, StyleApplicationSource source)
{
	if (!node || side < 0 || side > 3) return;
	const auto parts = splitWords(value);
	if (!parts.empty()) setStyleValue(node, borderSideWidthProperty(side), parseLengthForNode(parts[0], node.id(), LengthAxis::None), source);
	for (const auto &part : parts) {
		if (part.empty()) continue;
		if (part[0] == '#' || part.find("rgb") != std::string::npos) {
			applyBorderSideColorValue(node, side, value, source);
			return;
		}
	}
}

void applyBackgroundValue(NodeHandle node, const std::string &value, StyleApplicationSource source)
{
	if (!node) return;
#if GEA_RECPROF
	g_profBgCalls++;
	const int64_t _bgt = recNow();
	struct BgTimer { int64_t s; ~BgTimer() { g_profBgUs += recNow() - s; } } _bgTimer{_bgt};
#endif
	Node &target = treeState().nodes[node.id()];
	rstyleMut(target.style).bg_grid_axes = 0;
	rstyleMut(target.style).bg_grid_color = 0;
	rstyleMut(target.style).bg_grid_alpha = 255;
	rstyleMut(target.style).bg_grid_line_x = 0;
	rstyleMut(target.style).bg_grid_line_y = 0;
	rstyleMut(target.style).bg_overlay_gradient = 0;
	rstyleMut(target.style).bg_radial_gradient = 0;

	ParsedLinearGradient gradient;
	ParsedLinearGradient overlayGradient;
	bool hasOverlayGradient = false;
	ParsedRadialGradient radialGradient;
	const auto layers = splitTopLevel(value, ',');
	for (const auto &layerValue : layers) {
		const ParsedGradientLineLayer lineLayer = parseGradientLineLayer(layerValue, node.id());
		if (lineLayer.valid) {
			rstyleMut(target.style).bg_grid_axes |= lineLayer.vertical ? 1 : 2;
			rstyleMut(target.style).bg_grid_color = cssColorNative(lineLayer.color);
			rstyleMut(target.style).bg_grid_alpha = static_cast<uint8_t>(lineLayer.color.a);
			if (lineLayer.vertical)
				rstyleMut(target.style).bg_grid_line_x = static_cast<uint8_t>(std::max(1, std::min(lineLayer.lineWidth, 255)));
			else
				rstyleMut(target.style).bg_grid_line_y = static_cast<uint8_t>(std::max(1, std::min(lineLayer.lineWidth, 255)));
			continue;
		}
		const ParsedLinearGradient layerGradient = parseLinearGradient(layerValue);
		if (layerGradient.valid) {
			if (gradient.valid) {
				overlayGradient = gradient;
				hasOverlayGradient = true;
			}
			gradient = layerGradient;
			continue;
		}
		const ParsedRadialGradient layerRadial = parseRadialGradient(layerValue);
		if (layerRadial.valid) radialGradient = layerRadial;
	}
	const uint8_t gridAxes = rstyle(target.style).bg_grid_axes;
	const uint16_t gridColor = rstyle(target.style).bg_grid_color;
	const uint8_t gridAlpha = rstyle(target.style).bg_grid_alpha;
	const uint16_t gridStepX = rstyle(target.style).bg_grid_step_x;
	const uint16_t gridStepY = rstyle(target.style).bg_grid_step_y;
	const uint8_t gridLineX = rstyle(target.style).bg_grid_line_x;
	const uint8_t gridLineY = rstyle(target.style).bg_grid_line_y;
	if (gradient.valid) {
		setStyleValue(node, Property::BackgroundColor, cssColorStyleValue(gradient.from), source);
		setStyleValue(node, Property::HasBackground, 1, source);
		target.style.bg_fill = 1;
		target.style.bg_alpha = static_cast<uint8_t>(gradient.from.a);
		rstyleMut(target.style).bg_gradient_from_color = cssColorNative(gradient.from);
		rstyleMut(target.style).bg_gradient_mid_color = gradient.hasMid ? cssColorNative(gradient.mid) : 0;
		rstyleMut(target.style).bg_gradient_to_color = cssColorNative(gradient.to);
		rstyleMut(target.style).bg_gradient_from_alpha = static_cast<uint8_t>(gradient.from.a);
		rstyleMut(target.style).bg_gradient_mid_alpha = static_cast<uint8_t>(gradient.hasMid ? gradient.mid.a : 255);
		rstyleMut(target.style).bg_gradient_to_alpha = static_cast<uint8_t>(gradient.to.a);
		rstyleMut(target.style).bg_gradient_mid_stop = static_cast<uint16_t>(gradient.midStopPermille);
		rstyleMut(target.style).bg_gradient_to_stop = static_cast<uint16_t>(gradient.toStopPermille);
		rstyleMut(target.style).bg_gradient_has_mid = gradient.hasMid ? 1 : 0;
		rstyleMut(target.style).bg_gradient_angle = static_cast<int16_t>(gradient.angleTenths);
		if (hasOverlayGradient) {
			rstyleMut(target.style).bg_overlay_gradient = 1;
			rstyleMut(target.style).bg_overlay_gradient_from_color = cssColorNative(overlayGradient.from);
			rstyleMut(target.style).bg_overlay_gradient_mid_color = overlayGradient.hasMid ? cssColorNative(overlayGradient.mid) : 0;
			rstyleMut(target.style).bg_overlay_gradient_to_color = cssColorNative(overlayGradient.to);
			rstyleMut(target.style).bg_overlay_gradient_from_alpha = static_cast<uint8_t>(overlayGradient.from.a);
			rstyleMut(target.style).bg_overlay_gradient_mid_alpha = static_cast<uint8_t>(overlayGradient.hasMid ? overlayGradient.mid.a : 255);
			rstyleMut(target.style).bg_overlay_gradient_to_alpha = static_cast<uint8_t>(overlayGradient.to.a);
			rstyleMut(target.style).bg_overlay_gradient_mid_stop = static_cast<uint16_t>(overlayGradient.midStopPermille);
			rstyleMut(target.style).bg_overlay_gradient_to_stop = static_cast<uint16_t>(overlayGradient.toStopPermille);
			rstyleMut(target.style).bg_overlay_gradient_has_mid = overlayGradient.hasMid ? 1 : 0;
			rstyleMut(target.style).bg_overlay_gradient_angle = static_cast<int16_t>(overlayGradient.angleTenths);
		}
		if (radialGradient.valid) {
			rstyleMut(target.style).bg_radial_gradient = 1;
			rstyleMut(target.style).bg_radial_gradient_from_color = cssColorNative(radialGradient.from);
			rstyleMut(target.style).bg_radial_gradient_to_color = cssColorNative(radialGradient.to);
			rstyleMut(target.style).bg_radial_gradient_from_alpha = static_cast<uint8_t>(radialGradient.from.a);
			rstyleMut(target.style).bg_radial_gradient_to_alpha = static_cast<uint8_t>(radialGradient.to.a);
			rstyleMut(target.style).bg_radial_gradient_stop = static_cast<uint16_t>(radialGradient.stopPermille);
			rstyleMut(target.style).bg_radial_gradient_cx = static_cast<int16_t>(radialGradient.cxPermille);
			rstyleMut(target.style).bg_radial_gradient_cy = static_cast<int16_t>(radialGradient.cyPermille);
			rstyleMut(target.style).bg_radial_gradient_rx = static_cast<int16_t>(radialGradient.rxPermille);
			rstyleMut(target.style).bg_radial_gradient_ry = static_cast<int16_t>(radialGradient.ryPermille);
		}
		rstyleMut(target.style).bg_grid_axes = gridAxes;
		rstyleMut(target.style).bg_grid_color = gridColor;
		rstyleMut(target.style).bg_grid_alpha = gridAlpha;
		rstyleMut(target.style).bg_grid_step_x = gridStepX;
		rstyleMut(target.style).bg_grid_step_y = gridStepY;
		rstyleMut(target.style).bg_grid_line_x = gridLineX;
		rstyleMut(target.style).bg_grid_line_y = gridLineY;
		markNodeDisplayCommandsDirtyForStyleApply(node.id());
		return;
	}

	const ParsedCssColor color = parseCssColor(firstColorToken(value));
	setStyleValue(node, Property::BackgroundColor, color.valid ? cssColorStyleValue(color) : parseColorStyleValue(value), source);
	setStyleValue(node, Property::HasBackground, 1, source);
	target.style.bg_fill = 0;
	target.style.bg_alpha = static_cast<uint8_t>(color.valid ? color.a : 255);
	rstyleMut(target.style).bg_grid_axes = gridAxes;
	rstyleMut(target.style).bg_grid_color = gridColor;
	rstyleMut(target.style).bg_grid_alpha = gridAlpha;
	rstyleMut(target.style).bg_grid_step_x = gridStepX;
	rstyleMut(target.style).bg_grid_step_y = gridStepY;
	rstyleMut(target.style).bg_grid_line_x = gridLineX;
	rstyleMut(target.style).bg_grid_line_y = gridLineY;
	markNodeDisplayCommandsDirtyForStyleApply(node.id());
}

void applyBackgroundSizeValue(NodeHandle node, const std::string &value)
{
	if (!node) return;
	Node &target = treeState().nodes[node.id()];
	if (rstyle(target.style).bg_grid_axes == 0) return;
	const auto layers = splitTopLevel(value, ',');
	if (layers.empty()) return;
	const auto parts = splitWords(layers[0]);
	if (parts.empty()) return;
	const int stepX = parseLengthForNode(parts[0], node.id(), LengthAxis::Horizontal);
	const int stepY = parseLengthForNode(parts.size() > 1 ? parts[1] : parts[0], node.id(), LengthAxis::Vertical);
	if (stepX > 0) rstyleMut(target.style).bg_grid_step_x = static_cast<uint16_t>(std::min(stepX, 65535));
	if (stepY > 0) rstyleMut(target.style).bg_grid_step_y = static_cast<uint16_t>(std::min(stepY, 65535));
	markNodeDisplayCommandsDirtyForStyleApply(node.id());
}

void applyTextColorValue(NodeHandle node, const std::string &value, StyleApplicationSource source)
{
	if (!node) return;
	const ParsedCssColor color = parseCssColor(firstColorToken(value));
	setStyleValue(node, Property::Color, color.valid ? cssColorStyleValue(color) : parseColorStyleValue(value), source);
	treeState().nodes[node.id()].style.text_alpha = static_cast<uint8_t>(color.valid ? color.a : 255);
	markNodeDisplayCommandsDirtyForStyleApply(node.id());
}

void applyBorderColorValue(NodeHandle node, const std::string &value, StyleApplicationSource source)
{
	if (!node) return;
	const ParsedCssColor color = parseCssColor(firstColorToken(value));
	setStyleValue(node, Property::BorderColor, color.valid ? cssColorStyleValue(color) : parseColorStyleValue(value), source);
	treeState().nodes[node.id()].style.border_alpha = static_cast<uint8_t>(color.valid ? color.a : 255);
	markNodeDisplayCommandsDirtyForStyleApply(node.id());
}

int16_t *animatedTransformSlot(RareStyle &rs, Property property)
{
	switch (property) {
	case Property::TransformRotate: return &rs.transform_rotate;
	case Property::TransformRotateX: return &rs.transform_rotate_x;
	case Property::TransformRotateY: return &rs.transform_rotate_y;
	case Property::TransformTranslateX: return &rs.transform_translate_x;
	case Property::TransformTranslateY: return &rs.transform_translate_y;
	case Property::TransformTranslateZ: return &rs.transform_translate_z;
	case Property::TransformTranslateXPercent: return &rs.transform_translate_x_percent;
	case Property::TransformTranslateYPercent: return &rs.transform_translate_y_percent;
	case Property::TransformScaleX: return &rs.transform_scale_x;
	case Property::TransformScaleY: return &rs.transform_scale_y;
	default: return nullptr;
	}
}

bool applyAnimatedTransformStyleValueFast(int nodeId, Property property, int value)
{
	auto &state = treeState();
	if (nodeId < 0 || nodeId >= state.nodeCount) return true;
	Node &target = state.nodes[nodeId];
	RareStyle &rs = rstyleMut(target.style);
	int16_t *slot = animatedTransformSlot(rs, property);
	if (!slot) return false;
	const int16_t next = static_cast<int16_t>(value);
	if (*slot == next) return true;
	*slot = next;
	if (state.styleInvalidationSuppressionDepth > 0) return true;
	if (!nodeParticipatesInMountedTree(state, nodeId)) return true;
	target.render.dirty = 1;
	target.render.layout_dirty = 1;
	target.render.non_scroll_dirty = 1;
	target.render.transform_dirty = 1;
	state.transformScanSerial = ~0ull;
	state.transformScanValid = false;
	Tree::instance().markNodeDisplayCommandsDirty(nodeId);
	return true;
}

}  // namespace

void Style::set(Property property, int value) const
{
	if (nodeId_ < 0) return;
	Tree::instance().setStyle(nodeId_, property, value);
	if (isInheritedStyleProperty(property)) recomputeDescendantClassStyles(nodeId_);
}

void Style::backgroundColor(int rgb565) const
{
	set(Property::BackgroundColor, rgb565);
	set(Property::HasBackground, 1);
}

void Style::setProperty(const std::string &property, const std::string &value) const
{
	if (nodeId_ < 0) return;
	StyleSheet::instance().applyProperty(NodeHandle(nodeId_), property, value);
}

bool Style::removeProperty(const std::string &property) const
{
	if (nodeId_ < 0) return false;
	return StyleSheet::instance().removeProperty(NodeHandle(nodeId_), property);
}

void applyAnimatedStyleValue(int nodeId, Property property, int value)
{
	if (nodeId < 0) return;
	if (applyAnimatedTransformStyleValueFast(nodeId, property, value)) return;
	Tree::instance().setStyleFromClass(nodeId, property, value);
	if (isInheritedStyleProperty(property)) recomputeDescendantClassStyles(nodeId);
}

namespace {

bool isInheritedStyleProperty(Property property)
{
	return property == Property::Color ||
	       property == Property::FontId ||
	       property == Property::FontSize ||
	       property == Property::FontWeight ||
	       property == Property::LineHeight ||
	       property == Property::TextAlign ||
	       property == Property::TextTransform ||
	       property == Property::WhiteSpace;
}

void applyInheritedStyleDefaults(int node)
{
	auto &state = treeState();
	if (node < 0 || node >= state.nodeCount) return;
	const int parent = state.nodes[node].parent;
	if (parent < 0 || parent >= state.nodeCount) return;

	const auto &parentStyle = state.nodes[parent].style;
	auto &style = state.nodes[node].style;
	style.text_color = parentStyle.text_color;
	style.font_id = parentStyle.font_id;
	style.font_size = parentStyle.font_size;
	style.font_weight = parentStyle.font_weight;
	style.line_height = parentStyle.line_height;
	style.text_align = parentStyle.text_align;
	style.text_transform = parentStyle.text_transform;
	style.white_space = parentStyle.white_space;
}

bool applyNumberDeclarationWithSource(NodeHandle node,
                                      CssDeclarationId declaration,
                                      double value,
                                      StyleApplicationSource source)
{
	if (!node) return false;
	const int length = numericLength(value);
	switch (declaration) {
	case CssDeclarationId::Gap:
		setStyleValue(node, Property::Gap, length, source);
		return true;
	case CssDeclarationId::Width:
		setStyleValue(node, Property::Width, length, source);
		return true;
	case CssDeclarationId::Height:
		setStyleValue(node, Property::Height, length, source);
		return true;
	case CssDeclarationId::MinWidth:
		setStyleValue(node, Property::MinWidth, length, source);
		return true;
	case CssDeclarationId::MinHeight:
		setStyleValue(node, Property::MinHeight, length, source);
		return true;
	case CssDeclarationId::MaxWidth:
		setStyleValue(node, Property::MaxWidth, length, source);
		return true;
	case CssDeclarationId::MaxHeight:
		setStyleValue(node, Property::MaxHeight, length, source);
		return true;
	case CssDeclarationId::Flex:
		setStyleValue(node, Property::Flex, rawNumber(value), source);
		setStyleValue(node, Property::FlexShrink, rawNumber(1.0), source);
		setStyleValue(node, Property::FlexBasis, kUnset, source);  // numeric `flex: N` has no explicit basis
		return true;
	case CssDeclarationId::Padding:
		setAllPadding(node, length, source);
		return true;
	case CssDeclarationId::PaddingTop:
		setStyleValue(node, Property::PaddingTop, length, source);
		return true;
	case CssDeclarationId::PaddingRight:
		setStyleValue(node, Property::PaddingRight, length, source);
		return true;
	case CssDeclarationId::PaddingBottom:
		setStyleValue(node, Property::PaddingBottom, length, source);
		return true;
	case CssDeclarationId::PaddingLeft:
		setStyleValue(node, Property::PaddingLeft, length, source);
		return true;
	case CssDeclarationId::Margin:
		setAllMargin(node, length, source);
		return true;
	case CssDeclarationId::MarginTop:
		setStyleValue(node, Property::MarginTop, length, source);
		return true;
	case CssDeclarationId::MarginRight:
		setStyleValue(node, Property::MarginRight, length, source);
		return true;
	case CssDeclarationId::MarginBottom:
		setStyleValue(node, Property::MarginBottom, length, source);
		return true;
	case CssDeclarationId::MarginLeft:
		setStyleValue(node, Property::MarginLeft, length, source);
		return true;
	case CssDeclarationId::Top:
		setStyleValue(node, Property::Top, length, source);
		return true;
	case CssDeclarationId::Right:
		setStyleValue(node, Property::Right, length, source);
		return true;
	case CssDeclarationId::Bottom:
		setStyleValue(node, Property::Bottom, length, source);
		return true;
	case CssDeclarationId::Left:
		setStyleValue(node, Property::Left, length, source);
		return true;
	case CssDeclarationId::ZIndex:
		setStyleValue(node, Property::ZIndex, rawNumber(value), source);
		return true;
	case CssDeclarationId::Opacity:
		setStyleValue(node, Property::Opacity, numericOpacity(value), source);
		return true;
	case CssDeclarationId::BorderWidth:
		setStyleValue(node, Property::BorderWidth, length, source);
		return true;
	case CssDeclarationId::BorderTopWidth:
		setStyleValue(node, Property::BorderTopWidth, length, source);
		return true;
	case CssDeclarationId::BorderRightWidth:
		setStyleValue(node, Property::BorderRightWidth, length, source);
		return true;
	case CssDeclarationId::BorderBottomWidth:
		setStyleValue(node, Property::BorderBottomWidth, length, source);
		return true;
	case CssDeclarationId::BorderLeftWidth:
		setStyleValue(node, Property::BorderLeftWidth, length, source);
		return true;
	case CssDeclarationId::BorderRadius:
		setAllBorderRadius(node, length, source);
		return true;
	case CssDeclarationId::BorderTopLeftRadius:
		setStyleValue(node, Property::BorderRadiusTopLeft, length, source);
		return true;
	case CssDeclarationId::BorderTopRightRadius:
		setStyleValue(node, Property::BorderRadiusTopRight, length, source);
		return true;
	case CssDeclarationId::BorderBottomRightRadius:
		setStyleValue(node, Property::BorderRadiusBottomRight, length, source);
		return true;
	case CssDeclarationId::BorderBottomLeftRadius:
		setStyleValue(node, Property::BorderRadiusBottomLeft, length, source);
		return true;
	case CssDeclarationId::FontSize:
		setStyleValue(node, Property::FontSize, length, source);
		return true;
	case CssDeclarationId::FontWeight:
		setStyleValue(node, Property::FontWeight, std::clamp(roundToInt(value), 1, 1000), source);
		return true;
	case CssDeclarationId::LineHeight:
		setStyleValue(node, Property::LineHeight, roundToInt(static_cast<double>(currentFontSizeForNode(node.id())) * value), source);
		return true;
	case CssDeclarationId::Transform:
	case CssDeclarationId::Rotate:
		setStyleValue(node, Property::TransformRotate, numericRotateTenths(value), source);
		return true;
	case CssDeclarationId::Scale: {
		const int scale = numericScalePermille(value);
		setStyleValue(node, Property::TransformScaleX, scale, source);
		setStyleValue(node, Property::TransformScaleY, scale, source);
		return true;
	}
	default:
		return false;
	}
}

bool applyNumberPropertyWithSource(NodeHandle node, const char *property, double value, StyleApplicationSource source)
{
	if (!node || !property) return false;
	return applyNumberDeclarationWithSource(node, classifyDeclaration(property), value, source);
}

bool applyKnownResolvedPropertyWithSource(NodeHandle node, CssDeclarationId declaration, const std::string &value, StyleApplicationSource source)
{
	if (!node) return true;
	const int nodeId = node.id();
	switch (declaration) {
	case CssDeclarationId::Display:
		setStyleValue(node, Property::Display, displayValue(value), source);
		return true;
	case CssDeclarationId::FlexDirection:
		setStyleValue(node, Property::FlexDirection, flexDirectionValue(value), source);
		return true;
	case CssDeclarationId::FlexWrap:
		setStyleValue(node, Property::FlexWrap, flexWrapValue(value), source);
		return true;
	case CssDeclarationId::JustifyContent:
		setStyleValue(node, Property::JustifyContent, flexAlignValue(value), source);
		return true;
	case CssDeclarationId::AlignItems:
		setStyleValue(node, Property::AlignItems, flexAlignValue(value), source);
		return true;
	case CssDeclarationId::JustifyItems:
		setStyleValue(node, Property::JustifyItems, flexAlignValue(value), source);
		return true;
	case CssDeclarationId::AlignContent:
		setStyleValue(node, Property::AlignContent, flexAlignValue(value), source);
		return true;
	case CssDeclarationId::AlignSelf:
		setStyleValue(node, Property::AlignSelf, alignSelfValue(value), source);
		return true;
	case CssDeclarationId::PlaceItems: {
		const int align = flexAlignValue(value);
		setStyleValue(node, Property::AlignItems, align, source);
		setStyleValue(node, Property::JustifyItems, align, source);
		setStyleValue(node, Property::JustifyContent, align, source);
		return true;
	}
	case CssDeclarationId::GridTemplateColumns:
		applyGridTemplateValue(node, value, true);
		return true;
	case CssDeclarationId::GridTemplateRows:
		applyGridTemplateValue(node, value, false);
		return true;
	case CssDeclarationId::Ignored:
	case CssDeclarationId::Content:
	case CssDeclarationId::Animation:
		return true;
	case CssDeclarationId::Gap:
		setStyleValue(node, Property::Gap, parseLengthForNode(value, nodeId, LengthAxis::Horizontal), source);
		return true;
	case CssDeclarationId::Width:
		setSizeValue(node, Property::Width, Property::WidthPercent, value, LengthAxis::Horizontal, source);
		return true;
	case CssDeclarationId::Height:
		setSizeValue(node, Property::Height, Property::HeightPercent, value, LengthAxis::Vertical, source);
		return true;
	case CssDeclarationId::MinWidth:
		setStyleValue(node, Property::MinWidth, parseLengthForNode(value, nodeId, LengthAxis::Horizontal), source);
		return true;
	case CssDeclarationId::MinHeight:
		setStyleValue(node, Property::MinHeight, parseLengthForNode(value, nodeId, LengthAxis::Vertical), source);
		return true;
	case CssDeclarationId::MaxWidth:
		setStyleValue(node, Property::MaxWidth, parseLengthForNode(value, nodeId, LengthAxis::Horizontal), source);
		return true;
	case CssDeclarationId::MaxHeight:
		setStyleValue(node, Property::MaxHeight, parseLengthForNode(value, nodeId, LengthAxis::Vertical), source);
		return true;
	case CssDeclarationId::Flex: {
		double grow = 0.0;
		double shrink = 1.0;
		int basis = kUnset;
		int numberIndex = 0;
		std::size_t pos = 0;
		while (pos < value.size()) {
			while (pos < value.size() && (value[pos] == ' ' || value[pos] == '\t')) pos++;
			const std::size_t start = pos;
			while (pos < value.size() && value[pos] != ' ' && value[pos] != '\t') pos++;
			if (pos <= start) break;
			const std::string tok = value.substr(start, pos - start);
			if (tok == "auto" || tok == "content" || tok == "max-content" ||
			    tok == "min-content" || tok == "fit-content") {
				basis = kUnset;
				continue;
			}
			if (tok == "none") { grow = 0.0; shrink = 0.0; basis = kUnset; continue; }
			char *endp = nullptr;
			const double num = std::strtod(tok.c_str(), &endp);
			const bool hasUnit = endp && *endp != '\0';
			if (hasUnit) {
				basis = tok.find('%') != std::string::npos
			            ? kUnset
			            : parseLengthForNode(tok, nodeId, LengthAxis::Horizontal);
			} else {
				if (numberIndex == 0) grow = num;
				else if (numberIndex == 1) shrink = num;
				numberIndex++;
			}
		}
		setStyleValue(node, Property::Flex, rawNumber(grow), source);
		setStyleValue(node, Property::FlexShrink, rawNumber(shrink), source);
		setStyleValue(node, Property::FlexBasis, basis, source);
		return true;
	}
	case CssDeclarationId::FlexGrow:
		setStyleValue(node, Property::Flex, rawNumber(std::strtod(value.c_str(), nullptr)), source);
		return true;
	case CssDeclarationId::FlexShrink:
		setStyleValue(node, Property::FlexShrink, rawNumber(std::strtod(value.c_str(), nullptr)), source);
		return true;
	case CssDeclarationId::FlexBasis: {
		const bool definite = value != "auto" && value != "content" && value != "max-content" &&
		                      value != "min-content" && value != "fit-content" &&
		                      value.find('%') == std::string::npos;
		setStyleValue(node, Property::FlexBasis,
		              definite ? parseLengthForNode(value, nodeId, LengthAxis::Horizontal) : kUnset, source);
		return true;
	}
	case CssDeclarationId::Padding:
		setPaddingBox(node, parseBoxLengths(value, nodeId), source);
		return true;
	case CssDeclarationId::PaddingTop:
		setStyleValue(node, Property::PaddingTop, parseLengthForNode(value, nodeId, LengthAxis::Vertical), source);
		return true;
	case CssDeclarationId::PaddingRight:
		setStyleValue(node, Property::PaddingRight, parseLengthForNode(value, nodeId, LengthAxis::Horizontal), source);
		return true;
	case CssDeclarationId::PaddingBottom:
		setStyleValue(node, Property::PaddingBottom, parseLengthForNode(value, nodeId, LengthAxis::Vertical), source);
		return true;
	case CssDeclarationId::PaddingLeft:
		setStyleValue(node, Property::PaddingLeft, parseLengthForNode(value, nodeId, LengthAxis::Horizontal), source);
		return true;
	case CssDeclarationId::Margin:
		setMarginBox(node, parseBoxLengths(value, nodeId), source);
		return true;
	case CssDeclarationId::MarginTop:
		setStyleValue(node, Property::MarginTop, parseLengthForNode(value, nodeId, LengthAxis::Vertical), source);
		return true;
	case CssDeclarationId::MarginRight:
		setStyleValue(node, Property::MarginRight, parseLengthForNode(value, nodeId, LengthAxis::Horizontal), source);
		return true;
	case CssDeclarationId::MarginBottom:
		setStyleValue(node, Property::MarginBottom, parseLengthForNode(value, nodeId, LengthAxis::Vertical), source);
		return true;
	case CssDeclarationId::MarginLeft:
		setStyleValue(node, Property::MarginLeft, parseLengthForNode(value, nodeId, LengthAxis::Horizontal), source);
		return true;
	case CssDeclarationId::Position:
		setStyleValue(node, Property::Position, positionValue(value), source);
		return true;
	case CssDeclarationId::Inset: {
		const BoxLengths box = parseBoxLengths(value, nodeId);
		setStyleValue(node, Property::Top, box.top, source);
		setStyleValue(node, Property::Right, box.right, source);
		setStyleValue(node, Property::Bottom, box.bottom, source);
		setStyleValue(node, Property::Left, box.left, source);
		return true;
	}
	case CssDeclarationId::Top:
		setPositionOffsetValue(node, Property::Top, Property::TopPercent, value, LengthAxis::Vertical, source);
		return true;
	case CssDeclarationId::Right:
		setPositionOffsetValue(node, Property::Right, Property::RightPercent, value, LengthAxis::Horizontal, source);
		return true;
	case CssDeclarationId::Bottom:
		setPositionOffsetValue(node, Property::Bottom, Property::BottomPercent, value, LengthAxis::Vertical, source);
		return true;
	case CssDeclarationId::Left:
		setPositionOffsetValue(node, Property::Left, Property::LeftPercent, value, LengthAxis::Horizontal, source);
		return true;
	case CssDeclarationId::ZIndex:
		setStyleValue(node, Property::ZIndex, rawNumber(std::strtod(value.c_str(), nullptr)), source);
		return true;
	case CssDeclarationId::ActiveBackgroundColor:
		setStyleValue(node, Property::ActiveBackgroundColor, parseColorStyleValue(value), source);
		setStyleValue(node, Property::HasActiveBackground, 1, source);
		return true;
	case CssDeclarationId::Background:
		applyBackgroundValue(node, value, source);
		return true;
	case CssDeclarationId::BackgroundSize:
		applyBackgroundSizeValue(node, value);
		return true;
	case CssDeclarationId::ObjectFit:
		setStyleValue(node, Property::ImageFit, imageFitValue(value), source);
		return true;
	case CssDeclarationId::Color:
		applyTextColorValue(node, value, source);
		return true;
	case CssDeclarationId::Opacity:
		setStyleValue(node, Property::Opacity, parseOpacity(value), source);
		return true;
	case CssDeclarationId::BorderColor:
		applyBorderColorValue(node, value, source);
		return true;
	case CssDeclarationId::Border:
		applyBorderShorthand(node, value, source);
		return true;
	case CssDeclarationId::BorderWidth:
		setStyleValue(node, Property::BorderWidth, parseLengthForNode(value, nodeId, LengthAxis::None), source);
		return true;
	case CssDeclarationId::BorderTop:
		applyBorderSideShorthand(node, 0, value, source);
		return true;
	case CssDeclarationId::BorderRight:
		applyBorderSideShorthand(node, 1, value, source);
		return true;
	case CssDeclarationId::BorderBottom:
		applyBorderSideShorthand(node, 2, value, source);
		return true;
	case CssDeclarationId::BorderLeft:
		applyBorderSideShorthand(node, 3, value, source);
		return true;
	case CssDeclarationId::BorderTopWidth:
		setStyleValue(node, Property::BorderTopWidth, parseLengthForNode(value, nodeId, LengthAxis::None), source);
		return true;
	case CssDeclarationId::BorderRightWidth:
		setStyleValue(node, Property::BorderRightWidth, parseLengthForNode(value, nodeId, LengthAxis::None), source);
		return true;
	case CssDeclarationId::BorderBottomWidth:
		setStyleValue(node, Property::BorderBottomWidth, parseLengthForNode(value, nodeId, LengthAxis::None), source);
		return true;
	case CssDeclarationId::BorderLeftWidth:
		setStyleValue(node, Property::BorderLeftWidth, parseLengthForNode(value, nodeId, LengthAxis::None), source);
		return true;
	case CssDeclarationId::BorderTopColor:
		applyBorderSideColorValue(node, 0, value, source);
		return true;
	case CssDeclarationId::BorderRightColor:
		applyBorderSideColorValue(node, 1, value, source);
		return true;
	case CssDeclarationId::BorderBottomColor:
		applyBorderSideColorValue(node, 2, value, source);
		return true;
	case CssDeclarationId::BorderLeftColor:
		applyBorderSideColorValue(node, 3, value, source);
		return true;
	case CssDeclarationId::BorderRadius:
		applyBorderRadiusValue(node, value, source);
		return true;
	case CssDeclarationId::BorderTopLeftRadius:
		setBorderRadiusCornerValue(node, 0, value, source);
		return true;
	case CssDeclarationId::BorderTopRightRadius:
		setBorderRadiusCornerValue(node, 1, value, source);
		return true;
	case CssDeclarationId::BorderBottomRightRadius:
		setBorderRadiusCornerValue(node, 2, value, source);
		return true;
	case CssDeclarationId::BorderBottomLeftRadius:
		setBorderRadiusCornerValue(node, 3, value, source);
		return true;
	case CssDeclarationId::FontFamily: {
		const std::string family = primaryFontFamily(value);
		const int familyId = gea::framework::graphics::FontRegistry::familyId(family.c_str());
		setStyleValue(node, Property::FontId, familyId, source);
		return true;
	}
	case CssDeclarationId::FontSize:
		setStyleValue(node, Property::FontSize, parseLengthForNode(value, nodeId, LengthAxis::Vertical), source);
		return true;
	case CssDeclarationId::FontWeight:
		setStyleValue(node, Property::FontWeight, fontWeightValue(value), source);
		return true;
	case CssDeclarationId::LineHeight:
		setStyleValue(node, Property::LineHeight, parseLineHeightForNode(value, nodeId), source);
		return true;
	case CssDeclarationId::TextAlign:
		setStyleValue(node, Property::TextAlign, textAlignValue(value), source);
		return true;
	case CssDeclarationId::TextDecoration:
		setStyleValue(node, Property::TextDecoration, textDecorationValue(value), source);
		return true;
	case CssDeclarationId::TextTransform:
		setStyleValue(node, Property::TextTransform, textTransformValue(value), source);
		return true;
	case CssDeclarationId::WhiteSpace:
		setStyleValue(node, Property::WhiteSpace, whiteSpaceValue(value), source);
		return true;
	case CssDeclarationId::TextOverflow:
		setStyleValue(node, Property::TextOverflow, textOverflowValue(value), source);
		return true;
	case CssDeclarationId::BackfaceVisibility:
		setStyleValue(node, Property::Backface, backfaceValue(value), source);
		return true;
	case CssDeclarationId::PointerEvents:
		setStyleValue(node, Property::PointerEvents, pointerEventsValue(value), source);
		return true;
	case CssDeclarationId::Overflow:
		setStyleValue(node, Property::Overflow, overflowValue(value), source);
		return true;
	case CssDeclarationId::OverflowX:
		setStyleValue(node, Property::OverflowX, overflowValue(value), source);
		return true;
	case CssDeclarationId::OverflowY:
		setStyleValue(node, Property::OverflowY, overflowValue(value), source);
		return true;
	case CssDeclarationId::MaskImage:
		setStyleValue(node, Property::MaskRightFadeWidth, parseRightFadeMaskWidth(value, nodeId), source);
		return true;
	case CssDeclarationId::Transform:
		setTransformComponents(node, parseTransformComponents(value, nodeId), source);
		return true;
	case CssDeclarationId::Rotate:
		setStyleValue(node, Property::TransformRotate, parseRotateTenths(value), source);
		return true;
	case CssDeclarationId::Scale: {
		const int scale = parseScalePermille(value);
		setStyleValue(node, Property::TransformScaleX, scale, source);
		setStyleValue(node, Property::TransformScaleY, scale, source);
		return true;
	}
	case CssDeclarationId::Filter:
		setStyleValue(node, Property::FilterBlur, parseFilterBlurRadius(value, nodeId), source);
		return true;
	case CssDeclarationId::BoxShadow:
		applyBoxShadowValue(node, value, source);
		return true;
	case CssDeclarationId::TransformOrigin: {
		const auto parts = splitWords(value);
		setStyleValue(node, Property::TransformOriginX, parseOriginPart(parts.empty() ? "" : parts[0], 500), source);
		setStyleValue(node, Property::TransformOriginY, parseOriginPart(parts.size() < 2 ? "" : parts[1], 500), source);
		return true;
	}
	case CssDeclarationId::Perspective:
		setStyleValue(node, Property::Perspective, parseLengthForNode(value, nodeId, LengthAxis::Horizontal), source);
		return true;
	case CssDeclarationId::PerspectiveOrigin: {
		const auto parts = splitWords(value);
		setStyleValue(node, Property::PerspectiveOriginX, parseOriginPart(parts.empty() ? "" : parts[0], 500), source);
		setStyleValue(node, Property::PerspectiveOriginY, parseOriginPart(parts.size() < 2 ? "" : parts[1], 500), source);
		return true;
	}
	case CssDeclarationId::Unknown:
	case CssDeclarationId::Custom:
		return false;
	}
	return false;
}

const CssCompiledValue *compiledCssValueForHandle(std::uint16_t handle)
{
	const auto &list = compiledCssValues();
	return handle < list.size() ? &list[handle] : nullptr;
}

const CssCompiledBackground *compiledCssBackgroundForHandle(std::uint16_t handle)
{
	const auto &list = compiledCssBackgrounds();
	return handle < list.size() ? &list[handle] : nullptr;
}

const CssCompiledGridTemplate *compiledCssGridTemplateForHandle(std::uint16_t handle)
{
	const auto &list = compiledCssGridTemplates();
	return handle < list.size() ? &list[handle] : nullptr;
}

int percentPermille(const CssLengthSpec &length)
{
	return roundToInt(static_cast<double>(length.value) * 10.0);
}

void setCompiledSizeValue(NodeHandle node,
                          Property lengthProperty,
                          Property percentProperty,
                          const CssLengthSpec &length,
                          LengthAxis axis,
                          StyleApplicationSource source)
{
	const ResolvedCssLength resolved = resolveCompiledLengthForNodeDetailed(length, node.id(), axis);
	if (resolved.isAuto) {
		setStyleValue(node, lengthProperty, kUnset, source);
		setStyleValue(node, percentProperty, kUnset, source);
		return;
	}
	if (resolved.isPercent)
		setStyleValue(node, percentProperty, resolved.value, source);
	else
		setStyleValue(node, lengthProperty, resolved.value, source);
}

void setCompiledPositionOffsetValue(NodeHandle node,
                                    Property lengthProperty,
                                    Property percentProperty,
                                    const CssLengthSpec &length,
                                    LengthAxis axis,
                                    StyleApplicationSource source)
{
	const ResolvedCssLength resolved = resolveCompiledLengthForNodeDetailed(length, node.id(), axis);
	if (resolved.isPercent)
		setStyleValue(node, percentProperty, resolved.value, source);
	else
		setStyleValue(node, lengthProperty, resolved.value, source);
}

void setCompiledBorderRadiusCornerValue(NodeHandle node,
                                        int corner,
                                        const CssLengthSpec &length,
                                        StyleApplicationSource source)
{
	const ResolvedCssLength resolved = resolveCompiledLengthForNodeDetailed(length, node.id(), LengthAxis::None);
	if (resolved.isPercent)
		setStyleValue(node, borderRadiusPercentProperty(corner), resolved.value, source);
	else
		setStyleValue(node, borderRadiusLengthProperty(corner), resolved.value, source);
}

void resolveCompiledTranslate(const CssLengthSpec &length,
                              int nodeId,
                              LengthAxis axis,
                              int &px,
                              int &percent)
{
	const ResolvedCssLength resolved = resolveCompiledLengthForNodeDetailed(length, nodeId, axis);
	if (resolved.isPercent) {
		px = 0;
		percent = resolved.value;
		return;
	}
	px = resolved.value;
	percent = 0;
}

TransformComponents transformFromCompiled(const CssCompiledValue &compiled, int nodeId)
{
	TransformComponents t;
	t.rotateX = compiled.values[0];
	t.rotateY = compiled.values[1];
	t.rotateZ = compiled.values[2];
	t.scaleX = compiled.values[8];
	t.scaleY = compiled.values[9];
	t.hasRotateX = (compiled.flags & (1u << 0)) != 0;
	t.hasRotateY = (compiled.flags & (1u << 1)) != 0;
	t.hasRotateZ = (compiled.flags & (1u << 2)) != 0;
	t.hasTranslateX = (compiled.flags & (1u << 3)) != 0;
	t.hasTranslateY = (compiled.flags & (1u << 4)) != 0;
	t.hasTranslateZ = (compiled.flags & (1u << 5)) != 0;
	t.hasScaleX = (compiled.flags & (1u << 8)) != 0;
	t.hasScaleY = (compiled.flags & (1u << 9)) != 0;
	if (t.hasTranslateX)
		resolveCompiledTranslate(compiled.lengths[0], nodeId, LengthAxis::Horizontal, t.translateX, t.translateXPercent);
	if (t.hasTranslateY)
		resolveCompiledTranslate(compiled.lengths[1], nodeId, LengthAxis::Vertical, t.translateY, t.translateYPercent);
	if (t.hasTranslateZ) {
		t.translateZ = resolveCompiledLengthForNode(compiled.lengths[2], nodeId, LengthAxis::Horizontal);
	}
	return t;
}

bool applyCompiledColorValue(NodeHandle node,
                             CssDeclarationId declaration,
                             int styleColor,
                             int nativeColor,
                             int alpha,
                             StyleApplicationSource source)
{
	if (!node) return true;
	const int nodeId = node.id();
	Node &target = treeState().nodes[nodeId];
	switch (declaration) {
	case CssDeclarationId::Color:
		if (target.style.text_color == static_cast<style_color_t>(nativeColor) &&
		    target.style.text_alpha == static_cast<std::uint8_t>(alpha))
			return true;
		setStyleValueKnownTarget(node, target, Property::Color, styleColor, source);
		target.style.text_alpha = static_cast<std::uint8_t>(alpha);
		markNodeDisplayCommandsDirtyForStyleApply(nodeId);
		return true;
		case CssDeclarationId::ActiveBackgroundColor:
			setStyleValueKnownTarget(node, target, Property::ActiveBackgroundColor, styleColor, source);
			setStyleValueKnownTarget(node, target, Property::HasActiveBackground, 1, source);
			return true;
		case CssDeclarationId::Background: {
			const RareStyle &current = rstyle(target.style);
			if (target.style.has_bg == 1 &&
			    target.style.bg_color == static_cast<style_color_t>(nativeColor) &&
			    target.style.bg_alpha == static_cast<std::uint8_t>(alpha) &&
			    target.style.bg_fill == 0 &&
			    current.bg_gradient_has_mid == 0 &&
			    current.bg_overlay_gradient == 0 &&
			    current.bg_radial_gradient == 0 &&
			    current.bg_grid_axes == 0)
				return true;
			RareStyle &rs = rstyleMut(target.style);
			rs.bg_grid_axes = 0;
			rs.bg_grid_color = 0;
			rs.bg_grid_alpha = 255;
			rs.bg_grid_line_x = 0;
			rs.bg_grid_line_y = 0;
			rs.bg_overlay_gradient = 0;
			rs.bg_radial_gradient = 0;
			setStyleValueKnownTarget(node, target, Property::BackgroundColor, styleColor, source);
			setStyleValueKnownTarget(node, target, Property::HasBackground, 1, source);
			target.style.bg_fill = 0;
			target.style.bg_alpha = static_cast<std::uint8_t>(alpha);
			markNodeDisplayCommandsDirtyForStyleApply(nodeId);
			return true;
		}
	case CssDeclarationId::BorderColor:
		if (target.style.border_color == static_cast<style_color_t>(nativeColor) &&
		    target.style.border_alpha == static_cast<std::uint8_t>(alpha))
			return true;
		setStyleValueKnownTarget(node, target, Property::BorderColor, styleColor, source);
		target.style.border_alpha = static_cast<std::uint8_t>(alpha);
		markNodeDisplayCommandsDirtyForStyleApply(nodeId);
		return true;
	case CssDeclarationId::BorderTopColor:
	case CssDeclarationId::BorderRightColor:
	case CssDeclarationId::BorderBottomColor:
	case CssDeclarationId::BorderLeftColor: {
			const int side = declaration == CssDeclarationId::BorderTopColor ? 0 :
			    declaration == CssDeclarationId::BorderRightColor ? 1 :
			    declaration == CssDeclarationId::BorderBottomColor ? 2 : 3;
			const RareStyle &current = rstyle(target.style);
			if (current.border_side_color[side] == static_cast<style_color_t>(nativeColor) &&
			    current.border_side_alpha[side] == static_cast<std::uint8_t>(alpha))
				return true;
			setStyleValueKnownTarget(node, target, borderSideColorProperty(side), styleColor, source);
			rstyleMut(target.style).border_side_alpha[side] = static_cast<std::uint8_t>(alpha);
			markNodeDisplayCommandsDirtyForStyleApply(nodeId);
			return true;
		}
	default:
		return false;
	}
}

struct ResolvedCompiledCssColor {
	std::int32_t styleColor = 0;
	style_color_t nativeColor = 0;
	std::uint8_t alpha = 255;
};

bool resolveCompiledColorRef(int nodeId,
                             CssAtomId atom,
                             std::uint8_t hasFallback,
                             std::int32_t fallbackStyleColor,
                             style_color_t fallbackNativeColor,
                             std::uint8_t fallbackAlpha,
                             ResolvedCompiledCssColor &out)
{
	if (atom == kInvalidCssAtom) {
		out.styleColor = fallbackStyleColor;
		out.nativeColor = fallbackNativeColor;
		out.alpha = fallbackAlpha;
		return true;
	}
	if (const NodeCustomProperty *entry = lookupCustomPropertyEntry(nodeId, atom)) {
		if (entry->hasColor()) {
			out.styleColor = entry->colorStyle;
			out.nativeColor = static_cast<style_color_t>(entry->colorNative);
			out.alpha = entry->colorAlpha;
			return true;
		}
		const CachedCssColor color = cachedCssColorForValue(entry->value);
		if (!color.valid) return false;
		out.styleColor = color.styleColor;
		out.nativeColor = static_cast<style_color_t>(color.nativeColor);
		out.alpha = color.alpha;
		return true;
	}
	if (hasFallback == 0) return false;
	out.styleColor = fallbackStyleColor;
	out.nativeColor = fallbackNativeColor;
	out.alpha = fallbackAlpha;
	return true;
}

bool resolveCompiledLinearGradientColors(const CssCompiledLinearGradient &input,
                                         int nodeId,
                                         CssCompiledLinearGradient &out)
{
	out = input;
	ResolvedCompiledCssColor color;
	if (!resolveCompiledColorRef(nodeId,
	                             input.fromColorAtom,
	                             input.fromColorHasFallback,
	                             input.fromStyleColor,
	                             input.fromNativeColor,
	                             input.fromAlpha,
	                             color))
		return false;
	out.fromStyleColor = color.styleColor;
	out.fromNativeColor = color.nativeColor;
	out.fromAlpha = color.alpha;

	if (input.hasMid) {
		if (!resolveCompiledColorRef(nodeId,
		                             input.midColorAtom,
		                             input.midColorHasFallback,
		                             0,
		                             input.midNativeColor,
		                             input.midAlpha,
		                             color))
			return false;
		out.midNativeColor = color.nativeColor;
		out.midAlpha = color.alpha;
	}

	if (!resolveCompiledColorRef(nodeId,
	                             input.toColorAtom,
	                             input.toColorHasFallback,
	                             0,
	                             input.toNativeColor,
	                             input.toAlpha,
	                             color))
		return false;
	out.toNativeColor = color.nativeColor;
	out.toAlpha = color.alpha;
	return true;
}

bool resolveCompiledRadialGradientColors(const CssCompiledRadialGradient &input,
                                         int nodeId,
                                         CssCompiledRadialGradient &out)
{
	out = input;
	ResolvedCompiledCssColor color;
	if (!resolveCompiledColorRef(nodeId,
	                             input.fromColorAtom,
	                             input.fromColorHasFallback,
	                             0,
	                             input.fromNativeColor,
	                             input.fromAlpha,
	                             color))
		return false;
	out.fromNativeColor = color.nativeColor;
	out.fromAlpha = color.alpha;
	if (!resolveCompiledColorRef(nodeId,
	                             input.toColorAtom,
	                             input.toColorHasFallback,
	                             0,
	                             input.toNativeColor,
	                             input.toAlpha,
	                             color))
		return false;
	out.toNativeColor = color.nativeColor;
	out.toAlpha = color.alpha;
	return true;
}

void applyCompiledLinearGradient(RareStyle &rs, const CssCompiledLinearGradient &gradient)
{
	rs.bg_gradient_from_color = gradient.fromNativeColor;
	rs.bg_gradient_mid_color = gradient.midNativeColor;
	rs.bg_gradient_to_color = gradient.toNativeColor;
	rs.bg_gradient_from_alpha = gradient.fromAlpha;
	rs.bg_gradient_mid_alpha = gradient.midAlpha;
	rs.bg_gradient_to_alpha = gradient.toAlpha;
	rs.bg_gradient_mid_stop = gradient.midStopPermille;
	rs.bg_gradient_to_stop = gradient.toStopPermille;
	rs.bg_gradient_has_mid = gradient.hasMid;
	rs.bg_gradient_angle = gradient.angleTenths;
}

void applyCompiledOverlayGradient(RareStyle &rs, const CssCompiledLinearGradient &gradient)
{
	rs.bg_overlay_gradient = 1;
	rs.bg_overlay_gradient_from_color = gradient.fromNativeColor;
	rs.bg_overlay_gradient_mid_color = gradient.midNativeColor;
	rs.bg_overlay_gradient_to_color = gradient.toNativeColor;
	rs.bg_overlay_gradient_from_alpha = gradient.fromAlpha;
	rs.bg_overlay_gradient_mid_alpha = gradient.midAlpha;
	rs.bg_overlay_gradient_to_alpha = gradient.toAlpha;
	rs.bg_overlay_gradient_mid_stop = gradient.midStopPermille;
	rs.bg_overlay_gradient_to_stop = gradient.toStopPermille;
	rs.bg_overlay_gradient_has_mid = gradient.hasMid;
	rs.bg_overlay_gradient_angle = gradient.angleTenths;
}

void applyCompiledRadialGradient(RareStyle &rs, const CssCompiledRadialGradient &gradient)
{
	rs.bg_radial_gradient = 1;
	rs.bg_radial_gradient_from_color = gradient.fromNativeColor;
	rs.bg_radial_gradient_to_color = gradient.toNativeColor;
	rs.bg_radial_gradient_from_alpha = gradient.fromAlpha;
	rs.bg_radial_gradient_to_alpha = gradient.toAlpha;
	rs.bg_radial_gradient_stop = gradient.stopPermille;
	rs.bg_radial_gradient_cx = gradient.cxPermille;
	rs.bg_radial_gradient_cy = gradient.cyPermille;
	rs.bg_radial_gradient_rx = gradient.rxPermille;
	rs.bg_radial_gradient_ry = gradient.ryPermille;
}

bool compiledLinearGradientMatches(const RareStyle &rs, const CssCompiledLinearGradient &gradient)
{
	return rs.bg_gradient_from_color == gradient.fromNativeColor &&
	       rs.bg_gradient_mid_color == gradient.midNativeColor &&
	       rs.bg_gradient_to_color == gradient.toNativeColor &&
	       rs.bg_gradient_from_alpha == gradient.fromAlpha &&
	       rs.bg_gradient_mid_alpha == gradient.midAlpha &&
	       rs.bg_gradient_to_alpha == gradient.toAlpha &&
	       rs.bg_gradient_mid_stop == gradient.midStopPermille &&
	       rs.bg_gradient_to_stop == gradient.toStopPermille &&
	       rs.bg_gradient_has_mid == gradient.hasMid &&
	       rs.bg_gradient_angle == gradient.angleTenths;
}

bool compiledOverlayGradientMatches(const RareStyle &rs,
                                    const CssCompiledLinearGradient &gradient,
                                    bool present)
{
	if (!present) return rs.bg_overlay_gradient == 0;
	return rs.bg_overlay_gradient == 1 &&
	       rs.bg_overlay_gradient_from_color == gradient.fromNativeColor &&
	       rs.bg_overlay_gradient_mid_color == gradient.midNativeColor &&
	       rs.bg_overlay_gradient_to_color == gradient.toNativeColor &&
	       rs.bg_overlay_gradient_from_alpha == gradient.fromAlpha &&
	       rs.bg_overlay_gradient_mid_alpha == gradient.midAlpha &&
	       rs.bg_overlay_gradient_to_alpha == gradient.toAlpha &&
	       rs.bg_overlay_gradient_mid_stop == gradient.midStopPermille &&
	       rs.bg_overlay_gradient_to_stop == gradient.toStopPermille &&
	       rs.bg_overlay_gradient_has_mid == gradient.hasMid &&
	       rs.bg_overlay_gradient_angle == gradient.angleTenths;
}

bool compiledRadialGradientMatches(const RareStyle &rs,
                                   const CssCompiledRadialGradient &gradient,
                                   bool present)
{
	if (!present) return rs.bg_radial_gradient == 0;
	return rs.bg_radial_gradient == 1 &&
	       rs.bg_radial_gradient_from_color == gradient.fromNativeColor &&
	       rs.bg_radial_gradient_to_color == gradient.toNativeColor &&
	       rs.bg_radial_gradient_from_alpha == gradient.fromAlpha &&
	       rs.bg_radial_gradient_to_alpha == gradient.toAlpha &&
	       rs.bg_radial_gradient_stop == gradient.stopPermille &&
	       rs.bg_radial_gradient_cx == gradient.cxPermille &&
	       rs.bg_radial_gradient_cy == gradient.cyPermille &&
	       rs.bg_radial_gradient_rx == gradient.rxPermille &&
	       rs.bg_radial_gradient_ry == gradient.ryPermille;
}

std::uint8_t resolveCompiledGridLineWidth(const CssLengthSpec &length, int nodeId, LengthAxis axis)
{
	const int px = resolveCompiledLengthForNode(length, nodeId, axis);
	if (px <= 0) return 0;
	return static_cast<std::uint8_t>(std::max(1, std::min(px, 255)));
}

bool applyCompiledBackgroundValue(NodeHandle node,
                                  const CssCompiledBackground &background,
                                  StyleApplicationSource source)
{
	if (!node || !background.hasGradient) return false;
	const int nodeId = node.id();
	CssCompiledLinearGradient gradient;
	if (!resolveCompiledLinearGradientColors(background.gradient, nodeId, gradient)) return false;
	CssCompiledLinearGradient overlayGradient;
	if (background.hasOverlayGradient &&
	    !resolveCompiledLinearGradientColors(background.overlayGradient, nodeId, overlayGradient))
		return false;
	CssCompiledRadialGradient radialGradient;
	if (background.hasRadialGradient &&
	    !resolveCompiledRadialGradientColors(background.radialGradient, nodeId, radialGradient))
		return false;

	std::uint8_t gridAxes = background.gridAxes;
	std::uint8_t gridLineX = 0;
	std::uint8_t gridLineY = 0;
	if (background.hasGridLineX) {
		gridLineX = resolveCompiledGridLineWidth(background.gridLineX, nodeId, LengthAxis::None);
		if (gridLineX == 0) gridAxes &= static_cast<std::uint8_t>(~1u);
	}
	if (background.hasGridLineY) {
		gridLineY = resolveCompiledGridLineWidth(background.gridLineY, nodeId, LengthAxis::None);
		if (gridLineY == 0) gridAxes &= static_cast<std::uint8_t>(~2u);
	}

	Node &target = treeState().nodes[nodeId];
	const RareStyle &current = rstyle(target.style);
	if (target.style.has_bg == 1 &&
	    target.style.bg_color == gradient.fromNativeColor &&
	    target.style.bg_fill == 1 &&
	    target.style.bg_alpha == gradient.fromAlpha &&
	    compiledLinearGradientMatches(current, gradient) &&
	    compiledOverlayGradientMatches(current, overlayGradient, background.hasOverlayGradient) &&
	    compiledRadialGradientMatches(current, radialGradient, background.hasRadialGradient) &&
	    current.bg_grid_axes == gridAxes &&
	    current.bg_grid_color == background.gridColor &&
	    current.bg_grid_alpha == background.gridAlpha &&
	    current.bg_grid_line_x == gridLineX &&
	    current.bg_grid_line_y == gridLineY)
		return true;
	RareStyle &rs = rstyleMut(target.style);
	rs.bg_grid_axes = 0;
	rs.bg_grid_color = 0;
	rs.bg_grid_alpha = 255;
	rs.bg_grid_line_x = 0;
	rs.bg_grid_line_y = 0;
	rs.bg_overlay_gradient = 0;
	rs.bg_radial_gradient = 0;

	setStyleValue(node, Property::BackgroundColor, gradient.fromStyleColor, source);
	setStyleValue(node, Property::HasBackground, 1, source);
	target.style.bg_fill = 1;
	target.style.bg_alpha = gradient.fromAlpha;
	applyCompiledLinearGradient(rs, gradient);
	if (background.hasOverlayGradient) applyCompiledOverlayGradient(rs, overlayGradient);
	if (background.hasRadialGradient) applyCompiledRadialGradient(rs, radialGradient);
	rs.bg_grid_axes = gridAxes;
	rs.bg_grid_color = background.gridColor;
	rs.bg_grid_alpha = background.gridAlpha;
	rs.bg_grid_line_x = gridLineX;
	rs.bg_grid_line_y = gridLineY;
	markNodeDisplayCommandsDirtyForStyleApply(nodeId);
	return true;
}

bool applyCachedStaticBackgroundValue(NodeHandle node,
                                      const CssCompiledBackground &background,
                                      std::uint8_t gridAxes,
                                      std::uint8_t gridLineX,
                                      std::uint8_t gridLineY,
                                      StyleApplicationSource source)
{
	if (!node || !background.hasGradient) return false;
	if (source != StyleApplicationSource::ClassRule)
		return applyCompiledBackgroundValue(node, background, source);
	const int nodeId = node.id();
	auto &state = treeState();
	if (nodeId < 0 || nodeId >= state.nodeCount) return true;
	Node &target = state.nodes[nodeId];
	const CssCompiledLinearGradient &gradient = background.gradient;
	const RareStyle &current = rstyle(target.style);
	if (target.style.has_bg == 1 &&
	    target.style.bg_color == gradient.fromNativeColor &&
	    target.style.bg_fill == 1 &&
	    target.style.bg_alpha == gradient.fromAlpha &&
	    compiledLinearGradientMatches(current, gradient) &&
	    compiledOverlayGradientMatches(current, background.overlayGradient, background.hasOverlayGradient) &&
	    compiledRadialGradientMatches(current, background.radialGradient, background.hasRadialGradient) &&
	    current.bg_grid_axes == gridAxes &&
	    current.bg_grid_color == background.gridColor &&
	    current.bg_grid_alpha == background.gridAlpha &&
	    current.bg_grid_line_x == gridLineX &&
	    current.bg_grid_line_y == gridLineY)
		return true;

	target.style.bg_color = gradient.fromNativeColor;
	target.style.has_bg = 1;
	target.style.bg_fill = 1;
	target.style.bg_alpha = gradient.fromAlpha;

	RareStyle &rs = rstyleMut(target.style);
	rs.bg_grid_axes = 0;
	rs.bg_grid_color = 0;
	rs.bg_grid_alpha = 255;
	rs.bg_grid_line_x = 0;
	rs.bg_grid_line_y = 0;
	rs.bg_overlay_gradient = 0;
	rs.bg_radial_gradient = 0;
	applyCompiledLinearGradient(rs, gradient);
	if (background.hasOverlayGradient) applyCompiledOverlayGradient(rs, background.overlayGradient);
	if (background.hasRadialGradient) applyCompiledRadialGradient(rs, background.radialGradient);
	rs.bg_grid_axes = gridAxes;
	rs.bg_grid_color = background.gridColor;
	rs.bg_grid_alpha = background.gridAlpha;
	rs.bg_grid_line_x = gridLineX;
	rs.bg_grid_line_y = gridLineY;
	markNodeDisplayCommandsDirtyForStyleApply(nodeId);
	return true;
}

bool applyRuntimeFlexValue(NodeHandle node,
                           int grow,
                           int shrink,
                           std::uint8_t hasBasis,
                           const CssLengthSpec &basis,
                           StyleApplicationSource source)
{
	if (!node) return false;
	const int nodeId = node.id();
	setStyleValue(node, Property::Flex, grow, source);
	setStyleValue(node, Property::FlexShrink, shrink, source);
	setStyleValue(node,
	              Property::FlexBasis,
	              hasBasis != 0
	                  ? resolveCompiledLengthForNode(basis, nodeId, LengthAxis::Horizontal)
	                  : kUnset,
	              source);
	return true;
}

bool applyCompiledFlexValue(NodeHandle node, const CssCompiledValue &compiled, StyleApplicationSource source)
{
	if (compiled.kind != CssCompiledKind::Flex) return false;
	return applyRuntimeFlexValue(node,
	                             compiled.values[0],
	                             compiled.values[1],
	                             compiled.aux,
	                             compiled.lengths[0],
	                             source);
}

bool applyRuntimeFlexBasisValue(NodeHandle node,
                                std::uint8_t hasBasis,
                                const CssLengthSpec &basis,
                                StyleApplicationSource source)
{
	if (!node) return false;
	const int nodeId = node.id();
	setStyleValue(node,
	              Property::FlexBasis,
	              hasBasis != 0
	                  ? resolveCompiledLengthForNode(basis, nodeId, LengthAxis::Horizontal)
	                  : kUnset,
	              source);
	return true;
}

bool applyCompiledFlexBasisValue(NodeHandle node, const CssCompiledValue &compiled, StyleApplicationSource source)
{
	if (compiled.kind != CssCompiledKind::FlexBasis) return false;
	return applyRuntimeFlexBasisValue(node, compiled.aux, compiled.lengths[0], source);
}

bool applyRuntimeLengthValue(NodeHandle node,
                             CssDeclarationId declaration,
                             const CssLengthSpec &length,
                             StyleApplicationSource source)
{
	if (!node) return false;
	const int nodeId = node.id();
	switch (declaration) {
	case CssDeclarationId::Gap: setStyleValue(node, Property::Gap, resolveCompiledLengthForNode(length, nodeId, LengthAxis::Horizontal), source); return true;
	case CssDeclarationId::MinWidth: setStyleValue(node, Property::MinWidth, resolveCompiledLengthForNode(length, nodeId, LengthAxis::Horizontal), source); return true;
	case CssDeclarationId::MinHeight: setStyleValue(node, Property::MinHeight, resolveCompiledLengthForNode(length, nodeId, LengthAxis::Vertical), source); return true;
	case CssDeclarationId::MaxWidth: setStyleValue(node, Property::MaxWidth, resolveCompiledLengthForNode(length, nodeId, LengthAxis::Horizontal), source); return true;
	case CssDeclarationId::MaxHeight: setStyleValue(node, Property::MaxHeight, resolveCompiledLengthForNode(length, nodeId, LengthAxis::Vertical), source); return true;
	case CssDeclarationId::PaddingTop: setStyleValue(node, Property::PaddingTop, resolveCompiledLengthForNode(length, nodeId, LengthAxis::Vertical), source); return true;
	case CssDeclarationId::PaddingRight: setStyleValue(node, Property::PaddingRight, resolveCompiledLengthForNode(length, nodeId, LengthAxis::Horizontal), source); return true;
	case CssDeclarationId::PaddingBottom: setStyleValue(node, Property::PaddingBottom, resolveCompiledLengthForNode(length, nodeId, LengthAxis::Vertical), source); return true;
	case CssDeclarationId::PaddingLeft: setStyleValue(node, Property::PaddingLeft, resolveCompiledLengthForNode(length, nodeId, LengthAxis::Horizontal), source); return true;
	case CssDeclarationId::MarginTop: setStyleValue(node, Property::MarginTop, resolveCompiledLengthForNode(length, nodeId, LengthAxis::Vertical), source); return true;
	case CssDeclarationId::MarginRight: setStyleValue(node, Property::MarginRight, resolveCompiledLengthForNode(length, nodeId, LengthAxis::Horizontal), source); return true;
	case CssDeclarationId::MarginBottom: setStyleValue(node, Property::MarginBottom, resolveCompiledLengthForNode(length, nodeId, LengthAxis::Vertical), source); return true;
	case CssDeclarationId::MarginLeft: setStyleValue(node, Property::MarginLeft, resolveCompiledLengthForNode(length, nodeId, LengthAxis::Horizontal), source); return true;
	case CssDeclarationId::BorderWidth: setStyleValue(node, Property::BorderWidth, resolveCompiledLengthForNode(length, nodeId, LengthAxis::None), source); return true;
	case CssDeclarationId::BorderTopWidth: setStyleValue(node, Property::BorderTopWidth, resolveCompiledLengthForNode(length, nodeId, LengthAxis::None), source); return true;
	case CssDeclarationId::BorderRightWidth: setStyleValue(node, Property::BorderRightWidth, resolveCompiledLengthForNode(length, nodeId, LengthAxis::None), source); return true;
	case CssDeclarationId::BorderBottomWidth: setStyleValue(node, Property::BorderBottomWidth, resolveCompiledLengthForNode(length, nodeId, LengthAxis::None), source); return true;
	case CssDeclarationId::BorderLeftWidth: setStyleValue(node, Property::BorderLeftWidth, resolveCompiledLengthForNode(length, nodeId, LengthAxis::None), source); return true;
	case CssDeclarationId::FontSize: setStyleValue(node, Property::FontSize, resolveCompiledLengthForNode(length, nodeId, LengthAxis::Vertical), source); return true;
	case CssDeclarationId::Perspective: setStyleValue(node, Property::Perspective, resolveCompiledLengthForNode(length, nodeId, LengthAxis::Horizontal), source); return true;
	case CssDeclarationId::MaskImage:
		setStyleValue(node,
		              Property::MaskRightFadeWidth,
		              std::max(0, resolveCompiledLengthForNode(length, nodeId, LengthAxis::Horizontal)),
		              source);
		return true;
	default:
		return false;
	}
}

bool applyCompiledLengthValue(NodeHandle node, const CssCompiledValue &compiled, StyleApplicationSource source)
{
	if (compiled.kind != CssCompiledKind::Length) return false;
	return applyRuntimeLengthValue(node, compiled.declaration, compiled.lengths[0], source);
}

bool applyRuntimeSizeValue(NodeHandle node,
                           CssDeclarationId declaration,
                           const CssLengthSpec &length,
                           StyleApplicationSource source)
{
	if (!node) return false;
	if (declaration == CssDeclarationId::Width) {
		setCompiledSizeValue(node, Property::Width, Property::WidthPercent, length, LengthAxis::Horizontal, source);
		return true;
	}
	if (declaration == CssDeclarationId::Height) {
		setCompiledSizeValue(node, Property::Height, Property::HeightPercent, length, LengthAxis::Vertical, source);
		return true;
	}
	return false;
}

bool applyCompiledSizeValue(NodeHandle node, const CssCompiledValue &compiled, StyleApplicationSource source)
{
	if (compiled.kind != CssCompiledKind::Size) return false;
	return applyRuntimeSizeValue(node, compiled.declaration, compiled.lengths[0], source);
}

bool applyRuntimePositionOffsetValue(NodeHandle node,
                                     CssDeclarationId declaration,
                                     const CssLengthSpec &length,
                                     StyleApplicationSource source)
{
	if (!node) return false;
	if (declaration == CssDeclarationId::Top) {
		setCompiledPositionOffsetValue(node, Property::Top, Property::TopPercent, length, LengthAxis::Vertical, source);
		return true;
	}
	if (declaration == CssDeclarationId::Right) {
		setCompiledPositionOffsetValue(node, Property::Right, Property::RightPercent, length, LengthAxis::Horizontal, source);
		return true;
	}
	if (declaration == CssDeclarationId::Bottom) {
		setCompiledPositionOffsetValue(node, Property::Bottom, Property::BottomPercent, length, LengthAxis::Vertical, source);
		return true;
	}
	if (declaration == CssDeclarationId::Left) {
		setCompiledPositionOffsetValue(node, Property::Left, Property::LeftPercent, length, LengthAxis::Horizontal, source);
		return true;
	}
	return false;
}

bool applyCompiledPositionOffsetValue(NodeHandle node, const CssCompiledValue &compiled, StyleApplicationSource source)
{
	if (compiled.kind != CssCompiledKind::PositionOffset) return false;
	return applyRuntimePositionOffsetValue(node, compiled.declaration, compiled.lengths[0], source);
}

bool applyCompiledBoxValue(NodeHandle node, const CssCompiledValue &compiled, StyleApplicationSource source)
{
	if (!node || compiled.kind != CssCompiledKind::Box) return false;
	const int nodeId = node.id();
	const BoxLengths box{
	    resolveCompiledLengthForNode(compiled.lengths[0], nodeId, LengthAxis::Vertical),
	    resolveCompiledLengthForNode(compiled.lengths[1], nodeId, LengthAxis::Horizontal),
	    resolveCompiledLengthForNode(compiled.lengths[2], nodeId, LengthAxis::Vertical),
	    resolveCompiledLengthForNode(compiled.lengths[3], nodeId, LengthAxis::Horizontal)};
	if (compiled.declaration == CssDeclarationId::Padding) {
		setPaddingBox(node, box, source);
		return true;
	}
	if (compiled.declaration == CssDeclarationId::Margin) {
		setMarginBox(node, box, source);
		return true;
	}
	if (compiled.declaration == CssDeclarationId::Inset) {
		setStyleValue(node, Property::Top, box.top, source);
		setStyleValue(node, Property::Right, box.right, source);
		setStyleValue(node, Property::Bottom, box.bottom, source);
		setStyleValue(node, Property::Left, box.left, source);
		return true;
	}
	return false;
}

bool applyRuntimeBackgroundSizeValue(NodeHandle node,
                                     const CssLengthSpec &stepXLength,
                                     const CssLengthSpec &stepYLength)
{
	if (!node) return false;
	const int nodeId = node.id();
	auto &state = treeState();
	if (nodeId < 0 || nodeId >= state.nodeCount) return true;
	Node &target = state.nodes[nodeId];
	if (rstyle(target.style).bg_grid_axes == 0) return true;
	const int stepX = resolveCompiledLengthForNode(stepXLength, nodeId, LengthAxis::Horizontal);
	const int stepY = resolveCompiledLengthForNode(stepYLength, nodeId, LengthAxis::Vertical);
	RareStyle &rs = rstyleMut(target.style);
	if (stepX > 0) rs.bg_grid_step_x = static_cast<std::uint16_t>(std::min(stepX, 65535));
	if (stepY > 0) rs.bg_grid_step_y = static_cast<std::uint16_t>(std::min(stepY, 65535));
	markNodeDisplayCommandsDirtyForStyleApply(nodeId);
	return true;
}

bool applyCompiledBackgroundSizeValue(NodeHandle node, const CssCompiledValue &compiled)
{
	if (compiled.kind != CssCompiledKind::BackgroundSize) return false;
	return applyRuntimeBackgroundSizeValue(node, compiled.lengths[0], compiled.lengths[1]);
}

bool applyRuntimeBorderShorthandValue(NodeHandle node,
                                      const CssLengthSpec &width,
                                      int color,
                                      int alpha,
                                      StyleApplicationSource source)
{
	if (!node) return false;
	const int nodeId = node.id();
	auto &state = treeState();
	if (nodeId < 0 || nodeId >= state.nodeCount) return true;
	Node &target = state.nodes[nodeId];
	setStyleValue(node,
	              Property::BorderWidth,
	              resolveCompiledLengthForNode(width, nodeId, LengthAxis::None),
	              source);
	if (alpha >= 0) {
		setStyleValue(node, Property::BorderColor, color, source);
		target.style.border_alpha = static_cast<std::uint8_t>(alpha);
		markNodeDisplayCommandsDirtyForStyleApply(nodeId);
	}
	return true;
}

bool applyCompiledBorderShorthandValue(NodeHandle node, const CssCompiledValue &compiled, StyleApplicationSource source)
{
	if (compiled.kind != CssCompiledKind::BorderShorthand) return false;
	return applyRuntimeBorderShorthandValue(node,
	                                       compiled.lengths[0],
	                                       compiled.values[0],
	                                       compiled.aux != 0 ? compiled.values[1] : -1,
	                                       source);
}

bool applyRuntimeBorderSideShorthandValue(NodeHandle node,
                                          CssDeclarationId declaration,
                                          const CssLengthSpec &width,
                                          int color,
                                          int alpha,
                                          StyleApplicationSource source)
{
	if (!node) return false;
	const int side = borderSideForDeclaration(declaration);
	if (side < 0) return false;
	const int nodeId = node.id();
	auto &state = treeState();
	if (nodeId < 0 || nodeId >= state.nodeCount) return true;
	Node &target = state.nodes[nodeId];
	setStyleValue(node,
	              borderSideWidthProperty(side),
	              resolveCompiledLengthForNode(width, nodeId, LengthAxis::None),
	              source);
	if (alpha >= 0) {
		setStyleValue(node, borderSideColorProperty(side), color, source);
		rstyleMut(target.style).border_side_alpha[side] = static_cast<std::uint8_t>(alpha);
		markNodeDisplayCommandsDirtyForStyleApply(nodeId);
	}
	return true;
}

bool applyCompiledBorderSideShorthandValue(NodeHandle node, const CssCompiledValue &compiled, StyleApplicationSource source)
{
	if (compiled.kind != CssCompiledKind::BorderSideShorthand) return false;
	return applyRuntimeBorderSideShorthandValue(node,
	                                           compiled.declaration,
	                                           compiled.lengths[0],
	                                           compiled.values[0],
	                                           compiled.aux != 0 ? compiled.values[1] : -1,
	                                           source);
}

bool applyCompiledBorderRadiusValue(NodeHandle node, const CssCompiledValue &compiled, StyleApplicationSource source)
{
	if (!node || compiled.kind != CssCompiledKind::BorderRadius) return false;
	if (compiled.declaration == CssDeclarationId::BorderRadius) {
		for (int corner = 0; corner < 4; ++corner)
			setCompiledBorderRadiusCornerValue(node, corner, compiled.lengths[corner], source);
		return true;
	}
	if (compiled.declaration == CssDeclarationId::BorderTopLeftRadius) {
		setCompiledBorderRadiusCornerValue(node, 0, compiled.lengths[0], source);
		return true;
	}
	if (compiled.declaration == CssDeclarationId::BorderTopRightRadius) {
		setCompiledBorderRadiusCornerValue(node, 1, compiled.lengths[0], source);
		return true;
	}
	if (compiled.declaration == CssDeclarationId::BorderBottomRightRadius) {
		setCompiledBorderRadiusCornerValue(node, 2, compiled.lengths[0], source);
		return true;
	}
	if (compiled.declaration == CssDeclarationId::BorderBottomLeftRadius) {
		setCompiledBorderRadiusCornerValue(node, 3, compiled.lengths[0], source);
		return true;
	}
	return false;
}

bool applyRuntimeFilterBlurValue(NodeHandle node,
                                 std::uint8_t hasRadius,
                                 const CssLengthSpec &length,
                                 StyleApplicationSource source)
{
	if (!node) return false;
	const int nodeId = node.id();
	int radius = hasRadius == 0 ? 0 : resolveCompiledLengthForNode(length, nodeId, LengthAxis::None);
	if (radius < 0) radius = 0;
	if (radius > 64) radius = 64;
	setStyleValue(node, Property::FilterBlur, radius, source);
	return true;
}

bool applyCompiledFilterBlurValue(NodeHandle node, const CssCompiledValue &compiled, StyleApplicationSource source)
{
	if (compiled.kind != CssCompiledKind::FilterBlur) return false;
	return applyRuntimeFilterBlurValue(node, compiled.aux, compiled.lengths[0], source);
}

bool applyCompiledBoxShadowValue(NodeHandle node, const CssCompiledValue &compiled, StyleApplicationSource source)
{
	if (!node || compiled.kind != CssCompiledKind::BoxShadow) return false;
	const int nodeId = node.id();
	if (compiled.aux == 0 || compiled.values[1] == 0) {
		setStyleValue(node, Property::BoxShadowInset, 0, source);
		setStyleValue(node, Property::BoxShadowOffsetX, 0, source);
		setStyleValue(node, Property::BoxShadowOffsetY, 0, source);
		setStyleValue(node, Property::BoxShadowBlur, 0, source);
		setStyleValue(node, Property::BoxShadowSpread, 0, source);
		setStyleValue(node, Property::BoxShadowColor, 0, source);
		setStyleValue(node, Property::BoxShadowAlpha, 0, source);
		return true;
	}
	int blur = resolveCompiledLengthForNode(compiled.lengths[2], nodeId, LengthAxis::None);
	if (blur < 0) blur = 0;
	if (blur > 96) blur = 96;
	setStyleValue(node,
	              Property::BoxShadowOffsetX,
	              resolveCompiledLengthForNode(compiled.lengths[0], nodeId, LengthAxis::Horizontal),
	              source);
	setStyleValue(node,
	              Property::BoxShadowOffsetY,
	              resolveCompiledLengthForNode(compiled.lengths[1], nodeId, LengthAxis::Vertical),
	              source);
	setStyleValue(node, Property::BoxShadowBlur, blur, source);
	setStyleValue(node,
	              Property::BoxShadowSpread,
	              resolveCompiledLengthForNode(compiled.lengths[3], nodeId, LengthAxis::None),
	              source);
	setStyleValue(node, Property::BoxShadowColor, compiled.values[0], source);
	setStyleValue(node, Property::BoxShadowAlpha, compiled.values[1], source);
	setStyleValue(node, Property::BoxShadowInset, 1, source);
	return true;
}

bool applyRuntimeLineHeightValue(NodeHandle node,
                                 std::uint8_t kind,
                                 const CssLengthSpec &length,
                                 StyleApplicationSource source)
{
	if (!node) return false;
	const int nodeId = node.id();
	if (kind == 0) {
		setStyleValue(node, Property::LineHeight, 0, source);
		return true;
	}
	if (kind == 1) {
		setStyleValue(node,
		              Property::LineHeight,
		              roundToInt(static_cast<double>(currentFontSizeForNode(nodeId)) *
		                         static_cast<double>(length.value)),
		              source);
		return true;
	}
	if (kind == 2) {
		setStyleValue(node,
		              Property::LineHeight,
		              roundToInt(static_cast<double>(currentFontSizeForNode(nodeId)) *
		                         static_cast<double>(length.value) / 100.0),
		              source);
		return true;
	}
	if (kind == 3) {
		setStyleValue(node,
		              Property::LineHeight,
		              resolveCompiledLengthForNode(length, nodeId, LengthAxis::Vertical),
		              source);
		return true;
	}
	return false;
}

bool applyCompiledLineHeightValue(NodeHandle node, const CssCompiledValue &compiled, StyleApplicationSource source)
{
	if (compiled.kind != CssCompiledKind::LineHeight) return false;
	return applyRuntimeLineHeightValue(node, compiled.aux, compiled.lengths[0], source);
}

bool applyCompiledCssValueWithSource(NodeHandle node, const CssCompiledValue &compiled, StyleApplicationSource source)
{
	if (!node) return true;
	const int nodeId = node.id();
	switch (compiled.kind) {
	case CssCompiledKind::Noop:
		return true;
		case CssCompiledKind::DirectProperty: {
			const int propertyIndex = compiled.values[0];
			if (propertyIndex < 0 || propertyIndex >= static_cast<int>(Property::Count)) return false;
			setStyleValue(node, static_cast<Property>(propertyIndex), compiled.values[1], source);
			return true;
		}
		case CssCompiledKind::DirectPropertyGroup: {
			int count = compiled.values[0];
			if (count < 0) count = 0;
			if (count > 4) count = 4;
			for (int i = 0; i < count; ++i) {
				const int propertyIndex = compiled.values[1 + i * 2];
				if (propertyIndex < 0 || propertyIndex >= static_cast<int>(Property::Count)) return false;
				setStyleValue(node, static_cast<Property>(propertyIndex), compiled.values[2 + i * 2], source);
			}
			return true;
		}
		case CssCompiledKind::Keyword:
		switch (compiled.declaration) {
		case CssDeclarationId::Display: setStyleValue(node, Property::Display, compiled.values[0], source); return true;
		case CssDeclarationId::ObjectFit: setStyleValue(node, Property::ImageFit, compiled.values[0], source); return true;
		case CssDeclarationId::FlexDirection: setStyleValue(node, Property::FlexDirection, compiled.values[0], source); return true;
		case CssDeclarationId::FlexWrap: setStyleValue(node, Property::FlexWrap, compiled.values[0], source); return true;
		case CssDeclarationId::JustifyContent: setStyleValue(node, Property::JustifyContent, compiled.values[0], source); return true;
		case CssDeclarationId::AlignItems: setStyleValue(node, Property::AlignItems, compiled.values[0], source); return true;
		case CssDeclarationId::JustifyItems: setStyleValue(node, Property::JustifyItems, compiled.values[0], source); return true;
		case CssDeclarationId::AlignContent: setStyleValue(node, Property::AlignContent, compiled.values[0], source); return true;
		case CssDeclarationId::AlignSelf: setStyleValue(node, Property::AlignSelf, compiled.values[0], source); return true;
		case CssDeclarationId::PlaceItems:
			setStyleValue(node, Property::AlignItems, compiled.values[0], source);
			setStyleValue(node, Property::JustifyItems, compiled.values[0], source);
			setStyleValue(node, Property::JustifyContent, compiled.values[0], source);
			return true;
		case CssDeclarationId::Position: setStyleValue(node, Property::Position, compiled.values[0], source); return true;
		case CssDeclarationId::TextAlign: setStyleValue(node, Property::TextAlign, compiled.values[0], source); return true;
		case CssDeclarationId::TextDecoration: setStyleValue(node, Property::TextDecoration, compiled.values[0], source); return true;
		case CssDeclarationId::TextTransform: setStyleValue(node, Property::TextTransform, compiled.values[0], source); return true;
		case CssDeclarationId::WhiteSpace: setStyleValue(node, Property::WhiteSpace, compiled.values[0], source); return true;
		case CssDeclarationId::TextOverflow: setStyleValue(node, Property::TextOverflow, compiled.values[0], source); return true;
		case CssDeclarationId::BackfaceVisibility: setStyleValue(node, Property::Backface, compiled.values[0], source); return true;
		case CssDeclarationId::PointerEvents: setStyleValue(node, Property::PointerEvents, compiled.values[0], source); return true;
		case CssDeclarationId::Overflow: setStyleValue(node, Property::Overflow, compiled.values[0], source); return true;
		case CssDeclarationId::OverflowX: setStyleValue(node, Property::OverflowX, compiled.values[0], source); return true;
		case CssDeclarationId::OverflowY: setStyleValue(node, Property::OverflowY, compiled.values[0], source); return true;
		case CssDeclarationId::FontFamily: setStyleValue(node, Property::FontId, compiled.values[0], source); return true;
		case CssDeclarationId::FontWeight: setStyleValue(node, Property::FontWeight, compiled.values[0], source); return true;
		default: return false;
		}
	case CssCompiledKind::Opacity:
		setStyleValue(node, Property::Opacity, compiled.values[0], source);
		return true;
	case CssCompiledKind::Number:
		if (compiled.declaration == CssDeclarationId::ZIndex) {
			setStyleValue(node, Property::ZIndex, compiled.values[0], source);
			return true;
		}
		if (compiled.declaration == CssDeclarationId::FlexGrow) {
			setStyleValue(node, Property::Flex, compiled.values[0], source);
			return true;
		}
		if (compiled.declaration == CssDeclarationId::FlexShrink) {
			setStyleValue(node, Property::FlexShrink, compiled.values[0], source);
			return true;
		}
		if (compiled.declaration == CssDeclarationId::FontWeight) {
			setStyleValue(node, Property::FontWeight, compiled.values[0], source);
			return true;
		}
		return false;
	case CssCompiledKind::Flex:
		return applyCompiledFlexValue(node, compiled, source);
	case CssCompiledKind::FlexBasis:
		return applyCompiledFlexBasisValue(node, compiled, source);
	case CssCompiledKind::Length:
		return applyCompiledLengthValue(node, compiled, source);
	case CssCompiledKind::Size:
		return applyCompiledSizeValue(node, compiled, source);
	case CssCompiledKind::PositionOffset:
		return applyCompiledPositionOffsetValue(node, compiled, source);
	case CssCompiledKind::Box:
		return applyCompiledBoxValue(node, compiled, source);
	case CssCompiledKind::Color:
		return applyCompiledColorValue(node,
		                               compiled.declaration,
		                               compiled.values[0],
		                               compiled.values[1],
		                               compiled.values[2],
		                               source);
	case CssCompiledKind::ColorVar: {
		const CssAtomId atom = static_cast<CssAtomId>(compiled.values[0]);
		if (const NodeCustomProperty *entry = lookupCustomPropertyEntry(nodeId, atom)) {
			CachedCssColor color;
			if (entry->hasColor()) {
				color.styleColor = entry->colorStyle;
				color.nativeColor = entry->colorNative;
				color.alpha = entry->colorAlpha;
				color.valid = true;
			} else {
				color = cachedCssColorForValue(entry->value);
			}
			if (color.valid)
				return applyCompiledColorValue(node,
				                               compiled.declaration,
				                               color.styleColor,
				                               color.nativeColor,
				                               color.alpha,
				                               source);
			return false;
		}
		if (compiled.aux == 0) return false;
		return applyCompiledColorValue(node,
		                               compiled.declaration,
		                               compiled.values[1],
		                               compiled.values[2],
		                               compiled.values[3],
		                               source);
	}
	case CssCompiledKind::Rotate:
		setStyleValue(node, Property::TransformRotate, compiled.values[0], source);
		return true;
	case CssCompiledKind::Scale:
		setStyleValue(node, Property::TransformScaleX, compiled.values[0], source);
		setStyleValue(node, Property::TransformScaleY, compiled.values[0], source);
		return true;
	case CssCompiledKind::OriginPair:
		if (compiled.declaration == CssDeclarationId::TransformOrigin) {
			setStyleValue(node, Property::TransformOriginX, compiled.values[0], source);
			setStyleValue(node, Property::TransformOriginY, compiled.values[1], source);
			return true;
		}
		if (compiled.declaration == CssDeclarationId::PerspectiveOrigin) {
			setStyleValue(node, Property::PerspectiveOriginX, compiled.values[0], source);
			setStyleValue(node, Property::PerspectiveOriginY, compiled.values[1], source);
			return true;
		}
		return false;
	case CssCompiledKind::Transform: {
		const TransformComponents transform = transformFromCompiled(compiled, nodeId);
		if (!applyTransformComponentsFast(node, transform, source))
			setTransformComponents(node, transform, source);
		return true;
	}
	case CssCompiledKind::Background: {
		const CssCompiledBackground *background = compiledCssBackgroundForHandle(static_cast<std::uint16_t>(compiled.values[0]));
		return background ? applyCompiledBackgroundValue(node, *background, source) : false;
	}
	case CssCompiledKind::BackgroundSize:
		return applyCompiledBackgroundSizeValue(node, compiled);
	case CssCompiledKind::BorderShorthand:
		return applyCompiledBorderShorthandValue(node, compiled, source);
	case CssCompiledKind::BorderSideShorthand:
		return applyCompiledBorderSideShorthandValue(node, compiled, source);
	case CssCompiledKind::BorderRadius:
		return applyCompiledBorderRadiusValue(node, compiled, source);
	case CssCompiledKind::FilterBlur:
		return applyCompiledFilterBlurValue(node, compiled, source);
	case CssCompiledKind::BoxShadow:
		return applyCompiledBoxShadowValue(node, compiled, source);
	case CssCompiledKind::GridTemplate: {
		const CssCompiledGridTemplate *grid =
		    compiledCssGridTemplateForHandle(static_cast<std::uint16_t>(compiled.values[0]));
		if (!grid) return false;
		if (compiled.declaration == CssDeclarationId::GridTemplateColumns) {
			applyCompiledGridTemplateValue(node, *grid, true);
			return true;
		}
		if (compiled.declaration == CssDeclarationId::GridTemplateRows) {
			applyCompiledGridTemplateValue(node, *grid, false);
			return true;
		}
		return false;
	}
	case CssCompiledKind::LineHeight:
		return applyCompiledLineHeightValue(node, compiled, source);
	case CssCompiledKind::None:
		return false;
	}
	return false;
}

struct InlineCompiledStyleCacheEntry {
	CssDeclarationId declaration = CssDeclarationId::Unknown;
	std::uint16_t compiledValue = kNoCompiledCssValue;
	bool valid = false;
	bool compileAttempted = false;
	std::string value;
};

#if GEA_INLINE_COMPILED_STYLE_CACHE_ENTRIES > 0
InlineCompiledStyleCacheEntry g_inlineCompiledStyleCache[kInlineCompiledStyleCacheEntries];
int g_inlineCompiledStyleCacheNext = 0;
#endif

void clearInlineCompiledStyleCache()
{
#if GEA_INLINE_COMPILED_STYLE_CACHE_ENTRIES > 0
	for (InlineCompiledStyleCacheEntry &entry : g_inlineCompiledStyleCache)
		entry = InlineCompiledStyleCacheEntry{};
	g_inlineCompiledStyleCacheNext = 0;
#endif
}

const CssCompiledValue *inlineCompiledStyleValueFor(CssDeclarationId declaration, const std::string &value)
{
#if GEA_INLINE_COMPILED_STYLE_CACHE_ENTRIES <= 0
	(void)declaration;
	(void)value;
	return nullptr;
#else
	if (static_cast<std::uint32_t>(GEA_COMPILED_CSS_VALUE_MASK) == 0u) return nullptr;
	if (declaration == CssDeclarationId::Unknown || declaration == CssDeclarationId::Custom || value.empty())
		return nullptr;

	for (InlineCompiledStyleCacheEntry &entry : g_inlineCompiledStyleCache) {
		if (!entry.valid || entry.declaration != declaration || entry.value != value) continue;
		if (entry.compiledValue != kNoCompiledCssValue)
			return compiledCssValueForHandle(entry.compiledValue);
		if (entry.compileAttempted) return nullptr;
		entry.compileAttempted = true;
		entry.compiledValue = compileCssValue(declaration, CssText::view(value));
		return compiledCssValueForHandle(entry.compiledValue);
	}

	int slot = -1;
	for (int i = 0; i < kInlineCompiledStyleCacheEntries; ++i) {
		if (g_inlineCompiledStyleCache[i].valid) continue;
		slot = i;
		break;
	}
	if (slot < 0) {
		for (int offset = 0; offset < kInlineCompiledStyleCacheEntries; ++offset) {
			const int candidate = (g_inlineCompiledStyleCacheNext + offset) % kInlineCompiledStyleCacheEntries;
			if (g_inlineCompiledStyleCache[candidate].compiledValue != kNoCompiledCssValue) continue;
			slot = candidate;
			g_inlineCompiledStyleCacheNext = (candidate + 1) % kInlineCompiledStyleCacheEntries;
			break;
		}
	}
	if (slot < 0) return nullptr;
	InlineCompiledStyleCacheEntry &entry = g_inlineCompiledStyleCache[slot];
	entry = InlineCompiledStyleCacheEntry{};
	entry.valid = true;
	entry.declaration = declaration;
	entry.value = value;
	return nullptr;
#endif
}

void applyPropertyWithSource(NodeHandle node,
                             CssDeclarationId declaration,
                             const std::string &rawValue,
                             StyleApplicationSource source)
{
	if (!node) return;
	const int nodeId = node.id();
	if (declaration == CssDeclarationId::Unknown) return;
#if GEA_RECPROF
	g_profApplyCalls++;
	const int64_t _vt = recNow();
#endif
	const bool directValue = rawValue.find("var(") == std::string::npos && isTrimmedCssValue(rawValue);
	const CssCompiledValue *compiledValue = directValue
	    ? inlineCompiledStyleValueFor(declaration, rawValue)
	    : nullptr;
	std::string resolvedValue;
	if (!directValue)
		resolvedValue = resolveCssVarsForNode(rawValue, nodeId);
#if GEA_RECPROF
	g_profVarUs += recNow() - _vt;
	const int64_t _bt = recNow();
	struct BodyTimer { int64_t s; ~BodyTimer() { g_profBodyUs += recNow() - s; } } _bodyTimer{_bt};
#endif
	if (compiledValue && applyCompiledCssValueWithSource(node, *compiledValue, source)) return;
	const std::string &value = directValue ? rawValue : resolvedValue;
	(void)applyKnownResolvedPropertyWithSource(node, declaration, value, source);
}

void applyPropertyWithSource(NodeHandle node, const char *property, const std::string &rawValue, StyleApplicationSource source)
{
	if (!node || !property) return;
	const CssDeclarationId declaration = classifyDeclaration(property);
	if (declaration == CssDeclarationId::Custom) {
		setCustomPropertyValue(ensureRareData(node.id()).customProperties, internCssAtom(property), rawValue);
		return;
	}
	applyPropertyWithSource(node, declaration, rawValue, source);
}

void applyPropertyWithSource(NodeHandle node, const std::string &property, const std::string &rawValue, StyleApplicationSource source)
{
	applyPropertyWithSource(node, property.c_str(), rawValue, source);
}

void applyRulePropertyWithCompiledValue(NodeHandle node,
                                        const CssRule &rule,
                                        const CssCompiledValue *compiled,
                                        StyleApplicationSource source)
{
	if (!node) return;
	const int nodeId = node.id();
	if (rule.declaration == CssDeclarationId::Custom) {
		setCustomPropertyRuleValue(ensureRareData(nodeId).customProperties,
		                           rule.propertyAtom,
		                           cssRuleTextForHandle(rule.valueText),
		                           nodeId,
		                           rule.compiledValue);
		return;
	}
#if GEA_RECPROF
	g_profApplyCalls++;
	const int64_t _vt = recNow();
#endif
	if (compiled) {
		if (applyCompiledCssValueWithSource(node, *compiled, source)) return;
		if (rule.valueText == kNoCssRuleText) return;
	}
	const CssText &rawValue = cssRuleTextForHandle(rule.valueText);
	const std::string value = resolveCssVarsForNode(rawValue, nodeId);
#if GEA_RECPROF
	g_profVarUs += recNow() - _vt;
	const int64_t _bt = recNow();
	struct BodyTimer { int64_t s; ~BodyTimer() { g_profBodyUs += recNow() - s; } } _bodyTimer{_bt};
#endif
	if (applyKnownResolvedPropertyWithSource(node, rule.declaration, value, source)) return;
	applyPropertyWithSource(node,
	                        cssRuleTextForHandle(rule.propertyText).str(),
	                        rawValue.str(),
	                        source);
}

void applyRulePropertyWithSource(NodeHandle node, const CssRule &rule, StyleApplicationSource source)
{
	applyRulePropertyWithCompiledValue(node, rule, compiledCssValueForHandle(rule.compiledValue), source);
}

bool removeInlineStyleProperties(int node, std::initializer_list<Property> properties)
{
	auto &state = treeState();
	if (node < 0 || node >= state.nodeCount) return false;
	bool changed = false;
	if (NodeRareData *rd = rareDataFor(node))
		for (const Property property : properties) changed = rd->inlineStyles.remove(property) || changed;
	if (changed) recomputeSubtreeClassStyles(node);
	return changed;
}

bool removeInlineStyleProperty(NodeHandle node, const std::string &property)
{
	if (!node) return false;
	const int id = node.id();
	if (property == "display") return removeInlineStyleProperties(id, {Property::Display});
	if (property == "flex-direction") return removeInlineStyleProperties(id, {Property::FlexDirection});
	if (property == "flex-wrap") return removeInlineStyleProperties(id, {Property::FlexWrap});
	if (property == "justify-content") return removeInlineStyleProperties(id, {Property::JustifyContent});
	if (property == "align-items") return removeInlineStyleProperties(id, {Property::AlignItems});
	if (property == "justify-items") return removeInlineStyleProperties(id, {Property::JustifyItems});
	if (property == "align-content") return removeInlineStyleProperties(id, {Property::AlignContent});
	if (property == "align-self") return removeInlineStyleProperties(id, {Property::AlignSelf});
	if (property == "place-items") return removeInlineStyleProperties(id, {Property::AlignItems, Property::JustifyItems});
	if (property == "gap") return removeInlineStyleProperties(id, {Property::Gap});
	if (property == "width") return removeInlineStyleProperties(id, {Property::Width, Property::WidthPercent});
	if (property == "height") return removeInlineStyleProperties(id, {Property::Height, Property::HeightPercent});
	if (property == "min-width") return removeInlineStyleProperties(id, {Property::MinWidth});
	if (property == "min-height") return removeInlineStyleProperties(id, {Property::MinHeight});
	if (property == "max-width") return removeInlineStyleProperties(id, {Property::MaxWidth});
	if (property == "max-height") return removeInlineStyleProperties(id, {Property::MaxHeight});
	if (property == "flex") return removeInlineStyleProperties(id, {Property::Flex, Property::FlexShrink, Property::FlexBasis});
	if (property == "flex-grow") return removeInlineStyleProperties(id, {Property::Flex});
	if (property == "flex-shrink") return removeInlineStyleProperties(id, {Property::FlexShrink});
	if (property == "flex-basis") return removeInlineStyleProperties(id, {Property::FlexBasis});
	if (property == "padding")
		return removeInlineStyleProperties(id, {Property::PaddingTop, Property::PaddingRight, Property::PaddingBottom, Property::PaddingLeft});
	if (property == "padding-top") return removeInlineStyleProperties(id, {Property::PaddingTop});
	if (property == "padding-right") return removeInlineStyleProperties(id, {Property::PaddingRight});
	if (property == "padding-bottom") return removeInlineStyleProperties(id, {Property::PaddingBottom});
	if (property == "padding-left") return removeInlineStyleProperties(id, {Property::PaddingLeft});
	if (property == "margin")
		return removeInlineStyleProperties(id, {Property::MarginTop, Property::MarginRight, Property::MarginBottom, Property::MarginLeft});
	if (property == "margin-top") return removeInlineStyleProperties(id, {Property::MarginTop});
	if (property == "margin-right") return removeInlineStyleProperties(id, {Property::MarginRight});
	if (property == "margin-bottom") return removeInlineStyleProperties(id, {Property::MarginBottom});
	if (property == "margin-left") return removeInlineStyleProperties(id, {Property::MarginLeft});
	if (property == "position") return removeInlineStyleProperties(id, {Property::Position});
	if (property == "top") return removeInlineStyleProperties(id, {Property::Top, Property::TopPercent});
	if (property == "right") return removeInlineStyleProperties(id, {Property::Right, Property::RightPercent});
	if (property == "bottom") return removeInlineStyleProperties(id, {Property::Bottom, Property::BottomPercent});
	if (property == "left") return removeInlineStyleProperties(id, {Property::Left, Property::LeftPercent});
	if (property == "inset")
		return removeInlineStyleProperties(id,
		                                   {Property::Top,
		                                    Property::Right,
		                                    Property::Bottom,
		                                    Property::Left,
		                                    Property::TopPercent,
		                                    Property::RightPercent,
		                                    Property::BottomPercent,
		                                    Property::LeftPercent});
	if (property == "z-index") return removeInlineStyleProperties(id, {Property::ZIndex});
	if (property == "background" || property == "background-color")
		return removeInlineStyleProperties(id, {Property::BackgroundColor, Property::HasBackground});
	if (property == "color") return removeInlineStyleProperties(id, {Property::Color});
	if (property == "opacity") return removeInlineStyleProperties(id, {Property::Opacity});
	if (property == "border-color") return removeInlineStyleProperties(id, {Property::BorderColor});
	if (property == "border-width") return removeInlineStyleProperties(id, {Property::BorderWidth});
	if (property == "border") return removeInlineStyleProperties(id, {Property::BorderWidth, Property::BorderColor});
	if (property == "border-top") return removeInlineStyleProperties(id, {Property::BorderTopWidth, Property::BorderTopColor});
	if (property == "border-right") return removeInlineStyleProperties(id, {Property::BorderRightWidth, Property::BorderRightColor});
	if (property == "border-bottom") return removeInlineStyleProperties(id, {Property::BorderBottomWidth, Property::BorderBottomColor});
	if (property == "border-left") return removeInlineStyleProperties(id, {Property::BorderLeftWidth, Property::BorderLeftColor});
	if (property == "border-top-width") return removeInlineStyleProperties(id, {Property::BorderTopWidth});
	if (property == "border-right-width") return removeInlineStyleProperties(id, {Property::BorderRightWidth});
	if (property == "border-bottom-width") return removeInlineStyleProperties(id, {Property::BorderBottomWidth});
	if (property == "border-left-width") return removeInlineStyleProperties(id, {Property::BorderLeftWidth});
	if (property == "border-top-color") return removeInlineStyleProperties(id, {Property::BorderTopColor});
	if (property == "border-right-color") return removeInlineStyleProperties(id, {Property::BorderRightColor});
	if (property == "border-bottom-color") return removeInlineStyleProperties(id, {Property::BorderBottomColor});
	if (property == "border-left-color") return removeInlineStyleProperties(id, {Property::BorderLeftColor});
	if (property == "border-radius")
		return removeInlineStyleProperties(id,
		                                   {Property::BorderRadiusTopLeft,
		                                    Property::BorderRadiusTopRight,
		                                    Property::BorderRadiusBottomRight,
		                                    Property::BorderRadiusBottomLeft,
		                                    Property::BorderRadiusTopLeftPercent,
		                                    Property::BorderRadiusTopRightPercent,
		                                    Property::BorderRadiusBottomRightPercent,
		                                    Property::BorderRadiusBottomLeftPercent});
	if (property == "border-top-left-radius")
		return removeInlineStyleProperties(id, {Property::BorderRadiusTopLeft, Property::BorderRadiusTopLeftPercent});
	if (property == "border-top-right-radius")
		return removeInlineStyleProperties(id, {Property::BorderRadiusTopRight, Property::BorderRadiusTopRightPercent});
	if (property == "border-bottom-right-radius")
		return removeInlineStyleProperties(id, {Property::BorderRadiusBottomRight, Property::BorderRadiusBottomRightPercent});
	if (property == "border-bottom-left-radius")
		return removeInlineStyleProperties(id, {Property::BorderRadiusBottomLeft, Property::BorderRadiusBottomLeftPercent});
	if (property == "font-family") return removeInlineStyleProperties(id, {Property::FontId});
	if (property == "font-size") return removeInlineStyleProperties(id, {Property::FontSize});
	if (property == "font-weight") return removeInlineStyleProperties(id, {Property::FontWeight});
	if (property == "line-height") return removeInlineStyleProperties(id, {Property::LineHeight});
	if (property == "text-align") return removeInlineStyleProperties(id, {Property::TextAlign});
	if (property == "text-decoration" || property == "text-decoration-line")
		return removeInlineStyleProperties(id, {Property::TextDecoration});
	if (property == "text-transform") return removeInlineStyleProperties(id, {Property::TextTransform});
	if (property == "white-space") return removeInlineStyleProperties(id, {Property::WhiteSpace});
	if (property == "text-overflow") return removeInlineStyleProperties(id, {Property::TextOverflow});
	if (property == "backface-visibility") return removeInlineStyleProperties(id, {Property::Backface});
	if (property == "pointer-events") return removeInlineStyleProperties(id, {Property::PointerEvents});
	if (property == "overflow") return removeInlineStyleProperties(id, {Property::Overflow, Property::OverflowX, Property::OverflowY});
	if (property == "overflow-x") return removeInlineStyleProperties(id, {Property::OverflowX});
	if (property == "overflow-y") return removeInlineStyleProperties(id, {Property::OverflowY});
	if (property == "mask-image" || property == "-webkit-mask-image")
		return removeInlineStyleProperties(id, {Property::MaskRightFadeWidth});
	if (property == "transform")
		return removeInlineStyleProperties(id,
		                                   {Property::TransformRotate,
		                                    Property::TransformRotateX,
		                                    Property::TransformRotateY,
		                                    Property::TransformTranslateX,
		                                    Property::TransformTranslateY,
		                                    Property::TransformTranslateZ,
		                                    Property::TransformTranslateXPercent,
		                                    Property::TransformTranslateYPercent,
		                                    Property::TransformScaleX,
		                                    Property::TransformScaleY});
	if (property == "rotate") return removeInlineStyleProperties(id, {Property::TransformRotate});
	if (property == "filter") return removeInlineStyleProperties(id, {Property::FilterBlur});
	if (property == "box-shadow")
		return removeInlineStyleProperties(id,
		                                   {Property::BoxShadowInset,
		                                    Property::BoxShadowOffsetX,
		                                    Property::BoxShadowOffsetY,
		                                    Property::BoxShadowBlur,
		                                    Property::BoxShadowSpread,
		                                    Property::BoxShadowColor,
		                                    Property::BoxShadowAlpha});
	if (property == "transform-origin")
		return removeInlineStyleProperties(id, {Property::TransformOriginX, Property::TransformOriginY});
	if (property == "perspective") return removeInlineStyleProperties(id, {Property::Perspective});
	if (property == "perspective-origin")
		return removeInlineStyleProperties(id, {Property::PerspectiveOriginX, Property::PerspectiveOriginY});
	return false;
}

void replayInlineStyles(int node)
{
	auto &state = treeState();
	if (node < 0 || node >= state.nodeCount) return;
	if (const NodeRareData *rd = rareDataFor(node))
		for (std::size_t i = 0, n = rd->inlineStyles.size(); i < n; ++i) {
			const NodeStyleOverride &entry = rd->inlineStyles.at(i);
			Tree::instance().setStyleFromClass(node, entry.property, entry.value);
		}
}

void applyDefaultStyleOverrides(int node)
{
	auto &state = treeState();
	if (node < 0 || node >= state.nodeCount) return;
	if (const NodeRareData *rd = rareDataFor(node))
		for (std::size_t i = 0, n = rd->defaultStyles.size(); i < n; ++i) {
			const NodeStyleOverride &entry = rd->defaultStyles.at(i);
			Tree::instance().setStyleFromClass(node, entry.property, entry.value);
		}
}

int16_t g_pseudoBeforeTagId = -1;
int16_t g_pseudoAfterTagId = -1;

int16_t pseudoBeforeTagId()
{
	if (g_pseudoBeforeTagId < 0) g_pseudoBeforeTagId = internTag("::before");
	return g_pseudoBeforeTagId;
}

int16_t pseudoAfterTagId()
{
	if (g_pseudoAfterTagId < 0) g_pseudoAfterTagId = internTag("::after");
	return g_pseudoAfterTagId;
}

int16_t pseudoTagId(CssRule::PseudoElement pseudo)
{
	return pseudo == CssRule::PseudoElement::Before ? pseudoBeforeTagId() : pseudoAfterTagId();
}

bool isGeneratedPseudoNode(const Node &node)
{
	const int16_t tag = node.tag_id;
	return tag == pseudoBeforeTagId() || tag == pseudoAfterTagId();
}

std::string normalizeSelectorText(const std::string &selector)
{
	std::string out = trimCssValue(selector);
	std::size_t write = 0;
	bool previousSpace = false;
	for (std::size_t read = 0; read < out.size(); ++read) {
		const unsigned char c = static_cast<unsigned char>(out[read]);
		if (c <= ' ') {
			if (!previousSpace) out[write++] = ' ';
			previousSpace = true;
		} else {
			out[write++] = out[read];
			previousSpace = false;
		}
	}
	out.resize(write);
	if (!out.empty() && out.back() == ' ') out.pop_back();
	return out;
}

bool isRootNode(int node)
{
	const auto &state = treeState();
	if (node < 0 || node >= state.nodeCount) return false;
	const int mountedRoot = Tree::instance().mountedRoot();
	if (mountedRoot >= 0) return node == mountedRoot;
	return state.nodes[node].parent < 0;
}

bool isFirstElementChild(int node)
{
	const auto &state = treeState();
	if (node < 0 || node >= state.nodeCount) return false;
	const int parent = state.nodes[node].parent;
	if (parent < 0 || parent >= state.nodeCount) return false;
	for (int child = state.nodes[parent].first_child; child >= 0; child = state.nodes[child].next_sibling) {
		if (isGeneratedPseudoNode(state.nodes[child])) continue;
		return child == node;
	}
	return false;
}

bool isLastElementChild(int node)
{
	const auto &state = treeState();
	if (node < 0 || node >= state.nodeCount) return false;
	const int parent = state.nodes[node].parent;
	if (parent < 0 || parent >= state.nodeCount) return false;
	for (int child = state.nodes[parent].last_child; child >= 0; child = state.nodes[child].prev_sibling) {
		if (isGeneratedPseudoNode(state.nodes[child])) continue;
		return child == node;
	}
	return false;
}

template <typename T, std::size_t InlineCount>
struct SmallSelectorList {
	T inlineValues[InlineCount]{};
	std::vector<T> spillValues;
	std::uint8_t inlineCount = 0;
	bool spilled = false;

	std::size_t size() const { return spilled ? spillValues.size() : inlineCount; }
	bool empty() const { return size() == 0; }

	T &at(std::size_t index)
	{
		return spilled ? spillValues[index] : inlineValues[index];
	}

	const T &at(std::size_t index) const
	{
		return spilled ? spillValues[index] : inlineValues[index];
	}

	T &front() { return at(0); }
	const T &front() const { return at(0); }
	T &back() { return at(size() - 1); }
	const T &back() const { return at(size() - 1); }

	void push_back(const T &value)
	{
		if (!spilled && inlineCount < InlineCount) {
			inlineValues[inlineCount++] = value;
			return;
		}
		ensureSpilled();
		spillValues.push_back(value);
	}

	void push_back(T &&value)
	{
		if (!spilled && inlineCount < InlineCount) {
			inlineValues[inlineCount++] = std::move(value);
			return;
		}
		ensureSpilled();
		spillValues.push_back(std::move(value));
	}

private:
	void ensureSpilled()
	{
		if (spilled) return;
		spillValues.assign(inlineValues, inlineValues + inlineCount);
		spilled = true;
	}
};

bool equalsLiteral(const char *text, std::size_t length, const char *literal)
{
	const std::size_t literalLength = std::strlen(literal ? literal : "");
	return length == literalLength && std::memcmp(text ? text : "", literal ? literal : "", length) == 0;
}

// A simple (compound) selector parsed into its constituents. `valid` is false for
// syntactically unmatchable inputs (empty, contains "::", unknown pseudo, bad
// marker). The match against a specific node stays per-call; only the parse is
// cached, since parsing the compound string each call dominated selector matching
// during recompute.
struct ParsedSimpleSelector {
	int16_t tagId = -1;
	CssAtomId idAtom = kInvalidCssAtom;
	SmallSelectorList<CssAtomId, 4> classIds;
	bool hasTag = false;
	bool wantsRoot = false;
	bool wantsFirstChild = false;
	bool wantsLastChild = false;
	bool rootTag = false;
	bool hasMatcher = false;
	bool valid = true;
};

ParsedSimpleSelector parseSimpleSelector(const char *rawSimple, std::size_t rawLength)
{
	ParsedSimpleSelector p;
	const char *simple = rawSimple ? rawSimple : "";
	std::size_t startOffset = 0;
	std::size_t endOffset = rawLength;
	while (startOffset < endOffset && static_cast<unsigned char>(simple[startOffset]) <= ' ') ++startOffset;
	while (endOffset > startOffset && static_cast<unsigned char>(simple[endOffset - 1]) <= ' ') --endOffset;
	if (startOffset == endOffset) {
		p.valid = false;
		return p;
	}
	for (std::size_t j = startOffset; j + 1 < endOffset; ++j) {
		if (simple[j] == ':' && simple[j + 1] == ':') {
			p.valid = false;
			return p;
		}
	}
	std::size_t i = startOffset;
	if (simple[i] != '.' && simple[i] != '#' && simple[i] != ':') {
		const std::size_t start = i;
		while (i < endOffset && simple[i] != '.' && simple[i] != '#' && simple[i] != ':') ++i;
		const std::string tag = toLowerAscii(std::string(simple + start, i - start));
		p.hasTag = true;
		if (tag == "body" || tag == "html")
			p.rootTag = true;
		else
			p.tagId = internTag(tag.c_str());
	}
	while (i < endOffset) {
		const char marker = simple[i++];
		const std::size_t start = i;
		if (marker == '.') {
			while (i < endOffset && simple[i] != '.' && simple[i] != '#' && simple[i] != ':') ++i;
			p.classIds.push_back(internCssAtom(simple + start, i - start));
		} else if (marker == '#') {
			while (i < endOffset && simple[i] != '.' && simple[i] != '#' && simple[i] != ':') ++i;
			p.idAtom = internCssAtom(simple + start, i - start);
		} else if (marker == ':') {
			while (i < endOffset && simple[i] != '.' && simple[i] != '#' && simple[i] != ':') ++i;
			const std::size_t pseudoLength = i - start;
			if (equalsLiteral(simple + start, pseudoLength, "root")) p.wantsRoot = true;
			else if (equalsLiteral(simple + start, pseudoLength, "first-child")) p.wantsFirstChild = true;
			else if (equalsLiteral(simple + start, pseudoLength, "last-child")) p.wantsLastChild = true;
			else { p.valid = false; return p; }
		} else {
			p.valid = false;
			return p;
		}
	}
	p.hasMatcher = p.wantsRoot || p.wantsFirstChild || p.wantsLastChild ||
	               p.hasTag || p.idAtom != kInvalidCssAtom || !p.classIds.empty();
	return p;
}

struct SelectorPart {
	ParsedSimpleSelector simple;
	bool directParent = false;
};

struct SelectorPlan {
	SmallSelectorList<SelectorPart, 4> parts;
	SmallSelectorList<CssAtomId, 4> ancestorClasses;
	SmallSelectorList<int16_t, 4> ancestorTags;
	CssAtomId rightmostId = kInvalidCssAtom;
	CssAtomId rightmostClass = kInvalidCssAtom;
	int16_t rightmostTag = -1;
	int specificity = 0;
	bool rightmostRoot = false;
	bool valid = true;
};

template <typename T>
void appendUnique(SmallSelectorList<T, 4> &values, T value)
{
	for (std::size_t i = 0, n = values.size(); i < n; ++i)
		if (values.at(i) == value) return;
	values.push_back(value);
}

int simpleSelectorSpecificity(const ParsedSimpleSelector &simple)
{
	int ids = simple.idAtom == kInvalidCssAtom ? 0 : 1;
	int classes = static_cast<int>(simple.classIds.size());
	if (simple.wantsRoot) classes += 1;
	if (simple.wantsFirstChild) classes += 1;
	if (simple.wantsLastChild) classes += 1;
	const int elements = simple.hasTag ? 1 : 0;
	return ids * 10000 + classes * 100 + elements;
}

void finishSelectorPlan(SelectorPlan &plan)
{
	if (plan.parts.empty()) {
		plan.valid = false;
		return;
	}

	const ParsedSimpleSelector &rightmost = plan.parts.back().simple;
	if (rightmost.idAtom != kInvalidCssAtom) {
		plan.rightmostId = rightmost.idAtom;
	} else if (!rightmost.classIds.empty() && rightmost.classIds.front() != kInvalidCssAtom) {
		plan.rightmostClass = rightmost.classIds.front();
	} else if (rightmost.hasTag && !rightmost.rootTag && rightmost.tagId >= 0) {
		plan.rightmostTag = rightmost.tagId;
	} else if (rightmost.wantsRoot || rightmost.rootTag) {
		plan.rightmostRoot = true;
	}

	for (std::size_t p = 0; p + 1 < plan.parts.size(); ++p) {
		const ParsedSimpleSelector &ancestor = plan.parts.at(p).simple;
		for (std::size_t c = 0, n = ancestor.classIds.size(); c < n; ++c) {
			const CssAtomId cls = ancestor.classIds.at(c);
			if (cls != kInvalidCssAtom) appendUnique(plan.ancestorClasses, cls);
		}
		if (ancestor.hasTag && !ancestor.rootTag && ancestor.tagId >= 0)
			appendUnique(plan.ancestorTags, ancestor.tagId);
	}
}

SelectorPlan parseNormalizedSelectorPlan(const char *selectorText, std::size_t selectorLength)
{
	SelectorPlan plan;
	const char *selector = selectorText ? selectorText : "";
	bool nextDirect = false;
	std::size_t i = 0;
	while (i < selectorLength) {
		while (i < selectorLength && static_cast<unsigned char>(selector[i]) <= ' ') ++i;
		if (i >= selectorLength) break;
		if (selector[i] == '>') {
			nextDirect = true;
			++i;
			continue;
		}
		const std::size_t start = i;
		while (i < selectorLength && static_cast<unsigned char>(selector[i]) > ' ' && selector[i] != '>') ++i;
		if (i <= start) continue;
		SelectorPart part;
		part.simple = parseSimpleSelector(selector + start, i - start);
		part.directParent = nextDirect;
		nextDirect = false;
		if (!part.simple.valid) plan.valid = false;
		plan.specificity += simpleSelectorSpecificity(part.simple);
		plan.parts.push_back(std::move(part));
	}
	finishSelectorPlan(plan);
	return plan;
}

SelectorPlan parseSelectorPlan(const std::string &rawSelector)
{
	const std::string selector = normalizeSelectorText(rawSelector);
	return parseNormalizedSelectorPlan(selector.c_str(), selector.size());
}

std::vector<SelectorPlan> &selectorPlans()
{
	static std::vector<SelectorPlan> plans;
	return plans;
}

std::unordered_map<std::string, std::uint16_t> &selectorPlanCache()
{
	static std::unordered_map<std::string, std::uint16_t> cache;
	return cache;
}

std::uint16_t compileSelectorPlan(const CssText &selector)
{
	if (selector.empty()) return kNoSelectorPlan;
	const std::string key(selector.c_str(), selector.length);
	auto &cache = selectorPlanCache();
	const auto cached = cache.find(key);
	if (cached != cache.end()) return cached->second;
	auto &plans = selectorPlans();
	if (plans.size() >= 0xFFFFu) return kNoSelectorPlan;
	plans.push_back(parseNormalizedSelectorPlan(key.c_str(), key.size()));
	const auto handle = static_cast<std::uint16_t>(plans.size() - 1);
	cache.emplace(key, handle);
	return handle;
}

ParsedSimpleSelector staticSimpleSelectorForSpec(const StaticStyleSimpleSelectorSpec &spec)
{
	ParsedSimpleSelector p;
	if (spec.tag && spec.tag[0] != '\0') {
		const std::string tag = toLowerAscii(spec.tag);
		p.hasTag = true;
		if (tag == "body" || tag == "html")
			p.rootTag = true;
		else
			p.tagId = internTag(tag.c_str());
	}
	if (spec.id && spec.id[0] != '\0')
		p.idAtom = internCssAtom(spec.id);
	for (const char *className : spec.classes) {
		if (!className || className[0] == '\0') continue;
		p.classIds.push_back(internCssAtom(className));
	}
	p.wantsRoot = spec.wantsRoot;
	p.wantsFirstChild = spec.wantsFirstChild;
	p.wantsLastChild = spec.wantsLastChild;
	p.hasMatcher = p.wantsRoot || p.wantsFirstChild || p.wantsLastChild ||
	               p.hasTag || p.idAtom != kInvalidCssAtom || !p.classIds.empty();
	p.valid = p.hasMatcher;
	return p;
}

std::uint16_t storeStaticSelectorPlan(const char *selector,
                                      std::initializer_list<StaticStyleSelectorPartSpec> parts)
{
	const char *selectorText = selector ? selector : "";
	const SelectorTextSlice selectorSlice = selectorTextWithoutPseudo(selectorText, std::strlen(selectorText));
	if (!selectorSlice.data || selectorSlice.length == 0) return kNoSelectorPlan;
	const std::string key(selectorSlice.data, selectorSlice.length);
	auto &cache = selectorPlanCache();
	const auto cached = cache.find(key);
	if (cached != cache.end()) return cached->second;
	auto &plans = selectorPlans();
	if (plans.size() >= 0xFFFFu) return kNoSelectorPlan;

	SelectorPlan plan;
	for (const StaticStyleSelectorPartSpec &spec : parts) {
		SelectorPart part;
		part.simple = staticSimpleSelectorForSpec(spec.simple);
		part.directParent = spec.directParent;
		if (!part.simple.valid) plan.valid = false;
		plan.specificity += simpleSelectorSpecificity(part.simple);
		plan.parts.push_back(std::move(part));
	}
	finishSelectorPlan(plan);
	plans.push_back(std::move(plan));
	const auto handle = static_cast<std::uint16_t>(plans.size() - 1);
	cache.emplace(key, handle);
	return handle;
}

const SelectorPlan *selectorPlanForHandle(std::uint16_t handle)
{
	const auto &plans = selectorPlans();
	if (handle == kNoSelectorPlan || static_cast<std::size_t>(handle) >= plans.size()) return nullptr;
	return &plans[static_cast<std::size_t>(handle)];
}

CssAtomId nodeIdAttributeAtom(int node);

bool matchSimpleSelector(int node, const ParsedSimpleSelector &parsed)
{
	const auto &state = treeState();
	if (node < 0 || node >= state.nodeCount) return false;
	if (!parsed.valid) return false;
	const bool wantsRoot = parsed.wantsRoot;
	const bool wantsFirstChild = parsed.wantsFirstChild;
	const bool wantsLastChild = parsed.wantsLastChild;

	if (wantsRoot && !isRootNode(node)) return false;
	if (wantsFirstChild && !isFirstElementChild(node)) return false;
	if (wantsLastChild && !isLastElementChild(node)) return false;
	if (parsed.hasTag) {
		if (parsed.rootTag) {
			if (!isRootNode(node)) return false;
		} else if (state.nodes[node].tag_id != parsed.tagId) {
			return false;
		}
	}
	if (parsed.idAtom != kInvalidCssAtom && nodeIdAttributeAtom(node) != parsed.idAtom) return false;
	for (std::size_t i = 0, n = parsed.classIds.size(); i < n; ++i) {
		const CssAtomId classId = parsed.classIds.at(i);
		if (classId == kInvalidCssAtom || !state.classLists[node].containsAtom(classId)) return false;
	}
	return parsed.hasMatcher;
}

void clearSelectorPartsCache()
{
	selectorPlans().clear();
	selectorPlanCache().clear();
}

bool selectorPlanMatchesNode(const SelectorPlan &plan, int node)
{
	const auto &parts = plan.parts;
	if (!plan.valid) return false;
	if (parts.empty()) return false;
	const auto &state = treeState();
	if (node < 0 || node >= state.nodeCount) return false;

	int current = node;
	const int last = static_cast<int>(parts.size()) - 1;
	if (!matchSimpleSelector(current, parts.at(static_cast<std::size_t>(last)).simple)) return false;

	for (int target = last - 1; target >= 0; --target) {
		const bool direct = parts.at(static_cast<std::size_t>(target + 1)).directParent;
		const int parent = current >= 0 && current < state.nodeCount ? state.nodes[current].parent : -1;
		if (direct) {
			current = parent;
			if (current < 0) return false;
			if (!matchSimpleSelector(current, parts.at(static_cast<std::size_t>(target)).simple)) return false;
			continue;
		}

		bool found = false;
		for (int ancestor = parent; ancestor >= 0 && ancestor < state.nodeCount; ancestor = state.nodes[ancestor].parent) {
			if (matchSimpleSelector(ancestor, parts.at(static_cast<std::size_t>(target)).simple)) {
				current = ancestor;
				found = true;
				break;
			}
		}
		if (!found) return false;
	}
	return true;
}

bool selectorMatchesNode(const std::string &selector, int node)
{
	const SelectorPlan plan = parseSelectorPlan(selector);
	return selectorPlanMatchesNode(plan, node);
}

bool selectorMatchesNode(const CssRule &rule, int node)
{
	const SelectorPlan *plan = selectorPlanForHandle(rule.selectorPlan);
	return plan && selectorPlanMatchesNode(*plan, node);
}

bool ruleMatchesNode(const CssRule &rule, int node)
{
	if (rule.pseudoElement != CssRule::PseudoElement::None) return false;
	const auto &state = treeState();
	if (node < 0 || node >= state.nodeCount) return false;
	switch (rule.selectorType) {
	case CssRule::SelectorType::Class:
		return state.classLists[node].containsAtom(rule.selectorAtom);
	case CssRule::SelectorType::Element:
		return state.nodes[node].tag_id == rule.selectorTagId;
	case CssRule::SelectorType::Selector:
		return selectorMatchesNode(rule, node);
	}
	return false;
}

// --- @media condition evaluation -------------------------------------------
// Conditions are evaluated live against the current viewport. Lengths resolve
// to logical CSS px (physical / DPR) to match CSS author intent; orientation
// and aspect-ratio use the raw dimension ratio (DPR-independent); resolution /
// device-pixel-ratio compare against DPR (in dppx). Empty condition always
// applies. recomputeAllClassStyles() re-runs on resize, so breakpoints reflow.

double mediaLogicalWidth()
{
	const double dpr = g_device_pixel_ratio > 0.0 ? g_device_pixel_ratio : 1.0;
	return g_viewport_width / dpr;
}

double mediaLogicalHeight()
{
	const double dpr = g_device_pixel_ratio > 0.0 ? g_device_pixel_ratio : 1.0;
	return g_viewport_height / dpr;
}

// Parse a media-feature value: ratio "W/H" -> W/H; "Ndpi" -> N/96 dppx;
// "Ndppx"/"Nx" -> N; px / unitless / dpr -> N.
bool parseMediaNumber(const std::string &raw, double &out)
{
	const std::string s = trimCssValue(raw);
	if (s.empty()) return false;
	const std::size_t slash = s.find('/');
	if (slash != std::string::npos) {
		const double w = std::strtod(s.c_str(), nullptr);
		const double h = std::strtod(s.c_str() + slash + 1, nullptr);
		if (h == 0.0) return false;
		out = w / h;
		return true;
	}
	char *end = nullptr;
	const double v = std::strtod(s.c_str(), &end);
	if (end == s.c_str()) return false;
	const std::string unit = trimCssValue(std::string(end));
	if (unit == "dpi") { out = v / 96.0; return true; }
	if (unit == "dppx" || unit == "x") { out = v; return true; }
	out = v;
	return true;
}

enum class CssMediaFeatureKind : std::uint8_t {
	AlwaysFalse,
	Orientation,
	Monochrome,
	Width,
	Height,
	AspectRatio,
	Resolution
};

enum class CssMediaCompare : std::uint8_t {
	Equal,
	Min,
	Max,
	Boolean
};

struct CssMediaTerm {
	CssMediaFeatureKind kind = CssMediaFeatureKind::AlwaysFalse;
	CssMediaCompare compare = CssMediaCompare::Equal;
	double value = 0.0;
	std::uint8_t orientation = 0;  // 1 = portrait, 2 = landscape
};

struct CssMediaQueryPlan {
	bool valid = false;
	std::vector<CssMediaTerm> terms;
};

struct CssMediaConditionPlan {
	std::vector<CssMediaQueryPlan> queries;
};

std::vector<CssMediaConditionPlan> &compiledMediaConditionPlans()
{
	static std::vector<CssMediaConditionPlan> plans;
	return plans;
}

std::unordered_map<std::string, std::uint16_t> &mediaConditionPlanCache()
{
	static std::unordered_map<std::string, std::uint16_t> cache;
	return cache;
}

CssMediaFeatureKind mediaFeatureKindForStatic(StaticStyleMediaFeatureKind kind)
{
	switch (kind) {
	case StaticStyleMediaFeatureKind::Orientation: return CssMediaFeatureKind::Orientation;
	case StaticStyleMediaFeatureKind::Monochrome: return CssMediaFeatureKind::Monochrome;
	case StaticStyleMediaFeatureKind::Width: return CssMediaFeatureKind::Width;
	case StaticStyleMediaFeatureKind::Height: return CssMediaFeatureKind::Height;
	case StaticStyleMediaFeatureKind::AspectRatio: return CssMediaFeatureKind::AspectRatio;
	case StaticStyleMediaFeatureKind::Resolution: return CssMediaFeatureKind::Resolution;
	case StaticStyleMediaFeatureKind::AlwaysFalse: return CssMediaFeatureKind::AlwaysFalse;
	}
	return CssMediaFeatureKind::AlwaysFalse;
}

CssMediaCompare mediaCompareForStatic(StaticStyleMediaCompare compare)
{
	switch (compare) {
	case StaticStyleMediaCompare::Equal: return CssMediaCompare::Equal;
	case StaticStyleMediaCompare::Min: return CssMediaCompare::Min;
	case StaticStyleMediaCompare::Max: return CssMediaCompare::Max;
	case StaticStyleMediaCompare::Boolean: return CssMediaCompare::Boolean;
	}
	return CssMediaCompare::Equal;
}

std::uint16_t storeStaticMediaConditionPlan(const char *condition,
                                            std::initializer_list<StaticStyleMediaQuerySpec> queries)
{
	const char *conditionText = condition ? condition : "";
	if (conditionText[0] == '\0') return kNoMediaConditionPlan;
	const std::string key(conditionText);
	auto &cache = mediaConditionPlanCache();
	const auto cached = cache.find(key);
	if (cached != cache.end()) return cached->second;
	auto &plans = compiledMediaConditionPlans();
	if (plans.size() >= kNoMediaConditionPlan) return kNoMediaConditionPlan;

	CssMediaConditionPlan plan;
	for (const StaticStyleMediaQuerySpec &querySpec : queries) {
		CssMediaQueryPlan query;
		query.valid = querySpec.valid;
		for (const StaticStyleMediaTermSpec &termSpec : querySpec.terms) {
			CssMediaTerm term;
			term.kind = mediaFeatureKindForStatic(termSpec.kind);
			term.compare = mediaCompareForStatic(termSpec.compare);
			term.value = termSpec.value;
			term.orientation = termSpec.orientation;
			query.terms.push_back(term);
		}
		plan.queries.push_back(std::move(query));
	}
	plans.push_back(std::move(plan));
	const auto handle = static_cast<std::uint16_t>(plans.size() - 1);
	cache.emplace(key, handle);
	return handle;
}

void clearMediaConditionPlans()
{
	compiledMediaConditionPlans().clear();
	mediaConditionPlanCache().clear();
}

CssMediaTerm alwaysFalseMediaTerm()
{
	return {};
}

bool mediaFeatureKindForName(const std::string &name, CssMediaFeatureKind &kind, CssMediaCompare &compare)
{
	compare = CssMediaCompare::Equal;
	if (name == "orientation") {
		kind = CssMediaFeatureKind::Orientation;
		return true;
	}
	if (name == "monochrome") {
		kind = CssMediaFeatureKind::Monochrome;
		return true;
	}
	if (name == "min-monochrome") {
		kind = CssMediaFeatureKind::Monochrome;
		compare = CssMediaCompare::Min;
		return true;
	}
	if (name == "max-monochrome") {
		kind = CssMediaFeatureKind::Monochrome;
		compare = CssMediaCompare::Max;
		return true;
	}
	if (name == "width") {
		kind = CssMediaFeatureKind::Width;
		return true;
	}
	if (name == "min-width") {
		kind = CssMediaFeatureKind::Width;
		compare = CssMediaCompare::Min;
		return true;
	}
	if (name == "max-width") {
		kind = CssMediaFeatureKind::Width;
		compare = CssMediaCompare::Max;
		return true;
	}
	if (name == "height") {
		kind = CssMediaFeatureKind::Height;
		return true;
	}
	if (name == "min-height") {
		kind = CssMediaFeatureKind::Height;
		compare = CssMediaCompare::Min;
		return true;
	}
	if (name == "max-height") {
		kind = CssMediaFeatureKind::Height;
		compare = CssMediaCompare::Max;
		return true;
	}
	if (name == "aspect-ratio") {
		kind = CssMediaFeatureKind::AspectRatio;
		return true;
	}
	if (name == "min-aspect-ratio") {
		kind = CssMediaFeatureKind::AspectRatio;
		compare = CssMediaCompare::Min;
		return true;
	}
	if (name == "max-aspect-ratio") {
		kind = CssMediaFeatureKind::AspectRatio;
		compare = CssMediaCompare::Max;
		return true;
	}
	if (name == "resolution" || name == "device-pixel-ratio") {
		kind = CssMediaFeatureKind::Resolution;
		return true;
	}
	if (name == "min-resolution" || name == "min-device-pixel-ratio") {
		kind = CssMediaFeatureKind::Resolution;
		compare = CssMediaCompare::Min;
		return true;
	}
	if (name == "max-resolution" || name == "max-device-pixel-ratio") {
		kind = CssMediaFeatureKind::Resolution;
		compare = CssMediaCompare::Max;
		return true;
	}
	return false;
}

CssMediaTerm compileMediaFeatureTerm(const std::string &name, const std::string &value)
{
	CssMediaFeatureKind kind = CssMediaFeatureKind::AlwaysFalse;
	CssMediaCompare compare = CssMediaCompare::Equal;
	if (!mediaFeatureKindForName(name, kind, compare)) return alwaysFalseMediaTerm();

	CssMediaTerm term;
	term.kind = kind;
	term.compare = compare;
	if (kind == CssMediaFeatureKind::Orientation) {
		const std::string orientation = trimCssValue(value);
		if (orientation == "portrait") term.orientation = 1;
		else if (orientation == "landscape") term.orientation = 2;
		else return alwaysFalseMediaTerm();
		return term;
	}
	double parsed = 0.0;
	if (!parseMediaNumber(value, parsed)) return alwaysFalseMediaTerm();
	term.value = parsed;
	return term;
}

CssMediaQueryPlan compileMediaQueryPlan(const std::string &query)
{
	CssMediaQueryPlan plan;
	const std::string q = trimCssValue(query);
	if (q.empty()) return plan;
	plan.valid = true;
	std::size_t i = 0;
	while (true) {
		const std::size_t open = q.find('(', i);
		if (open == std::string::npos) break;
		const std::size_t close = q.find(')', open);
		if (close == std::string::npos) {
			plan.valid = false;
			plan.terms.clear();
			return plan;
		}
		const std::string feature = q.substr(open + 1, close - open - 1);
		const std::size_t colon = feature.find(':');
		if (colon != std::string::npos) {
			const std::string name = trimCssValue(feature.substr(0, colon));
			const std::string val = trimCssValue(feature.substr(colon + 1));
			plan.terms.push_back(compileMediaFeatureTerm(name, val));
		} else if (trimCssValue(feature) == "monochrome") {
			CssMediaTerm term;
			term.kind = CssMediaFeatureKind::Monochrome;
			term.compare = CssMediaCompare::Boolean;
			plan.terms.push_back(term);
		}
		i = close + 1;
	}
	return plan;
}

std::uint16_t compileMediaConditionPlan(const CssText &condition)
{
	if (condition.empty()) return kNoMediaConditionPlan;
	const std::string raw = condition.str();
	auto &cache = mediaConditionPlanCache();
	const auto cached = cache.find(raw);
	if (cached != cache.end()) return cached->second;
	auto &plans = compiledMediaConditionPlans();
	if (plans.size() >= kNoMediaConditionPlan) return kNoMediaConditionPlan;
	CssMediaConditionPlan plan;
	std::size_t start = 0;
	while (true) {
		const std::size_t comma = raw.find(',', start);
		plan.queries.push_back(compileMediaQueryPlan(raw.substr(start, comma == std::string::npos ? std::string::npos : comma - start)));
		if (comma == std::string::npos) break;
		start = comma + 1;
	}
	plans.push_back(std::move(plan));
	const auto handle = static_cast<std::uint16_t>(plans.size() - 1);
	cache.emplace(raw, handle);
	return handle;
}

// Monochrome bits-per-pixel of the board's panel: 0 on color displays, 1 on
// 1-bit panels (e-paper). Boards set GEA_EMBEDDED_DISPLAY_MONOCHROME=1 in
// their target CMakeLists; this TU compiles per-target so no runtime plumbing
// is needed. Lets stylesheets theme e-ink boards via `@media (monochrome)`.
#ifndef GEA_EMBEDDED_DISPLAY_MONOCHROME
#define GEA_EMBEDDED_DISPLAY_MONOCHROME 0
#endif

bool mediaFeatureMatches(const std::string &name, const std::string &value)
{
	const double w = mediaLogicalWidth();
	const double h = mediaLogicalHeight();
	const double dpr = g_device_pixel_ratio > 0.0 ? g_device_pixel_ratio : 1.0;
	if (name == "orientation") {
		const std::string v = trimCssValue(value);
		if (v == "portrait") return h >= w;
		if (v == "landscape") return w > h;
		return false;
	}
	double n = 0.0;
	if (!parseMediaNumber(value, n)) return false;
	constexpr double mono = GEA_EMBEDDED_DISPLAY_MONOCHROME;
	if (name == "monochrome") return mono == n;
	if (name == "min-monochrome") return mono >= n;
	if (name == "max-monochrome") return mono <= n;
	if (name == "min-width") return w >= n;
	if (name == "max-width") return w <= n;
	if (name == "width") return w == n;
	if (name == "min-height") return h >= n;
	if (name == "max-height") return h <= n;
	if (name == "height") return h == n;
	const double aspect = h != 0.0 ? w / h : 0.0;
	if (name == "min-aspect-ratio") return aspect >= n;
	if (name == "max-aspect-ratio") return aspect <= n;
	if (name == "aspect-ratio") return aspect == n;
	if (name == "min-resolution" || name == "min-device-pixel-ratio") return dpr >= n;
	if (name == "max-resolution" || name == "max-device-pixel-ratio") return dpr <= n;
	if (name == "resolution" || name == "device-pixel-ratio") return dpr == n;
	return false;  // unknown feature -> this query cannot match (CSS semantics)
}

// One query: parenthesized `(feature: value)` terms (AND), with media-type /
// `and` keywords ignored. All present features must match.
bool mediaQueryMatches(const std::string &query)
{
	const std::string q = trimCssValue(query);
	if (q.empty()) return false;
	std::size_t i = 0;
	while (true) {
		const std::size_t open = q.find('(', i);
		if (open == std::string::npos) break;
		const std::size_t close = q.find(')', open);
		if (close == std::string::npos) return false;
		const std::string feature = q.substr(open + 1, close - open - 1);
		const std::size_t colon = feature.find(':');
		if (colon != std::string::npos) {
			const std::string name = trimCssValue(feature.substr(0, colon));
			const std::string val = trimCssValue(feature.substr(colon + 1));
			if (!mediaFeatureMatches(name, val)) return false;
		} else if (trimCssValue(feature) == "monochrome") {
			// Boolean form `(monochrome)`: matches when the panel has any
			// monochrome bits. Other value-less terms stay ignored (the
			// pre-existing behavior) so legacy queries keep matching.
			if (GEA_EMBEDDED_DISPLAY_MONOCHROME <= 0) return false;
		}
		i = close + 1;
	}
	return true;
}

// Full condition: comma-separated queries are OR'd.
bool mediaConditionMatches(const std::string &condition)
{
	if (condition.empty()) return true;
	std::size_t start = 0;
	while (true) {
		const std::size_t comma = condition.find(',', start);
		const std::string query = condition.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
		if (mediaQueryMatches(query)) return true;
		if (comma == std::string::npos) break;
		start = comma + 1;
	}
	return false;
}

bool mediaConditionMatches(const CssText &condition)
{
	if (condition.empty()) return true;
	return mediaConditionMatches(condition.str());
}

bool compareMediaNumber(double actual, CssMediaCompare compare, double expected)
{
	switch (compare) {
	case CssMediaCompare::Equal: return actual == expected;
	case CssMediaCompare::Min: return actual >= expected;
	case CssMediaCompare::Max: return actual <= expected;
	case CssMediaCompare::Boolean: return actual > 0.0;
	}
	return false;
}

bool compiledMediaTermMatches(const CssMediaTerm &term)
{
	const double w = mediaLogicalWidth();
	const double h = mediaLogicalHeight();
	const double dpr = g_device_pixel_ratio > 0.0 ? g_device_pixel_ratio : 1.0;
	constexpr double mono = GEA_EMBEDDED_DISPLAY_MONOCHROME;
	switch (term.kind) {
	case CssMediaFeatureKind::AlwaysFalse:
		return false;
	case CssMediaFeatureKind::Orientation:
		if (term.orientation == 1) return h >= w;
		if (term.orientation == 2) return w > h;
		return false;
	case CssMediaFeatureKind::Monochrome:
		return compareMediaNumber(mono, term.compare, term.value);
	case CssMediaFeatureKind::Width:
		return compareMediaNumber(w, term.compare, term.value);
	case CssMediaFeatureKind::Height:
		return compareMediaNumber(h, term.compare, term.value);
	case CssMediaFeatureKind::AspectRatio:
		return compareMediaNumber(h != 0.0 ? w / h : 0.0, term.compare, term.value);
	case CssMediaFeatureKind::Resolution:
		return compareMediaNumber(dpr, term.compare, term.value);
	}
	return false;
}

bool compiledMediaQueryMatches(const CssMediaQueryPlan &query)
{
	if (!query.valid) return false;
	for (const auto &term : query.terms)
		if (!compiledMediaTermMatches(term)) return false;
	return true;
}

bool compiledMediaConditionMatches(std::uint16_t handle)
{
	const auto &plans = compiledMediaConditionPlans();
	if (handle >= plans.size()) return false;
	const auto &condition = plans[handle];
	for (const auto &query : condition.queries)
		if (compiledMediaQueryMatches(query)) return true;
	return false;
}

bool ruleMediaMatchesUncached(const CssRule &rule)
{
	if (rule.mediaPlan != kNoMediaConditionPlan && rule.mediaPlan < compiledMediaConditionPlans().size())
		return compiledMediaConditionMatches(rule.mediaPlan);
	return mediaConditionMatches(cssRuleTextForHandle(rule.mediaText));
}

int findPseudoChild(int parent, CssRule::PseudoElement pseudo)
{
	const int16_t tag = pseudoTagId(pseudo);
	auto &state = treeState();
	if (parent < 0 || parent >= state.nodeCount) return -1;
	for (int child = state.nodes[parent].first_child; child >= 0; child = state.nodes[child].next_sibling) {
		if (state.nodes[child].tag_id == tag) return child;
	}
	return -1;
}

int firstNonPseudoChild(int parent)
{
	auto &state = treeState();
	if (parent < 0 || parent >= state.nodeCount) return -1;
	for (int child = state.nodes[parent].first_child; child >= 0; child = state.nodes[child].next_sibling) {
		if (!isGeneratedPseudoNode(state.nodes[child])) return child;
	}
	return -1;
}

bool nodeHasGeneratedPseudoChild(int parent)
{
	auto &state = treeState();
	if (parent < 0 || parent >= state.nodeCount) return false;
	for (int child = state.nodes[parent].first_child; child >= 0; child = state.nodes[child].next_sibling) {
		if (isGeneratedPseudoNode(state.nodes[child])) return true;
	}
	return false;
}

int ensurePseudoChild(int parent, CssRule::PseudoElement pseudo)
{
	int existing = findPseudoChild(parent, pseudo);
	if (existing >= 0) return existing;
	Tree &tree = Tree::instance();
	const int child = tree.createView();
	if (child < 0) return -1;
	tree.setTagName(child, pseudo == CssRule::PseudoElement::Before ? "::before" : "::after");
	if (pseudo == CssRule::PseudoElement::Before)
		tree.insertBefore(child, parent, firstNonPseudoChild(parent));
	else
		tree.setParent(child, parent);
	return child;
}

static constexpr std::size_t kRuleCandidateCacheClassCapacity = 6;

struct RuleCandidateSignature {
	CssAtomId classes[kRuleCandidateCacheClassCapacity]{};
	CssAtomId idAtom = kInvalidCssAtom;
	int16_t tagId = -1;
	std::uint8_t classCount = 0;
	bool root = false;
};

class RuleCandidateList {
public:
	std::size_t size() const { return inlineCount_ + spill_.size(); }
	bool empty() const { return size() == 0; }

	int &operator[](std::size_t index)
	{
		return index < inlineCount_ ? inline_[index] : spill_[index - inlineCount_];
	}

	const int &operator[](std::size_t index) const
	{
		return index < inlineCount_ ? inline_[index] : spill_[index - inlineCount_];
	}

	void clear()
	{
		for (std::size_t i = 0; i < inlineCount_; ++i) inline_[i] = 0;
		inlineCount_ = 0;
		spill_.clear();
	}

	void push_back(int rule)
	{
		if (inlineCount_ < kInlineCapacity) {
			inline_[inlineCount_++] = rule;
			return;
		}
		spill_.push_back(rule);
	}

private:
	static constexpr std::size_t kInlineCapacity =
	    static_cast<std::size_t>(GEA_CSS_RULE_CANDIDATE_INLINE_RULES);
	std::array<int, kInlineCapacity> inline_{};
	std::vector<int> spill_;
	std::size_t inlineCount_ = 0;
};

struct RuleCandidateCacheEntry {
	CssAtomId classes[kRuleCandidateCacheClassCapacity]{};
	CssAtomId idAtom = kInvalidCssAtom;
	int16_t tagId = -1;
	std::uint8_t classCount = 0;
	bool root = false;
	bool signatureLocal = false;
	RuleCandidateList rules;
};

void resetRuleCandidateCacheEntry(RuleCandidateCacheEntry &entry)
{
	for (CssAtomId &cls : entry.classes) cls = kInvalidCssAtom;
	entry.idAtom = kInvalidCssAtom;
	entry.tagId = -1;
	entry.classCount = 0;
	entry.root = false;
	entry.signatureLocal = false;
	entry.rules.clear();
}

class RuleCandidateCacheStore {
public:
	std::size_t size() const { return inlineCount_ + spill_.size(); }

	RuleCandidateCacheEntry &operator[](std::size_t index)
	{
		return index < inlineCount_ ? inline_[index] : spill_[index - inlineCount_];
	}

	const RuleCandidateCacheEntry &operator[](std::size_t index) const
	{
		return index < inlineCount_ ? inline_[index] : spill_[index - inlineCount_];
	}

	RuleCandidateCacheEntry &emplace_back()
	{
		if (inlineCount_ < kInlineCapacity) {
			RuleCandidateCacheEntry &entry = inline_[inlineCount_++];
			resetRuleCandidateCacheEntry(entry);
			return entry;
		}
		spill_.emplace_back();
		RuleCandidateCacheEntry &entry = spill_.back();
		resetRuleCandidateCacheEntry(entry);
		return entry;
	}

	RuleCandidateCacheEntry &back()
	{
		return spill_.empty() ? inline_[inlineCount_ - 1] : spill_.back();
	}

	void clear()
	{
		for (std::size_t i = 0; i < inlineCount_; ++i) resetRuleCandidateCacheEntry(inline_[i]);
		inlineCount_ = 0;
		spill_.clear();
	}

private:
	static constexpr std::size_t kInlineCapacity =
	    static_cast<std::size_t>(GEA_CSS_RULE_CANDIDATE_INLINE_CACHE_ENTRIES);
	std::array<RuleCandidateCacheEntry, kInlineCapacity> inline_{};
	std::vector<RuleCandidateCacheEntry> spill_;
	std::size_t inlineCount_ = 0;
};

void sortRuleCandidateSignatureClasses(RuleCandidateSignature &signature)
{
	for (std::uint8_t i = 1; i < signature.classCount; ++i) {
		const CssAtomId cls = signature.classes[i];
		std::uint8_t j = i;
		while (j > 0 && signature.classes[j - 1] > cls) {
			signature.classes[j] = signature.classes[j - 1];
			--j;
		}
		signature.classes[j] = cls;
	}
}

struct PropertyWriteMask {
	std::uint64_t lo = 0;
	std::uint64_t hi = 0;

	bool empty() const { return lo == 0 && hi == 0; }

	void addIndex(int index)
	{
		if (index < 0 || index >= 128) return;
		if (index < 64)
			lo |= (std::uint64_t{1} << index);
		else
			hi |= (std::uint64_t{1} << (index - 64));
	}

	void add(Property property)
	{
		addIndex(static_cast<int>(property));
	}

	void addAll(const PropertyWriteMask &other)
	{
		lo |= other.lo;
		hi |= other.hi;
	}

	bool containsAll(const PropertyWriteMask &other) const
	{
		return (other.lo & ~lo) == 0 && (other.hi & ~hi) == 0;
	}
};

constexpr int kVirtualBackgroundSizeWrite = static_cast<int>(Property::Count);
constexpr int kVirtualGridTemplateColumnsWrite = kVirtualBackgroundSizeWrite + 1;
constexpr int kVirtualGridTemplateRowsWrite = kVirtualBackgroundSizeWrite + 2;
static_assert(kVirtualGridTemplateRowsWrite < 128, "PropertyWriteMask virtual bits overflow");

constexpr std::uint16_t kNoCachedStyleApplyOp = 0xFFFFu;

bool ruleWriteMask(const CssRule &rule, PropertyWriteMask &mask);

// Rule-matching index. A full class-style recompute is O(nodes x rules), and
// with a large stylesheet (the weather app registers ~686 rules) scanning every
// rule for every node dominates mount/recompute time. The index buckets rules by
// their selector key (class name / element tag) so a node only examines the rules
// that could apply to it (its classes + its tag) plus the complex-selector rules
// (which must always be checked via selectorMatchesNode). Candidates are emitted
// in cascade order: specificity first, then source order for ties.
// pseudoElement is NOT filtered here — each caller filters for the pseudo it wants.
struct RuleIndex {
#if GEA_EMBEDDED_UI_STATE_DYNAMIC_INIT
	// Runtime initialization keeps the nonzero sentinels out of .data (see
	// state_init.h); noinline stops the compiler folding them back in.
	__attribute__((noinline)) RuleIndex() { valid = false; }
#endif

	DenseRuleBuckets byClass;
	DenseRuleBuckets byTag;
	// Complex (Selector-type) rules, bucketed by their RIGHTMOST simple selector's
	// primary key: a node can only match the full selector if it matches the
	// rightmost part, so a node only needs to test rules keyed by its own id,
	// classes, tag, or root-ness (plus selAlways for universal rightmosts).
	// selectorMatchesNode
	// still verifies the ancestor chain. This is what makes a near-root class change
	// (theme switch) cheap: a node tests ~its-own rules, not all ~136 selector rules.
	DenseRuleBuckets selById;
	DenseRuleBuckets selByClass;
	DenseRuleBuckets selByTag;
	std::vector<int> selRoot;
	std::vector<int> selAlways;
	std::vector<int> animationRules;  // rules with property == "animation" (any selector type)
	RuleCandidateCacheStore candidateCache;
	// Classes/tags that appear in a NON-rightmost simple selector of any complex rule
	// (i.e. as an ancestor matcher, like `.theme-night` in `.theme-night .icon`).
	// Used by the incremental recompute: a node whose class change touches one of
	// these keys may alter its DESCENDANTS' selector matches, so its subtree must be
	// recomputed in full — a custom-prop/inheritance diff alone wouldn't catch it.
	DenseIdSet ancestorClasses;
	DenseIdSet ancestorTags;
	// CSS specificity per rule (indexed by rule index), packed as
	// ids*10000 + (classes+pseudo-classes)*100 + (elements+pseudo-elements). Used to
	// order the cascade: a more-specific rule wins over a less-specific one
	// regardless of source order (real CSS), instead of pure source-order last-wins.
	std::vector<int> specificity;
	std::vector<PropertyWriteMask> writeMasks;
	std::vector<std::uint16_t> styleOps;
	// Scratch used while building an active rule plan. Reused across nodes so the
	// hot path avoids allocating a set just to remove rules reached through more
	// than one selector key. Serial 0 means "not seen".
	std::vector<std::uint16_t> candidateSeen;
	std::uint16_t candidateSerial = 0;
	// Media-query results are viewport-global. Cache one active bit per rule so a
	// recompute over many nodes does not re-evaluate the same @media expressions.
	std::vector<std::uint8_t> mediaMatches;
	int mediaWidth = -1;
	int mediaHeight = -1;
	int mediaDprMilli = -1;
	bool hasMediaConditions = false;
	bool hasPseudoElementRules = false;
	bool mediaCacheValid = false;
	bool valid = false;
};

RuleIndex g_ruleIndex;

std::size_t &ruleCandidateCacheLastHit()
{
	static std::size_t index = static_cast<std::size_t>(-1);
	return index;
}

void clearActiveRulePlanCache();
void clearCachedStyleApplyOps();
std::uint16_t cachedStyleApplyOpForCompiledValue(const CssCompiledValue *compiled);

// CSS specificity of a single rule's selector. Class -> one class; Element -> one
// element; a complex Selector sums its parts (each compound contributes its
// classes/ids/pseudo-classes/tag). Pseudo-elements (::before/::after) add an
// element-level unit. Inline styles and UA defaults are applied outside the
// cascade loop, so they keep their natural (highest / lowest) priority.
int computeRuleSpecificity(const CssRule &rule)
{
	int ids = 0, classes = 0, elements = 0;
	switch (rule.selectorType) {
	case CssRule::SelectorType::Class:
		classes = 1;
		break;
	case CssRule::SelectorType::Element:
		elements = 1;
		break;
	case CssRule::SelectorType::Selector: {
		const SelectorPlan *plan = selectorPlanForHandle(rule.selectorPlan);
		if (plan) return plan->specificity +
		                  ((rule.pseudoElement == CssRule::PseudoElement::Before ||
		                    rule.pseudoElement == CssRule::PseudoElement::After) ? 1 : 0);
		break;
	}
	}
	if (rule.pseudoElement == CssRule::PseudoElement::Before ||
	    rule.pseudoElement == CssRule::PseudoElement::After)
		elements += 1;
	return ids * 10000 + classes * 100 + elements;
}

void invalidateRuleIndex()
{
	g_ruleIndex.valid = false;
}

void rebuildRuleIndexIfNeeded()
{
	if (g_ruleIndex.valid) return;
	g_ruleIndex.byClass.clear();
	g_ruleIndex.byTag.clear();
	g_ruleIndex.selById.clear();
	g_ruleIndex.selByClass.clear();
	g_ruleIndex.selByTag.clear();
	g_ruleIndex.selRoot.clear();
	g_ruleIndex.selAlways.clear();
	g_ruleIndex.animationRules.clear();
	g_ruleIndex.candidateCache.clear();
	ruleCandidateCacheLastHit() = static_cast<std::size_t>(-1);
	clearActiveRulePlanCache();
	clearCachedStyleApplyOps();
	g_ruleIndex.ancestorClasses.clear();
	g_ruleIndex.ancestorTags.clear();
	const auto &list = rules();
	g_ruleIndex.specificity.assign(list.size(), 0);
	g_ruleIndex.writeMasks.assign(list.size(), {});
	g_ruleIndex.styleOps.assign(list.size(), kNoCachedStyleApplyOp);
	g_ruleIndex.candidateSeen.assign(list.size(), 0);
	g_ruleIndex.candidateSerial = 0;
	g_ruleIndex.mediaMatches.assign(list.size(), 0);
	g_ruleIndex.hasMediaConditions = false;
	g_ruleIndex.hasPseudoElementRules = false;
	g_ruleIndex.mediaCacheValid = false;
	for (int i = 0; i < static_cast<int>(list.size()); ++i) {
		g_ruleIndex.specificity[i] = computeRuleSpecificity(list[i]);
		PropertyWriteMask writeMask;
		if (ruleWriteMask(list[i], writeMask))
			g_ruleIndex.writeMasks[static_cast<std::size_t>(i)] = writeMask;
		g_ruleIndex.styleOps[static_cast<std::size_t>(i)] =
		    cachedStyleApplyOpForCompiledValue(compiledCssValueForHandle(list[i].compiledValue));
		switch (list[i].selectorType) {
		case CssRule::SelectorType::Class:
			if (list[i].selectorAtom != kInvalidCssAtom) g_ruleIndex.byClass.add(list[i].selectorAtom, i);
			break;
		case CssRule::SelectorType::Element:
			if (list[i].selectorTagId >= 0) g_ruleIndex.byTag.add(list[i].selectorTagId, i);
			break;
		case CssRule::SelectorType::Selector: {
			// Key by the rightmost simple selector's first class, else its tag, else
			// "always" (:root / universal / pseudo-only) — the node must match the
			// rightmost for the whole selector to match.
			const SelectorPlan *plan = selectorPlanForHandle(list[i].selectorPlan);
			if (!plan || !plan->valid || plan->parts.empty()) {
				g_ruleIndex.selAlways.push_back(i);
			} else {
				if (plan->rightmostId != kInvalidCssAtom)
					g_ruleIndex.selById.add(plan->rightmostId, i);
				else if (plan->rightmostClass != kInvalidCssAtom)
					g_ruleIndex.selByClass.add(plan->rightmostClass, i);
				else if (plan->rightmostTag >= 0)
					g_ruleIndex.selByTag.add(plan->rightmostTag, i);
				else if (plan->rightmostRoot)
					g_ruleIndex.selRoot.push_back(i);
				else g_ruleIndex.selAlways.push_back(i);
				// Every non-rightmost simple selector is an ancestor matcher: record its
				// classes/tag so the incremental recompute can detect when a node's class
				// change could flip a descendant's match.
				for (std::size_t ancestor = 0, count = plan->ancestorClasses.size(); ancestor < count; ++ancestor) {
					const CssAtomId cls = plan->ancestorClasses.at(ancestor);
					if (cls != kInvalidCssAtom) g_ruleIndex.ancestorClasses.insert(cls);
				}
				for (std::size_t ancestor = 0, count = plan->ancestorTags.size(); ancestor < count; ++ancestor) {
					const int16_t tag = plan->ancestorTags.at(ancestor);
					if (tag >= 0) g_ruleIndex.ancestorTags.insert(tag);
				}
			}
			break;
		}
		}
		if (list[i].mediaPlan != kNoMediaConditionPlan || list[i].mediaText != kNoCssRuleText)
			g_ruleIndex.hasMediaConditions = true;
		if (list[i].pseudoElement == CssRule::PseudoElement::Before ||
		    list[i].pseudoElement == CssRule::PseudoElement::After)
			g_ruleIndex.hasPseudoElementRules = true;
		if (list[i].propertyKind == CssRuleProperty::Animation) g_ruleIndex.animationRules.push_back(i);
	}
	g_ruleIndex.byClass.finalize();
	g_ruleIndex.byTag.finalize();
	g_ruleIndex.selById.finalize();
	g_ruleIndex.selByClass.finalize();
	g_ruleIndex.selByTag.finalize();
	g_ruleIndex.valid = true;
}

int mediaDprMilliKey()
{
	const double dpr = g_device_pixel_ratio > 0.0 ? g_device_pixel_ratio : 1.0;
	return static_cast<int>(std::llround(dpr * 1000.0));
}

void rebuildRuleMediaCacheIfNeeded()
{
	rebuildRuleIndexIfNeeded();
	if (!g_ruleIndex.hasMediaConditions) return;
	const int dpr = mediaDprMilliKey();
	if (g_ruleIndex.mediaCacheValid &&
	    g_ruleIndex.mediaWidth == g_viewport_width &&
	    g_ruleIndex.mediaHeight == g_viewport_height &&
	    g_ruleIndex.mediaDprMilli == dpr)
		return;
	const auto &list = rules();
	if (g_ruleIndex.mediaMatches.size() != list.size())
		g_ruleIndex.mediaMatches.assign(list.size(), 0);
	for (int i = 0; i < static_cast<int>(list.size()); ++i)
		g_ruleIndex.mediaMatches[static_cast<std::size_t>(i)] =
		    ruleMediaMatchesUncached(list[static_cast<std::size_t>(i)]) ? 1u : 0u;
	clearActiveRulePlanCache();
	g_ruleIndex.mediaWidth = g_viewport_width;
	g_ruleIndex.mediaHeight = g_viewport_height;
	g_ruleIndex.mediaDprMilli = dpr;
	g_ruleIndex.mediaCacheValid = true;
}

bool ruleMediaMatchesIndex(int ruleIndex)
{
	rebuildRuleIndexIfNeeded();
	if (!g_ruleIndex.hasMediaConditions) return true;
	rebuildRuleMediaCacheIfNeeded();
	if (ruleIndex < 0 || static_cast<std::size_t>(ruleIndex) >= g_ruleIndex.mediaMatches.size()) return false;
	return g_ruleIndex.mediaMatches[static_cast<std::size_t>(ruleIndex)] != 0;
}

std::uint16_t nextCandidateCollectSerial()
{
	std::uint16_t next = static_cast<std::uint16_t>(g_ruleIndex.candidateSerial + 1u);
	if (next == 0) {
		std::fill(g_ruleIndex.candidateSeen.begin(), g_ruleIndex.candidateSeen.end(), 0);
		next = 1;
	}
	g_ruleIndex.candidateSerial = next;
	return next;
}

CssAtomId nodeIdAttributeAtom(int node)
{
	const NodeRareData *rd = rareDataFor(node);
	return rd ? rd->attributes.idAtom : kInvalidCssAtom;
}

enum ActiveRuleBucket : std::uint8_t {
	kActiveMainCustom,
	kActiveMainRule,
	kActiveBeforeCustom,
	kActiveBeforeRule,
	kActiveAfterCustom,
	kActiveAfterRule,
	kActiveAnimation,
	kActiveRuleBucketCount
};

struct ActiveRulePlanCacheEntry;

struct ActiveRulePlan {
	struct Bucket {
		static constexpr std::size_t kInlineRuleCapacity = 24;
		int inlineRules[kInlineRuleCapacity];
		int *spillRules = nullptr;
		std::size_t spillCount = 0;
		std::size_t spillCapacity = 0;
		std::uint8_t count = 0;
		bool spilled = false;

		~Bucket()
		{
			delete[] spillRules;
		}

		void clear()
		{
			if (spilled)
				spillCount = 0;
			else
				count = 0;
		}

			std::size_t size() const { return spilled ? spillCount : count; }
			bool empty() const { return size() == 0; }
			const int *data() const { return spilled ? spillRules : inlineRules; }

			void push(int rule)
		{
			if (!spilled && count < kInlineRuleCapacity) {
				inlineRules[count++] = rule;
				return;
			}
			if (!spilled) {
				spillCapacity = kInlineRuleCapacity * 2;
				spillRules = new int[spillCapacity];
				for (std::size_t i = 0; i < count; ++i)
					spillRules[i] = inlineRules[i];
				spillCount = count;
				spilled = true;
			}
			if (spillCount >= spillCapacity) {
				const std::size_t nextCapacity = spillCapacity ? spillCapacity * 2 : kInlineRuleCapacity * 2;
				int *next = new int[nextCapacity];
				for (std::size_t i = 0; i < spillCount; ++i)
					next[i] = spillRules[i];
				delete[] spillRules;
				spillRules = next;
				spillCapacity = nextCapacity;
			}
			spillRules[spillCount++] = rule;
		}

		int at(std::size_t index) const
		{
			return spilled ? spillRules[index] : inlineRules[index];
		}

		void set(std::size_t index, int rule)
		{
			if (spilled)
				spillRules[index] = rule;
			else
				inlineRules[index] = rule;
		}
	};

	Bucket buckets[kActiveRuleBucketCount];
	const ActiveRulePlanCacheEntry *cachedEntry = nullptr;

	void clear()
	{
		cachedEntry = nullptr;
		for (auto &bucket : buckets) bucket.clear();
	}

	bool has(int bucket) const;

	void push(int bucket, int rule)
	{
		if (bucket < 0 || bucket >= kActiveRuleBucketCount) return;
		cachedEntry = nullptr;
		buckets[bucket].push(rule);
	}
};

int activeRuleBucketFor(const CssRule &rule);

constexpr std::uint16_t kNoCachedRuleIndex = 0xFFFFu;
constexpr std::uint8_t kCachedStyleApplyOpPropertyCapacity = 4;
constexpr std::uint8_t kCachedTransformValueCount = 10;
constexpr std::uint8_t kActiveRulePlanCacheRuleCapacity = 96;

enum class CachedStyleApplyOpKind : std::uint8_t {
	DirectProperties,
	Noop,
	Transform,
	CompiledTransform,
	Color,
	ColorVar,
	Background,
	StaticBackground,
	BackgroundSize,
	RuntimeLength,
	RuntimeSize,
	RuntimePositionOffset,
	RuntimeBackgroundSize,
	RuntimeFlex,
	RuntimeFlexBasis,
	RuntimeBorderShorthand,
	RuntimeBorderSideShorthand,
	RuntimeFilterBlur,
	RuntimeLineHeight,
	CompiledLength,
	CompiledSize,
	CompiledPositionOffset,
	CompiledBox,
	CompiledBackgroundSize,
	CompiledFlex,
	CompiledFlexBasis,
	CompiledBorderShorthand,
	CompiledBorderSideShorthand,
	CompiledBorderRadius,
	CompiledFilterBlur,
	CompiledBoxShadow,
	CompiledLineHeight,
	GridTemplate
};

struct CachedStyleApplyOp {
	CachedStyleApplyOpKind kind = CachedStyleApplyOpKind::DirectProperties;
	std::uint8_t propertyCount = 0;
	std::uint8_t declaration = 0;
	std::uint8_t properties[kCachedStyleApplyOpPropertyCapacity]{};
	std::int32_t values[kCachedStyleApplyOpPropertyCapacity]{};
	std::int16_t transform[kCachedTransformValueCount]{};
};

class CachedStyleApplyOpStore {
public:
	std::size_t size() const { return inlineCount_ + spill_.size(); }

	CachedStyleApplyOp &operator[](std::size_t index)
	{
		return index < inlineCount_ ? inline_[index] : spill_[index - inlineCount_];
	}

	const CachedStyleApplyOp &operator[](std::size_t index) const
	{
		return index < inlineCount_ ? inline_[index] : spill_[index - inlineCount_];
	}

	void push_back(const CachedStyleApplyOp &op)
	{
		if (inlineCount_ < kInlineCapacity) {
			inline_[inlineCount_++] = op;
			return;
		}
		spill_.push_back(op);
	}

	void clear()
	{
		for (std::size_t i = 0; i < inlineCount_; ++i) inline_[i] = CachedStyleApplyOp{};
		inlineCount_ = 0;
		spill_.clear();
	}

private:
	static constexpr std::size_t kInlineCapacity =
	    static_cast<std::size_t>(GEA_CSS_CACHED_STYLE_APPLY_INLINE_OPS);
	std::array<CachedStyleApplyOp, kInlineCapacity> inline_{};
	std::vector<CachedStyleApplyOp> spill_;
	std::size_t inlineCount_ = 0;
};

struct CachedRuleApply {
	std::uint16_t styleOp = kNoCachedStyleApplyOp;
	std::uint16_t ruleIndex = kNoCachedRuleIndex;
	std::uint16_t compiledValue = kNoCompiledCssValue;
};

struct ActiveRulePlanCacheEntry {
#if GEA_EMBEDDED_UI_STATE_DYNAMIC_INIT
	CssAtomId classes[kRuleCandidateCacheClassCapacity];
	CssAtomId idAtom;
	int16_t tagId;
	std::uint8_t classCount;
	bool root;
	std::uint8_t bucketCounts[kActiveRuleBucketCount];
	std::uint8_t bucketOffsets[kActiveRuleBucketCount];
	CachedRuleApply rules[kActiveRulePlanCacheRuleCapacity];

	// Runtime initialization keeps the nonzero sentinels out of .data (see
	// state_init.h). Defined below resetActiveRulePlanCacheEntry, which it uses.
	ActiveRulePlanCacheEntry();
#else
	CssAtomId classes[kRuleCandidateCacheClassCapacity]{};
	CssAtomId idAtom = kInvalidCssAtom;
	int16_t tagId = -1;
	std::uint8_t classCount = 0;
	bool root = false;
	std::uint8_t bucketCounts[kActiveRuleBucketCount]{};
	std::uint8_t bucketOffsets[kActiveRuleBucketCount]{};
	CachedRuleApply rules[kActiveRulePlanCacheRuleCapacity]{};
#endif
};

void resetActiveRulePlanCacheEntry(ActiveRulePlanCacheEntry &entry)
{
	for (CssAtomId &cls : entry.classes) cls = kInvalidCssAtom;
	entry.idAtom = kInvalidCssAtom;
	entry.tagId = -1;
	entry.classCount = 0;
	entry.root = false;
	for (std::uint8_t &count : entry.bucketCounts) count = 0;
	for (std::uint8_t &offset : entry.bucketOffsets) offset = 0;
	for (CachedRuleApply &rule : entry.rules) {
		rule.styleOp = kNoCachedStyleApplyOp;
		rule.ruleIndex = kNoCachedRuleIndex;
		rule.compiledValue = kNoCompiledCssValue;
	}
}

#if GEA_EMBEDDED_UI_STATE_DYNAMIC_INIT
// noinline stops the compiler folding the reset back into a .data image.
__attribute__((noinline)) ActiveRulePlanCacheEntry::ActiveRulePlanCacheEntry()
{
	resetActiveRulePlanCacheEntry(*this);
}
#endif

class ActiveRulePlanCacheStore {
public:
	std::size_t size() const { return inlineCount_ + spill_.size(); }

	ActiveRulePlanCacheEntry &operator[](std::size_t index)
	{
		return index < inlineCount_ ? inline_[index] : spill_[index - inlineCount_];
	}

	const ActiveRulePlanCacheEntry &operator[](std::size_t index) const
	{
		return index < inlineCount_ ? inline_[index] : spill_[index - inlineCount_];
	}

	ActiveRulePlanCacheEntry &emplace_back()
	{
		if (inlineCount_ < kInlineCapacity) {
			ActiveRulePlanCacheEntry &entry = inline_[inlineCount_++];
			resetActiveRulePlanCacheEntry(entry);
			return entry;
		}
		spill_.emplace_back();
		ActiveRulePlanCacheEntry &entry = spill_.back();
		resetActiveRulePlanCacheEntry(entry);
		return entry;
	}

	ActiveRulePlanCacheEntry &back()
	{
		return spill_.empty() ? inline_[inlineCount_ - 1] : spill_.back();
	}

	void pop_back()
	{
		if (!spill_.empty()) {
			spill_.pop_back();
			return;
		}
		if (inlineCount_ == 0) return;
		resetActiveRulePlanCacheEntry(inline_[--inlineCount_]);
	}

	void clear()
	{
		for (std::size_t i = 0; i < inlineCount_; ++i) resetActiveRulePlanCacheEntry(inline_[i]);
		inlineCount_ = 0;
		spill_.clear();
	}

private:
	static constexpr std::size_t kInlineCapacity =
	    static_cast<std::size_t>(GEA_CSS_ACTIVE_RULE_PLAN_INLINE_CACHE_ENTRIES);
	std::array<ActiveRulePlanCacheEntry, kInlineCapacity> inline_{};
	std::vector<ActiveRulePlanCacheEntry> spill_;
	std::size_t inlineCount_ = 0;
};

ActiveRulePlanCacheStore &activeRulePlanCache()
{
	static ActiveRulePlanCacheStore cache;
	return cache;
}

std::size_t &activeRulePlanCacheLastHit()
{
	static std::size_t index = static_cast<std::size_t>(-1);
	return index;
}

CachedStyleApplyOpStore &cachedStyleApplyOps()
{
	static CachedStyleApplyOpStore ops;
	return ops;
}

void clearCachedStyleApplyOps()
{
	cachedStyleApplyOps().clear();
}

void clearActiveRulePlanCache()
{
	activeRulePlanCache().clear();
	activeRulePlanCacheLastHit() = static_cast<std::size_t>(-1);
}

bool activeRulePlanCacheEntryMatches(const ActiveRulePlanCacheEntry &entry,
                                     const RuleCandidateSignature &signature)
{
	if (entry.idAtom != signature.idAtom ||
	    entry.tagId != signature.tagId ||
	    entry.classCount != signature.classCount ||
	    entry.root != signature.root)
		return false;
	for (std::size_t i = 0; i < signature.classCount; ++i)
		if (entry.classes[i] != signature.classes[i]) return false;
	return true;
}

const ActiveRulePlanCacheEntry *cachedActiveRulePlanForSignature(const RuleCandidateSignature &signature)
{
	const auto &cache = activeRulePlanCache();
	std::size_t &lastHit = activeRulePlanCacheLastHit();
	if (lastHit < cache.size() && activeRulePlanCacheEntryMatches(cache[lastHit], signature))
		return &cache[lastHit];
	for (std::size_t i = 0; i < cache.size(); ++i) {
		if (!activeRulePlanCacheEntryMatches(cache[i], signature)) continue;
		lastHit = i;
		return &cache[i];
	}
	return nullptr;
}

void setActiveRulePlanFromCache(ActiveRulePlan &plan, const ActiveRulePlanCacheEntry &entry)
{
	plan.clear();
	plan.cachedEntry = &entry;
}

std::size_t activeRulePlanBucketSize(const ActiveRulePlan &plan, int bucket)
{
	if (bucket < 0 || bucket >= kActiveRuleBucketCount) return 0;
	if (plan.cachedEntry) return plan.cachedEntry->bucketCounts[bucket];
	return plan.buckets[bucket].size();
}

bool ActiveRulePlan::has(int bucket) const
{
	return activeRulePlanBucketSize(*this, bucket) != 0;
}

DenseRuleBucketSpan activeRulePlanBucketSpan(const ActiveRulePlan &plan, int bucket)
{
	if (bucket < 0 || bucket >= kActiveRuleBucketCount) return {};
	if (plan.cachedEntry) return {};
	const ActiveRulePlan::Bucket &local = plan.buckets[bucket];
	const std::size_t count = local.size();
	return count == 0 ? DenseRuleBucketSpan{} : DenseRuleBucketSpan{local.data(), count};
}

struct CachedRuleApplyBucketSpan {
	const CachedRuleApply *data = nullptr;
	std::size_t count = 0;
};

CachedRuleApplyBucketSpan activeRulePlanCachedRuleSpan(const ActiveRulePlan &plan, int bucket)
{
	if (!plan.cachedEntry || bucket < 0 || bucket >= kActiveRuleBucketCount) return {};
	const std::size_t count = plan.cachedEntry->bucketCounts[bucket];
	return count == 0
	    ? CachedRuleApplyBucketSpan{}
	    : CachedRuleApplyBucketSpan{plan.cachedEntry->rules + plan.cachedEntry->bucketOffsets[bucket], count};
}

bool activeRulePlanFitsCache(const ActiveRulePlan &plan)
{
	std::size_t total = 0;
	for (const auto &bucket : plan.buckets) total += bucket.size();
	return total <= kActiveRulePlanCacheRuleCapacity;
}

void copyActiveRulePlanSignature(ActiveRulePlanCacheEntry &entry,
                                 const RuleCandidateSignature &signature)
{
	entry.idAtom = signature.idAtom;
	entry.tagId = signature.tagId;
	entry.classCount = signature.classCount;
	entry.root = signature.root;
	for (std::size_t i = 0; i < signature.classCount; ++i)
		entry.classes[i] = signature.classes[i];
}

bool addCachedStyleApplyProperty(CachedStyleApplyOp &op, Property property, int value)
{
	const int propertyIndex = static_cast<int>(property);
	if (propertyIndex < 0 || propertyIndex >= static_cast<int>(Property::Count)) return false;
	if (op.propertyCount >= kCachedStyleApplyOpPropertyCapacity) return false;
	op.properties[op.propertyCount] = static_cast<std::uint8_t>(propertyIndex);
	op.values[op.propertyCount] = value;
	op.propertyCount++;
	return true;
}

std::int32_t cachedLengthValueBits(float value)
{
	std::int32_t bits = 0;
	static_assert(sizeof(bits) == sizeof(value));
	std::memcpy(&bits, &value, sizeof(bits));
	return bits;
}

float cachedLengthValueFromBits(std::int32_t bits)
{
	float value = 0.0f;
	static_assert(sizeof(bits) == sizeof(value));
	std::memcpy(&value, &bits, sizeof(value));
	return value;
}

void storeCachedLengthSpec(CachedStyleApplyOp &op, int valueSlot, const CssLengthSpec &length)
{
	if (valueSlot < 0 || valueSlot + 1 >= static_cast<int>(kCachedStyleApplyOpPropertyCapacity)) return;
	op.values[valueSlot] = cachedLengthValueBits(length.value);
	op.values[valueSlot + 1] = static_cast<std::int32_t>(length.unit);
}

CssLengthSpec loadCachedLengthSpec(const CachedStyleApplyOp &op, int valueSlot)
{
	CssLengthSpec length;
	if (valueSlot < 0 || valueSlot + 1 >= static_cast<int>(kCachedStyleApplyOpPropertyCapacity)) return length;
	length.value = cachedLengthValueFromBits(op.values[valueSlot]);
	length.unit = static_cast<CssLengthUnit>(op.values[valueSlot + 1]);
	return length;
}

bool buildRuntimeLengthApplyOp(CachedStyleApplyOp &op,
                               CachedStyleApplyOpKind kind,
                               CssDeclarationId declaration,
                               const CssLengthSpec &length)
{
	op = CachedStyleApplyOp{};
	op.kind = kind;
	op.declaration = static_cast<std::uint8_t>(declaration);
	storeCachedLengthSpec(op, 0, length);
	return true;
}

bool fixedCachedLengthValue(const CssLengthSpec &length, int &value)
{
	switch (length.unit) {
	case CssLengthUnit::Raw:
		value = rawNumber(length.value);
		return true;
	case CssLengthUnit::Px:
		value = cssPixelLength(length.value);
		return true;
	default:
		return false;
	}
}

bool percentCachedLengthValue(const CssLengthSpec &length, int &value)
{
	if (length.unit != CssLengthUnit::Percent) return false;
	value = roundToInt(static_cast<double>(length.value) * 10.0);
	return true;
}

bool addCachedLengthDeclaration(CachedStyleApplyOp &op, CssDeclarationId declaration, const CssLengthSpec &length)
{
	int value = 0;
	if (!fixedCachedLengthValue(length, value)) return false;
	switch (declaration) {
	case CssDeclarationId::Gap: return addCachedStyleApplyProperty(op, Property::Gap, value);
	case CssDeclarationId::MinWidth: return addCachedStyleApplyProperty(op, Property::MinWidth, value);
	case CssDeclarationId::MinHeight: return addCachedStyleApplyProperty(op, Property::MinHeight, value);
	case CssDeclarationId::MaxWidth: return addCachedStyleApplyProperty(op, Property::MaxWidth, value);
	case CssDeclarationId::MaxHeight: return addCachedStyleApplyProperty(op, Property::MaxHeight, value);
	case CssDeclarationId::PaddingTop: return addCachedStyleApplyProperty(op, Property::PaddingTop, value);
	case CssDeclarationId::PaddingRight: return addCachedStyleApplyProperty(op, Property::PaddingRight, value);
	case CssDeclarationId::PaddingBottom: return addCachedStyleApplyProperty(op, Property::PaddingBottom, value);
	case CssDeclarationId::PaddingLeft: return addCachedStyleApplyProperty(op, Property::PaddingLeft, value);
	case CssDeclarationId::MarginTop: return addCachedStyleApplyProperty(op, Property::MarginTop, value);
	case CssDeclarationId::MarginRight: return addCachedStyleApplyProperty(op, Property::MarginRight, value);
	case CssDeclarationId::MarginBottom: return addCachedStyleApplyProperty(op, Property::MarginBottom, value);
	case CssDeclarationId::MarginLeft: return addCachedStyleApplyProperty(op, Property::MarginLeft, value);
	case CssDeclarationId::BorderWidth: return addCachedStyleApplyProperty(op, Property::BorderWidth, value);
	case CssDeclarationId::BorderTopWidth: return addCachedStyleApplyProperty(op, Property::BorderTopWidth, value);
	case CssDeclarationId::BorderRightWidth: return addCachedStyleApplyProperty(op, Property::BorderRightWidth, value);
	case CssDeclarationId::BorderBottomWidth: return addCachedStyleApplyProperty(op, Property::BorderBottomWidth, value);
	case CssDeclarationId::BorderLeftWidth: return addCachedStyleApplyProperty(op, Property::BorderLeftWidth, value);
	case CssDeclarationId::FontSize: return addCachedStyleApplyProperty(op, Property::FontSize, value);
	case CssDeclarationId::Perspective: return addCachedStyleApplyProperty(op, Property::Perspective, value);
	case CssDeclarationId::MaskImage:
		return addCachedStyleApplyProperty(op, Property::MaskRightFadeWidth, std::max(0, value));
	default:
		return false;
	}
}

bool addCachedSizeDeclaration(CachedStyleApplyOp &op,
                              CssDeclarationId declaration,
                              const CssLengthSpec &length)
{
	const bool width = declaration == CssDeclarationId::Width;
	const bool height = declaration == CssDeclarationId::Height;
	if (!width && !height) return false;
	int value = 0;
	if (length.unit == CssLengthUnit::Auto) {
		return addCachedStyleApplyProperty(op, width ? Property::Width : Property::Height, kUnset) &&
		       addCachedStyleApplyProperty(op, width ? Property::WidthPercent : Property::HeightPercent, kUnset);
	}
	if (percentCachedLengthValue(length, value))
		return addCachedStyleApplyProperty(op, width ? Property::WidthPercent : Property::HeightPercent, value);
	if (fixedCachedLengthValue(length, value))
		return addCachedStyleApplyProperty(op, width ? Property::Width : Property::Height, value);
	return false;
}

bool addCachedPositionDeclaration(CachedStyleApplyOp &op,
                                  CssDeclarationId declaration,
                                  const CssLengthSpec &length)
{
	Property lengthProperty = Property::Top;
	Property percentProperty = Property::TopPercent;
	switch (declaration) {
	case CssDeclarationId::Top: lengthProperty = Property::Top; percentProperty = Property::TopPercent; break;
	case CssDeclarationId::Right: lengthProperty = Property::Right; percentProperty = Property::RightPercent; break;
	case CssDeclarationId::Bottom: lengthProperty = Property::Bottom; percentProperty = Property::BottomPercent; break;
	case CssDeclarationId::Left: lengthProperty = Property::Left; percentProperty = Property::LeftPercent; break;
	default: return false;
	}
	int value = 0;
	if (percentCachedLengthValue(length, value)) return addCachedStyleApplyProperty(op, percentProperty, value);
	if (fixedCachedLengthValue(length, value)) return addCachedStyleApplyProperty(op, lengthProperty, value);
	return false;
}

bool addCachedBorderRadiusCorner(CachedStyleApplyOp &op, int corner, const CssLengthSpec &length)
{
	int value = 0;
	if (percentCachedLengthValue(length, value))
		return addCachedStyleApplyProperty(op, borderRadiusPercentProperty(corner), value);
	if (fixedCachedLengthValue(length, value))
		return addCachedStyleApplyProperty(op, borderRadiusLengthProperty(corner), value);
	return false;
}

bool addCachedBorderRadiusDeclaration(CachedStyleApplyOp &op,
                                      CssDeclarationId declaration,
                                      const CssCompiledValue &compiled)
{
	if (declaration == CssDeclarationId::BorderRadius) {
		for (int corner = 0; corner < 4; ++corner)
			if (!addCachedBorderRadiusCorner(op, corner, compiled.lengths[corner])) return false;
		return true;
	}
	if (declaration == CssDeclarationId::BorderTopLeftRadius)
		return addCachedBorderRadiusCorner(op, 0, compiled.lengths[0]);
	if (declaration == CssDeclarationId::BorderTopRightRadius)
		return addCachedBorderRadiusCorner(op, 1, compiled.lengths[0]);
	if (declaration == CssDeclarationId::BorderBottomRightRadius)
		return addCachedBorderRadiusCorner(op, 2, compiled.lengths[0]);
	if (declaration == CssDeclarationId::BorderBottomLeftRadius)
		return addCachedBorderRadiusCorner(op, 3, compiled.lengths[0]);
	return false;
}

bool addCachedBoxDeclaration(CachedStyleApplyOp &op,
                             CssDeclarationId declaration,
                             const CssCompiledValue &compiled)
{
	Property properties[4]{};
	switch (declaration) {
	case CssDeclarationId::Padding:
		properties[0] = Property::PaddingTop;
		properties[1] = Property::PaddingRight;
		properties[2] = Property::PaddingBottom;
		properties[3] = Property::PaddingLeft;
		break;
	case CssDeclarationId::Margin:
		properties[0] = Property::MarginTop;
		properties[1] = Property::MarginRight;
		properties[2] = Property::MarginBottom;
		properties[3] = Property::MarginLeft;
		break;
	case CssDeclarationId::Inset:
		properties[0] = Property::Top;
		properties[1] = Property::Right;
		properties[2] = Property::Bottom;
		properties[3] = Property::Left;
		break;
	default:
		return false;
	}
	int values[4]{};
	for (int i = 0; i < 4; ++i)
		if (!fixedCachedLengthValue(compiled.lengths[i], values[i])) return false;
	for (int i = 0; i < 4; ++i)
		if (!addCachedStyleApplyProperty(op, properties[i], values[i])) return false;
	return true;
}

bool addCachedKeywordDeclaration(CachedStyleApplyOp &op, CssDeclarationId declaration, int value)
{
	switch (declaration) {
	case CssDeclarationId::Display: return addCachedStyleApplyProperty(op, Property::Display, value);
	case CssDeclarationId::ObjectFit: return addCachedStyleApplyProperty(op, Property::ImageFit, value);
	case CssDeclarationId::FlexDirection: return addCachedStyleApplyProperty(op, Property::FlexDirection, value);
	case CssDeclarationId::FlexWrap: return addCachedStyleApplyProperty(op, Property::FlexWrap, value);
	case CssDeclarationId::JustifyContent: return addCachedStyleApplyProperty(op, Property::JustifyContent, value);
	case CssDeclarationId::AlignItems: return addCachedStyleApplyProperty(op, Property::AlignItems, value);
	case CssDeclarationId::JustifyItems: return addCachedStyleApplyProperty(op, Property::JustifyItems, value);
	case CssDeclarationId::AlignContent: return addCachedStyleApplyProperty(op, Property::AlignContent, value);
	case CssDeclarationId::AlignSelf: return addCachedStyleApplyProperty(op, Property::AlignSelf, value);
	case CssDeclarationId::PlaceItems:
		return addCachedStyleApplyProperty(op, Property::AlignItems, value) &&
		       addCachedStyleApplyProperty(op, Property::JustifyItems, value) &&
		       addCachedStyleApplyProperty(op, Property::JustifyContent, value);
	case CssDeclarationId::Position: return addCachedStyleApplyProperty(op, Property::Position, value);
	case CssDeclarationId::TextAlign: return addCachedStyleApplyProperty(op, Property::TextAlign, value);
	case CssDeclarationId::TextDecoration: return addCachedStyleApplyProperty(op, Property::TextDecoration, value);
	case CssDeclarationId::TextTransform: return addCachedStyleApplyProperty(op, Property::TextTransform, value);
	case CssDeclarationId::WhiteSpace: return addCachedStyleApplyProperty(op, Property::WhiteSpace, value);
	case CssDeclarationId::TextOverflow: return addCachedStyleApplyProperty(op, Property::TextOverflow, value);
	case CssDeclarationId::BackfaceVisibility: return addCachedStyleApplyProperty(op, Property::Backface, value);
	case CssDeclarationId::PointerEvents: return addCachedStyleApplyProperty(op, Property::PointerEvents, value);
	case CssDeclarationId::Overflow: return addCachedStyleApplyProperty(op, Property::Overflow, value);
	case CssDeclarationId::OverflowX: return addCachedStyleApplyProperty(op, Property::OverflowX, value);
	case CssDeclarationId::OverflowY: return addCachedStyleApplyProperty(op, Property::OverflowY, value);
	case CssDeclarationId::FontFamily: return addCachedStyleApplyProperty(op, Property::FontId, value);
	case CssDeclarationId::FontWeight: return addCachedStyleApplyProperty(op, Property::FontWeight, value);
	default: return false;
	}
}

std::int16_t cachedInt16Value(int value)
{
	return static_cast<std::int16_t>(std::clamp(value, -32768, 32767));
}

bool cachedTransformTranslateValue(const CssLengthSpec &length,
                                   bool allowPercent,
                                   int &px,
                                   int &percent)
{
	px = 0;
	percent = 0;
	if (allowPercent && percentCachedLengthValue(length, percent)) return true;
	return fixedCachedLengthValue(length, px);
}

bool buildCachedTransformApplyOp(const CssCompiledValue &compiled, CachedStyleApplyOp &op)
{
	if (compiled.kind != CssCompiledKind::Transform) return false;
	TransformComponents transform;
	transform.rotateX = compiled.values[0];
	transform.rotateY = compiled.values[1];
	transform.rotateZ = compiled.values[2];
	transform.scaleX = compiled.values[8];
	transform.scaleY = compiled.values[9];
	if ((compiled.flags & (1u << 3)) != 0 &&
	    !cachedTransformTranslateValue(compiled.lengths[0], true, transform.translateX, transform.translateXPercent))
		return false;
	if ((compiled.flags & (1u << 4)) != 0 &&
	    !cachedTransformTranslateValue(compiled.lengths[1], true, transform.translateY, transform.translateYPercent))
		return false;
	if ((compiled.flags & (1u << 5)) != 0) {
		int unusedPercent = 0;
		if (!cachedTransformTranslateValue(compiled.lengths[2], false, transform.translateZ, unusedPercent)) return false;
	}

	op = CachedStyleApplyOp{};
	op.kind = CachedStyleApplyOpKind::Transform;
	op.transform[0] = cachedInt16Value(transform.rotateX);
	op.transform[1] = cachedInt16Value(transform.rotateY);
	op.transform[2] = cachedInt16Value(transform.rotateZ);
	op.transform[3] = cachedInt16Value(transform.translateX);
	op.transform[4] = cachedInt16Value(transform.translateY);
	op.transform[5] = cachedInt16Value(transform.translateZ);
	op.transform[6] = cachedInt16Value(transform.translateXPercent);
	op.transform[7] = cachedInt16Value(transform.translateYPercent);
	op.transform[8] = cachedInt16Value(transform.scaleX);
	op.transform[9] = cachedInt16Value(transform.scaleY);
	return true;
}

bool buildCachedCompiledTransformApplyOp(const CssCompiledValue &compiled, CachedStyleApplyOp &op)
{
	if (compiled.kind != CssCompiledKind::Transform) return false;
	op = CachedStyleApplyOp{};
	op.kind = CachedStyleApplyOpKind::CompiledTransform;
	return true;
}

bool buildCachedColorApplyOp(const CssCompiledValue &compiled, CachedStyleApplyOp &op)
{
	if (compiled.kind != CssCompiledKind::Color) return false;
	op = CachedStyleApplyOp{};
	op.kind = CachedStyleApplyOpKind::Color;
	op.declaration = static_cast<std::uint8_t>(compiled.declaration);
	op.values[0] = compiled.values[0];
	op.values[1] = compiled.values[1];
	op.values[2] = compiled.values[2];
	return true;
}

bool buildCachedColorVarApplyOp(const CssCompiledValue &compiled, CachedStyleApplyOp &op)
{
	if (compiled.kind != CssCompiledKind::ColorVar) return false;
	op = CachedStyleApplyOp{};
	op.kind = CachedStyleApplyOpKind::ColorVar;
	op.declaration = static_cast<std::uint8_t>(compiled.declaration);
	op.values[0] = compiled.values[0];
	op.values[1] = compiled.values[1];
	op.values[2] = compiled.values[2];
	op.values[3] = compiled.aux == 0 ? -1 : compiled.values[3];
	return true;
}

bool compiledLinearGradientIsStaticLiteral(const CssCompiledLinearGradient &gradient)
{
	return gradient.fromColorAtom == kInvalidCssAtom &&
	       gradient.midColorAtom == kInvalidCssAtom &&
	       gradient.toColorAtom == kInvalidCssAtom &&
	       gradient.fromColorHasFallback == 0 &&
	       gradient.midColorHasFallback == 0 &&
	       gradient.toColorHasFallback == 0;
}

bool compiledRadialGradientIsStaticLiteral(const CssCompiledRadialGradient &gradient)
{
	return gradient.fromColorAtom == kInvalidCssAtom &&
	       gradient.toColorAtom == kInvalidCssAtom &&
	       gradient.fromColorHasFallback == 0 &&
	       gradient.toColorHasFallback == 0;
}

bool cachedStaticGridLineWidth(const CssLengthSpec &length, std::uint8_t &out)
{
	int px = 0;
	if (!fixedCachedLengthValue(length, px)) return false;
	if (px <= 0) {
		out = 0;
		return true;
	}
	out = static_cast<std::uint8_t>(std::max(1, std::min(px, 255)));
	return true;
}

bool buildCachedStaticBackgroundApplyOp(const CssCompiledValue &compiled, CachedStyleApplyOp &op)
{
	if (compiled.kind != CssCompiledKind::Background) return false;
	const std::uint16_t handle = static_cast<std::uint16_t>(compiled.values[0]);
	const CssCompiledBackground *background = compiledCssBackgroundForHandle(handle);
	if (!background || !background->hasGradient) return false;
	if (!compiledLinearGradientIsStaticLiteral(background->gradient)) return false;
	if (background->hasOverlayGradient &&
	    !compiledLinearGradientIsStaticLiteral(background->overlayGradient))
		return false;
	if (background->hasRadialGradient &&
	    !compiledRadialGradientIsStaticLiteral(background->radialGradient))
		return false;

	std::uint8_t gridAxes = background->gridAxes;
	std::uint8_t gridLineX = 0;
	std::uint8_t gridLineY = 0;
	if (background->hasGridLineX) {
		if (!cachedStaticGridLineWidth(background->gridLineX, gridLineX)) return false;
		if (gridLineX == 0) gridAxes &= static_cast<std::uint8_t>(~1u);
	}
	if (background->hasGridLineY) {
		if (!cachedStaticGridLineWidth(background->gridLineY, gridLineY)) return false;
		if (gridLineY == 0) gridAxes &= static_cast<std::uint8_t>(~2u);
	}

	op = CachedStyleApplyOp{};
	op.kind = CachedStyleApplyOpKind::StaticBackground;
	op.values[0] = handle;
	op.values[1] = gridAxes;
	op.values[2] = gridLineX;
	op.values[3] = gridLineY;
	return true;
}

bool buildCachedBackgroundApplyOp(const CssCompiledValue &compiled, CachedStyleApplyOp &op)
{
	if (compiled.kind != CssCompiledKind::Background) return false;
	if (buildCachedStaticBackgroundApplyOp(compiled, op)) return true;
	const std::uint16_t handle = static_cast<std::uint16_t>(compiled.values[0]);
	if (!compiledCssBackgroundForHandle(handle)) return false;
	op = CachedStyleApplyOp{};
	op.kind = CachedStyleApplyOpKind::Background;
	op.values[0] = handle;
	return true;
}

bool buildCachedGridTemplateApplyOp(const CssCompiledValue &compiled, CachedStyleApplyOp &op)
{
	if (compiled.kind != CssCompiledKind::GridTemplate) return false;
	if (compiled.declaration != CssDeclarationId::GridTemplateColumns &&
	    compiled.declaration != CssDeclarationId::GridTemplateRows)
		return false;
	const std::uint16_t handle = static_cast<std::uint16_t>(compiled.values[0]);
	if (!compiledCssGridTemplateForHandle(handle)) return false;
	op = CachedStyleApplyOp{};
	op.kind = CachedStyleApplyOpKind::GridTemplate;
	op.declaration = static_cast<std::uint8_t>(compiled.declaration);
	op.values[0] = handle;
	return true;
}

bool buildCachedBackgroundSizeApplyOp(const CssCompiledValue &compiled, CachedStyleApplyOp &op)
{
	if (compiled.kind != CssCompiledKind::BackgroundSize) return false;
	int stepX = 0;
	int stepY = 0;
	if (!fixedCachedLengthValue(compiled.lengths[0], stepX) ||
	    !fixedCachedLengthValue(compiled.lengths[1], stepY))
		return false;
	op = CachedStyleApplyOp{};
	op.kind = CachedStyleApplyOpKind::BackgroundSize;
	op.values[0] = std::clamp(stepX, 0, 65535);
	op.values[1] = std::clamp(stepY, 0, 65535);
	return true;
}

bool buildRuntimeBackgroundSizeApplyOp(const CssCompiledValue &compiled, CachedStyleApplyOp &op)
{
	if (compiled.kind != CssCompiledKind::BackgroundSize) return false;
	op = CachedStyleApplyOp{};
	op.kind = CachedStyleApplyOpKind::RuntimeBackgroundSize;
	storeCachedLengthSpec(op, 0, compiled.lengths[0]);
	storeCachedLengthSpec(op, 2, compiled.lengths[1]);
	return true;
}

bool buildCachedStyleApplyOp(const CssCompiledValue *compiled, CachedStyleApplyOp &op)
{
	op = CachedStyleApplyOp{};
	if (!compiled) return false;
	switch (compiled->kind) {
	case CssCompiledKind::Noop:
		op.kind = CachedStyleApplyOpKind::Noop;
		return true;
	case CssCompiledKind::DirectProperty: {
		const int propertyIndex = compiled->values[0];
		if (propertyIndex < 0 || propertyIndex >= static_cast<int>(Property::Count)) return false;
		return addCachedStyleApplyProperty(op, static_cast<Property>(propertyIndex), compiled->values[1]);
	}
	case CssCompiledKind::DirectPropertyGroup: {
		int count = compiled->values[0];
		if (count < 0 || count > static_cast<int>(kCachedStyleApplyOpPropertyCapacity)) return false;
		for (int i = 0; i < count; ++i) {
			const int propertyIndex = compiled->values[1 + i * 2];
			if (propertyIndex < 0 || propertyIndex >= static_cast<int>(Property::Count)) return false;
			if (!addCachedStyleApplyProperty(op,
			                                 static_cast<Property>(propertyIndex),
			                                 compiled->values[2 + i * 2])) return false;
		}
		return op.propertyCount != 0;
	}
	case CssCompiledKind::Keyword:
		return addCachedKeywordDeclaration(op, compiled->declaration, compiled->values[0]);
	case CssCompiledKind::Opacity:
		return addCachedStyleApplyProperty(op, Property::Opacity, compiled->values[0]);
	case CssCompiledKind::Number:
		if (compiled->declaration == CssDeclarationId::ZIndex)
			return addCachedStyleApplyProperty(op, Property::ZIndex, compiled->values[0]);
		if (compiled->declaration == CssDeclarationId::FlexGrow)
			return addCachedStyleApplyProperty(op, Property::Flex, compiled->values[0]);
		if (compiled->declaration == CssDeclarationId::FlexShrink)
			return addCachedStyleApplyProperty(op, Property::FlexShrink, compiled->values[0]);
		if (compiled->declaration == CssDeclarationId::FontWeight)
			return addCachedStyleApplyProperty(op, Property::FontWeight, compiled->values[0]);
		return false;
	case CssCompiledKind::Length:
		if (addCachedLengthDeclaration(op, compiled->declaration, compiled->lengths[0])) return true;
		return buildRuntimeLengthApplyOp(op,
		                                 CachedStyleApplyOpKind::RuntimeLength,
		                                 compiled->declaration,
		                                 compiled->lengths[0]);
	case CssCompiledKind::Size:
		if (addCachedSizeDeclaration(op, compiled->declaration, compiled->lengths[0])) return true;
		return buildRuntimeLengthApplyOp(op,
		                                 CachedStyleApplyOpKind::RuntimeSize,
		                                 compiled->declaration,
		                                 compiled->lengths[0]);
	case CssCompiledKind::PositionOffset:
		if (addCachedPositionDeclaration(op, compiled->declaration, compiled->lengths[0])) return true;
		return buildRuntimeLengthApplyOp(op,
		                                 CachedStyleApplyOpKind::RuntimePositionOffset,
		                                 compiled->declaration,
		                                 compiled->lengths[0]);
	case CssCompiledKind::Box:
		if (addCachedBoxDeclaration(op, compiled->declaration, *compiled)) return true;
		op = CachedStyleApplyOp{};
		op.kind = CachedStyleApplyOpKind::CompiledBox;
		return true;
	case CssCompiledKind::Color:
		return buildCachedColorApplyOp(*compiled, op);
	case CssCompiledKind::ColorVar:
		return buildCachedColorVarApplyOp(*compiled, op);
	case CssCompiledKind::Rotate:
		return addCachedStyleApplyProperty(op, Property::TransformRotate, compiled->values[0]);
	case CssCompiledKind::Scale:
		return addCachedStyleApplyProperty(op, Property::TransformScaleX, compiled->values[0]) &&
		       addCachedStyleApplyProperty(op, Property::TransformScaleY, compiled->values[0]);
	case CssCompiledKind::OriginPair:
		if (compiled->declaration == CssDeclarationId::TransformOrigin)
			return addCachedStyleApplyProperty(op, Property::TransformOriginX, compiled->values[0]) &&
			       addCachedStyleApplyProperty(op, Property::TransformOriginY, compiled->values[1]);
		if (compiled->declaration == CssDeclarationId::PerspectiveOrigin)
			return addCachedStyleApplyProperty(op, Property::PerspectiveOriginX, compiled->values[0]) &&
			       addCachedStyleApplyProperty(op, Property::PerspectiveOriginY, compiled->values[1]);
		return false;
	case CssCompiledKind::Transform:
		return buildCachedTransformApplyOp(*compiled, op) ||
		       buildCachedCompiledTransformApplyOp(*compiled, op);
	case CssCompiledKind::Background:
		return buildCachedBackgroundApplyOp(*compiled, op);
	case CssCompiledKind::BackgroundSize:
		if (buildCachedBackgroundSizeApplyOp(*compiled, op)) return true;
		return buildRuntimeBackgroundSizeApplyOp(*compiled, op);
	case CssCompiledKind::GridTemplate:
		return buildCachedGridTemplateApplyOp(*compiled, op);
	case CssCompiledKind::LineHeight:
		if (compiled->aux == 0 && addCachedStyleApplyProperty(op, Property::LineHeight, 0)) return true;
		op = CachedStyleApplyOp{};
		op.kind = CachedStyleApplyOpKind::RuntimeLineHeight;
		op.values[0] = compiled->aux;
		storeCachedLengthSpec(op, 1, compiled->lengths[0]);
		return true;
	case CssCompiledKind::FilterBlur: {
		int value = 0;
		if (compiled->aux != 0 && !fixedCachedLengthValue(compiled->lengths[0], value)) {
			op = CachedStyleApplyOp{};
			op.kind = CachedStyleApplyOpKind::RuntimeFilterBlur;
			op.values[0] = compiled->aux;
			storeCachedLengthSpec(op, 1, compiled->lengths[0]);
			return true;
		}
		value = std::clamp(value, 0, 64);
		return addCachedStyleApplyProperty(op, Property::FilterBlur, value);
	}
	case CssCompiledKind::Flex: {
		if (!addCachedStyleApplyProperty(op, Property::Flex, compiled->values[0])) return false;
		if (!addCachedStyleApplyProperty(op, Property::FlexShrink, compiled->values[1])) return false;
		if (compiled->aux == 0)
			return addCachedStyleApplyProperty(op, Property::FlexBasis, kUnset);
		int basis = 0;
		if (!fixedCachedLengthValue(compiled->lengths[0], basis)) {
			op = CachedStyleApplyOp{};
			op.kind = CachedStyleApplyOpKind::RuntimeFlex;
			op.values[0] = compiled->values[0];
			op.values[1] = compiled->values[1];
			op.values[2] = compiled->aux;
			storeCachedLengthSpec(op, 2, compiled->lengths[0]);
			return true;
		}
		return addCachedStyleApplyProperty(op, Property::FlexBasis, basis);
	}
	case CssCompiledKind::FlexBasis: {
		if (compiled->aux == 0) return addCachedStyleApplyProperty(op, Property::FlexBasis, kUnset);
		int basis = 0;
		if (fixedCachedLengthValue(compiled->lengths[0], basis))
			return addCachedStyleApplyProperty(op, Property::FlexBasis, basis);
		op = CachedStyleApplyOp{};
		op.kind = CachedStyleApplyOpKind::RuntimeFlexBasis;
		storeCachedLengthSpec(op, 0, compiled->lengths[0]);
		return true;
	}
	case CssCompiledKind::BorderShorthand: {
		int width = 0;
		if (!fixedCachedLengthValue(compiled->lengths[0], width) ||
		    (compiled->aux != 0 && compiled->values[1] != 255)) {
			op = CachedStyleApplyOp{};
			op.kind = CachedStyleApplyOpKind::RuntimeBorderShorthand;
			storeCachedLengthSpec(op, 0, compiled->lengths[0]);
			op.values[2] = compiled->aux != 0 ? compiled->values[0] : 0;
			op.values[3] = compiled->aux != 0 ? compiled->values[1] : -1;
			return true;
		}
		if (!addCachedStyleApplyProperty(op, Property::BorderWidth, width)) return false;
		return compiled->aux == 0 || addCachedStyleApplyProperty(op, Property::BorderColor, compiled->values[0]);
	}
	case CssCompiledKind::BorderSideShorthand: {
		const int side = borderSideForDeclaration(compiled->declaration);
		if (side < 0) return false;
		int width = 0;
		if (!fixedCachedLengthValue(compiled->lengths[0], width) ||
		    (compiled->aux != 0 && compiled->values[1] != 255)) {
			op = CachedStyleApplyOp{};
			op.kind = CachedStyleApplyOpKind::RuntimeBorderSideShorthand;
			op.declaration = static_cast<std::uint8_t>(compiled->declaration);
			storeCachedLengthSpec(op, 0, compiled->lengths[0]);
			op.values[2] = compiled->aux != 0 ? compiled->values[0] : 0;
			op.values[3] = compiled->aux != 0 ? compiled->values[1] : -1;
			return true;
		}
		if (!addCachedStyleApplyProperty(op, borderSideWidthProperty(side), width)) return false;
		return compiled->aux == 0 || addCachedStyleApplyProperty(op, borderSideColorProperty(side), compiled->values[0]);
	}
	case CssCompiledKind::BorderRadius:
		if (addCachedBorderRadiusDeclaration(op, compiled->declaration, *compiled)) return true;
		op = CachedStyleApplyOp{};
		op.kind = CachedStyleApplyOpKind::CompiledBorderRadius;
		return true;
	case CssCompiledKind::BoxShadow:
		op = CachedStyleApplyOp{};
		op.kind = CachedStyleApplyOpKind::CompiledBoxShadow;
		return true;
	default:
		return false;
	}
}

bool cachedStyleApplyOpsEqual(const CachedStyleApplyOp &a, const CachedStyleApplyOp &b)
{
	if (a.kind != b.kind) return false;
	if (a.kind == CachedStyleApplyOpKind::Noop)
		return true;
	if (a.kind == CachedStyleApplyOpKind::Transform) {
		for (std::uint8_t i = 0; i < kCachedTransformValueCount; ++i)
			if (a.transform[i] != b.transform[i]) return false;
		return true;
	}
	if (a.kind == CachedStyleApplyOpKind::CompiledTransform)
		return true;
	if (a.kind == CachedStyleApplyOpKind::RuntimeLength ||
	    a.kind == CachedStyleApplyOpKind::RuntimeSize ||
	    a.kind == CachedStyleApplyOpKind::RuntimePositionOffset)
		return a.declaration == b.declaration &&
		       a.values[0] == b.values[0] &&
		       a.values[1] == b.values[1];
	if (a.kind == CachedStyleApplyOpKind::RuntimeBackgroundSize)
		return a.values[0] == b.values[0] &&
		       a.values[1] == b.values[1] &&
		       a.values[2] == b.values[2] &&
		       a.values[3] == b.values[3];
	if (a.kind == CachedStyleApplyOpKind::RuntimeFlex)
		return a.values[0] == b.values[0] &&
		       a.values[1] == b.values[1] &&
		       a.values[2] == b.values[2] &&
		       a.values[3] == b.values[3];
	if (a.kind == CachedStyleApplyOpKind::RuntimeFlexBasis)
		return a.values[0] == b.values[0] && a.values[1] == b.values[1];
	if (a.kind == CachedStyleApplyOpKind::RuntimeBorderShorthand)
		return a.values[0] == b.values[0] &&
		       a.values[1] == b.values[1] &&
		       a.values[2] == b.values[2] &&
		       a.values[3] == b.values[3];
	if (a.kind == CachedStyleApplyOpKind::RuntimeBorderSideShorthand)
		return a.declaration == b.declaration &&
		       a.values[0] == b.values[0] &&
		       a.values[1] == b.values[1] &&
		       a.values[2] == b.values[2] &&
		       a.values[3] == b.values[3];
	if (a.kind == CachedStyleApplyOpKind::RuntimeFilterBlur ||
	    a.kind == CachedStyleApplyOpKind::RuntimeLineHeight)
		return a.values[0] == b.values[0] &&
		       a.values[1] == b.values[1] &&
		       a.values[2] == b.values[2];
	if (a.kind == CachedStyleApplyOpKind::CompiledLength ||
	    a.kind == CachedStyleApplyOpKind::CompiledSize ||
	    a.kind == CachedStyleApplyOpKind::CompiledPositionOffset ||
	    a.kind == CachedStyleApplyOpKind::CompiledBox ||
	    a.kind == CachedStyleApplyOpKind::CompiledBackgroundSize ||
	    a.kind == CachedStyleApplyOpKind::CompiledFlex ||
	    a.kind == CachedStyleApplyOpKind::CompiledFlexBasis ||
	    a.kind == CachedStyleApplyOpKind::CompiledBorderShorthand ||
	    a.kind == CachedStyleApplyOpKind::CompiledBorderSideShorthand ||
	    a.kind == CachedStyleApplyOpKind::CompiledBorderRadius ||
	    a.kind == CachedStyleApplyOpKind::CompiledFilterBlur ||
	    a.kind == CachedStyleApplyOpKind::CompiledBoxShadow ||
	    a.kind == CachedStyleApplyOpKind::CompiledLineHeight)
		return true;
	if (a.kind == CachedStyleApplyOpKind::Color) {
		return a.declaration == b.declaration &&
		       a.values[0] == b.values[0] &&
		       a.values[1] == b.values[1] &&
		       a.values[2] == b.values[2];
	}
	if (a.kind == CachedStyleApplyOpKind::ColorVar) {
		return a.declaration == b.declaration &&
		       a.values[0] == b.values[0] &&
		       a.values[1] == b.values[1] &&
		       a.values[2] == b.values[2] &&
		       a.values[3] == b.values[3];
	}
	if (a.kind == CachedStyleApplyOpKind::Background)
		return a.values[0] == b.values[0];
	if (a.kind == CachedStyleApplyOpKind::StaticBackground)
		return a.values[0] == b.values[0] &&
		       a.values[1] == b.values[1] &&
		       a.values[2] == b.values[2] &&
		       a.values[3] == b.values[3];
	if (a.kind == CachedStyleApplyOpKind::BackgroundSize)
		return a.values[0] == b.values[0] && a.values[1] == b.values[1];
	if (a.kind == CachedStyleApplyOpKind::GridTemplate)
		return a.declaration == b.declaration && a.values[0] == b.values[0];
	if (a.propertyCount != b.propertyCount) return false;
	for (std::uint8_t i = 0; i < a.propertyCount; ++i)
		if (a.properties[i] != b.properties[i] || a.values[i] != b.values[i]) return false;
	return true;
}

std::uint16_t cachedStyleApplyOpForCompiledValue(const CssCompiledValue *compiled)
{
	CachedStyleApplyOp op;
	if (!buildCachedStyleApplyOp(compiled, op)) return kNoCachedStyleApplyOp;
	auto &ops = cachedStyleApplyOps();
	for (std::size_t i = 0; i < ops.size(); ++i)
		if (cachedStyleApplyOpsEqual(ops[i], op)) return static_cast<std::uint16_t>(i);
	if (ops.size() >= kNoCachedStyleApplyOp) return kNoCachedStyleApplyOp;
	ops.push_back(op);
	return static_cast<std::uint16_t>(ops.size() - 1);
}

void storeActiveRulePlanForSignature(const RuleCandidateSignature &signature, const ActiveRulePlan &plan)
{
	static constexpr std::size_t kActiveRulePlanCacheLimit = 64;
	if (!activeRulePlanFitsCache(plan)) return;
	auto &cache = activeRulePlanCache();
	if (cache.size() >= kActiveRulePlanCacheLimit) return;
	cache.emplace_back();
	ActiveRulePlanCacheEntry &entry = cache.back();
	activeRulePlanCacheLastHit() = cache.size() - 1;
	copyActiveRulePlanSignature(entry, signature);
	const auto &ruleList = rules();
	std::size_t write = 0;
	for (int bucket = 0; bucket < kActiveRuleBucketCount; ++bucket) {
		const auto &src = plan.buckets[bucket];
		const std::size_t count = src.size();
		entry.bucketCounts[bucket] = static_cast<std::uint8_t>(count);
		entry.bucketOffsets[bucket] = static_cast<std::uint8_t>(write);
		for (std::size_t i = 0; i < count; ++i) {
			if (write >= kActiveRulePlanCacheRuleCapacity) {
				cache.pop_back();
				return;
			}
			const int ri = src.at(i);
			if (ri < 0 || static_cast<std::size_t>(ri) >= ruleList.size()) {
				entry.rules[write++] = {};
				continue;
			}
			const CssRule &rule = ruleList[static_cast<std::size_t>(ri)];
			CachedRuleApply &cachedRule = entry.rules[write++];
			if (ri >= static_cast<int>(kNoCachedRuleIndex)) {
				cachedRule = {};
				continue;
			}
			cachedRule.ruleIndex = static_cast<std::uint16_t>(ri);
			cachedRule.compiledValue = rule.compiledValue;
			cachedRule.styleOp =
			    static_cast<std::size_t>(ri) < g_ruleIndex.styleOps.size()
			        ? g_ruleIndex.styleOps[static_cast<std::size_t>(ri)]
			        : cachedStyleApplyOpForCompiledValue(compiledCssValueForHandle(rule.compiledValue));
		}
	}
}

bool selectorRuleIsSignatureLocal(const CssRule &rule)
{
	if (rule.selectorType != CssRule::SelectorType::Selector) return true;
	const SelectorPlan *plan = selectorPlanForHandle(rule.selectorPlan);
	if (!plan || !plan->valid || plan->parts.size() != 1) return false;
	const ParsedSimpleSelector &simple = plan->parts.at(0).simple;
	return !simple.wantsFirstChild && !simple.wantsLastChild;
}

bool signatureContainsClass(const RuleCandidateSignature &signature, CssAtomId classId)
{
	if (classId == kInvalidCssAtom) return false;
	for (std::size_t i = 0; i < signature.classCount; ++i)
		if (signature.classes[i] == classId) return true;
	return false;
}

bool simpleSelectorMatchesSignature(const ParsedSimpleSelector &simple,
                                    const RuleCandidateSignature &signature)
{
	if (!simple.valid || simple.wantsFirstChild || simple.wantsLastChild) return false;
	if (simple.wantsRoot && !signature.root) return false;
	if (simple.hasTag) {
		if (simple.rootTag) {
			if (!signature.root) return false;
		} else if (signature.tagId != simple.tagId) {
			return false;
		}
	}
	if (simple.idAtom != kInvalidCssAtom && signature.idAtom != simple.idAtom) return false;
	for (std::size_t i = 0, n = simple.classIds.size(); i < n; ++i)
		if (!signatureContainsClass(signature, simple.classIds.at(i))) return false;
	return simple.hasMatcher;
}

bool selectorRuleMatchesSignature(const CssRule &rule, const RuleCandidateSignature &signature)
{
	if (rule.selectorType != CssRule::SelectorType::Selector) return true;
	const SelectorPlan *plan = selectorPlanForHandle(rule.selectorPlan);
	if (!plan || !plan->valid || plan->parts.size() != 1) return false;
	return simpleSelectorMatchesSignature(plan->parts.at(0).simple, signature);
}

bool candidateRulesAreSignatureLocal(const RuleCandidateList &candidates,
                                     const std::vector<CssRule> &ruleList)
{
	for (std::size_t i = 0, n = candidates.size(); i < n; ++i) {
		const int ri = candidates[i];
		if (ri < 0 || static_cast<std::size_t>(ri) >= ruleList.size()) return false;
		if (!selectorRuleIsSignatureLocal(ruleList[static_cast<std::size_t>(ri)])) return false;
	}
	return true;
}

bool cascadeRuleBefore(int a, int b)
{
	const std::vector<int> &spec = g_ruleIndex.specificity;
	if (a < 0 || b < 0 ||
	    static_cast<std::size_t>(a) >= spec.size() ||
	    static_cast<std::size_t>(b) >= spec.size())
		return a < b;
	return spec[static_cast<std::size_t>(a)] != spec[static_cast<std::size_t>(b)]
	    ? spec[static_cast<std::size_t>(a)] < spec[static_cast<std::size_t>(b)]
	    : a < b;
}

void sortActiveRuleBucketByCascade(ActiveRulePlan::Bucket &bucket)
{
	for (std::size_t i = 1, n = bucket.size(); i < n; ++i) {
		const int rule = bucket.at(i);
		std::size_t j = i;
		while (j > 0 && cascadeRuleBefore(rule, bucket.at(j - 1))) {
			bucket.set(j, bucket.at(j - 1));
			--j;
		}
		bucket.set(j, rule);
	}
}

void sortActiveRulePlanByCascade(ActiveRulePlan &plan)
{
	for (auto &bucket : plan.buckets)
		sortActiveRuleBucketByCascade(bucket);
}

void compactActiveRuleBucket(ActiveRulePlan::Bucket &bucket, const std::uint8_t *skip)
{
	std::size_t write = 0;
	const std::size_t bucketSize = bucket.size();
	for (std::size_t read = 0; read < bucketSize; ++read) {
		if (skip[read]) continue;
		if (write != read) bucket.set(write, bucket.at(read));
		++write;
	}
	if (bucket.spilled)
		bucket.spillCount = write;
	else
		bucket.count = static_cast<std::uint8_t>(write);
}

void addPropertyWrite(PropertyWriteMask &mask, Property property)
{
	switch (property) {
	case Property::Width:
	case Property::WidthPercent:
		mask.add(Property::Width);
		mask.add(Property::WidthPercent);
		return;
	case Property::Height:
	case Property::HeightPercent:
		mask.add(Property::Height);
		mask.add(Property::HeightPercent);
		return;
	case Property::Top:
	case Property::TopPercent:
		mask.add(Property::Top);
		mask.add(Property::TopPercent);
		return;
	case Property::Right:
	case Property::RightPercent:
		mask.add(Property::Right);
		mask.add(Property::RightPercent);
		return;
	case Property::Bottom:
	case Property::BottomPercent:
		mask.add(Property::Bottom);
		mask.add(Property::BottomPercent);
		return;
	case Property::Left:
	case Property::LeftPercent:
		mask.add(Property::Left);
		mask.add(Property::LeftPercent);
		return;
	case Property::BorderRadiusTopLeft:
	case Property::BorderRadiusTopLeftPercent:
		mask.add(Property::BorderRadiusTopLeft);
		mask.add(Property::BorderRadiusTopLeftPercent);
		return;
	case Property::BorderRadiusTopRight:
	case Property::BorderRadiusTopRightPercent:
		mask.add(Property::BorderRadiusTopRight);
		mask.add(Property::BorderRadiusTopRightPercent);
		return;
	case Property::BorderRadiusBottomRight:
	case Property::BorderRadiusBottomRightPercent:
		mask.add(Property::BorderRadiusBottomRight);
		mask.add(Property::BorderRadiusBottomRightPercent);
		return;
	case Property::BorderRadiusBottomLeft:
	case Property::BorderRadiusBottomLeftPercent:
		mask.add(Property::BorderRadiusBottomLeft);
		mask.add(Property::BorderRadiusBottomLeftPercent);
		return;
	case Property::Overflow:
		mask.add(Property::Overflow);
		mask.add(Property::OverflowX);
		mask.add(Property::OverflowY);
		return;
	case Property::OverflowX:
		mask.add(Property::Overflow);
		mask.add(Property::OverflowX);
		return;
	case Property::OverflowY:
		mask.add(Property::Overflow);
		mask.add(Property::OverflowY);
		return;
	default:
		mask.add(property);
		return;
	}
}

void addTransformPropertyWrites(PropertyWriteMask &mask)
{
	mask.add(Property::TransformRotate);
	mask.add(Property::TransformRotateX);
	mask.add(Property::TransformRotateY);
	mask.add(Property::TransformTranslateX);
	mask.add(Property::TransformTranslateY);
	mask.add(Property::TransformTranslateZ);
	mask.add(Property::TransformTranslateXPercent);
	mask.add(Property::TransformTranslateYPercent);
	mask.add(Property::TransformScaleX);
	mask.add(Property::TransformScaleY);
}

void addBoxShadowPropertyWrites(PropertyWriteMask &mask)
{
	mask.add(Property::BoxShadowInset);
	mask.add(Property::BoxShadowOffsetX);
	mask.add(Property::BoxShadowOffsetY);
	mask.add(Property::BoxShadowBlur);
	mask.add(Property::BoxShadowSpread);
	mask.add(Property::BoxShadowColor);
	mask.add(Property::BoxShadowAlpha);
}

bool addColorDeclarationWrites(CssDeclarationId declaration, PropertyWriteMask &mask)
{
	switch (declaration) {
	case CssDeclarationId::Color:
		mask.add(Property::Color);
		return true;
	case CssDeclarationId::ActiveBackgroundColor:
		mask.add(Property::ActiveBackgroundColor);
		mask.add(Property::HasActiveBackground);
		return true;
	case CssDeclarationId::Background:
		mask.add(Property::BackgroundColor);
		mask.add(Property::HasBackground);
		return true;
	case CssDeclarationId::BorderColor:
		mask.add(Property::BorderColor);
		return true;
	case CssDeclarationId::BorderTopColor:
	case CssDeclarationId::BorderRightColor:
	case CssDeclarationId::BorderBottomColor:
	case CssDeclarationId::BorderLeftColor: {
		const int side = declaration == CssDeclarationId::BorderTopColor ? 0 :
		    declaration == CssDeclarationId::BorderRightColor ? 1 :
		    declaration == CssDeclarationId::BorderBottomColor ? 2 : 3;
		mask.add(borderSideColorProperty(side));
		return true;
	}
	default:
		return false;
	}
}

bool addCompiledValueWrites(const CssCompiledValue &compiled, PropertyWriteMask &mask)
{
	switch (compiled.kind) {
	case CssCompiledKind::Noop:
		return true;
	case CssCompiledKind::DirectProperty: {
		const int propertyIndex = compiled.values[0];
		if (propertyIndex < 0 || propertyIndex >= static_cast<int>(Property::Count)) return false;
		addPropertyWrite(mask, static_cast<Property>(propertyIndex));
		return true;
	}
	case CssCompiledKind::DirectPropertyGroup: {
		int count = compiled.values[0];
		if (count < 0) count = 0;
		if (count > 4) count = 4;
		for (int i = 0; i < count; ++i) {
			const int propertyIndex = compiled.values[1 + i * 2];
			if (propertyIndex < 0 || propertyIndex >= static_cast<int>(Property::Count)) return false;
			addPropertyWrite(mask, static_cast<Property>(propertyIndex));
		}
		return true;
	}
	case CssCompiledKind::Keyword:
		switch (compiled.declaration) {
		case CssDeclarationId::Display: addPropertyWrite(mask, Property::Display); return true;
		case CssDeclarationId::ObjectFit: addPropertyWrite(mask, Property::ImageFit); return true;
		case CssDeclarationId::FlexDirection: addPropertyWrite(mask, Property::FlexDirection); return true;
		case CssDeclarationId::FlexWrap: addPropertyWrite(mask, Property::FlexWrap); return true;
		case CssDeclarationId::JustifyContent: addPropertyWrite(mask, Property::JustifyContent); return true;
		case CssDeclarationId::AlignItems: addPropertyWrite(mask, Property::AlignItems); return true;
		case CssDeclarationId::JustifyItems: addPropertyWrite(mask, Property::JustifyItems); return true;
		case CssDeclarationId::AlignContent: addPropertyWrite(mask, Property::AlignContent); return true;
		case CssDeclarationId::AlignSelf: addPropertyWrite(mask, Property::AlignSelf); return true;
		case CssDeclarationId::PlaceItems:
			addPropertyWrite(mask, Property::AlignItems);
			addPropertyWrite(mask, Property::JustifyItems);
			addPropertyWrite(mask, Property::JustifyContent);
			return true;
		case CssDeclarationId::Position: addPropertyWrite(mask, Property::Position); return true;
		case CssDeclarationId::TextAlign: addPropertyWrite(mask, Property::TextAlign); return true;
		case CssDeclarationId::TextDecoration: addPropertyWrite(mask, Property::TextDecoration); return true;
		case CssDeclarationId::TextTransform: addPropertyWrite(mask, Property::TextTransform); return true;
		case CssDeclarationId::WhiteSpace: addPropertyWrite(mask, Property::WhiteSpace); return true;
		case CssDeclarationId::TextOverflow: addPropertyWrite(mask, Property::TextOverflow); return true;
		case CssDeclarationId::BackfaceVisibility: addPropertyWrite(mask, Property::Backface); return true;
		case CssDeclarationId::PointerEvents: addPropertyWrite(mask, Property::PointerEvents); return true;
		case CssDeclarationId::Overflow: addPropertyWrite(mask, Property::Overflow); return true;
		case CssDeclarationId::OverflowX: addPropertyWrite(mask, Property::OverflowX); return true;
		case CssDeclarationId::OverflowY: addPropertyWrite(mask, Property::OverflowY); return true;
		case CssDeclarationId::FontFamily: addPropertyWrite(mask, Property::FontId); return true;
		case CssDeclarationId::FontWeight: addPropertyWrite(mask, Property::FontWeight); return true;
		default: return false;
		}
	case CssCompiledKind::Opacity:
		addPropertyWrite(mask, Property::Opacity);
		return true;
	case CssCompiledKind::Number:
		if (compiled.declaration == CssDeclarationId::ZIndex) {
			addPropertyWrite(mask, Property::ZIndex);
			return true;
		}
		if (compiled.declaration == CssDeclarationId::FlexGrow) {
			addPropertyWrite(mask, Property::Flex);
			return true;
		}
		if (compiled.declaration == CssDeclarationId::FlexShrink) {
			addPropertyWrite(mask, Property::FlexShrink);
			return true;
		}
		if (compiled.declaration == CssDeclarationId::FontWeight) {
			addPropertyWrite(mask, Property::FontWeight);
			return true;
		}
		return false;
	case CssCompiledKind::Flex:
		addPropertyWrite(mask, Property::Flex);
		addPropertyWrite(mask, Property::FlexShrink);
		addPropertyWrite(mask, Property::FlexBasis);
		return true;
	case CssCompiledKind::FlexBasis:
		addPropertyWrite(mask, Property::FlexBasis);
		return true;
	case CssCompiledKind::Length:
		switch (compiled.declaration) {
		case CssDeclarationId::Gap: addPropertyWrite(mask, Property::Gap); return true;
		case CssDeclarationId::MinWidth: addPropertyWrite(mask, Property::MinWidth); return true;
		case CssDeclarationId::MinHeight: addPropertyWrite(mask, Property::MinHeight); return true;
		case CssDeclarationId::MaxWidth: addPropertyWrite(mask, Property::MaxWidth); return true;
		case CssDeclarationId::MaxHeight: addPropertyWrite(mask, Property::MaxHeight); return true;
		case CssDeclarationId::PaddingTop: addPropertyWrite(mask, Property::PaddingTop); return true;
		case CssDeclarationId::PaddingRight: addPropertyWrite(mask, Property::PaddingRight); return true;
		case CssDeclarationId::PaddingBottom: addPropertyWrite(mask, Property::PaddingBottom); return true;
		case CssDeclarationId::PaddingLeft: addPropertyWrite(mask, Property::PaddingLeft); return true;
		case CssDeclarationId::MarginTop: addPropertyWrite(mask, Property::MarginTop); return true;
		case CssDeclarationId::MarginRight: addPropertyWrite(mask, Property::MarginRight); return true;
		case CssDeclarationId::MarginBottom: addPropertyWrite(mask, Property::MarginBottom); return true;
		case CssDeclarationId::MarginLeft: addPropertyWrite(mask, Property::MarginLeft); return true;
		case CssDeclarationId::BorderWidth: addPropertyWrite(mask, Property::BorderWidth); return true;
		case CssDeclarationId::BorderTopWidth: addPropertyWrite(mask, Property::BorderTopWidth); return true;
		case CssDeclarationId::BorderRightWidth: addPropertyWrite(mask, Property::BorderRightWidth); return true;
		case CssDeclarationId::BorderBottomWidth: addPropertyWrite(mask, Property::BorderBottomWidth); return true;
		case CssDeclarationId::BorderLeftWidth: addPropertyWrite(mask, Property::BorderLeftWidth); return true;
		case CssDeclarationId::FontSize: addPropertyWrite(mask, Property::FontSize); return true;
		case CssDeclarationId::Perspective: addPropertyWrite(mask, Property::Perspective); return true;
		case CssDeclarationId::MaskImage: addPropertyWrite(mask, Property::MaskRightFadeWidth); return true;
		default: return false;
		}
	case CssCompiledKind::Size:
		if (compiled.declaration == CssDeclarationId::Width) {
			addPropertyWrite(mask, Property::Width);
			return true;
		}
		if (compiled.declaration == CssDeclarationId::Height) {
			addPropertyWrite(mask, Property::Height);
			return true;
		}
		return false;
	case CssCompiledKind::PositionOffset:
		if (compiled.declaration == CssDeclarationId::Top) {
			addPropertyWrite(mask, Property::Top);
			return true;
		}
		if (compiled.declaration == CssDeclarationId::Right) {
			addPropertyWrite(mask, Property::Right);
			return true;
		}
		if (compiled.declaration == CssDeclarationId::Bottom) {
			addPropertyWrite(mask, Property::Bottom);
			return true;
		}
		if (compiled.declaration == CssDeclarationId::Left) {
			addPropertyWrite(mask, Property::Left);
			return true;
		}
		return false;
	case CssCompiledKind::Box:
		if (compiled.declaration == CssDeclarationId::Padding) {
			mask.add(Property::PaddingTop);
			mask.add(Property::PaddingRight);
			mask.add(Property::PaddingBottom);
			mask.add(Property::PaddingLeft);
			return true;
		}
		if (compiled.declaration == CssDeclarationId::Margin) {
			mask.add(Property::MarginTop);
			mask.add(Property::MarginRight);
			mask.add(Property::MarginBottom);
			mask.add(Property::MarginLeft);
			return true;
		}
		if (compiled.declaration == CssDeclarationId::Inset) {
			mask.add(Property::Top);
			mask.add(Property::Right);
			mask.add(Property::Bottom);
			mask.add(Property::Left);
			return true;
		}
		return false;
	case CssCompiledKind::Color:
		return addColorDeclarationWrites(compiled.declaration, mask);
	case CssCompiledKind::ColorVar:
		return false;
	case CssCompiledKind::Rotate:
		addPropertyWrite(mask, Property::TransformRotate);
		return true;
	case CssCompiledKind::Scale:
		addPropertyWrite(mask, Property::TransformScaleX);
		addPropertyWrite(mask, Property::TransformScaleY);
		return true;
	case CssCompiledKind::OriginPair:
		if (compiled.declaration == CssDeclarationId::TransformOrigin) {
			addPropertyWrite(mask, Property::TransformOriginX);
			addPropertyWrite(mask, Property::TransformOriginY);
			return true;
		}
		if (compiled.declaration == CssDeclarationId::PerspectiveOrigin) {
			addPropertyWrite(mask, Property::PerspectiveOriginX);
			addPropertyWrite(mask, Property::PerspectiveOriginY);
			return true;
		}
		return false;
	case CssCompiledKind::Transform:
		addTransformPropertyWrites(mask);
		return true;
	case CssCompiledKind::Background:
		return false;
	case CssCompiledKind::BackgroundSize:
		mask.addIndex(kVirtualBackgroundSizeWrite);
		return true;
	case CssCompiledKind::BorderShorthand:
		addPropertyWrite(mask, Property::BorderWidth);
		if (compiled.aux != 0) addPropertyWrite(mask, Property::BorderColor);
		return true;
	case CssCompiledKind::BorderSideShorthand: {
		const int side = borderSideForDeclaration(compiled.declaration);
		if (side < 0) return false;
		addPropertyWrite(mask, borderSideWidthProperty(side));
		if (compiled.aux != 0) addPropertyWrite(mask, borderSideColorProperty(side));
		return true;
	}
	case CssCompiledKind::BorderRadius:
		if (compiled.declaration == CssDeclarationId::BorderRadius) {
			for (int corner = 0; corner < 4; ++corner)
				addPropertyWrite(mask, borderRadiusLengthProperty(corner));
			return true;
		}
		if (compiled.declaration == CssDeclarationId::BorderTopLeftRadius) {
			addPropertyWrite(mask, Property::BorderRadiusTopLeft);
			return true;
		}
		if (compiled.declaration == CssDeclarationId::BorderTopRightRadius) {
			addPropertyWrite(mask, Property::BorderRadiusTopRight);
			return true;
		}
		if (compiled.declaration == CssDeclarationId::BorderBottomRightRadius) {
			addPropertyWrite(mask, Property::BorderRadiusBottomRight);
			return true;
		}
		if (compiled.declaration == CssDeclarationId::BorderBottomLeftRadius) {
			addPropertyWrite(mask, Property::BorderRadiusBottomLeft);
			return true;
		}
		return false;
	case CssCompiledKind::FilterBlur:
		addPropertyWrite(mask, Property::FilterBlur);
		return true;
	case CssCompiledKind::BoxShadow:
		addBoxShadowPropertyWrites(mask);
		return true;
	case CssCompiledKind::GridTemplate:
		if (compiled.declaration == CssDeclarationId::GridTemplateColumns) {
			mask.addIndex(kVirtualGridTemplateColumnsWrite);
			return true;
		}
		if (compiled.declaration == CssDeclarationId::GridTemplateRows) {
			mask.addIndex(kVirtualGridTemplateRowsWrite);
			return true;
		}
		return false;
	case CssCompiledKind::LineHeight:
		addPropertyWrite(mask, Property::LineHeight);
		return true;
	case CssCompiledKind::None:
		return false;
	}
	return false;
}

bool ruleWriteMask(const CssRule &rule, PropertyWriteMask &mask)
{
	if (rule.declaration == CssDeclarationId::Custom) return false;
	const CssCompiledValue *compiled = compiledCssValueForHandle(rule.compiledValue);
	if (!compiled) return false;
	return addCompiledValueWrites(*compiled, mask);
}

void collapseShadowedActiveRuleBucket(ActiveRulePlan::Bucket &bucket)
{
	const auto &ruleList = rules();
	const std::size_t bucketSize = bucket.size();
	if (bucketSize <= 1) return;
	std::uint8_t inlineSkip[64]{};
	std::vector<std::uint8_t> spillSkip;
	std::uint8_t *skip = inlineSkip;
	if (bucketSize > sizeof(inlineSkip) / sizeof(inlineSkip[0])) {
		spillSkip.assign(bucketSize, 0);
		skip = spillSkip.data();
	}

	bool anySkip = false;
	PropertyWriteMask laterWrites;
	for (std::size_t reverseIndex = bucketSize; reverseIndex > 0; --reverseIndex) {
		const std::size_t i = reverseIndex - 1;
		const int ri = bucket.at(i);
		if (ri < 0 || static_cast<std::size_t>(ri) >= ruleList.size()) continue;
		if (static_cast<std::size_t>(ri) >= g_ruleIndex.writeMasks.size()) continue;
		const PropertyWriteMask &writes = g_ruleIndex.writeMasks[static_cast<std::size_t>(ri)];
		if (writes.empty()) continue;
		if (laterWrites.containsAll(writes)) {
			skip[i] = 1;
			anySkip = true;
		} else {
			laterWrites.addAll(writes);
		}
	}
	if (anySkip) compactActiveRuleBucket(bucket, skip);
}

void collapseShadowedActiveRules(ActiveRulePlan &plan)
{
	collapseShadowedActiveRuleBucket(plan.buckets[kActiveMainRule]);
	collapseShadowedActiveRuleBucket(plan.buckets[kActiveBeforeRule]);
	collapseShadowedActiveRuleBucket(plan.buckets[kActiveAfterRule]);
}

int activeRuleBucketFor(const CssRule &rule)
{
	if (rule.pseudoElement == CssRule::PseudoElement::Unsupported) return -1;
	const bool custom = isCustomRuleProperty(rule);
	switch (rule.pseudoElement) {
	case CssRule::PseudoElement::None:
		return custom ? kActiveMainCustom : kActiveMainRule;
	case CssRule::PseudoElement::Before:
		return custom ? kActiveBeforeCustom : kActiveBeforeRule;
	case CssRule::PseudoElement::After:
		return custom ? kActiveAfterCustom : kActiveAfterRule;
	case CssRule::PseudoElement::Unsupported:
		return -1;
	}
	return -1;
}

bool captureRuleCandidateSignature(int selectorNode, RuleCandidateSignature &signature)
{
	const auto &state = treeState();
	if (selectorNode < 0 || selectorNode >= state.nodeCount) return false;
	const NodeClassList &classes = state.classLists[selectorNode];
	if (classes.size() > kRuleCandidateCacheClassCapacity) return false;
	signature = RuleCandidateSignature{};
	signature.classCount = static_cast<std::uint8_t>(classes.size());
	for (std::size_t i = 0; i < classes.size(); ++i)
		signature.classes[i] = classes.at(i);
	sortRuleCandidateSignatureClasses(signature);
	signature.idAtom = nodeIdAttributeAtom(selectorNode);
	signature.tagId = state.nodes[selectorNode].tag_id;
	signature.root = isRootNode(selectorNode);
	return true;
}

bool candidateSignatureMatches(const RuleCandidateCacheEntry &entry, const RuleCandidateSignature &signature)
{
	if (entry.idAtom != signature.idAtom ||
	    entry.tagId != signature.tagId ||
	    entry.classCount != signature.classCount ||
	    entry.root != signature.root)
		return false;
	for (std::size_t i = 0; i < signature.classCount; ++i)
		if (entry.classes[i] != signature.classes[i]) return false;
	return true;
}

void copyCandidateSignature(RuleCandidateCacheEntry &entry, const RuleCandidateSignature &signature)
{
	entry.idAtom = signature.idAtom;
	entry.tagId = signature.tagId;
	entry.classCount = signature.classCount;
	entry.root = signature.root;
	for (std::size_t i = 0; i < signature.classCount; ++i)
		entry.classes[i] = signature.classes[i];
}

void addRuleCandidateIndex(RuleCandidateList &out,
                           int ruleIndex,
                           std::uint16_t serial,
                           const std::vector<CssRule> &ruleList)
{
	if (ruleIndex < 0 || static_cast<std::size_t>(ruleIndex) >= g_ruleIndex.candidateSeen.size()) return;
	std::uint16_t &seen = g_ruleIndex.candidateSeen[static_cast<std::size_t>(ruleIndex)];
	if (seen == serial) return;
	seen = serial;
	if (static_cast<std::size_t>(ruleIndex) >= ruleList.size()) return;
	out.push_back(ruleIndex);
}

void addRuleCandidateBucket(RuleCandidateList &out,
                            DenseRuleBucketSpan bucket,
                            std::uint16_t serial,
                            const std::vector<CssRule> &ruleList)
{
	if (bucket.empty()) return;
	for (std::size_t i = 0; i < bucket.count; ++i)
		addRuleCandidateIndex(out, bucket.data[i], serial, ruleList);
}

void addRuleCandidateBucket(RuleCandidateList &out,
                            const std::vector<int> &bucket,
                            std::uint16_t serial,
                            const std::vector<CssRule> &ruleList)
{
	for (const int ri : bucket)
		addRuleCandidateIndex(out, ri, serial, ruleList);
}

void sortRuleCandidateListByCascade(RuleCandidateList &candidates)
{
	for (std::size_t i = 1, n = candidates.size(); i < n; ++i) {
		const int rule = candidates[i];
		std::size_t j = i;
		while (j > 0 && cascadeRuleBefore(rule, candidates[j - 1])) {
			candidates[j] = candidates[j - 1];
			--j;
		}
		candidates[j] = rule;
	}
}

void buildRuleCandidatesForSignature(const RuleCandidateSignature &signature, RuleCandidateList &out)
{
	out.clear();
	const auto &ruleList = rules();
	const std::uint16_t serial = nextCandidateCollectSerial();
	for (std::size_t classIndex = 0; classIndex < signature.classCount; ++classIndex) {
		const CssAtomId cls = signature.classes[classIndex];
		addRuleCandidateBucket(out, g_ruleIndex.byClass.get(cls), serial, ruleList);
		addRuleCandidateBucket(out, g_ruleIndex.selByClass.get(cls), serial, ruleList);
	}
	if (signature.idAtom != kInvalidCssAtom)
		addRuleCandidateBucket(out, g_ruleIndex.selById.get(signature.idAtom), serial, ruleList);
	addRuleCandidateBucket(out, g_ruleIndex.byTag.get(signature.tagId), serial, ruleList);
	addRuleCandidateBucket(out, g_ruleIndex.selByTag.get(signature.tagId), serial, ruleList);
	if (signature.root)
		addRuleCandidateBucket(out, g_ruleIndex.selRoot, serial, ruleList);
	addRuleCandidateBucket(out, g_ruleIndex.selAlways, serial, ruleList);
	sortRuleCandidateListByCascade(out);
}

const RuleCandidateCacheEntry *cachedRuleCandidateEntryForSignature(const RuleCandidateSignature &signature)
{
	static constexpr std::size_t kRuleCandidateCacheLimit = 64;
	std::size_t &lastHit = ruleCandidateCacheLastHit();
	if (lastHit < g_ruleIndex.candidateCache.size() &&
	    candidateSignatureMatches(g_ruleIndex.candidateCache[lastHit], signature))
		return &g_ruleIndex.candidateCache[lastHit];
	for (std::size_t i = 0; i < g_ruleIndex.candidateCache.size(); ++i) {
		if (!candidateSignatureMatches(g_ruleIndex.candidateCache[i], signature)) continue;
		lastHit = i;
		return &g_ruleIndex.candidateCache[i];
	}
	if (g_ruleIndex.candidateCache.size() >= kRuleCandidateCacheLimit) return nullptr;
	g_ruleIndex.candidateCache.emplace_back();
	RuleCandidateCacheEntry &entry = g_ruleIndex.candidateCache.back();
	copyCandidateSignature(entry, signature);
	buildRuleCandidatesForSignature(signature, entry.rules);
	entry.signatureLocal = candidateRulesAreSignatureLocal(entry.rules, rules());
	lastHit = g_ruleIndex.candidateCache.size() - 1;
	return &entry;
}

void addCachedActiveCandidateRule(ActiveRulePlan &plan,
                                  int selectorNode,
                                  int ruleIndex,
                                  const std::vector<CssRule> &ruleList,
                                  const std::vector<std::uint8_t> *mediaMatches,
                                  const RuleCandidateSignature *signature = nullptr)
{
	if (ruleIndex < 0 || static_cast<std::size_t>(ruleIndex) >= ruleList.size()) return;
	if (mediaMatches &&
	    (static_cast<std::size_t>(ruleIndex) >= mediaMatches->size() ||
	     (*mediaMatches)[static_cast<std::size_t>(ruleIndex)] == 0))
		return;
	const CssRule &rule = ruleList[static_cast<std::size_t>(ruleIndex)];
	if (rule.selectorType == CssRule::SelectorType::Selector) {
		if (signature && selectorRuleIsSignatureLocal(rule)) {
			if (!selectorRuleMatchesSignature(rule, *signature)) return;
		} else if (!selectorMatchesNode(rule, selectorNode)) {
			return;
		}
	}
	const int bucket = activeRuleBucketFor(rule);
	if (bucket >= 0) plan.push(bucket, ruleIndex);
	if (rule.propertyKind == CssRuleProperty::Animation &&
	    rule.pseudoElement == CssRule::PseudoElement::None)
		plan.push(kActiveAnimation, ruleIndex);
}

void addActiveCandidateRule(ActiveRulePlan &plan,
                            int selectorNode,
                            int ruleIndex,
                            std::uint16_t serial,
                            const std::vector<CssRule> &ruleList,
                            const std::vector<std::uint8_t> *mediaMatches)
{
	if (ruleIndex < 0 || static_cast<std::size_t>(ruleIndex) >= g_ruleIndex.candidateSeen.size()) return;
	std::uint16_t &seen = g_ruleIndex.candidateSeen[static_cast<std::size_t>(ruleIndex)];
	if (seen == serial) return;
	seen = serial;
	if (static_cast<std::size_t>(ruleIndex) >= ruleList.size()) return;
	if (mediaMatches &&
	    (static_cast<std::size_t>(ruleIndex) >= mediaMatches->size() ||
	     (*mediaMatches)[static_cast<std::size_t>(ruleIndex)] == 0))
		return;
	const CssRule &rule = ruleList[static_cast<std::size_t>(ruleIndex)];
	if (rule.selectorType == CssRule::SelectorType::Selector && !selectorMatchesNode(rule, selectorNode)) return;
	const int bucket = activeRuleBucketFor(rule);
	if (bucket >= 0) plan.push(bucket, ruleIndex);
	if (rule.propertyKind == CssRuleProperty::Animation &&
	    rule.pseudoElement == CssRule::PseudoElement::None)
		plan.push(kActiveAnimation, ruleIndex);
}

void addActiveCandidateRuleBucket(ActiveRulePlan &plan,
                                  int selectorNode,
                                  DenseRuleBucketSpan bucket,
                                  std::uint16_t serial,
                                  const std::vector<CssRule> &ruleList,
                                  const std::vector<std::uint8_t> *mediaMatches)
{
	if (bucket.empty()) return;
	for (std::size_t i = 0; i < bucket.count; ++i)
		addActiveCandidateRule(plan, selectorNode, bucket.data[i], serial, ruleList, mediaMatches);
}

void addActiveCandidateRuleBucket(ActiveRulePlan &plan,
                                  int selectorNode,
                                  const std::vector<int> &bucket,
                                  std::uint16_t serial,
                                  const std::vector<CssRule> &ruleList,
                                  const std::vector<std::uint8_t> *mediaMatches)
{
	if (bucket.empty()) return;
	for (const int ri : bucket)
		addActiveCandidateRule(plan, selectorNode, ri, serial, ruleList, mediaMatches);
}

void buildActiveRulePlanForNode(int selectorNode, ActiveRulePlan &plan)
{
	plan.clear();
	rebuildRuleIndexIfNeeded();
	if (g_ruleIndex.hasMediaConditions) rebuildRuleMediaCacheIfNeeded();
	const auto &state = treeState();
	if (selectorNode < 0 || selectorNode >= state.nodeCount) return;
	const auto &ruleList = rules();
	const std::vector<std::uint8_t> *mediaMatches =
	    g_ruleIndex.hasMediaConditions ? &g_ruleIndex.mediaMatches : nullptr;
	RuleCandidateSignature signature;
	if (captureRuleCandidateSignature(selectorNode, signature)) {
		if (const ActiveRulePlanCacheEntry *cachedPlan = cachedActiveRulePlanForSignature(signature)) {
			setActiveRulePlanFromCache(plan, *cachedPlan);
			return;
		}
		if (const RuleCandidateCacheEntry *candidateEntry = cachedRuleCandidateEntryForSignature(signature)) {
			for (std::size_t i = 0, n = candidateEntry->rules.size(); i < n; ++i) {
				const int ri = candidateEntry->rules[i];
				addCachedActiveCandidateRule(plan, selectorNode, ri, ruleList, mediaMatches, &signature);
			}
			collapseShadowedActiveRules(plan);
			if (candidateEntry->signatureLocal)
				storeActiveRulePlanForSignature(signature, plan);
			return;
		}
	}
	const std::uint16_t serial = nextCandidateCollectSerial();
	const NodeClassList &classes = state.classLists[selectorNode];
	for (std::size_t classIndex = 0, classCount = classes.size(); classIndex < classCount; ++classIndex) {
		const CssAtomId cls = classes.at(classIndex);
		addActiveCandidateRuleBucket(plan, selectorNode, g_ruleIndex.byClass.get(cls), serial, ruleList, mediaMatches);
		addActiveCandidateRuleBucket(plan, selectorNode, g_ruleIndex.selByClass.get(cls), serial, ruleList, mediaMatches);
	}
	const CssAtomId idAtom = nodeIdAttributeAtom(selectorNode);
	if (idAtom != kInvalidCssAtom)
		addActiveCandidateRuleBucket(plan, selectorNode, g_ruleIndex.selById.get(idAtom), serial, ruleList, mediaMatches);
	const int16_t nodeTag = state.nodes[selectorNode].tag_id;
	addActiveCandidateRuleBucket(plan, selectorNode, g_ruleIndex.byTag.get(nodeTag), serial, ruleList, mediaMatches);
	addActiveCandidateRuleBucket(plan, selectorNode, g_ruleIndex.selByTag.get(nodeTag), serial, ruleList, mediaMatches);
	if (isRootNode(selectorNode))
		addActiveCandidateRuleBucket(plan, selectorNode, g_ruleIndex.selRoot, serial, ruleList, mediaMatches);
	addActiveCandidateRuleBucket(plan, selectorNode, g_ruleIndex.selAlways, serial, ruleList, mediaMatches);
	sortActiveRulePlanByCascade(plan);
	collapseShadowedActiveRules(plan);
}

bool applyCachedStyleApplyOpWithSource(NodeHandle node,
                                       std::uint16_t handle,
                                       std::uint16_t compiledValue,
                                       StyleApplicationSource source)
{
	if (!node || handle == kNoCachedStyleApplyOp) return false;
	const auto &ops = cachedStyleApplyOps();
	if (handle >= ops.size()) return false;
	const CachedStyleApplyOp &op = ops[handle];
	switch (op.kind) {
	case CachedStyleApplyOpKind::Noop:
		return true;
	case CachedStyleApplyOpKind::Transform: {
		if (applyTransformSlotsFast(node, op.transform, source)) return true;
		TransformComponents transform;
		transform.rotateX = op.transform[0];
		transform.rotateY = op.transform[1];
		transform.rotateZ = op.transform[2];
		transform.translateX = op.transform[3];
		transform.translateY = op.transform[4];
		transform.translateZ = op.transform[5];
		transform.translateXPercent = op.transform[6];
		transform.translateYPercent = op.transform[7];
		transform.scaleX = op.transform[8];
		transform.scaleY = op.transform[9];
		if (!applyTransformComponentsFast(node, transform, source))
			setTransformComponents(node, transform, source);
		return true;
	}
	case CachedStyleApplyOpKind::CompiledTransform: {
		const CssCompiledValue *compiled = compiledCssValueForHandle(compiledValue);
		if (!compiled || compiled->kind != CssCompiledKind::Transform) return false;
		const TransformComponents transform = transformFromCompiled(*compiled, node.id());
		if (!applyTransformComponentsFast(node, transform, source))
			setTransformComponents(node, transform, source);
		return true;
	}
	case CachedStyleApplyOpKind::Color:
		return applyCompiledColorValue(node,
		                               static_cast<CssDeclarationId>(op.declaration),
		                               op.values[0],
		                               op.values[1],
		                               op.values[2],
		                               source);
	case CachedStyleApplyOpKind::ColorVar: {
		ResolvedCompiledCssColor color;
		const bool hasFallback = op.values[3] >= 0;
		if (!resolveCompiledColorRef(node.id(),
		                             static_cast<CssAtomId>(op.values[0]),
		                             hasFallback ? 1 : 0,
		                             op.values[1],
		                             static_cast<style_color_t>(op.values[2]),
		                             static_cast<std::uint8_t>(hasFallback ? op.values[3] : 0),
		                             color))
			return false;
		return applyCompiledColorValue(node,
		                               static_cast<CssDeclarationId>(op.declaration),
		                               color.styleColor,
		                               color.nativeColor,
		                               color.alpha,
		                               source);
	}
	case CachedStyleApplyOpKind::Background: {
		const CssCompiledBackground *background =
		    compiledCssBackgroundForHandle(static_cast<std::uint16_t>(op.values[0]));
		return background ? applyCompiledBackgroundValue(node, *background, source) : false;
	}
	case CachedStyleApplyOpKind::StaticBackground: {
		const CssCompiledBackground *background =
		    compiledCssBackgroundForHandle(static_cast<std::uint16_t>(op.values[0]));
		return background ? applyCachedStaticBackgroundValue(node,
		                                                     *background,
		                                                     static_cast<std::uint8_t>(op.values[1]),
		                                                     static_cast<std::uint8_t>(op.values[2]),
		                                                     static_cast<std::uint8_t>(op.values[3]),
		                                                     source)
		                  : false;
	}
	case CachedStyleApplyOpKind::BackgroundSize: {
		const int nodeId = node.id();
		auto &state = treeState();
		if (nodeId < 0 || nodeId >= state.nodeCount) return true;
		Node &target = state.nodes[nodeId];
		if (rstyle(target.style).bg_grid_axes == 0) return true;
		RareStyle &rs = rstyleMut(target.style);
		if (op.values[0] > 0) rs.bg_grid_step_x = static_cast<std::uint16_t>(op.values[0]);
		if (op.values[1] > 0) rs.bg_grid_step_y = static_cast<std::uint16_t>(op.values[1]);
		markNodeDisplayCommandsDirtyForStyleApply(nodeId);
		return true;
	}
	case CachedStyleApplyOpKind::GridTemplate: {
		const CssCompiledGridTemplate *grid =
		    compiledCssGridTemplateForHandle(static_cast<std::uint16_t>(op.values[0]));
		if (!grid) return false;
		const auto declaration = static_cast<CssDeclarationId>(op.declaration);
		if (declaration == CssDeclarationId::GridTemplateColumns) {
			applyCompiledGridTemplateValue(node, *grid, true);
			return true;
		}
		if (declaration == CssDeclarationId::GridTemplateRows) {
			applyCompiledGridTemplateValue(node, *grid, false);
			return true;
		}
		return false;
	}
	case CachedStyleApplyOpKind::RuntimeLength:
		return applyRuntimeLengthValue(node,
		                               static_cast<CssDeclarationId>(op.declaration),
		                               loadCachedLengthSpec(op, 0),
		                               source);
	case CachedStyleApplyOpKind::RuntimeSize:
		return applyRuntimeSizeValue(node,
		                             static_cast<CssDeclarationId>(op.declaration),
		                             loadCachedLengthSpec(op, 0),
		                             source);
	case CachedStyleApplyOpKind::RuntimePositionOffset:
		return applyRuntimePositionOffsetValue(node,
		                                       static_cast<CssDeclarationId>(op.declaration),
		                                       loadCachedLengthSpec(op, 0),
		                                       source);
	case CachedStyleApplyOpKind::RuntimeBackgroundSize:
		return applyRuntimeBackgroundSizeValue(node,
		                                       loadCachedLengthSpec(op, 0),
		                                       loadCachedLengthSpec(op, 2));
	case CachedStyleApplyOpKind::RuntimeFlex:
		return applyRuntimeFlexValue(node,
		                             op.values[0],
		                             op.values[1],
		                             static_cast<std::uint8_t>(op.values[2]),
		                             loadCachedLengthSpec(op, 2),
		                             source);
	case CachedStyleApplyOpKind::RuntimeFlexBasis:
		return applyRuntimeFlexBasisValue(node, 1, loadCachedLengthSpec(op, 0), source);
	case CachedStyleApplyOpKind::RuntimeBorderShorthand:
		return applyRuntimeBorderShorthandValue(node,
		                                        loadCachedLengthSpec(op, 0),
		                                        op.values[2],
		                                        op.values[3],
		                                        source);
	case CachedStyleApplyOpKind::RuntimeBorderSideShorthand:
		return applyRuntimeBorderSideShorthandValue(node,
		                                            static_cast<CssDeclarationId>(op.declaration),
		                                            loadCachedLengthSpec(op, 0),
		                                            op.values[2],
		                                            op.values[3],
		                                            source);
	case CachedStyleApplyOpKind::RuntimeFilterBlur:
		return applyRuntimeFilterBlurValue(node,
		                                   static_cast<std::uint8_t>(op.values[0]),
		                                   loadCachedLengthSpec(op, 1),
		                                   source);
	case CachedStyleApplyOpKind::RuntimeLineHeight:
		return applyRuntimeLineHeightValue(node,
		                                   static_cast<std::uint8_t>(op.values[0]),
		                                   loadCachedLengthSpec(op, 1),
		                                   source);
	case CachedStyleApplyOpKind::CompiledLength:
		if (const CssCompiledValue *compiled = compiledCssValueForHandle(compiledValue))
			return applyCompiledLengthValue(node, *compiled, source);
		return false;
	case CachedStyleApplyOpKind::CompiledSize:
		if (const CssCompiledValue *compiled = compiledCssValueForHandle(compiledValue))
			return applyCompiledSizeValue(node, *compiled, source);
		return false;
	case CachedStyleApplyOpKind::CompiledPositionOffset:
		if (const CssCompiledValue *compiled = compiledCssValueForHandle(compiledValue))
			return applyCompiledPositionOffsetValue(node, *compiled, source);
		return false;
	case CachedStyleApplyOpKind::CompiledBox:
		if (const CssCompiledValue *compiled = compiledCssValueForHandle(compiledValue))
			return applyCompiledBoxValue(node, *compiled, source);
		return false;
	case CachedStyleApplyOpKind::CompiledBackgroundSize:
		if (const CssCompiledValue *compiled = compiledCssValueForHandle(compiledValue))
			return applyCompiledBackgroundSizeValue(node, *compiled);
		return false;
	case CachedStyleApplyOpKind::CompiledFlex:
		if (const CssCompiledValue *compiled = compiledCssValueForHandle(compiledValue))
			return applyCompiledFlexValue(node, *compiled, source);
		return false;
	case CachedStyleApplyOpKind::CompiledFlexBasis:
		if (const CssCompiledValue *compiled = compiledCssValueForHandle(compiledValue))
			return applyCompiledFlexBasisValue(node, *compiled, source);
		return false;
	case CachedStyleApplyOpKind::CompiledBorderShorthand:
		if (const CssCompiledValue *compiled = compiledCssValueForHandle(compiledValue))
			return applyCompiledBorderShorthandValue(node, *compiled, source);
		return false;
	case CachedStyleApplyOpKind::CompiledBorderSideShorthand:
		if (const CssCompiledValue *compiled = compiledCssValueForHandle(compiledValue))
			return applyCompiledBorderSideShorthandValue(node, *compiled, source);
		return false;
	case CachedStyleApplyOpKind::CompiledBorderRadius:
		if (const CssCompiledValue *compiled = compiledCssValueForHandle(compiledValue))
			return applyCompiledBorderRadiusValue(node, *compiled, source);
		return false;
	case CachedStyleApplyOpKind::CompiledFilterBlur:
		if (const CssCompiledValue *compiled = compiledCssValueForHandle(compiledValue))
			return applyCompiledFilterBlurValue(node, *compiled, source);
		return false;
	case CachedStyleApplyOpKind::CompiledBoxShadow:
		if (const CssCompiledValue *compiled = compiledCssValueForHandle(compiledValue))
			return applyCompiledBoxShadowValue(node, *compiled, source);
		return false;
	case CachedStyleApplyOpKind::CompiledLineHeight:
		if (const CssCompiledValue *compiled = compiledCssValueForHandle(compiledValue))
			return applyCompiledLineHeightValue(node, *compiled, source);
		return false;
	case CachedStyleApplyOpKind::DirectProperties:
		if (source == StyleApplicationSource::ClassRule) {
			auto &state = treeState();
			if (state.styleInvalidationSuppressionDepth > 0) {
				const int nodeId = node.id();
				if (nodeId < 0 || nodeId >= state.nodeCount) return true;
				Node &target = state.nodes[nodeId];
				for (std::uint8_t i = 0; i < op.propertyCount; ++i) {
					const int propertyIndex = op.properties[i];
					if (propertyIndex < 0 || propertyIndex >= static_cast<int>(Property::Count)) return false;
					if (!setClassRuleValueFastUnchecked(target, static_cast<Property>(propertyIndex), op.values[i])) return false;
				}
				return true;
			}
		}
		for (std::uint8_t i = 0; i < op.propertyCount; ++i) {
			const int propertyIndex = op.properties[i];
			if (propertyIndex < 0 || propertyIndex >= static_cast<int>(Property::Count)) return false;
			setStyleValue(node, static_cast<Property>(propertyIndex), op.values[i], source);
		}
		return true;
	}
	return false;
}

void applyCachedRuleWithSource(NodeHandle node, const CachedRuleApply &op, StyleApplicationSource source)
{
	if (applyCachedStyleApplyOpWithSource(node, op.styleOp, op.compiledValue, source)) return;
	if (!node || op.ruleIndex == kNoCachedRuleIndex) return;
	const auto &ruleList = rules();
	if (op.ruleIndex >= ruleList.size()) return;
	const CssRule &rule = ruleList[op.ruleIndex];
	const CssCompiledValue *compiled = compiledCssValueForHandle(op.compiledValue);
	applyRulePropertyWithCompiledValue(node, rule, compiled, source);
}

void applyActiveRuleSpanToNode(int node, const ActiveRulePlan &plan, int bucketId)
{
	const NodeHandle nodeHandle(node);
	if (!nodeHandle) return;
	if (plan.cachedEntry) {
		const CachedRuleApplyBucketSpan bucket = activeRulePlanCachedRuleSpan(plan, bucketId);
		for (std::size_t i = 0; i < bucket.count; ++i) {
			applyCachedRuleWithSource(nodeHandle, bucket.data[i], StyleApplicationSource::ClassRule);
		}
		return;
	}

	const auto &ruleList = rules();
	const DenseRuleBucketSpan bucket = activeRulePlanBucketSpan(plan, bucketId);
	for (std::size_t i = 0; i < bucket.count; ++i) {
		const int ri = bucket.data[i];
		if (ri < 0 || static_cast<std::size_t>(ri) >= ruleList.size()) continue;
		const CssRule &rule = ruleList[static_cast<std::size_t>(ri)];
		const std::uint16_t styleOp =
		    static_cast<std::size_t>(ri) < g_ruleIndex.styleOps.size()
		        ? g_ruleIndex.styleOps[static_cast<std::size_t>(ri)]
		        : cachedStyleApplyOpForCompiledValue(compiledCssValueForHandle(rule.compiledValue));
		if (applyCachedStyleApplyOpWithSource(nodeHandle, styleOp, rule.compiledValue, StyleApplicationSource::ClassRule)) continue;
		const CssCompiledValue *compiled = compiledCssValueForHandle(rule.compiledValue);
		applyRulePropertyWithCompiledValue(nodeHandle, rule, compiled, StyleApplicationSource::ClassRule);
	}
}

void applyActiveRuleSpansToNode(int node, const ActiveRulePlan &plan, int customBucket, int ruleBucket)
{
	applyActiveRuleSpanToNode(node, plan, customBucket);
	applyActiveRuleSpanToNode(node, plan, ruleBucket);
}

void syncPseudoElementsForNode(int node, const ActiveRulePlan *existingPlan = nullptr)
{
	ActiveRulePlan ownedPlan;
	const ActiveRulePlan *plan = existingPlan;
	if (!plan) {
		buildActiveRulePlanForNode(node, ownedPlan);
		plan = &ownedPlan;
	}

	const struct {
		CssRule::PseudoElement pseudo;
		int customBucket;
		int ruleBucket;
	} pseudoBuckets[] = {
	    {CssRule::PseudoElement::Before, kActiveBeforeCustom, kActiveBeforeRule},
	    {CssRule::PseudoElement::After, kActiveAfterCustom, kActiveAfterRule},
	};
	for (const auto &entry : pseudoBuckets) {
		const bool hasMatchingRule = plan->has(entry.customBucket) || plan->has(entry.ruleBucket);
		if (!hasMatchingRule) {
			const int stalePseudoNode = findPseudoChild(node, entry.pseudo);
			if (stalePseudoNode >= 0) Tree::instance().removeNode(stalePseudoNode);
			continue;
		}
		const int pseudoNode = ensurePseudoChild(node, entry.pseudo);
		if (pseudoNode < 0) continue;
		if (NodeRareData *rd = rareDataFor(pseudoNode)) rd->customProperties.clear();
		clearCustomPropertyLookupCache();
		int16_t staleRareStyle = treeState().nodes[pseudoNode].style.rare_style;
		Tree::instance().resetStyleForClassRecompute(pseudoNode);
		if (staleRareStyle >= 0 && treeState().nodes[pseudoNode].style.rare_style != staleRareStyle)
			releaseRareStyle(staleRareStyle);
		applyInheritedStyleDefaults(pseudoNode);
		applyDefaultStyleOverrides(pseudoNode);
		applyActiveRuleSpansToNode(pseudoNode, *plan, entry.customBucket, entry.ruleBucket);
		replayInlineStyles(pseudoNode);
	}
}

void recomputeNodeClassStyles(int node)
{
	auto &state = treeState();
	if (node < 0 || node >= state.nodeCount) return;

	const ComputedStyle beforeStyle = state.nodes[node].style;
	int16_t staleRareStyle = beforeStyle.rare_style;
	const int beforeImageId = state.nodes[node].image_id;
	state.styleInvalidationSuppressionDepth++;
	// Record this node's custom-property dependencies fresh (lookupCustomProperty
	// appends into g_nodeRefs[g_recordingNode] for every var() resolved here,
	// including the node's pseudo-elements via syncPseudoElementsForNode below).
	const int prevRecordingNode = g_recordingNode;
	g_recordingNode = node;
	if (node >= 0 && node < kMaxNodes) g_nodeRefs[node].clearForRecompute();
#if GEA_RECPROF
	g_profNodes++;
	int64_t _t = recNow();
#endif
	if (NodeRareData *rd = rareDataFor(node)) rd->customProperties.clear();
	clearCustomPropertyLookupCache();
	Tree::instance().resetStyleForClassRecompute(node);
	applyInheritedStyleDefaults(node);
	applyDefaultStyleOverrides(node);
#if GEA_RECPROF
	g_profResetUs += recNow() - _t;
	_t = recNow();
#endif
	ActiveRulePlan activePlan;
	buildActiveRulePlanForNode(node, activePlan);
#if GEA_RECPROF
	g_profCandUs += recNow() - _t;
	_t = recNow();
#endif
	applyActiveRuleSpansToNode(node, activePlan, kActiveMainCustom, kActiveMainRule);
#if GEA_RECPROF
	g_profApplyUs += recNow() - _t;
	_t = recNow();
#endif
	replayInlineStyles(node);
	primeCssAnimationsForNode(node, &activePlan);
	// A runtime `src` attribute's image id is NOT class-derived, so the reset
	// above must not lose it: restore it unless a class rule supplied its own
	// image. (Re-resolving from the attribute instead would re-read and
	// re-decode the file — e.g. a full-page EPUB cover from the SD card — on
	// every recompute of the node or any ancestor.)
	if (state.nodes[node].type == NodeType::Image && state.nodes[node].image_id < 0 &&
	    beforeImageId >= 0 && Tree::instance().hasAttribute(node, "src"))
		state.nodes[node].image_id = beforeImageId;
	state.styleInvalidationSuppressionDepth--;
	markClassRecomputeStyleDiff(node, beforeStyle, beforeImageId);
	if (staleRareStyle >= 0 && state.nodes[node].style.rare_style != staleRareStyle)
		releaseRareStyle(staleRareStyle);
#if GEA_RECPROF
	g_profMiscUs += recNow() - _t;
	_t = recNow();
#endif
	if (!isGeneratedPseudoNode(state.nodes[node]) &&
	    (g_ruleIndex.hasPseudoElementRules || nodeHasGeneratedPseudoChild(node)))
		syncPseudoElementsForNode(node, &activePlan);
#if GEA_RECPROF
	g_profPseudoUs += recNow() - _t;
#endif
	g_recordingNode = prevRecordingNode;
}

// When set (during the initial app mount), per-op class-style recomputes are
// deferred. Otherwise every setTagName / setClassName / appendChild re-walks the
// growing subtree against every CSS rule, which is O(nodes^2 x rules) over a
// mount — pathological for a large tree + large stylesheet (e.g. the weather
// app: 34s of CPU). endStyleMountBatch() clears this and runs a single
// recomputeAllClassStyles() pass, which produces identical final styles (it is
// the same pass the resize path already uses).
bool g_styleMountBatchActive = false;
// Roots whose subtree recompute was deferred while a batch is active. Processed
// (deduped + ancestor-subsumed) by endStyleMountBatch into the minimum set of
// subtree recomputes. This coalesces a burst of class changes — an app mount, or
// a reactive update like a city switch that flips a near-root class plus a few
// descendant classes — from N full/overlapping recomputes into one pass.
std::vector<int> g_pendingRecomputeRoots;

void recomputeSubtreeClassStyles(int node)
{
	if (g_styleMountBatchActive) {
		g_pendingRecomputeRoots.push_back(node);
		return;
	}
	auto &state = treeState();
	if (node < 0 || node >= state.nodeCount) return;
	recomputeNodeClassStyles(node);
	for (int child = state.nodes[node].first_child; child >= 0; child = state.nodes[child].next_sibling) {
		if (isGeneratedPseudoNode(state.nodes[child])) continue;
		recomputeSubtreeClassStyles(child);
	}
}

void recomputeDescendantClassStyles(int node)
{
	auto &state = treeState();
	if (node < 0 || node >= state.nodeCount) return;
	for (int child = state.nodes[node].first_child; child >= 0; child = state.nodes[child].next_sibling) {
		if (isGeneratedPseudoNode(state.nodes[child])) continue;
		recomputeSubtreeClassStyles(child);
	}
}

// --- Incremental subtree recompute ----------------------------------------
// The six inheritable computed-style fields (see applyInheritedStyleDefaults). If
// a node's recompute changes any of these, its descendants must recompute too.
struct InheritSnapshot {
	std::uint16_t text_color;
	std::int16_t font_id;
	std::int16_t font_size;
	std::int16_t font_weight;
	std::int16_t line_height;
	std::uint8_t text_align;
	std::uint8_t text_transform;
	std::uint8_t white_space;
};

InheritSnapshot snapshotInheritables(const ComputedStyle &s)
{
	return InheritSnapshot{static_cast<std::uint16_t>(s.text_color), static_cast<std::int16_t>(s.font_id),
	                       static_cast<std::int16_t>(s.font_size), static_cast<std::int16_t>(s.font_weight),
	                       static_cast<std::int16_t>(s.line_height),
	                       static_cast<std::uint8_t>(s.text_align), static_cast<std::uint8_t>(s.text_transform),
	                       static_cast<std::uint8_t>(s.white_space)};
}

bool inheritablesDiffer(const InheritSnapshot &a, const InheritSnapshot &b)
{
	return a.text_color != b.text_color || a.font_id != b.font_id || a.font_size != b.font_size ||
	       a.font_weight != b.font_weight || a.line_height != b.line_height ||
	       a.text_align != b.text_align || a.text_transform != b.text_transform ||
	       a.white_space != b.white_space;
}

inline void listInsertUnique(CssAtomSmallList &v, CssAtomId s)
{
	v.insertUnique(s);
}

inline void listErase(CssAtomSmallList &v, CssAtomId s)
{
	v.erase(s);
}

// True if any of the node's current classes (or its tag) is used as an ancestor
// matcher in some complex selector — so a change to this node's class set could
// flip which rules its DESCENDANTS match.
bool classChangeAffectsDescendants(int node)
{
	if (g_ruleIndex.ancestorClasses.empty() && g_ruleIndex.ancestorTags.empty()) return false;
	const auto &state = treeState();
	const NodeClassList &classes = state.classLists[node];
	for (std::size_t classIndex = 0, classCount = classes.size(); classIndex < classCount; ++classIndex) {
		const CssAtomId cls = classes.at(classIndex);
		if (g_ruleIndex.ancestorClasses.contains(cls)) return true;
	}
	return g_ruleIndex.ancestorTags.contains(state.nodes[node].tag_id);
}

struct NodeClassSnapshot {
	static constexpr std::size_t kInlineCount = 8;

	CssAtomId inlineTokens[kInlineCount]{};
	CssAtomId *spillTokens = nullptr;
	std::size_t count = 0;

	NodeClassSnapshot() = default;
	explicit NodeClassSnapshot(const NodeClassList &classes) { capture(classes); }
	NodeClassSnapshot(const NodeClassSnapshot &) = delete;
	NodeClassSnapshot &operator=(const NodeClassSnapshot &) = delete;

	~NodeClassSnapshot()
	{
		delete[] spillTokens;
	}

	void capture(const NodeClassList &classes)
	{
		delete[] spillTokens;
		spillTokens = nullptr;
		count = classes.size();
		CssAtomId *out = count > kInlineCount ? (spillTokens = new CssAtomId[count]) : inlineTokens;
		for (std::size_t i = 0; i < count; ++i)
			out[i] = classes.at(i);
	}

	CssAtomId at(std::size_t index) const
	{
		return spillTokens ? spillTokens[index] : inlineTokens[index];
	}
};

bool classTokensTouchAncestorSelectors(const NodeClassSnapshot &oldTokens, const NodeClassList &current)
{
	if (g_ruleIndex.ancestorClasses.empty()) return false;
	for (std::size_t i = 0; i < oldTokens.count; ++i) {
		const CssAtomId token = oldTokens.at(i);
		if (g_ruleIndex.ancestorClasses.contains(token)) return true;
	}
	for (std::size_t classIndex = 0, classCount = current.size(); classIndex < classCount; ++classIndex) {
		const CssAtomId token = current.at(classIndex);
		if (g_ruleIndex.ancestorClasses.contains(token)) return true;
	}
	return false;
}

struct CustomPropertyFingerprint {
	CssAtomId nameId = kInvalidCssAtom;
	CssAtomId valueAtom = kInvalidCssAtom;
	std::int32_t colorStyle = 0;
	std::int32_t colorNative = 0;
	float lengthValue = 0.0f;
	std::uint8_t colorAlpha = 255;
	std::uint8_t lengthUnit = 0;
	std::uint8_t flags = 0;
	bool valueEmpty = true;
	bool exact = true;
};

CustomPropertyFingerprint customPropertyFingerprint(const NodeCustomProperty &property)
{
	CustomPropertyFingerprint out;
	out.nameId = property.nameId;
	out.valueEmpty = property.value.empty();
	out.valueAtom = property.valueAtom;
	out.colorStyle = property.colorStyle;
	out.colorNative = property.colorNative;
	out.lengthValue = property.lengthValue;
	out.colorAlpha = property.colorAlpha;
	out.lengthUnit = property.lengthUnit;
	out.flags = property.flags;
	out.exact = out.valueEmpty || out.valueAtom != kInvalidCssAtom;
	return out;
}

bool customPropertyFingerprintEqualsValue(const CustomPropertyFingerprint &before,
                                          const NodeCustomProperty &after)
{
	if (before.nameId != after.nameId) return false;
	if (before.flags != after.flags) return false;
	if ((before.flags & 1u) != 0 &&
	    (before.colorStyle != after.colorStyle ||
	     before.colorNative != after.colorNative ||
	     before.colorAlpha != after.colorAlpha))
		return false;
	if ((before.flags & 2u) != 0 &&
	    (before.lengthUnit != after.lengthUnit ||
	     std::fabs(static_cast<double>(before.lengthValue) -
	               static_cast<double>(after.lengthValue)) >= 0.0001))
		return false;
	if (!before.exact) return false;  // conservative: recompute descendants rather than risk a stale var().
	if (before.valueEmpty) return after.value.empty();
	if (after.value.empty()) return false;
	return after.valueAtom == before.valueAtom && after.valueAtom != kInvalidCssAtom;
}

struct CustomPropertySnapshot {
	static constexpr std::uint8_t kInlineCount = 4;
	CustomPropertyFingerprint inlineEntries[kInlineCount]{};
	CustomPropertyFingerprint *spillEntries = nullptr;
	std::size_t spillCount = 0;
	std::size_t spillCapacity = 0;
	std::uint8_t inlineCount = 0;
	bool spilled = false;

	CustomPropertySnapshot() = default;
	CustomPropertySnapshot(const CustomPropertySnapshot &) = delete;
	CustomPropertySnapshot &operator=(const CustomPropertySnapshot &) = delete;

	~CustomPropertySnapshot()
	{
		delete[] spillEntries;
	}

	void add(const NodeCustomProperty &property)
	{
		const CustomPropertyFingerprint fingerprint = customPropertyFingerprint(property);
		if (!spilled && inlineCount < kInlineCount) {
			inlineEntries[inlineCount++] = fingerprint;
			return;
		}
		if (!spilled) {
			spillCapacity = kInlineCount * 2;
			spillEntries = new CustomPropertyFingerprint[spillCapacity];
			for (std::size_t i = 0; i < inlineCount; ++i)
				spillEntries[i] = inlineEntries[i];
			spillCount = inlineCount;
			spilled = true;
		}
		if (spillCount >= spillCapacity) {
			const std::size_t nextCapacity = spillCapacity ? spillCapacity * 2 : kInlineCount * 2;
			auto *next = new CustomPropertyFingerprint[nextCapacity];
			for (std::size_t i = 0; i < spillCount; ++i)
				next[i] = spillEntries[i];
			delete[] spillEntries;
			spillEntries = next;
			spillCapacity = nextCapacity;
		}
		spillEntries[spillCount++] = fingerprint;
	}

	std::size_t size() const { return spilled ? spillCount : inlineCount; }
	const CustomPropertyFingerprint &at(std::size_t index) const
	{
		return spilled ? spillEntries[index] : inlineEntries[index];
	}

	const CustomPropertyFingerprint *find(CssAtomId nameId) const
	{
		for (std::size_t i = 0, n = size(); i < n; ++i) {
			const auto &entry = at(i);
			if (entry.nameId == nameId) return &entry;
		}
		return nullptr;
	}
};

void snapshotCustomProperties(const NodeRareData *rareData, CustomPropertySnapshot &snapshot)
{
	if (!rareData) return;
	for (const auto &property : rareData->customProperties.values)
		snapshot.add(property);
}

void noteClassMutationForIncremental(int node, const NodeClassSnapshot &oldTokens)
{
	if (!g_styleMountBatchActive) return;
	rebuildRuleIndexIfNeeded();
	const auto &state = treeState();
	if (node < 0 || node >= state.nodeCount) return;
	if (classTokensTouchAncestorSelectors(oldTokens, state.classLists[node]))
		g_forceFullSubtreeMark.insert(node);
}

// Recompute `node` only if it is directly changed, inherits a changed value, or
// references a custom property whose resolved value changed; otherwise keep its
// existing (still-correct) computed style. `changedAbove` carries the names of
// custom properties whose value differs for this node vs the previous recompute;
// `forceByParent` is set when an ancestor's inheritable values changed;
// `forceSubtree` is set (and stays set for all descendants) when an ancestor's
// class change could alter descendant selector matches.
void recomputeNodeIncremental(int node, const CssAtomSmallList &changedAbove, bool forceByParent,
                              bool forceSubtree, const DenseNodeMark &pending)
{
	auto &state = treeState();
	if (node < 0 || node >= state.nodeCount) return;
	if (isGeneratedPseudoNode(state.nodes[node])) return;

	const NodeCustomPropRefs &refs = g_nodeRefs[node];
	const bool tracked = refs.tracked;
	const bool referencesChanged = refs.touches(changedAbove);
	const bool directlyChanged = pending.contains(node);
	// A never-recorded node (e.g. freshly created this batch) must recompute — we
	// have no dependency info to justify skipping it.
	const bool forceFull = g_forceFullSubtreeMark.contains(node);
	const bool mustRecompute = forceByParent || forceSubtree || forceFull || directlyChanged || referencesChanged || !tracked;
	// A node whose class change could flip a descendant-combinator match forces its
	// whole subtree (descendant selector matches may have changed). g_forceFullSubtreeMark
	// is the precise signal (captured at setClassName with both old+new classes);
	// classChangeAffectsDescendants is a current-class fallback for other paths.
	const bool childForceSubtree =
	    forceSubtree || forceFull || (directlyChanged && classChangeAffectsDescendants(node));

	CssAtomSmallList changedForChildren = changedAbove;
	bool inheritablesChanged = false;

	if (mustRecompute) {
#if GEA_INCREMENTAL_VERIFY
		g_incrRecomputedNodes.insert(node);
#endif
		const InheritSnapshot before = snapshotInheritables(state.nodes[node].style);
		CustomPropertySnapshot beforeCustom;
		snapshotCustomProperties(rareDataFor(node), beforeCustom);
		recomputeNodeClassStyles(node);
		inheritablesChanged = inheritablesDiffer(before, snapshotInheritables(state.nodes[node].style));
		// Reconcile the cascaded custom-property set for descendants: a (re)defined
		// property whose value changed is now changed for them; one whose value is
		// unchanged shadows any same-named change from above; a removed one un-shadows.
		static const std::vector<NodeCustomProperty> kNoCustomProps;
		const NodeRareData *rdCustomAfter = rareDataFor(node);
		const auto &afterCustom = rdCustomAfter ? rdCustomAfter->customProperties.values : kNoCustomProps;
		for (const auto &kv : afterCustom) {
			const CustomPropertyFingerprint *old = beforeCustom.find(kv.nameId);
			if (!old || !customPropertyFingerprintEqualsValue(*old, kv)) listInsertUnique(changedForChildren, kv.nameId);
			else listErase(changedForChildren, kv.nameId);
		}
		for (std::size_t i = 0, n = beforeCustom.size(); i < n; ++i) {
			const CustomPropertyFingerprint &b = beforeCustom.at(i);
			bool stillDefined = false;
			for (const auto &kv : afterCustom)
				if (kv.nameId == b.nameId) { stillDefined = true; break; }
			if (!stillDefined) listInsertUnique(changedForChildren, b.nameId);
		}
	} else {
		// Skipped: style + custom properties unchanged. Any property this node defines
		// shadows a same-named change from above with its (unchanged) value.
		if (const NodeRareData *rd = rareDataFor(node))
			for (const auto &kv : rd->customProperties.values)
				listErase(changedForChildren, kv.nameId);
	}

	for (int child = state.nodes[node].first_child; child >= 0; child = state.nodes[child].next_sibling) {
		if (isGeneratedPseudoNode(state.nodes[child])) continue;
		recomputeNodeIncremental(child, changedForChildren, inheritablesChanged, childForceSubtree, pending);
	}
}

void recomputeSubtreeIncremental(int root, const DenseNodeMark &pending)
{
	// The root is a directly-changed node, so it always recomputes; its custom-prop
	// and inheritable diffs seed the descendant walk.
	recomputeNodeIncremental(root, CssAtomSmallList{}, /*forceByParent=*/true, /*forceSubtree=*/false, pending);
}

#if GEA_INCREMENTAL_VERIFY
void collectSubtreeStyles(int node, std::vector<int> &ids, std::vector<ComputedStyle> &styles, std::vector<int> &imageIds)
{
	auto &state = treeState();
	if (node < 0 || node >= state.nodeCount) return;
	ids.push_back(node);
	styles.push_back(state.nodes[node].style);
	imageIds.push_back(state.nodes[node].image_id);
	for (int child = state.nodes[node].first_child; child >= 0; child = state.nodes[child].next_sibling)
		collectSubtreeStyles(child, ids, styles, imageIds);
}
#endif

void syncNodeClassAttribute(int node)
{
	// No-op: the class is no longer mirrored into the attribute table — doing so
	// would force a rare-data block onto every styled node, defeating the sparse
	// attribute storage. getAttribute("class") reads the dense NodeClassList
	// directly (see Tree::getAttribute). Kept as a no-op so call sites are intact.
	(void)node;
}

void recomputeAllClassStyles()
{
	auto &state = treeState();
	g_nodeRefOverflow.clear();
	clearCustomPropertyLookupCache();
	for (auto &refs : g_nodeRefs) refs.reset();
	for (int i = 0; i < state.nodeCount; i++) {
		if (state.nodes[i].parent < 0) recomputeSubtreeClassStyles(i);
	}
}

int g_ruleRegistrationBatchDepth = 0;
bool g_ruleRegistrationRulesChanged = false;
bool g_ruleRegistrationKeyframesChanged = false;

void flushRuleRegistrationBatch()
{
	if (g_ruleRegistrationKeyframesChanged) {
		invalidateKeyframeRuleIndex();
		g_ruleRegistrationKeyframesChanged = false;
	}
	if (g_ruleRegistrationRulesChanged) {
		invalidateRuleIndex();
		recomputeAllClassStyles();
		g_ruleRegistrationRulesChanged = false;
	}
}

void noteStyleRuleRegistrationChanged()
{
	if (g_ruleRegistrationBatchDepth > 0) {
		g_ruleRegistrationRulesChanged = true;
		return;
	}
	invalidateRuleIndex();
	recomputeAllClassStyles();
}

void noteKeyframeRuleRegistrationChanged()
{
	if (g_ruleRegistrationBatchDepth > 0) {
		g_ruleRegistrationKeyframesChanged = true;
		return;
	}
	invalidateKeyframeRuleIndex();
}

std::vector<std::string> splitCssTokens(const std::string &value)
{
	std::vector<std::string> out;
	std::size_t i = 0;
	while (i < value.size()) {
		while (i < value.size() && static_cast<unsigned char>(value[i]) <= ' ') ++i;
		const std::size_t start = i;
		int depth = 0;
		while (i < value.size()) {
			const char c = value[i];
			if (c == '(') depth++;
			else if (c == ')' && depth > 0) depth--;
			else if (static_cast<unsigned char>(c) <= ' ' && depth == 0) break;
			++i;
		}
		if (i > start) out.push_back(value.substr(start, i - start));
	}
	return out;
}

bool parseTimeMs(const std::string &token, std::uint32_t &out)
{
	const std::string lower = toLowerAscii(trimCssValue(token));
	if (lower.size() < 2) return false;
	char *end = nullptr;
	const double amount = std::strtod(lower.c_str(), &end);
	if (end == lower.c_str()) return false;
	const std::string unit = trimCssValue(std::string(end));
	if (unit == "ms") {
		out = static_cast<std::uint32_t>(std::max(0, roundToInt(amount)));
		return true;
	}
	if (unit == "s") {
		out = static_cast<std::uint32_t>(std::max(0, roundToInt(amount * 1000.0)));
		return true;
	}
	return false;
}

gea::css::Easing easingFromCss(const std::string &token)
{
	const std::string name = toLowerAscii(trimCssValue(token));
	if (name == "linear") return gea::css::Easing::linear();
	if (name == "ease") return gea::css::Easing::ease();
	if (name == "ease-in") return gea::css::Easing::easeIn();
	if (name == "ease-out") return gea::css::Easing::easeOut();
	if (name == "ease-in-out") return gea::css::Easing::easeInOut();
	if (startsWith(name, "steps(")) {
		const int n = static_cast<int>(std::strtol(functionInner(name, "steps").c_str(), nullptr, 10));
		return gea::css::Easing::steps(n > 0 ? n : 1);
	}
	if (startsWith(name, "cubic-bezier(")) {
		const auto parts = splitTopLevel(functionInner(name, "cubic-bezier"), ',');
		if (parts.size() == 4) {
			return gea::css::Easing::cubicBezier(std::strtod(parts[0].c_str(), nullptr),
			                                    std::strtod(parts[1].c_str(), nullptr),
			                                    std::strtod(parts[2].c_str(), nullptr),
			                                    std::strtod(parts[3].c_str(), nullptr));
		}
	}
	return gea::css::Easing::ease();
}

struct CssAnimationSpec {
	CssAtomId nameAtom = kInvalidCssAtom;
	std::uint32_t durationMs = 0;
	std::uint32_t delayMs = 0;
	int iterations = 1;
	gea::css::Direction direction = gea::css::Direction::Normal;
	gea::css::Fill fill = gea::css::Fill::None;
	gea::css::Easing easing = gea::css::Easing::ease();
	bool valid = false;
};

CssAnimationSpec parseAnimationShorthand(const std::string &value)
{
	CssAnimationSpec spec;
	bool sawDuration = false;
	bool sawName = false;
	for (const auto &tokenRaw : splitCssTokens(value)) {
		const std::string token = trimCssValue(tokenRaw);
		const std::string lower = toLowerAscii(token);
		std::uint32_t timeMs = 0;
		if (parseTimeMs(lower, timeMs)) {
			if (!sawDuration) {
				spec.durationMs = timeMs;
				sawDuration = true;
			} else {
				spec.delayMs = timeMs;
			}
			continue;
		}
		if (lower == "infinite") {
			spec.iterations = -1;
			continue;
		}
		char *end = nullptr;
		const long count = std::strtol(lower.c_str(), &end, 10);
		if (end && *end == '\0' && count >= 0) {
			spec.iterations = static_cast<int>(count);
			continue;
		}
		if (lower == "reverse") {
			spec.direction = gea::css::Direction::Reverse;
			continue;
		}
		if (lower == "alternate") {
			spec.direction = gea::css::Direction::Alternate;
			continue;
		}
		if (lower == "alternate-reverse") {
			spec.direction = gea::css::Direction::AlternateReverse;
			continue;
		}
		if (lower == "forwards") {
			spec.fill = gea::css::Fill::Forwards;
			continue;
		}
		if (lower == "backwards") {
			spec.fill = gea::css::Fill::Backwards;
			continue;
		}
		if (lower == "both") {
			spec.fill = gea::css::Fill::Both;
			continue;
		}
		if (lower == "linear" || lower == "ease" || lower == "ease-in" ||
		    lower == "ease-out" || lower == "ease-in-out" ||
		    startsWith(lower, "steps(") || startsWith(lower, "cubic-bezier(")) {
			spec.easing = easingFromCss(token);
			continue;
		}
		if (lower == "normal" || lower == "running" || lower == "none") continue;
		if (!sawName) {
			sawName = true;
			spec.nameAtom = internCssAtom(token);
		}
	}
	spec.valid = spec.nameAtom != kInvalidCssAtom && spec.durationMs > 0;
	return spec;
}

std::vector<CssAnimationSpec> &compiledCssAnimationSpecs()
{
	static std::vector<CssAnimationSpec> specs;
	return specs;
}

gea::css::Direction staticAnimationDirection(StaticStyleAnimationDirection direction)
{
	switch (direction) {
	case StaticStyleAnimationDirection::Normal: return gea::css::Direction::Normal;
	case StaticStyleAnimationDirection::Reverse: return gea::css::Direction::Reverse;
	case StaticStyleAnimationDirection::Alternate: return gea::css::Direction::Alternate;
	case StaticStyleAnimationDirection::AlternateReverse: return gea::css::Direction::AlternateReverse;
	}
	return gea::css::Direction::Normal;
}

gea::css::Fill staticAnimationFill(StaticStyleAnimationFill fill)
{
	switch (fill) {
	case StaticStyleAnimationFill::None: return gea::css::Fill::None;
	case StaticStyleAnimationFill::Forwards: return gea::css::Fill::Forwards;
	case StaticStyleAnimationFill::Backwards: return gea::css::Fill::Backwards;
	case StaticStyleAnimationFill::Both: return gea::css::Fill::Both;
	}
	return gea::css::Fill::None;
}

gea::css::Easing staticAnimationEasing(StaticStyleAnimationEasing easing)
{
	switch (easing.kind) {
	case StaticStyleAnimationEasingKind::Linear: return gea::css::Easing::linear();
	case StaticStyleAnimationEasingKind::Ease: return gea::css::Easing::ease();
	case StaticStyleAnimationEasingKind::EaseIn: return gea::css::Easing::easeIn();
	case StaticStyleAnimationEasingKind::EaseOut: return gea::css::Easing::easeOut();
	case StaticStyleAnimationEasingKind::EaseInOut: return gea::css::Easing::easeInOut();
	case StaticStyleAnimationEasingKind::CubicBezier:
		return gea::css::Easing::cubicBezier(easing.x1, easing.y1, easing.x2, easing.y2);
	case StaticStyleAnimationEasingKind::Steps:
		return gea::css::Easing::steps(easing.steps > 0 ? easing.steps : 1);
	}
	return gea::css::Easing::ease();
}

std::uint16_t storeStaticAnimationSpec(const char *name,
                                       std::uint32_t durationMs,
                                       std::uint32_t delayMs,
                                       int iterations,
                                       StaticStyleAnimationDirection direction,
                                       StaticStyleAnimationFill fill,
                                       StaticStyleAnimationEasing easing)
{
	if (!name || !*name || durationMs == 0) return kNoCompiledCssAnimationSpec;
	auto &specs = compiledCssAnimationSpecs();
	if (specs.size() >= kNoCompiledCssAnimationSpec) return kNoCompiledCssAnimationSpec;
	CssAnimationSpec spec;
	spec.nameAtom = internCssAtom(name);
	spec.durationMs = durationMs;
	spec.delayMs = delayMs;
	spec.iterations = iterations;
	spec.direction = staticAnimationDirection(direction);
	spec.fill = staticAnimationFill(fill);
	spec.easing = staticAnimationEasing(easing);
	spec.valid = true;
	specs.push_back(spec);
	return static_cast<std::uint16_t>(specs.size() - 1);
}

CssRule makeStaticAnimationCssRule(StaticStyleSelectorKind selectorKind,
                                   const char *selector,
                                   const char *name,
                                   std::uint32_t durationMs,
                                   std::uint32_t delayMs,
                                   int iterations,
                                   StaticStyleAnimationDirection direction,
                                   StaticStyleAnimationFill fill,
                                   StaticStyleAnimationEasing easing,
                                   const char *media)
{
	CssRule rule = makeStaticCompiledCssRule(selectorKind,
	                                        selector,
	                                        CssRuleProperty::Animation,
	                                        CssDeclarationId::Animation,
	                                        kNoCompiledCssValue,
	                                        media);
	rule.compiledAnimationSpec = storeStaticAnimationSpec(name,
	                                                     durationMs,
	                                                     delayMs,
	                                                     iterations,
	                                                     direction,
	                                                     fill,
	                                                     easing);
	return rule;
}

std::uint16_t compileCssAnimationSpec(const CssText &rawValue)
{
	if (rawValue.empty() || rawValue.hasVarReference()) return kNoCompiledCssAnimationSpec;
	const CssAnimationSpec spec = parseAnimationShorthand(rawValue.trimmedStr());
	if (!spec.valid) return kNoCompiledCssAnimationSpec;
	auto &specs = compiledCssAnimationSpecs();
	if (specs.size() >= kNoCompiledCssAnimationSpec) return kNoCompiledCssAnimationSpec;
	specs.push_back(spec);
	return static_cast<std::uint16_t>(specs.size() - 1);
}

const CssAnimationSpec *compiledCssAnimationSpecForHandle(std::uint16_t handle)
{
	const auto &specs = compiledCssAnimationSpecs();
	return handle < specs.size() ? &specs[handle] : nullptr;
}

CssAnimationSpec animationSpecForNode(int node)
{
	CssAnimationSpec spec;
	rebuildRuleIndexIfNeeded();
	const auto &list = rules();
	for (const int ri : g_ruleIndex.animationRules) {
		const CssRule &rule = list[ri];
		if (!ruleMediaMatchesIndex(ri) || !ruleMatchesNode(rule, node)) continue;
		if (const CssAnimationSpec *compiled = compiledCssAnimationSpecForHandle(rule.compiledAnimationSpec))
			spec = *compiled;
		else
			spec = parseAnimationShorthand(cssRuleTextForHandle(rule.valueText).str());
	}
	return spec;
}

CssAnimationSpec animationSpecForNodeFromActivePlan(const ActiveRulePlan &plan)
{
	CssAnimationSpec spec;
	if (plan.cachedEntry) {
		const CachedRuleApplyBucketSpan bucket = activeRulePlanCachedRuleSpan(plan, kActiveAnimation);
		const auto &list = rules();
		for (std::size_t i = 0; i < bucket.count; ++i) {
			const std::uint16_t ri = bucket.data[i].ruleIndex;
			if (ri == kNoCachedRuleIndex || ri >= list.size()) continue;
			const CssRule &rule = list[ri];
			if (const CssAnimationSpec *compiled = compiledCssAnimationSpecForHandle(rule.compiledAnimationSpec))
				spec = *compiled;
			else
				spec = parseAnimationShorthand(cssRuleTextForHandle(rule.valueText).str());
		}
		return spec;
	}

	const auto &list = rules();
	const DenseRuleBucketSpan bucket = activeRulePlanBucketSpan(plan, kActiveAnimation);
	for (std::size_t i = 0; i < bucket.count; ++i) {
		const int ri = bucket.data[i];
		if (ri < 0 || static_cast<std::size_t>(ri) >= list.size()) continue;
		const CssRule &rule = list[ri];
		if (const CssAnimationSpec *compiled = compiledCssAnimationSpecForHandle(rule.compiledAnimationSpec))
			spec = *compiled;
		else
			spec = parseAnimationShorthand(cssRuleTextForHandle(rule.valueText).str());
	}
	return spec;
}

double currentStyleValue(const Node &node, Property property)
{
	switch (property) {
	case Property::TransformRotate: return static_cast<double>(rstyle(node.style).transform_rotate) / 10.0;
	case Property::TransformRotateX: return static_cast<double>(rstyle(node.style).transform_rotate_x) / 10.0;
	case Property::TransformRotateY: return static_cast<double>(rstyle(node.style).transform_rotate_y) / 10.0;
	case Property::TransformTranslateX: return rstyle(node.style).transform_translate_x;
	case Property::TransformTranslateY: return rstyle(node.style).transform_translate_y;
	case Property::TransformTranslateZ: return rstyle(node.style).transform_translate_z;
	case Property::TransformTranslateXPercent: return rstyle(node.style).transform_translate_x_percent;
	case Property::TransformTranslateYPercent: return rstyle(node.style).transform_translate_y_percent;
	case Property::TransformScaleX: return rstyle(node.style).transform_scale_x;
	case Property::TransformScaleY: return rstyle(node.style).transform_scale_y;
	case Property::FilterBlur: return rstyle(node.style).filter_blur_radius;
	case Property::Opacity: return node.style.opacity;
	case Property::Width: return node.style.width;
	case Property::Height: return node.style.height;
	case Property::WidthPercent: return node.style.width_percent == kUnset ? 0 : node.style.width_percent;
	case Property::HeightPercent: return node.style.height_percent == kUnset ? 0 : node.style.height_percent;
	case Property::LineHeight: return node.style.line_height;
	case Property::FontWeight: return node.style.font_weight;
	case Property::Top: return node.style.pos_offsets[0] == kUnset ? 0 : node.style.pos_offsets[0];
	case Property::Right: return node.style.pos_offsets[1] == kUnset ? 0 : node.style.pos_offsets[1];
	case Property::Bottom: return node.style.pos_offsets[2] == kUnset ? 0 : node.style.pos_offsets[2];
	case Property::Left: return node.style.pos_offsets[3] == kUnset ? 0 : node.style.pos_offsets[3];
	case Property::TopPercent: return node.style.pos_offset_percent[0] == kUnset ? 0 : node.style.pos_offset_percent[0];
	case Property::RightPercent: return node.style.pos_offset_percent[1] == kUnset ? 0 : node.style.pos_offset_percent[1];
	case Property::BottomPercent: return node.style.pos_offset_percent[2] == kUnset ? 0 : node.style.pos_offset_percent[2];
	case Property::LeftPercent: return node.style.pos_offset_percent[3] == kUnset ? 0 : node.style.pos_offset_percent[3];
	case Property::BackgroundColor: return node.style.bg_color;
	case Property::Color: return node.style.text_color;
	default: return 0;
	}
}

struct CssAnimationTrack {
	Property property;
	gea::css::KeyframeList keyframes;
};

struct CssAnimationTrackList {
	static constexpr std::size_t kInlineTrackCapacity = 8;

	CssAnimationTrack inlineTracks[kInlineTrackCapacity]{};
	CssAnimationTrack *spillTracks = nullptr;
	std::size_t spillCount = 0;
	std::size_t spillCapacity = 0;
	std::uint8_t inlineCount = 0;
	bool spilled = false;

	CssAnimationTrackList() = default;
	CssAnimationTrackList(const CssAnimationTrackList &) = delete;
	CssAnimationTrackList &operator=(const CssAnimationTrackList &) = delete;

	~CssAnimationTrackList()
	{
		delete[] spillTracks;
	}

	std::size_t size() const { return spilled ? spillCount : inlineCount; }
	bool empty() const { return size() == 0; }

	CssAnimationTrack &at(std::size_t index)
	{
		return spilled ? spillTracks[index] : inlineTracks[index];
	}

	const CssAnimationTrack &at(std::size_t index) const
	{
		return spilled ? spillTracks[index] : inlineTracks[index];
	}

	CssAnimationTrack &push(Property property)
	{
		if (!spilled && inlineCount < kInlineTrackCapacity) {
			CssAnimationTrack &track = inlineTracks[inlineCount++];
			track.property = property;
			track.keyframes.clear();
			return track;
		}
		if (!spilled) {
			spillCapacity = kInlineTrackCapacity * 2;
			spillTracks = new CssAnimationTrack[spillCapacity];
			for (std::size_t i = 0; i < inlineCount; ++i)
				spillTracks[i] = inlineTracks[i];
			spillCount = inlineCount;
			spilled = true;
		}
		if (spillCount >= spillCapacity) {
			const std::size_t nextCapacity = spillCapacity ? spillCapacity * 2 : kInlineTrackCapacity * 2;
			CssAnimationTrack *next = new CssAnimationTrack[nextCapacity];
			for (std::size_t i = 0; i < spillCount; ++i)
				next[i] = spillTracks[i];
			delete[] spillTracks;
			spillTracks = next;
			spillCapacity = nextCapacity;
		}
		CssAnimationTrack &track = spillTracks[spillCount++];
		track.property = property;
		track.keyframes.clear();
		return track;
	}

	void removeShortTracks()
	{
		std::size_t write = 0;
		const std::size_t n = size();
		for (std::size_t read = 0; read < n; ++read) {
			CssAnimationTrack &track = at(read);
			if (track.keyframes.size() < 2) continue;
			if (write != read) at(write) = track;
			++write;
		}
		if (spilled)
			spillCount = write;
		else
			inlineCount = static_cast<std::uint8_t>(write);
	}
};

void addTrackKeyframe(CssAnimationTrackList &tracks, Property property, double offset, double value)
{
	for (std::size_t i = 0, n = tracks.size(); i < n; ++i) {
		CssAnimationTrack &track = tracks.at(i);
		if (track.property != property) continue;
		track.keyframes.push_back({offset, value});
		return;
	}
	CssAnimationTrack &track = tracks.push(property);
	track.keyframes.push_back({offset, value});
}

void addTransformComponentKeyframes(CssAnimationTrackList &tracks, double offset, const TransformComponents &transform)
{
	if (transform.hasRotateX) addTrackKeyframe(tracks, Property::TransformRotateX, offset, transform.rotateX / 10.0);
	if (transform.hasRotateY) addTrackKeyframe(tracks, Property::TransformRotateY, offset, transform.rotateY / 10.0);
	if (transform.hasRotateZ) addTrackKeyframe(tracks, Property::TransformRotate, offset, transform.rotateZ / 10.0);
	if (transform.hasTranslateX) addTrackKeyframe(tracks, Property::TransformTranslateX, offset, transform.translateX);
	if (transform.hasTranslateY) addTrackKeyframe(tracks, Property::TransformTranslateY, offset, transform.translateY);
	if (transform.hasTranslateZ) addTrackKeyframe(tracks, Property::TransformTranslateZ, offset, transform.translateZ);
	if (transform.hasTranslateX) addTrackKeyframe(tracks, Property::TransformTranslateXPercent, offset, transform.translateXPercent);
	if (transform.hasTranslateY) addTrackKeyframe(tracks, Property::TransformTranslateYPercent, offset, transform.translateYPercent);
	if (transform.hasScaleX) addTrackKeyframe(tracks, Property::TransformScaleX, offset, transform.scaleX);
	if (transform.hasScaleY) addTrackKeyframe(tracks, Property::TransformScaleY, offset, transform.scaleY);
}

void addTransformKeyframes(CssAnimationTrackList &tracks, int nodeId, int offsetPermille, const std::string &value)
{
	const TransformComponents transform = parseTransformComponents(value, nodeId);
	const double offset = std::max(0, std::min(1000, offsetPermille)) / 1000.0;
	addTransformComponentKeyframes(tracks, offset, transform);
}

bool addCompiledPropertyKeyframe(CssAnimationTrackList &tracks, int nodeId, int offsetPermille, const CssCompiledValue &compiled)
{
	const double offset = std::max(0, std::min(1000, offsetPermille)) / 1000.0;
	switch (compiled.kind) {
	case CssCompiledKind::Noop:
		return true;
	case CssCompiledKind::DirectProperty: {
		const Property property = static_cast<Property>(compiled.values[0]);
		switch (property) {
		case Property::Opacity:
		case Property::Width:
		case Property::Height:
		case Property::Left:
		case Property::Top:
		case Property::BackgroundColor:
		case Property::Color:
		case Property::FilterBlur:
			addTrackKeyframe(tracks, property, offset, compiled.values[1]);
			return true;
		default:
			return false;
		}
	}
	case CssCompiledKind::Opacity:
		addTrackKeyframe(tracks, Property::Opacity, offset, compiled.values[0]);
		return true;
	case CssCompiledKind::Color:
		if (compiled.declaration == CssDeclarationId::Background) {
			addTrackKeyframe(tracks, Property::BackgroundColor, offset, compiled.values[0]);
			return true;
		}
		if (compiled.declaration == CssDeclarationId::Color) {
			addTrackKeyframe(tracks, Property::Color, offset, compiled.values[0]);
			return true;
		}
		return false;
	case CssCompiledKind::ColorVar: {
		ResolvedCompiledCssColor color;
		const bool hasFallback = compiled.aux != 0;
		if (!resolveCompiledColorRef(nodeId,
		                             static_cast<CssAtomId>(compiled.values[0]),
		                             hasFallback ? 1 : 0,
		                             compiled.values[1],
		                             static_cast<style_color_t>(compiled.values[2]),
		                             static_cast<std::uint8_t>(hasFallback ? compiled.values[3] : 0),
		                             color))
			return false;
		if (compiled.declaration == CssDeclarationId::Background) {
			addTrackKeyframe(tracks, Property::BackgroundColor, offset, color.styleColor);
			return true;
		}
		if (compiled.declaration == CssDeclarationId::Color) {
			addTrackKeyframe(tracks, Property::Color, offset, color.styleColor);
			return true;
		}
		return false;
	}
	case CssCompiledKind::Length:
	case CssCompiledKind::Size:
	case CssCompiledKind::PositionOffset: {
		const CssLengthSpec &length = compiled.lengths[0];
		if (compiled.declaration == CssDeclarationId::Width) {
			addTrackKeyframe(tracks, Property::Width, offset, resolveCompiledLengthForNode(length, nodeId, LengthAxis::Horizontal));
			return true;
		}
		if (compiled.declaration == CssDeclarationId::Height) {
			addTrackKeyframe(tracks, Property::Height, offset, resolveCompiledLengthForNode(length, nodeId, LengthAxis::Vertical));
			return true;
		}
		if (compiled.declaration == CssDeclarationId::Left) {
			addTrackKeyframe(tracks, Property::Left, offset, resolveCompiledLengthForNode(length, nodeId, LengthAxis::Horizontal));
			return true;
		}
		if (compiled.declaration == CssDeclarationId::Top) {
			addTrackKeyframe(tracks, Property::Top, offset, resolveCompiledLengthForNode(length, nodeId, LengthAxis::Vertical));
			return true;
		}
		return false;
	}
	case CssCompiledKind::Transform:
		addTransformComponentKeyframes(tracks, offset, transformFromCompiled(compiled, nodeId));
		return true;
	case CssCompiledKind::Rotate:
		if (compiled.declaration != CssDeclarationId::Rotate) return false;
		addTrackKeyframe(tracks, Property::TransformRotate, offset, compiled.values[0] / 10.0);
		return true;
	case CssCompiledKind::Scale:
		if (compiled.declaration != CssDeclarationId::Scale) return false;
		addTrackKeyframe(tracks, Property::TransformScaleX, offset, compiled.values[0]);
		addTrackKeyframe(tracks, Property::TransformScaleY, offset, compiled.values[0]);
		return true;
	case CssCompiledKind::FilterBlur: {
		int radius = compiled.aux == 0 ? 0 : resolveCompiledLengthForNode(compiled.lengths[0], nodeId, LengthAxis::None);
		if (radius < 0) radius = 0;
		if (radius > 64) radius = 64;
		addTrackKeyframe(tracks, Property::FilterBlur, offset, radius);
		return true;
	}
	default:
		return false;
	}
}

void addPropertyKeyframe(CssAnimationTrackList &tracks, int nodeId, int offsetPermille, CssDeclarationId declaration, const std::string &value)
{
	const double offset = std::max(0, std::min(1000, offsetPermille)) / 1000.0;
	if (declaration == CssDeclarationId::Transform) {
		addTransformKeyframes(tracks, nodeId, offsetPermille, value);
		return;
	}
	if (declaration == CssDeclarationId::Opacity) addTrackKeyframe(tracks, Property::Opacity, offset, parseOpacity(value));
	else if (declaration == CssDeclarationId::Rotate)
		addTrackKeyframe(tracks, Property::TransformRotate, offset, parseRotateTenths(value) / 10.0);
	else if (declaration == CssDeclarationId::Scale) {
		const int scale = parseScalePermille(value);
		addTrackKeyframe(tracks, Property::TransformScaleX, offset, scale);
		addTrackKeyframe(tracks, Property::TransformScaleY, offset, scale);
	}
	else if (declaration == CssDeclarationId::Width) addTrackKeyframe(tracks, Property::Width, offset, parseLengthForNode(value, nodeId, LengthAxis::Horizontal));
	else if (declaration == CssDeclarationId::Height) addTrackKeyframe(tracks, Property::Height, offset, parseLengthForNode(value, nodeId, LengthAxis::Vertical));
	else if (declaration == CssDeclarationId::Left) addTrackKeyframe(tracks, Property::Left, offset, parseLengthForNode(value, nodeId, LengthAxis::Horizontal));
	else if (declaration == CssDeclarationId::Top) addTrackKeyframe(tracks, Property::Top, offset, parseLengthForNode(value, nodeId, LengthAxis::Vertical));
	else if (declaration == CssDeclarationId::Background)
		addTrackKeyframe(tracks, Property::BackgroundColor, offset, parseColorStyleValue(value));
	else if (declaration == CssDeclarationId::Color)
		addTrackKeyframe(tracks, Property::Color, offset, parseColorStyleValue(value));
	else if (declaration == CssDeclarationId::Filter)
		addTrackKeyframe(tracks, Property::FilterBlur, offset, parseFilterBlurRadius(value, nodeId));
}

void sortTrackKeyframesByOffset(gea::css::KeyframeList &keyframes)
{
	for (std::size_t i = 1; i < keyframes.size(); ++i) {
		const gea::css::Keyframe keyframe = keyframes[i];
		std::size_t j = i;
		while (j > 0 && keyframe.offset < keyframes[j - 1].offset) {
			keyframes[j] = keyframes[j - 1];
			--j;
		}
		keyframes[j] = keyframe;
	}
}

void normalizeTrackEndpoints(CssAnimationTrack &track, const Node &node)
{
	sortTrackKeyframesByOffset(track.keyframes);
	if (track.keyframes.empty()) return;
	const double current = currentStyleValue(node, track.property);
	const bool needsStart = track.keyframes.front().offset > 0.0;
	const bool needsEnd = track.keyframes.back().offset < 1.0;
	if (needsStart) track.keyframes.push_back({0.0, current});
	if (needsEnd) track.keyframes.push_back({1.0, current});
	if (needsStart) sortTrackKeyframesByOffset(track.keyframes);
}

void buildAnimationTracksForNode(int nodeId, const CssAnimationSpec &spec, CssAnimationTrackList &tracks)
{
	if (!spec.valid) return;
	auto &state = treeState();
	if (nodeId < 0 || nodeId >= state.nodeCount) return;

	const DenseRuleBucketSpan indices = keyframeRuleIndicesForName(spec.nameAtom);
	if (indices.empty()) return;
	const auto &rules = keyframeRules();
	for (std::size_t i = 0; i < indices.count; ++i) {
		const int ri = indices.data[i];
		if (ri < 0 || ri >= static_cast<int>(rules.size())) continue;
		const auto &rule = rules[ri];
		if (const CssCompiledValue *compiled = compiledCssValueForHandle(rule.compiledValue)) {
			if (addCompiledPropertyKeyframe(tracks, nodeId, rule.offsetPermille, *compiled)) continue;
		}
		addPropertyKeyframe(tracks,
		                    nodeId,
		                    rule.offsetPermille,
		                    rule.declaration,
		                    cssRuleTextForHandle(rule.valueText).str());
	}
	for (std::size_t i = 0, n = tracks.size(); i < n; ++i)
		normalizeTrackEndpoints(tracks.at(i), state.nodes[nodeId]);
	tracks.removeShortTracks();
}

gea::css::Animation animationFromTrack(int nodeId, const CssAnimationSpec &spec, const CssAnimationTrack &track)
{
	gea::css::Animation animation;
	animation.nodeId = nodeId;
	animation.property = track.property;
	animation.kind = gea::css::kindOf(track.property);
	animation.keyframes = track.keyframes;
	animation.easing = spec.easing;
	animation.durationMs = spec.durationMs;
	animation.delayMs = spec.delayMs;
	animation.iterations = spec.iterations;
	animation.direction = spec.direction;
	animation.fill = spec.fill;
	return animation;
}

void applyPrimedAnimationValue(const gea::css::Animation &animation, double value)
{
	NodeHandle node(animation.nodeId);
	if (!node) return;
	switch (animation.kind) {
	case gea::css::ValueKind::Angle:
		setStyleValue(node, animation.property, static_cast<int>(std::llround(value * 10.0)), StyleApplicationSource::ClassRule);
		return;
	case gea::css::ValueKind::Color: {
		const int rgb565 = static_cast<int>(std::llround(value));
		setStyleValue(node, animation.property, rgb565, StyleApplicationSource::ClassRule);
		if (animation.property == Property::BackgroundColor)
			setStyleValue(node, Property::HasBackground, 1, StyleApplicationSource::ClassRule);
		else if (animation.property == Property::ActiveBackgroundColor)
			setStyleValue(node, Property::HasActiveBackground, 1, StyleApplicationSource::ClassRule);
		return;
	}
	case gea::css::ValueKind::Scalar:
		setStyleValue(node, animation.property, static_cast<int>(std::llround(value)), StyleApplicationSource::ClassRule);
		return;
	}
}

void primeCssAnimationsForNode(int node, const ActiveRulePlan *activePlan)
{
	if (activePlan) {
		if (!activePlan->has(kActiveAnimation)) return;
	} else {
		rebuildRuleIndexIfNeeded();
		if (g_ruleIndex.animationRules.empty()) return;
	}
	const CssAnimationSpec spec = activePlan ? animationSpecForNodeFromActivePlan(*activePlan) : animationSpecForNode(node);
	CssAnimationTrackList tracks;
	buildAnimationTracksForNode(node, spec, tracks);
	for (std::size_t i = 0, n = tracks.size(); i < n; ++i) {
		const CssAnimationTrack &track = tracks.at(i);
		const gea::css::Animation animation = animationFromTrack(node, spec, track);
		const gea::css::Progress progress = gea::css::computeProgress(animation, 0.0);
		if (progress.active) applyPrimedAnimationValue(animation, gea::css::sampleTrack(animation, progress.p));
	}
}

void startAnimationForNode(int nodeId, const CssAnimationSpec &spec, std::uint32_t nowMs)
{
	CssAnimationTrackList tracks;
	buildAnimationTracksForNode(nodeId, spec, tracks);
	for (std::size_t i = 0, n = tracks.size(); i < n; ++i) {
		const CssAnimationTrack &track = tracks.at(i);
		gea::css::Animation animation = animationFromTrack(nodeId, spec, track);
		gea::css::AnimationEngine::instance().start(std::move(animation), nowMs);
	}
}

}  // namespace

void beginStyleMountBatch()
{
	g_styleMountBatchActive = true;
}

bool styleMountBatchActive()
{
	return g_styleMountBatchActive;
}

void endStyleMountBatch()
{
	if (!g_styleMountBatchActive) return;
	g_styleMountBatchActive = false;
	if (g_pendingRecomputeRoots.empty()) {
		g_forceFullSubtreeMark.clear();
		return;
	}

	g_pendingRecomputeMark.clear();
	std::vector<int> roots;
	auto &state = treeState();
	roots.reserve(g_pendingRecomputeRoots.size());
	for (const int node : g_pendingRecomputeRoots) {
		if (node < 0 || node >= state.nodeCount) continue;
		if (g_pendingRecomputeMark.contains(node)) continue;
		g_pendingRecomputeMark.insert(node);
		roots.push_back(node);
	}
	g_pendingRecomputeRoots.clear();
	if (roots.empty()) {
		g_forceFullSubtreeMark.clear();
		return;
	}

#if GEA_RECPROF
	g_profResetUs = g_profCandUs = g_profApplyUs = g_profVarUs = g_profMiscUs = g_profPseudoUs = g_profBodyUs = 0;
	g_profBgUs = g_profXformUs = g_profLenUs = g_profColorUs = 0;
	g_profNodes = g_profApplyCalls = g_profBgCalls = g_profXformCalls = g_profLenCalls = g_profColorCalls = 0;
	const int64_t _batchStart = recNow();
#endif
	// The top-most pending roots: a root whose ancestor is also pending is covered by
	// that ancestor's subtree recompute (and for a mount where every node is pending,
	// recomputing each separately would reintroduce the O(nodes^2) blow-up).
	std::vector<int> topRoots;
	topRoots.reserve(roots.size());
	for (const int node : roots) {
		if (node < 0 || node >= state.nodeCount) continue;
		bool covered = false;
		for (int ancestor = state.nodes[node].parent; ancestor >= 0; ancestor = state.nodes[ancestor].parent) {
			if (g_pendingRecomputeMark.contains(ancestor)) {
				covered = true;
				break;
			}
		}
		if (!covered) topRoots.push_back(node);
	}

	// Incremental recompute: a near-root class change (e.g. a theme switch) only
	// re-styles nodes that actually depend on what changed; the rest keep their
	// already-correct computed style. Equivalent to recomputeSubtreeClassStyles but
	// skips the per-node parse/dispatch cost for unaffected nodes.
#if GEA_INCREMENTAL_VERIFY
	g_incrRecomputedNodes.clear();
#endif
	for (const int node : topRoots) recomputeSubtreeIncremental(node, g_pendingRecomputeMark);

#if GEA_INCREMENTAL_VERIFY
	// Prove equivalence on-device: snapshot the incremental result, then run the full
	// recompute and assert every node's computed style is byte-identical.
	std::vector<int> vIds;
	std::vector<ComputedStyle> vStyles;
	std::vector<int> vImg;
	for (const int node : topRoots) collectSubtreeStyles(node, vIds, vStyles, vImg);
	for (const int node : topRoots) recomputeSubtreeClassStyles(node);
	int mismatches = 0;
	int logged = 0;
	for (std::size_t i = 0; i < vIds.size(); ++i) {
		const int id = vIds[i];
		// Field-wise compare (ComputedStyle has padding, so memcmp gives false diffs).
		// styleEqualExceptTextPaint covers every field but text_color, which we add.
		const bool equal = styleEqualExceptTextPaint(vStyles[i], state.nodes[id].style) &&
		                   vStyles[i].text_color == state.nodes[id].style.text_color &&
		                   vImg[i] == state.nodes[id].image_id;
		if (!equal) {
			mismatches++;
			if (logged < 8) {
				const bool recomputed = g_incrRecomputedNodes.count(id) > 0;
				const std::string refs = (id >= 0 && id < kMaxNodes) ? g_nodeRefs[id].debugString() : std::string();
				GEA_STYLE_LOGW("gea.recincr", "  mismatch node=%d tag=%s class=[%s] incrRecomputed=%d refs=[%s]", id,
				               tagFromId(state.nodes[id].tag_id), state.classLists[id].value().c_str(), recomputed ? 1 : 0,
				               refs.c_str());
				logged++;
			}
		}
	}
	if (mismatches > 0)
		GEA_STYLE_LOGW("gea.recincr", "VERIFY MISMATCH: %d/%d nodes differ", mismatches, static_cast<int>(vIds.size()));
	else
		GEA_STYLE_LOGW("gea.recincr", "VERIFY OK: %d nodes identical", static_cast<int>(vIds.size()));
#endif
#if GEA_RECPROF
	const int64_t _batchUs = recNow() - _batchStart;
	if (_batchUs > 50000) {
		std::snprintf(g_recprofLast, sizeof g_recprofLast,
			"recompute=%lldus nodes=%d applyCalls=%d | reset=%lldus cand=%lldus apply=%lldus(var=%lldus filter=%lldus body=%lldus[bg=%lldus/%d xform=%lldus/%d len=%lldus/%d color=%lldus/%d simple=%lldus]) misc=%lldus pseudo=%lldus",
			static_cast<long long>(_batchUs), g_profNodes, g_profApplyCalls,
			static_cast<long long>(g_profResetUs), static_cast<long long>(g_profCandUs),
			static_cast<long long>(g_profApplyUs), static_cast<long long>(g_profVarUs),
			static_cast<long long>(g_profApplyUs - g_profVarUs - g_profBodyUs),
			static_cast<long long>(g_profBodyUs),
			static_cast<long long>(g_profBgUs), g_profBgCalls,
			static_cast<long long>(g_profXformUs), g_profXformCalls,
			static_cast<long long>(g_profLenUs), g_profLenCalls,
			static_cast<long long>(g_profColorUs), g_profColorCalls,
			static_cast<long long>(g_profBodyUs - g_profBgUs - g_profXformUs - g_profLenUs - g_profColorUs),
			static_cast<long long>(g_profMiscUs), static_cast<long long>(g_profPseudoUs));
		GEA_STYLE_LOGW("gea.recprof", "%s", g_recprofLast);
	}
#endif
	g_forceFullSubtreeMark.clear();
}

// Query hook for the last recompute-batch profile line (GEA_RECPROF builds).
extern "C" const char *gea_recprof_last()
{
#if GEA_RECPROF
	return g_recprofLast[0] ? g_recprofLast : nullptr;
#else
	return nullptr;
#endif
}

StyleSheet &StyleSheet::instance()
{
	static StyleSheet sheet;
	return sheet;
}

void Style::rotateDegrees(double value) const
{
	set(Property::TransformRotate, numericRotateTenths(value));
}

void Style::scale(double value) const
{
	const int scale = numericScalePermille(value);
	set(Property::TransformScaleX, scale);
	set(Property::TransformScaleY, scale);
}

void StyleSheet::clear()
{
	rules().clear();
	keyframeRules().clear();
	g_ruleRegistrationBatchDepth = 0;
	g_ruleRegistrationRulesChanged = false;
	g_ruleRegistrationKeyframesChanged = false;
	clearKeyframeRuleIndex();
	clearCssRuleTexts();
	compiledCssValues().clear();
	clearInlineCompiledStyleCache();
	compiledCssAnimationSpecs().clear();
	clearCompiledCssBackgrounds();
	clearCompiledCssGridTemplates();
	clearCompiledCssLengthExpressions();
	clearCompiledCssLengthCache();
	clearCompiledCssColorCache();
	clearMediaConditionPlans();
	gea::css::AnimationEngine::instance().clear();
	invalidateRuleIndex();
	clearSelectorPartsCache();
	g_nodeRefOverflow.clear();
	clearCustomPropertyLookupCache();
	for (auto &refs : g_nodeRefs) refs.reset();  // stale per-node custom-prop dependencies from the previous app
	g_pendingRecomputeMark.clear();
	g_forceFullSubtreeMark.clear();
	recomputeAllClassStyles();
}

void StyleSheet::beginRuleRegistrationBatch()
{
	if (g_ruleRegistrationBatchDepth < 0x7FFF) ++g_ruleRegistrationBatchDepth;
}

void StyleSheet::endRuleRegistrationBatch()
{
	if (g_ruleRegistrationBatchDepth <= 0) return;
	--g_ruleRegistrationBatchDepth;
	if (g_ruleRegistrationBatchDepth == 0) flushRuleRegistrationBatch();
}

void StyleSheet::registerRule(const std::string &className, const std::string &property, const std::string &value, const std::string &media)
{
	rules().push_back(makeCssRule(CssRule::SelectorType::Class,
	                              CssRule::PseudoElement::None,
	                              CssText::view(className),
	                              CssText::copy(property),
	                              CssText::copy(value),
	                              CssText::copy(media)));
	noteStyleRuleRegistrationChanged();
}

void StyleSheet::registerElementRule(const std::string &elementName, const std::string &property, const std::string &value, const std::string &media)
{
	rules().push_back(makeCssRule(CssRule::SelectorType::Element,
	                              CssRule::PseudoElement::None,
	                              CssText::view(elementName),
	                              CssText::copy(property),
	                              CssText::copy(value),
	                              CssText::copy(media)));
	noteStyleRuleRegistrationChanged();
}

void StyleSheet::registerSelectorRule(const std::string &selector, const std::string &property, const std::string &value, const std::string &media)
{
	const SelectorTextSlice selectorSlice = selectorTextWithoutPseudo(selector.c_str(), selector.size());
	rules().push_back(makeCssRule(CssRule::SelectorType::Selector,
	                              selectorSlice.pseudo,
	                              CssText::view(selectorSlice.data, selectorSlice.length),
	                              CssText::copy(property),
	                              CssText::copy(value),
	                              CssText::copy(media)));
	noteStyleRuleRegistrationChanged();
}

void StyleSheet::registerKeyframeRule(const std::string &name, int offsetPermille, const std::string &property, const std::string &value)
{
	keyframeRules().push_back(makeCssKeyframeRule(CssText::view(name), offsetPermille, CssText::view(property), CssText::copy(value)));
	noteKeyframeRuleRegistrationChanged();
}

void StyleSheet::registerStaticRule(const char *className, const char *property, const char *value, const char *media)
{
	rules().push_back(makeCssRule(CssRule::SelectorType::Class,
	                              CssRule::PseudoElement::None,
	                              CssText::literal(className),
	                              CssText::literal(property),
	                              CssText::literal(value),
	                              CssText::literal(media)));
	noteStyleRuleRegistrationChanged();
}

void StyleSheet::registerStaticElementRule(const char *elementName, const char *property, const char *value, const char *media)
{
	rules().push_back(makeCssRule(CssRule::SelectorType::Element,
	                              CssRule::PseudoElement::None,
	                              CssText::literal(elementName),
	                              CssText::literal(property),
	                              CssText::literal(value),
	                              CssText::literal(media)));
	noteStyleRuleRegistrationChanged();
}

void StyleSheet::registerStaticSelectorRule(const char *selector, const char *property, const char *value, const char *media)
{
	const char *selectorText = selector ? selector : "";
	const SelectorTextSlice selectorSlice = selectorTextWithoutPseudo(selectorText, std::strlen(selectorText));
	rules().push_back(makeCssRule(CssRule::SelectorType::Selector,
	                              selectorSlice.pseudo,
	                              CssText::view(selectorSlice.data, selectorSlice.length),
	                              CssText::literal(property),
	                              CssText::literal(value),
	                              CssText::literal(media)));
	noteStyleRuleRegistrationChanged();
}

void StyleSheet::registerStaticKeyframeRule(const char *name, int offsetPermille, const char *property, const char *value)
{
	keyframeRules().push_back(makeCssKeyframeRule(CssText::literal(name), offsetPermille, CssText::literal(property), CssText::literal(value)));
	noteKeyframeRuleRegistrationChanged();
}

void StyleSheet::registerStaticPropertyKeyframeRule(const char *name, int offsetPermille, Property property, int value)
{
	keyframeRules().push_back(makeStaticPropertyKeyframeRule(name, offsetPermille, property, value));
	noteKeyframeRuleRegistrationChanged();
}

void StyleSheet::registerStaticColorKeyframeRule(const char *name,
                                                 int offsetPermille,
                                                 StaticStyleColorProperty property,
                                                 int r,
                                                 int g,
                                                 int b,
                                                 int a)
{
	keyframeRules().push_back(makeStaticColorKeyframeRule(name, offsetPermille, property, r, g, b, a));
	noteKeyframeRuleRegistrationChanged();
}

void StyleSheet::registerStaticColorVarKeyframeRule(const char *name,
                                                    int offsetPermille,
                                                    StaticStyleColorProperty property,
                                                    const char *varName,
                                                    bool hasFallback,
                                                    int r,
                                                    int g,
                                                    int b,
                                                    int a)
{
	keyframeRules().push_back(makeStaticColorVarKeyframeRule(name,
	                                                        offsetPermille,
	                                                        property,
	                                                        varName,
	                                                        hasFallback,
	                                                        r,
	                                                        g,
	                                                        b,
	                                                        a));
	noteKeyframeRuleRegistrationChanged();
}

void StyleSheet::registerStaticLengthKeyframeRule(const char *name,
                                                  int offsetPermille,
                                                  StaticStyleLengthProperty property,
                                                  StaticStyleLengthUnit unit,
                                                  float value)
{
	keyframeRules().push_back(makeStaticLengthKeyframeRule(name, offsetPermille, property, StaticStyleLengthSpec{unit, value}));
	noteKeyframeRuleRegistrationChanged();
}

void StyleSheet::registerStaticLengthSpecKeyframeRule(const char *name,
                                                      int offsetPermille,
                                                      StaticStyleLengthProperty property,
                                                      StaticStyleLengthSpec length)
{
	keyframeRules().push_back(makeStaticLengthKeyframeRule(name, offsetPermille, property, length));
	noteKeyframeRuleRegistrationChanged();
}

void StyleSheet::registerStaticFilterBlurKeyframeRule(const char *name,
                                                      int offsetPermille,
                                                      StaticStyleLengthSpec radius)
{
	keyframeRules().push_back(makeStaticFilterBlurKeyframeRule(name, offsetPermille, radius));
	noteKeyframeRuleRegistrationChanged();
}

void StyleSheet::registerStaticTransformKeyframeRule(const char *name,
                                                     int offsetPermille,
                                                     std::uint16_t flags,
                                                     int rotateX,
                                                     int rotateY,
                                                     int rotateZ,
                                                     StaticStyleLengthSpec translateX,
                                                     StaticStyleLengthSpec translateY,
                                                     StaticStyleLengthSpec translateZ,
                                                     int scaleX,
                                                     int scaleY)
{
	keyframeRules().push_back(makeStaticTransformKeyframeRule(name,
	                                                        offsetPermille,
	                                                        flags,
	                                                        rotateX,
	                                                        rotateY,
	                                                        rotateZ,
	                                                        translateX,
	                                                        translateY,
	                                                        translateZ,
	                                                        scaleX,
	                                                        scaleY));
	noteKeyframeRuleRegistrationChanged();
}

void StyleSheet::registerStaticPropertyRule(StaticStyleSelectorKind selectorKind,
                                            const char *selector,
                                            Property property,
                                            int value,
                                            const char *media)
{
	rules().push_back(makeDirectPropertyCssRule(selectorKind, selector, property, value, media));
	noteStyleRuleRegistrationChanged();
}

void StyleSheet::registerStaticPropertyGroupRule(StaticStyleSelectorKind selectorKind,
                                                 const char *selector,
                                                 std::initializer_list<StaticStylePropertyValue> properties,
                                                 const char *media)
{
	rules().push_back(makeDirectPropertyGroupCssRule(selectorKind, selector, properties, media));
	noteStyleRuleRegistrationChanged();
}

std::uint16_t StyleSheet::registerStaticSelectorPlan(const char *selector,
                                                     std::initializer_list<StaticStyleSelectorPartSpec> parts)
{
	return storeStaticSelectorPlan(selector, parts);
}

std::uint16_t StyleSheet::registerStaticMediaConditionPlan(const char *condition,
                                                           std::initializer_list<StaticStyleMediaQuerySpec> queries)
{
	return storeStaticMediaConditionPlan(condition, queries);
}

void StyleSheet::registerStaticColorRule(StaticStyleSelectorKind selectorKind,
                                         const char *selector,
                                         StaticStyleColorProperty property,
                                         int r,
                                         int g,
                                         int b,
                                         int a,
                                         const char *media)
{
	rules().push_back(makeStaticColorCssRule(selectorKind, selector, property, r, g, b, a, media));
	noteStyleRuleRegistrationChanged();
}

void StyleSheet::registerStaticColorVarRule(StaticStyleSelectorKind selectorKind,
                                            const char *selector,
                                            StaticStyleColorProperty property,
                                            const char *name,
                                            bool hasFallback,
                                            int r,
                                            int g,
                                            int b,
                                            int a,
                                            const char *media)
{
	rules().push_back(makeStaticColorVarCssRule(selectorKind,
	                                           selector,
	                                           property,
	                                           name,
	                                           hasFallback,
	                                           r,
	                                           g,
	                                           b,
	                                           a,
	                                           media));
	noteStyleRuleRegistrationChanged();
}

void StyleSheet::registerStaticLengthRule(StaticStyleSelectorKind selectorKind,
                                          const char *selector,
                                          StaticStyleLengthProperty property,
                                          StaticStyleLengthUnit unit,
                                          float value,
                                          const char *media)
{
	rules().push_back(makeStaticLengthCssRule(selectorKind, selector, property, unit, value, media));
	noteStyleRuleRegistrationChanged();
}

std::uint16_t StyleSheet::registerStaticLengthExpression(StaticStyleLengthExpressionKind kind,
                                                         StaticStyleLengthSpec a,
                                                         StaticStyleLengthSpec b,
                                                         StaticStyleLengthSpec c,
                                                         float scalar,
                                                         const char *name,
                                                         bool hasFallback)
{
	CssLengthExpression expression;
	expression.kind = cssLengthExpressionKindForStatic(kind);
	expression.a = cssLengthSpecForStatic(a);
	expression.b = cssLengthSpecForStatic(b);
	expression.c = cssLengthSpecForStatic(c);
	expression.scalar = scalar;
	if (kind == StaticStyleLengthExpressionKind::Var) {
		expression.nameAtom = internCssAtom(name ? name : "");
		expression.hasFallback = hasFallback ? 1 : 0;
	}
	return storeCompiledCssLengthExpression(expression);
}

void StyleSheet::registerStaticLengthSpecRule(StaticStyleSelectorKind selectorKind,
                                              const char *selector,
                                              StaticStyleLengthProperty property,
                                              StaticStyleLengthSpec length,
                                              const char *media)
{
	rules().push_back(makeStaticLengthSpecCssRule(selectorKind, selector, property, length, media));
	noteStyleRuleRegistrationChanged();
}

void StyleSheet::registerStaticFontFamilyRule(StaticStyleSelectorKind selectorKind,
                                              const char *selector,
                                              const char *family,
                                              const char *media)
{
	rules().push_back(makeStaticFontFamilyCssRule(selectorKind, selector, family, media));
	noteStyleRuleRegistrationChanged();
}

void StyleSheet::registerStaticLineHeightRule(StaticStyleSelectorKind selectorKind,
                                              const char *selector,
                                              StaticStyleLineHeightKind kind,
                                              StaticStyleLengthSpec value,
                                              const char *media)
{
	rules().push_back(makeStaticLineHeightCssRule(selectorKind, selector, kind, value, media));
	noteStyleRuleRegistrationChanged();
}

void StyleSheet::registerStaticFlexRule(StaticStyleSelectorKind selectorKind,
                                        const char *selector,
                                        int grow,
                                        StaticStyleLengthSpec basis,
                                        bool hasBasis,
                                        const char *media)
{
	rules().push_back(makeStaticFlexCssRule(selectorKind, selector, grow, basis, hasBasis, media));
	noteStyleRuleRegistrationChanged();
}

void StyleSheet::registerStaticBorderRule(StaticStyleSelectorKind selectorKind,
                                          const char *selector,
                                          StaticStyleLengthSpec width,
                                          int r,
                                          int g,
                                          int b,
                                          int a,
                                          const char *media)
{
	rules().push_back(makeStaticBorderCssRule(selectorKind, selector, width, r, g, b, a, media));
	noteStyleRuleRegistrationChanged();
}

void StyleSheet::registerStaticBorderRadiusRule(StaticStyleSelectorKind selectorKind,
                                                const char *selector,
                                                StaticStyleLengthSpec topLeft,
                                                StaticStyleLengthSpec topRight,
                                                StaticStyleLengthSpec bottomRight,
                                                StaticStyleLengthSpec bottomLeft,
                                                const char *media)
{
	rules().push_back(makeStaticBorderRadiusCssRule(selectorKind, selector, topLeft, topRight, bottomRight, bottomLeft, media));
	noteStyleRuleRegistrationChanged();
}

void StyleSheet::registerStaticBorderRadiusCornerRule(StaticStyleSelectorKind selectorKind,
                                                      const char *selector,
                                                      StaticStyleBorderRadiusCorner corner,
                                                      StaticStyleLengthSpec radius,
                                                      const char *media)
{
	rules().push_back(makeStaticBorderRadiusCornerCssRule(selectorKind, selector, corner, radius, media));
	noteStyleRuleRegistrationChanged();
}

void StyleSheet::registerStaticFilterBlurRule(StaticStyleSelectorKind selectorKind,
                                              const char *selector,
                                              StaticStyleLengthSpec radius,
                                              const char *media)
{
	rules().push_back(makeStaticFilterBlurCssRule(selectorKind, selector, radius, media));
	noteStyleRuleRegistrationChanged();
}

void StyleSheet::registerStaticBoxShadowNoneRule(StaticStyleSelectorKind selectorKind,
                                                 const char *selector,
                                                 const char *media)
{
	rules().push_back(makeStaticBoxShadowNoneCssRule(selectorKind, selector, media));
	noteStyleRuleRegistrationChanged();
}

void StyleSheet::registerStaticBackgroundRule(StaticStyleSelectorKind selectorKind,
                                              const char *selector,
                                              StaticStyleLinearGradient gradient,
                                              StaticStyleLinearGradient overlayGradient,
                                              bool hasOverlayGradient,
                                              StaticStyleBackgroundGridLine gridX,
                                              StaticStyleBackgroundGridLine gridY,
                                              const char *media)
{
	rules().push_back(makeStaticBackgroundCssRule(selectorKind,
	                                             selector,
	                                             gradient,
	                                             overlayGradient,
	                                             hasOverlayGradient,
	                                             gridX,
	                                             gridY,
	                                             media));
	noteStyleRuleRegistrationChanged();
}

void StyleSheet::registerStaticBackgroundFullRule(StaticStyleSelectorKind selectorKind,
                                                  const char *selector,
                                                  StaticStyleLinearGradientRef gradient,
                                                  StaticStyleLinearGradientRef overlayGradient,
                                                  bool hasOverlayGradient,
                                                  StaticStyleRadialGradientRef radialGradient,
                                                  StaticStyleBackgroundGridLine gridX,
                                                  StaticStyleBackgroundGridLine gridY,
                                                  const char *media)
{
	rules().push_back(makeStaticBackgroundFullCssRule(selectorKind,
	                                                 selector,
	                                                 gradient,
	                                                 overlayGradient,
	                                                 hasOverlayGradient,
	                                                 radialGradient,
	                                                 gridX,
	                                                 gridY,
	                                                 media));
	noteStyleRuleRegistrationChanged();
}

void StyleSheet::registerStaticBackgroundSizeRule(StaticStyleSelectorKind selectorKind,
                                                  const char *selector,
                                                  StaticStyleLengthSpec stepX,
                                                  StaticStyleLengthSpec stepY,
                                                  const char *media)
{
	rules().push_back(makeStaticBackgroundSizeCssRule(selectorKind, selector, stepX, stepY, media));
	noteStyleRuleRegistrationChanged();
}

void StyleSheet::registerStaticAnimationRule(StaticStyleSelectorKind selectorKind,
                                             const char *selector,
                                             const char *name,
                                             std::uint32_t durationMs,
                                             std::uint32_t delayMs,
                                             int iterations,
                                             StaticStyleAnimationDirection direction,
                                             StaticStyleAnimationFill fill,
                                             StaticStyleAnimationEasing easing,
                                             const char *media)
{
	rules().push_back(makeStaticAnimationCssRule(selectorKind,
	                                           selector,
	                                           name,
	                                           durationMs,
	                                           delayMs,
	                                           iterations,
	                                           direction,
	                                           fill,
	                                           easing,
	                                           media));
	noteStyleRuleRegistrationChanged();
}

void StyleSheet::registerStaticCustomLengthRule(StaticStyleSelectorKind selectorKind,
                                                const char *selector,
                                                const char *name,
                                                StaticStyleLengthSpec length,
                                                const char *media)
{
	rules().push_back(makeStaticCustomLengthCssRule(selectorKind, selector, name, length, media));
	noteStyleRuleRegistrationChanged();
}

void StyleSheet::registerStaticCustomColorRule(StaticStyleSelectorKind selectorKind,
                                               const char *selector,
                                               const char *name,
                                               int r,
                                               int g,
                                               int b,
                                               int a,
                                               const char *media)
{
	rules().push_back(makeStaticCustomColorCssRule(selectorKind, selector, name, r, g, b, a, media));
	noteStyleRuleRegistrationChanged();
}

void StyleSheet::registerStaticTransformRule(StaticStyleSelectorKind selectorKind,
                                             const char *selector,
                                             std::uint16_t flags,
                                             int rotateX,
                                             int rotateY,
                                             int rotateZ,
                                             StaticStyleLengthSpec translateX,
                                             StaticStyleLengthSpec translateY,
                                             StaticStyleLengthSpec translateZ,
                                             int scaleX,
                                             int scaleY,
                                             const char *media)
{
	rules().push_back(makeStaticTransformCssRule(selectorKind,
	                                            selector,
	                                            flags,
	                                            rotateX,
	                                            rotateY,
	                                            rotateZ,
	                                            translateX,
	                                            translateY,
	                                            translateZ,
	                                            scaleX,
	                                            scaleY,
	                                            media));
	{
		static const bool gTraceCube = std::getenv("GEA_DEBUG_CUBE") != nullptr;
		if (gTraceCube)
			std::printf("[cube] registerStaticTransformRule sel=%s flags=%u rx=%d ry=%d rz=%d tzUnit=%d tzVal=%.2f\n",
			            selector ? selector : "(null)", static_cast<unsigned>(flags), rotateX, rotateY, rotateZ,
			            static_cast<int>(translateZ.unit), static_cast<double>(translateZ.value));
	}
	noteStyleRuleRegistrationChanged();
}

void StyleSheet::registerStaticOriginRule(StaticStyleSelectorKind selectorKind,
                                          const char *selector,
                                          StaticStyleOriginProperty property,
                                          int xPermille,
                                          int yPermille,
                                          const char *media)
{
	rules().push_back(makeStaticOriginCssRule(selectorKind, selector, property, xPermille, yPermille, media));
	noteStyleRuleRegistrationChanged();
}

void StyleSheet::registerStaticGridTemplateRule(StaticStyleSelectorKind selectorKind,
                                                const char *selector,
                                                StaticStyleGridTemplateProperty property,
                                                std::initializer_list<StaticStyleGridTemplateTrack> tracks,
                                                const char *media)
{
	rules().push_back(makeStaticGridTemplateCssRule(selectorKind, selector, property, tracks, media));
	noteStyleRuleRegistrationChanged();
}

void StyleSheet::applyClass(NodeHandle node, const std::string &className) const
{
	if (!node) return;
	setClassName(node, className);
}

void StyleSheet::setClassName(NodeHandle node, const std::string &className) const
{
	if (!node) return;
	Tree::instance().setClassName(node.id(), className);
}

bool StyleSheet::applyNumberProperty(NodeHandle node, const char *property, double value) const
{
	return applyNumberPropertyWithSource(node, property, value, StyleApplicationSource::Inline);
}

bool StyleSheet::applyNumberProperty(NodeHandle node, StyleDeclaration declaration, double value) const
{
	return applyNumberDeclarationWithSource(node, declaration, value, StyleApplicationSource::Inline);
}

bool StyleSheet::removeProperty(NodeHandle node, const std::string &property) const
{
	return removeInlineStyleProperty(node, property);
}

void StyleSheet::applyProperty(NodeHandle node, StyleDeclaration declaration, const std::string &value) const
{
	applyPropertyWithSource(node, declaration, value, StyleApplicationSource::Inline);
}

void StyleSheet::applyProperty(NodeHandle node, const char *property, const std::string &value) const
{
	applyPropertyWithSource(node, property, value, StyleApplicationSource::Inline);
}

void StyleSheet::applyProperty(NodeHandle node, const std::string &property, const std::string &value) const
{
	applyPropertyWithSource(node, property, value, StyleApplicationSource::Inline);
}

void StyleSheet::recomputeSubtree(int nodeId) const
{
	recomputeSubtreeClassStyles(nodeId);
}

void StyleSheet::startCssAnimations(std::uint32_t nowMs) const
{
	rebuildRuleIndexIfNeeded();
	if (g_ruleIndex.animationRules.empty()) return;
	auto &state = treeState();
	for (int node = 0; node < state.nodeCount; ++node) {
		if (isDisplayNone(state.nodes[node].style)) continue;
		const CssAnimationSpec spec = animationSpecForNode(node);
		startAnimationForNode(node, spec, nowMs);
	}
}

void Tree::setClassName(int node, const std::string &className)
{
	setClassName(node, className.c_str());
}

void Tree::setClassName(int node, const char *className)
{
	auto &state = treeState();
	if (node < 0 || node >= state.nodeCount) return;
	// Capture the old classes before mutating, so the incremental recompute can tell
	// whether this change could flip a descendant-combinator match (added OR removed).
	const NodeClassSnapshot oldTokens(state.classLists[node]);
	const bool changed = state.classLists[node].set(className);
	if (!changed) return;
	noteClassMutationForIncremental(node, oldTokens);
	syncNodeClassAttribute(node);
	recomputeSubtreeClassStyles(node);
}

// Global tag-string table. A std::deque keeps element addresses stable across
// growth, so tagFromId can hand out c_str() pointers that stay valid. Index 0
// is the empty tag, matching Node::tag_id's zero default.
namespace {
std::deque<std::string> &tagTable()
{
	static std::deque<std::string> table = {std::string()};
	return table;
}
}  // namespace

int16_t internTag(const char *tag)
{
	const char *s = tag ? tag : "";
	auto &table = tagTable();
	for (std::size_t i = 0; i < table.size(); i++) {
		if (table[i] == s) return static_cast<int16_t>(i);
	}
	table.emplace_back(s);
	return static_cast<int16_t>(table.size() - 1);
}

const char *tagFromId(int16_t id)
{
	auto &table = tagTable();
	if (id < 0 || static_cast<std::size_t>(id) >= table.size()) return "";
	return table[static_cast<std::size_t>(id)].c_str();
}

// ---- RareStyle pool ---------------------------------------------------------
// Cold style fields, pooled and keyed by ComputedStyle::rare_style. The pool can
// keep a target-sized first tier in SRAM and spill to a deque; a free list keeps
// handles reused.
// rareStylePool() + rstyle() are now inline in node_model.h (hot read path). The
// free list stays here — only rstyleMut/releaseRareStyle/resetRareStylePool use it.
// When GEA_EMBEDDED_RARE_STYLE_INLINE is enabled, the rare fields are embedded in
// ComputedStyle, so these become trivial (rstyleMut returns the embedded struct;
// release/reset are no-ops) and the pool + free list are compiled out.
#if !GEA_EMBEDDED_RARE_STYLE_INLINE
namespace {
std::vector<int16_t> &rareStyleFreeList()
{
	static std::vector<int16_t> freeList;
	return freeList;
}
}  // namespace
#endif

RareStyle &rstyleMut(ComputedStyle &style)
{
	// rstyleMut is also used by native setup/tests that write transform fields
	// directly, bypassing Tree::setStyle's transform-cache invalidation.
	auto &state = treeState();
	state.transformScanSerial = ~0ull;
	state.transformScanValid = false;
#if GEA_EMBEDDED_RARE_STYLE_INLINE
	return style.rare;
#else
	auto &pool = rareStylePool();
	if (style.rare_style >= 0) return pool[static_cast<std::size_t>(style.rare_style)];
	int16_t handle;
	auto &freeList = rareStyleFreeList();
	if (!freeList.empty()) {
		handle = freeList.back();
		freeList.pop_back();
	} else {
		handle = static_cast<int16_t>(pool.size());
		pool.emplace_back();
	}
	style.rare_style = handle;
	return pool[static_cast<std::size_t>(handle)];
#endif
}

void releaseRareStyle(int16_t &handle)
{
#if GEA_EMBEDDED_RARE_STYLE_INLINE
	(void)handle;  // embedded rare is freed with the node; nothing to release
#else
	if (handle < 0) return;
	rareStylePool()[static_cast<std::size_t>(handle)] = RareStyle{};  // clear for reuse
	rareStyleFreeList().push_back(handle);
	handle = -1;
#endif
}

void resetRareStylePool()
{
#if GEA_EMBEDDED_RARE_STYLE_INLINE
	// rare fields are embedded per-node; there is no shared pool to reset.
#else
	rareStylePool().clear();
	rareStyleFreeList().clear();
#endif
}

void Tree::setTagName(int node, const char *tagName)
{
	auto &state = treeState();
	if (node < 0 || node >= state.nodeCount) return;
	const int16_t id = internTag(tagName ? tagName : "");
	if (state.nodes[node].tag_id == id) return;
	state.nodes[node].tag_id = id;
	recomputeSubtreeClassStyles(node);
}

const char *Tree::tagName(int node) const
{
	auto &state = treeState();
	if (node < 0 || node >= state.nodeCount) return "";
	return tagFromId(state.nodes[node].tag_id);
}

bool Tree::addClass(int node, const std::string &token)
{
	auto &state = treeState();
	if (node < 0 || node >= state.nodeCount) return false;
	const NodeClassSnapshot oldTokens(state.classLists[node]);
	if (!state.classLists[node].add(token)) return false;
	noteClassMutationForIncremental(node, oldTokens);
	syncNodeClassAttribute(node);
	recomputeSubtreeClassStyles(node);
	return true;
}

bool Tree::removeClass(int node, const std::string &token)
{
	auto &state = treeState();
	if (node < 0 || node >= state.nodeCount) return false;
	const NodeClassSnapshot oldTokens(state.classLists[node]);
	if (!state.classLists[node].remove(token)) return false;
	noteClassMutationForIncremental(node, oldTokens);
	syncNodeClassAttribute(node);
	recomputeSubtreeClassStyles(node);
	return true;
}

bool Tree::toggleClass(int node, const std::string &token)
{
	auto &state = treeState();
	if (node < 0 || node >= state.nodeCount) return false;
	const NodeClassSnapshot oldTokens(state.classLists[node]);
	const bool willContain = !state.classLists[node].contains(token);
	if (willContain) {
		if (!state.classLists[node].add(token)) return false;
	} else {
		if (!state.classLists[node].remove(token)) return false;
	}
	noteClassMutationForIncremental(node, oldTokens);
	syncNodeClassAttribute(node);
	recomputeSubtreeClassStyles(node);
	return willContain;
}

bool Tree::toggleClass(int node, const std::string &token, bool force)
{
	auto &state = treeState();
	if (node < 0 || node >= state.nodeCount) return false;
	const NodeClassSnapshot oldTokens(state.classLists[node]);
	const bool changed = force ? state.classLists[node].add(token) : state.classLists[node].remove(token);
	if (changed) {
		noteClassMutationForIncremental(node, oldTokens);
		syncNodeClassAttribute(node);
		recomputeSubtreeClassStyles(node);
	}
	return force ? state.classLists[node].contains(token) : false;
}

void Tree::clearClasses(int node)
{
	auto &state = treeState();
	if (node < 0 || node >= state.nodeCount) return;
	if (state.classLists[node].empty()) {
		syncNodeClassAttribute(node);
		return;
	}
	const NodeClassSnapshot oldTokens(state.classLists[node]);
	state.classLists[node].clear();
	noteClassMutationForIncremental(node, oldTokens);
	syncNodeClassAttribute(node);
	recomputeSubtreeClassStyles(node);
}

bool Tree::hasClass(int node, const std::string &token) const
{
	auto &state = treeState();
	return node >= 0 && node < state.nodeCount && state.classLists[node].contains(token);
}

std::string Tree::className(int node) const
{
	auto &state = treeState();
	if (node < 0 || node >= state.nodeCount) return std::string();
	return state.classLists[node].value();
}

void setViewportMetrics(int width, int height, double devicePixelRatio)
{
	g_viewport_width = width;
	g_viewport_height = height;
	g_device_pixel_ratio = sanitizedDevicePixelRatio(devicePixelRatio);
	clearStaticLengthExpressionResolutionCache();
	invalidateRuleIndex();
	recomputeAllClassStyles();
}

double devicePixelRatio()
{
	return g_device_pixel_ratio;
}

void setDevicePixelRatio(double devicePixelRatio)
{
	const double sanitized = sanitizedDevicePixelRatio(devicePixelRatio);
	if (sanitized == g_device_pixel_ratio) return;
	g_device_pixel_ratio = sanitized;
	clearStaticLengthExpressionResolutionCache();
	invalidateRuleIndex();
	recomputeAllClassStyles();
}

void setSafeAreaInsetBottom(int inset)
{
	g_safe_area_inset_bottom = inset > 0 ? inset : 0;
}

int safeAreaInsetBottom()
{
	return g_safe_area_inset_bottom;
}

// C ABI shims are kept for older generated code, but C++ code should call the
// namespaced functions above.
extern "C" void gea_style_set_viewport_metrics(int width, int height, double devicePixelRatio)
{
	gea::embedded::ui::setViewportMetrics(width, height, devicePixelRatio);
}

extern "C" double gea_style_get_device_pixel_ratio()
{
	return gea::embedded::ui::devicePixelRatio();
}

extern "C" void gea_style_set_device_pixel_ratio(double devicePixelRatio)
{
	gea::embedded::ui::setDevicePixelRatio(devicePixelRatio);
}

// Physical-pixel height reserved at the bottom of the panel for a
// platform-drawn overlay (geaos home button). Set once by the target;
// read by the virtual keyboard so it floats above the overlay instead
// of being painted over by it.
extern "C" void gea_style_set_safe_area_inset_bottom(int inset)
{
	gea::embedded::ui::setSafeAreaInsetBottom(inset);
}

extern "C" int gea_style_get_safe_area_inset_bottom()
{
	return gea::embedded::ui::safeAreaInsetBottom();
}

// Backward-compatible entry point for targets that have no DPR concept yet.
extern "C" void gea_style_set_viewport_size(int width, int height)
{
	gea::embedded::ui::setViewportMetrics(width, height, 1.0);
}

}  // namespace gea::embedded::ui

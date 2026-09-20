// SPDX-License-Identifier: Apache-2.0
#include "display_invalidation.h"
#include "internal.h"
#include "node_lifecycle.h"
#include "refresh_perf.h"
#include "style_values.h"
#include "tree_state.h"

// Skip the inline-style record for Left/Top (see setStyleValue). Default off: keep the
// record so static-authored absolute elements survive class-recompute. Opt in per-app
// for reactive-keyed-list-position apps (bouncing-balls-jsx) that re-apply every frame.
#ifndef GEA_EMBEDDED_SKIP_POSITION_INLINE_RECORD
#define GEA_EMBEDDED_SKIP_POSITION_INLINE_RECORD 0
#endif

namespace gea::embedded::ui {

namespace {

bool nodeParticipatesInMountedTree(const TreeState &state, int node)
{
	const int root = Tree::instance().mountedRoot();
	if (root < 0) return false;
	for (int current = node; current >= 0 && current < state.nodeCount; current = state.nodes[current].parent) {
		if (current == root) return true;
	}
	return false;
}

bool isTransformProperty(Property prop)
{
	switch (prop) {
	case Property::TransformRotate:
	case Property::TransformRotateX:
	case Property::TransformRotateY:
	case Property::TransformTranslateX:
	case Property::TransformTranslateY:
	case Property::TransformTranslateZ:
	case Property::TransformTranslateXPercent:
	case Property::TransformTranslateYPercent:
	case Property::TransformScaleX:
	case Property::TransformScaleY:
	case Property::TransformOriginX:
	case Property::TransformOriginY:
	case Property::Perspective:
	case Property::PerspectiveOriginX:
	case Property::PerspectiveOriginY:
		return true;
	default:
		return false;
	}
}

bool isLayoutProperty(Property prop)
{
	switch (prop) {
	case Property::Display:
	case Property::FlexDirection:
	case Property::FlexWrap:
	case Property::JustifyContent:
	case Property::AlignItems:
	case Property::JustifyItems:
	case Property::AlignContent:
	case Property::AlignSelf:
	case Property::Gap:
	case Property::Width:
	case Property::Height:
	case Property::WidthPercent:
	case Property::HeightPercent:
	case Property::MinWidth:
	case Property::MinHeight:
	case Property::MaxWidth:
	case Property::MaxHeight:
	case Property::Flex:
	case Property::FlexShrink:
	case Property::FlexBasis:
	case Property::PaddingTop:
	case Property::PaddingRight:
	case Property::PaddingBottom:
	case Property::PaddingLeft:
	case Property::MarginTop:
	case Property::MarginRight:
	case Property::MarginBottom:
	case Property::MarginLeft:
	case Property::Position:
	case Property::Top:
	case Property::Right:
	case Property::Bottom:
	case Property::Left:
	case Property::TopPercent:
	case Property::RightPercent:
	case Property::BottomPercent:
	case Property::LeftPercent:
	case Property::Overflow:
	case Property::OverflowX:
	case Property::OverflowY:
	case Property::FontId:
	case Property::FontSize:
	case Property::FontWeight:
	case Property::LineHeight:
	case Property::WhiteSpace:
	case Property::TextOverflow:
	case Property::ImageId:
	case Property::ImageFit:
		return true;
	default:
		return false;
	}
}

bool isPaintProperty(Property prop)
{
	return DisplayInvalidation::rebuildsNodeDisplayCommands(prop) && !isTransformProperty(prop);
}

struct ScopedRefreshStat
{
	std::int64_t start;
	std::int64_t &slot;

	explicit ScopedRefreshStat(std::int64_t &slot)
		: start(refreshPerfNowUs()), slot(slot)
	{
	}

	~ScopedRefreshStat()
	{
		slot += refreshPerfNowUs() - start;
	}
};

void setStyleValue(Tree &tree, int node, Property prop, int value, bool recordInline)
{
	auto &perf = refreshPerfStatsMutable();
	ScopedRefreshStat timer(perf.treeSetStyleUs);
	perf.treeSetStyleCalls++;
	auto &state = treeState();
	if (node < 0 || node >= state.nodeCount) return;
	// Record every authored inline property — including Left/Top — so it survives a
	// class-change / style recompute. resetStyleForClassRecompute() rebuilds a node's
	// style from its class rules + inlineStyles, so anything NOT recorded here is lost
	// on the next recompute. A static template that authors inline left/top exactly
	// once (e.g. an absolutely-positioned board: `style={{ left: BOARD_X, top: BOARD_Y }}`)
	// depends on this: previously Left/Top were skipped as a perf shortcut for the
	// reactive keyed-list position path (~128 writes/frame in bouncing-balls), which
	// pinned every statically-authored absolute element to (0,0) after the first
	// recompute while width/height (which WERE recorded) survived. The keyed-list path
	// re-applies its live left/top every frame, so a recompute that momentarily reverts
	// to the last-recorded value self-corrects on the next frame; the only cost there is
	// the per-write record.
	// Per-app opt (GEA_EMBEDDED_SKIP_POSITION_INLINE_RECORD): the inline-style record
	// exists only so authored styles survive a class-recompute. Apps whose Left/Top come
	// exclusively from a reactive keyed-list (bouncing-balls: ~128 writes/frame) re-apply
	// live position every frame, so a recompute self-corrects next frame — the record is
	// pure waste (~384µs/frame here). Skip Left/Top recording for those apps ONLY; keep it
	// for any app that authors static absolute left/top AND toggles classes.
#if GEA_EMBEDDED_SKIP_POSITION_INLINE_RECORD
	if (recordInline && prop != Property::Left && prop != Property::Top)
#else
	if (recordInline)
#endif
		ensureRareData(node).inlineStyles.set(prop, value);
	Node *n = &state.nodes[node];
	int changed = 0;
	int prevOpacity = -1;
	// These previous-bg values feed ONLY the BackgroundColor solid-recolor fast path
	// below (itself gated on prop == BackgroundColor). Reading them on every setStyle —
	// including the 4 pooled rstyle() bg-gradient lookups — is pure waste for the common
	// layout/paint writes (e.g. 128 left/top writes per frame in bouncing-balls = ~512
	// wasted pooled reads). Gate them on the property.
	const bool isBgColorChange = (prop == Property::BackgroundColor);
	const style_color_t previousBgColor = isBgColorChange ? n->style.bg_color : style_color_t{};
	const uint8_t previousHasBg = isBgColorChange ? n->style.has_bg : 0;
	const uint8_t previousBgAlpha = isBgColorChange ? n->style.bg_alpha : 0;
	const int8_t previousBgFill = isBgColorChange ? n->style.bg_fill : 0;
	const uint8_t previousBgGradientHasMid = isBgColorChange ? rstyle(n->style).bg_gradient_has_mid : 0;
	const uint8_t previousBgOverlayGradient = isBgColorChange ? rstyle(n->style).bg_overlay_gradient : 0;
	const uint8_t previousBgRadialGradient = isBgColorChange ? rstyle(n->style).bg_radial_gradient : 0;
	const uint8_t previousBgGridAxes = isBgColorChange ? rstyle(n->style).bg_grid_axes : 0;
	switch (prop) {
	case Property::Display:
		// Any application of the `display` property is explicit authoring — record
		// it so the inline-formatting heuristic treats e.g. `display:block` on a
		// <span> as block-level (stacks) rather than its default inline behaviour.
		if (!n->style.display_explicit) { n->style.display_explicit = 1; changed = 1; }
		if (n->style.display != value) { n->style.display = value; changed = 1; }
		break;
	case Property::FlexDirection:
		if (!n->style.flex_direction_explicit) {
			n->style.flex_direction_explicit = 1;
			changed = 1;
		}
		if (n->style.flex_direction != value) {
			n->style.flex_direction = value;
			changed = 1;
		}
		break;
	case Property::FlexWrap:        if (n->style.flex_wrap != value) { n->style.flex_wrap = value; changed = 1; } break;
	case Property::JustifyContent:  if (n->style.justify_content != value) { n->style.justify_content = value; changed = 1; } break;
	case Property::AlignItems:      if (n->style.align_items != value) { n->style.align_items = value; changed = 1; } break;
	case Property::JustifyItems:    if (n->style.justify_items != value) { n->style.justify_items = value; changed = 1; } break;
	case Property::AlignContent:    if (n->style.align_content != value) { n->style.align_content = value; changed = 1; } break;
	case Property::AlignSelf:       if (n->style.align_self != value) { n->style.align_self = value; changed = 1; } break;
	case Property::Gap:              if (n->style.gap != value) { n->style.gap = value; changed = 1; } break;
	case Property::Width:
		if (n->style.width != value || n->style.width_percent != kUnset) {
			n->style.width = value;
			n->style.width_percent = kUnset;
			changed = 1;
		}
		break;
	case Property::Height:
		if (n->style.height != value || n->style.height_percent != kUnset) {
			n->style.height = value;
			n->style.height_percent = kUnset;
			changed = 1;
		}
		break;
	case Property::WidthPercent:
		if (n->style.width_percent != value || n->style.width != kUnset) {
			n->style.width_percent = value;
			n->style.width = kUnset;
			changed = 1;
		}
		break;
	case Property::HeightPercent:
		if (n->style.height_percent != value || n->style.height != kUnset) {
			n->style.height_percent = value;
			n->style.height = kUnset;
			changed = 1;
		}
		break;
	case Property::MinWidth:        if (n->style.min_width != value) { n->style.min_width = value; changed = 1; } break;
	case Property::MinHeight:       if (n->style.min_height != value) { n->style.min_height = value; changed = 1; } break;
	case Property::MaxWidth:        if (n->style.max_width != value) { n->style.max_width = value; changed = 1; } break;
	case Property::MaxHeight:       if (n->style.max_height != value) { n->style.max_height = value; changed = 1; } break;
	case Property::Flex:             if (n->style.flex != value) { n->style.flex = value; changed = 1; } break;
	case Property::FlexShrink:       if (n->style.flex_shrink != value) { n->style.flex_shrink = value; changed = 1; } break;
	case Property::FlexBasis:        if (n->style.flex_basis != value) { n->style.flex_basis = value; changed = 1; } break;
	case Property::PaddingTop:      if (n->style.padding[0] != value) { n->style.padding[0] = value; changed = 1; } break;
	case Property::PaddingRight:    if (n->style.padding[1] != value) { n->style.padding[1] = value; changed = 1; } break;
	case Property::PaddingBottom:   if (n->style.padding[2] != value) { n->style.padding[2] = value; changed = 1; } break;
	case Property::PaddingLeft:     if (n->style.padding[3] != value) { n->style.padding[3] = value; changed = 1; } break;
	case Property::MarginTop:       if (n->style.margin[0] != value) { n->style.margin[0] = value; changed = 1; } break;
	case Property::MarginRight:     if (n->style.margin[1] != value) { n->style.margin[1] = value; changed = 1; } break;
	case Property::MarginBottom:    if (n->style.margin[2] != value) { n->style.margin[2] = value; changed = 1; } break;
	case Property::MarginLeft:      if (n->style.margin[3] != value) { n->style.margin[3] = value; changed = 1; } break;
	case Property::Position:         if (n->style.position != value) { n->style.position = value; changed = 1; } break;
	case Property::Top:
		if (n->style.pos_offsets[0] != value || n->style.pos_offset_percent[0] != kUnset) {
			n->style.pos_offsets[0] = value;
			n->style.pos_offset_percent[0] = kUnset;
			changed = 1;
		}
		break;
	case Property::Right:
		if (n->style.pos_offsets[1] != value || n->style.pos_offset_percent[1] != kUnset) {
			n->style.pos_offsets[1] = value;
			n->style.pos_offset_percent[1] = kUnset;
			changed = 1;
		}
		break;
	case Property::Bottom:
		if (n->style.pos_offsets[2] != value || n->style.pos_offset_percent[2] != kUnset) {
			n->style.pos_offsets[2] = value;
			n->style.pos_offset_percent[2] = kUnset;
			changed = 1;
		}
		break;
	case Property::Left:
		if (n->style.pos_offsets[3] != value || n->style.pos_offset_percent[3] != kUnset) {
			n->style.pos_offsets[3] = value;
			n->style.pos_offset_percent[3] = kUnset;
			changed = 1;
		}
		break;
	case Property::TopPercent:
		if (n->style.pos_offset_percent[0] != value || n->style.pos_offsets[0] != kUnset) {
			n->style.pos_offset_percent[0] = value;
			n->style.pos_offsets[0] = kUnset;
			changed = 1;
		}
		break;
	case Property::RightPercent:
		if (n->style.pos_offset_percent[1] != value || n->style.pos_offsets[1] != kUnset) {
			n->style.pos_offset_percent[1] = value;
			n->style.pos_offsets[1] = kUnset;
			changed = 1;
		}
		break;
	case Property::BottomPercent:
		if (n->style.pos_offset_percent[2] != value || n->style.pos_offsets[2] != kUnset) {
			n->style.pos_offset_percent[2] = value;
			n->style.pos_offsets[2] = kUnset;
			changed = 1;
		}
		break;
	case Property::LeftPercent:
		if (n->style.pos_offset_percent[3] != value || n->style.pos_offsets[3] != kUnset) {
			n->style.pos_offset_percent[3] = value;
			n->style.pos_offsets[3] = kUnset;
			changed = 1;
		}
		break;
	case Property::ZIndex:          if (n->style.z_index != value) { n->style.z_index = value; changed = 1; } break;
	case Property::BackgroundColor: {
		style_color_t next = StyleValues::pixelFromStyleValue(value);
		if (n->style.bg_color != next) { n->style.bg_color = next; changed = 1; }
		if (n->style.bg_alpha != 255) { n->style.bg_alpha = 255; changed = 1; }
		if (n->style.bg_fill != 0) { n->style.bg_fill = 0; changed = 1; }
		if (rstyle(n->style).bg_gradient_has_mid != 0) { rstyleMut(n->style).bg_gradient_has_mid = 0; changed = 1; }
		if (rstyle(n->style).bg_overlay_gradient != 0) { rstyleMut(n->style).bg_overlay_gradient = 0; changed = 1; }
		if (rstyle(n->style).bg_radial_gradient != 0) { rstyleMut(n->style).bg_radial_gradient = 0; changed = 1; }
		if (rstyle(n->style).bg_grid_axes != 0) { rstyleMut(n->style).bg_grid_axes = 0; changed = 1; }
		break;
	}
	case Property::HasBackground:           if (n->style.has_bg != value) { n->style.has_bg = value; changed = 1; } break;
	case Property::ActiveBackgroundColor: {
		style_color_t next = StyleValues::pixelFromStyleValue(value);
		if (n->style.active_bg_color != next) { n->style.active_bg_color = next; changed = 1; }
		break;
	}
	case Property::HasActiveBackground:    if (n->style.has_active_bg != value) { n->style.has_active_bg = value; changed = 1; } break;
	case Property::Color: {
		style_color_t next = StyleValues::pixelFromStyleValue(value);
		if (n->style.text_color != next) { n->style.text_color = next; changed = 1; }
		if (n->style.text_alpha != 255) { n->style.text_alpha = 255; changed = 1; }
		break;
	}
	case Property::Opacity: {
		uint8_t next = (uint8_t)value;
		if (n->style.opacity != next) { prevOpacity = n->style.opacity; n->style.opacity = next; changed = 1; }
		break;
	}
	case Property::BlinkInterval:
		value = value > 0 ? value : 0;
		if (n->style.blink_interval_ms != value) {
			n->style.blink_interval_ms = value;
			n->style.blink_started_ms = state.lastFrameMs;
			n->style.blink_visible = 1;
			changed = 1;
		}
		break;
	case Property::BorderWidth:     if (n->style.border_width != value) { n->style.border_width = value; changed = 1; } break;
	case Property::BorderColor: {
		style_color_t next = StyleValues::pixelFromStyleValue(value);
		if (n->style.border_color != next) { n->style.border_color = next; changed = 1; }
		if (n->style.border_alpha != 255) { n->style.border_alpha = 255; changed = 1; }
		break;
	}
	case Property::BorderTopWidth: if (rstyle(n->style).border_side_width[0] != value) { rstyleMut(n->style).border_side_width[0] = value; changed = 1; } break;
	case Property::BorderRightWidth: if (rstyle(n->style).border_side_width[1] != value) { rstyleMut(n->style).border_side_width[1] = value; changed = 1; } break;
	case Property::BorderBottomWidth: if (rstyle(n->style).border_side_width[2] != value) { rstyleMut(n->style).border_side_width[2] = value; changed = 1; } break;
	case Property::BorderLeftWidth: if (rstyle(n->style).border_side_width[3] != value) { rstyleMut(n->style).border_side_width[3] = value; changed = 1; } break;
	case Property::BorderTopColor: {
		style_color_t next = StyleValues::pixelFromStyleValue(value);
		if (rstyle(n->style).border_side_color[0] != next) { rstyleMut(n->style).border_side_color[0] = next; changed = 1; }
		if (rstyle(n->style).border_side_alpha[0] != 255) { rstyleMut(n->style).border_side_alpha[0] = 255; changed = 1; }
		break;
	}
	case Property::BorderRightColor: {
		style_color_t next = StyleValues::pixelFromStyleValue(value);
		if (rstyle(n->style).border_side_color[1] != next) { rstyleMut(n->style).border_side_color[1] = next; changed = 1; }
		if (rstyle(n->style).border_side_alpha[1] != 255) { rstyleMut(n->style).border_side_alpha[1] = 255; changed = 1; }
		break;
	}
	case Property::BorderBottomColor: {
		style_color_t next = StyleValues::pixelFromStyleValue(value);
		if (rstyle(n->style).border_side_color[2] != next) { rstyleMut(n->style).border_side_color[2] = next; changed = 1; }
		if (rstyle(n->style).border_side_alpha[2] != 255) { rstyleMut(n->style).border_side_alpha[2] = 255; changed = 1; }
		break;
	}
	case Property::BorderLeftColor: {
		style_color_t next = StyleValues::pixelFromStyleValue(value);
		if (rstyle(n->style).border_side_color[3] != next) { rstyleMut(n->style).border_side_color[3] = next; changed = 1; }
		if (rstyle(n->style).border_side_alpha[3] != 255) { rstyleMut(n->style).border_side_alpha[3] = 255; changed = 1; }
		break;
	}
	case Property::BorderRadiusTopLeft:
		if (n->style.border_radius[0] != value || n->style.border_radius_percent[0] != kUnset) {
			n->style.border_radius[0] = value;
			n->style.border_radius_percent[0] = kUnset;
			changed = 1;
		}
		break;
	case Property::BorderRadiusTopRight:
		if (n->style.border_radius[1] != value || n->style.border_radius_percent[1] != kUnset) {
			n->style.border_radius[1] = value;
			n->style.border_radius_percent[1] = kUnset;
			changed = 1;
		}
		break;
	case Property::BorderRadiusBottomRight:
		if (n->style.border_radius[2] != value || n->style.border_radius_percent[2] != kUnset) {
			n->style.border_radius[2] = value;
			n->style.border_radius_percent[2] = kUnset;
			changed = 1;
		}
		break;
	case Property::BorderRadiusBottomLeft:
		if (n->style.border_radius[3] != value || n->style.border_radius_percent[3] != kUnset) {
			n->style.border_radius[3] = value;
			n->style.border_radius_percent[3] = kUnset;
			changed = 1;
		}
		break;
	case Property::BorderRadiusTopLeftPercent:
		if (n->style.border_radius_percent[0] != value || n->style.border_radius[0] != 0) {
			n->style.border_radius_percent[0] = value;
			n->style.border_radius[0] = 0;
			changed = 1;
		}
		break;
	case Property::BorderRadiusTopRightPercent:
		if (n->style.border_radius_percent[1] != value || n->style.border_radius[1] != 0) {
			n->style.border_radius_percent[1] = value;
			n->style.border_radius[1] = 0;
			changed = 1;
		}
		break;
	case Property::BorderRadiusBottomRightPercent:
		if (n->style.border_radius_percent[2] != value || n->style.border_radius[2] != 0) {
			n->style.border_radius_percent[2] = value;
			n->style.border_radius[2] = 0;
			changed = 1;
		}
		break;
	case Property::BorderRadiusBottomLeftPercent:
		if (n->style.border_radius_percent[3] != value || n->style.border_radius[3] != 0) {
			n->style.border_radius_percent[3] = value;
			n->style.border_radius[3] = 0;
			changed = 1;
		}
		break;
	case Property::FontId:          if (n->style.font_id != value) { n->style.font_id = value; changed = 1; } break;
	case Property::FontSize:        if (n->style.font_size != value) { n->style.font_size = value; changed = 1; } break;
	case Property::FontWeight:      if (n->style.font_weight != value) { n->style.font_weight = value; changed = 1; } break;
	case Property::LineHeight:      if (n->style.line_height != value) { n->style.line_height = value; changed = 1; } break;
	case Property::TextAlign:       if (n->style.text_align != value) { n->style.text_align = value; changed = 1; } break;
	case Property::TextDecoration:  if (n->style.text_decoration != value) { n->style.text_decoration = value; changed = 1; } break;
	case Property::TextTransform:   if (n->style.text_transform != value) { n->style.text_transform = value; changed = 1; } break;
	case Property::WhiteSpace:      if (n->style.white_space != static_cast<int8_t>(value)) { n->style.white_space = static_cast<int8_t>(value); changed = 1; } break;
	case Property::TextOverflow:    if (n->style.text_overflow != static_cast<int8_t>(value)) { n->style.text_overflow = static_cast<int8_t>(value); changed = 1; } break;
	case Property::Backface:        if (n->style.backface_hidden != static_cast<int8_t>(value)) { n->style.backface_hidden = static_cast<int8_t>(value); changed = 1; } break;
	case Property::PointerEvents:   if (n->style.pointer_events != static_cast<int8_t>(value)) { n->style.pointer_events = static_cast<int8_t>(value); changed = 1; } break;
	case Property::Overflow: {
		const int8_t next = static_cast<int8_t>(value);
		if (n->style.overflow != next || n->style.overflow_x != next || n->style.overflow_y != next) {
			n->style.overflow = next;
			n->style.overflow_x = next;
			n->style.overflow_y = next;
			changed = 1;
		}
		break;
	}
	case Property::OverflowX: {
		const int8_t next = static_cast<int8_t>(value);
		const int8_t aggregate = aggregateOverflow(next, n->style.overflow_y);
		if (n->style.overflow_x != next || n->style.overflow != aggregate) {
			n->style.overflow_x = next;
			n->style.overflow = aggregate;
			changed = 1;
		}
		break;
	}
	case Property::OverflowY: {
		const int8_t next = static_cast<int8_t>(value);
		const int8_t aggregate = aggregateOverflow(n->style.overflow_x, next);
		if (n->style.overflow_y != next || n->style.overflow != aggregate) {
			n->style.overflow_y = next;
			n->style.overflow = aggregate;
			changed = 1;
		}
		break;
	}
	case Property::MaskRightFadeWidth: {
		const int16_t next = static_cast<int16_t>(value < 0 ? 0 : value > 32767 ? 32767 : value);
		if (n->style.mask_right_fade_width != next) {
			n->style.mask_right_fade_width = next;
			changed = 1;
		}
		break;
	}
	case Property::ImageId:         if (n->image_id != value) { n->image_id = value; changed = 1; } break;
	case Property::ImageFit:        if (n->style.image_fit != value) { n->style.image_fit = value; changed = 1; } break;
	case Property::TransformRotate:
		if (rstyle(n->style).transform_rotate != value) {
			rstyleMut(n->style).transform_rotate = value;
			changed = 1;
		}
		break;
	case Property::TransformRotateX: if (rstyle(n->style).transform_rotate_x != value) { rstyleMut(n->style).transform_rotate_x = value; changed = 1; } break;
	case Property::TransformRotateY: if (rstyle(n->style).transform_rotate_y != value) { rstyleMut(n->style).transform_rotate_y = value; changed = 1; } break;
	case Property::TransformTranslateX: if (rstyle(n->style).transform_translate_x != value) { rstyleMut(n->style).transform_translate_x = value; changed = 1; } break;
	case Property::TransformTranslateY: if (rstyle(n->style).transform_translate_y != value) { rstyleMut(n->style).transform_translate_y = value; changed = 1; } break;
	case Property::TransformTranslateZ: if (rstyle(n->style).transform_translate_z != value) { rstyleMut(n->style).transform_translate_z = value; changed = 1; } break;
	case Property::TransformTranslateXPercent: if (rstyle(n->style).transform_translate_x_percent != value) { rstyleMut(n->style).transform_translate_x_percent = value; changed = 1; } break;
	case Property::TransformTranslateYPercent: if (rstyle(n->style).transform_translate_y_percent != value) { rstyleMut(n->style).transform_translate_y_percent = value; changed = 1; } break;
	case Property::TransformScaleX: if (rstyle(n->style).transform_scale_x != value) { rstyleMut(n->style).transform_scale_x = value; changed = 1; } break;
	case Property::TransformScaleY: if (rstyle(n->style).transform_scale_y != value) { rstyleMut(n->style).transform_scale_y = value; changed = 1; } break;
	case Property::TransformOriginX: if (rstyle(n->style).transform_origin_x != value) { rstyleMut(n->style).transform_origin_x = value; changed = 1; } break;
	case Property::TransformOriginY: if (rstyle(n->style).transform_origin_y != value) { rstyleMut(n->style).transform_origin_y = value; changed = 1; } break;
	case Property::Perspective: if (rstyle(n->style).perspective != value) { rstyleMut(n->style).perspective = value; changed = 1; } break;
	case Property::PerspectiveOriginX: if (rstyle(n->style).perspective_origin_x != value) { rstyleMut(n->style).perspective_origin_x = value; changed = 1; } break;
	case Property::PerspectiveOriginY: if (rstyle(n->style).perspective_origin_y != value) { rstyleMut(n->style).perspective_origin_y = value; changed = 1; } break;
	case Property::FilterBlur: if (rstyle(n->style).filter_blur_radius != value) { rstyleMut(n->style).filter_blur_radius = value; changed = 1; } break;
	case Property::BoxShadowInset: {
		const uint8_t next = value != 0 ? 1 : 0;
		if (rstyle(n->style).box_shadow_inset != next) { rstyleMut(n->style).box_shadow_inset = next; changed = 1; }
		break;
	}
	case Property::BoxShadowOffsetX: if (rstyle(n->style).box_shadow_offset_x != value) { rstyleMut(n->style).box_shadow_offset_x = value; changed = 1; } break;
	case Property::BoxShadowOffsetY: if (rstyle(n->style).box_shadow_offset_y != value) { rstyleMut(n->style).box_shadow_offset_y = value; changed = 1; } break;
	case Property::BoxShadowBlur: if (rstyle(n->style).box_shadow_blur_radius != value) { rstyleMut(n->style).box_shadow_blur_radius = value; changed = 1; } break;
	case Property::BoxShadowSpread: if (rstyle(n->style).box_shadow_spread != value) { rstyleMut(n->style).box_shadow_spread = value; changed = 1; } break;
	case Property::BoxShadowColor: {
		const style_color_t next = StyleValues::pixelFromStyleValue(value);
		if (rstyle(n->style).box_shadow_color != next) { rstyleMut(n->style).box_shadow_color = next; changed = 1; }
		break;
	}
	case Property::BoxShadowAlpha: {
		const uint8_t next = static_cast<uint8_t>(value < 0 ? 0 : value > 255 ? 255 : value);
		if (rstyle(n->style).box_shadow_alpha != next) { rstyleMut(n->style).box_shadow_alpha = next; changed = 1; }
		break;
	}
	default:
		perf.treeSetStyleNoop++;
		return;
	}
	if (!changed) {
		perf.treeSetStyleNoop++;
		return;
	}
	perf.treeSetStyleChanged++;
	if (isLayoutProperty(prop))
		perf.treeSetStyleLayoutChanged++;
	if (isTransformProperty(prop))
		perf.treeSetStyleTransformChanged++;
	else if (isPaintProperty(prop))
		perf.treeSetStylePaintChanged++;
	if (state.styleInvalidationSuppressionDepth > 0) return;
	if (!nodeParticipatesInMountedTree(state, node)) return;
	// An ImageId (src) swap on an element with a FIXED CSS display box (explicit
	// width+height — e.g. a forecast row's 24x20 icon re-pointed at a different icon
	// every data refresh) is PAINT-ONLY: the layout box is the styled box regardless
	// of the new image's intrinsic size, so no reflow and no sibling movement. Patch
	// just this node's BlitImage command in place and repaint its box; DON'T set
	// layout_dirty or displayListDirty (ImageId is otherwise treated as a layout
	// property, which would force a full relayout + full display-list rebuild + a
	// full-screen-coalesced re-raster — measured as the bulk of a ~350ms weather city
	// switch's 18 icon swaps). An auto-sized <img> still falls through to the full
	// path below, since its intrinsic size can move layout.
	if (prop == Property::ImageId) {
		const bool fixedBox =
			(n->style.width != kUnset || n->style.width_percent != kUnset) &&
			(n->style.height != kUnset || n->style.height_percent != kUnset);
		if (fixedBox) {
			n->render.dirty = 1;
			n->render.non_scroll_dirty = 1;
			tree.markNodeDisplayCommandsDirty(node);
			return;
		}
	}
	if (prop == Property::BackgroundColor &&
	    previousHasBg &&
	    previousBgAlpha == 255 &&
	    previousBgFill == 0 &&
	    previousBgGradientHasMid == 0 &&
	    previousBgOverlayGradient == 0 &&
	    previousBgRadialGradient == 0 &&
	    previousBgGridAxes == 0 &&
	    n->style.has_bg &&
	    n->style.bg_alpha == 255 &&
	    n->style.bg_fill == 0 &&
	    rstyle(n->style).bg_gradient_has_mid == 0 &&
	    rstyle(n->style).bg_overlay_gradient == 0 &&
	    rstyle(n->style).bg_radial_gradient == 0 &&
	    rstyle(n->style).bg_grid_axes == 0 &&
	    previousBgColor != n->style.bg_color) {
		n->render.bg_recolor_pending = 1;
		n->render.bg_recolor_from = previousBgColor;
		n->render.bg_recolor_to = n->style.bg_color;
	}
	n->render.dirty = 1;
	// Paint-only properties (background, colors, shadows, ...) repaint in
	// place: geometry is untouched, so they don't demand a relayout pass.
	// Layout and transform properties keep the conservative marking.
	if (isLayoutProperty(prop) || isTransformProperty(prop))
		n->render.layout_dirty = 1;
	n->render.non_scroll_dirty = 1;
	if (isTransformProperty(prop)) {
		n->render.transform_dirty = 1;
		state.transformScanSerial = ~0ull;
		state.transformScanValid = false;  // a transform was added/changed → drop durable no-transform cache
	}
	if (DisplayInvalidation::rebuildsNodeDisplayCommands(prop)) {
		const bool stayLocal = DisplayInvalidation::nodeDisplayChangeCanStayLocal(node);
		bool forceFull = false;
		if (prop == Property::Display) {
			forceFull = !stayLocal;
		} else if (prop == Property::Opacity) {
			// Partial→partial fade: the SetAlpha scope already exists on both
			// sides (opacity 0 still records a scope), so patch its value in place
			// and keep the display list — no rebuild, no displayListDirty. The
			// full per-rect replay path (tree_render) replays the scope in order,
			// so the patched alpha applies correctly. (The incremental
			// direct-replay path can't use this: it replays only a node's
			// [drawStart,drawEnd], which excludes the scope — which is why
			// canReplayDirectDirtyRegions still bails and we fall to the safe
			// full-per-rect replay. The record stays incremental either way.)
			if (stayLocal && prevOpacity < 255 && n->style.opacity < 255 &&
			    DisplayList::instance().patchNodeAlpha(node, static_cast<uint8_t>(n->style.opacity)))
				return;
			// Crossing the 255 boundary (scope appears/disappears) or a non-leaf:
			// a leaf toggling strictly 0↔255 re-records locally; otherwise rebuild.
			const bool toggleOnly = (prevOpacity == 0 || prevOpacity == 255) &&
			                        (n->style.opacity == 0 || n->style.opacity == 255);
			forceFull = !stayLocal || !toggleOnly;
		} else if (prop == Property::MaskRightFadeWidth ||
		           prop == Property::FilterBlur ||
		           prop == Property::BoxShadowInset ||
		           prop == Property::BoxShadowOffsetX ||
		           prop == Property::BoxShadowOffsetY ||
		           prop == Property::BoxShadowBlur ||
		           prop == Property::BoxShadowSpread ||
		           prop == Property::BoxShadowColor ||
		           prop == Property::BoxShadowAlpha) {
			forceFull = true;
		}
		if (forceFull) {
			// A partial-opacity change only alters this node's own pixels — no
			// reflow, no draw-order change — so the rebuilt list can be replayed
			// over just the dirty-node region (content-dirty skips the
			// full-viewport repaint). Display toggles (visibility/order) and
			// filter/box-shadow (pixel spillover past the node box) keep the
			// conservative full repaint.
			if (prop == Property::Opacity)
				tree.markDisplayListContentDirty();
			else
				tree.markDisplayListDirty();
		} else {
			tree.markNodeDisplayCommandsDirty(node);
		}
	} else if (!DisplayInvalidation::retainsDisplayCommands(prop)) {
		// An absolute, childless leaf changing size is out of flow: no sibling
		// reflow, no draw-order change. Mark only this node's commands dirty (so
		// it re-records at the new size) and let AbsoluteLeafRefresh reconcile
		// layout.{width,height} from style — no displayListDirty, so the display
		// list stays keepable and only this node re-records (the static text
		// labels are NOT re-recorded). Every other size/layout/z-index change
		// reflows or reorders → full rebuild.
		const bool absLeafSize = (prop == Property::Width || prop == Property::Height) &&
		                         n->style.position == 1 && n->first_child < 0 && n->type != NodeType::Text;
		if (absLeafSize)
			tree.markNodeDisplayCommandsDirty(node);
		else
			tree.markDisplayListDirty();
	}
}

}  // namespace

void Tree::setStyle(int node, Property prop, int value)
{
	setStyleValue(*this, node, prop, value, true);
}

void Tree::setDefaultStyle(int node, Property prop, int value)
{
	auto &state = treeState();
	if (node < 0 || node >= state.nodeCount) return;
	ensureRareData(node).defaultStyles.set(prop, value);
	StyleSheet::instance().recomputeSubtree(node);
}

void Tree::setStyleFromClass(int node, Property prop, int value)
{
	setStyleValue(*this, node, prop, value, false);
}

void Tree::resetStyleForClassRecompute(int node)
{
	auto &state = treeState();
	if (node < 0 || node >= state.nodeCount) return;

	Node defaults;
	NodeLifecycle::init(&defaults, state.nodes[node].type);
	state.nodes[node].style = defaults.style;
	state.nodes[node].image_id = defaults.image_id;
	if (state.styleInvalidationSuppressionDepth > 0) return;
	if (!nodeParticipatesInMountedTree(state, node)) return;
	state.nodes[node].render.dirty = 1;
	state.nodes[node].render.layout_dirty = 1;
	state.nodes[node].render.non_scroll_dirty = 1;
	markDisplayListDirty();
}

}  // namespace gea::embedded::ui

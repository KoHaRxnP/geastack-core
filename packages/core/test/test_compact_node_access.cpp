// SPDX-License-Identifier: Apache-2.0
// Architecture probe only: deliberately NOT wired into the engine. This makes
// the unavoidable extra base load explicit before changing hot Node storage.
#include "ui/node_model.h"
using namespace gea::embedded::ui;
namespace {
struct CompactAux {
  // Accessed once at entry to a layout/scroll operation, then direct members.
  std::int32_t scroll_x, scroll_y, content_width, content_height;
  std::int32_t previous_scroll_x, previous_scroll_y;
  std::int16_t memo_avail_w, memo_avail_h, memo_result_w, memo_result_h;
  std::uint16_t memo_pass;
  std::int16_t memo2_avail_w, memo2_avail_h;
  std::uint16_t memo2_pass;
  std::int16_t inline_indent, static_block_start;
  std::uint8_t static_block_axis;
  style_color_t recolor_from, recolor_to;
  std::int16_t text_dirty_x0, text_dirty_x1;
};
struct CompactNodeProjection {
  const ComputedStyle *style;
  CompactAux *aux;
  NodeText text;
  std::int16_t parent, first_child, last_child, next_sibling, prev_sibling;
  std::int16_t rare_data, image_id, tag_id;
  std::int16_t x, y, width, height;
  std::int16_t previous_x, previous_y, previous_width, previous_height;
  std::uint8_t dirty, scroll_dirty, non_scroll_dirty, layout_dirty;
  std::uint8_t bg_recolor_pending, text_layout_stable, text_partial_dirty, inline_baseline;
  NodeType type;
};
inline int styleGroup(const ComputedStyle &s) {
  return s.width + s.height + s.min_height + s.max_height + s.gap +
    s.flex + s.flex_shrink + s.font_size;
}
}
extern "C" {
char compact_probe_base_bytes[sizeof(CompactNodeProjection)];
char compact_probe_aux_bytes[sizeof(CompactAux)];
char compact_probe_style_bytes[sizeof(ComputedStyle)];
__attribute__((noinline)) int compact_probe_inline_group(const Node *n) {
  return styleGroup(n->style);
}
__attribute__((noinline)) int compact_probe_shared_group(const CompactNodeProjection *n) {
  const auto &style = *n->style;
  return styleGroup(style);
}
__attribute__((noinline)) int compact_probe_inline_one(const Node *n) { return n->style.width; }
__attribute__((noinline)) int compact_probe_shared_one(const CompactNodeProjection *n) { return n->style->width; }
}

// SPDX-License-Identifier: Apache-2.0
#include "graphics/font.h"
#include "native_test_harness.h"
#include "ui/document.h"
#include "ui/node.h"
#include "ui/node_lifecycle.h"
#include "ui/style.h"
#include "ui/tree_internal.h"
#include "ui/tree_state.h"
#include <cassert>
#include <cstdio>
#include <string>
#include <utility>
namespace gea::framework::app::generated {
void drainMicrotasks() {}
} // namespace gea::framework::app::generated
namespace gea::framework::graphics::generated {
void ensureLinked() {}
const RasterizedFontData *lookupFontForFamily(int, int) { return nullptr; }
} // namespace gea::framework::graphics::generated
using namespace gea::embedded::ui;
using namespace gea::embedded::test;
int main() {
  // Assignment to a potentially-overlapping member must preserve its neighbour.
  // Exercise copy, move, reset and vector growth on the actual public records.
  Node node{};
  node.tag_id = 37;
  node.type = NodeType::Text;
  node.parent = 19;
  node.rare_data = 23;
  node.render.dirty = 1;
  for (int round = 0; round < 40; ++round) {
    ComputedStyle style{};
    style.width = 12 + round;
    node.style = style;
    assert(node.tag_id == 37 && node.parent == 19 && node.rare_data == 23);
    assert(node.style.width == 12 + round);
    node.style = std::move(style);
    assert(node.tag_id == 37 && node.parent == 19 && node.rare_data == 23);
    node.style = ComputedStyle{};
    assert(node.tag_id == 37 && node.parent == 19 && node.rare_data == 23);
    LayoutBox layout{};
    layout.static_block_axis = 2;
    layout.memo_pass = round + 1;
    layout.width = 40 + round;
    node.layout = layout;
    assert(node.type == NodeType::Text && node.render.dirty == 1);
    assert(node.layout.static_block_axis == 2 &&
           node.layout.width == 40 + round);
    node.layout = std::move(layout);
    assert(node.type == NodeType::Text && node.render.dirty == 1);
    node.layout = LayoutBox{};
    assert(node.type == NodeType::Text && node.render.dirty == 1);
    node.text = "persistent text";
    node.layout.width = 173;
    node.style.width = 81;
    node.style.font_weight = 700;
    NodeLifecycle::resetStyle(node.style);
    assert(node.tag_id == 37 && node.type == NodeType::Text &&
           node.parent == 19 && node.rare_data == 23);
    assert(node.text == "persistent text" && node.layout.width == 173 &&
           node.render.dirty == 1);
    assert(node.style.width == kUnset && node.style.font_weight == 400 &&
           node.style.font_id == -1 && node.style.flex_shrink == 1 &&
           node.style.line_height_multiplier == -1 &&
           node.style.rare_style == -1);
    Node copied = node;
    assert(copied.tag_id == 37 && copied.type == NodeType::Text &&
           copied.parent == 19);
    Node moved = std::move(copied);
    assert(moved.tag_id == 37 && moved.type == NodeType::Text &&
           moved.rare_data == 23);
  }
  node = Node{};
  assert(node.tag_id == 0 && node.rare_data == -1);
  NodeRareData rare;
  for (int round = 0; round < 40; ++round) {
    rare.inlineStaticPosition = {123, -45, true, true};
    NodeEventListeners source;
    source.entries.resize(round + 1);
    source.types =
        0; // Synthetic entries do not register event dispatch counts.
    rare.listeners = source;
    assert(rare.inlineStaticPosition.x == 123 &&
           rare.inlineStaticPosition.y == -45);
    assert(rare.inlineStaticPosition.continuationLine &&
           rare.inlineStaticPosition.valid);
    rare.listeners = std::move(source);
    assert(rare.inlineStaticPosition.x == 123 &&
           rare.inlineStaticPosition.y == -45);
    rare.listeners = NodeEventListeners{};
    assert(rare.inlineStaticPosition.continuationLine &&
           rare.inlineStaticPosition.valid);
    rare.clear();
    assert(!rare.inlineStaticPosition.valid && rare.listeners.entries.empty());
  }
  // Long compound/ancestor selectors force both inline and spilled lists, then
  // rule/plan growth moves them repeatedly. Pseudo flags must survive all
  // moves.
  for (int round = 0; round < 3; ++round) {
    resetNativeHost();
    auto &sheet = StyleSheet::instance();
    sheet.clear();
    setViewportMetrics(160, 240, 1);
    auto root = Document::instance().createView();
    root.classList().set("root");
    auto parent = root;
    std::string descendant = ".root", direct = ".root";
    for (int i = 0; i < 6; ++i) {
      auto child = Document::instance().createView();
      const auto cls = "branch-" + std::to_string(i);
      child.classList().set(cls);
      parent.appendChild(child);
      parent = child;
      descendant += " ." + cls;
      direct += " > ." + cls;
    }
    auto first = Document::instance().createView(),
         last = Document::instance().createView();
    first.classList().set("leaf a b c d e f");
    last.classList().set("leaf a b c d e f");
    first.setAttribute("id", "chosen");
    parent.appendChild(first);
    parent.appendChild(last);
    sheet.registerElementRule("div", "height", "12px");
    sheet.registerRule("leaf", "width", "13px");
    sheet.registerSelectorRule(descendant + " .leaf.a.b.c.d.e.f", "width",
                               "73px");
    sheet.registerSelectorRule(direct + " > .leaf.a.b.c.d.e.f", "width",
                               "75px");
    sheet.registerSelectorRule("#chosen", "height", "51px");
    sheet.registerSelectorRule(":root", "padding", "2px");
    sheet.registerSelectorRule(".leaf:first-child", "border-width", "3px");
    sheet.registerSelectorRule(".leaf:last-child", "border-width", "5px");
    sheet.registerSelectorRule(".root .missing .leaf", "width", "99px");
    sheet.registerSelectorRule(".leaf:unknown-pseudo", "width", "98px");
    sheet.registerSelectorRule(".leaf:hover", "width", "97px");
    for (int i = 0; i < 180; ++i)
      sheet.registerSelectorRule(".absent-" + std::to_string(i) +
                                     " .unused.a.b.c.d.e.f",
                                 "width", "96px");
    Tree::instance().mount(root.id(), 160, 240);
    Tree::instance().computeLayout(root.id(), 160, 240);
    const auto &a = Tree::instance().node(first.id()).style;
    const auto &b = Tree::instance().node(last.id()).style;
    assert(a.width == 75 && b.width == 75 && a.height == 51);
    assert(a.border_width == 3 && b.border_width == 5);
    assert(Tree::instance().node(root.id()).style.padding[0] == 2);
    std::printf("selectors %d: widths=%d/%d height=%d borders=%d/%d\n", round,
                a.width, b.width, a.height, a.border_width, b.border_width);
    auto clone = first.cloneNode(false);
    parent.appendChild(clone);
    Document::instance().refresh(root, 160, 240);
    assert(Tree::instance().node(clone.id()).style.width == 75);
    assert(Tree::instance().node(clone.id()).style.border_width == 5);
  }
  std::puts("PASS: overlapping record assignment, inline/spilled selectors and "
            "clone state");
}

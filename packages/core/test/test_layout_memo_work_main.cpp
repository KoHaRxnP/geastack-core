// SPDX-License-Identifier: Apache-2.0
// Compare layout work and geometry for nested flex measurement/resize patterns.
#include "native_test_harness.h"
#include "graphics/font.h"
#include "ui/document.h"
#include "ui/internal.h"
#include "ui/refresh_perf.h"
#include "ui/tree_internal.h"
#include <cassert>
#include <cstdio>
#include <cstdint>
namespace gea::framework::app::generated { void drainMicrotasks() {} }
namespace gea::framework::graphics::generated {
void ensureLinked() {}
const RasterizedFontData *lookupFontForFamily(int, int) { return nullptr; }
}
using namespace gea::embedded::ui;
using namespace gea::embedded::test;
static NodeHandle branch(int depth, int seed) {
  auto view = Document::instance().createView();
  auto style = view.style();
  style.setProperty("display", "flex");
  style.setProperty("flex-direction", depth % 2 ? "row" : "column");
  style.setProperty("align-items", "center");
  style.setProperty("justify-content", "center");
  style.setProperty("flex", "1 1 auto");
  style.setProperty("box-sizing", "border-box");
  style.setProperty("padding", "2px");
  style.setProperty("gap", "3px");
  if (depth == 0) {
    style.setProperty("width", seed % 2 ? "57px" : "29px");
    style.setProperty("height", "11px");
    view.appendChild(Document::instance().createText("memo text"));
  } else {
    view.appendChild(branch(depth - 1, seed + 1));
    if (depth <= 3) view.appendChild(branch(depth - 1, seed + 2));
  }
  return view;
}
int main() {
  for (int depth = 1; depth <= 7; ++depth) {
    resetNativeHost(); setNativeDisplaySize(600, 450); setViewportMetrics(600, 450, 1.0);
    auto root = branch(depth, 0);
    root.style().setProperty("width", "100%");
    root.style().setProperty("height", "100%");
    auto &tree = Tree::instance();
    for (int frame = 0; frame < 12; ++frame) {
      const int width = frame % 3 == 0 ? 90 : frame % 3 == 1 ? 300 : 600;
      refreshPerfStatsMutable() = {};
      tree.mount(root.id(), width, 450);
      const auto stats = refreshPerfStatsMutable();
      std::uint64_t hash = 1469598103934665603ull;
      for (int i=0; i<tree.nodeCount(); ++i) {
        const auto &b = tree.node(i).layout;
        for (int v : {int(b.x), int(b.y), int(b.width), int(b.height)}) hash = (hash ^ std::uint32_t(v)) * 1099511628211ull;
      }
      std::printf("%d %d %d %d %d %016llx\n", depth, frame, tree.nodeCount(), stats.treeLayoutNodeCalls, stats.treeLayoutMemoHits, static_cast<unsigned long long>(hash));
    }
  }
}

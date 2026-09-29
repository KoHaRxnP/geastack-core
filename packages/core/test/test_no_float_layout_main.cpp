// SPDX-License-Identifier: Apache-2.0
// Float pruning must preserve mixed flow, relative wrappers, and height clamps.
#include "graphics/font.h"
#include "native_test_harness.h"
#include "ui/document.h"
#include "ui/internal.h"
#include "ui/tree_internal.h"
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <vector>
namespace gea::framework::app::generated {
void drainMicrotasks() {}
} // namespace gea::framework::app::generated
namespace gea::framework::graphics::generated {
void ensureLinked() {}
const RasterizedFontData *lookupFontForFamily(int, int) { return nullptr; }
} // namespace gea::framework::graphics::generated
using namespace gea::embedded::ui;
using namespace gea::embedded::test;
static std::vector<int> ids;
static NodeHandle view(const char *tag, int depth) {
  auto n = Document::instance().createView();
  ids.push_back(n.id());
  Tree::instance().setTagName(n.id(), tag);
  n.style().setProperty("position", "relative");
  n.style().setProperty("top", "1px");
  n.style().setProperty("background", depth % 2 ? "#335588" : "#884422");
  n.style().setProperty("padding", "2px");
  n.style().setProperty("margin-top", "-1px");
  return n;
}
static NodeHandle branch(int depth) {
  auto n = view("div", depth);
  auto first = view("span", depth + 1);
  first.style().setProperty("width", "31px");
  first.style().setProperty("height", "9px");
  n.appendChild(first);
  if (depth)
    n.appendChild(branch(depth - 1));
  else {
    auto block = view("div", 0);
    block.style().setProperty("height", "18px");
    n.appendChild(block);
  }
  auto last = view("span", depth + 1);
  last.style().setProperty("width", "47px");
  last.style().setProperty("height", "11px");
  n.appendChild(last);
  return n;
}
int main() {
  for (int depth : {0, 2, 5})
    for (int width : {70, 120, 180}) {
      resetNativeHost();
      StyleSheet::instance().clear();
      ids.clear();
      setNativeDisplaySize(200, 160);
      setViewportMetrics(200, 160, 1.0);
      auto root = branch(depth);
      root.style().setProperty("box-sizing", "border-box");
      root.style().setProperty("min-height", "20px");
      root.style().setProperty("max-height", "70px");
      root.style().setProperty("overflow", "hidden");
      for (int frame = 0; frame < 2; ++frame) {
        root.style().setProperty(
            "width", std::to_string(frame ? width / 2 : width) + "px");
        NodeHandle(ids.back())
            .style()
            .setProperty("height", frame ? "31px" : "11px");
        Document::instance().refresh(root, 200, 160);
        DisplayList::instance().replay();
        const auto &box = Tree::instance().node(root.id()).layout;
        assert(box.height >= 20 && box.height <= 70);
        std::uint64_t hash = 1469598103934665603ull;
        for (int y = 0; y < 160; ++y)
          for (int x = 0; x < 200; ++x)
            hash = (hash ^ displayPixelAt(x, y)) * 1099511628211ull;
        std::printf("%d/%d/%d %016llx", depth, width, frame,
                    (unsigned long long)hash);
        for (int id : ids) {
          const auto &b = Tree::instance().node(id).layout;
          std::printf(" %d,%d,%d,%d", b.x, b.y, b.width, b.height);
        }
        std::puts("");
      }
    }
}

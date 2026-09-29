// SPDX-License-Identifier: Apache-2.0
#include "css/declarative.h"
#include "graphics/font.h"
#include "host/timers.h"
#include "native_test_harness.h"
#include "ui/document.h"
#include "ui/node.h"
#include "ui/style.h"
#include "ui/tree_internal.h"
#include <cassert>
#include <cstdio>
namespace gea::framework::app::generated {
void drainMicrotasks() {}
} // namespace gea::framework::app::generated
namespace gea::framework::graphics::generated {
void ensureLinked() {}
const RasterizedFontData *lookupFontForFamily(int, int) { return nullptr; }
} // namespace gea::framework::graphics::generated
int main() {
  using namespace gea::embedded::ui;
  using namespace gea::embedded::test;
  for (int round = 0; round < 3; ++round) {
    resetNativeHost();
    auto &sheet = StyleSheet::instance();
    sheet.clear();
    sheet.registerRule("probe", "width", "20px");
    auto node = Document::instance().createView();
    node.classList().set("probe");
    int first = 0, nested = 0;
    gea::host::requestAnimationFrame([&](auto now) {
      ++first;
      assert(now == 16);
      node.style().width(31);
      gea::host::requestAnimationFrame([&](auto next) {
        ++nested;
        assert(next == 32);
        node.style().width(42);
      });
    });
    gea::css::DeclarativeAnimations::scanAndStart(0);
    sheet.startCssAnimations(0);
    auto &engine = gea::css::AnimationEngine::instance();
    assert(!engine.active() && engine.count() == 0);
    pumpFrame(16);
    assert(first == 1 && nested == 0);
    assert(Tree::instance().node(node.id()).style.width == 31);
    pumpFrame(32);
    assert(first == 1 && nested == 1);
    assert(Tree::instance().node(node.id()).style.width == 42);
    pumpFrame(48);
    assert(first == 1 && nested == 1 && !engine.active());
    std::printf("round=%d width=42 frame-callbacks=2\n", round);
  }
}

// SPDX-License-Identifier: Apache-2.0
// Coalesced paint shortcuts must match a clean replay, including presented pixels.
#include "display.h"
#include "graphics/font.h"
#include "native_test_harness.h"
#include "ui/document.h"
#include "ui/internal.h"
#include "ui/refresh_perf.h"
#include "ui/style.h"
#include "ui/tree_internal.h"
#include <cassert>
#include <cstdio>
#include <vector>
namespace gea::framework::app::generated { void drainMicrotasks() {} }
namespace gea::framework::graphics::generated {
void ensureLinked() {}
const RasterizedFontData *lookupFontForFamily(int, int) { return nullptr; }
}
using namespace gea::embedded::test;
using namespace gea::embedded::ui;
using gea::platform::display::Display;
int main() {
  int failures = 0;
  for (int rounded : {0, 1}) for (int scenario = 0; scenario < 13; ++scenario) {
    resetNativeHost(); setNativeDisplaySize(160, 64); setViewportMetrics(160, 64, 1);
    auto &sheet = StyleSheet::instance(); sheet.clear();
    sheet.registerRule("red", "background-color", "#dd2200");
    sheet.registerRule("green", "background-color", "#00aa22");
    sheet.registerRule("blue", "background-color", "#2244dd");
    auto root = Document::instance().createView();
    root.style().setProperty("width", "160px"); root.style().setProperty("height", "64px");
    root.style().setProperty("background", "#123456");
    NodeHandle leaf((scenario < 3 || scenario == 12) ? Document::instance().createView().id() : Document::instance().createText("1000").id());
    leaf.style().setProperty("position", "absolute");
    leaf.style().setProperty("left", "12px"); leaf.style().setProperty("top", "12px");
    leaf.style().setProperty("width", "130px"); leaf.style().setProperty("height", "32px");
    leaf.style().setProperty("white-space", "nowrap"); leaf.style().setProperty("color", "white");
    if (rounded) leaf.style().setProperty("border-radius", "7px");
    if (scenario == 1 || scenario == 8 || scenario == 9) leaf.classList().set("red");
    else leaf.style().setProperty("background-color", "#dd2200");
    root.appendChild(leaf); Document::instance().mount(root, 160, 64);
    Document::instance().refresh(root, 160, 64);
    Display::clearNoFlush(); DisplayList::instance().replay(); Display::flush();
    auto bg = [&] { leaf.style().setProperty("background-color", "#2244dd"); };
    switch (scenario) {
      case 0: bg(); break;
      case 1: leaf.classList().set("green"); leaf.classList().set("blue"); break;
      case 2: leaf.style().setProperty("background-color", "#00aa22"); bg(); break;
      case 3: leaf.setText("1001"); break;
      case 4: leaf.setText("1001"); leaf.setText("2001"); break;
      case 5: leaf.setText("1001"); leaf.setText("1"); break;
      case 6: leaf.setText("1001"); bg(); break;
      case 7: bg(); leaf.setText("1001"); break;
      case 8: leaf.setText("1001"); leaf.classList().set("blue"); break;
      case 9: leaf.classList().set("blue"); leaf.setText("1001"); break;
      case 10: leaf.setText("1001"); leaf.style().setProperty("color", "#ffff00"); break;
      case 11: leaf.style().setProperty("color", "#ffff00"); leaf.setText("1001"); break;
      case 12:
        Tree::instance().setStyle(leaf.id(), Property::BackgroundColor, 0x07e0);
        Tree::instance().setStyle(leaf.id(), Property::BackgroundColor, 0x001f);
        break;
    }
    const auto &paint = Tree::instance().node(leaf.id()).render;
    // Inspect only stable flags, so the same fixture builds against the old engine.
#ifndef GEA_TEST_REPAINT_REFERENCE
    assert(!(paint.bg_recolor_pending && paint.text_partial_dirty));
    if (scenario == 0 || scenario == 1 || scenario == 2 || scenario == 12) assert(paint.bg_recolor_pending);
    if (scenario == 3 || scenario == 4) assert(paint.text_partial_dirty);
#endif
    refreshPerfStatsMutable() = {};
    Document::instance().refresh(root, 160, 64);
    const auto perf = refreshPerfStatsMutable();
    std::vector<unsigned short> dirty, presented;
    for (int y=0;y<64;++y) for (int x=0;x<160;++x) {
      dirty.push_back(displayPixelAt(x,y)); presented.push_back(presentedPixelAt(x,y));
    }
    Display::clearNoFlush(); Display::resetClip(); Display::setAlpha(255);
    DisplayList::instance().replay();
    int differs=0, panelDiffers=0;
    for (int y=0;y<64;++y) for (int x=0;x<160;++x) {
      auto reference=displayPixelAt(x,y);
      differs += dirty[y*160+x]!=reference; panelDiffers += presented[y*160+x]!=reference;
    }
    std::printf("rounded=%d scenario=%d canvas_diff=%d panel_diff=%d layout=%d memo=%d\n", rounded, scenario, differs, panelDiffers, perf.treeLayoutNodeCalls, perf.treeLayoutMemoHits);
    failures += differs != 0 || panelDiffers != 0;
  }
  return failures ? 1 : 0;
}

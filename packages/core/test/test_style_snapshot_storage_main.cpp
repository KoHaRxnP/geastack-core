// SPDX-License-Identifier: Apache-2.0
#include "graphics/font.h"
#include "native_test_harness.h"
#include "ui/document.h"
#include "ui/node.h"
#include "ui/style.h"
#include "ui/tree_internal.h"
#include <array>
#include <cassert>
#include <cstdio>
#include <string>
namespace gea::framework::app::generated { void drainMicrotasks() {} }
namespace gea::framework::graphics::generated {
void ensureLinked() {}
const RasterizedFontData *lookupFontForFamily(int, int) { return nullptr; }
}
using namespace gea::embedded::ui;
int main() {
  gea::embedded::test::resetNativeHost();
  auto &sheet = StyleSheet::instance();
  auto &tree = Tree::instance();
  sheet.clear();
  setViewportMetrics(240, 160, 1);
  // Six inherited variables overflow the four-entry snapshot.
  static constexpr const char *names[] = {"--c0", "--c1", "--c2", "--c3", "--c4", "--c5"};
  for (int mode = 0; mode < 2; ++mode) {
    const char *theme = mode ? "theme-b" : "theme-a";
    for (int i = 0; i < 6; ++i) {
      sheet.registerStaticCustomColorRule(StaticStyleSelectorKind::Class,
          theme, names[i], mode ? 0 : 255, 16 * i,
          mode ? 255 : 0, 255);
    }
    sheet.registerRule(theme, "font-size", mode ? "26px" : "18px");
    sheet.registerRule(theme, "font-weight", mode ? "700" : "400");
    sheet.registerRule(theme, "line-height", mode ? "2" : "1");
#if GEA_CSS_CUSTOM_PROPERTY_LENGTHS
    sheet.registerRule(theme, "--length", mode ? "36px" : "12px");
#endif
  }
  // Backgrounds come from class rules, the form the compiler emits; they must
  // re-resolve each variable against the ancestor theme on every switch.
  static constexpr const char *backgrounds[] = {"bg0", "bg1", "bg2", "bg3", "bg4", "bg5"};
  static constexpr const char *uses[] = {"var(--c0)", "var(--c1)", "var(--c2)", "var(--c3)", "var(--c4)", "var(--c5)"};
  for (int i = 0; i < 6; ++i) sheet.registerRule(backgrounds[i], "background-color", uses[i]);
#if GEA_CSS_CUSTOM_PROPERTY_LENGTHS
  sheet.registerRule("ancestor-target", "width", "var(--length)");
#endif
  sheet.registerSelectorRule(".theme-a .ancestor-target", "height", "21px");
  sheet.registerSelectorRule(".theme-b .ancestor-target", "height", "35px");
  auto root = Document::instance().createView();
  root.style().width(240); root.style().height(160);
  std::array<NodeHandle, 6> children{};
  for (int i = 0; i < 6; ++i) {
    children[i] = Document::instance().createView();
    children[i].classList().set(std::string("ancestor-target ") + backgrounds[i]);
#if !GEA_CSS_CUSTOM_PROPERTY_LENGTHS
    children[i].style().width(36);
#endif
    root.appendChild(children[i]);
  }
  root.classList().set("theme-a");
  // An inline whole-value var() reads a static custom color's native value;
  // that entry has no text for var() substitution to use.
  auto inlineUser = Document::instance().createView();
  root.appendChild(inlineUser);
  inlineUser.style().setProperty("background-color", "var(--c1)");
  tree.mount(root.id(), 240, 160);
  {
    const auto &s = tree.node(inlineUser.id()).style;
    assert(s.has_bg && s.bg_alpha == 255 &&
        s.bg_color == gea::framework::graphics::pixel::nativeColor(255, 16, 0));
  }
  for (int round = 0; round < 30; ++round) {
    const bool mode = (round & 1) != 0;
    // Old and new tokens exceed both the proven two-token capacity and the
    // ordinary eight-token snapshot, exercising the safe overflow path.
    beginStyleMountBatch();
    root.classList().set(std::string(mode ? "theme-b" : "theme-a") +
        " a b c d e f g h i j");
    endStyleMountBatch();
    tree.computeLayout(root.id(), 240, 160);
    for (int i = 0; i < 6; ++i) {
      const auto &s = tree.node(children[i].id()).style;
      assert(s.bg_color == gea::framework::graphics::pixel::nativeColor(
          mode ? 0 : 255, 16 * i, mode ? 255 : 0));
      assert(s.bg_alpha == 255 && s.has_bg);
      assert(s.font_size == (mode ? 26 : 18));
      assert(s.font_weight == (mode ? 700 : 400));
      assert(s.height == (mode ? 35 : 21));
#if GEA_CSS_CUSTOM_PROPERTY_LENGTHS
      assert(tree.node(children[i].id()).layout.width == (mode ? 36 : 12));
#endif
    }
  }
  std::puts("PASS: overflowed class/custom snapshots preserve inherited colors, fonts, lengths and ancestor selectors");
}

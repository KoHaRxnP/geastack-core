#include "canvas.h"
#include "display.h"
#include "graphics/font.h"
#include "native_test_harness.h"
#include "ui/internal.h"
#include <algorithm>
#include <cstdio>
#include <vector>

namespace gea::framework::app::generated {
void drainMicrotasks() {}
} // namespace gea::framework::app::generated
namespace gea::framework::graphics::generated {
void ensureLinked() {}
const RasterizedFontData *lookupFontForFamily(int, int) {
  static const std::uint8_t atlas[] = {255, 128, 0,   255, 0,   255, 128, 0,
                                       0,   128, 255, 0,   255, 0,   0,   255};
  static const Glyph glyphs[] = {{32, 0, 0, 0, 0, 6, 0, 0},
                                 {88, 0, 0, 4, 4, 6, 0, 4}};
  static const RasterizedFontData font{9301, 4,      8, 4, -4,
                                       2,    glyphs, 4, 4, atlas};
  return &font;
}
} // namespace gea::framework::graphics::generated
using namespace gea::embedded::ui;
using namespace gea::embedded::test;
namespace pixel = gea::framework::graphics::pixel;

int main() {
  resetNativeHost();
  setNativeDisplaySize(480, 320);
  auto *canvas = gea::platform::display::Display::canvas();
  auto &list = DisplayList::instance();
  DisplayCommand command{};
  command.type = DisplayCommandType::DrawProjectedText;
  command.bx = 20;
  command.by = 30;
  command.bw = 15;
  command.bh = 15;
  auto &text = command.projectedText;
  text.text = "XX";
  text.srcX = 0;
  text.srcY = 0;
  text.srcW = 12;
  text.srcH = 8;
  text.x0 = 20;
  text.y0 = 30;
  text.x1 = 34;
  text.y1 = 32;
  text.x2 = 33;
  text.y2 = 43;
  text.x3 = 21;
  text.y3 = 41;
  text.fontId = 9301;
  text.fontSize = 8;
  text.color = pixel::nativeColor(240, 200, 100);
  const auto background = pixel::nativeColor(20, 30, 40);
  bool ok = true;
  for (const int alpha : {255, 128}) {
    text.alpha = alpha;
    for (const int parentAlpha : {255, 180}) {
      for (const bool clipped : {false, true}) {
        list.clear();
        auto *alphaCommand = list.append();
        *alphaCommand = {};
        alphaCommand->type = DisplayCommandType::SetAlpha;
        alphaCommand->alpha.alpha = parentAlpha;
        if (clipped) {
          auto *clip = list.append();
          *clip = {};
          clip->type = DisplayCommandType::PushClip;
          clip->clip.x = 23;
          clip->clip.y = 33;
          clip->clip.w = 8;
          clip->clip.h = 8;
        }
        *list.append() = command;
        canvas->bindPixels(canvas->pixels(), 480, 320);
        canvas->resetClip();
        std::fill(canvas->pixels(), canvas->pixels() + 480 * 320, background);
        gea::platform::display::Display::setAlpha(parentAlpha);
        list.replay();
        std::vector<pixel::native_t> expected(canvas->pixels(),
                                              canvas->pixels() + 480 * 320);
        const auto ink =
            std::count_if(expected.begin(), expected.end(),
                          [&](auto value) { return value != background; });
        if (!ink) {
          std::fprintf(stderr, "projected text test has no ink\n");
          return 1;
        }
        canvas->bindPixelsRotatedLandscape(canvas->pixels(), 480, 320);
        canvas->resetClip();
        std::fill(canvas->pixels(), canvas->pixels() + 480 * 320, background);
        gea::platform::display::Display::setAlpha(parentAlpha);
        list.replay();
        for (int y = 0; y < 320; ++y)
          for (int x = 0; x < 480; ++x) {
            if (canvas->readPixelNative(x, y) != expected[y * 480 + x]) {
              std::fprintf(stderr,
                           "rotated projected text differs at %d,%d (alpha=%d "
                           "parent=%d clipped=%d)\n",
                           x, y, alpha, parentAlpha, clipped);
              ok = false;
              goto nextCase;
            }
          }
      nextCase:;
      }
    }
  }
  auto &tree = Tree::instance();
  int empty = tree.createText();
  tree.setText(empty, "XX");
  tree.setStyle(empty, Property::FontId, 9301);
  tree.setStyle(empty, Property::FontSize, 8);
  tree.setStyle(empty, Property::Overflow, 1);
  auto &node = tree.node(empty);
  node.layout.x = 47;
  node.layout.y = 80;
  node.layout.width = 0;
  node.layout.height = 23;
  list.clear();
  TextRenderer::record(node);
  if (list.commandCount() != 0) {
    std::fprintf(stderr, "zero-width clipped text emitted paint commands\n");
    ok = false;
  }
  std::printf("rotated projected text: %s (480x320, opaque/partial alpha, "
              "parent alpha, clipping)\n",
              ok ? "PASS" : "FAIL");
  return ok ? 0 : 1;
}

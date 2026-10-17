#include "graphics/font.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <vector>

namespace gea::platform::storage {
bool ensureMounted() { return true; }
} // namespace gea::platform::storage

extern "C" bool gea_embedded_assets_prepare();
extern "C" bool gea_embedded_asset_lookup(const char *, const unsigned char **,
                                          unsigned long *);
extern "C" const unsigned char gea_asset_pixel_png[];

int main(int argc, char **argv) {
  assert(argc == 4);
  const bool expected = std::strcmp(argv[1], "ready") == 0;
  assert(gea_embedded_assets_prepare() == expected);
  const unsigned char *image = nullptr;
  unsigned long length = 0;
  assert(gea_embedded_asset_lookup("/pixel.png", &image, &length) == expected);
  using namespace gea::framework::graphics::generated;
  ensureLinked();
  const auto *font = lookupFontForFamily(lookupFontFamily("Bebas Neue"), 20);
  assert((font != nullptr) == expected);
  if (!expected)
    return 0;
  assert(image == gea_asset_pixel_png);
  assert(length > 8 && std::memcmp(image, "\x89PNG\r\n\x1a\n", 8) == 0);
  assert(font->atlasBits == 8 && font->sizePx == 20);
  assert(font->glyphCount > 90);
  const auto size =
      static_cast<std::size_t>(font->atlasWidth * font->atlasHeight);
  std::vector<unsigned char> pixels(size);
  auto *file = std::fopen(argv[3], "rb");
  assert(file && std::fread(pixels.data(), 1, size, file) == size);
  std::fclose(file);
  assert(std::memcmp(pixels.data(), font->atlas, size) == 0);
  // Once initialized, rendering never reads the volume again.
  assert(std::remove(argv[2]) == 0);
  assert(std::remove(argv[3]) == 0);
  assert(gea_embedded_assets_prepare());
  assert(gea_embedded_asset_lookup("/pixel.png", &image, &length));
  assert(lookupFontForFamily(lookupFontFamily("Bebas Neue"), 20) == font);
  assert(std::memcmp(pixels.data(), font->atlas, size) == 0);
}

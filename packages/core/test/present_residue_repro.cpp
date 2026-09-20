// Standalone repro: does the ESP32/geaos incremental frame-diff present
// (present::dirtyRects + region rasterization applied to a persistent panel)
// leave residue that a full-frame render would not?
//
// This mirrors what targets/esp32/display.cpp::present()+presentRegion() do on
// device. macOS/web/native_test_host all render full-frame, so they never
// exercise this path -- which is why a residue bug here goes uncaught.

#include "display_present.h"

#include <cstdio>
#include <cstdint>
#include <vector>

namespace dp = gea::framework::display_present;
using gea::framework::graphics::Canvas;

static int W = 410;
static int H = 502;
static int kChunkRows = 80;  // setFlushConfig({ rows: 80 })

// ---- synthetic ground tile: row 0 grass(green), rows 1..h-1 dirt(brown) ----
static std::vector<std::uint16_t> g_tile;
static int kTileW = 18;
static int kTileH = 36;
static const std::uint16_t SKY = 0x4D9F;
static const std::uint16_t GRASS = 0x07E0;
static const std::uint16_t DIRT = 0xA145;

static void makeTile() {
  g_tile.resize((size_t)kTileW * kTileH);
  for (int y = 0; y < kTileH; y++)
    for (int x = 0; x < kTileW; x++)
      g_tile[(size_t)y * kTileW + x] = (y == 0) ? GRASS : DIRT;
}

// Build one frame's command list for a given camera scroll x of the platform.
static dp::Frame makeFrame(int tileX, int tileY, int platformW) {
  dp::Frame f;
  using T = gea::platform::display::DisplayPresentCommandType;

  dp::Command bg;
  bg.type = T::FillRectRgb565;
  bg.x = 0; bg.y = 0; bg.w = W; bg.h = H; bg.color = SKY; bg.alpha = 255;
  f.commands.push_back(std::move(bg));

  dp::Command tile;
  tile.type = T::DrawImageTiledX;
  tile.pixels = g_tile.data();
  tile.alphaPixels = nullptr;
  tile.srcWidth = kTileW;
  tile.srcHeight = kTileH;
  tile.x = tileX; tile.y = tileY; tile.w = platformW; tile.alpha = 255;
  f.commands.push_back(std::move(tile));

  return f;
}

// Full-frame reference render (what macOS/web show).
static void fullRender(const dp::Frame &f, std::vector<std::uint16_t> &out) {
  out.assign((size_t)W * H, 0);
  dp::rasterFrameRows(out.data(), /*regionX0*/0, /*regionWidth*/W,
                      /*row*/0, /*chunkRows*/H, /*displayHeight*/H, f);
}

// Apply one present to the persistent panel, exactly like presentRegion():
// compute dirty regions vs previous frame, rasterize current frame clipped to
// each region, copy those region pixels into the panel.
static void presentIncremental(const dp::Frame *prev, const dp::Frame &cur,
                               std::vector<std::uint16_t> &panel) {
  constexpr int kMaxRects = 24;
  dp::Rect regions[kMaxRects];
  int n = dp::dirtyRects(prev, cur, regions, kMaxRects, W, H);
  std::vector<std::uint16_t> chunk((size_t)W * kChunkRows);
  // Validation knob: GEA_SHRINK=1 shrinks each dirty region's bottom by 1px,
  // injecting the exact "1px row never repainted" bug. If the repro is sound it
  // must then REPORT residue. With it unset, we test the real present logic.
  static const bool kShrink = [](){ const char *e = std::getenv("GEA_SHRINK"); return e && e[0] == '1'; }();
  for (int i = 0; i < n; i++) {
    dp::Rect region = dp::clampAndAlign(regions[i], W, H);
    if (!dp::valid(region)) continue;
    if (kShrink && region.y1 > region.y0) region.y1 -= 1;
    int width = region.x1 - region.x0 + 1;
    for (int row = region.y0; row <= region.y1; row += kChunkRows) {
      int rows = kChunkRows;
      if (row + rows > region.y1 + 1) rows = region.y1 - row + 1;
      if (rows <= 0) break;
      dp::rasterFrameRows(chunk.data(), region.x0, width, row, rows, H, cur);
      for (int r = 0; r < rows; r++)
        for (int c = 0; c < width; c++)
          panel[(size_t)(row + r) * W + (region.x0 + c)] =
              chunk[(size_t)r * width + c];
    }
  }
}

// A faithful-ish sky-hop frame: static background (sky+2 hills+water), several
// air platforms (drawImageTiledX) at various rows that scroll + cull in/out,
// pulsing+scrolling coins (fillCircle+strokeCircle) that get collected
// (command-count changes), a player image, and a HUD (fillRect + changing text).
static std::vector<std::uint16_t> g_player;
static int kPlW = 29, kPlH = 34;
static void makePlayer() {
  g_player.assign((size_t)kPlW * kPlH, 0xF800);
}

struct Plat { int worldX; int y; int w; };
struct CoinC { int worldX; int y; bool collected; };

static dp::Frame makeSkyHopFrame(int cameraX, int score, int walkFrame,
                                 const std::vector<Plat> &plats,
                                 const std::vector<CoinC> &coins) {
  using T = gea::platform::display::DisplayPresentCommandType;
  dp::Frame f;
  auto push = [&](dp::Command &c) { f.commands.push_back(std::move(c)); };
  auto viewX = [&](int wx) { return wx - cameraX; };

  // --- background (static, drawn every frame) ---
  { dp::Command c; c.type=T::FillRectRgb565; c.x=0;c.y=0;c.w=W;c.h=H;c.color=SKY;c.alpha=255; push(c); }
  { dp::Command c; c.type=T::FillTriangleRgb565; c.x0=-58;c.y0=188;c.x1=128;c.y1=24;c.x2=336;c.y2=188;c.color=0x4D4D;c.alpha=255; push(c); }
  { dp::Command c; c.type=T::FillTriangleRgb565; c.x0=156;c.y0=188;c.x1=330;c.y1=46;c.x2=W+82;c.y2=188;c.color=0x6E6D;c.alpha=255; push(c); }
  { dp::Command c; c.type=T::FillRectRgb565; c.x=0;c.y=182;c.w=W;c.h=H-182;c.color=0x2C9A;c.alpha=255; push(c); }

  // --- coins (culled by x), pulsing radius, with stroke ---
  for (const auto &co : coins) {
    if (co.collected) continue;
    int x = viewX(co.worldX);
    if (x < -16 || x > W + 16) continue;
    int r = ((walkFrame / 28) % 2) ? 8 : 7;
    { dp::Command c; c.type=T::FillCircleRgb565; c.x=x;c.y=co.y;c.radius=r;c.color=0xFE4D;c.alpha=255; push(c); }
    { dp::Command c; c.type=T::StrokeCircleRgb565; c.x=x;c.y=co.y;c.radius=r;c.color=0x8C63;c.alpha=255; push(c); }
  }

  // --- air platforms (culled by x) ---
  for (const auto &p : plats) {
    int x = viewX(p.worldX);
    if (x + p.w < 0 || x > W) continue;
    dp::Command c; c.type=T::DrawImageTiledX; c.pixels=g_tile.data(); c.alphaPixels=nullptr;
    c.srcWidth=kTileW; c.srcHeight=kTileH; c.x=x; c.y=p.y; c.w=p.w; c.alpha=255; push(c);
  }

  // --- player (always drawn) ---
  { dp::Command c; c.type=T::DrawImage; c.pixels=g_player.data(); c.alphaPixels=nullptr;
    c.srcWidth=kPlW; c.srcHeight=kPlH; c.x=viewX(cameraX + 170); c.y=200; c.alpha=255; push(c); }

  // --- hud: bar + changing score text ---
  { dp::Command c; c.type=T::FillRectRgb565; c.x=0;c.y=0;c.w=W;c.h=36;c.color=0x1965;c.alpha=255; push(c); }
  { dp::Command c; c.type=T::FillText; c.text=std::string("Coins ")+std::to_string(score); c.x=116;c.y=9;c.color=0xFFFF;c.scale=1.0f;c.alpha=255; push(c); }

  return f;
}

int main() {
  makeTile();
  makePlayer();
  int tileY = 108;
  int platformW = 108;  // 6 tiles wide

  std::vector<std::uint16_t> panel((size_t)W * H, 0);
  std::vector<std::uint16_t> ref;

  dp::Frame prev;
  bool havePrev = false;
  int totalResidue = 0;
  int firstBadFrame = -1;

  // Air platforms at various rows (row*36), scattered across the world.
  std::vector<Plat> plats = {
    {120,108,108},{360,180,72},{500,144,108},{640,216,72},{760,108,144},{900,180,108},
  };
  // Coins scattered, get collected as the player passes.
  std::vector<CoinC> coins;
  for (int i=0;i<14;i++) coins.push_back({150+i*60, 90+(i%3)*36, false});

  // Scroll the camera right 1px/frame (world scrolls left), like viewportX.
  for (int frame = 0; frame < 80; frame++) {
    int cameraX = frame;            // pans right 1px/frame
    int score = frame / 6;          // score climbs, HUD text changes
    int walkFrame = frame * 5;      // drives coin pulse
    // collect coins the player has passed (changes command count)
    for (auto &co : coins) if (!co.collected && co.worldX < cameraX + 150) co.collected = true;
    dp::Frame cur = makeSkyHopFrame(cameraX, score, walkFrame, plats, coins);

    presentIncremental(havePrev ? &prev : nullptr, cur, panel);
    fullRender(cur, ref);

    // Compare panel (device incremental) vs ref (full render).
    int residue = 0;
    int sampleX = -1, sampleY = -1;
    int rowCounts[600] = {0};
    for (int y = 0; y < H; y++) {
      for (int x = 0; x < W; x++) {
        if (panel[(size_t)y * W + x] != ref[(size_t)y * W + x]) {
          residue++;
          if (y < 600) rowCounts[y]++;
          if (sampleX < 0) { sampleX = x; sampleY = y; }
        }
      }
    }
    if (residue > 0) {
      if (firstBadFrame < 0) firstBadFrame = frame;
      totalResidue += residue;
      // Report rows where mismatch occurs, with counts.
      printf("frame %d cameraX=%d: %d mismatching px; first at (%d,%d) "
             "panel=0x%04X ref=0x%04X\n",
             frame, cameraX, residue, sampleX, sampleY,
             panel[(size_t)sampleY * W + sampleX],
             ref[(size_t)sampleY * W + sampleX]);
      printf("           mismatch rows:");
      for (int y = 0; y < 600; y++)
        if (rowCounts[y]) printf(" %d(x%d)", y, rowCounts[y]);
      printf("\n");
    }

    prev = std::move(cur);
    havePrev = true;
  }

  printf("\n==== %s: %d total residue px across frames (firstBadFrame=%d) ====\n",
         totalResidue ? "RESIDUE REPRODUCED" : "no residue", totalResidue,
         firstBadFrame);
  return totalResidue ? 1 : 0;
}

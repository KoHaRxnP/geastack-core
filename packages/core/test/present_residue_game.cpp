// Drives the REAL compiled sky-hop canvas game, holds the right control to
// scroll the camera sideways, and on every present() runs the device's
// incremental frame-diff present (present::dirtyRects + region raster applied
// to a persistent panel) and compares it to a full-frame render. Any pixel
// where the incremental panel != full render is residue the device would show
// but macOS/web/native full-frame paths hide.

#include "native_test_harness.h"
#include "ui/document.h"

#include "display_present.h"

#include <cstdio>
#include <vector>

extern void __gea_top_level();

namespace dp = gea::framework::display_present;

namespace gea { namespace embedded { namespace test {
extern void (*gPresentObserver)(const gea::platform::display::DisplayPresentCommand *, int);
} } }

static int kChunkRows = 80;

static void fullRender(const dp::Frame &f, int W, int H, std::vector<std::uint16_t> &out) {
  out.assign((size_t)W * H, 0);
  dp::rasterFrameRows(out.data(), 0, W, 0, H, H, f);
}

static void presentIncremental(const dp::Frame *prev, const dp::Frame &cur,
                               int W, int H, std::vector<std::uint16_t> &panel) {
  constexpr int kMaxRects = 24;
  dp::Rect regions[kMaxRects];
  int n = dp::dirtyRects(prev, cur, regions, kMaxRects, W, H);
  std::vector<std::uint16_t> chunk((size_t)W * kChunkRows);
  for (int i = 0; i < n; i++) {
    dp::Rect region = dp::clampAndAlign(regions[i], W, H);
    if (!dp::valid(region)) continue;
    int width = region.x1 - region.x0 + 1;
    for (int row = region.y0; row <= region.y1; row += kChunkRows) {
      int rows = kChunkRows;
      if (row + rows > region.y1 + 1) rows = region.y1 - row + 1;
      if (rows <= 0) break;
      dp::rasterFrameRows(chunk.data(), region.x0, width, row, rows, H, cur);
      for (int r = 0; r < rows; r++)
        for (int c = 0; c < width; c++)
          panel[(size_t)(row + r) * W + (region.x0 + c)] = chunk[(size_t)r * width + c];
    }
  }
}

// persistent observer state (mirrors device DisplayBackend present state)
static dp::Frame g_prev;
static bool g_havePrev = false;
static std::vector<std::uint16_t> g_panel;
static int g_frame = 0;
static int g_totalResidue = 0;
static int g_firstBadFrame = -1;
static int g_residueFrames = 0;
static int g_incrementalPresents = 0;
static int g_fallbackPresents = 0;
static int g_tileXmin = 1 << 30, g_tileXmax = -(1 << 30);
static int g_lastCmdCount = -1, g_cmdCountChanges = 0;
static long g_typeHist[16] = {0};

static void observer(const gea::platform::display::DisplayPresentCommand *cmds, int n) {
  auto *cv = gea::platform::display::Display::canvas();
  if (!cv) return;
  const int W = cv->width(), H = cv->height();
  if (W <= 0 || H <= 0) return;
  if ((int)g_panel.size() != W * H) g_panel.assign((size_t)W * H, 0);

  dp::Frame cur;
  if (!dp::extractFrame(cmds, n, cur)) return;  // device present() returns false
  g_frame++;

  // diagnostics: track motion + command-count churn (cull) + type histogram
  if (g_lastCmdCount >= 0 && g_lastCmdCount != n) g_cmdCountChanges++;
  g_lastCmdCount = n;
  using TT = gea::platform::display::DisplayPresentCommandType;
  for (const auto &c : cur.commands) {
    g_typeHist[(int)c.type]++;
    if (c.type == TT::DrawImageTiledX || c.type == TT::DrawImage || c.type == TT::FillCircleRgb565) {
      if (c.x < g_tileXmin) g_tileXmin = c.x;
      if (c.x > g_tileXmax) g_tileXmax = c.x;
    }
  }

  const bool curOpaque = dp::frameHasOpaqueBase(cur, W, H);
  const bool prevOpaque = g_havePrev && dp::frameHasOpaqueBase(g_prev, W, H);
  if (!curOpaque || !prevOpaque) {
    // device falls back to full-screen present here
    g_fallbackPresents++;
    fullRender(cur, W, H, g_panel);
    g_prev = std::move(cur);
    g_havePrev = true;
    return;
  }
  g_incrementalPresents++;

  presentIncremental(&g_prev, cur, W, H, g_panel);

  std::vector<std::uint16_t> ref;
  fullRender(cur, W, H, ref);

  int residue = 0, sampleX = -1, sampleY = -1;
  std::vector<int> rowCounts(H, 0);
  for (int y = 0; y < H; y++)
    for (int x = 0; x < W; x++)
      if (g_panel[(size_t)y * W + x] != ref[(size_t)y * W + x]) {
        residue++; rowCounts[y]++;
        if (sampleX < 0) { sampleX = x; sampleY = y; }
      }

  if (residue > 0) {
    if (g_firstBadFrame < 0) g_firstBadFrame = g_frame;
    g_totalResidue += residue;
    g_residueFrames++;
    if (g_residueFrames <= 12) {
      printf("present#%d (cmds=%d): %d residue px; first (%d,%d) panel=0x%04X ref=0x%04X\n",
             g_frame, n, residue, sampleX, sampleY,
             g_panel[(size_t)sampleY * W + sampleX], ref[(size_t)sampleY * W + sampleX]);
      printf("    residue rows:");
      for (int y = 0; y < H; y++) if (rowCounts[y]) printf(" %d(x%d)", y, rowCounts[y]);
      printf("\n");
    }
  }

  g_prev = std::move(cur);
  g_havePrev = true;
}

int main() {
  using namespace gea::embedded::test;
  using gea::framework::events::TouchPhase;

  resetNativeHost();
  gea::embedded::ui::Document::instance().ensureAppRoot("app");
  __gea_top_level();
  refresh();

  gPresentObserver = &observer;

  double t = 0.0;
  // Let assets load + a few idle frames settle.
  for (int i = 0; i < 6; i++) { t += 16.67; pumpFrame(t); }

  // Hold the RIGHT control button down: player accelerates right, camera pans,
  // the world (ground tiles) scrolls left. Right button center ~ (117,457).
  dispatchTouch(TouchPhase::Down, true, 117, 457);
  for (int i = 0; i < 240; i++) { t += 16.67; pumpFrame(t); }
  dispatchTouch(TouchPhase::Up, false, 117, 457);

  auto *cv = gea::platform::display::Display::canvas();
  printf("\ncanvas=%dx%d  presents=%d (incremental=%d, fullFallback=%d)  "
         "movingX range=[%d..%d]  cmdCountChanges=%d\n",
         cv ? cv->width() : -1, cv ? cv->height() : -1, g_frame,
         g_incrementalPresents, g_fallbackPresents, g_tileXmin, g_tileXmax,
         g_cmdCountChanges);
  const char *typeNames[] = {"Clear","FillRect","StrokeRect","FillTri","FillCircle",
    "StrokeCircle","FillCircles","DrawImage","DrawImageScaled","DrawImageRotated90CW","DrawImageTiledX","FillText"};
  printf("cmd type histogram:");
  for (int i = 0; i < 12; i++) if (g_typeHist[i]) printf(" %s=%ld", typeNames[i], g_typeHist[i]);
  printf("\n");
  printf("imageLoadCount=%d\n", imageLoadCount());
  printf("==== %s: %d residue px over %d/%d frames (firstBadPresent=%d) ====\n",
         g_totalResidue ? "RESIDUE REPRODUCED" : "no residue",
         g_totalResidue, g_residueFrames, g_frame, g_firstBadFrame);
  return 0;
}

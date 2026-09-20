#pragma once

#include "pixel.h"

#include <cstdint>
#include <cstddef>

namespace gea::framework::graphics {
struct CircleEntry;    // canvas.h; only referenced by pointer here
struct TriangleEntry;  // canvas.h; only referenced by pointer here
class Canvas;
}  // namespace gea::framework::graphics

namespace gea::platform::display {

#ifndef GEA_EMBEDDED_DISPLAY_WIDTH
#define GEA_EMBEDDED_DISPLAY_WIDTH 410
#endif

#ifndef GEA_EMBEDDED_DISPLAY_HEIGHT
#define GEA_EMBEDDED_DISPLAY_HEIGHT 502
#endif

#ifndef GEA_EMBEDDED_DISPLAY_NATIVE_WIDTH
#define GEA_EMBEDDED_DISPLAY_NATIVE_WIDTH GEA_EMBEDDED_DISPLAY_WIDTH
#endif

#ifndef GEA_EMBEDDED_DISPLAY_NATIVE_HEIGHT
#define GEA_EMBEDDED_DISPLAY_NATIVE_HEIGHT GEA_EMBEDDED_DISPLAY_HEIGHT
#endif

inline constexpr int kWidth = GEA_EMBEDDED_DISPLAY_WIDTH;
inline constexpr int kHeight = GEA_EMBEDDED_DISPLAY_HEIGHT;
inline constexpr int kNativeWidth = GEA_EMBEDDED_DISPLAY_NATIVE_WIDTH;
inline constexpr int kNativeHeight = GEA_EMBEDDED_DISPLAY_NATIVE_HEIGHT;
inline constexpr int kConsoleFontWidth = 8;
inline constexpr int kConsoleFontHeight = 16;
inline constexpr int kConsoleFontScale = 3;
inline constexpr int kConsoleGlyphWidth = kConsoleFontWidth * kConsoleFontScale;
inline constexpr int kConsoleGlyphHeight = kConsoleFontHeight * kConsoleFontScale;
inline constexpr int kConsoleColumns = kWidth / kConsoleGlyphWidth;
inline constexpr int kConsoleRows = kHeight / kConsoleGlyphHeight;

struct DisplayFlushPerfStats {
	int64_t totalUs = 0;
	int64_t setWindowUs = 0;
	int64_t slotWaitUs = 0;
	int64_t copyUs = 0;
	int64_t rasterUs = 0;
	int64_t byteSwapUs = 0;
	int64_t txUs = 0;
	int64_t completeWaitUs = 0;
	int64_t entryWaitUs = 0;
	int64_t chunkWaitUs = 0;
	int64_t tailWaitUs = 0;
	int callCount = 0;
	int chunkCount = 0;
	int pixelCount = 0;
};

struct DisplayFlushStageDetail {
	int x0 = 0;
	int y0 = 0;
	int x1 = -1;
	int y1 = -1;
	int row = 0;
	int rows = 0;
	int pixels = 0;
};

enum class DisplayPresentCommandType : uint8_t {
	Clear,
	FillRectRgb565,
	StrokeRectRgb565,
	FillTriangleRgb565,
	FillCircleRgb565,
	StrokeCircleRgb565,
	FillCirclesRgb565,
	DrawImage,
	DrawImageScaled,
	DrawImageRotated90CW,
	DrawImageTiledX,
	FillText,
	FillTrianglesRgb565
};

struct DisplayPresentCommand {
	DisplayPresentCommandType type = DisplayPresentCommandType::Clear;
	union {
		struct {
			gea::framework::graphics::pixel::native_t color;
		} clear;
		struct {
			int x;
			int y;
			int w;
			int h;
			gea::framework::graphics::pixel::native_t color;
			uint8_t alpha;
		} fillRectRgb565;
		struct {
			int x;
			int y;
			int w;
			int h;
			gea::framework::graphics::pixel::native_t color;
			uint8_t alpha;
		} strokeRectRgb565;
		struct {
			int x0;
			int y0;
			int x1;
			int y1;
			int x2;
			int y2;
			gea::framework::graphics::pixel::native_t color;
			uint8_t alpha;
		} fillTriangleRgb565;
		struct {
			int x;
			int y;
			int radius;
			gea::framework::graphics::pixel::native_t color;
			uint8_t alpha;
		} fillCircleRgb565;
		struct {
			int x;
			int y;
			int radius;
			gea::framework::graphics::pixel::native_t color;
			uint8_t alpha;
		} strokeCircleRgb565;
			struct {
				const uint16_t *xs;
				const uint16_t *ys;
				const gea::framework::graphics::pixel::native_t *colors;
				int count;
				int radius;
				uint8_t alpha;
			} fillCirclesRgb565;
			struct {
				// Entries pre-packed (and depth-ordered) by the canvas layer.
				const gea::framework::graphics::TriangleEntry *entries;
				int count;
				uint8_t alpha;
			} fillTrianglesRgb565;
		struct {
			const gea::framework::graphics::pixel::native_t *pixels;
			const uint8_t *alphaPixels;
			int srcWidth;
			int srcHeight;
			int x;
			int y;
			uint8_t alpha;
		} drawImage;
		struct {
			const gea::framework::graphics::pixel::native_t *pixels;
			const uint8_t *alphaPixels;
			int srcWidth;
			int srcHeight;
			int x;
			int y;
			int w;
			int h;
			uint8_t alpha;
			// Corner radius applied to the DESTINATION box, all four corners.
			// min(w,h)/2 is a circle, which is how an opaque (alpha-free) sprite
			// gets a round mask without carrying an alpha plane: the blit skips
			// the outside-the-shape pixels instead of blending them.
			int radius;
		} drawImageScaled;
		struct {
			const gea::framework::graphics::pixel::native_t *pixels;
			const uint8_t *alphaPixels;
			int srcWidth;
			int srcHeight;
			int x;
			int y;
			int w;
			int h;
			uint8_t alpha;
		} drawImageRotated90CW;
		struct {
			const gea::framework::graphics::pixel::native_t *pixels;
			const uint8_t *alphaPixels;
			int srcWidth;
			int srcHeight;
			int x;
			int y;
			int w;
			uint8_t alpha;
		} drawImageTiledX;
		struct {
			const char *text;
			int x;
			int y;
			gea::framework::graphics::pixel::native_t color;
			float scale;
			int fontFamilyId;
			int fontSizePx;
			uint8_t alpha;
		} fillText;
	};
};

// Inclusive rect, half-open is fine if x1 < x0 (treated as empty by the
// driver). Used for multi-rect framebuffer flush.
struct DisplayFlushRect {
	int x0;
	int y0;
	int x1;
	int y1;
};

// The raster callback fills a NATIVE pixel row for a streamed region. This is
// native_t (identical to uint16_t on RGB565 targets — a no-op there); on a
// grayscale GRAY4/GRAY2 target it is the 1-byte gray value, so no RGB565 is
// dragged through the streaming path.
using DisplayStreamRasterFn = void (*)(gea::framework::graphics::pixel::native_t *pixels,
                                       int width,
                                       int height,
                                       int origin_x,
                                       int origin_y,
                                       void *user);

class Display {
public:
	static bool init();
	static bool start();
	static gea::framework::graphics::Canvas *canvas();
	// True when canvas() is the panel's scanout framebuffer (no logical->panel
	// rotation): writes to it appear on the next refresh with no flush/rotate.
	// False when an orientation forces a separate logical framebuffer that must
	// be PPA-rotated to the panel each flush. Defaults to false on targets that
	// don't override it.
	static bool framebufferIsPanelDirect();
	// If the panel has a portrait scanout buffer a producer (e.g. the camera) can
	// PPA/DMA straight into for a self-owned rect — no off-screen compose + re-blit —
	// return it plus its width/height and true. False when the UI composes off-screen
	// and a flush/rotate is required. Default false; only the DSI panel overrides it.
	static bool panelScanoutSurface(uint16_t **out_buffer, int *out_width, int *out_height);
	// One-pass camera→panel target. Given a logical rect (the viewfinder), returns the
	// live panel scanout buffer + its pic dims, the panel rect that logical rect maps to
	// under the current orientation, and the orientation's rotation in 90° steps (0..3).
	// A producer (the camera) composes its own source rotation with out_rot_steps and
	// scales+rotates straight into the panel in ONE PPA pass — no off-screen rotate-to-
	// panel second pass. Available in any orientation. Default false on targets without it.
	// out_flip is set to 1 when the producer covers the whole panel and a second
	// framebuffer exists: out_buffer is then the non-scanned (back) buffer, and the
	// producer must call flipPanelToBack() after writing it to swap it in tear-free at
	// vblank. out_flip = 0 means out_buffer is the live scanned buffer (write in place).
	static bool panelDirectTarget(int logical_x, int logical_y, int logical_w, int logical_h,
	                              uint16_t **out_buffer, int *out_buffer_w, int *out_buffer_h,
	                              int *out_panel_x, int *out_panel_y, int *out_panel_w, int *out_panel_h,
	                              int *out_rot_steps, int *out_flip);
	// Swap the back framebuffer (just written by a full-panel producer) in at the next
	// vblank. No-op on single-framebuffer targets. The pointer overload flips the
	// exact buffer returned by panelDirectTarget(); use it when a long producer could
	// let the writable-buffer choice change before the flip.
	static void flipPanelToBack();
	static void flipPanelToBack(uint16_t *buffer);
	static bool copySnapshotRgb565(uint16_t *dst, int pixel_capacity, int *width, int *height, bool presented);
	// Same snapshot, but in the board's NATIVE sub-byte-PACKED storage (GRAY4/
	// GRAY2 targets only -- GEA_PIXEL_STORAGE_PACKED) instead of expanded RGB565.
	// A packed panel's true framebuffer is a quarter (GRAY4) or eighth (GRAY2)
	// the size of an RGB565 expansion of the same pixels; a caller that needs
	// RGB565 (the wire format screenshot commands emit) allocates this small
	// buffer and expands per-pixel itself while it encodes, instead of asking
	// the device to hold a full RGB565-sized allocation it may not have. `dst`
	// is a flat, unpadded packed pixel STREAM: pixel index i (row-major,
	// i = y*width+x) sits at pixel::packed::byteIndex(i)/shiftFor(i), with no
	// per-row alignment gap -- so byte_capacity need only be
	// pixel::packed::rowBytes(width*height). Only implemented on
	// GEA_PIXEL_STORAGE_PACKED boards; RGB565/8888 targets never call it.
	static bool copySnapshotPacked(uint8_t *dst, int byte_capacity, int *width, int *height, bool presented);
	static int countNonBlackPixels(bool presented);
	static void clear();
	static void clearNoFlush();
	static void print(const char *text);
	static void flush();
	// Transmit `count` framebuffer rects to the panel inside one driver
	// entry. Each rect is independently chunked / windowed (we can't merge
	// without re-transmitting between rects), but we share the call boundary
	// so callers that already track multiple dirty rects (tree_render's
	// per-region replay loop) don't pay 16× the per-call overhead. After a
	// successful return the canvas dirty rect is cleared; passing rects
	// outside the framebuffer is silently clipped.
	static void flushRects(const DisplayFlushRect *rects, int count, bool allowPerChunkDrain = true);
	// Fused flush: instead of copying each window from the framebuffer, invoke `raster`
	// to generate each DMA chunk in place (the render layer rebinds the draw canvas onto
	// the chunk buffer and replays the dirty display-list into it). Skips the PSRAM
	// framebuffer round-trip and overlaps rasterization with DMA. Call
	// rebindCanvasToFramebuffer() afterward to restore the canvas binding.
	static void flushRectsRasterized(const DisplayFlushRect *rects, int count, DisplayStreamRasterFn raster, void *user, bool allowPerChunkDrain = true);
	static void rebindCanvasToFramebuffer();
	static bool streamRect(int x, int y, int w, int h, DisplayStreamRasterFn raster, void *user);
	static bool present(const DisplayPresentCommand *commands, int command_count);
	static void setFlushConfig(int chunk_rows, int queue_depth);
	// Present render scale: 1 = native, 2 = the app renders into a half-panel
	// viewport and present() 2x-upscales it to the panel (quarters fill/overdraw).
	// Per-present, so it never affects full-res apps sharing the present path.
	// Only the ESP32 backend implements the upscale; other targets no-op.
	static void setPresentScale(int scale);
	// Reserve `bytes` of internal RAM by shrinking the elastic staging budget
	// (release with reserveInternal(0)). Used by the connectivity layer so WiFi/
	// BLE bring-up has headroom; the staging resize is applied on the frame task.
	static void reserveInternal(std::size_t bytes);
	// Apply a pending reserveInternal() staging resize. Call once per frame on the
	// frame task: the resize otherwise only lands inside a content flush, so a
	// static screen (no dirty region to flush) would never free the reserved RAM.
	static void applyPendingInternalReserve();
	static int flushChunkRows();
	static int flushQueueDepth();
	static int flushBufferBytes();
	static void pushClip(int x, int y, int w, int h);
	static void popClip();
	static void resetClip();
	static void setAlpha(uint8_t alpha);
	static uint8_t alpha();
	static void setAA(int samples);
	static int aa();
	static int brightness();
	static void setBrightness(int brightness_percent);
	// High-brightness mode: a separate, higher ceiling than the 0-100 brightness
	// range (a sunlight boost on AMOLED controllers that have one). Deliberately
	// a toggle, not brightness > 100, so a slider can never reach it. Returns
	// false where the panel has no such mode.
	static bool setHighBrightnessMode(bool enabled);
	static bool highBrightnessMode();
	// Opt-in tearing sync (TE/VBlank). Off by default. When enabled, the frame
	// scheduler calls vsyncWaitForFrame() at the top of each frame so the app's
	// draw+flush lands inside the panel's VBlank lead window. No-op on targets
	// that don't wire a TE line. Toggled from TS via Display.setVSync(boolean).
	static void setVSync(bool on);
	static bool vsyncEnabled();
	// Mark the next present as a full-screen damage-all: skip the dirty-rect diff
	// and the persistent previous-frame copy for that frame. Toggled from TS via
	// Display.invalidate() while panning. No-op on targets without a present diff.
	static void invalidate();
	static void vsyncWaitForFrame();
	static void clip(int *x0, int *y0, int *x1, int *y1);
	static void fillRect(int x, int y, int w, int h, gea::framework::graphics::pixel::native_t color);
	static void scrollRect(int x, int y, int w, int h, int dx, int dy);
	// Drop any active software-scroll-register state. Called by Tree::clear()
	// when the tree is torn down (app switch / mount of a fresh tree) so the
	// next app's drawing isn't translated through the previous app's region.
	// No-op on platforms / native-test stubs that don't implement the
	// software scroll register.
	static void resetScrollRegion();
	static void strokeRect(int x, int y, int w, int h, gea::framework::graphics::pixel::native_t color);
	static void fillCircle(int cx, int cy, int r, gea::framework::graphics::pixel::native_t color);
	static void strokeCircle(int cx, int cy, int r, gea::framework::graphics::pixel::native_t color);
	static void drawLine(int x0, int y0, int x1, int y1, gea::framework::graphics::pixel::native_t color);
	static void drawArc(int cx, int cy, int r, int start_deg, int end_deg, gea::framework::graphics::pixel::native_t color);
	static void fillTriangle(int x0, int y0, int x1, int y1, int x2, int y2, gea::framework::graphics::pixel::native_t color);
	static void drawText(const char *text, int x, int y, gea::framework::graphics::pixel::native_t color, float scale);
	static void drawTextFont(const char *text, int x, int y, gea::framework::graphics::pixel::native_t color, int font_id);
	static void drawTextFontFamily(const char *text, int x, int y, gea::framework::graphics::pixel::native_t color, int family_id, int size_px);
	static void setPixel(int x, int y, gea::framework::graphics::pixel::native_t color);
	static void fillRoundedRect(int x, int y, int w, int h, int tl, int tr, int br, int bl, gea::framework::graphics::pixel::native_t color);
	static void fillRoundedRectBoxesRgb565(const int16_t *xs, const int16_t *ys, int count,
	                                       int w, int h, int tl, int tr, int br, int bl,
	                                       const gea::framework::graphics::pixel::native_t *colors);
	static void strokeRoundedRect(int x, int y, int w, int h, int tl, int tr, int br, int bl, int lw, gea::framework::graphics::pixel::native_t color);
	static void blitImage(const gea::framework::graphics::pixel::native_t *src, const uint8_t *alpha, int src_w, int src_h, int dx, int dy);
	static void blitImageScaled(const gea::framework::graphics::pixel::native_t *src, const uint8_t *alpha, int src_w, int src_h, int dx, int dy, int dst_w, int dst_h);
	static void setWorldOverlay(const uint16_t *world_pixels, int world_width, int world_height, int panel_top, int panel_height);
	static void setWorldScroll(int scroll_x);
	static void flushStatsRead(int64_t *total_us, int *call_count, int *pixel_count);
	static DisplayFlushPerfStats flushPerfStatsRead();
	// Cumulative flush odometer, never reset per frame -- lets a caller watch for
	// the panel going quiet after an interaction (flushPerfStatsRead is zeroed
	// every frame, so it cannot answer that).
	static void flushOdometerRead(uint32_t &calls, uint64_t &pixels);
	// Present-path diagnostics for GEADEV (counters + last tile-shape
	// ineligibility); boards without a batched present leave outputs zero.
	static void presentPathDebug(int *calls, int *direct, int *general, int *rejected,
	                             int *tileShapeFailKind, int *tileShapeFailType, int *tileShapeFailCount);
	static void landFrameDebug(int *total, int *align, int *raster, int *text, int *flip);
	static void landPanDebug(int *detect, int *kick, int *strips, int *wait, int *interior);
	static void flushStatsReset();
	// Diagnostic: name of the current display flush sub-stage, plus the chunk
	// or region index in flight. Returned name is a static string. Safe to
	// call from any task (atomic load).
	static const char *flushStageName();
	static int flushStageChunk();
	static DisplayFlushStageDetail flushStageDetail();
};

}  // namespace gea::platform::display

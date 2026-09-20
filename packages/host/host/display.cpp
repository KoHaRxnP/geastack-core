// SPDX-License-Identifier: Apache-2.0
#include "gea/embedded-host.h"
#include "canvas.h"
#include "display.h"
#include "host/display_orientation.h"

namespace gea::framework::display {

namespace {

namespace pixel = gea::framework::graphics::pixel;

pixel::Format &activePixelFormat()
{
  static pixel::Format format =
#if GEA_EMBEDDED_PIXEL_FORMAT == GEA_PIXEL_RGBA8888
      pixel::Format::Rgba8888;
#elif GEA_EMBEDDED_PIXEL_FORMAT == GEA_PIXEL_ARGB8888
      pixel::Format::Argb8888;
#else
      pixel::Format::Rgb565;
#endif
  return format;
}

bool supportsPixelFormat(pixel::Format format)
{
#if GEA_EMBEDDED_PIXEL_FORMAT == GEA_PIXEL_RGBA8888
  return format == pixel::Format::Rgba8888;
#elif GEA_EMBEDDED_PIXEL_FORMAT == GEA_PIXEL_ARGB8888
  return format == pixel::Format::Argb8888;
#else
  return format == pixel::Format::Rgb565;
#endif
}

}  // namespace

double DisplayBackend::brightness() {
  return static_cast<double>(gea::platform::display::Display::brightness());
}

void DisplayBackend::setBrightness(double brightness) {
  gea::platform::display::Display::setBrightness(static_cast<int>(brightness));
}

void DisplayBackend::setAA(double samples) {
  gea::platform::display::Display::setAA(static_cast<int>(samples));
}

void DisplayBackend::setFlushConfig(double rows, double depth) {
  gea::platform::display::Display::setFlushConfig(static_cast<int>(rows), static_cast<int>(depth));
}

// E-paper refresh hooks. Weak no-op defaults so the host surface exists on
// every target; an e-paper board's display driver provides the strong
// definitions (the e-paper board's display.cpp in @geastack/targets).
extern "C" __attribute__((weak)) void gea_epaper_set_refresh_config(
    int full_refresh_every_partials,
    int full_refresh_hard_cap_partials,
    int fast_streak_window_ms,
    const std::uint8_t *partial_lut, int partial_lut_len,
    const std::uint8_t *fast_lut, int fast_lut_len)
{
  (void)full_refresh_every_partials;
  (void)full_refresh_hard_cap_partials;
  (void)fast_streak_window_ms;
  (void)partial_lut;
  (void)partial_lut_len;
  (void)fast_lut;
  (void)fast_lut_len;
}

extern "C" __attribute__((weak)) void gea_epaper_full_refresh(void) {}

extern "C" __attribute__((weak)) void gea_epaper_set_grayscale(int enabled)
{
  (void)enabled;
}

// Whether a partial update whose rect covers the whole screen is promoted to a
// full (flashing) refresh. Drivers default to promoting — a full-coverage
// GL16/A2 partial leaves ghosting on view switches — but a reading app whose
// every page turn covers the screen can opt out and rely on the
// fullRefreshEveryPartials cadence to clear ghosting instead.
extern "C" __attribute__((weak)) void gea_epaper_set_full_on_cover(int enabled)
{
  (void)enabled;
}

void DisplayBackend::setEpaperRefreshConfig(double fullRefreshEveryPartials,
                                            double fullRefreshHardCapPartials,
                                            double fastStreakWindowMs,
                                            const std::vector<std::uint8_t> &partialLut, bool hasPartialLut,
                                            const std::vector<std::uint8_t> &fastLut, bool hasFastLut) {
  gea_epaper_set_refresh_config(static_cast<int>(fullRefreshEveryPartials),
                                static_cast<int>(fullRefreshHardCapPartials),
                                static_cast<int>(fastStreakWindowMs),
                                hasPartialLut ? partialLut.data() : nullptr,
                                hasPartialLut ? static_cast<int>(partialLut.size()) : -1,
                                hasFastLut ? fastLut.data() : nullptr,
                                hasFastLut ? static_cast<int>(fastLut.size()) : -1);
}

void DisplayBackend::epaperFullRefresh() {
  gea_epaper_full_refresh();
}

void DisplayBackend::setEpaperGrayscale(bool enabled) {
  gea_epaper_set_grayscale(enabled ? 1 : 0);
}

void DisplayBackend::setEpaperFullOnCover(bool enabled) {
  gea_epaper_set_full_on_cover(enabled ? 1 : 0);
}

double DisplayBackend::nativeWidth() {
  return static_cast<double>(detail::DisplayOrientationState::nativeWidth());
}

double DisplayBackend::nativeHeight() {
  return static_cast<double>(detail::DisplayOrientationState::nativeHeight());
}

std::string DisplayBackend::orientation() {
  return detail::DisplayOrientationState::orientationString();
}

void DisplayBackend::setOrientation(const std::string &orientation) {
  detail::DisplayOrientationState::setOrientation(orientation);
}

std::vector<std::string> DisplayBackend::supportedOrientations() {
  return detail::DisplayOrientationState::supportedOrientations();
}

void DisplayBackend::setSupportedOrientations(const std::vector<std::string> &orientations) {
  detail::DisplayOrientationState::setSupportedOrientations(orientations);
}

void DisplayBackend::setSupportedOrientations(const std::string &orientation) {
  detail::DisplayOrientationState::setSupportedOrientations(orientation);
}

bool DisplayBackend::autoRotate() {
  return detail::DisplayOrientationState::autoRotate();
}

void DisplayBackend::setAutoRotate(bool enabled) {
  detail::DisplayOrientationState::setAutoRotate(enabled);
}

void DisplayBackend::setVSync(bool on) {
  gea::platform::display::Display::setVSync(on);
}

void DisplayBackend::setTextRasterCache(bool on) {
  // The text sprite cache is pure shared-rasterizer state — forward straight to
  // Canvas, no per-platform Display layer (unlike setVSync, which drives hardware TE).
  gea::framework::graphics::Canvas::setTextRasterCacheEnabled(on);
}

void DisplayBackend::setTextSolidBackdrop(int rrggbb) {
  // Pure shared-rasterizer state, same as setTextRasterCache.
  gea::framework::graphics::Canvas::setTextSolidBackdrop(rrggbb);
}

void DisplayBackend::invalidate() {
  gea::platform::display::Display::invalidate();
}

std::string DisplayBackend::pixelFormat() {
  return pixel::name(activePixelFormat());
}

void DisplayBackend::setPixelFormat(const std::string &format) {
  const pixel::Format requested = pixel::parseFormat(format.c_str(), activePixelFormat());
  if (supportsPixelFormat(requested)) activePixelFormat() = requested;
}

std::string DisplayBackend::panelPixelFormat() {
  return pixel::name(activePixelFormat());
}

std::vector<std::string> DisplayBackend::supportedPixelFormats() {
  return {pixel::name(activePixelFormat())};
}

void DisplayBackend::updateAutoRotationFromAccelerometer() {
  detail::DisplayOrientationState::updateAutoRotationFromAccelerometer();
}

void DisplayBackend::updateAutoRotation(double accelerationX, double accelerationY, double accelerationZ) {
  detail::DisplayOrientationState::updateAutoRotation(accelerationX, accelerationY, accelerationZ);
}

}  // namespace gea::framework::display

// Weak default for boards whose display has no batched-present diagnostics;
// the P4 backend provides the strong definition.
__attribute__((weak)) void gea::platform::display::Display::presentPathDebug(
    int *calls, int *direct, int *general, int *rejected,
    int *tileShapeFailKind, int *tileShapeFailType, int *tileShapeFailCount)
{
	*calls = 0;
	*direct = 0;
	*general = 0;
	*rejected = 0;
	*tileShapeFailKind = 0;
	*tileShapeFailType = 0;
	*tileShapeFailCount = 0;
}

__attribute__((weak)) void gea::platform::display::Display::landFrameDebug(int *total, int *align, int *raster, int *text, int *flip)
{
	*total = 0;
	*align = 0;
	*raster = 0;
	*text = 0;
	*flip = 0;
}

__attribute__((weak)) void gea::platform::display::Display::landPanDebug(int *detect, int *kick, int *strips, int *wait, int *interior)
{
	*detect = 0;
	*kick = 0;
	*strips = 0;
	*wait = 0;
	*interior = 0;
}

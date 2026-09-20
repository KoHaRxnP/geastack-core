// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "pixel.h"

#include <cstdint>

namespace gea::embedded::ui {

class StyleValues {
public:
	// Convert a raw style-value colour int into this board's native pixel, applied
	// once when the colour is written into node.style. The style-value int holds
	// the authoring colour in the board's pre-panel form: raw (unswapped) RGB565 on
	// 16-bit panels — swapped to panel byte order here — or the target's 8888 layout on full-colour
	// boards (no panel concept, identity). After this, node.style colours are native
	// and nothing downstream converts.
	static gea::framework::graphics::pixel::native_t pixelFromStyleValue(int value)
	{
#if GEA_PIXEL_FORMAT_IS_8888 || GEA_PIXEL_FORMAT_IS_GRAY
		// No panel byte-swap: the style value already IS the final native pixel
		// (RGBA8888, or a gray level — 0..15 GRAY4 / 0..3 GRAY2 — from
		// nativeStyleValue). Identity.
		return static_cast<gea::framework::graphics::pixel::native_t>(static_cast<std::uint32_t>(value));
#else
		return gea::framework::graphics::pixel::fromRgb565((uint16_t)value);
#endif
	}
};

}  // namespace gea::embedded::ui

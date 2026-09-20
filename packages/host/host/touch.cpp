// SPDX-License-Identifier: Apache-2.0
// Firmware-side touch host binding. The framework owns the touch-facing shape;
// the generated ABI entry point below only adapts current compiler output.

#include "gea/embedded-host.h"
#include "touch.h"

namespace gea::framework::host {

class TouchSurface {
public:
	static gea::host::TouchSample read()
	{
		int x = 0;
		int y = 0;
		const int touching = platform::touch::Touchscreen::readCached(&x, &y);
		return {touching != 0, static_cast<double>(x), static_cast<double>(y)};
	}
};

}  // namespace gea::framework::host

gea::host::TouchSample gea::host::Touch::read() const {
  return gea::framework::host::TouchSurface::read();
}

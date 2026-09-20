// SPDX-License-Identifier: Apache-2.0
#include "viewport_region.h"

#include "display.h"
#include "internal.h"

namespace gea::embedded::ui {

int ViewportRegion::clipRect(int width, int height, int *x, int *y, int *w, int *h)
{
	if (*w <= 0 || *h <= 0 || width <= 0 || height <= 0) return 0;
	int x0 = *x;
	int y0 = *y;
	int x1 = *x + *w - 1;
	int y1 = *y + *h - 1;
	if (x0 < 0) x0 = 0;
	if (y0 < 0) y0 = 0;
	if (x1 >= width) x1 = width - 1;
	if (y1 >= height) y1 = height - 1;
	if (x0 > x1 || y0 > y1) return 0;
	*x = x0;
	*y = y0;
	*w = x1 - x0 + 1;
	*h = y1 - y0 + 1;
	return 1;
}

void ViewportRegion::replayDisplayRegion(int x, int y, int w, int h)
{
	if (w <= 0 || h <= 0) return;
	gea::platform::display::Display::pushClip(x, y, w, h);
	DisplayList::instance().replay();
	gea::platform::display::Display::popClip();
}

}  // namespace gea::embedded::ui

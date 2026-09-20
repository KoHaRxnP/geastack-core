// SPDX-License-Identifier: Apache-2.0
#pragma once

namespace gea::embedded::ui {

class ViewportRegion {
public:
	static int clipRect(int width, int height, int *x, int *y, int *w, int *h);
	static void replayDisplayRegion(int x, int y, int w, int h);
};

}  // namespace gea::embedded::ui

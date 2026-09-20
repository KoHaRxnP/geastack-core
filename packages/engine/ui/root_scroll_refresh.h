// SPDX-License-Identifier: Apache-2.0
#pragma once

namespace gea::embedded::ui {

class RootScrollOnlyRefresh {
public:
	struct DirtyNode {
		int id;
		int x0;
		int y0;
		int x1;
		int y1;
	};

	static int refresh(int root, int width, int height);

private:
	static int refreshNode(int root,
	                       DirtyNode *extraDirtyNodes,
	                       int *extraDirtyCount,
	                       int *slotNodes,
	                       int *slotCount,
	                       int width,
	                       int height);
	static void shiftDescendantLayoutY(int id, int dy);
	static void shiftDescendantLayoutX(int id, int dx);
};

}  // namespace gea::embedded::ui

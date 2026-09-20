// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "node.h"

namespace gea::embedded::ui {

class ImageElement : public NodeHandle {
public:
	using NodeHandle::NodeHandle;
	static ImageElement create();
	void imageId(int id) const { style().imageId(id); }
	void fit(int value) const { style().imageFit(value); }
};

}  // namespace gea::embedded::ui

// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "node.h"

namespace gea::embedded::ui {

class ViewElement : public NodeHandle {
public:
	using NodeHandle::NodeHandle;
	static ViewElement create();
};

}  // namespace gea::embedded::ui

// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "node.h"

namespace gea::embedded::ui {

class TextElement : public NodeHandle {
public:
	using NodeHandle::NodeHandle;
	static TextElement create(const char *text = "");
	void textContent(const char *text) const { setText(text); }
};

}  // namespace gea::embedded::ui

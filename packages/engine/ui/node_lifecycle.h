// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "node_model.h"

namespace gea::embedded::ui {

class NodeLifecycle {
public:
	static void init(Node *node, NodeType type);
	static void resetStyle(ComputedStyle &style);
};

}  // namespace gea::embedded::ui

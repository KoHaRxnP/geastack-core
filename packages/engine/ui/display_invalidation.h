// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "style.h"

namespace gea::embedded::ui {

class DisplayInvalidation {
public:
	static bool retainsDisplayCommands(Property prop);
	static bool rebuildsNodeDisplayCommands(Property prop);
	static bool nodeDisplayChangeCanStayLocal(int node);
};

}  // namespace gea::embedded::ui

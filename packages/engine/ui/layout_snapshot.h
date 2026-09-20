// SPDX-License-Identifier: Apache-2.0
#pragma once

namespace gea::embedded::ui {

class LayoutSnapshot {
public:
	static void capture();
	static void markChangesDirty();
};

}  // namespace gea::embedded::ui

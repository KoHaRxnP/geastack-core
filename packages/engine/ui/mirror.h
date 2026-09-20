// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>

namespace gea::embedded::ui {

class Mirror {
public:
	static Mirror &instance();

	bool scrollDirtyAny() const;
	void copyScrollDirty(uint64_t *dst, int wordCount) const;
	void clearScrollDirty() const;
	bool nodeIsScrollable(int node) const;
	int scrollX(int node) const;
	void setScrollX(int node, int scrollX) const;
	int scrollY(int node) const;
	void setScrollY(int node, int scrollY) const;

private:
	Mirror() = default;
};

}  // namespace gea::embedded::ui

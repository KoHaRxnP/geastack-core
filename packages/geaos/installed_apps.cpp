// SPDX-License-Identifier: Apache-2.0
#include "apps.h"

namespace gea::framework::apps {

int InstalledApps::planCount()
{
	return generated::installedAppPlanCount();
}

const InstalledAppPlanEntry *InstalledApps::planAt(int index)
{
	return generated::installedAppPlanAt(index);
}

namespace generated {

__attribute__((weak)) int installedAppPlanCount()
{
	return 0;
}

__attribute__((weak)) const InstalledAppPlanEntry *installedAppPlanAt(int index)
{
	(void)index;
	return nullptr;
}

}  // namespace generated

}  // namespace gea::framework::apps

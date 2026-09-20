// SPDX-License-Identifier: Apache-2.0
#include "services/app_state.h"

#include "apps.h"
#include "services/mirror.h"

namespace gea::framework::services {

const char *AppState::currentAppId()
{
	return gea::framework::apps::AppManager::currentId();
}

void AppState::afterAppInit()
{
	MirrorService::clearDirty();
}

}  // namespace gea::framework::services

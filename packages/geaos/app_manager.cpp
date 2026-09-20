// SPDX-License-Identifier: Apache-2.0
#include "apps.h"

#include "event.h"
#include "services/frame_scheduler.h"

namespace gea::framework::apps {

namespace {

AppLauncherPlatform *platform_ = nullptr;

}  // namespace

void AppManager::setPlatform(AppLauncherPlatform *platform)
{
	platform_ = platform;
}

AppLauncherPlatform *AppManager::platform()
{
	return platform_;
}

const char *AppManager::currentId()
{
	return platform_ ? platform_->currentInstalledAppId() : nullptr;
}

bool AppManager::launch(const char *appId)
{
	if (!appId || appId[0] == '\0') return false;
	return platform_ ? platform_->launchInstalledApp(appId) : false;
}

bool AppManager::returnRunningAppToLauncher(const char *trigger)
{
	(void)trigger;
	return platform_ ? platform_->launchInstalledApp(launcherAppId()) : false;
}

bool AppManager::queueSettingsToggle()
{
	gea::framework::events::Event event{};
	event.type = gea::framework::events::EventType::SettingsToggle;
	return gea::framework::services::FrameScheduler::sendEvent(event);
}

bool AppManager::shouldEnableLauncherButton()
{
	return platform_ && !platform_->runningAppIsLauncher(launcherAppId());
}

void AppManager::startLauncherButtonTask()
{
	if (platform_) platform_->startLauncherButtonTask();
}

}  // namespace gea::framework::apps

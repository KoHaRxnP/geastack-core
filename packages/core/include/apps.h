#pragma once

namespace gea::framework::apps {

struct InstalledAppPlanEntry {
	const char *appId = nullptr;
	const char *slotLabel = nullptr;
};

class InstalledApps {
public:
	static int planCount();
	static const InstalledAppPlanEntry *planAt(int index);
};

class AppLauncherPlatform {
public:
	virtual ~AppLauncherPlatform() = default;

	virtual const char *currentInstalledAppId() = 0;
	virtual bool runningAppIsLauncher(const char *launcherAppId) = 0;
	virtual bool launchInstalledApp(const char *appId) = 0;
	virtual void startLauncherButtonTask() {}
};

class AppManager {
public:
	static constexpr const char *launcherAppId() { return "app-launcher"; }
	static void setPlatform(AppLauncherPlatform *platform);
	static AppLauncherPlatform *platform();
	static const char *currentId();
	static bool launch(const char *appId);
	static bool returnRunningAppToLauncher(const char *trigger);
	static bool queueSettingsToggle();
	static bool shouldEnableLauncherButton();
	static void startLauncherButtonTask();
};

namespace generated {

int installedAppPlanCount();
const InstalledAppPlanEntry *installedAppPlanAt(int index);

}

}  // namespace gea::framework::apps

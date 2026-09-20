#pragma once

namespace gea::framework::services {

class AppState {
public:
	static bool init();
	static void lock();
	static void unlock();
	static const char *currentAppId();
	static void afterAppInit();
};

}  // namespace gea::framework::services

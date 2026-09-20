#pragma once

namespace gea::framework::services {

class RuntimeLog {
public:
	static void printRuntimeBanner();
	static void appStarted(bool networkReady);
};

}  // namespace gea::framework::services

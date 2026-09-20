#pragma once

namespace gea::framework::services {

class BatteryService {
public:
	static void update();
	static void start();
	static void poll(int nowMs);
};

}  // namespace gea::framework::services

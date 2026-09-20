#pragma once

namespace gea::platform::power {

class Power {
public:
	static bool init();
	static int batteryPercent();
};

}  // namespace gea::platform::power

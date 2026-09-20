#pragma once

#include <stdint.h>

namespace gea::platform::sensors {

class Accelerometer {
public:
	static void init();
	static void close();
	static void calibrateBias();
	static int tiltX();
	static int tiltY();
	static double accelerationX();
	static double accelerationY();
	static double accelerationZ();
	static double gyroscopeX();
	static double gyroscopeY();
	static double gyroscopeZ();
	static void setWebTilt(int x, int y);
};

}  // namespace gea::platform::sensors

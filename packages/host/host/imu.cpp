// SPDX-License-Identifier: Apache-2.0
#include "imu.h"
#include "gea/embedded-host.h"

namespace gea::framework::sensors {

void AccelerometerBackend::init() {
  gea::platform::sensors::Accelerometer::init();
}

void AccelerometerBackend::close() {
  gea::platform::sensors::Accelerometer::close();
}

void AccelerometerBackend::calibrateBias() {
  gea::platform::sensors::Accelerometer::calibrateBias();
}

double AccelerometerBackend::tiltX() {
  return static_cast<double>(gea::platform::sensors::Accelerometer::tiltX());
}

double AccelerometerBackend::tiltY() {
  return static_cast<double>(gea::platform::sensors::Accelerometer::tiltY());
}

double AccelerometerBackend::accelerationX() {
  return gea::platform::sensors::Accelerometer::accelerationX();
}

double AccelerometerBackend::accelerationY() {
  return gea::platform::sensors::Accelerometer::accelerationY();
}

double AccelerometerBackend::accelerationZ() {
  return gea::platform::sensors::Accelerometer::accelerationZ();
}

double AccelerometerBackend::gyroscopeX() {
  return gea::platform::sensors::Accelerometer::gyroscopeX();
}

double AccelerometerBackend::gyroscopeY() {
  return gea::platform::sensors::Accelerometer::gyroscopeY();
}

double AccelerometerBackend::gyroscopeZ() {
  return gea::platform::sensors::Accelerometer::gyroscopeZ();
}

}  // namespace gea::framework::sensors

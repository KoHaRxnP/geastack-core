// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstddef>
#include <cstdint>

namespace gea::platform::sensors {
struct MagnetometerReading {
  // Compensated whole microtesla. BMM150 overflow sentinels are preserved;
  // saturatedAxes identifies axes that must not be used as field strength.
  std::int16_t x = 0, y = 0, z = 0;
  bool valid = false;
  std::uint8_t saturatedAxes = 0; // X=1, Y=2, Z=4.
  std::uint64_t timestampUs = 0;
};
} // namespace gea::platform::sensors

extern "C" {
bool gea_magnetometer_open() __attribute__((weak));
void gea_magnetometer_close() __attribute__((weak));
unsigned gea_magnetometer_count() __attribute__((weak));
bool gea_magnetometer_read(unsigned,
                           gea::platform::sensors::MagnetometerReading *)
    __attribute__((weak));
bool gea_magnetometer_read_all(gea::platform::sensors::MagnetometerReading *,
                               std::size_t) __attribute__((weak));
}

namespace gea::platform::sensors {
class Magnetometer {
public:
  // Idempotent. No bus or pins are claimed until open is called.
  static bool open() {
    return gea_magnetometer_open && gea_magnetometer_open();
  }
  static void close() {
    if (gea_magnetometer_close)
      gea_magnetometer_close();
  }
  static unsigned count() {
    return gea_magnetometer_count ? gea_magnetometer_count() : 0;
  }
  static bool read(unsigned index, MagnetometerReading &sample) {
    sample = {};
    return gea_magnetometer_read && gea_magnetometer_read(index, &sample);
  }
  // Reads the board's sensors back-to-back under one driver lock, with a
  // shared completion timestamp. This is not hardware-triggered sampling.
  // False if capacity is insufficient or any sensor failed; inspect valid
  // on individual readings. The output is cleared even when unsupported.
  static bool readAll(MagnetometerReading *samples, std::size_t capacity) {
    if (!samples)
      return false;
    for (std::size_t i = 0; i < capacity; ++i)
      samples[i] = {};
    return gea_magnetometer_read_all &&
           gea_magnetometer_read_all(samples, capacity);
  }
};
} // namespace gea::platform::sensors

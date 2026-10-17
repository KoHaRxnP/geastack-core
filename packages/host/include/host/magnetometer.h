// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "platform/magnetometer.h"
#include <cmath>
#include <cstring>
#include <limits>
#include <type_traits>

namespace gea::host {
struct MagnetometerFacade {
  bool open() const { return gea::platform::sensors::Magnetometer::open(); }
  void close() const { gea::platform::sensors::Magnetometer::close(); }
  double count() const { return gea::platform::sensors::Magnetometer::count(); }
  template <class Values> bool read(double index, Values &&values) const {
    static_assert(
        std::is_same_v<std::remove_cvref_t<decltype(*values.data())>, double>);
    if (values.size() < 5)
      return false;
    // The compiler's const typed-array view still owns a mutable backing
    // buffer. Preserve its detached-buffer check and subarray byte offset.
    (void)values.data();
    auto *destination = [&]() {
      if constexpr (requires {
                      values.buffer();
                      values.byteOffset();
                    })
        return values.buffer()->data() +
               static_cast<std::size_t>(values.byteOffset());
      else {
        static_assert(!std::is_const_v<
                      std::remove_reference_t<decltype(*values.data())>>);
        return reinterpret_cast<std::uint8_t *>(values.data());
      }
    }();
    gea::platform::sensors::MagnetometerReading sample{};
    const bool valid = std::isfinite(index) && index >= 0 &&
                       index <= std::numeric_limits<unsigned>::max() &&
                       std::trunc(index) == index &&
                       gea::platform::sensors::Magnetometer::read(
                           static_cast<unsigned>(index), sample);
    const double result[] = {static_cast<double>(sample.x),
                             static_cast<double>(sample.y),
                             static_cast<double>(sample.z),
                             static_cast<double>(sample.saturatedAxes),
                             static_cast<double>(sample.timestampUs)};
    std::memcpy(destination, result, sizeof(result));
    return valid;
  }
};
inline constexpr MagnetometerFacade Magnetometer{};
} // namespace gea::host

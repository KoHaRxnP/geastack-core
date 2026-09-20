// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "power.h"

namespace gea::host {

// Battery state for app code. level() is the charge percentage (0-100) read from
// the platform PMU — gea::platform::power::Power::batteryPercent(), an I2C read of
// the AXP2101 on this board (not a flash op, so safe from any task, unlike NVS).
// Lets a watch face show a battery complication.
//
// Header-only: links from any TU including gea/embedded.h with no
// idf_component_register SRCS entry. The inline method is only instantiated where
// actually called, so targets without a Power impl are unaffected unless an app
// uses Battery.
struct BatteryFacade {
  double level() const {
    return static_cast<double>(gea::platform::power::Power::batteryPercent());
  }
};

inline constexpr BatteryFacade Battery{};

}  // namespace gea::host

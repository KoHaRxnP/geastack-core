// SPDX-License-Identifier: Apache-2.0
#include "services/battery_service.h"

#include "ble.h"
#include "power.h"

#include <cstdint>

namespace gea::framework::services {

void BatteryService::update()
{
	int pct = gea::platform::power::Power::batteryPercent();
	if (pct >= 0) {
		gea::framework::bluetooth::bluetooth().hid().setBatteryLevel(static_cast<std::uint8_t>(pct));
	}
}

}  // namespace gea::framework::services

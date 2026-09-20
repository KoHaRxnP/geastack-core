#include "power/eta6098/eta6098.h"

namespace gea::chips::eta6098 {

namespace {

double clampDouble(double value, double minValue, double maxValue) {
	if (value < minValue) return minValue;
	if (value > maxValue) return maxValue;
	return value;
}

}  // namespace

PowerManagementUnit::PowerManagementUnit(PowerIo &io, BatteryAdcConfig config) : io_(io), config_(config) {}

bool PowerManagementUnit::begin() {
	return io_.setPowerHold(true);
}

bool PowerManagementUnit::shutdown() {
	return io_.setPowerHold(false);
}

bool PowerManagementUnit::powerKeyPressed() {
	bool pressed = false;
	return io_.readPowerKey(pressed) && pressed;
}

double PowerManagementUnit::batteryVoltageMv() {
	int raw = 0;
	if (!io_.readBatteryAdcRaw(raw) || config_.adcMax <= 0) return 0.0;
	const double normalized = clampDouble(static_cast<double>(raw) / static_cast<double>(config_.adcMax), 0.0, 1.0);
	return normalized * config_.referenceMv * config_.dividerRatio;
}

int PowerManagementUnit::batteryPercent() {
	const double voltage = batteryVoltageMv();
	if (voltage <= 0.0 || config_.fullMv <= config_.emptyMv) return -1;
	const double normalized = (voltage - config_.emptyMv) / (config_.fullMv - config_.emptyMv);
	return static_cast<int>(clampDouble(normalized, 0.0, 1.0) * 100.0 + 0.5);
}

}  // namespace gea::chips::eta6098

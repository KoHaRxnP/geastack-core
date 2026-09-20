#pragma once

#include <cstdint>

namespace gea::chips::eta6098 {

struct BatteryAdcConfig {
	double referenceMv = 3300.0;
	double dividerRatio = 2.0;
	int adcMax = 4095;
	double emptyMv = 3300.0;
	double fullMv = 4200.0;
};

class PowerIo {
public:
	virtual ~PowerIo() = default;
	virtual bool setPowerHold(bool enabled) = 0;
	virtual bool readPowerKey(bool &pressed) = 0;
	virtual bool readBatteryAdcRaw(int &raw) = 0;
};

class PowerManagementUnit {
public:
	PowerManagementUnit(PowerIo &io, BatteryAdcConfig config = {});

	bool begin();
	bool shutdown();
	bool powerKeyPressed();
	double batteryVoltageMv();
	int batteryPercent();

private:
	PowerIo &io_;
	BatteryAdcConfig config_;
};

}  // namespace gea::chips::eta6098

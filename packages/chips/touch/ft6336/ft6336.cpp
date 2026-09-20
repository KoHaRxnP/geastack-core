#include "touch/ft6336/ft6336.h"

namespace gea::chips::ft6336 {

namespace {

constexpr std::uint8_t kTouchCountRegister = 0x02;
constexpr std::uint8_t kGestureEnableRegister = 0xD0;
constexpr std::uint8_t kGestureEnable = 0x01;
constexpr std::uint8_t kChipIdRegister = 0xA3;

}  // namespace

bool ControllerCore::configure(RegisterBus &bus, bool gestureMode) {
	if (gestureMode && !bus.writeRegister(kGestureEnableRegister, kGestureEnable)) return false;
	std::uint8_t chipId = 0;
	return readChipId(bus, chipId) && chipId == kExpectedChipId;
}

bool ControllerCore::readChipId(RegisterBus &bus, std::uint8_t &chipId) {
	return bus.readRegister(kChipIdRegister, chipId);
}

bool ControllerCore::read(RegisterBus &bus, TouchSample &sample) {
	MultiTouchSample multi{};
	if (!readMulti(bus, multi)) {
		sample.touching = false;
		return false;
	}

	sample.touching = true;
	sample.x = multi.x[0];
	sample.y = multi.y[0];
	return true;
}

bool ControllerCore::readMulti(RegisterBus &bus, MultiTouchSample &sample) {
	std::uint8_t buf[13]{};
	if (!bus.readRegisters(kTouchCountRegister, buf, sizeof(buf))) {
		sample.count = 0;
		return false;
	}

	int count = buf[0] & 0x0F;
	if (count > kMaxTouchPoints) count = kMaxTouchPoints;
	if (count <= 0) {
		sample.count = 0;
		return false;
	}

	sample.count = count;
	sample.x[0] = ((buf[1] & 0x0F) << 8) | buf[2];
	sample.y[0] = ((buf[3] & 0x0F) << 8) | buf[4];
	if (count >= 2) {
		sample.x[1] = ((buf[7] & 0x0F) << 8) | buf[8];
		sample.y[1] = ((buf[9] & 0x0F) << 8) | buf[10];
	}
	return true;
}

}  // namespace gea::chips::ft6336

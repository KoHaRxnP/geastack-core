#include "touch/ft3168/ft3168.h"

namespace gea::chips::ft3168 {

namespace {

constexpr std::uint8_t kGestureThresholdRegister = 0x80;
constexpr std::uint8_t kInterruptModeRegister = 0x86;
constexpr std::uint8_t kReportRateRegister = 0x87;
constexpr std::uint8_t kTouchCountRegister = 0x02;

}  // namespace

bool ControllerCore::configure(RegisterBus &bus) {
	if (!bus.writeRegister(kGestureThresholdRegister, 128)) return false;
	if (!bus.writeRegister(kInterruptModeRegister, 1)) return false;
	return bus.writeRegister(kReportRateRegister, 10);
}

bool ControllerCore::read(RegisterBus &bus, TouchSample &sample) {
	// Single coalesced transaction: the touch-count register (0x02) is
	// immediately followed by the first touch point (0x03..0x06), so read all
	// five contiguous bytes in one I2C transaction instead of two — halves the
	// per-report bus overhead without changing the sampling rate.
	std::uint8_t buf[5]{};
	if (!bus.readRegisters(kTouchCountRegister, buf, sizeof(buf))) {
		sample.touching = false;
		return false;
	}

	if ((buf[0] & 0x0F) == 0) {
		sample.touching = false;
		return false;
	}

	sample.touching = true;
	sample.x = ((buf[1] & 0x0F) << 8) | buf[2];
	sample.y = ((buf[3] & 0x0F) << 8) | buf[4];
	return true;
}

bool ControllerCore::readMulti(RegisterBus &bus, MultiTouchSample &sample) {
	// 0x02 = touch count; FocalTech lays the two contacts out contiguously after
	// it: P1 at 0x03..0x06, P2 at 0x09..0x0C (XH/XL/YH/YL each, 12-bit; the high
	// nibble of XH/YH carries event-flag/touch-id bits). One 11-byte transaction.
	std::uint8_t buf[11]{};
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
	sample.x[0] = ((buf[1] & 0x0F) << 8) | buf[2];  // P1 (0x03..0x06)
	sample.y[0] = ((buf[3] & 0x0F) << 8) | buf[4];
	if (count >= 2) {
		sample.x[1] = ((buf[7] & 0x0F) << 8) | buf[8];  // P2 (0x09..0x0C)
		sample.y[1] = ((buf[9] & 0x0F) << 8) | buf[10];
	}
	return true;
}

}  // namespace gea::chips::ft3168

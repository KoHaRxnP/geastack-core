#pragma once

#include <cstddef>
#include <cstdint>

namespace gea::chips::ft3168 {

inline constexpr std::uint8_t kI2cAddress = 0x38;
inline constexpr std::uint32_t kI2cFrequencyHz = 400000;

struct TouchSample {
	bool touching = false;
	int x = 0;
	int y = 0;
};

// Up to two simultaneous contacts — all the gesture layer needs (pinch-zoom).
// Point 0 mirrors the single-touch TouchSample.
inline constexpr int kMaxTouchPoints = 2;
struct MultiTouchSample {
	int count = 0;
	int x[kMaxTouchPoints] = {0, 0};
	int y[kMaxTouchPoints] = {0, 0};
};

class RegisterBus {
public:
	virtual ~RegisterBus() = default;
	virtual bool writeRegister(std::uint8_t reg, std::uint8_t value) = 0;
	virtual bool readRegisters(std::uint8_t reg, std::uint8_t *data, std::size_t length) = 0;
};

class ControllerCore {
public:
	bool configure(RegisterBus &bus);
	bool read(RegisterBus &bus, TouchSample &sample);
	// Reads up to kMaxTouchPoints contacts in one transaction. Returns true if at
	// least one finger is down. Needed for pinch-zoom (two-finger gestures).
	bool readMulti(RegisterBus &bus, MultiTouchSample &sample);
};

}  // namespace gea::chips::ft3168

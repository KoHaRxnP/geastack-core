#pragma once

#include <cstddef>
#include <cstdint>

namespace gea::chips::ft6336 {

inline constexpr std::uint8_t kI2cAddress = 0x38;
inline constexpr std::uint32_t kI2cFrequencyHz = 400000;
inline constexpr std::uint8_t kExpectedChipId = 0x64;

struct TouchSample {
	bool touching = false;
	int x = 0;
	int y = 0;
};

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
	virtual bool readRegister(std::uint8_t reg, std::uint8_t &value) = 0;
	virtual bool readRegisters(std::uint8_t reg, std::uint8_t *data, std::size_t length) = 0;
};

class ControllerCore {
public:
	bool configure(RegisterBus &bus, bool gestureMode = false);
	bool readChipId(RegisterBus &bus, std::uint8_t &chipId);
	bool read(RegisterBus &bus, TouchSample &sample);
	bool readMulti(RegisterBus &bus, MultiTouchSample &sample);
};

}  // namespace gea::chips::ft6336

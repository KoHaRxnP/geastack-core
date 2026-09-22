#pragma once

#include <cstdint>

// WCH CH422G I2C I/O expander: eight push-pull/input pins (IO0-IO7) and four
// open-drain outputs (OC0-OC3). It has no register file: each function answers
// at its own fixed 7-bit address and takes a single data byte, so a "write" is
// one byte transmitted to the address of the function, and a "read" is one byte
// received from the read address.
namespace gea::chips::ch422g {

inline constexpr std::uint32_t kI2cFrequencyHz = 400000;

// The four fixed addresses, written as the datasheet's 8-bit codes shifted
// down to 7 bits.
inline constexpr std::uint8_t kAddressWriteSystem = 0x48 >> 1;     // 0x24: mode/parameter byte
inline constexpr std::uint8_t kAddressWriteOpenDrain = 0x46 >> 1;  // 0x23: OC0-OC3 levels
inline constexpr std::uint8_t kAddressWriteIo = 0x70 >> 1;         // 0x38: IO0-IO7 levels
inline constexpr std::uint8_t kAddressReadIo = 0x4d >> 1;          // 0x26: IO0-IO7 input levels

// System parameter bits.
inline constexpr std::uint8_t kSystemIoOutputEnable = 1 << 0;  // IO0-IO7 drive their level (else input)
inline constexpr std::uint8_t kSystemScanEnable = 1 << 1;      // segment-display scan mode (unused here)
inline constexpr std::uint8_t kSystemOpenDrainOutputs = 1 << 2;  // OC pins open-drain (else push-pull)
inline constexpr std::uint8_t kSystemSleep = 1 << 3;

inline constexpr int kIoPinCount = 8;
inline constexpr int kOpenDrainPinCount = 4;
// Pin numbering: 0-7 are IO0-IO7, 8-11 are OC0-OC3.
inline constexpr int kPinCount = kIoPinCount + kOpenDrainPinCount;

// Power-on state of the output latches.
inline constexpr std::uint8_t kDefaultIoLevels = 0xff;
inline constexpr std::uint8_t kDefaultOpenDrainLevels = 0x0f;

class Bus {
public:
	virtual ~Bus() = default;
	virtual bool write(std::uint8_t address, std::uint8_t value) = 0;
	virtual bool read(std::uint8_t address, std::uint8_t &value) = 0;
};

class Driver {
public:
	// Puts IO0-IO7 into output mode and latches the given levels. The chip
	// cannot be read back for its output latches, so the driver shadows them.
	bool configure(Bus &bus, std::uint8_t ioLevels = kDefaultIoLevels, std::uint8_t openDrainLevels = kDefaultOpenDrainLevels);
	bool setPin(Bus &bus, int pin, bool high);
	bool pin(int pin) const;
	bool readInputs(Bus &bus, std::uint8_t &levels);
	std::uint8_t ioLevels() const { return io_; }
	std::uint8_t openDrainLevels() const { return openDrain_; }

private:
	std::uint8_t system_ = kSystemIoOutputEnable;
	std::uint8_t io_ = kDefaultIoLevels;
	std::uint8_t openDrain_ = kDefaultOpenDrainLevels;
};

}  // namespace gea::chips::ch422g

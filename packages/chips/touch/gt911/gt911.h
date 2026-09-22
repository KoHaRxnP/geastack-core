#pragma once

#include <cstddef>
#include <cstdint>

// Goodix GT911 capacitive touch controller: the target-independent protocol.
// Register addresses are 16 bits wide and sent big-endian on the I2C wire; a
// platform binding supplies the bus and owns reset, interrupt and task delivery.
namespace gea::chips::gt911 {

// The controller samples its INT line while reset is released and picks one of
// two 7-bit addresses from it: INT low selects 0x5d, INT high selects 0x14. A
// binding that cannot drive INT during reset probes both.
inline constexpr std::uint8_t kI2cAddressPrimary = 0x5d;
inline constexpr std::uint8_t kI2cAddressSecondary = 0x14;
inline constexpr std::uint32_t kI2cFrequencyHz = 400000;

inline constexpr std::uint16_t kRegProductId = 0x8140;   // 4 ASCII bytes, "911"
inline constexpr std::uint16_t kRegStatus = 0x814e;      // bit7 = buffer ready, bits0-3 = touch count
inline constexpr std::uint16_t kRegPointData = 0x814f;   // 8 bytes per point, up to 5 points
inline constexpr int kPointStride = 8;
inline constexpr int kMaxReportedPoints = 5;

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
	virtual bool writeRegisters(std::uint16_t reg, const std::uint8_t *data, std::size_t length) = 0;
	virtual bool readRegisters(std::uint16_t reg, std::uint8_t *data, std::size_t length) = 0;
};

class ControllerCore {
public:
	// Reads the product id and checks it answers as a GT911. Returns false when
	// nothing at this address is a GT911 (the binding then tries the other one).
	bool configure(RegisterBus &bus);
	bool read(RegisterBus &bus, TouchSample &sample);
	// Reads up to kMaxTouchPoints contacts. The controller sets the buffer-ready
	// bit only when a new report is available; between reports the previous
	// sample is returned unchanged, so a caller polling faster than the report
	// rate never sees a phantom lift. Returns true if at least one finger is down.
	bool readMulti(RegisterBus &bus, MultiTouchSample &sample);

private:
	MultiTouchSample last_ = {};
};

}  // namespace gea::chips::gt911

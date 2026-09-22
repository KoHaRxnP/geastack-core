#include "ch422g.h"

namespace gea::chips::ch422g {

bool Driver::configure(Bus &bus, std::uint8_t ioLevels, std::uint8_t openDrainLevels) {
	system_ = kSystemIoOutputEnable;
	if (!bus.write(kAddressWriteSystem, system_)) return false;
	io_ = ioLevels;
	openDrain_ = openDrainLevels;
	if (!bus.write(kAddressWriteIo, io_)) return false;
	return bus.write(kAddressWriteOpenDrain, openDrain_);
}

bool Driver::setPin(Bus &bus, int pin, bool high) {
	if (pin < 0 || pin >= kPinCount) return false;
	if (pin < kIoPinCount) {
		const std::uint8_t mask = static_cast<std::uint8_t>(1u << pin);
		const std::uint8_t next = high ? static_cast<std::uint8_t>(io_ | mask) : static_cast<std::uint8_t>(io_ & ~mask);
		if (!bus.write(kAddressWriteIo, next)) return false;
		io_ = next;
		return true;
	}
	const std::uint8_t mask = static_cast<std::uint8_t>(1u << (pin - kIoPinCount));
	const std::uint8_t next = high ? static_cast<std::uint8_t>(openDrain_ | mask) : static_cast<std::uint8_t>(openDrain_ & ~mask);
	if (!bus.write(kAddressWriteOpenDrain, next)) return false;
	openDrain_ = next;
	return true;
}

bool Driver::pin(int pin) const {
	if (pin < 0 || pin >= kPinCount) return false;
	if (pin < kIoPinCount) return (io_ >> pin) & 1u;
	return (openDrain_ >> (pin - kIoPinCount)) & 1u;
}

bool Driver::readInputs(Bus &bus, std::uint8_t &levels) {
	return bus.read(kAddressReadIo, levels);
}

}  // namespace gea::chips::ch422g

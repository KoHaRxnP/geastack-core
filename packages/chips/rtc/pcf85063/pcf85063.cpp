#include "rtc/pcf85063/pcf85063.h"

namespace gea::chips::pcf85063 {

namespace {

constexpr std::uint8_t kControl1 = 0x00;
constexpr std::uint8_t kControl2 = 0x01;
constexpr std::uint8_t kSeconds = 0x04;
constexpr std::uint8_t kDays = 0x07;
constexpr std::uint8_t kSecondAlarm = 0x0B;

constexpr std::uint8_t kControl1CapSel = 0x01;
constexpr std::uint8_t kControl1SoftwareReset = 0x10;
constexpr std::uint8_t kControl2AlarmInterruptEnable = 0x80;
constexpr std::uint8_t kControl2AlarmFlag = 0x40;
constexpr std::uint8_t kAlarmDisable = 0x80;

int clamp(int value, int minValue, int maxValue) {
	if (value < minValue) return minValue;
	if (value > maxValue) return maxValue;
	return value;
}

}  // namespace

RealTimeClock::RealTimeClock(RegisterBus &bus, int yearOffset) : bus_(bus), yearOffset_(yearOffset) {}

bool RealTimeClock::begin() {
	return bus_.writeRegister(kControl1, kControl1CapSel);
}

bool RealTimeClock::reset() {
	return bus_.writeRegister(kControl1, kControl1CapSel | kControl1SoftwareReset);
}

bool RealTimeClock::setTime(const DateTime &time) {
	const std::uint8_t data[] = {
	    decToBcd(clamp(time.second, 0, 59)),
	    decToBcd(clamp(time.minute, 0, 59)),
	    decToBcd(clamp(time.hour, 0, 23)),
	};
	return bus_.writeRegisters(kSeconds, data, sizeof(data));
}

bool RealTimeClock::setDate(const DateTime &time) {
	const std::uint8_t data[] = {
	    decToBcd(clamp(time.day, 1, 31)),
	    decToBcd(clamp(time.weekday, 0, 6)),
	    decToBcd(clamp(time.month, 1, 12)),
	    decToBcd(clamp(time.year - yearOffset_, 0, 99)),
	};
	return bus_.writeRegisters(kDays, data, sizeof(data));
}

bool RealTimeClock::setDateTime(const DateTime &time) {
	const std::uint8_t data[] = {
	    decToBcd(clamp(time.second, 0, 59)),
	    decToBcd(clamp(time.minute, 0, 59)),
	    decToBcd(clamp(time.hour, 0, 23)),
	    decToBcd(clamp(time.day, 1, 31)),
	    decToBcd(clamp(time.weekday, 0, 6)),
	    decToBcd(clamp(time.month, 1, 12)),
	    decToBcd(clamp(time.year - yearOffset_, 0, 99)),
	};
	return bus_.writeRegisters(kSeconds, data, sizeof(data));
}

bool RealTimeClock::readDateTime(DateTime &time) {
	std::uint8_t data[7]{};
	if (!bus_.readRegisters(kSeconds, data, sizeof(data))) return false;

	time.second = bcdToDec(data[0] & 0x7F);
	time.minute = bcdToDec(data[1] & 0x7F);
	time.hour = bcdToDec(data[2] & 0x3F);
	time.day = bcdToDec(data[3] & 0x3F);
	time.weekday = bcdToDec(data[4] & 0x07);
	time.month = bcdToDec(data[5] & 0x1F);
	time.year = bcdToDec(data[6]) + yearOffset_;
	return true;
}

bool RealTimeClock::enableAlarm() {
	return bus_.writeRegister(kControl2, kControl2AlarmInterruptEnable);
}

bool RealTimeClock::disableAlarm() {
	std::uint8_t value = 0;
	if (!bus_.readRegister(kControl2, value)) return false;
	value &= static_cast<std::uint8_t>(~(kControl2AlarmInterruptEnable | kControl2AlarmFlag));
	return bus_.writeRegister(kControl2, value);
}

bool RealTimeClock::clearAlarmFlag() {
	std::uint8_t value = 0;
	if (!bus_.readRegister(kControl2, value)) return false;
	value &= static_cast<std::uint8_t>(~kControl2AlarmFlag);
	return bus_.writeRegister(kControl2, value);
}

bool RealTimeClock::alarmFlag(bool &active) {
	std::uint8_t value = 0;
	if (!bus_.readRegister(kControl2, value)) return false;
	active = (value & kControl2AlarmFlag) != 0;
	return true;
}

bool RealTimeClock::setAlarm(const DateTime &time) {
	const std::uint8_t data[] = {
	    decToBcd(clamp(time.second, 0, 59)),
	    decToBcd(clamp(time.minute, 0, 59)),
	    decToBcd(clamp(time.hour, 0, 23)),
	    kAlarmDisable,
	    kAlarmDisable,
	};
	return bus_.writeRegisters(kSecondAlarm, data, sizeof(data));
}

std::uint8_t RealTimeClock::decToBcd(int value) {
	value = clamp(value, 0, 99);
	return static_cast<std::uint8_t>(((value / 10) << 4) | (value % 10));
}

int RealTimeClock::bcdToDec(std::uint8_t value) {
	return ((value >> 4) * 10) + (value & 0x0F);
}

}  // namespace gea::chips::pcf85063

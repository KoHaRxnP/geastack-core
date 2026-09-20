#pragma once

#include <cstddef>
#include <cstdint>

namespace gea::chips::pcf85063 {

inline constexpr std::uint8_t kI2cAddress = 0x51;
inline constexpr std::uint32_t kI2cFrequencyHz = 400000;
inline constexpr int kDefaultYearOffset = 1970;

struct DateTime {
	int year = kDefaultYearOffset;
	int month = 1;
	int day = 1;
	int weekday = 0;
	int hour = 0;
	int minute = 0;
	int second = 0;
};

class RegisterBus {
public:
	virtual ~RegisterBus() = default;
	virtual bool writeRegister(std::uint8_t reg, std::uint8_t value) = 0;
	virtual bool writeRegisters(std::uint8_t reg, const std::uint8_t *data, std::size_t length) = 0;
	virtual bool readRegister(std::uint8_t reg, std::uint8_t &value) = 0;
	virtual bool readRegisters(std::uint8_t reg, std::uint8_t *data, std::size_t length) = 0;
};

class RealTimeClock {
public:
	explicit RealTimeClock(RegisterBus &bus, int yearOffset = kDefaultYearOffset);

	bool begin();
	bool reset();
	bool setTime(const DateTime &time);
	bool setDate(const DateTime &time);
	bool setDateTime(const DateTime &time);
	bool readDateTime(DateTime &time);

	bool enableAlarm();
	bool disableAlarm();
	bool clearAlarmFlag();
	bool alarmFlag(bool &active);
	bool setAlarm(const DateTime &time);

private:
	static std::uint8_t decToBcd(int value);
	static int bcdToDec(std::uint8_t value);

	RegisterBus &bus_;
	int yearOffset_;
};

}  // namespace gea::chips::pcf85063

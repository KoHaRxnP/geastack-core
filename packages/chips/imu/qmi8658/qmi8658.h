#pragma once

#include <cstddef>
#include <cstdint>

namespace gea::chips::qmi8658 {

inline constexpr std::uint8_t kI2cAddress = 0x6B;
inline constexpr std::uint32_t kI2cFrequencyHz = 400000;

class RegisterBus {
public:
	virtual ~RegisterBus() = default;
	virtual bool writeRegister(std::uint8_t reg, std::uint8_t value) = 0;
	virtual bool readRegister(std::uint8_t reg, std::uint8_t &value) = 0;
	virtual bool readRegisters(std::uint8_t reg, std::uint8_t *data, std::size_t length) = 0;
};

class Delay {
public:
	virtual ~Delay() = default;
	virtual void milliseconds(int ms) = 0;
};

struct Sample {
	double ax = 0;
	double ay = 0;
	double az = 0;
	double gx = 0;
	double gy = 0;
	double gz = 0;
};

class Driver {
public:
	Driver(RegisterBus &bus, Delay &delay);

	bool begin();
	void close();
	void calibrateBias();
	bool readSample(Sample &sample);

	int tiltX();
	int tiltY();
	double accelerationX();
	double accelerationY();
	double accelerationZ();
	double gyroscopeX();
	double gyroscopeY();
	double gyroscopeZ();
	std::uint8_t lastWhoAmI() const;

private:
	bool readOpenSample(Sample &sample);
	bool resetAndConfigure();
	static int tiltFromAxis(double axis);

	RegisterBus &bus_;
	Delay &delay_;
	bool open_ = false;
	double biasX_ = 0;
	double biasY_ = 0;
	double biasZ_ = 0;
	std::uint8_t lastWhoAmI_ = 0;
};

}  // namespace gea::chips::qmi8658

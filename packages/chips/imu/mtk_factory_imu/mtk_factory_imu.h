#pragma once

// Generic, target-independent driver for the MediaTek "factory" IMU char-node
// interface (the GSENSOR / GYROHUB ioctl ABI that returns an ASCII hex triplet
// per read). Shared across MTK SoCs (MT6765 cactus, MT6761 lokmat, …). All
// device transport (open/ioctl/threads) is supplied by the platform binding
// through the SensorChar interface; all board facts (axis permutation, signs,
// divisors) are supplied as Config. This file knows neither a /dev path nor an
// ioctl number — exactly like chips/imu/qmi8658 knows no I2C bus.

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>

namespace gea::chips::mtk_factory_imu {

inline constexpr double kStandardGravity = 9.80665;
inline constexpr double kDefaultVendorDivisor = 1000.0;  // accel raw -> m/s^2
inline constexpr double kDefaultGyroDivisor = 333.0;     // gyro raw -> deg/s

// Continuous still-detection bias tracking knobs (deg/s). When every gyro axis
// is within kStillDps of the running bias, ease the bias toward the reading so a
// slow offset can't integrate into cursor drift, without absorbing real motion.
inline constexpr double kStillDps = 2.5;
inline constexpr double kBiasTrack = 0.02;

struct Acceleration {
	double x = 0.0;
	double y = 0.0;
	double z = kStandardGravity;
};

struct GyroSample {
	double x = 0.0;
	double y = 0.0;
	double z = 0.0;
};

// Per-board axis composition: out[k] = sign[k] * raw[src[k]] / divisor.
// src indexes the raw factory triplet (0=rawX, 1=rawY, 2=rawZ).
struct AxisMap {
	int accelSrc[3] = {0, 1, 2};
	int accelSign[3] = {1, 1, 1};
	int gyroSrc[3] = {0, 1, 2};
	int gyroSign[3] = {1, 1, 1};
};

struct Config {
	double accelDivisor = kDefaultVendorDivisor;
	double gyroDivisor = kDefaultGyroDivisor;
	AxisMap axes{};
};

// Transport the platform binding implements (the RegisterBus analog for a Linux
// ioctl char node). The driver never touches an fd, a /dev path, or an ioctl.
class SensorChar {
public:
	virtual ~SensorChar() = default;
	virtual bool init() = 0;                              // open + enable (INIT ioctl)
	virtual bool readTriplet(char *buffer, std::size_t length) = 0;  // blocking READ ioctl
	virtual void close() = 0;
};

inline double sanitizeDivisor(double divisor, double fallback)
{
	if (!std::isfinite(divisor) || divisor <= 0.0) return fallback;
	return divisor;
}

inline bool parseSensorAxis(const char *&cursor, std::int32_t &value)
{
	while (*cursor == ' ' || *cursor == '\t' || *cursor == '\n' || *cursor == '\r') ++cursor;
	if (*cursor == '\0') return false;

	errno = 0;
	char *end = nullptr;
	const unsigned long raw = std::strtoul(cursor, &end, 16);
	if (end == cursor || errno != 0) return false;

	value = static_cast<std::int32_t>(static_cast<std::uint32_t>(raw));
	cursor = end;
	return true;
}

inline bool parseSensorTriplet(const char *text, std::int32_t &x, std::int32_t &y, std::int32_t &z)
{
	if (!text) return false;
	const char *cursor = text;
	return parseSensorAxis(cursor, x) && parseSensorAxis(cursor, y) && parseSensorAxis(cursor, z);
}

class Driver {
public:
	Driver(SensorChar &accel, SensorChar &gyro, const Config &config)
	    : accel_(accel), gyro_(gyro), config_(config)
	{
	}

	bool beginAccel() { return accel_.init(); }
	bool beginGyro() { return gyro_.init(); }

	// Read one accel sample. On any failure fills `out` with gravity-on-Z so a
	// wedged read never injects a bogus tilt (matches the legacy behavior), and
	// returns false so the caller can log once.
	bool readAccel(Acceleration &out)
	{
		char buffer[64] = {};
		if (!accel_.readTriplet(buffer, sizeof(buffer))) {
			out = {0.0, 0.0, kStandardGravity};
			return false;
		}
		std::int32_t raw[3] = {0, 0, 0};
		if (!parseSensorTriplet(buffer, raw[0], raw[1], raw[2])) {
			out = {0.0, 0.0, kStandardGravity};
			return false;
		}
		const double d = sanitizeDivisor(config_.accelDivisor, kDefaultVendorDivisor);
		const double mapped[3] = {
		    config_.axes.accelSign[0] * static_cast<double>(raw[config_.axes.accelSrc[0]]) / d,
		    config_.axes.accelSign[1] * static_cast<double>(raw[config_.axes.accelSrc[1]]) / d,
		    config_.axes.accelSign[2] * static_cast<double>(raw[config_.axes.accelSrc[2]]) / d,
		};
		out = {mapped[0], mapped[1], mapped[2]};
		return true;
	}

	// Read one gyro sample (deg/s, gea axes; bias NOT applied here).
	bool readGyro(GyroSample &out)
	{
		char buffer[64] = {};
		if (!gyro_.readTriplet(buffer, sizeof(buffer))) {
			out = {};
			return false;
		}
		std::int32_t raw[3] = {0, 0, 0};
		if (!parseSensorTriplet(buffer, raw[0], raw[1], raw[2])) {
			out = {};
			return false;
		}
		const double d = sanitizeDivisor(config_.gyroDivisor, kDefaultGyroDivisor);
		out.x = config_.axes.gyroSign[0] * static_cast<double>(raw[config_.axes.gyroSrc[0]]) / d;
		out.y = config_.axes.gyroSign[1] * static_cast<double>(raw[config_.axes.gyroSrc[1]]) / d;
		out.z = config_.axes.gyroSign[2] * static_cast<double>(raw[config_.axes.gyroSrc[2]]) / d;
		return true;
	}

	// Still-detection bias tracking: ease bias toward the reading only when all
	// axes are within kStillDps of the running bias.
	void trackStillBias(const GyroSample &sample)
	{
		const double dx = sample.x - bias_.x;
		const double dy = sample.y - bias_.y;
		const double dz = sample.z - bias_.z;
		if (std::fabs(dx) < kStillDps && std::fabs(dy) < kStillDps && std::fabs(dz) < kStillDps) {
			bias_.x += kBiasTrack * dx;
			bias_.y += kBiasTrack * dy;
			bias_.z += kBiasTrack * dz;
		}
	}

	void setBias(const GyroSample &bias) { bias_ = bias; }
	GyroSample bias() const { return bias_; }

	// Divisors may be refined at/after channel init (board default -> /sys probe ->
	// optional env override), matching the legacy geaos behavior.
	void setAccelDivisor(double divisor) { config_.accelDivisor = divisor; }
	void setGyroDivisor(double divisor) { config_.gyroDivisor = divisor; }
	double accelDivisor() const { return config_.accelDivisor; }
	double gyroDivisor() const { return config_.gyroDivisor; }

	int tiltX(const Acceleration &a) const { return tiltFromAxis(a.y); }
	int tiltY(const Acceleration &a) const { return tiltFromAxis(-a.x); }

private:
	static int tiltFromAxis(double axisMetersPerSecondSquared)
	{
		const double axisG = axisMetersPerSecondSquared / kStandardGravity;
		const int value = static_cast<int>(axisG * 70.0);
		return std::clamp(value, -100, 100);
	}

	SensorChar &accel_;
	SensorChar &gyro_;
	Config config_{};
	GyroSample bias_{};
};

}  // namespace gea::chips::mtk_factory_imu

#pragma once

namespace gea::framework::geolocation {

namespace detail {

// Read a numeric/bool field off a boxed record, keeping the fallback when the
// field is absent. The value-bridges below run only at dynamic boundaries
// (module-boundary records, boxed callbacks) — typed lowering never calls
// them; they exist so the generated record converters compile (same pattern
// as host/media.h).
template <typename T, typename Value>
T fieldOr(const Value &value, const char *name, T fallback)
{
	if constexpr (requires { value.record_get_literal(name); }) {
		const auto field = value.record_get_literal(name);
		if constexpr (requires { field.is_nullish(); static_cast<T>(field); }) {
			if (!field.is_nullish()) return static_cast<T>(field);
		}
	}
	return fallback;
}

}  // namespace detail

struct GeolocationCoordinates {
	double latitude = 0.0;
	double longitude = 0.0;
	double altitude = 0.0;
	double accuracy = -1.0;
	double altitudeAccuracy = -1.0;
	double heading = -1.0;
	double speed = -1.0;

	template <typename Value>
	static GeolocationCoordinates __gea_native_from_value(const Value &value)
	{
		GeolocationCoordinates out{};
		out.latitude = detail::fieldOr(value, "latitude", out.latitude);
		out.longitude = detail::fieldOr(value, "longitude", out.longitude);
		out.altitude = detail::fieldOr(value, "altitude", out.altitude);
		out.accuracy = detail::fieldOr(value, "accuracy", out.accuracy);
		out.altitudeAccuracy = detail::fieldOr(value, "altitudeAccuracy", out.altitudeAccuracy);
		out.heading = detail::fieldOr(value, "heading", out.heading);
		out.speed = detail::fieldOr(value, "speed", out.speed);
		return out;
	}
};

struct GeolocationPosition {
	GeolocationCoordinates coords;
	double timestamp = 0.0;
	bool hasFix = false;

	template <typename Value>
	static GeolocationPosition __gea_native_from_value(const Value &value)
	{
		GeolocationPosition out{};
		if constexpr (requires { value.record_get_literal("coords"); }) {
			const auto coords = value.record_get_literal("coords");
			if constexpr (requires { coords.is_nullish(); }) {
				if (!coords.is_nullish()) out.coords = GeolocationCoordinates::__gea_native_from_value(coords);
			}
		}
		out.timestamp = detail::fieldOr(value, "timestamp", out.timestamp);
		out.hasFix = detail::fieldOr(value, "hasFix", out.hasFix);
		return out;
	}
};

struct GeolocationPositionError {
	double code = 2.0;
	const char *message = "Position unavailable";

	// `message` keeps its default: a boxed record's string has no storage a
	// stable const char* could borrow from.
	template <typename Value>
	static GeolocationPositionError __gea_native_from_value(const Value &value)
	{
		GeolocationPositionError out{};
		out.code = detail::fieldOr(value, "code", out.code);
		return out;
	}
};

struct GeolocationOptions {
	bool enableHighAccuracy = false;
	double timeout = 0.0;
	double maximumAge = 0.0;

	template <typename Value>
	static GeolocationOptions __gea_native_from_value(const Value &value)
	{
		GeolocationOptions out{};
		out.enableHighAccuracy = detail::fieldOr(value, "enableHighAccuracy", out.enableHighAccuracy);
		out.timeout = detail::fieldOr(value, "timeout", out.timeout);
		out.maximumAge = detail::fieldOr(value, "maximumAge", out.maximumAge);
		return out;
	}
};

class GeolocationDriver {
public:
	virtual ~GeolocationDriver() = default;

	virtual bool init() = 0;
	virtual bool hasFix() const = 0;
	virtual GeolocationPosition currentPosition() const = 0;
};

class GeolocationAdapter {
public:
	static void setDriver(GeolocationDriver *driver);
	static GeolocationDriver *driver();

	bool init() const;
	bool hasFix() const;
	GeolocationPosition currentPosition() const;
};

inline GeolocationAdapter geolocation()
{
	return GeolocationAdapter{};
}

}  // namespace gea::framework::geolocation

// SPDX-License-Identifier: Apache-2.0
#include "geolocation.h"

namespace gea::framework::geolocation {

namespace {

class NullGeolocationDriver final : public GeolocationDriver {
public:
	bool init() override { return false; }
	bool hasFix() const override { return false; }
	GeolocationPosition currentPosition() const override { return {}; }
};

NullGeolocationDriver &nullDriver()
{
	static NullGeolocationDriver driver;
	return driver;
}

GeolocationDriver *&driverSlot()
{
	static GeolocationDriver *driver = &nullDriver();
	return driver;
}

}  // namespace

void GeolocationAdapter::setDriver(GeolocationDriver *driver)
{
	driverSlot() = driver ? driver : &nullDriver();
}

GeolocationDriver *GeolocationAdapter::driver()
{
	return driverSlot();
}

bool GeolocationAdapter::init() const { return driver()->init(); }
bool GeolocationAdapter::hasFix() const { return driver()->hasFix(); }
GeolocationPosition GeolocationAdapter::currentPosition() const { return driver()->currentPosition(); }

}  // namespace gea::framework::geolocation

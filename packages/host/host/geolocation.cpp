// SPDX-License-Identifier: Apache-2.0
#include "gea/embedded-host.h"

namespace gea::framework::geolocation {

bool GeolocationBackend::hasFix()
{
	return geolocation().hasFix();
}

GeolocationPosition GeolocationBackend::currentPosition()
{
	return geolocation().currentPosition();
}

double GeolocationBackend::latitude()
{
	return currentPosition().coords.latitude;
}

double GeolocationBackend::longitude()
{
	return currentPosition().coords.longitude;
}

double GeolocationBackend::accuracy()
{
	return currentPosition().coords.accuracy;
}

}  // namespace gea::framework::geolocation

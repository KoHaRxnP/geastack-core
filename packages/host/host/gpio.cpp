// SPDX-License-Identifier: Apache-2.0
#include "gpio.h"

#include "gea/embedded-host.h"

namespace gea::framework::gpio {

namespace {

// The TS surface hands every number over as a double. Truncating here — once,
// at the boundary — keeps the platform contract in ints and means a fractional
// pin number lands on a pin instead of on undefined behaviour.
int toPin(double value) { return static_cast<int>(value); }

}  // namespace

bool GpioBackend::configureOutput(double pin)
{
	return gea::platform::gpio::Gpio::configureOutput(toPin(pin));
}

bool GpioBackend::configureInput(double pin, bool pull_up)
{
	return gea::platform::gpio::Gpio::configureInput(toPin(pin), pull_up);
}

bool GpioBackend::write(double pin, bool level)
{
	return gea::platform::gpio::Gpio::write(toPin(pin), level);
}

bool GpioBackend::read(double pin) { return gea::platform::gpio::Gpio::read(toPin(pin)); }

bool AddressableLedBackend::set(double pin, double r, double g, double b)
{
	return gea::platform::gpio::AddressableLed::set(toPin(pin), toPin(r), toPin(g), toPin(b));
}

bool AddressableLedBackend::off(double pin) { return gea::platform::gpio::AddressableLed::off(toPin(pin)); }

bool AddressableLedBackend::attach(double pin, double count)
{
	return gea::platform::gpio::AddressableLed::attach(toPin(pin), toPin(count));
}

bool AddressableLedBackend::setPixel(double index, double r, double g, double b)
{
	return gea::platform::gpio::AddressableLed::setPixel(toPin(index), toPin(r), toPin(g), toPin(b));
}

bool AddressableLedBackend::show() { return gea::platform::gpio::AddressableLed::show(); }

void AddressableLedBackend::detach() { gea::platform::gpio::AddressableLed::detach(); }

}  // namespace gea::framework::gpio

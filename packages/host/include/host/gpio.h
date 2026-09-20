// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "backends.h"

namespace gea::host {

struct GpioFacade {
  bool configureOutput(double pin) const { return gea::framework::gpio::GpioBackend::configureOutput(pin); }
  bool configureInput(double pin, bool pull_up) const
  {
    return gea::framework::gpio::GpioBackend::configureInput(pin, pull_up);
  }
  bool write(double pin, bool level) const { return gea::framework::gpio::GpioBackend::write(pin, level); }
  bool read(double pin) const { return gea::framework::gpio::GpioBackend::read(pin); }
};

struct LedFacade {
  bool set(double pin, double r, double g, double b) const
  {
    return gea::framework::gpio::AddressableLedBackend::set(pin, r, g, b);
  }
  bool off(double pin) const { return gea::framework::gpio::AddressableLedBackend::off(pin); }
  bool attach(double pin, double count) const { return gea::framework::gpio::AddressableLedBackend::attach(pin, count); }
  bool setPixel(double index, double r, double g, double b) const
  {
    return gea::framework::gpio::AddressableLedBackend::setPixel(index, r, g, b);
  }
  bool show() const { return gea::framework::gpio::AddressableLedBackend::show(); }
  void detach() const { gea::framework::gpio::AddressableLedBackend::detach(); }
};

inline constexpr GpioFacade Gpio{};
inline constexpr LedFacade Led{};

}  // namespace gea::host

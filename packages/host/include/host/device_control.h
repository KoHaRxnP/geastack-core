// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <string>

namespace gea::host {

// Run a host shell command and return its combined stdout+stderr. This is the
// gea-app (CSS runtime) equivalent of @geajs/apple's runDeviceCommand: the gea
// Companion uses it to drive the device (GEADEV over USB) and to install and
// uninstall apps. Both are the `gea` CLI now (@geastack/cli); the scripts this
// used to name lived in the old single repo and are archived there. Unlike the
// other host facades this one is NOT header-only — exec() needs NSTask, so the
// impl is a macOS .mm in @geastack/apple. The header
// is included by gea/embedded.h on every target, but exec() is only referenced
// by the macOS companion, so non-macOS targets need no implementation.
struct DeviceControlFacade {
  std::string exec(const std::string &command) const;
};

inline constexpr DeviceControlFacade DeviceControl{};

}  // namespace gea::host

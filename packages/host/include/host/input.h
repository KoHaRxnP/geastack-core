// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "backends.h"

namespace gea::host {

struct InputFacade {
  bool consumeBackButton() const { return gea::framework::input::InputBackend::consumeBackButton(); }
};

inline constexpr InputFacade Input{};

}  // namespace gea::host

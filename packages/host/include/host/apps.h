// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <string>

namespace gea::host {

class Apps {
public:
  double launch(const std::string &app_id) const;
};

inline constexpr Apps apps{};

}  // namespace gea::host

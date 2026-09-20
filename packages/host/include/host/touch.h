// SPDX-License-Identifier: Apache-2.0
#pragma once

namespace gea::host {

struct TouchSample {
  bool touching = false;
  double x = 0;
  double y = 0;
};

class Touch {
public:
  TouchSample read() const;
  TouchSample readRaw() const { return read(); }
};

inline constexpr Touch touch{};

}  // namespace gea::host

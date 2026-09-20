// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <display.h>
#include <ui/document.h>

namespace gea::host {

struct WindowFacade {
  double innerWidth() const {
#if defined(GEA_EMBEDDED_DIRECT_CANVAS_CONTEXT) && GEA_EMBEDDED_DIRECT_CANVAS_CONTEXT
    return static_cast<double>(gea::platform::display::kWidth);
#else
    int w = gea::embedded::ui::Document::preferredMountWidth();
    return static_cast<double>(w > 0 ? w : gea::platform::display::kWidth);
#endif
  }

  double innerHeight() const {
#if defined(GEA_EMBEDDED_DIRECT_CANVAS_CONTEXT) && GEA_EMBEDDED_DIRECT_CANVAS_CONTEXT
    return static_cast<double>(gea::platform::display::kHeight);
#else
    int h = gea::embedded::ui::Document::preferredMountHeight();
    return static_cast<double>(h > 0 ? h : gea::platform::display::kHeight);
#endif
  }
};

inline constexpr WindowFacade window{};

}  // namespace gea::host

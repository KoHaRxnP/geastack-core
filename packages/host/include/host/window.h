// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <display.h>
#include <event_listener_options.h>
#include <ui/document.h>
#include <ui/tree_events.h>
#include <string>
#include <type_traits>
#include <cstdlib>

namespace gea { template <typename Signature> struct CallableObject; }

namespace gea::host {

struct WindowFacade {
  template <typename Result, typename Argument>
  void addEventListener(const std::string &type,
                        const gea::CallableObject<Result(Argument)> &handler,
                        bool capture = false, bool once = false) const {
    if (gea::embedded::ui::windowEventListenersRetiring()) return;
    static_assert(std::is_constructible_v<std::decay_t<Argument>,
                  const gea::framework::events::PointerEvent &>,
                  "native window listener needs an installed event carrier");
    const auto held = handler;
    if (!gea::embedded::ui::addWindowEventListener(type.c_str(),
          handler.functionObjectIdentity().get(),
          gea::framework::events::explicitEventCallback([](auto& held, gea::framework::events::PointerEvent &event) {
            held.call(static_cast<std::decay_t<Argument>>(event));
          }, held), capture, once)) std::abort();
  }
  template <typename Result>
  void addEventListener(const std::string &type,
                        const gea::CallableObject<Result()> &handler,
                        bool capture = false, bool once = false) const {
    if (gea::embedded::ui::windowEventListenersRetiring()) return;
    const auto held = handler;
    if (!gea::embedded::ui::addWindowEventListener(type.c_str(),
          handler.functionObjectIdentity().get(),
          gea::framework::events::explicitEventCallback([](auto& held, gea::framework::events::PointerEvent &) { held.call(); }, held),
          capture, once)) std::abort();
  }
  template <typename Handler>
  void removeEventListener(const std::string &type, const Handler &handler,
                           bool capture = false) const {
    gea::embedded::ui::removeWindowEventListener(type.c_str(),
        handler.functionObjectIdentity().get(), capture);
  }
  template <typename Handler, typename Options>
    requires (!std::is_same_v<std::decay_t<Options>,bool>)
  void addEventListener(const std::string &type, const Handler &handler, const Options &options) const {
    const auto parsed = gea::framework::events::listenerOptions(options);
    addEventListener(type,handler,parsed.capture,parsed.once);
  }
  template <typename Handler, typename Options>
    requires (!std::is_same_v<std::decay_t<Options>,bool>)
  void removeEventListener(const std::string &type, const Handler &handler, const Options &options) const {
    const auto parsed = gea::framework::events::listenerOptions(options);
    removeEventListener(type,handler,parsed.capture);
  }
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

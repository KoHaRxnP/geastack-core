// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <string>

namespace gea::host {

namespace notify_detail {
// One shared instance across all TUs (inline function-local statics are
// ODR-merged). The notification is transient (RAM, not persisted).
inline std::string &buffer() {
  static std::string s;
  return s;
}
inline std::uint32_t &counter() {
  static std::uint32_t n = 0;
  return n;
}
}  // namespace notify_detail

// Post a transient notification from platform/device code (e.g. the GEADEV
// NOTIFY command, driven by the gea Companion). Apps observe Notify.seq()
// changing and read Notify.text() to show a banner.
inline void postNotification(const std::string &text) {
  notify_detail::buffer() = text;
  notify_detail::counter()++;
}

// Transient host->app notification channel for app code. text() is the latest
// message; seq() increments on each post, so an app can detect a NEW one
// (rather than re-showing the same text). Reads are plain RAM (no flash), so
// they are safe from any task.
struct NotifyFacade {
  std::string text() const { return notify_detail::buffer(); }
  double seq() const { return static_cast<double>(notify_detail::counter()); }
};

inline constexpr NotifyFacade Notify{};

}  // namespace gea::host

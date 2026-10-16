#pragma once

#include "platform/uart.h"
#include <cmath>
#include <limits>
#include <type_traits>

namespace gea::host {
struct UartFacade {
  static int integer(double value) {
    return std::isfinite(value) && value >= 0 &&
                   value <= std::numeric_limits<int>::max() &&
                   std::trunc(value) == value
               ? static_cast<int>(value)
               : -1;
  }
  bool open(double port, double tx, double rx, double baud, double rxBytes,
            double txBytes) const {
    return gea::platform::uart::Uart::open(
        integer(port), {integer(tx), integer(rx), integer(baud),
                        integer(rxBytes), integer(txBytes)});
  }
  bool isOpen(double port) const {
    return gea::platform::uart::Uart::isOpen(integer(port));
  }
  void close(double port) const {
    gea::platform::uart::Uart::close(integer(port));
  }
  template <typename Bytes> double read(double port, Bytes &&bytes) const {
    static_assert(std::is_same_v<std::remove_cvref_t<decltype(*bytes.data())>,
                                 std::uint8_t>);
    if constexpr (requires {
                    bytes.buffer();
                    bytes.byteOffset();
                  }) {
      // The compiler passes a const TypedArray VIEW. Its ArrayBuffer remains
      // mutable: receive into that shared backing store, respecting subarrays.
      // data() preserves the runtime's detached-buffer check before accessing
      // it.
      (void)bytes.data();
      const auto count = bytes.size();
      auto *destination = count
                              ? bytes.buffer()->data() +
                                    static_cast<std::size_t>(bytes.byteOffset())
                              : nullptr;
      return gea::platform::uart::Uart::read(integer(port), destination, count);
    } else {
      static_assert(
          !std::is_const_v<std::remove_reference_t<decltype(*bytes.data())>>);
      return gea::platform::uart::Uart::read(integer(port), bytes.data(),
                                             bytes.size());
    }
  }
  template <typename Bytes>
  double write(double port, const Bytes &bytes) const {
    static_assert(std::is_same_v<std::remove_cvref_t<decltype(*bytes.data())>,
                                 std::uint8_t>);
    return gea::platform::uart::Uart::write(integer(port), bytes.data(),
                                            bytes.size());
  }
  double available(double port) const {
    return gea::platform::uart::Uart::available(integer(port));
  }
  double writable(double port) const {
    return gea::platform::uart::Uart::writable(integer(port));
  }
};
inline constexpr UartFacade Uart{};
} // namespace gea::host

#pragma once
#include "host/uart.h"
#include "platform/esp_now.h"
#include <cstring>
namespace gea::host {
struct EspNowFacade {
  bool open(double channel, double rate) const {
    return gea::platform::espnow::EspNow::open(UartFacade::integer(channel),
                                               UartFacade::integer(rate));
  }
  bool isOpen() const { return gea::platform::espnow::EspNow::isOpen(); }
  bool canSend() const { return gea::platform::espnow::EspNow::canSend(); }
  bool setMaxTxPower(double quarterDbm) const {
    return gea::platform::espnow::EspNow::setMaxTxPower(
        UartFacade::integer(quarterDbm));
  }
  double getMaxTxPower() const {
    return gea::platform::espnow::EspNow::getMaxTxPower();
  }
  void close() const { gea::platform::espnow::EspNow::close(); }
  template <class Address, class Key>
  bool addPeer(const Address &mac, const Key &key) const {
    return mac.size() == 6 && (key.size() == 0 || key.size() == 16) &&
           gea::platform::espnow::EspNow::addPeer(
               mac.data(), key.size() ? key.data() : nullptr);
  }
  template <class Address, class Bytes>
  double send(const Address &mac, const Bytes &bytes) const {
    return mac.size() == 6 ? gea::platform::espnow::EspNow::send(
                                 mac.data(), bytes.data(), bytes.size())
                           : -1;
  }
  template <class Bytes> static auto *writable(Bytes &&bytes) {
    (void)bytes.data();
    if constexpr (requires {
                    bytes.buffer();
                    bytes.byteOffset();
                  })
      return bytes.buffer()->data() +
             static_cast<std::size_t>(bytes.byteOffset());
    else
      return bytes.data();
  }
  template <class Address, class Bytes>
  double receive(Address &&source, Bytes &&bytes) const {
    if (source.size() != 6 || bytes.size() < 1470)
      return -1;
    // Check both views before removing a datagram from the queue.
    auto *dst = writable(bytes);
    auto *mac = writable(source);
    gea::platform::espnow::Datagram packet{};
    if (!gea::platform::espnow::EspNow::isOpen())
      return -1;
    if (!gea::platform::espnow::EspNow::receive(packet))
      return 0;
    std::memcpy(mac, packet.source.data(), 6);
    std::memcpy(dst, packet.bytes.data(), packet.size);
    return packet.size;
  }
};
inline constexpr EspNowFacade EspNow{};
} // namespace gea::host

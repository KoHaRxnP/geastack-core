#pragma once
#include <array>
#include <cstddef>
#include <cstdint>

namespace gea::platform::espnow {
struct Datagram {
  std::array<std::uint8_t, 6> source{};
  std::array<std::uint8_t, 1470> bytes{};
  std::size_t size = 0;
  std::int64_t receivedUs = 0;
  int rssi = 0;
  // Raw 32-bit receive clock in microseconds, not the esp_timer epoch. The
  // SDK qualifies its precision only without modem/light sleep. Zero is a
  // valid timestamp; absence of RX metadata is represented separately.
  std::uint32_t radioTimestampUs = 0;
  bool radioTimestampValid = false;
};
struct Statistics {
  std::uint32_t sent = 0, failed = 0, received = 0, dropped = 0;
  std::uint32_t maximumSendUs = 0;
  // Last completed datagram, published before canSend() becomes true. The
  // single sending task may read these after canSend() for its prior send.
  std::uint64_t lastSendStartedUs = 0, lastSendCompletedUs = 0;
  std::uint32_t lastSendRate = 0, lastSendStatus = 0;
  // Optional diagnostic copy of the SDK callback's 24-byte Action MAC header.
  // Validity is separate: zero fields are valid values, and Retry=0 does not
  // prove that the hardware made only one transmission attempt.
  std::uint16_t lastSendFrameControl = 0, lastSendDuration = 0,
                lastSendSequenceControl = 0;
  bool lastSendMacValid = false;
};
} // namespace gea::platform::espnow
extern "C" {
bool gea_espnow_open(int channel, int rate) __attribute__((weak));
void gea_espnow_close() __attribute__((weak));
bool gea_espnow_is_open() __attribute__((weak));
bool gea_espnow_can_send() __attribute__((weak));
bool gea_espnow_capture_tx_header(bool enabled) __attribute__((weak));
bool gea_espnow_set_max_tx_power(int quarterDbm) __attribute__((weak));
int gea_espnow_get_max_tx_power() __attribute__((weak));
bool gea_espnow_peer(const std::uint8_t *mac, const std::uint8_t *key)
    __attribute__((weak));
int gea_espnow_send(const std::uint8_t *mac, const void *data, std::size_t size)
    __attribute__((weak));
bool gea_espnow_receive(gea::platform::espnow::Datagram *)
    __attribute__((weak));
void gea_espnow_wake() __attribute__((weak));
void gea_espnow_wait(unsigned ms) __attribute__((weak));
void gea_espnow_statistics(gea::platform::espnow::Statistics *)
    __attribute__((weak));
}
namespace gea::platform::espnow {
class EspNow {
public:
  // Channel zero borrows the running Wi-Fi radio without changing its channel.
  // Otherwise claims an idle radio. Rate is 24/36/54 Mbps (OFDM), 65 (HT20
  // MCS7), or 115 on ESP32-S31 (HE20 MCS9 short GI, nominal 114.7 Mbps).
  // Unsupported rates/protocols fail; a borrowed radio is never reconfigured.
  static bool open(int channel, int rate = 54) {
    return gea_espnow_open && gea_espnow_open(channel, rate);
  }
  static bool isOpen() { return gea_espnow_is_open && gea_espnow_is_open(); }
  static bool canSend() { return gea_espnow_can_send && gea_espnow_can_send(); }
  // Native diagnostics only; default off and reset on radio close/open.
  // Enable only on an owned radio. Copies metadata, never changes the frame.
  static bool captureTxHeader(bool enabled) {
    return gea_espnow_capture_tx_header &&
           gea_espnow_capture_tx_header(enabled);
  }
  // Quarter-dBm cap, 8..84. Only an owned radio may be changed; the SDK
  // quantizes the request. Read back the actual cap, or -1 if unavailable.
  static bool setMaxTxPower(int quarterDbm) {
    return gea_espnow_set_max_tx_power &&
           gea_espnow_set_max_tx_power(quarterDbm);
  }
  static int getMaxTxPower() {
    return gea_espnow_get_max_tx_power ? gea_espnow_get_max_tx_power() : -1;
  }
  static void close() {
    if (gea_espnow_close)
      gea_espnow_close();
  }
  static bool addPeer(const std::uint8_t *mac,
                      const std::uint8_t *key = nullptr) {
    return gea_espnow_peer && gea_espnow_peer(mac, key);
  }
  // Copies one datagram; length on admission, zero when busy, -1 on error.
  // MAC completion is NOT an application acknowledgement.
  static int send(const std::uint8_t *mac, const void *bytes,
                  std::size_t size) {
    return gea_espnow_send ? gea_espnow_send(mac, bytes, size) : -1;
  }
  static bool receive(Datagram &packet) {
    return gea_espnow_receive && gea_espnow_receive(&packet);
  }
  // One native consumer. Wakes for RX or TX completion; never call from the UI.
  static void wake() {
    if (gea_espnow_wake)
      gea_espnow_wake();
  }
  static void wait(unsigned ms) {
    if (gea_espnow_wait)
      gea_espnow_wait(ms);
  }
  static Statistics statistics() {
    Statistics s{};
    if (gea_espnow_statistics)
      gea_espnow_statistics(&s);
    return s;
  }
};
} // namespace gea::platform::espnow

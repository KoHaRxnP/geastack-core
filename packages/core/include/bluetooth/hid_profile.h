#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace gea::framework::bluetooth::hid {

struct ByteView {
  const std::uint8_t *data = nullptr;
  std::size_t size = 0;
};

inline constexpr std::uint16_t kServiceUuid = 0x1812;
inline constexpr std::uint16_t kBatteryServiceUuid = 0x180F;
inline constexpr std::uint16_t kInformationUuid = 0x2A4A;
inline constexpr std::uint16_t kReportMapUuid = 0x2A4B;
inline constexpr std::uint16_t kControlPointUuid = 0x2A4C;
inline constexpr std::uint16_t kProtocolModeUuid = 0x2A4E;
inline constexpr std::uint16_t kReportUuid = 0x2A4D;
inline constexpr std::uint16_t kReportReferenceUuid = 0x2908;
inline constexpr std::uint16_t kBatteryLevelUuid = 0x2A19;

inline constexpr std::uint16_t kDefaultAppearance = 0x03C2;  // HID Mouse (hid-clicker is a pointer)
inline constexpr std::uint8_t kReportProtocol = 1;
inline constexpr const char *kDefaultDeviceName = "Gea Embedded BLE";

inline constexpr std::uint8_t kInformation[] = {0x0B, 0x01, 0x00, 0x15};
inline constexpr std::uint8_t kKeyboardReportReference[] = {0x01, 0x01};
inline constexpr std::uint8_t kMouseReportReference[] = {0x03, 0x01};

inline constexpr std::uint8_t kReportMap[] = {
    0x05, 0x01, 0x09, 0x06, 0xA1, 0x01, 0x85, 0x01,
    0x05, 0x07, 0x19, 0xE0, 0x29, 0xE7, 0x15, 0x00,
    0x25, 0x01, 0x75, 0x01, 0x95, 0x08, 0x81, 0x02,
    0x95, 0x01, 0x75, 0x08, 0x81, 0x01, 0x95, 0x05,
    0x75, 0x01, 0x05, 0x08, 0x19, 0x01, 0x29, 0x05,
    0x91, 0x02, 0x95, 0x01, 0x75, 0x03, 0x91, 0x01,
    0x95, 0x06, 0x75, 0x08, 0x15, 0x00, 0x25, 0x65,
    0x05, 0x07, 0x19, 0x00, 0x29, 0x65, 0x81, 0x00,
    0xC0,
    0x05, 0x01, 0x09, 0x02, 0xA1, 0x01, 0x85, 0x03,
    0x09, 0x01, 0xA1, 0x00, 0x05, 0x09, 0x19, 0x01,
    0x29, 0x03, 0x15, 0x00, 0x25, 0x01, 0x95, 0x03,
    0x75, 0x01, 0x81, 0x02, 0x95, 0x01, 0x75, 0x05,
    0x81, 0x01, 0x05, 0x01, 0x09, 0x30, 0x09, 0x31,
    0x09, 0x38, 0x15, 0x81, 0x25, 0x7F, 0x75, 0x08,
    0x95, 0x03, 0x81, 0x06, 0xC0, 0xC0,
};

class KeyboardMouseReports {
public:
  ByteView keyboard() const { return {keyboard_, sizeof(keyboard_)}; }
  ByteView mouse() const { return {mouse_, sizeof(mouse_)}; }

  void clearKeyboard() { std::memset(keyboard_, 0, sizeof(keyboard_)); }
  void clearMouse() { std::memset(mouse_, 0, sizeof(mouse_)); }
  void clear()
  {
    clearKeyboard();
    clearMouse();
  }

  void keyDown(int modifier, int hidCode)
  {
    keyboard_[0] = static_cast<std::uint8_t>(modifier);
    keyboard_[2] = static_cast<std::uint8_t>(hidCode);
  }

  void keyUp() { clearKeyboard(); }

  void mouseMove(int dx, int dy, int buttons, int wheel)
  {
    mouse_[0] = static_cast<std::uint8_t>(buttons & 0x07);
    mouse_[1] = static_cast<std::uint8_t>(clampSignedByte(dx));
    mouse_[2] = static_cast<std::uint8_t>(clampSignedByte(dy));
    mouse_[3] = static_cast<std::uint8_t>(clampSignedByte(wheel));
  }

private:
  static std::int8_t clampSignedByte(int value)
  {
    if (value < -127) return -127;
    if (value > 127) return 127;
    return static_cast<std::int8_t>(value);
  }

  std::uint8_t keyboard_[8] = {};
  std::uint8_t mouse_[4] = {};
};

}  // namespace gea::framework::bluetooth::hid

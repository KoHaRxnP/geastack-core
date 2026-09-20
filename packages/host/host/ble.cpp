// SPDX-License-Identifier: Apache-2.0
#include <cstdint>
#include <string>
#include <vector>

#include "ble.h"
#include "gea/embedded-host.h"

namespace gea::framework::bluetooth {

class HidValue {
 public:
  static int from(double value) {
    return static_cast<int>(value);
  }
};

void HidBackend::init(const std::string &device_name, double appearance, const std::string &mac_address) {
  const char *mac_address_ptr = mac_address.empty() ? nullptr : mac_address.c_str();
  bluetooth().hid().init(device_name.c_str(), static_cast<std::uint16_t>(appearance), mac_address_ptr);
}

bool HidBackend::enabled() {
  return bluetooth().hid().enabled();
}

void HidBackend::setEnabled(bool enabled) {
  bluetooth().hid().setEnabled(enabled);
}

void HidBackend::startAdvertising() {
  bluetooth().hid().startAdvertising();
}

void HidBackend::stopAdvertising() {
  bluetooth().hid().stopAdvertising();
}

bool HidBackend::connected() {
  return bluetooth().hid().connected();
}

bool HidBackend::bound() {
  return bluetooth().hid().bound();
}

double HidBackend::batteryLevel() {
  return static_cast<double>(bluetooth().hid().batteryLevel());
}

std::string HidBackend::mac() {
  const char *value = bluetooth().hid().mac();
  return value ? value : "";
}

std::string HidBackend::deviceName() {
  const char *value = bluetooth().hid().deviceName();
  return value ? value : "";
}

void HidBackend::keyTap(double hid_code) {
  bluetooth().hid().keyTap(HidValue::from(hid_code));
}

void HidBackend::keyDown(double modifier, double hid_code) {
  bluetooth().hid().keyDown(HidValue::from(modifier), HidValue::from(hid_code));
}

void HidBackend::keyUp() {
  bluetooth().hid().keyUp();
}

void HidBackend::mouseMove(double dx, double dy, double buttons, double wheel) {
  bluetooth().hid().mouseMove(
      HidValue::from(dx), HidValue::from(dy), HidValue::from(buttons), HidValue::from(wheel));
}

void HidBackend::mouseClick(double button) {
  bluetooth().hid().mouseClick(HidValue::from(button));
}

void HidBackend::midiEnable() {
  bluetooth().hid().midiEnable();
}

bool HidBackend::midiBound() {
  return bluetooth().hid().midiBound();
}

void HidBackend::midiSend(const std::vector<double> &bytes) {
  std::uint8_t packet[20];  // one BLE-MIDI packet, ATT_MTU(23)-3 notification payload
  int n = 0;
  for (double b : bytes) {
    if (n >= static_cast<int>(sizeof packet)) break;
    int v = static_cast<int>(b);
    if (v < 0) v = 0;
    if (v > 255) v = 255;
    packet[n++] = static_cast<std::uint8_t>(v);
  }
  if (n > 0) bluetooth().hid().midiSend(packet, n);
}

void HidBackend::midiStartScan() {
  bluetooth().hid().midiStartScan();
}

void HidBackend::midiStopScan() {
  bluetooth().hid().midiStopScan();
}

bool HidBackend::midiScanning() {
  return bluetooth().hid().midiScanning();
}

double HidBackend::midiScanCount() {
  return static_cast<double>(bluetooth().hid().midiScanCount());
}

std::string HidBackend::midiScanNameAt(double index) {
  const char *value = bluetooth().hid().midiScanNameAt(HidValue::from(index));
  return value ? value : "";
}

void HidBackend::midiConnect(double index) {
  bluetooth().hid().midiConnect(HidValue::from(index));
}

void HidBackend::midiDisconnect() {
  bluetooth().hid().midiDisconnect();
}

void HidBackend::hidHostStartScan() {
  bluetooth().hid().hidHostStartScan();
}

void HidBackend::hidHostStopScan() {
  bluetooth().hid().hidHostStopScan();
}

bool HidBackend::hidHostScanning() {
  return bluetooth().hid().hidHostScanning();
}

double HidBackend::hidHostScanCount() {
  return static_cast<double>(bluetooth().hid().hidHostScanCount());
}

std::string HidBackend::hidHostScanNameAt(double index) {
  const char *value = bluetooth().hid().hidHostScanNameAt(HidValue::from(index));
  return value ? value : "";
}

void HidBackend::hidHostConnect(double index) {
  bluetooth().hid().hidHostConnect(HidValue::from(index));
}

void HidBackend::hidHostDisconnect() {
  bluetooth().hid().hidHostDisconnect();
}

bool HidBackend::hidHostBound() {
  return bluetooth().hid().hidHostBound();
}

double HidBackend::hidHostReportCount() {
  return static_cast<double>(bluetooth().hid().hidHostReportCount());
}

double HidBackend::hidHostReportIdAt(double index) {
  return static_cast<double>(bluetooth().hid().hidHostReportIdAt(HidValue::from(index)));
}

double HidBackend::hidHostReportLenAt(double index) {
  return static_cast<double>(bluetooth().hid().hidHostReportLenAt(HidValue::from(index)));
}

double HidBackend::hidHostReportByteAt(double index, double byteIndex) {
  return static_cast<double>(bluetooth().hid().hidHostReportByteAt(HidValue::from(index), HidValue::from(byteIndex)));
}

void HidBackend::hidHostClearReports() {
  bluetooth().hid().hidHostClearReports();
}

double HidBackend::connectionCount() {
  return static_cast<double>(bluetooth().hid().connectionCount());
}

double HidBackend::connectionKindAt(double index) {
  return static_cast<double>(bluetooth().hid().connectionKindAt(HidValue::from(index)));
}

std::string HidBackend::connectionNameAt(double index) {
  const char *value = bluetooth().hid().connectionNameAt(HidValue::from(index));
  return value ? value : "";
}

void HidBackend::configSetDocument(const std::vector<double> &bytes) {
  std::vector<std::uint8_t> buffer;
  buffer.reserve(bytes.size());
  for (double b : bytes) {
    int v = static_cast<int>(b);
    if (v < 0) v = 0;
    if (v > 255) v = 255;
    buffer.push_back(static_cast<std::uint8_t>(v));
  }
  bluetooth().hid().configSetDocument(buffer.data(), static_cast<int>(buffer.size()));
}

double HidBackend::configPendingLength() {
  return static_cast<double>(bluetooth().hid().configPendingLength());
}

double HidBackend::configPendingByteAt(double index) {
  return static_cast<double>(bluetooth().hid().configPendingByteAt(HidValue::from(index)));
}

void HidBackend::configConsumePending() {
  bluetooth().hid().configConsumePending();
}

bool HidBackend::configPairing() {
  return bluetooth().hid().configPairing();
}

std::string HidBackend::configPairCode() {
  const char *value = bluetooth().hid().configPairCode();
  return value ? value : "";
}

void HidBackend::configDismissPairing() {
  bluetooth().hid().configDismissPairing();
}

void HidBackend::configPushActivity(double index) {
  bluetooth().hid().configPushActivity(HidValue::from(index));
}

}  // namespace gea::framework::bluetooth

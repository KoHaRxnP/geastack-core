// SPDX-License-Identifier: Apache-2.0
#include "ble.h"

namespace gea::framework::bluetooth {

namespace {

class NullBluetoothHidDriver final : public BluetoothHidDriver {
public:
	void preinit() override {}
	void init(const char *deviceName, std::uint16_t appearance, const char *macAddress) override
	{
		(void)deviceName;
		(void)appearance;
		(void)macAddress;
	}
	bool enabled() const override { return false; }
	void setEnabled(bool enabled) override { (void)enabled; }
	void startAdvertising() override {}
	void stopAdvertising() override {}
	bool connected() const override { return false; }
	bool bound() const override { return false; }
	std::uint8_t batteryLevel() const override { return 0; }
	const char *mac() override { return ""; }
	const char *deviceName() const override { return ""; }
	void keyTap(int hidCode) override { (void)hidCode; }
	void keyDown(int modifier, int hidCode) override
	{
		(void)modifier;
		(void)hidCode;
	}
	void keyUp() override {}
	void mouseMove(int dx, int dy, int buttons, int wheel) override
	{
		(void)dx;
		(void)dy;
		(void)buttons;
		(void)wheel;
	}
	void mouseClick(int button) override { (void)button; }
	void setBatteryLevel(std::uint8_t level) override { (void)level; }
};

NullBluetoothHidDriver &nullDriver()
{
	static NullBluetoothHidDriver driver;
	return driver;
}

BluetoothHidDriver *&driverSlot()
{
	static BluetoothHidDriver *driver = &nullDriver();
	return driver;
}

}  // namespace

void BluetoothHidDevice::setDriver(BluetoothHidDriver *driver)
{
	driverSlot() = driver ? driver : &nullDriver();
}

BluetoothHidDriver *BluetoothHidDevice::driver()
{
	return driverSlot();
}

void BluetoothHidDevice::preinit() const { driver()->preinit(); }

void BluetoothHidDevice::init(const char *deviceName, std::uint16_t appearance, const char *macAddress) const
{
	driver()->init(deviceName, appearance, macAddress);
}

bool BluetoothHidDevice::enabled() const { return driver()->enabled(); }
void BluetoothHidDevice::setEnabled(bool enabled) const { driver()->setEnabled(enabled); }
void BluetoothHidDevice::startAdvertising() const { driver()->startAdvertising(); }
void BluetoothHidDevice::stopAdvertising() const { driver()->stopAdvertising(); }
bool BluetoothHidDevice::connected() const { return driver()->connected(); }
bool BluetoothHidDevice::bound() const { return driver()->bound(); }
std::uint8_t BluetoothHidDevice::batteryLevel() const { return driver()->batteryLevel(); }
const char *BluetoothHidDevice::mac() const { return driver()->mac(); }
const char *BluetoothHidDevice::deviceName() const { return driver()->deviceName(); }
void BluetoothHidDevice::keyTap(int hidCode) const { driver()->keyTap(hidCode); }
void BluetoothHidDevice::keyDown(int modifier, int hidCode) const { driver()->keyDown(modifier, hidCode); }
void BluetoothHidDevice::keyUp() const { driver()->keyUp(); }
void BluetoothHidDevice::mouseMove(int dx, int dy, int buttons, int wheel) const { driver()->mouseMove(dx, dy, buttons, wheel); }
void BluetoothHidDevice::mouseClick(int button) const { driver()->mouseClick(button); }
void BluetoothHidDevice::setBatteryLevel(std::uint8_t level) const { driver()->setBatteryLevel(level); }
void BluetoothHidDevice::midiEnable() const { driver()->midiEnable(); }
bool BluetoothHidDevice::midiBound() const { return driver()->midiBound(); }
void BluetoothHidDevice::midiSend(const std::uint8_t *packet, int length) const { driver()->midiSend(packet, length); }
void BluetoothHidDevice::midiStartScan() const { driver()->midiStartScan(); }
void BluetoothHidDevice::midiStopScan() const { driver()->midiStopScan(); }
bool BluetoothHidDevice::midiScanning() const { return driver()->midiScanning(); }
int BluetoothHidDevice::midiScanCount() const { return driver()->midiScanCount(); }
const char *BluetoothHidDevice::midiScanNameAt(int index) const { return driver()->midiScanNameAt(index); }
void BluetoothHidDevice::midiConnect(int index) const { driver()->midiConnect(index); }
void BluetoothHidDevice::midiDisconnect() const { driver()->midiDisconnect(); }
void BluetoothHidDevice::hidHostStartScan() const { driver()->hidHostStartScan(); }
void BluetoothHidDevice::hidHostStopScan() const { driver()->hidHostStopScan(); }
bool BluetoothHidDevice::hidHostScanning() const { return driver()->hidHostScanning(); }
int BluetoothHidDevice::hidHostScanCount() const { return driver()->hidHostScanCount(); }
const char *BluetoothHidDevice::hidHostScanNameAt(int index) const { return driver()->hidHostScanNameAt(index); }
void BluetoothHidDevice::hidHostConnect(int index) const { driver()->hidHostConnect(index); }
void BluetoothHidDevice::hidHostDisconnect() const { driver()->hidHostDisconnect(); }
bool BluetoothHidDevice::hidHostBound() const { return driver()->hidHostBound(); }
int BluetoothHidDevice::hidHostReportCount() const { return driver()->hidHostReportCount(); }
int BluetoothHidDevice::hidHostReportIdAt(int index) const { return driver()->hidHostReportIdAt(index); }
int BluetoothHidDevice::hidHostReportLenAt(int index) const { return driver()->hidHostReportLenAt(index); }
int BluetoothHidDevice::hidHostReportByteAt(int index, int byteIndex) const { return driver()->hidHostReportByteAt(index, byteIndex); }
void BluetoothHidDevice::hidHostClearReports() const { driver()->hidHostClearReports(); }
int BluetoothHidDevice::connectionCount() const { return driver()->connectionCount(); }
int BluetoothHidDevice::connectionKindAt(int index) const { return driver()->connectionKindAt(index); }
const char *BluetoothHidDevice::connectionNameAt(int index) const { return driver()->connectionNameAt(index); }
void BluetoothHidDevice::configSetDocument(const std::uint8_t *bytes, int length) const { driver()->configSetDocument(bytes, length); }
int BluetoothHidDevice::configPendingLength() const { return driver()->configPendingLength(); }
int BluetoothHidDevice::configPendingByteAt(int index) const { return driver()->configPendingByteAt(index); }
void BluetoothHidDevice::configConsumePending() const { driver()->configConsumePending(); }
bool BluetoothHidDevice::configPairing() const { return driver()->configPairing(); }
const char *BluetoothHidDevice::configPairCode() const { return driver()->configPairCode(); }
void BluetoothHidDevice::configDismissPairing() const { driver()->configDismissPairing(); }
void BluetoothHidDevice::configPushActivity(int index) const { driver()->configPushActivity(index); }

}  // namespace gea::framework::bluetooth

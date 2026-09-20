#pragma once

#include <cstdint>

namespace gea::framework::bluetooth {

class BluetoothHidDriver {
public:
	virtual ~BluetoothHidDriver() = default;

	virtual void preinit() = 0;
	virtual void init(const char *deviceName, std::uint16_t appearance, const char *macAddress = nullptr) = 0;
	virtual bool enabled() const = 0;
	virtual void setEnabled(bool enabled) = 0;
	virtual void startAdvertising() = 0;
	virtual void stopAdvertising() = 0;
	virtual bool connected() const = 0;
	virtual bool bound() const = 0;
	virtual std::uint8_t batteryLevel() const = 0;
	virtual const char *mac() = 0;
	virtual const char *deviceName() const = 0;
	virtual void keyTap(int hidCode) = 0;
	virtual void keyDown(int modifier, int hidCode) = 0;
	virtual void keyUp() = 0;
	virtual void mouseMove(int dx, int dy, int buttons, int wheel) = 0;
	virtual void mouseClick(int button) = 0;
	virtual void setBatteryLevel(std::uint8_t level) = 0;

	// BLE-MIDI (MIDI over GATT, service 03B80E5A-EDE8-4B33-A751-6CE34EC4C700).
	// Optional capability: drivers that do not implement it inherit these
	// no-ops, so the HID-only targets are unaffected. midiEnable() registers
	// the MIDI service in the GATT database (call before/independent of
	// init(); the driver folds it into its advertising data). midiSend()
	// transmits one already-framed BLE-MIDI packet (header + timestamp +
	// MIDI bytes) as a notification on the MIDI I/O characteristic.
	virtual void midiEnable() {}
	virtual bool midiBound() const { return false; }
	virtual void midiSend(const std::uint8_t *packet, int length)
	{
		(void)packet;
		(void)length;
	}

	// BLE-MIDI central role: scan for BLE-MIDI peripherals (WIDI adapters,
	// pedals), connect as a GATT client, and write framed packets to the
	// peer's MIDI I/O characteristic. One role is active at a time: starting
	// a scan stops advertising; a central link suppresses re-advertising.
	virtual void midiStartScan() {}
	virtual void midiStopScan() {}
	virtual bool midiScanning() const { return false; }
	virtual int midiScanCount() const { return 0; }
	virtual const char *midiScanNameAt(int index)
	{
		(void)index;
		return "";
	}
	virtual void midiConnect(int index) { (void)index; }
	virtual void midiDisconnect() {}

	// HID host role: the device acts as a BLE central hosting a remote HID
	// peripheral (a keyboard/macro pad such as the XPPen ACK05). Scan,
	// connect, subscribe to every input-report characteristic in the peer's
	// HID service, and queue the raw notification payloads for the app to
	// poll and decode. Optional capability with no-op defaults like MIDI.
	virtual void hidHostStartScan() {}
	virtual void hidHostStopScan() {}
	virtual bool hidHostScanning() const { return false; }
	virtual int hidHostScanCount() const { return 0; }
	virtual const char *hidHostScanNameAt(int index)
	{
		(void)index;
		return "";
	}
	virtual void hidHostConnect(int index) { (void)index; }
	virtual void hidHostDisconnect() {}
	virtual bool hidHostBound() const { return false; }
	// Raw input-report queue (oldest first). reportIdAt() is the report ID
	// the notification came from; bytes are the payload after the ID.
	virtual int hidHostReportCount() const { return 0; }
	virtual int hidHostReportIdAt(int index) const
	{
		(void)index;
		return 0;
	}
	virtual int hidHostReportLenAt(int index) const
	{
		(void)index;
		return 0;
	}
	virtual int hidHostReportByteAt(int index, int byteIndex) const
	{
		(void)index;
		(void)byteIndex;
		return 0;
	}
	virtual void hidHostClearReports() {}

	// Connection registry across every role this device holds at once.
	// kind: 0 = central peer connected TO us while we serve HID (a desktop
	// using us as keyboard/mouse), 1 = central peer on our MIDI service (a
	// DAW), 2 = HID peripheral WE host (the XPPen), 3 = BLE-MIDI peripheral
	// WE drive (the pedal/WIDI adapter).
	virtual int connectionCount() const { return 0; }
	virtual int connectionKindAt(int index) const
	{
		(void)index;
		return 0;
	}
	virtual const char *connectionNameAt(int index)
	{
		(void)index;
		return "";
	}

	// Config service (custom GATT, service A1E0F000-...). A Web Bluetooth
	// portal reads the app's config blob and writes a new one, gated by a
	// 4-digit pairing code shown on the device. Optional capability with
	// no-op defaults like MIDI/HID-host. configSetDocument() publishes the
	// blob the portal reads back; configPending*() expose a committed inbound
	// blob for the app to drain; the pairing trio drives the on-device
	// pairing overlay.
	virtual void configSetDocument(const std::uint8_t *bytes, int length)
	{
		(void)bytes;
		(void)length;
	}
	virtual int configPendingLength() const { return 0; }
	virtual int configPendingByteAt(int index) const
	{
		(void)index;
		return 0;
	}
	virtual void configConsumePending() {}
	virtual bool configPairing() const { return false; }
	virtual const char *configPairCode() { return ""; }
	virtual void configDismissPairing() {}

	// Device -> portal activity push. When a control fires on the device, the
	// app calls this with the control's index; a driver that hosts the config
	// service sends a CTRL notification (opcode 0xA0 + index) so a subscribed
	// portal can highlight the matching control live. No-op default.
	virtual void configPushActivity(int index) { (void)index; }
};

class BluetoothHidDevice {
public:
	static void setDriver(BluetoothHidDriver *driver);
	static BluetoothHidDriver *driver();

	void preinit() const;
	void init(const char *deviceName, std::uint16_t appearance, const char *macAddress = nullptr) const;
	bool enabled() const;
	void setEnabled(bool enabled) const;
	void startAdvertising() const;
	void stopAdvertising() const;
	bool connected() const;
	bool bound() const;
	std::uint8_t batteryLevel() const;
	const char *mac() const;
	const char *deviceName() const;
	void keyTap(int hidCode) const;
	void keyDown(int modifier, int hidCode) const;
	void keyUp() const;
	void mouseMove(int dx, int dy, int buttons, int wheel) const;
	void mouseClick(int button) const;
	void setBatteryLevel(std::uint8_t level) const;
	void midiEnable() const;
	bool midiBound() const;
	void midiSend(const std::uint8_t *packet, int length) const;
	void midiStartScan() const;
	void midiStopScan() const;
	bool midiScanning() const;
	int midiScanCount() const;
	const char *midiScanNameAt(int index) const;
	void midiConnect(int index) const;
	void midiDisconnect() const;
	void hidHostStartScan() const;
	void hidHostStopScan() const;
	bool hidHostScanning() const;
	int hidHostScanCount() const;
	const char *hidHostScanNameAt(int index) const;
	void hidHostConnect(int index) const;
	void hidHostDisconnect() const;
	bool hidHostBound() const;
	int hidHostReportCount() const;
	int hidHostReportIdAt(int index) const;
	int hidHostReportLenAt(int index) const;
	int hidHostReportByteAt(int index, int byteIndex) const;
	void hidHostClearReports() const;
	int connectionCount() const;
	int connectionKindAt(int index) const;
	const char *connectionNameAt(int index) const;
	void configSetDocument(const std::uint8_t *bytes, int length) const;
	int configPendingLength() const;
	int configPendingByteAt(int index) const;
	void configConsumePending() const;
	bool configPairing() const;
	const char *configPairCode() const;
	void configDismissPairing() const;
	void configPushActivity(int index) const;
};

class Bluetooth {
public:
	BluetoothHidDevice hid() const { return BluetoothHidDevice{}; }
};

inline Bluetooth bluetooth() {
	return Bluetooth{};
}

}  // namespace gea::framework::bluetooth

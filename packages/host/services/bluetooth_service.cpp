// SPDX-License-Identifier: Apache-2.0
#include "services/bluetooth_service.h"

#include "ble.h"
#include "services/diagnostics.h"

namespace gea::framework::services {

void BluetoothService::preinitForApp()
{
#ifdef GEA_EMBEDDED_APP_USES_BLE
	DiagnosticsServer::print("Preinitializing BLE controller...\n");
	HeapProbe::log("app:before_ble_preinit");
	gea::framework::bluetooth::bluetooth().hid().preinit();
	HeapProbe::log("app:after_ble_preinit");
#endif
	// When the macro is undefined the build omits the ESP32 BLE driver and bt
	// component. Keeping this function a no-op lets the shared runtime remain
	// source-compatible without retaining the controller's ~40 KB RAM footprint.
}

void BluetoothService::notifyConnected()
{
	DiagnosticsServer::print("BLE connected\n");
}

void BluetoothService::notifyDisconnected()
{
	DiagnosticsServer::print("BLE disconnected\n");
}

void BluetoothService::notifyBound()
{
	DiagnosticsServer::print("BLE HID bound\n");
}

}  // namespace gea::framework::services

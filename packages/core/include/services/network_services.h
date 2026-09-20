#pragma once

namespace gea::framework::services {

struct NetworkServicesOptions {
	// Device control and screenshots use the USB command channel. Keep the
	// separate TCP diagnostics server opt-in so its 6 KiB task does not compete
	// with WiFi OTA on RAM-constrained boards.
	bool diagnosticsEnabled = false;
	bool otaEnabled = true;
};

class NetworkServices {
public:
	// Records the requested service options. If early boot already connected
	// WiFi, starts the servers immediately and returns true. Otherwise the
	// platform WiFi bring-up calls startDeferredServers() after lazy connection.
	static bool start(const NetworkServicesOptions &options);

	// Starts diagnostics/OTA per the options recorded by start(). Called by the
	// platform WiFi driver once the radio is up and connected.
	static void startDeferredServers();
};

}  // namespace gea::framework::services

// SPDX-License-Identifier: Apache-2.0
#include "services/network_services.h"

#include "services/diagnostics.h"
#include "services/ota.h"
#include "wifi.h"

namespace gea::framework::services {

namespace {
// Requested network-service options, stashed by start() at boot and applied by
// startDeferredServers() once WiFi is actually brought up on demand.
NetworkServicesOptions g_options{};
bool g_optionsReady = false;
}  // namespace

bool NetworkServices::start(const NetworkServicesOptions &options)
{
#ifdef GEA_EMBEDDED_WIFI_DISABLED
	(void)options;
	DiagnosticsServer::print("WiFi disabled for this app; OTA and store mirror unavailable.\n");
	return false;
#else
	// WiFi is opt-in: it is NOT brought up at boot. Stash the requested service
	// options; the WiFi driver brings the radio up lazily the first time an app
	// enables WiFi, and that bring-up worker then calls startDeferredServers().
	g_options = options;
	g_optionsReady = true;
	if (network::wifi().connected()) {
		startDeferredServers();
		return true;
	}
	DiagnosticsServer::print("WiFi opt-in: deferred until an app enables it.\n");
	return false;
#endif
}

void NetworkServices::startDeferredServers()
{
#ifndef GEA_EMBEDDED_WIFI_DISABLED
	// Early WiFi can acquire an address before Runtime has supplied its service
	// policy. The later start() call observes the connected driver and returns
	// here again after setting g_optionsReady.
	if (!g_optionsReady) return;
	HeapProbe::log("app:after_wifi_init");
	// Firmware recovery gets the first network task allocation on RAM-constrained
	// boards. Starting diagnostics first could consume the final internal stack
	// block and leave the OTA HTTP server absent even though WiFi was connected.
	if (g_options.otaEnabled) {
		OtaServer::start();
	}
	if (g_options.diagnosticsEnabled) {
		HeapProbe::log("app:before_diag_start");
		DiagnosticsServer::start();
	}
	if (g_options.diagnosticsEnabled || g_options.otaEnabled) {
		HeapProbe::log("app:after_diag_ota_start");
	}
#endif
}

}  // namespace gea::framework::services

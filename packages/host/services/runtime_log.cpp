// SPDX-License-Identifier: Apache-2.0
#include "services/runtime_log.h"

#include "services/diagnostics.h"

namespace gea::framework::services {

void RuntimeLog::printRuntimeBanner()
{
	DiagnosticsServer::print("\n--- gea_embedded: gea TSX app (vite-plugin-gea -> geatsc) ---\n\n");
}

void RuntimeLog::appStarted(bool networkReady)
{
	DiagnosticsServer::print("\n--- app started, entering event loop ---\n");
	if (networkReady) {
		DiagnosticsServer::print("Ready for OTA. Use: curl -X POST http://<ip>:8080/ota --data-binary @build/gea_embedded.bin\n");
	} else {
		DiagnosticsServer::print("OTA unavailable. USB flash still works.\n");
	}
}

}  // namespace gea::framework::services

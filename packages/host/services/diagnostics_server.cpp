// SPDX-License-Identifier: Apache-2.0
#include "services/diagnostics.h"

#include "diagnostics_internal.h"
#include "services/diagnostics_platform.h"

#include <cstdint>

namespace gea::framework::services {

namespace {

class DiagnosticsRuntime {
public:
	static DiagnosticsRuntime &instance()
	{
		static DiagnosticsRuntime runtime;
		return runtime;
	}

	void start()
	{
		if (started_) return;
		if (!DiagnosticsServer::begin()) return;
		DiagnosticsPlatform &platform = diagnosticsPlatform();
		platform.installLogSink(DiagnosticsServer::vprint);
		started_ = true;
		platform.startTask(&DiagnosticsRuntime::taskMain, this);
	}

private:
	DiagnosticsRuntime() = default;

	void run()
	{
		DiagnosticsPlatform &platform = diagnosticsPlatform();
		DiagnosticsListener &listener = platform.listener();
		if (!listener.listen(DiagnosticsServer::port())) {
			platform.logError("Diagnostics server failed on port %d", DiagnosticsServer::port());
			return;
		}

		platform.logInfo("Diagnostics transport listening on port %d", DiagnosticsServer::port());

		DiagnosticsPeer *peer = nullptr;
		DiagnosticsConnection connection;

		while (true) {
			std::int64_t now_us = platform.nowUs();
			bool immediate = peer && connection.wantsImmediatePoll(now_us);
			int timeout_ms = immediate ? 0 : connection.pollIntervalMs();
			int wait_errno = 0;
			int events = listener.wait(peer, peer && connection.hasPendingOutput(), timeout_ms, &wait_errno);
			if (events < 0) {
				platform.logWarn("Diagnostics wait failed: errno=%d", wait_errno);
				platform.sleepMs(connection.pollIntervalMs());
				continue;
			}

			if (events & kDiagnosticsWaitListenerReadable) {
				DiagnosticsPeer *next_peer = listener.accept();
				if (next_peer) {
					if (peer) {
						peer->close("replaced by a new connection", 0);
						delete peer;
					}
					peer = next_peer;
					connection.reset();
					platform.logInfo("Diagnostics client connected");
				}
			}

			if (peer && (events & kDiagnosticsWaitPeerReadable)) {
				char incoming[16];
				int read_errno = 0;
				int received = peer->read(incoming, sizeof(incoming), &read_errno);
				if (received == kDiagnosticsIoWouldBlock) {
					// No payload after select; keep the peer and continue polling.
				} else if (received == 0) {
					peer->close("disconnected", 0);
					delete peer;
					peer = nullptr;
					connection.reset();
				} else if (received < 0) {
					peer->close("closed after receive error", read_errno);
					delete peer;
					peer = nullptr;
					connection.reset();
				} else {
					connection.handleIncoming(incoming, received);
				}
			}

			if (!peer) continue;

			if (connection.hasPendingOutput()) {
				int send_errno = 0;
				DiagnosticsSendStatus send_status = connection.flush(*peer, platform.nowUs(), &send_errno);
				if (send_status == DiagnosticsSendStatus::Backpressure) {
					platform.sleepMs(connection.backpressureDelayMs());
					continue;
				}
				if (send_status == DiagnosticsSendStatus::Closed) {
					peer->close("closed after send error", send_errno);
					delete peer;
					peer = nullptr;
					connection.reset();
					continue;
				}
			}

			connection.prepareNextFrame(platform.nowUs());
		}
	}

	static void taskMain(void *context)
	{
		static_cast<DiagnosticsRuntime *>(context)->run();
	}

	bool started_ = false;
};

}  // namespace

namespace diagnostics_internal {

void startServer()
{
	DiagnosticsRuntime::instance().start();
}

}  // namespace diagnostics_internal

}  // namespace gea::framework::services

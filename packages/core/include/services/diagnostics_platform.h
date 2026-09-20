#pragma once

#include "services/diagnostics.h"

#include <cstdarg>
#include <cstdint>

namespace gea::framework::services {

enum DiagnosticsWaitEvent {
	kDiagnosticsWaitNone = 0,
	kDiagnosticsWaitListenerReadable = 1 << 0,
	kDiagnosticsWaitPeerReadable = 1 << 1,
	kDiagnosticsWaitPeerWritable = 1 << 2,
};

constexpr int kDiagnosticsIoWouldBlock = -2;

class DiagnosticsPeer : public DiagnosticsOutput {
public:
	~DiagnosticsPeer() override = default;
	virtual int read(char *data, int len, int *err_out) = 0;
	virtual void close(const char *reason, int err) = 0;
};

class DiagnosticsListener {
public:
	virtual ~DiagnosticsListener() = default;
	virtual bool listen(int port) = 0;
	virtual DiagnosticsPeer *accept() = 0;
	virtual int wait(DiagnosticsPeer *peer, bool wait_for_peer_write, int timeout_ms, int *err_out) = 0;
};

using DiagnosticsVPrintSink = int (*)(const char *, std::va_list);

class DiagnosticsPlatform {
public:
	virtual ~DiagnosticsPlatform() = default;
	virtual DiagnosticsListener &listener() = 0;
	virtual void installLogSink(DiagnosticsVPrintSink sink) = 0;
	virtual void startTask(void (*entry)(void *), void *context) = 0;
	virtual std::int64_t nowUs() = 0;
	virtual void sleepMs(int ms) = 0;
	virtual void logInfo(const char *fmt, ...) = 0;
	virtual void logWarn(const char *fmt, ...) = 0;
	virtual void logError(const char *fmt, ...) = 0;
	virtual void logHeapProbe(const char *stage) = 0;
	virtual void logCurrentTaskStackProbe(const char *stage) { (void)stage; }
	virtual void logHeapTaskSummary(const char *stage) { (void)stage; }
};

DiagnosticsPlatform &diagnosticsPlatform();

}  // namespace gea::framework::services

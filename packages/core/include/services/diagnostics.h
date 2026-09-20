#pragma once

#include <cstdarg>
#include <cstdint>
#include <cstddef>

namespace gea::framework::services {

class DiagnosticsFrame {
public:
	virtual ~DiagnosticsFrame() = default;
	virtual bool empty() const = 0;
	virtual unsigned char *payload() = 0;
	virtual int payloadCapacity() const = 0;
	virtual bool queue(int channel, int type, const unsigned char *payload, int payload_len) = 0;
};

class DiagnosticsOutput {
public:
	virtual ~DiagnosticsOutput() = default;
	virtual int write(const unsigned char *data, int len, int *err_out) = 0;
};

enum class DiagnosticsSendStatus {
	Idle,
	Sent,
	Backpressure,
	Closed,
};

class DiagnosticsConnection {
public:
	DiagnosticsConnection();
	~DiagnosticsConnection();
	DiagnosticsConnection(const DiagnosticsConnection &) = delete;
	DiagnosticsConnection &operator=(const DiagnosticsConnection &) = delete;

	void reset();
	bool hasPendingOutput() const;
	bool wantsImmediatePoll(std::int64_t now_us) const;
	int pollIntervalMs() const;
	int backpressureDelayMs() const;
	void handleIncoming(const char *data, int len);
	DiagnosticsSendStatus flush(DiagnosticsOutput &output, std::int64_t now_us, int *err_out);
	void prepareNextFrame(std::int64_t now_us);

private:
	class Impl;
	Impl *impl_;
};

class DiagnosticsServer {
public:
	static bool begin();
	static int port();
	static void start();
	static void print(const char *fmt, ...);
	static int vprint(const char *fmt, std::va_list args);
};

class HeapProbe {
public:
	static void log(const char *stage);
};

class StackProbe {
public:
	static void logCurrentTask(const char *stage);
};

class HeapTaskSummary {
public:
	static void log(const char *stage);
};

}  // namespace gea::framework::services

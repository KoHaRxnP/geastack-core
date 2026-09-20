// SPDX-License-Identifier: Apache-2.0
#include "services/diagnostics.h"

#include "diagnostics_internal.h"
#include "memory.h"
#include "services/diagnostics_platform.h"

#include <cstdarg>
#include <cstddef>
#include <cstdio>
#include <mutex>

namespace gea::framework::services {

namespace {

// SPIRAM-backed ring that buffers log bytes between the producer (any task
// calling ESP_LOG) and the diagnostics network client, which drains it in
// 96-byte chunks per poll (see DiagnosticsConnection). It must comfortably hold
// the largest single burst — the ~1.5 KB per-frame perf line, written under one
// lock — plus slack for concurrent logs, or the client sees only the line's
// tail. 8 KB holds several perf lines of backlog at negligible SPIRAM cost.
constexpr std::size_t kLogRingSize = 8192;
constexpr int kDiagnosticsPort = 8081;

class DiagnosticsMemory {
public:
	static void *allocate(std::size_t size)
	{
		return gea::framework::memory::Allocator::allocatePreferSpiram(size);
	}
};

class LogRing {
public:
	bool begin()
	{
		std::lock_guard<std::mutex> guard(mutex_);
		if (!data_) {
			data_ = static_cast<char *>(DiagnosticsMemory::allocate(kLogRingSize));
			if (!data_) {
				std::fputs("Diagnostics log ring allocation failed; continuing without ring replay\n", stdout);
				return true;
			}
		}
		head_ = 0;
		used_ = 0;
		total_ = 0;
		return true;
	}

	void write(const char *data, std::size_t len)
	{
		if (!data || len == 0) return;
		std::lock_guard<std::mutex> guard(mutex_);
		if (!data_) return;
		for (std::size_t i = 0; i < len; i++) {
			data_[head_] = data[i];
			head_ = (head_ + 1) % kLogRingSize;
			if (used_ < kLogRingSize) used_++;
			total_++;
		}
	}

	std::size_t oldestTotal()
	{
		std::lock_guard<std::mutex> guard(mutex_);
		return oldestTotalLocked();
	}

	std::size_t latestTotal()
	{
		std::lock_guard<std::mutex> guard(mutex_);
		return total_;
	}

	int copySince(std::size_t *cursor, char *dst, int cap)
	{
		if (!cursor || !dst || cap <= 0) return 0;
		std::lock_guard<std::mutex> guard(mutex_);
		if (!data_) return 0;
		std::size_t oldest = oldestTotalLocked();
		if (*cursor < oldest) *cursor = oldest;
		std::size_t available = (total_ > *cursor) ? (total_ - *cursor) : 0;
		if (available == 0) return 0;
		std::size_t count = available > static_cast<std::size_t>(cap)
			? static_cast<std::size_t>(cap)
			: available;
		for (std::size_t i = 0; i < count; i++) {
			std::size_t absolute = *cursor + i;
			dst[i] = data_[absolute % kLogRingSize];
		}
		*cursor += count;
		return static_cast<int>(count);
	}

private:
	std::size_t oldestTotalLocked() const
	{
		return (total_ > used_) ? (total_ - used_) : 0;
	}

	std::mutex mutex_;
	char *data_ = nullptr;
	std::size_t head_ = 0;
	std::size_t used_ = 0;
	std::size_t total_ = 0;
};

LogRing &logRing()
{
	static LogRing ring;
	return ring;
}

}  // namespace

namespace diagnostics_internal {

bool beginLogRing()
{
	return logRing().begin();
}

void writeLog(const char *data, std::size_t len)
{
	logRing().write(data, len);
}

std::size_t oldestLogTotal()
{
	return logRing().oldestTotal();
}

std::size_t latestLogTotal()
{
	return logRing().latestTotal();
}

int copyLogSince(std::size_t *cursor, char *dst, int cap)
{
	return logRing().copySince(cursor, dst, cap);
}

}  // namespace diagnostics_internal

bool DiagnosticsServer::begin()
{
	return diagnostics_internal::beginLogRing();
}

int DiagnosticsServer::port()
{
	return kDiagnosticsPort;
}

namespace {

// Format `fmt`/`args` and tee the result to the USB console (stdout) and the
// diagnostics log ring (streamed to a network client). The common short line
// formats into a stack buffer; anything longer — notably the ~1.5 KB per-frame
// perf line — re-formats into a heap buffer so the whole line, including its
// trailing newline, is emitted instead of being clipped at 255 bytes (which
// dropped the newline and ran consecutive log lines together). Returns the
// untruncated formatted length, per the esp_log_set_vprintf contract.
int formatAndEmitLog(const char *fmt, std::va_list args)
{
	char stackBuf[256];
	std::va_list argsCopy;
	va_copy(argsCopy, args);
	const int len = std::vsnprintf(stackBuf, sizeof(stackBuf), fmt, args);
	if (len <= 0) {
		va_end(argsCopy);
		return len;
	}
	if (len < static_cast<int>(sizeof(stackBuf))) {
		std::fwrite(stackBuf, 1, static_cast<std::size_t>(len), stdout);
		diagnostics_internal::writeLog(stackBuf, static_cast<std::size_t>(len));
		va_end(argsCopy);
		return len;
	}
	const std::size_t size = static_cast<std::size_t>(len) + 1;
	char *heapBuf = static_cast<char *>(gea::framework::memory::Allocator::allocatePreferSpiram(size));
	if (heapBuf) {
		std::vsnprintf(heapBuf, size, fmt, argsCopy);
		std::fwrite(heapBuf, 1, static_cast<std::size_t>(len), stdout);
		diagnostics_internal::writeLog(heapBuf, static_cast<std::size_t>(len));
		gea::framework::memory::Allocator::free(heapBuf);
	} else {
		// Allocation failed: emit the clipped stack buffer rather than nothing.
		std::fwrite(stackBuf, 1, sizeof(stackBuf) - 1, stdout);
		diagnostics_internal::writeLog(stackBuf, sizeof(stackBuf) - 1);
	}
	va_end(argsCopy);
	return len;
}

}  // namespace

int DiagnosticsServer::vprint(const char *fmt, std::va_list args)
{
	return formatAndEmitLog(fmt, args);
}

void DiagnosticsServer::print(const char *fmt, ...)
{
	std::va_list args;
	va_start(args, fmt);
	formatAndEmitLog(fmt, args);
	va_end(args);
}

void DiagnosticsServer::start()
{
	diagnostics_internal::startServer();
}

void HeapProbe::log(const char *stage)
{
	diagnosticsPlatform().logHeapProbe(stage);
}

void StackProbe::logCurrentTask(const char *stage)
{
	diagnosticsPlatform().logCurrentTaskStackProbe(stage);
}

void HeapTaskSummary::log(const char *stage)
{
	diagnosticsPlatform().logHeapTaskSummary(stage);
}

}  // namespace gea::framework::services

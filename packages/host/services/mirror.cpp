// SPDX-License-Identifier: Apache-2.0
#include "services/mirror.h"

#include "memory.h"
#include "services/app_state.h"
#include "services/diagnostics.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace gea::framework::services {

namespace {

constexpr int kChannelMirror = 2;
constexpr int kMirrorIntervalMs = 16;
constexpr char kCmdEnableMirror = 'M';
constexpr char kCmdDisableMirror = 'm';
constexpr bool kForwardLogsDuringMirror = false;
constexpr bool kMirrorDebug = false;

class MirrorMemory {
public:
	static void *allocate(std::size_t size)
	{
		return gea::framework::memory::Allocator::allocatePreferSpiram(size);
	}

	static void release(void *ptr)
	{
		gea::framework::memory::Allocator::free(ptr);
	}
};

class MirrorRecord {
public:
	static int writeError(unsigned char *dst, int cap, const char *message)
	{
		if (!dst || cap < 2) return 0;
		if (!message) message = "store mirror is unavailable for this app";
		int msg_len = static_cast<int>(std::strlen(message));
		if (msg_len > 255) msg_len = 255;
		if (msg_len > cap - 2) msg_len = cap - 2;
		if (msg_len < 0) msg_len = 0;
		dst[0] = 7;
		dst[1] = static_cast<unsigned char>(msg_len & 0xFF);
		if (msg_len > 0) std::memcpy(dst + 2, message, static_cast<std::size_t>(msg_len));
		return msg_len + 2;
	}

	static void readCurrentAppId(char *dst, std::size_t cap)
	{
		if (!dst || cap == 0) return;
		dst[0] = '\0';
		AppState::lock();
		const char *app_id = AppState::currentAppId();
		if (app_id && app_id[0]) std::snprintf(dst, cap, "%s", app_id);
		AppState::unlock();
	}

	static std::uint16_t readU16(const unsigned char *src)
	{
		return static_cast<std::uint16_t>(src[0]) | (static_cast<std::uint16_t>(src[1]) << 8);
	}

	static std::int32_t readI32(const unsigned char *src)
	{
		std::uint32_t raw = static_cast<std::uint32_t>(src[0]) |
		                    (static_cast<std::uint32_t>(src[1]) << 8) |
		                    (static_cast<std::uint32_t>(src[2]) << 16) |
		                    (static_cast<std::uint32_t>(src[3]) << 24);
		return static_cast<std::int32_t>(raw);
	}

	static std::uint32_t readU32(const unsigned char *src)
	{
		return static_cast<std::uint32_t>(src[0]) |
		       (static_cast<std::uint32_t>(src[1]) << 8) |
		       (static_cast<std::uint32_t>(src[2]) << 16) |
		       (static_cast<std::uint32_t>(src[3]) << 24);
	}
};

class MirrorDebugLog {
public:
	static void record(const char *direction, const unsigned char *record, int len)
	{
		if (!kMirrorDebug) {
			(void)direction;
			(void)record;
			(void)len;
			return;
		}

		if (!record || len <= 0) return;
		switch (record[0]) {
		case 1:
			logBegin(direction, record, len);
			break;
		case 2:
			if (len >= 7) DiagnosticsServer::print("mirror %s int field=%u value=%ld len=%d\n",
			                                        direction,
			                                        static_cast<unsigned>(MirrorRecord::readU16(record + 1)),
			                                        static_cast<long>(MirrorRecord::readI32(record + 3)),
			                                        len);
			else DiagnosticsServer::print("mirror %s int malformed len=%d\n", direction, len);
			break;
		case 3:
			if (len >= 5) DiagnosticsServer::print("mirror %s string field=%u bytes=%u len=%d\n",
			                                        direction,
			                                        static_cast<unsigned>(MirrorRecord::readU16(record + 1)),
			                                        static_cast<unsigned>(MirrorRecord::readU16(record + 3)),
			                                        len);
			else DiagnosticsServer::print("mirror %s string malformed len=%d\n", direction, len);
			break;
		case 4:
			if (len >= 5) DiagnosticsServer::print("mirror %s array_len field=%u value=%u len=%d\n",
			                                        direction,
			                                        static_cast<unsigned>(MirrorRecord::readU16(record + 1)),
			                                        static_cast<unsigned>(MirrorRecord::readU16(record + 3)),
			                                        len);
			else DiagnosticsServer::print("mirror %s array_len malformed len=%d\n", direction, len);
			break;
		case 5:
			if (len >= 10) DiagnosticsServer::print("mirror %s array_int field=%u index=%u subfield=%u value=%ld len=%d\n",
			                                         direction,
			                                         static_cast<unsigned>(MirrorRecord::readU16(record + 1)),
			                                         static_cast<unsigned>(MirrorRecord::readU16(record + 3)),
			                                         static_cast<unsigned>(record[5]),
			                                         static_cast<long>(MirrorRecord::readI32(record + 6)),
			                                         len);
			else DiagnosticsServer::print("mirror %s array_int malformed len=%d\n", direction, len);
			break;
		case 8:
			if (len >= 7) DiagnosticsServer::print("mirror %s scroll node=%u y=%ld len=%d\n",
			                                        direction,
			                                        static_cast<unsigned>(MirrorRecord::readU16(record + 1)),
			                                        static_cast<long>(MirrorRecord::readI32(record + 3)),
			                                        len);
			else DiagnosticsServer::print("mirror %s scroll malformed len=%d\n", direction, len);
			break;
		case 6:
			DiagnosticsServer::print("mirror %s end len=%d\n", direction, len);
			break;
		case 7:
			DiagnosticsServer::print("mirror %s error len=%d\n", direction, len);
			break;
		default:
			DiagnosticsServer::print("mirror %s unknown kind=%u len=%d\n", direction, static_cast<unsigned>(record[0]), len);
			break;
		}
	}

private:
	static void logBegin(const char *direction, const unsigned char *record, int len)
	{
		int msg_kind = len > 1 ? record[1] : 0;
		int app_len = len > 2 ? record[2] : 0;
		if (app_len > len - 3) app_len = len > 3 ? len - 3 : 0;
		if (app_len < 0) app_len = 0;
		char app_id[64];
		int copy_len = app_len < static_cast<int>(sizeof(app_id)) - 1 ? app_len : static_cast<int>(sizeof(app_id)) - 1;
		if (copy_len > 0) std::memcpy(app_id, record + 3, static_cast<std::size_t>(copy_len));
		app_id[copy_len] = '\0';
		if (len >= 3 + app_len + 6) {
			int schema_off = 3 + app_len;
			DiagnosticsServer::print("mirror %s begin type=%s app=%s fields=%u schema=0x%08lx len=%d\n",
			                         direction,
			                         msg_kind == 1 ? "snapshot" : (msg_kind == 2 ? "diff" : "unknown"),
			                         app_id,
			                         static_cast<unsigned>(MirrorRecord::readU16(record + schema_off)),
			                         static_cast<unsigned long>(MirrorRecord::readU32(record + schema_off + 2)),
			                         len);
		} else {
			DiagnosticsServer::print("mirror %s begin type=%s app=%s len=%d\n",
			                         direction,
			                         msg_kind == 1 ? "snapshot" : (msg_kind == 2 ? "diff" : "unknown"),
			                         app_id,
			                         len);
		}
	}
};

}  // namespace

int MirrorService::beginSnapshot() { return 0; }
int MirrorService::beginDiff() { return 0; }
int MirrorService::nextRecord(unsigned char *dst, int capacity) { (void)dst; (void)capacity; return 0; }
void MirrorService::clearDirty() {}

MirrorTransport::~MirrorTransport()
{
	releaseBuffer();
}

int MirrorTransport::pollIntervalMs()
{
	return kMirrorIntervalMs;
}

bool MirrorTransport::enabled() const
{
	return enabled_;
}

bool MirrorTransport::suppressesLogForwarding() const
{
	return enabled_ && !kForwardLogsDuringMirror;
}

void MirrorTransport::reset()
{
	enabled_ = false;
	forceSnapshot_ = true;
	messageActive_ = false;
	heldLen_ = 0;
	appId_[0] = '\0';
	nextAt_ = 0;
	releaseBuffer();
}

bool MirrorTransport::handleCommand(char command)
{
	if (command == kCmdEnableMirror) {
		enable();
		return true;
	}
	if (command == kCmdDisableMirror) {
		disable();
		return true;
	}
	return false;
}

void MirrorTransport::enable()
{
	DiagnosticsServer::print("mirror rx command enable\n");
	enabled_ = true;
	forceSnapshot_ = true;
	messageActive_ = false;
	heldLen_ = 0;
	appId_[0] = '\0';
}

void MirrorTransport::disable()
{
	DiagnosticsServer::print("mirror rx command disable\n");
	reset();
}

bool MirrorTransport::ready(std::int64_t now) const
{
	return enabled_ && (messageActive_ || forceSnapshot_ || now >= nextAt_);
}

void MirrorTransport::syncCurrentApp()
{
	if (!enabled_) return;
	char current_app_id[sizeof(appId_)];
	MirrorRecord::readCurrentAppId(current_app_id, sizeof(current_app_id));
	if (std::strcmp(current_app_id, appId_) != 0) {
		DiagnosticsServer::print("mirror active app changed: '%s' -> '%s'\n", appId_, current_app_id);
		std::snprintf(appId_, sizeof(appId_), "%s", current_app_id);
		forceSnapshot_ = true;
		messageActive_ = false;
		heldLen_ = 0;
		nextAt_ = 0;
	}
}

bool MirrorTransport::ensureBuffer(DiagnosticsFrame &pending)
{
	if (!enabled_ || buffer_) return true;
	cap_ = pending.payloadCapacity();
	buffer_ = static_cast<unsigned char *>(MirrorMemory::allocate(static_cast<std::size_t>(cap_)));
	if (buffer_) return true;

	unsigned char err_payload[96];
	int err_len = MirrorRecord::writeError(err_payload, sizeof(err_payload), "mirror alloc failed");
	MirrorDebugLog::record("tx", err_payload, err_len);
	pending.queue(kChannelMirror, 1, err_payload, err_len);
	enabled_ = false;
	cap_ = 0;
	return false;
}

void MirrorTransport::beginIfDue(std::int64_t now, DiagnosticsFrame &pending)
{
	if (!enabled_ || now < nextAt_ || messageActive_) return;

	int is_snapshot = forceSnapshot_;
	AppState::lock();
	int started = is_snapshot ? MirrorService::beginSnapshot() : MirrorService::beginDiff();
	AppState::unlock();
	if (started > 0) {
		if (is_snapshot) DiagnosticsServer::print("mirror begin snapshot for app=%s\n", appId_);
		messageActive_ = true;
		heldLen_ = 0;
		forceSnapshot_ = false;
	} else if (forceSnapshot_) {
		unsigned char err_payload[160];
		int err_len = MirrorRecord::writeError(err_payload, sizeof(err_payload), "store mirror is unavailable for this app");
		MirrorDebugLog::record("tx", err_payload, err_len);
		pending.queue(kChannelMirror, 1, err_payload, err_len);
		forceSnapshot_ = false;
		nextAt_ = now + static_cast<std::int64_t>(kMirrorIntervalMs) * 1000;
	} else {
		nextAt_ = now + static_cast<std::int64_t>(kMirrorIntervalMs) * 1000;
	}
}

void MirrorTransport::appendBatch(std::int64_t now, DiagnosticsFrame &pending)
{
	if (!enabled_ || !messageActive_ || !buffer_ || !pending.empty()) return;

	int batch_len = 0;
	unsigned char *batch = pending.payload();
	const int payload_capacity = pending.payloadCapacity();
	while (batch_len < payload_capacity) {
		int rec_len = heldLen_;
		if (rec_len <= 0) {
			AppState::lock();
			rec_len = MirrorService::nextRecord(buffer_, cap_);
			AppState::unlock();
		}
		if (rec_len <= 0) {
			endMessage(now);
			break;
		}
		if (rec_len > payload_capacity) rec_len = payload_capacity;
		if (batch_len > 0 && batch_len + rec_len > payload_capacity) {
			heldLen_ = rec_len;
			break;
		}
		MirrorDebugLog::record("tx", buffer_, rec_len);
		std::memcpy(batch + batch_len, buffer_, static_cast<std::size_t>(rec_len));
		batch_len += rec_len;
		heldLen_ = 0;
		if (buffer_[0] == 6 || buffer_[0] == 7) {
			endMessage(now);
			break;
		}
	}
	if (batch_len > 0) pending.queue(kChannelMirror, 1, batch, batch_len);
}

void MirrorTransport::endMessage(std::int64_t now)
{
	messageActive_ = false;
	heldLen_ = 0;
	nextAt_ = now + static_cast<std::int64_t>(kMirrorIntervalMs) * 1000;
}

void MirrorTransport::releaseBuffer()
{
	if (!buffer_) return;
	MirrorMemory::release(buffer_);
	buffer_ = nullptr;
	cap_ = 0;
}

}  // namespace gea::framework::services

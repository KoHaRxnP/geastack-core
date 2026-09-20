// SPDX-License-Identifier: Apache-2.0
#include "services/diagnostics.h"

#include "diagnostics_internal.h"
#include "services/mirror.h"

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace gea::framework::services {

namespace {

constexpr int kFrameHeaderSize = 4;
constexpr int kMaxFramePayload = 1024;
constexpr int kMaxFrameSize = kFrameHeaderSize + kMaxFramePayload;
constexpr int kChannelLog = 1;
constexpr int kBackpressureDelayMs = 250;
constexpr int kBackpressureLogIntervalMs = 5000;

class PendingFrame : public DiagnosticsFrame {
public:
	bool hasPending() const { return len_ > offset_; }
	bool empty() const override { return len_ == 0; }
	int payloadCapacity() const override { return kMaxFramePayload; }

	void clear()
	{
		len_ = 0;
		offset_ = 0;
	}

	unsigned char *payload() override
	{
		return data_ + kFrameHeaderSize;
	}

	bool queue(int channel, int type, const unsigned char *payload, int payload_len) override
	{
		if (payload_len < 0) payload_len = 0;
		if (payload_len > kMaxFramePayload) payload_len = kMaxFramePayload;
		data_[0] = static_cast<unsigned char>(channel & 0xFF);
		data_[1] = static_cast<unsigned char>(type & 0xFF);
		data_[2] = static_cast<unsigned char>(payload_len & 0xFF);
		data_[3] = static_cast<unsigned char>((payload_len >> 8) & 0xFF);
		if (payload && payload_len > 0 && payload != data_ + kFrameHeaderSize) {
			std::memmove(data_ + kFrameHeaderSize, payload, static_cast<std::size_t>(payload_len));
		}
		len_ = kFrameHeaderSize + payload_len;
		offset_ = 0;
		return true;
	}

	int flushTo(DiagnosticsOutput &output, int *err_out)
	{
		if (err_out) *err_out = 0;
		if (len_ <= 0) return 1;
		while (offset_ < len_) {
			int written = output.write(data_ + offset_, len_ - offset_, err_out);
			if (written < 0) return -1;
			if (written == 0) return 0;
			offset_ += written;
		}
		return 1;
	}

private:
	unsigned char data_[kMaxFrameSize] = {};
	int len_ = 0;
	int offset_ = 0;
};

}  // namespace

class DiagnosticsConnection::Impl {
public:
	void reset()
	{
		mirror_.reset();
		pending_.clear();
		logCursor_ = diagnostics_internal::oldestLogTotal();
		lastBackpressureLogAt_ = 0;
	}

	bool hasPendingOutput() const
	{
		return pending_.hasPending();
	}

	bool wantsImmediatePoll(std::int64_t now_us) const
	{
		if (pending_.hasPending()) return true;
#ifndef GEA_EMBEDDED_MIRROR_DISABLED
		return mirror_.ready(now_us);
#else
		(void)now_us;
		return false;
#endif
	}

	int pollIntervalMs() const
	{
		return MirrorTransport::pollIntervalMs();
	}

	int backpressureDelayMs() const
	{
		return kBackpressureDelayMs;
	}

	void handleIncoming(const char *data, int len)
	{
		if (!data || len <= 0) return;
#ifndef GEA_EMBEDDED_MIRROR_DISABLED
		for (int i = 0; i < len; i++) {
			mirror_.handleCommand(data[i]);
		}
#else
		(void)data;
		(void)len;
#endif
	}

	DiagnosticsSendStatus flush(DiagnosticsOutput &output, std::int64_t now_us, int *err_out)
	{
		if (!pending_.hasPending()) return DiagnosticsSendStatus::Idle;
		int send_result = pending_.flushTo(output, err_out);
		if (send_result > 0) {
			pending_.clear();
			return DiagnosticsSendStatus::Sent;
		}
		if (send_result == 0) {
			int err = err_out ? *err_out : 0;
			if (now_us - lastBackpressureLogAt_ >=
			    static_cast<std::int64_t>(kBackpressureLogIntervalMs) * 1000) {
				DiagnosticsServer::print("Diagnostics send backpressure: errno=%d\n", err);
				lastBackpressureLogAt_ = now_us;
			}
			return DiagnosticsSendStatus::Backpressure;
		}
		return DiagnosticsSendStatus::Closed;
	}

	void prepareNextFrame(std::int64_t now_us)
	{
		if (pending_.hasPending()) return;

		int forward_logs = 1;
#ifndef GEA_EMBEDDED_MIRROR_DISABLED
		if (mirror_.suppressesLogForwarding()) forward_logs = 0;
#endif
		if (forward_logs) {
			char log_chunk[96];
			int log_len = diagnostics_internal::copyLogSince(&logCursor_, log_chunk, sizeof(log_chunk));
			if (log_len > 0) {
				pending_.queue(kChannelLog, 1, reinterpret_cast<const unsigned char *>(log_chunk), log_len);
				return;
			}
		} else {
			logCursor_ = diagnostics_internal::latestLogTotal();
		}

#ifndef GEA_EMBEDDED_MIRROR_DISABLED
		mirror_.syncCurrentApp();
		if (!mirror_.ensureBuffer(pending_)) return;
		mirror_.beginIfDue(now_us, pending_);
		mirror_.appendBatch(now_us, pending_);
#else
		(void)now_us;
#endif
	}

private:
	MirrorTransport mirror_;
	PendingFrame pending_;
	std::size_t logCursor_ = 0;
	std::int64_t lastBackpressureLogAt_ = 0;
};

DiagnosticsConnection::DiagnosticsConnection()
	: impl_(new Impl())
{
}

DiagnosticsConnection::~DiagnosticsConnection()
{
	delete impl_;
}

void DiagnosticsConnection::reset()
{
	impl_->reset();
}

bool DiagnosticsConnection::hasPendingOutput() const
{
	return impl_->hasPendingOutput();
}

bool DiagnosticsConnection::wantsImmediatePoll(std::int64_t now_us) const
{
	return impl_->wantsImmediatePoll(now_us);
}

int DiagnosticsConnection::pollIntervalMs() const
{
	return impl_->pollIntervalMs();
}

int DiagnosticsConnection::backpressureDelayMs() const
{
	return impl_->backpressureDelayMs();
}

void DiagnosticsConnection::handleIncoming(const char *data, int len)
{
	impl_->handleIncoming(data, len);
}

DiagnosticsSendStatus DiagnosticsConnection::flush(DiagnosticsOutput &output, std::int64_t now_us, int *err_out)
{
	return impl_->flush(output, now_us, err_out);
}

void DiagnosticsConnection::prepareNextFrame(std::int64_t now_us)
{
	impl_->prepareNextFrame(now_us);
}

}  // namespace gea::framework::services

#pragma once

#include "services/diagnostics.h"

#include <cstdint>

namespace gea::framework::services {

class MirrorService {
public:
	static int beginSnapshot();
	static int beginDiff();
	static int nextRecord(unsigned char *dst, int capacity);
	static void clearDirty();
};

class MirrorTransport {
public:
	~MirrorTransport();

	static int pollIntervalMs();

	bool enabled() const;
	bool suppressesLogForwarding() const;
	void reset();
	bool handleCommand(char command);
	bool ready(std::int64_t now) const;
	void syncCurrentApp();
	bool ensureBuffer(DiagnosticsFrame &pending);
	void beginIfDue(std::int64_t now, DiagnosticsFrame &pending);
	void appendBatch(std::int64_t now, DiagnosticsFrame &pending);

private:
	void enable();
	void disable();
	void endMessage(std::int64_t now);
	void releaseBuffer();

	bool enabled_ = false;
	bool forceSnapshot_ = true;
	bool messageActive_ = false;
	unsigned char *buffer_ = nullptr;
	int cap_ = 0;
	int heldLen_ = 0;
	std::int64_t nextAt_ = 0;
	char appId_[64] = "";
};

}  // namespace gea::framework::services

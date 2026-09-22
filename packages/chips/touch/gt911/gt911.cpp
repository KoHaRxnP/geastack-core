#include "gt911.h"

namespace gea::chips::gt911 {

bool ControllerCore::configure(RegisterBus &bus) {
	std::uint8_t id[4] = {0, 0, 0, 0};
	if (!bus.readRegisters(kRegProductId, id, sizeof(id))) return false;
	if (id[0] != '9' || id[1] != '1' || id[2] != '1') return false;
	// Clear a stale report left over from before reset so the first read sees
	// the buffer-ready bit only once new coordinates arrive.
	const std::uint8_t clear = 0;
	bus.writeRegisters(kRegStatus, &clear, 1);
	last_ = {};
	return true;
}

bool ControllerCore::read(RegisterBus &bus, TouchSample &sample) {
	MultiTouchSample multi;
	const bool touching = readMulti(bus, multi);
	sample.touching = touching;
	if (touching) {
		sample.x = multi.x[0];
		sample.y = multi.y[0];
	}
	return touching;
}

bool ControllerCore::readMulti(RegisterBus &bus, MultiTouchSample &sample) {
	std::uint8_t status = 0;
	if (!bus.readRegisters(kRegStatus, &status, 1)) {
		sample = last_;
		return sample.count > 0;
	}
	if ((status & 0x80) == 0) {
		// No new report since the last one was acknowledged.
		sample = last_;
		return sample.count > 0;
	}

	int count = status & 0x0f;
	if (count > kMaxReportedPoints) count = 0;
	MultiTouchSample next;
	if (count > 0) {
		const int wanted = count < kMaxTouchPoints ? count : kMaxTouchPoints;
		std::uint8_t data[kPointStride * kMaxTouchPoints] = {};
		if (!bus.readRegisters(kRegPointData, data, static_cast<std::size_t>(wanted) * kPointStride)) {
			sample = last_;
			return sample.count > 0;
		}
		for (int i = 0; i < wanted; i++) {
			const std::uint8_t *point = data + i * kPointStride;
			// point[0] is the track id; a lifted-and-replaced finger keeps its slot.
			next.x[i] = point[1] | (point[2] << 8);
			next.y[i] = point[3] | (point[4] << 8);
		}
		next.count = wanted;
	}

	// Acknowledge the report; the controller raises buffer-ready again on the
	// next scan. Done after the point read so the data stays consistent.
	const std::uint8_t clear = 0;
	bus.writeRegisters(kRegStatus, &clear, 1);

	last_ = next;
	sample = next;
	return sample.count > 0;
}

}  // namespace gea::chips::gt911

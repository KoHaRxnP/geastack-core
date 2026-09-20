#pragma once

#include <cstddef>
#include <cstdint>

namespace gea::framework::services {

class OtaServer {
public:
	enum class Result : std::uint8_t {
		Ok = 0,
		InvalidSize = 1,
		NoPartition = 2,
		BeginFailed = 3,
		WriteFailed = 4,
		SizeMismatch = 5,
		ValidationFailed = 6,
		BootSelectionFailed = 7,
		NotStarted = 8,
	};

	static bool start();
	static Result beginImage(std::size_t imageSize);
	static Result writeImage(const std::uint8_t *data, std::size_t length);
	static Result finishImage();
	static void abortImage();
	static std::size_t receivedBytes();
	static std::size_t expectedBytes();
	static void rebootSoon();
};

}  // namespace gea::framework::services

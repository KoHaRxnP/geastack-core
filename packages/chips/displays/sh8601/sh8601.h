#pragma once

#include <array>
#include <cstdint>

namespace gea::chips::sh8601 {

inline constexpr int kPanelXGap = 0;
inline constexpr int kPanelYGap = 0;
inline constexpr std::uint8_t kWriteCommandOpcode = 0x02;
inline constexpr std::uint8_t kWriteColorOpcode = 0x32;

struct AddressWindow {
	std::array<std::uint8_t, 4> columns;
	std::array<std::uint8_t, 4> rows;
};

class CommandSet {
public:
	static int qspiCommand(std::uint8_t opcode, int command);
	static int qspiParameterCommand(int command);
	static int qspiColorCommand(int command);
	static AddressWindow addressWindow(int x0, int y0, int x1, int y1);
};

}  // namespace gea::chips::sh8601

#pragma once

#include <array>
#include <cstdint>

namespace gea::chips::co5300 {

#ifndef GEA_EMBEDDED_CO5300_PANEL_X_GAP
#define GEA_EMBEDDED_CO5300_PANEL_X_GAP 22
#endif

#ifndef GEA_EMBEDDED_CO5300_PANEL_Y_GAP
#define GEA_EMBEDDED_CO5300_PANEL_Y_GAP 0
#endif

inline constexpr int kPanelXGap = GEA_EMBEDDED_CO5300_PANEL_X_GAP;
inline constexpr int kPanelYGap = GEA_EMBEDDED_CO5300_PANEL_Y_GAP;
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

}  // namespace gea::chips::co5300

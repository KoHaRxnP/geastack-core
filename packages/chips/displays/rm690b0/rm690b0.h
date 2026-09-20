#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace gea::chips::rm690b0 {

#ifndef GEA_EMBEDDED_RM690B0_PANEL_X_GAP
#define GEA_EMBEDDED_RM690B0_PANEL_X_GAP 16
#endif

#ifndef GEA_EMBEDDED_RM690B0_PANEL_Y_GAP
#define GEA_EMBEDDED_RM690B0_PANEL_Y_GAP 0
#endif

inline constexpr int kWidth = 450;
inline constexpr int kHeight = 600;
inline constexpr int kPanelXGap = GEA_EMBEDDED_RM690B0_PANEL_X_GAP;
inline constexpr int kPanelYGap = GEA_EMBEDDED_RM690B0_PANEL_Y_GAP;
inline constexpr std::uint8_t kWriteCommandOpcode = 0x02;
inline constexpr std::uint8_t kWriteColorOpcode = 0x32;
inline constexpr std::uint8_t kMemoryWrite = 0x2C;
inline constexpr std::uint8_t kBrightness = 0x51;

struct AddressWindow {
	std::array<std::uint8_t, 4> columns;
	std::array<std::uint8_t, 4> rows;
};

struct InitCommand {
	std::uint8_t command;
	const std::uint8_t *data;
	std::size_t length;
	int delayMs;
};

class CommandSet {
public:
	static int qspiCommand(std::uint8_t opcode, int command);
	static int qspiParameterCommand(int command);
	static int qspiColorCommand(int command);
	static AddressWindow addressWindow(int x0, int y0, int x1, int y1);
	static std::uint8_t brightnessByte(int brightnessPercent);
	static const InitCommand *initCommands(std::size_t &count);
};

}  // namespace gea::chips::rm690b0

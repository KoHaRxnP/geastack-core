#include "displays/rm690b0/rm690b0.h"

namespace gea::chips::rm690b0 {

namespace {

std::array<std::uint8_t, 4> packRange(int start, int end) {
	return {
	    static_cast<std::uint8_t>((start >> 8) & 0xFF),
	    static_cast<std::uint8_t>(start & 0xFF),
	    static_cast<std::uint8_t>((end >> 8) & 0xFF),
	    static_cast<std::uint8_t>(end & 0xFF),
	};
}

constexpr std::uint8_t kTearScanline[] = {0x01, 0xD1};
constexpr std::uint8_t kFe20[] = {0x20};
constexpr std::uint8_t kCmd63[] = {0xFF};
constexpr std::uint8_t kCmd26[] = {0x0A};
constexpr std::uint8_t kCmd24[] = {0x80};
constexpr std::uint8_t kFe00[] = {0x00};
constexpr std::uint8_t kPixelFormatRgb565[] = {0x55};
constexpr std::uint8_t kCmdC4[] = {0x80};
constexpr std::uint8_t kCmdC2[] = {0x00};
constexpr std::uint8_t kTearOn[] = {0x00};
constexpr std::uint8_t kBrightnessOff[] = {0x00};
constexpr std::uint8_t kBrightnessFull[] = {0xFF};

constexpr InitCommand kInitCommands[] = {
    {0x11, nullptr, 0, 120},
    {0x44, kTearScanline, sizeof(kTearScanline), 0},
    {0xFE, kFe20, sizeof(kFe20), 0},
    {0x63, kCmd63, sizeof(kCmd63), 0},
    {0x26, kCmd26, sizeof(kCmd26), 0},
    {0x24, kCmd24, sizeof(kCmd24), 0},
    {0xFE, kFe00, sizeof(kFe00), 0},
    {0x3A, kPixelFormatRgb565, sizeof(kPixelFormatRgb565), 0},
    {0xC4, kCmdC4, sizeof(kCmdC4), 0},
    {0xC2, kCmdC2, sizeof(kCmdC2), 10},
    {0x35, kTearOn, sizeof(kTearOn), 0},
    {0x51, kBrightnessOff, sizeof(kBrightnessOff), 0},
    {0x29, nullptr, 0, 10},
    {0x51, kBrightnessFull, sizeof(kBrightnessFull), 10},
};

}  // namespace

int CommandSet::qspiCommand(std::uint8_t opcode, int command) {
	return ((command & 0xFF) << 8) | (static_cast<int>(opcode) << 24);
}

int CommandSet::qspiParameterCommand(int command) {
	return qspiCommand(kWriteCommandOpcode, command);
}

int CommandSet::qspiColorCommand(int command) {
	return command >= 0 ? qspiCommand(kWriteColorOpcode, command) : -1;
}

AddressWindow CommandSet::addressWindow(int x0, int y0, int x1, int y1) {
	x0 += kPanelXGap;
	x1 += kPanelXGap;
	y0 += kPanelYGap;
	y1 += kPanelYGap;

	AddressWindow window{};
	window.columns = packRange(x0, x1);
	window.rows = packRange(y0, y1);
	return window;
}

std::uint8_t CommandSet::brightnessByte(int brightnessPercent) {
	if (brightnessPercent < 0) brightnessPercent = 0;
	if (brightnessPercent > 100) brightnessPercent = 100;
	return static_cast<std::uint8_t>((brightnessPercent * 255) / 100);
}

const InitCommand *CommandSet::initCommands(std::size_t &count) {
	count = sizeof(kInitCommands) / sizeof(kInitCommands[0]);
	return kInitCommands;
}

}  // namespace gea::chips::rm690b0

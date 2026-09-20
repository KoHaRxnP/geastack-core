#include "displays/co5300/co5300.h"

namespace gea::chips::co5300 {

namespace {

std::array<std::uint8_t, 4> packRange(int start, int end) {
	return {
	    static_cast<std::uint8_t>((start >> 8) & 0xFF),
	    static_cast<std::uint8_t>(start & 0xFF),
	    static_cast<std::uint8_t>((end >> 8) & 0xFF),
	    static_cast<std::uint8_t>(end & 0xFF),
	};
}

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

}  // namespace gea::chips::co5300

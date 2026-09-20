#include "canvas.h"

#include <array>
#include <cstdint>
#include <cstdio>

int main()
{
	using gea::framework::graphics::Canvas;

	constexpr int kWidth = 50;
	constexpr int kHeight = 50;
	constexpr int kX = 8;
	constexpr int kY = 8;
	constexpr int kSize = 34;
	constexpr int kRadius = 9;
	constexpr std::uint16_t kGreen = 0x07e0;

	std::array<std::uint16_t, kWidth * kHeight> pixels{};
	Canvas canvas;
	canvas.bindPixels(pixels.data(), kWidth, kHeight);
	canvas.clear(0x0000);
	canvas.setGlobalAlpha(45);
	canvas.fillRoundedRect(kX, kY, kSize, kSize, kRadius, kRadius, kRadius, kRadius, kGreen);

	const std::uint16_t seam = pixels[(kY + kRadius) * kWidth + (kX + kRadius)];
	const std::uint16_t neighbor = pixels[(kY + kRadius) * kWidth + (kX + kRadius + 1)];
	if (seam != neighbor) {
		std::fprintf(stderr,
		             "[test_canvas_rounded_rect_alpha] translucent rounded rect should blend each covered pixel once, seam=0x%04x neighbor=0x%04x\n",
		             seam,
		             neighbor);
		return 1;
	}

	return 0;
}

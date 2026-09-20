// Regression guard for PackedPixels<Bits>::copyPacked overlap correctness.
//
// The bug this locks down: the same-phase fast path used to do
// lead-then-memmove-then-tail unconditionally. For a RIGHTWARD overlapping move
// on a single row (dstX > srcX, same byte phase) that is wrong twice over — the
// leading partial-byte read-modify-write lands inside the memmove's SOURCE
// range, and the trailing loop then reads source pixels the memmove has already
// overwritten.
//
// It is a live path, not a theoretical one: Canvas::scrollRect passes
// dstRow == srcRow for a horizontal pan (dy == 0, dx != 0), reached from
// ui/tree_render.cpp's Display::scrollRect(vx, vy, vw, vh, panDx, 0). That is
// horizontal scrolling on every packed-framebuffer board — the GRAY4 e-paper
// targets (M5Paper, LilyGo T5) and the GRAY2 one (Xteink X3).
//
// Reference semantics are memmove's: as if the whole source region were
// snapshotted first, then stored.
//
// This file also runs a NEGATIVE CONTROL — the original algorithm, pasted
// verbatim — and FAILS if that old code passes. Without it, a future refactor
// could quietly turn the whole test into a tautology.
#include "pixel.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <vector>

using namespace gea::framework::graphics::pixel;

namespace {

int gFail = 0;

#define CHECK(cond, ...)                                                                       \
	do {                                                                                        \
		if (!(cond)) {                                                                          \
			std::printf("FAIL %s:%d ", __FILE__, __LINE__);                                     \
			std::printf(__VA_ARGS__);                                                           \
			std::printf("\n");                                                                  \
			if (++gFail > 20) { std::printf("too many failures\n"); std::exit(1); }              \
		}                                                                                       \
	} while (0)

// Overlap-safe reference: snapshot every source pixel, then store.
template <int Bits>
void refCopy(std::uint8_t *dstRow, int dstX, const std::uint8_t *srcRow, int srcX, int count)
{
	using P = PackedPixels<Bits>;
	std::vector<std::uint8_t> tmp(static_cast<std::size_t>(count > 0 ? count : 0));
	for (int i = 0; i < count; i++) tmp[static_cast<std::size_t>(i)] = P::get(srcRow, srcX + i);
	for (int i = 0; i < count; i++) P::set(dstRow, dstX + i, tmp[static_cast<std::size_t>(i)]);
}

// The ORIGINAL (buggy) algorithm, verbatim, for the negative control.
template <int Bits>
struct OldPacked {
	using P = PackedPixels<Bits>;
	static constexpr int kPxPerByte = P::kPxPerByte;

	static void copyPacked(std::uint8_t *dstRow, int dstX, const std::uint8_t *srcRow, int srcX, int count)
	{
		if ((dstX % kPxPerByte) == (srcX % kPxPerByte)) {
			int x = 0;
			while (x < count && ((dstX + x) % kPxPerByte) != 0) {
				P::set(dstRow, dstX + x, P::get(srcRow, srcX + x));
				x++;
			}
			const int mid = (count - x) / kPxPerByte;
			if (mid > 0)
				std::memmove(dstRow + P::byteIndex(dstX + x), srcRow + P::byteIndex(srcX + x),
				             static_cast<std::size_t>(mid));
			for (int i = x + mid * kPxPerByte; i < count; i++)
				P::set(dstRow, dstX + i, P::get(srcRow, srcX + i));
			return;
		}
		if (dstX <= srcX)
			for (int i = 0; i < count; i++) P::set(dstRow, dstX + i, P::get(srcRow, srcX + i));
		else
			for (int i = count - 1; i >= 0; i--) P::set(dstRow, dstX + i, P::get(srcRow, srcX + i));
	}
};

// Every (dstX, srcX, count) on a short row, several byte patterns, aliasing.
// Impl is a template-template so the same sweep drives both the real and the
// old algorithm.
template <int Bits, typename Copy>
long long sweepAliasing(Copy copyFn, bool report, const char *label)
{
	using P = PackedPixels<Bits>;
	constexpr int kRowPx = 24;
	constexpr int kRowBytes = kRowPx / P::kPxPerByte;
	long long bad = 0, total = 0;

	for (int pattern = 0; pattern < 4; pattern++) {
		std::uint8_t base[kRowBytes];
		for (int i = 0; i < kRowBytes; i++)
			base[i] = static_cast<std::uint8_t>(pattern == 0   ? i * 17 + 3
			                                    : pattern == 1 ? 0xFF
			                                    : pattern == 2 ? (i & 1 ? 0xA5 : 0x5A)
			                                                   : (i * 31 + 7));
		for (int dstX = 0; dstX < kRowPx; dstX++) {
			for (int srcX = 0; srcX < kRowPx; srcX++) {
				const int lim = kRowPx - (dstX > srcX ? dstX : srcX);
				for (int count = 0; count <= lim; count++) {
					std::uint8_t got[kRowBytes], want[kRowBytes];
					std::memcpy(got, base, kRowBytes);
					std::memcpy(want, base, kRowBytes);
					copyFn(got, dstX, got, srcX, count);
					refCopy<Bits>(want, dstX, want, srcX, count);
					total++;
					if (std::memcmp(got, want, kRowBytes) != 0) {
						bad++;
						if (report)
							CHECK(false, "%s aliasing dstX=%d srcX=%d count=%d pattern=%d", label, dstX,
							      srcX, count, pattern);
					}
				}
			}
		}
	}
	if (report) std::printf("  %s: %lld exhaustive aliasing cases\n", label, total);
	return bad;
}

template <int Bits>
void randomized(const char *label, unsigned seed)
{
	using P = PackedPixels<Bits>;
	constexpr int kRowPx = 96;
	constexpr int kRowBytes = kRowPx / P::kPxPerByte;

	std::mt19937 rng(seed);
	std::uniform_int_distribution<int> byteDist(0, 255);
	std::uniform_int_distribution<int> pxDist(0, kRowPx - 1);
	long long samePhase = 0, rightward = 0;

	constexpr int kIters = 200000;
	for (int iter = 0; iter < kIters; iter++) {
		std::uint8_t base[kRowBytes];
		for (int i = 0; i < kRowBytes; i++) base[i] = static_cast<std::uint8_t>(byteDist(rng));

		const int dstX = pxDist(rng);
		const int srcX = pxDist(rng);
		const int maxCount = kRowPx - (dstX > srcX ? dstX : srcX);
		if (maxCount <= 0) continue;
		std::uniform_int_distribution<int> cntDist(0, maxCount);
		const int count = cntDist(rng);

		if ((dstX % P::kPxPerByte) == (srcX % P::kPxPerByte)) samePhase++;
		if (dstX > srcX) rightward++;

		// Aliasing: dst and src are the SAME row.
		std::uint8_t got[kRowBytes], want[kRowBytes];
		std::memcpy(got, base, kRowBytes);
		std::memcpy(want, base, kRowBytes);
		P::copyPacked(got, dstX, got, srcX, count);
		refCopy<Bits>(want, dstX, want, srcX, count);
		CHECK(std::memcmp(got, want, kRowBytes) == 0, "%s alias dstX=%d srcX=%d count=%d", label, dstX,
		      srcX, count);

		// Distinct buffers must keep working too.
		std::uint8_t src2[kRowBytes], got2[kRowBytes], want2[kRowBytes];
		for (int i = 0; i < kRowBytes; i++) src2[i] = static_cast<std::uint8_t>(byteDist(rng));
		std::memcpy(got2, base, kRowBytes);
		std::memcpy(want2, base, kRowBytes);
		P::copyPacked(got2, dstX, src2, srcX, count);
		refCopy<Bits>(want2, dstX, src2, srcX, count);
		CHECK(std::memcmp(got2, want2, kRowBytes) == 0, "%s distinct dstX=%d srcX=%d count=%d", label,
		      dstX, srcX, count);
	}
	std::printf("  %s: %d randomized rounds (%lld same-phase, %lld rightward), aliasing + distinct\n",
	            label, kIters, samePhase, rightward);
}

// The originally-reported repro: pixels 5 and 12 came out wrong.
void reportedRepro()
{
	using P = PackedPixels<4>;
	std::uint8_t got[8], want[8];
	for (int i = 0; i < 8; i++)
		got[i] = want[i] = static_cast<std::uint8_t>(0x10 * (2 * i) + (2 * i + 1));
	P::copyPacked(got, /*dstX=*/3, got, /*srcX=*/1, /*count=*/10);
	refCopy<4>(want, 3, want, 1, 10);
	CHECK(std::memcmp(got, want, 8) == 0, "reported repro copyPacked(row,3,row,1,10) @4bpp");
	std::printf("  reported repro copyPacked(row,3,row,1,10) @4bpp: ok\n");
}

}  // namespace

int main()
{
	std::printf("packed copyPacked overlap regression test\n");

	reportedRepro();
	sweepAliasing<4>(PackedPixels<4>::copyPacked, true, "PackedPixels<4>");
	sweepAliasing<2>(PackedPixels<2>::copyPacked, true, "PackedPixels<2>");
	randomized<4>("PackedPixels<4>", 0xC0FFEEu);
	randomized<2>("PackedPixels<2>", 0xBEEF01u);

	// NEGATIVE CONTROL: the old algorithm must still fail this sweep. If it
	// passes, the sweep has stopped exercising the overlap case and every
	// "pass" above is meaningless.
	const long long old4 = sweepAliasing<4>(OldPacked<4>::copyPacked, false, "old4");
	const long long old2 = sweepAliasing<2>(OldPacked<2>::copyPacked, false, "old2");
	std::printf("  negative control: original algorithm wrong in %lld (4bpp) / %lld (2bpp) cases\n", old4,
	            old2);
	CHECK(old4 > 0 && old2 > 0,
	      "NEGATIVE CONTROL FAILED — the old buggy algorithm passes, so this test proves nothing");

	std::printf("%s (%d failures)\n", gFail == 0 ? "PASS" : "FAIL", gFail);
	return gFail == 0 ? 0 : 1;
}

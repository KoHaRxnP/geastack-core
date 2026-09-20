// Differential probe for the static-backdrop cache on css-3d-cube.
//
// The backdrop bake + transform-reproject fast path is gated on
// staticBackdropFrameSettle() returning kDynamic. That verdict was broken for
// weeks (it assumed pre-order node indices; the JSX runtime now emits
// post-order), so the whole path was dead and no test covered it. Restoring the
// verdict turned it back on and the cube rendered with corrupted pixels along
// the face seams.
//
// This probe pumps a deterministic frame sequence twice — once with a backdrop
// cache present (fast path live) and once without (gea_backdrop_cache returns
// nullptr, which disables the bake and with it the reproject) — and prints a
// hash of the PRESENTED panel per frame. The two runs must agree: the fast path
// is an optimization, so it owes pixel-identical output. The first frame whose
// hashes differ is the first frame the fast path got wrong.
//
//   run A: <runner>                       -> F <n> <hash>
//   run B: GEA_DIFF_NO_BACKDROP=1 <runner>
//   diff the two logs; then re-run both with GEA_DIFF_DUMP=<n> for row hashes,
//   and GEA_DIFF_DUMP_ROW=<y> for that row's pixels.
//
// Always exits 0: it is a diagnostic, and the pass/fail judgement is the diff.
#include "native_test_harness.h"
#include "ui/internal.h"
#include "ui/node.h"
#include "ui/tree_internal.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

extern void __gea_top_level();

namespace {

constexpr int kWidth = 410;
constexpr int kHeight = 502;

int envInt(const char *name, int fallback)
{
	const char *raw = std::getenv(name);
	return raw ? std::atoi(raw) : fallback;
}

std::uint64_t hashStart() { return 1469598103934665603ull; }

void hashPixel(std::uint64_t *h, std::uint16_t pixel)
{
	*h = (*h ^ static_cast<std::uint8_t>(pixel & 0xff)) * 1099511628211ull;
	*h = (*h ^ static_cast<std::uint8_t>(pixel >> 8)) * 1099511628211ull;
}

}  // namespace

// Present = no backdrop cache at all, which is exactly what the engine's weak
// default does. That arm is the reference: it never bakes, never reprojects, and
// paints every frame the long way.
extern "C" gea::framework::graphics::pixel::native_t *gea_backdrop_cache(int *cap_px)
{
	static std::vector<gea::framework::graphics::pixel::native_t> buffer(
			static_cast<std::size_t>(kWidth) * kHeight);
	if (std::getenv("GEA_DIFF_NO_BACKDROP"))
	{
		if (cap_px)
			*cap_px = 0;
		return nullptr;
	}
	if (cap_px)
		*cap_px = static_cast<int>(buffer.size());
	return buffer.data();
}

int main()
{
	using namespace gea::embedded::test;
	using namespace gea::embedded::ui;

	resetNativeHost();
	setNativeDisplaySize(kWidth, kHeight);
	__gea_top_level();
	refresh();
	StyleSheet::instance().startCssAnimations(0);
	pumpFrame(0);

	const int frames = envInt("GEA_DIFF_FRAMES", 240);
	const double stepMs = envInt("GEA_DIFF_STEP_MS", 21);
	const int dumpFrame = envInt("GEA_DIFF_DUMP", -1);
	const int dumpRow = envInt("GEA_DIFF_DUMP_ROW", -1);

	std::printf("[diff] backdrop=%s frames=%d step=%.0fms %dx%d\n",
							std::getenv("GEA_DIFF_NO_BACKDROP") ? "OFF(reference)" : "ON(fast path)",
							frames, stepMs, kWidth, kHeight);

	double t = 0.0;
	for (int f = 1; f <= frames; f++)
	{
		t += stepMs;
		pumpFrame(t);

		std::uint64_t frameHash = hashStart();
		for (int y = 0; y < kHeight; y++)
			for (int x = 0; x < kWidth; x++)
				hashPixel(&frameHash, presentedPixelAt(x, y));
		std::printf("F %d %016llx\n", f, static_cast<unsigned long long>(frameHash));

		if (f == dumpFrame)
		{
			if (dumpRow >= 0 && dumpRow < kHeight)
			{
				for (int x = 0; x < kWidth; x++)
					std::printf("PX %d %d %04x\n", x, dumpRow, presentedPixelAt(x, dumpRow));
			}
			else
			{
				for (int y = 0; y < kHeight; y++)
				{
					std::uint64_t rowHash = hashStart();
					for (int x = 0; x < kWidth; x++)
						hashPixel(&rowHash, presentedPixelAt(x, y));
					std::printf("ROW %d %016llx\n", y, static_cast<unsigned long long>(rowHash));
				}
			}
		}
	}
	return 0;
}

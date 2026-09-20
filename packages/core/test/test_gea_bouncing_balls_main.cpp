#include "native_test_harness.h"

#include <cstdio>

extern void __gea_top_level();

int main()
{
	using namespace gea::embedded::test;
	resetNativeHost();
	__gea_top_level();
	refresh();

	if (flushCallCount() <= 0 || flushPixelCount() <= 0) {
		std::fprintf(stderr,
		             "[test_gea_bouncing_balls_main] expected initial display flush, calls=%d pixels=%d\n",
		             flushCallCount(),
		             flushPixelCount());
		return 1;
	}

	const int initialCalls = flushCallCount();
	pumpFrame(16);
	pumpFrame(32);
	if (flushCallCount() <= initialCalls) {
		std::fprintf(stderr,
		             "[test_gea_bouncing_balls_main] expected animation frames to flush display, calls=%d initial=%d\n",
		             flushCallCount(),
		             initialCalls);
		return 1;
	}

	return 0;
}

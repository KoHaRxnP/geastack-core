#include "native_test_harness.h"

#include <cstdio>

extern void __gea_top_level();

int main()
{
	using namespace gea::embedded::test;
	resetNativeHost();
	__gea_top_level();
	refresh();

	const auto text = rootTextContent();
	if (!expectContains(text, "Pixel-faithful browser preview", "root text", "test_gea_static_card_main")) return 1;
	if (!expectContains(text, "Shared C layout and raster output in the browser.", "root text", "test_gea_static_card_main")) return 1;

	return 0;
}

#include "native_test_harness.h"

#include <cstdio>

extern void __gea_top_level();

int main()
{
	using namespace gea::embedded::test;
	resetNativeHost();
	__gea_top_level();
	refresh();

	auto text = rootTextContent();
	if (!expectContains(text, "idle", "initial text", "test_gea_hid_clicker_main")) return 1;
	if (!expectContains(text, "PREV", "initial text", "test_gea_hid_clicker_main")) return 1;
	if (!expectContains(text, "NEXT", "initial text", "test_gea_hid_clicker_main")) return 1;
	if (!expectContains(text, "MOUSE MODE", "initial text", "test_gea_hid_clicker_main")) return 1;

	if (!clickFirstText("MOUSE MODE", true)) return 1;
	refresh();
	text = rootTextContent();
	if (!expectContains(text, "LEFT", "mouse text", "test_gea_hid_clicker_main")) return 1;
	if (!expectContains(text, "SCROLL", "mouse text", "test_gea_hid_clicker_main")) return 1;
	if (!expectContains(text, "RIGHT", "mouse text", "test_gea_hid_clicker_main")) return 1;
	if (!expectContains(text, "PAD", "mouse text", "test_gea_hid_clicker_main")) return 1;

	if (!clickFirstText("PAD", true)) return 1;
	refresh();
	text = rootTextContent();
	if (!expectContains(text, "TRACKPAD", "trackpad text", "test_gea_hid_clicker_main")) return 1;
	if (!expectContains(text, "RIGHT CLICK", "trackpad text", "test_gea_hid_clicker_main")) return 1;

	if (!clickFirstText("BACK", true)) return 1;
	refresh();
	text = rootTextContent();
	if (!expectContains(text, "PREV", "post-back text", "test_gea_hid_clicker_main")) return 1;
	if (!expectContains(text, "MOUSE MODE", "post-back text", "test_gea_hid_clicker_main")) return 1;

	return 0;
}

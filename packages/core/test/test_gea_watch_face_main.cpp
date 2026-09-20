#include "native_test_harness.h"

#include <cstdio>
#include <string>

extern void __gea_top_level();

int main()
{
	using namespace gea::embedded::test;
	resetNativeHost();
	__gea_top_level();
	refresh();

	auto text = rootTextContent();
	if (!expectContains(text, "10:09", "initial text", "test_gea_watch_face_main")) return 1;
	if (!expectContains(text, "MON APR 27", "initial text", "test_gea_watch_face_main")) return 1;
	if (!expectContains(text, "Gea Test", "initial text", "test_gea_watch_face_main")) return 1;
	if (!expectContains(text, "192.0.2.1", "initial text", "test_gea_watch_face_main")) return 1;
	if (!expectContains(text, "Design sync", "initial text", "test_gea_watch_face_main")) return 1;

	pumpFrame(61000);
	// Store writes from the RAF callback queue reactive text patches; production
	// drains those microtasks at the start of the next frame.
	pumpFrame(61016);
	text = rootTextContent();
	if (!expectContains(text, "10:10", "frame text", "test_gea_watch_face_main")) return 1;

	if (!clickFirstText("Gea Test", true)) return 1;
	refresh();
	text = rootTextContent();
	if (!expectContains(text, "Wi-Fi Setup", "settings text", "test_gea_watch_face_main")) return 1;
	if (!expectContains(text, "Enter network name", "settings text", "test_gea_watch_face_main")) return 1;
	if (!expectContains(text, "Back", "settings text", "test_gea_watch_face_main")) return 1;
	if (!expectContains(text, "Save", "settings text", "test_gea_watch_face_main")) return 1;

	if (!clickFirstText("Back", true)) return 1;
	refresh();
	text = rootTextContent();
	if (!expectContains(text, "10:10", "post-back text", "test_gea_watch_face_main")) return 1;
	if (!expectContains(text, "Gea Test", "post-back text", "test_gea_watch_face_main")) return 1;

	return 0;
}

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
	if (!expectContains(text, "Counter", "initial text", "test_gea_counter_main")) return 1;
	if (!expectContains(text, "Ready", "initial text", "test_gea_counter_main")) return 1;
	if (nodesWithClass("counter-plus-button").size() != 1 || nodesWithClass("counter-minus-button").size() != 1 ||
	    nodesWithClass("counter-reset-button").size() != 1) {
		std::fprintf(stderr, "[test_gea_counter_main] expected plus, minus, and reset controls\n");
		dumpTree("test_gea_counter_main");
		return 1;
	}

	if (!pressFirstText("+", true)) return 1;
	refresh();
	text = rootTextContent();
	if (!expectContains(text, "1", "after plus", "test_gea_counter_main")) {
		dumpTree("test_gea_counter_main");
		return 1;
	}
	if (!expectContains(text, "Counting up", "after plus", "test_gea_counter_main")) return 1;

	if (!pressFirstText("Reset", true)) return 1;
	refresh();
	text = rootTextContent();
	if (!expectContains(text, "Reset to zero", "after reset", "test_gea_counter_main")) return 1;

	return 0;
}

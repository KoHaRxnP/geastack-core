// Generic smoke harness for apps compiled via the multi-file geatsc pipeline:
// gea-embedded compat -> vite-plugin-gea -> geatsc -> native retained UI.
// Verifies the app mounts, renders real content, and survives frame pumping.
// Per-app expectations come from the environment:
//   GEA_SMOKE_EXPECT      comma-separated substrings rootTextContent must contain
//   GEA_SMOKE_MIN_NODES   minimum mounted node count (default 10)

#include "native_test_harness.h"

#include "display.h"
#include "ui/document.h"
#include "ui/tree_internal.h"

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unistd.h>

extern void __gea_top_level();

namespace {

void fail_on_alarm(int /*signal*/)
{
	std::fputs("[test_gea_app_smoke_main] timed out\n", stderr);
	std::_Exit(124);
}

}  // namespace

int main()
{
	using namespace gea::embedded::test;
	std::signal(SIGALRM, fail_on_alarm);
	alarm(20);

	resetNativeHost();
	__gea_top_level();
	refresh();

	auto &tree = gea::embedded::ui::Tree::instance();
	long minNodes = 10;
	if (const char *env = std::getenv("GEA_SMOKE_MIN_NODES")) minNodes = std::atol(env);
	if (tree.nodeCount() < minNodes) {
		std::fprintf(stderr, "[test_gea_app_smoke_main] expected at least %ld nodes, got %d\n",
		             minNodes, tree.nodeCount());
		dumpTree("test_gea_app_smoke_main");
		return 1;
	}

	const auto text = rootTextContent();
	if (const char *env = std::getenv("GEA_SMOKE_EXPECT")) {
		std::string expects(env);
		std::size_t pos = 0;
		while (pos <= expects.size()) {
			const std::size_t comma = expects.find(',', pos);
			const std::string token = expects.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos);
			if (!token.empty() && text.find(token) == std::string::npos) {
				std::fprintf(stderr, "[test_gea_app_smoke_main] expected rendered text to contain '%s', got:\n%s\n",
				             token.c_str(), text.c_str());
				dumpTree("test_gea_app_smoke_main");
				return 1;
			}
			if (comma == std::string::npos) break;
			pos = comma + 1;
		}
	}

	// Pump a second of frames — animations, timers, and reactive updates must
	// not crash or assert.
	for (int frame = 0; frame < 60; ++frame) pumpFrame(16 + frame * 16);
	refresh();

	if (tree.nodeCount() <= 0) {
		std::fprintf(stderr, "[test_gea_app_smoke_main] tree emptied after frame pumping\n");
		return 1;
	}

	alarm(0);
	return 0;
}

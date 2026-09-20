#include "native_test_harness.h"

#include "app.h"
#include "host/backends.h"
#include "ui/tree_internal.h"

#include <cstdio>

extern void __gea_top_level();

namespace gea::framework::input {
bool InputBackend::consumeBackButton()
{
	return false;
}
}  // namespace gea::framework::input

namespace {
constexpr int kViewportWidth = 1170;
constexpr int kViewportHeight = 2532;
constexpr int kDevicePixelRatio = 3;
}

int main()
{
	using namespace gea::embedded::test;
	using gea::embedded::ui::Tree;

	resetNativeHost();
	gea::framework::app::Application::init(kViewportWidth, kViewportHeight, kDevicePixelRatio);
	__gea_top_level();
	refresh();
	pumpFrame(16);
	refresh();

	const auto text = rootTextContent();
	if (!expectContains(text, "Settings", "initial text", "test_gea_settings_main")) return 1;
	if (!expectContains(text, "Wi-Fi connection", "initial text", "test_gea_settings_main")) return 1;
	if (!expectContains(text, "Brightness", "initial text", "test_gea_settings_main")) return 1;
	if (!expectContains(text, "Bluetooth name", "initial text", "test_gea_settings_main")) return 1;
	if (text.find("Device settings") != std::string::npos) {
		std::fprintf(stderr, "[test_gea_settings_main] overview header should not render the Device settings subtitle\n");
		dumpTree("test_gea_settings_main");
		return 1;
	}
	if (text.find("Swipe down") != std::string::npos) {
		std::fprintf(stderr, "[test_gea_settings_main] settings app should render open, not prompt for top swipe\n");
		dumpTree("test_gea_settings_main");
		return 1;
	}

	const auto hosts = nodesWithClass("settings-panel-host");
	if (hosts.size() != 1) {
		std::fprintf(stderr, "[test_gea_settings_main] expected one visible settings panel host, got %zu\n", hosts.size());
		dumpTree("test_gea_settings_main");
		return 1;
	}

	const auto screens = nodesWithClass("settings-screen");
	if (screens.empty()) {
		std::fprintf(stderr, "[test_gea_settings_main] expected visible settings screen\n");
		dumpTree("test_gea_settings_main");
		return 1;
	}

	const auto overviewScrolls = nodesWithClass("settings-overview-scroll");
	if (overviewScrolls.size() != 1) {
		std::fprintf(stderr, "[test_gea_settings_main] expected one settings overview scroll, got %zu\n", overviewScrolls.size());
		dumpTree("test_gea_settings_main");
		return 1;
	}
	const auto rowLists = nodesWithClass("settings-row-list");
	if (rowLists.size() != 1) {
		std::fprintf(stderr, "[test_gea_settings_main] expected one settings row list, got %zu\n", rowLists.size());
		dumpTree("test_gea_settings_main");
		return 1;
	}

	auto &tree = Tree::instance();
	const auto &host = tree.node(hosts[0]);
	if (host.layout.width != kViewportWidth || host.layout.height != kViewportHeight) {
		std::fprintf(stderr,
		             "[test_gea_settings_main] expected settings host to fill viewport, got layout=(%d,%d %dx%d)\n",
		             host.layout.x,
		             host.layout.y,
		             host.layout.width,
		             host.layout.height);
		dumpTree("test_gea_settings_main");
		return 1;
	}

	const auto &screen = tree.node(screens[0]);
	const auto &overviewScroll = tree.node(overviewScrolls[0]);
	const auto &rowList = tree.node(rowLists[0]);
	if (overviewScroll.layout.y > 122) {
		std::fprintf(stderr,
		             "[test_gea_settings_main] expected overview header to shrink after removing subtitle, scrollY=%d\n",
		             overviewScroll.layout.y);
		dumpTree("test_gea_settings_main");
		return 1;
	}
	const int scrollBottom = overviewScroll.layout.y + overviewScroll.layout.height;
	const int screenBottom = screen.layout.y + screen.layout.height;
	if (scrollBottom > screenBottom) {
		std::fprintf(stderr,
		             "[test_gea_settings_main] expected overview scroll to fit screen, scrollBottom=%d screenBottom=%d scroll=(%d,%d %dx%d) screen=(%d,%d %dx%d)\n",
		             scrollBottom,
		             screenBottom,
		             overviewScroll.layout.x,
		             overviewScroll.layout.y,
		             overviewScroll.layout.width,
		             overviewScroll.layout.height,
		             screen.layout.x,
		             screen.layout.y,
		             screen.layout.width,
		             screen.layout.height);
		dumpTree("test_gea_settings_main");
		return 1;
	}

	const int leftInset = rowList.layout.x - overviewScroll.layout.x;
	const int rightInset = overviewScroll.layout.x + overviewScroll.layout.width - rowList.layout.x - rowList.layout.width;
	const int insetDelta = leftInset > rightInset ? leftInset - rightInset : rightInset - leftInset;
	if (insetDelta > 1) {
		std::fprintf(stderr,
		             "[test_gea_settings_main] expected row list centered in overview scroll, leftInset=%d rightInset=%d rowList=(%d,%d %dx%d) scroll=(%d,%d %dx%d)\n",
		             leftInset,
		             rightInset,
		             rowList.layout.x,
		             rowList.layout.y,
		             rowList.layout.width,
		             rowList.layout.height,
		             overviewScroll.layout.x,
		             overviewScroll.layout.y,
		             overviewScroll.layout.width,
		             overviewScroll.layout.height);
		dumpTree("test_gea_settings_main");
		return 1;
	}

	return 0;
}

#include "native_test_harness.h"

#include "app.h"
#include "event.h"
#include "events.h"
#include "host/backends.h"
#include "ui/tree_internal.h"

#include <cstdio>
#include <string>

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

bool expectLaunch(const std::string &label, const char *expected)
{
	using namespace gea::embedded::test;
	clearLastLaunchedApp();
	if (!pressFirstText(label, true)) {
		std::fprintf(stderr, "[test_gea_app_launcher_main] failed to press %s\n", label.c_str());
		dumpTree("test_gea_app_launcher_main");
		return false;
	}
	refresh();
	if (lastLaunchedApp() == expected) return true;
	std::fprintf(stderr,
	             "[test_gea_app_launcher_main] expected %s to launch %s, got %s\n",
	             label.c_str(),
	             expected,
	             lastLaunchedApp().c_str());
	dumpTree("test_gea_app_launcher_main");
	return false;
}

}  // namespace

int main()
{
	using namespace gea::embedded::test;
	using gea::embedded::ui::Tree;

	resetNativeHost();
	gea::framework::app::Application::init(kViewportWidth, kViewportHeight, kDevicePixelRatio);
	__gea_top_level();
	refresh();

	const auto text = rootTextContent();
	if (!expectContains(text, "App Launcher", "initial text", "test_gea_app_launcher_main")) return 1;
	if (!expectContains(text, "Balls JSX", "initial text", "test_gea_app_launcher_main")) return 1;
	if (!expectContains(text, "CSS 3D Cube", "initial text", "test_gea_app_launcher_main")) return 1;
	if (!expectContains(text, "Settings", "initial text", "test_gea_app_launcher_main")) return 1;
	if (text.find("Installed apps") != std::string::npos) {
		std::fprintf(stderr, "[test_gea_app_launcher_main] launcher subtitle should be hidden\n");
		dumpTree("test_gea_app_launcher_main");
		return 1;
	}
	if (text.find("animation test") != std::string::npos || text.find("3D CSS demo") != std::string::npos) {
		std::fprintf(stderr, "[test_gea_app_launcher_main] launcher cards should only render app titles\n");
		dumpTree("test_gea_app_launcher_main");
		return 1;
	}
	if (text.find("Tic Tac Toe") != std::string::npos) {
		std::fprintf(stderr, "[test_gea_app_launcher_main] minimal launcher should not include Tic Tac Toe\n");
		dumpTree("test_gea_app_launcher_main");
		return 1;
	}

	const auto launcherCards = nodesWithClass("launcher-card");
	if (launcherCards.size() != 3) {
		std::fprintf(stderr, "[test_gea_app_launcher_main] expected 3 launcher cards\n");
		dumpTree("test_gea_app_launcher_main");
		return 1;
	}
	const auto launcherIcons = nodesWithType(gea::embedded::ui::NodeType::Image);
	if (launcherIcons.size() != 3) {
		std::fprintf(stderr, "[test_gea_app_launcher_main] expected 3 launcher icon images\n");
		dumpTree("test_gea_app_launcher_main");
		return 1;
	}

	const auto launcherLists = nodesWithClass("launcher-list");
	if (launcherLists.empty()) {
		std::fprintf(stderr, "[test_gea_app_launcher_main] expected launcher list node\n");
		dumpTree("test_gea_app_launcher_main");
		return 1;
	}

	const int listNode = launcherLists[0];
	auto &tree = Tree::instance();
	if (!tree.hasAttribute(listNode, "momentum")) {
		std::fprintf(stderr, "[test_gea_app_launcher_main] expected launcher list to request momentum scrolling\n");
		dumpTree("test_gea_app_launcher_main");
		return 1;
	}
	const auto launcherRoots = nodesWithClass("launcher-root");
	if (launcherRoots.empty()) {
		std::fprintf(stderr, "[test_gea_app_launcher_main] expected launcher root node\n");
		dumpTree("test_gea_app_launcher_main");
		return 1;
	}
	const auto &launcherRoot = tree.node(launcherRoots[0]);
	if (launcherRoot.layout.width != kViewportWidth || launcherRoot.layout.height != kViewportHeight) {
		std::fprintf(stderr,
		             "[test_gea_app_launcher_main] expected launcher root to fill %dx%d viewport, got %dx%d\n",
		             kViewportWidth,
		             kViewportHeight,
		             launcherRoot.layout.width,
		             launcherRoot.layout.height);
		dumpTree("test_gea_app_launcher_main");
		return 1;
	}
	const auto headings = nodesWithClass("launcher-heading");
	const auto descriptions = nodesWithClass("launcher-description");
	const auto cardTitles = nodesWithClass("launcher-card-title");
	const auto cardDescriptions = nodesWithClass("launcher-card-description");
	if (headings.empty() || cardTitles.size() != 3 || !descriptions.empty() || !cardDescriptions.empty() ||
	    tree.node(headings[0]).style.font_size != 48 * kDevicePixelRatio ||
	    tree.node(headings[0]).layout.height != 58 * kDevicePixelRatio ||
	    tree.node(cardTitles[0]).style.font_size != 14 * kDevicePixelRatio ||
	    tree.node(cardTitles[0]).style.font_id < 0 ||
	    tree.node(cardTitles[1]).style.font_size != 14 * kDevicePixelRatio ||
	    tree.node(cardTitles[1]).style.font_id < 0 ||
	    tree.node(cardTitles[2]).style.font_size != 14 * kDevicePixelRatio ||
	    tree.node(cardTitles[2]).style.font_id < 0) {
		std::fprintf(stderr,
		             "[test_gea_app_launcher_main] expected launcher title CSS px font sizes to scale by DPR=%d and use generated fonts\n",
		             kDevicePixelRatio);
		dumpTree("test_gea_app_launcher_main");
		return 1;
	}
	const auto &list = tree.node(listNode);
	if (list.layout.width != launcherRoot.layout.width ||
	    list.layout.height != launcherRoot.layout.height - list.layout.y) {
		std::fprintf(stderr,
		             "[test_gea_app_launcher_main] expected launcher list to fill viewport width and remaining height: root=%dx%d list=(%d,%d %dx%d)\n",
		             launcherRoot.layout.width,
		             launcherRoot.layout.height,
		             list.layout.x,
		             list.layout.y,
		             list.layout.width,
		             list.layout.height);
		dumpTree("test_gea_app_launcher_main");
		return 1;
	}
	const auto &firstCard = tree.node(launcherCards[0]);
	const auto &secondCard = tree.node(launcherCards[1]);
	const auto &thirdCard = tree.node(launcherCards[2]);
	for (const int icon : launcherIcons) {
		const auto &iconNode = tree.node(icon);
		if (iconNode.image_id < 0 ||
		    iconNode.layout.width != 64 * kDevicePixelRatio ||
		    iconNode.layout.height != 64 * kDevicePixelRatio ||
		    iconNode.style.border_radius[0] != 12 * kDevicePixelRatio ||
		    iconNode.style.border_radius[1] != 12 * kDevicePixelRatio ||
		    iconNode.style.border_radius[2] != 12 * kDevicePixelRatio ||
		    iconNode.style.border_radius[3] != 12 * kDevicePixelRatio) {
			std::fprintf(stderr,
			             "[test_gea_app_launcher_main] expected square 64px launcher icon node %d with image id and 12px radius, got image=%d layout=%dx%d radius=%d/%d/%d/%d\n",
			             icon,
			             iconNode.image_id,
			             iconNode.layout.width,
			             iconNode.layout.height,
			             iconNode.style.border_radius[0],
			             iconNode.style.border_radius[1],
			             iconNode.style.border_radius[2],
			             iconNode.style.border_radius[3]);
			dumpTree("test_gea_app_launcher_main");
			return 1;
		}
	}
	const auto &firstIcon = tree.node(launcherIcons[0]);
	const std::uint16_t backgroundPixel = displayPixelAt(1, 1);
	const std::uint16_t iconCornerPixel = displayPixelAt(firstIcon.layout.x, firstIcon.layout.y);
	const std::uint16_t iconCenterPixel = displayPixelAt(firstIcon.layout.x + firstIcon.layout.width / 2,
	                                                     firstIcon.layout.y + firstIcon.layout.height / 2);
	if (iconCornerPixel != backgroundPixel || iconCenterPixel == backgroundPixel) {
		std::fprintf(stderr,
		             "[test_gea_app_launcher_main] expected launcher image border-radius to clip icon corners, bg=0x%04x corner=0x%04x center=0x%04x\n",
		             backgroundPixel,
		             iconCornerPixel,
		             iconCenterPixel);
		dumpTree("test_gea_app_launcher_main");
		return 1;
	}
	const int expectedCardWidth = 76 * kDevicePixelRatio;
	const int expectedCardHeight = 92 * kDevicePixelRatio;
	if (firstCard.layout.width != expectedCardWidth ||
	    firstCard.layout.height != expectedCardHeight ||
	    secondCard.layout.width != expectedCardWidth ||
	    secondCard.layout.height != expectedCardHeight ||
	    thirdCard.layout.width != expectedCardWidth ||
	    thirdCard.layout.height != expectedCardHeight ||
	    firstCard.layout.x != list.layout.x + list.style.padding[3] ||
	    secondCard.layout.x != firstCard.layout.x + firstCard.layout.width + list.style.gap ||
	    secondCard.layout.y != firstCard.layout.y ||
	    thirdCard.layout.x != secondCard.layout.x + secondCard.layout.width + list.style.gap ||
	    thirdCard.layout.y != firstCard.layout.y) {
		std::fprintf(stderr,
		             "[test_gea_app_launcher_main] expected three minimal launcher cards in one flex row: list=(%d,%d %dx%d) gap=%d paddingL=%d paddingR=%d first=(%d,%d %dx%d) second=(%d,%d %dx%d) third=(%d,%d %dx%d)\n",
		             list.layout.x,
		             list.layout.y,
		             list.layout.width,
		             list.layout.height,
		             list.style.gap,
		             list.style.padding[3],
		             list.style.padding[1],
		             firstCard.layout.x,
		             firstCard.layout.y,
		             firstCard.layout.width,
		             firstCard.layout.height,
		             secondCard.layout.x,
		             secondCard.layout.y,
		             secondCard.layout.width,
		             secondCard.layout.height,
		             thirdCard.layout.x,
		             thirdCard.layout.y,
		             thirdCard.layout.width,
		             thirdCard.layout.height);
		dumpTree("test_gea_app_launcher_main");
		return 1;
	}
	if (tree.node(listNode).layout.scroll_content_height > tree.node(listNode).layout.height) {
		std::fprintf(stderr,
		             "[test_gea_app_launcher_main] expected minimal launcher list not to overflow, content=%d height=%d\n",
		             tree.node(listNode).layout.scroll_content_height,
		             tree.node(listNode).layout.height);
		dumpTree("test_gea_app_launcher_main");
		return 1;
	}

	if (!expectLaunch("Balls JSX", "bouncing-balls-jsx")) return 1;
	if (!expectLaunch("CSS 3D Cube", "css-3d-cube")) return 1;
	if (!expectLaunch("Settings", "settings")) return 1;

	return 0;
}

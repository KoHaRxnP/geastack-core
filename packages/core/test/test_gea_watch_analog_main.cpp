#include "native_test_harness.h"

#include "ui/document.h"
#include "ui/style.h"
#include "ui/tree_internal.h"

#include <cmath>
#include <cstdio>

extern void __gea_top_level();

namespace {
constexpr const char *kTestName = "test_gea_watch_analog_main";
constexpr int kViewport = 480;

bool expectNear(int actual, int expected, int tolerance, const char *label)
{
	if (std::abs(actual - expected) <= tolerance) return true;
	std::fprintf(stderr, "[%s] expected %s near %d, got %d\n", kTestName, label, expected, actual);
	gea::embedded::test::dumpTree(kTestName);
	return false;
}

bool expectSingleClass(const char *className, int &out)
{
	const auto nodes = gea::embedded::test::nodesWithClass(className);
	if (nodes.size() == 1) {
		out = nodes[0];
		return true;
	}
	std::fprintf(stderr, "[%s] expected one .%s node, got %zu\n", kTestName, className, nodes.size());
	gea::embedded::test::dumpTree(kTestName);
	return false;
}

bool expectHandPivotCentered(const gea::embedded::ui::Node &face,
                             const gea::embedded::ui::Node &hand,
                             const char *label)
{
	const int faceCenterX = face.layout.x + face.layout.width / 2;
	const int faceCenterY = face.layout.y + face.layout.height / 2;
	const int handPivotX = hand.layout.x + hand.layout.width / 2;
	const int handPivotY = hand.layout.y + hand.layout.height;
	return expectNear(handPivotX, faceCenterX, 1, label) &&
	       expectNear(handPivotY, faceCenterY, 1, label);
}
}

int main()
{
	using namespace gea::embedded::test;
	using gea::embedded::ui::Tree;

	resetNativeHost();
	setNativeDisplaySize(kViewport, kViewport);
	gea::embedded::ui::Document::setPreferredMountSize(kViewport, kViewport);
	gea::embedded::ui::setViewportMetrics(kViewport, kViewport, 1.0);
	__gea_top_level();
	refresh();

	auto &tree = Tree::instance();
	int faceId = -1;
	int hourId = -1;
	int minuteId = -1;
	int secondId = -1;
	int capId = -1;
	if (!expectSingleClass("watch-analog-face", faceId) ||
	    !expectSingleClass("watch-analog-hour", hourId) ||
	    !expectSingleClass("watch-analog-minute", minuteId) ||
	    !expectSingleClass("watch-analog-second", secondId) ||
	    !expectSingleClass("watch-analog-cap", capId)) {
		return 1;
	}

	const auto &face = tree.node(faceId);
	const auto &hour = tree.node(hourId);
	const auto &minute = tree.node(minuteId);
	const auto &second = tree.node(secondId);
	const auto &cap = tree.node(capId);
	const int expectedFaceSize = 403;
	const int expectedFaceOffset = (kViewport - expectedFaceSize) / 2;
	if (!expectNear(face.layout.width, expectedFaceSize, 1, "face width") ||
	    !expectNear(face.layout.height, expectedFaceSize, 1, "face height") ||
	    !expectNear(face.layout.x, expectedFaceOffset, 1, "face x") ||
	    !expectNear(face.layout.y, expectedFaceOffset, 1, "face y")) {
		return 1;
	}

	if (!expectHandPivotCentered(face, hour, "hour hand pivot") ||
	    !expectHandPivotCentered(face, minute, "minute hand pivot") ||
	    !expectHandPivotCentered(face, second, "second hand pivot")) {
		return 1;
	}

	const int faceCenterX = face.layout.x + face.layout.width / 2;
	const int faceCenterY = face.layout.y + face.layout.height / 2;
	if (!expectNear(cap.layout.x + cap.layout.width / 2, faceCenterX, 1, "cap center x") ||
	    !expectNear(cap.layout.y + cap.layout.height / 2, faceCenterY, 1, "cap center y")) {
		return 1;
	}

	return 0;
}

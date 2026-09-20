#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "events.h"
#include "ui/node_model.h"

namespace gea::embedded::test {

void resetNativeHost();
void setNativeDisplaySize(int width, int height);
void refresh();
void pumpFrame(double timestampMs);
void setNativeNowMs(int timestampMs);

std::string textContent(int nodeId);
std::string rootTextContent();

std::vector<int> nodesWithClass(const std::string &className);
std::vector<int> nodesWithText(const std::string &text, bool exact = false);
std::vector<int> nodesWithType(gea::embedded::ui::NodeType type);
std::vector<int> nodesWithBox(int width, int height);

int firstNodeWithText(const std::string &text, bool exact = false);
std::size_t countDescendantsWithClass(int nodeId, const std::string &className);

bool dispatchPress(int targetId, int pressId = -1, int pressValue = -1);
bool dispatchClick(int targetId);
void dispatchTouch(gea::framework::events::TouchPhase phase, bool touching, int x, int y);
bool pressFirstText(const std::string &text, bool exact = false);
bool clickFirstText(const std::string &text, bool exact = false);

bool expectContains(const std::string &haystack, const char *needle, const char *label, const char *testName);
void dumpTree(const char *testName = "native_test");

std::string lastLaunchedApp();
void clearLastLaunchedApp();
int imageLoadCount();
int flushCallCount();
int flushRectCount();
int flushPixelCount();
int displayNonzeroPixelCount();
std::uint16_t displayPixelAt(int x, int y);
std::uint16_t presentedPixelAt(int x, int y);

}  // namespace gea::embedded::test

// SPDX-License-Identifier: Apache-2.0
#include "native_test_harness.h"
#include "graphics/font.h"
#include "ui/document.h"
#include "ui/node.h"
#include "ui/style.h"
#include "ui/tree_state.h"
#include "ui/tree_internal.h"
#include <cassert>
#include <cstdio>
namespace gea::framework::app::generated { void drainMicrotasks() {} }
namespace gea::framework::graphics::generated {
void ensureLinked() {}
const RasterizedFontData *lookupFontForFamily(int, int) { return nullptr; }
}
#ifndef GEA_CSS_POSITION_TOP
#define GEA_CSS_POSITION_TOP 1
#define GEA_CSS_POSITION_RIGHT 1
#define GEA_CSS_POSITION_BOTTOM 1
#define GEA_CSS_POSITION_LEFT 1
#define GEA_CSS_POSITION_TOP_PERCENT 1
#define GEA_CSS_POSITION_RIGHT_PERCENT 1
#define GEA_CSS_POSITION_BOTTOM_PERCENT 1
#define GEA_CSS_POSITION_LEFT_PERCENT 1
#endif
using namespace gea::embedded::ui;
using namespace gea::embedded::test;
int main() {
    const char *edges[] = {"top", "right", "bottom", "left"};
    const bool pixels[] = {GEA_CSS_POSITION_TOP, GEA_CSS_POSITION_RIGHT, GEA_CSS_POSITION_BOTTOM, GEA_CSS_POSITION_LEFT};
    const bool percents[] = {GEA_CSS_POSITION_TOP_PERCENT, GEA_CSS_POSITION_RIGHT_PERCENT, GEA_CSS_POSITION_BOTTOM_PERCENT, GEA_CSS_POSITION_LEFT_PERCENT};
    for (int side = 0; side < 4; ++side) for (bool percent : {false, true}) {
        if (!(percent ? percents[side] : pixels[side])) continue;
        for (bool relative : {false, true}) for (bool classRule : {false, true}) {
            resetNativeHost(); StyleSheet::instance().clear();
            setNativeDisplaySize(320, 240);
            setViewportMetrics(320, 240, 1.0);
            auto root = Document::instance().createView();
            root.style().width(200); root.style().height(100);
            auto child = Document::instance().createView();
            child.style().width(30); child.style().height(20);
            child.style().setProperty("position", relative ? "relative" : "absolute");
            root.appendChild(child);
            if (classRule) {
                StyleSheet::instance().registerRule("offset-a", edges[side], percent ? "25%" : "10px");
                StyleSheet::instance().registerRule("offset-b", edges[side], percent ? "-10%" : "-5px");
            }
            for (int frame = 0; frame < 4; ++frame) {
                if (classRule) child.classList().set(frame % 2 ? "offset-b" : "offset-a");
                else child.style().setProperty(edges[side], frame % 2 ? (percent ? "-10%" : "-5px") : (percent ? "25%" : "10px"));
                const int width = frame >= 2 ? 300 : 200, height = frame >= 2 ? 200 : 100;
                root.style().width(width); root.style().height(height);
                Tree::instance().computeLayout(root.id(), width, height);
                const auto &box = Tree::instance().node(child.id()).layout;
                const int basis = side % 2 ? width : height;
                const int offset = percent ? (frame % 2 ? -basis / 10 : basis / 4) : (frame % 2 ? -5 : 10);
                int x = 0, y = 0;
                if (side == 0) y = offset;
                if (side == 1) x = relative ? -offset : width - 30 - offset;
                if (side == 2) y = relative ? -offset : height - 20 - offset;
                if (side == 3) x = offset;
                if (!(box.x == x && box.y == y && box.width == 30 && box.height == 20))
                    std::fprintf(stderr, "position %d-%d-%d-%d-%d: actual %d,%d,%d,%d expected %d,%d,30,20\n", side, percent, relative, classRule, frame, box.x, box.y, box.width, box.height, x, y);
                assert(box.x == x && box.y == y && box.width == 30 && box.height == 20);
                std::printf("%d-%d-%d-%d-%d,%d,%d,%d,%d\n", side, percent, relative, classRule, frame, box.x, box.y, box.width, box.height);
            }
        }
    }
#if GEA_CSS_POSITION_LEFT && GEA_CSS_POSITION_RIGHT
    // Intrinsic absolute widths select either a left or right containing edge.
    for (const char *edge : {"left", "right"}) {
        resetNativeHost(); StyleSheet::instance().clear(); setViewportMetrics(200, 100, 1.0);
        auto root = Document::instance().createView(); root.style().width(200); root.style().height(100);
        auto child = Document::instance().createView(); child.style().setProperty("position", "absolute");
        child.style().setProperty(edge, "10px"); child.style().height(20);
        auto content = Document::instance().createView(); content.style().width(40); content.style().height(20);
        child.appendChild(content); root.appendChild(child); Tree::instance().computeLayout(root.id(), 200, 100);
        assert(Tree::instance().node(child.id()).layout.width == 40);
        const int expected = edge[0] == 'l' ? 10 : 150;
        assert(Tree::instance().node(child.id()).layout.x == expected);
        std::printf("intrinsic-%s,%d,40\n", edge, expected);
    }
#endif
}

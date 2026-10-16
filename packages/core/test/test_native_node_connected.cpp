#include "ui/tree_internal.h"
#include "ui/node.h"
#include <cstdio>
namespace gea::framework::app::generated { void drainMicrotasks() {} }
namespace gea::framework::graphics::generated { void ensureLinked() {} }
static int failures = 0;
static void check(bool ok, const char* label) { if (!ok) { std::printf("FAIL %s\n", label); ++failures; } }
int main() {
    using gea::embedded::ui::NodeHandle;
    auto& tree = gea::embedded::ui::Tree::instance();
    tree.clear();
    NodeHandle prebirth(0);
    int root = tree.createView(), child = tree.createView(), text = tree.createText();
    tree.setParent(child, root); tree.setParent(text, child);
    NodeHandle rootHandle(root), childHandle(child), textHandle(text);
    gea::framework::events::EventTarget eventTarget(child);
    check(!rootHandle.isConnected() && !childHandle.isConnected(), "unmounted nodes disconnected");
    tree.mount(root, 320, 240);
    check(rootHandle.isConnected() && childHandle.isConnected() && textHandle.isConnected(), "mounted root and descendants connected");
    check(NodeHandle(eventTarget).isConnected(), "event target retains allocation in connected projection");
    check(!prebirth.isConnected(), "prebirth handle cannot borrow root allocation");
    int detachedContainer = tree.createView();
    tree.setParent(child, detachedContainer);
    check(!childHandle.isConnected() && !textHandle.isConnected(), "detached live subtree disconnected");
    tree.setParent(child, root);
    check(childHandle.isConnected() && textHandle.isConnected(), "reattached subtree connected");
    tree.removeNode(child);
    int reused = tree.createView();
    check(reused == child, "actual child slot reused");
    tree.setParent(reused, root);
    check(!childHandle.isConnected() && !NodeHandle(eventTarget).isConnected(), "retired child and event leases cannot adopt connected replacement");
    check(NodeHandle(reused).isConnected(), "fresh replacement connected");
    int other = tree.createView();
    tree.mount(other, 320, 240);
    check(!rootHandle.isConnected() && NodeHandle(other).isConnected(), "mount switch disconnects old live tree");
    NodeHandle otherHandle(other);
    tree.removeNode(other);
    check(tree.mountedRoot() == -1, "removing mounted root retires mount authority");
    int replacement = tree.createView();
    check(replacement == other, "actual mounted-root slot reused");
    check(!NodeHandle(replacement).isConnected() && !otherHandle.isConnected(), "replacement root must be explicitly mounted");
    tree.mount(replacement, 320, 240);
    check(NodeHandle(replacement).isConnected() && !otherHandle.isConnected(), "new mount authenticates only current allocation");
    NodeHandle beforeClear(replacement);
    tree.clear();
    int afterClear = tree.createView(); tree.mount(afterClear, 320, 240);
    check(!beforeClear.isConnected() && NodeHandle(afterClear).isConnected(), "clear/reuse preserves current-generation connectivity");
    std::printf("native connectivity failures=%d\n", failures);
    return failures ? 1 : 0;
}

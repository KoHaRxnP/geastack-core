#include "ui/document.h"
#include "ui/tree_internal.h"
#include "ui/internal.h"
#include <algorithm>
#include <cassert>
#include <cstring>
#include <cstdio>
#include <string>

namespace gea::framework::graphics::generated { void ensureLinked() {} }

int main(int argc, char **argv) {
  using namespace gea::embedded::ui;
  auto &tree = Tree::instance();
  auto &document = Document::instance();
  const std::string mode = argc == 2 ? argv[1] : "basic";
  assert(mode == "basic" || mode == "block" || mode == "absolute" || mode == "structural-root" || mode == "relative" || mode == "fixed");
  tree.clear();
  const auto fragment = document.createDocumentFragment();
  assert(fragment.valid());
  std::fprintf(stderr, "actual native fragment nodeType=%d\n", int(fragment.nodeType));
  assert(fragment.nodeType == 11);
  const auto root = document.createElement("div");
  const auto before = document.createElement("span");
  const auto after = document.createElement("span");
  const auto comment = document.createComment("this must never be painted");
  const auto text = document.createText("body");
  assert(comment.valid() && comment.nodeType == 8 && text.nodeType == 3);
  assert(!fragment.parentHandle().valid());
  fragment.appendChild(before);
  fragment.appendChild(comment);
  fragment.appendChild(text);
  assert(before.parentHandle().id() == fragment.id());
  assert(fragment.lastElementChildHandle().id() == before.id());
  root.appendChild(after);
  root.insertBefore(fragment, after);
  assert(fragment.valid() && fragment.nodeType == 11 && !fragment.firstChildHandle().valid());
  assert(!fragment.parentHandle().valid());
  assert(root.childAt(0).id() == before.id() && root.childAt(1).id() == comment.id());
  assert(root.childAt(2).id() == text.id() && root.childAt(3).id() == after.id());
  assert(comment.parentHandle().id() == root.id());
  assert(root.lastElementChildHandle().id() == after.id());
  assert(root.elementChildren().size() == 2);
  assert(root.elementChildren()[0].id() == before.id() && root.elementChildren()[1].id() == after.id());
  assert(!comment.matches("*") && !fragment.matches("*"));
  assert(root.querySelectorAll("*").size() == 2);
  // An empty fragment is a live owner; it inserts nothing and can be reused.
  root.appendChild(fragment);
  const auto extra = document.createElement("div");
  fragment.appendChild(extra);
  root.appendChild(fragment);
  assert(root.lastElementChildHandle().id() == extra.id());
  assert(!fragment.firstChildHandle().valid() && extra.parentHandle().id() == root.id());
  // Inserting a node before itself preserves its existing order.
  root.insertBefore(before, before);
  assert(root.childAt(0).id() == before.id());
  // No partial transfer when the destination is inside the fragment subtree.
  fragment.appendChild(document.createElement("div"));
  const auto descendant = fragment.firstChildHandle();
  descendant.appendChild(fragment);
  assert(fragment.firstChildHandle().id() == descendant.id());
  assert(descendant.parentHandle().id() == fragment.id());
  // Structural nodes cannot acquire children; no input owner is destroyed.
  comment.appendChild(extra);
  text.appendChild(extra);
  assert(extra.parentHandle().id() == root.id());
  const auto inlineText = document.createText("heading");
  inlineText.setTagName("h1");
  const auto inlineChild = document.createElement("span");
  inlineText.appendChild(inlineChild);
  assert(inlineText.nodeType == 1 && inlineChild.parentHandle().id() == inlineText.id());
  root.appendChild(inlineText);
  assert(root.lastElementChildHandle().id() == inlineText.id());
  assert(root.elementChildren().back().id() == inlineText.id());
  const auto copy = fragment.cloneNode(true);
  assert(copy.nodeType == 11 && copy.firstChildHandle().valid());
  assert(copy.firstChildHandle().id() != descendant.id());
  const auto commentCopy = comment.cloneNode();
  assert(commentCopy.nodeType == 8);
  assert(std::strcmp(tree.node(commentCopy.id()).text.c_str(), tree.node(comment.id()).text.c_str()) == 0);
  // Stale handles never become fragment/parent projections of a reused slot.
  const int retired = commentCopy.id();
  commentCopy.remove();
  const auto replacement = document.createElement("div");
  assert(replacement.id() == retired && !commentCopy.valid() && commentCopy.nodeType == 0);
  root.appendChild(replacement);
  assert(!commentCopy.parentHandle().valid() && !commentCopy.lastElementChildHandle().valid());
  // Comment data and even explicit visual styles never create a box or hit target.
  const auto layoutRoot = document.createElement("div");
  const auto left = document.createElement("div");
  const auto right = document.createElement("div");
  const auto hidden = document.createComment("not painted");
  layoutRoot.style().setProperty("display", "flex");
  layoutRoot.style().setProperty("flex-direction", "row");
  layoutRoot.style().setProperty("gap", "10px");
  for (auto element : {left, right}) { element.style().width(20); element.style().height(20); }
  hidden.style().width(200); hidden.style().height(200);
  hidden.style().setProperty("background-color", "red");
  layoutRoot.appendChild(left); layoutRoot.appendChild(hidden); layoutRoot.appendChild(right);
  document.mount(layoutRoot, 300, 100);
  assert(tree.node(right.id()).layout.x - tree.node(left.id()).layout.x == 30);
  int layoutChildren[8]{};
  assert(LayoutEngine::instance().collectChildren(layoutRoot.id(), layoutChildren, 8, false) == 2);
  const auto painting = PaintOrder::collectChildren(layoutRoot.id());
  assert(std::find(painting.begin(), painting.end(), hidden.id()) == painting.end());
  assert(tree.hitTestNode(5, 5) != hidden.id());
  auto &display = DisplayList::instance();
  display.clear(); display.recordNode(hidden.id(), 255);
  assert(display.commandCount() == 0);
  if (mode == "block") {
    const auto block = document.createElement("div");
    const auto first = document.createElement("div");
    const auto last = document.createElement("div");
    const auto marker = document.createComment("no block formatting box");
    block.style().setProperty("display", "block");
    for (auto element : {first, last}) { element.style().width(20); element.style().height(20); }
    marker.style().width(200); marker.style().height(200);
    block.appendChild(first); block.appendChild(marker); block.appendChild(last);
    tree.computeLayout(block.id(), 300, 500);
    std::fprintf(stderr, "actual block firstY=%d firstH=%d lastY=%d commentH=%d\n", tree.node(first.id()).layout.y, tree.node(first.id()).layout.height, tree.node(last.id()).layout.y, tree.node(marker.id()).layout.height);
    assert(tree.node(last.id()).layout.y == tree.node(first.id()).layout.y + tree.node(first.id()).layout.height);
    assert(tree.node(marker.id()).layout.height == 0);
  }
  if (mode == "absolute") {
    const auto block = document.createElement("div");
    const auto marker = document.createComment("no absolute box");
    block.style().width(80); block.style().height(80);
    block.style().setProperty("position", "relative");
    marker.style().setProperty("position", "absolute");
    marker.style().setProperty("left", "400px");
    marker.style().setProperty("top", "300px");
    marker.style().width(100); marker.style().height(100);
    block.appendChild(marker);
    tree.computeLayout(block.id(), 80, 80);
    const auto &box = tree.node(marker.id()).layout;
    std::fprintf(stderr, "actual absolute comment x=%d y=%d w=%d h=%d\n", box.x, box.y, box.width, box.height);
    assert(box.x == 0 && box.y == 0 && box.width == 0 && box.height == 0);
  }
  if (mode == "relative" || mode == "fixed") {
    const auto parent = document.createElement("div");
    const auto marker = document.createComment("no positioned visual semantics");
    marker.style().setProperty("position", mode == "relative" ? "relative" : "fixed");
    marker.style().setProperty("left", "41px"); marker.style().setProperty("top", "37px");
    marker.style().width(100); marker.style().height(100);
    parent.appendChild(marker); tree.computeLayout(parent.id(), 320, 240);
    const auto &box = tree.node(marker.id()).layout;
    std::fprintf(stderr, "actual %s comment x=%d y=%d w=%d h=%d fixed=%d\n", mode.c_str(), box.x, box.y, box.width, box.height, LayoutEngine::containsViewportFixed(parent.id()));
    assert(box.x == 0 && box.y == 0 && box.width == 0 && box.height == 0);
    assert(!LayoutEngine::containsViewportFixed(parent.id()));
  }
  if (mode == "structural-root") {
    for (auto structural : {document.createComment("root"), document.createDocumentFragment()}) {
      structural.style().setProperty("margin", "17px");
      structural.style().width(100); structural.style().height(100);
      tree.computeLayout(structural.id(), 320, 240);
      const auto &box = tree.node(structural.id()).layout;
      std::fprintf(stderr, "actual structural root x=%d y=%d w=%d h=%d\n", box.x, box.y, box.width, box.height);
      assert(box.x == 0 && box.y == 0 && box.width == 0 && box.height == 0);
    }
  }
  tree.clear();
  std::puts("PASS actual native structural fragment/comment insertion, parent, clone, selectors and allocation ownership");
}

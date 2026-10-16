#include "ui/document.h"
#include "ui/tree_internal.h"
#include <cassert>
namespace gea::framework::graphics::generated { void ensureLinked() {} }
extern void __gea_top_level();
int main() {
  using namespace gea::embedded::ui;
  auto &tree = Tree::instance();
  auto &document = Document::instance();
  tree.clear();
  const auto root = document.querySelector("#app");
  assert(root.valid());
  document.mount(root, 320, 240);
  __gea_top_level();
  const auto parent = root.firstChildHandle();
  assert(parent.valid() && parent.nodeType == 1);
  assert(parent.firstChildHandle().nodeType == 8);
  assert(!parent.lastElementChildHandle().valid() && parent.elementChildren().empty());
  tree.clear();
}

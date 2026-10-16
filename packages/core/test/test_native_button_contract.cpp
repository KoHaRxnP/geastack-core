// CPU-only consumer of the actual emitted typed native button fixture.
// No AppKit/SDL/window/input/audio/service/network APIs are imported or called.
#include "ui/document.h"
#include "ui/tree_internal.h"
#include <cassert>
#include <string>
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
  const auto button = root.querySelector("#typed-button-contract");
  assert(button.valid() && button.isConnected());
  assert(tree.node(button.id()).type == NodeType::Button);
  assert(std::string(button.tagName()) == "button");
  assert(!button.disabled() && !button.hasAttribute("disabled"));
  assert(std::string(button.getAttribute("value")) == "saved-button-value");
  assert(root.elementChildren().size() == 1);
  tree.clear();
  assert(!button.valid());
}

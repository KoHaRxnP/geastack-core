// CPU consumer of actual normal CLI output; no renderer/window/OS input/audio.
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
  const auto input = root.querySelector("#typed-input-contract");
  assert(input.valid() && input.isConnected());
  assert(std::string(input.tagName()) == "input");
  assert(!input.disabled() && !input.hasAttribute("disabled"));
  assert(std::string(input.getAttribute("value")) == "");
  assert(root.elementChildren().size() == 1);
  // Native allocation retirement is separate from HTML detachment semantics.
  const int retiredId = input.id();
  input.remove();
  const auto replacement = document.createElement("input");
  assert(replacement.id() == retiredId && replacement.identity() != input.identity());
  replacement.setAttribute("value", "replacement text");
  assert(!input.valid() && input.id() == -1);
  assert(std::string(tree.getAttribute(input.id(), "value")) == "");
  assert(std::string(replacement.getAttribute("value")) == "replacement text");
  tree.clear();
}

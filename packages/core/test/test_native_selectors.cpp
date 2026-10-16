#include "ui/document.h"
#include "ui/node.h"
#include "ui/selector.h"
#include "ui/tree_internal.h"
#include <cstdio>
#include <stdexcept>
namespace gea::framework::app::generated {
void drainMicrotasks() {}
} // namespace gea::framework::app::generated
namespace gea::framework::graphics::generated {
void ensureLinked() {}
} // namespace gea::framework::graphics::generated
using namespace gea::embedded::ui;
static int fails = 0;
static void check(bool b, const char *s) {
  if (!b) {
    ++fails;
    printf("FAIL %s\n", s);
  }
}
int main(int argc, char **argv) {
  auto &t = Tree::instance();
  auto &d = Document::instance();
  t.clear();
  auto root = d.createElement("main"), field = d.createElement("fieldset"),
       legend = d.createElement("legend"), exempt = d.createElement("input"),
       input = d.createElement("input"), div = d.createElement("div");
  root.setAttribute("id", "app");
  root.appendChild(field);
  field.appendChild(legend);
  legend.appendChild(exempt);
  field.appendChild(input);
  root.appendChild(div);
  t.mount(root.id(), 320, 240);
  input.setAttribute("class", "control selected");
  input.setAttribute("data-act", "range-gain");
  input.setAttribute("data-tags", "one two");
  input.setAttribute("data-lang", "en-US");
  input.setAttribute("data-comma", "a,b");
  input.setAttribute("type", "range");
  if (argc == 3 && std::string(argv[1]) == "--query") {
    field.setAttribute("id", "group");
    legend.setAttribute("id", "legend");
    exempt.setAttribute("id", "exempt");
    input.setAttribute("id", "range");
    div.setAttribute("id", "generic");
    field.setDisabled(true);
    div.setDisabled(true);
    for (auto n : d.querySelectorAll(argv[2]))
      printf("%s\n", n.getAttribute("id"));
    return 0;
  }
  check(input.matches("main > fieldset "
                      "input.control.selected[data-act^='range-'][data-act$="
                      "gain][data-act*=ge-g]"),
        "compound child descendant six operators");
  check(input.matches("[data-tags~=two][data-lang|=en][data-comma='a,b']"),
        "token language quoted comma");
  check(input.matches("input:not([type=text], [data-missing])"),
        "nested comma not");
  check(!input.matches("input:not([type=range])"), "not exclusion");
  check(input.closest("fieldset").id() == field.id(), "closest ancestor");
  check(root.querySelectorAll("input").size() == 2, "element descendant query");
  check(d.querySelectorAll("body, main, input").size() == 3,
        "document root projection deduplicated");
  field.setDisabled(true);
  check(input.matches(":disabled") && exempt.matches(":enabled"),
        "fieldset first legend exemption");
  div.setDisabled(true);
  check(!div.matches(":disabled,:enabled") && div.matches("[disabled]"),
        "arbitrary disabled attr is not disabled control");
  field.setDisabled(false);
  input.setAttribute("disabled", "false");
  check(input.matches(":disabled"), "false attribute still presence");
  input.setDisabled(false);
  check(input.matches(":enabled") && !input.hasAttribute("disabled"),
        "false reflected property removes attr");
  input.setParent(div);
  check(input.closest("div").id() == div.id(), "reparent current ancestors");
  auto detached = d.createElement("section");
  div.setParent(detached);
  check(!input.isConnected() && input.closest("div").id() == div.id() &&
            d.querySelectorAll("input").size() == 1,
        "detached closest but document excludes");
  auto stale = input;
  input.remove();
  auto replacement = d.createElement("input");
  check(!stale.matches("input") && !stale.closest("input"),
        "stale handle cannot adopt reused slot");
  for (auto s : {"input,", "input[", "input,,div", "input + div", "input:hover",
                 "input:not()"}) {
    bool refused = false;
    try {
      root.querySelectorAll(s);
    } catch (const std::invalid_argument &) {
      refused = true;
    }
    check(refused, s);
  }
  bool nulRefused = false;
  try {
    input.matches(std::string("input\0bad", 9));
  } catch (const std::invalid_argument &) {
    nulRefused = true;
  }
  check(nulRefused, "embedded NUL cannot match a prefix");
  std::string nested = "input";
  for (int i = 0; i < 10; ++i)
    nested = ":not(" + nested + ")";
  check(selector::parse(nested.c_str()).status() ==
            selector::ParseStatus::limitExceeded,
        "nested syntax bound");
  auto textLeaf = d.createText("visible caption");
  textLeaf.setTagName("p");
  textLeaf.setAttribute("id", "caption");
  textLeaf.setAttribute("class", "status");
  textLeaf.setAttribute("data-role", "feedback");
  root.appendChild(textLeaf);
  auto anonymous = d.createText("anonymous");
  root.appendChild(anonymous);
  auto sentinel = d.createText("");
  sentinel.setTagName("#comment");
  root.appendChild(sentinel);
  check(static_cast<int>(textLeaf.nodeType) == 1 &&
            static_cast<int>(anonymous.nodeType) == 3 &&
            static_cast<int>(sentinel.nodeType) == 8,
        "tagged text leaf DOM kind and actual sentinels");
  check(textLeaf.matches("p#caption.status[data-role=feedback]") &&
            textLeaf.closest("main").id() == root.id(),
        "tagged leaf query closest/id/class/attribute");
  check(root.querySelector("p.status").id() == textLeaf.id() &&
            !anonymous.matches("*") && !sentinel.matches("*"),
        "native leaf admitted anonymous/sentinel excluded");
  auto nativeStepper=d.createElement("stepper");nativeStepper.setDisabled(true);
  check(nativeStepper.matches(":disabled"),"existing native stepper disabled contract");
  nativeStepper.setDisabled(false);check(nativeStepper.matches(":enabled"),"native stepper reenabled");
  std::string huge(4097, 'a');
  check(selector::parse(huge.c_str()).status() ==
            selector::ParseStatus::limitExceeded,
        "input bound");
  printf("native selector failures=%d\n", fails);
  return fails ? 1 : 0;
}

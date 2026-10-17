// Actual TouchRuntime entry used by SDL queued events; authored CPU contacts.
#include "ui/document.h"
#include "ui/tree_internal.h"
#include "ui/tree_events.h"
#include "touch.h"
#include "event.h"
#include <cassert>
#include <cstdio>
#include <string>
#include <vector>
namespace gea::framework::app::generated { void drainMicrotasks() {} }
namespace gea::framework::graphics::generated { void ensureLinked() {} }
using namespace gea::embedded::ui;
using namespace gea::framework::events;
template<class E> int buttonOf(const E &e){if constexpr(requires{e.button;})return e.button;else return -404;}
template<class E> int buttonsOf(const E &e){if constexpr(requires{e.buttons;})return e.buttons;else return -404;}
int main(){
 auto &doc=Document::instance();auto &tree=Tree::instance();tree.clear();
 auto root=doc.createView(),button=doc.createButton();button.style().width(120);button.style().height(40);root.appendChild(button);doc.mount(root,320,240);
 std::vector<std::string> phases;int clicks=0;
 for(const char *name:{"pointerdown","pointermove","pointerup"})tree.setEventListener(button.id(),name,[&](PointerEvent &e){
  const std::string name=e.typeName();phases.push_back(name);
  const int expected=name=="pointermove"?-1:0;
  std::fprintf(stderr,"native phase=%s button=%d buttons=%d\n",name.c_str(),buttonOf(e),buttonsOf(e));
  assert(buttonOf(e)==expected);assert(buttonsOf(e)==(name=="pointerup"?0:1));
 });
 tree.setEventListener(button.id(),"click",[&](PointerEvent &e){++clicks;assert(buttonOf(e)==0&&buttonsOf(e)==0);});
 for(TouchPhase phase:{TouchPhase::Down,TouchPhase::Move,TouchPhase::Up}){Event event{};event.type=EventType::Touch;event.touchPhase=phase;event.touching=phase!=TouchPhase::Up;event.x=phase==TouchPhase::Down?20:21;event.y=20;TouchRuntime::dispatchEvent(event);}
 assert((phases==std::vector<std::string>{"pointerdown","pointermove","pointerup"}));assert(clicks==1);
 PointerEvent unknown{};assert(buttonOf(unknown)==-1&&buttonsOf(unknown)==0);
 tree.clear();std::puts("PASS actual TouchRuntime authored contact button/buttons; no SDL queue, HAL or physical input execution");
}

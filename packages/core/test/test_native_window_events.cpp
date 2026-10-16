#include "ui/document.h"
#include "ui/tree_internal.h"
#include "ui/tree_events.h"
#include <cassert>
#include <stdexcept>
#include <vector>
namespace gea::framework::app::generated { void drainMicrotasks() {} }
namespace gea::framework::graphics::generated { void ensureLinked() {} }
using namespace gea::embedded::ui;
using namespace gea::framework::events;
int main() {
 auto &tree=Tree::instance();tree.clear();
 auto node=Document::instance().createElement("button");
 PointerEvent event{};event.type=PointerEventType::TouchMove;event.targetId=node.id();event.target=EventTarget(node.id());
 std::vector<int> seen;int a,b,c,d;
 const void *occurrence=event.identity();
 assert(addWindowEventListener("pointermove",&a,[&](PointerEvent &e){assert(e.identity()==occurrence);assert(e.eventPhase==EventPhase::Capturing);seen.push_back(1);e.preventDefault();},true));
 assert(addWindowEventListener("pointermove",&a,[&](PointerEvent &){assert(false);},true));
 assert(addWindowEventListener("pointermove",&a,[&](PointerEvent &e){assert(e.defaultPrevented);seen.push_back(2);}));
 assert(dispatchWindowEvent(event,true));assert(dispatchWindowEvent(event,false));assert((seen==std::vector<int>{1,2}));
 assert(removeWindowEventListener("pointermove",&a,true));assert(!removeWindowEventListener("pointermove",&a,true));
 assert(dispatchWindowEvent(event,false));assert(removeWindowEventListener("touchmove",&a,false));
 int once=0;assert(addWindowEventListener("pointermove",&b,[&](PointerEvent &e){++once;dispatchWindowEvent(e,false);},false,true));
 dispatchWindowEvent(event,false);assert(once==1);
 int late=0;assert(addWindowEventListener("pointermove",&c,[&](PointerEvent &){assert(addWindowEventListener("pointermove",&d,[&](PointerEvent &){++late;}));}));
 dispatchWindowEvent(event,false);assert(late==0);dispatchWindowEvent(event,false);assert(late==1);
 resetWindowEventListeners();
 assert(!addWindowEventListener("not-a-real-event",&a,[](PointerEvent &){}));
 assert(addWindowEventListener("blur",&a,[](PointerEvent &){throw std::runtime_error("test");}));
 event.type=PointerEventType::WindowBlur;event.currentTarget=EventTarget(node.id());event.currentTargetId=node.id();event.eventPhase=EventPhase::AtTarget;
 try {dispatchWindowEvent(event,false);assert(false);}catch(const std::runtime_error &){}
 assert(event.currentTarget.id()==node.id()&&event.currentTargetId==node.id()&&event.eventPhase==EventPhase::AtTarget);
 tree.clear();assert(!dispatchWindowEvent(event,false));
 auto root=Document::instance().createElement("main");
 auto button=Document::instance().createElement("button");
 tree.setParent(button.id(),root.id());tree.mount(root.id(),320,240);
 seen.clear();
 addWindowEventListener("keydown",&a,[&](PointerEvent&e){seen.push_back(1);e.preventDefault();},true);
 const auto nodeKey=tree.setEventListener(button.id(),"keydown",[&](PointerEvent&e){assert(e.defaultPrevented);seen.push_back(2);});
 setDocumentEventListener("keydown",[&](PointerEvent&){seen.push_back(3);});
 addWindowEventListener("keydown",&b,[&](PointerEvent&){seen.push_back(4);});
 PointerEvent key{};key.type=PointerEventType::KeyDown;key.targetId=button.id();
 assert(dispatchRootKeyEvent(key));assert((seen==std::vector<int>{1,2,3,4}));
 seen.clear();assert(tree.removeEventListener(button.id(),"keydown",nodeKey));tree.setEventListener(button.id(),"keydown",[&](PointerEvent&e){seen.push_back(2);e.stopPropagation();});
 PointerEvent stopped{};stopped.type=PointerEventType::KeyDown;stopped.targetId=button.id();
 dispatchRootKeyEvent(stopped);assert((seen==std::vector<int>{1,2}));
 resetWindowEventListeners();seen.clear();
 addWindowEventListener("pointerup",&a,[&](PointerEvent&){seen.push_back(1);},true);
 tree.setEventListener(button.id(),"pointerup",[&](PointerEvent&){seen.push_back(2);});
 addWindowEventListener("pointerup",&b,[&](PointerEvent&){seen.push_back(3);});
 PointerEvent release{};release.type=PointerEventType::TouchEnd;release.targetId=button.id();
 tree.dispatchEvent(release);assert((seen==std::vector<int>{1,2,3}));
 tree.clear();
 for (bool keyEvent : {false,true}) {
  auto stableRoot=Document::instance().createElement("main");
  auto victim=Document::instance().createElement("button");
  tree.setParent(victim.id(),stableRoot.id());tree.mount(stableRoot.id(),320,240);
  const int oldId=victim.id();int replacementCalls=0;
  const char*name=keyEvent?"keydown":"pointerup";
  addWindowEventListener(name,&a,[&](PointerEvent&){
   victim.remove();auto replacement=Document::instance().createElement("button");assert(replacement.id()==oldId);
   tree.setParent(replacement.id(),stableRoot.id());tree.setEventListener(replacement.id(),name,[&](PointerEvent&){++replacementCalls;});
  },true,true);
  PointerEvent retired{};retired.type=keyEvent?PointerEventType::KeyDown:PointerEventType::TouchEnd;retired.targetId=oldId;
  if(keyEvent)dispatchRootKeyEvent(retired);else tree.dispatchEvent(retired);
  assert(replacementCalls==0&&!retired.target.valid());tree.clear();
 }
 int lifecycle=0;
 addWindowEventListener("resize",&a,[&](PointerEvent&e){assert(!e.cancelable&&!e.bubbles);++lifecycle;},true,true);
 addWindowEventListener("resize",&b,[&](PointerEvent&e){assert(std::string(e.typeName())=="resize");++lifecycle;},false,true);
 assert(dispatchWindowLifecycleEvent(PointerEventType::WindowResize));assert(lifecycle==2);
 assert(!dispatchWindowLifecycleEvent(PointerEventType::WindowResize));
 assert(!dispatchWindowLifecycleEvent(PointerEventType::TouchEnd));
 tree.clear();
}

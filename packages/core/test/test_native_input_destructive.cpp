// Actual TouchRuntime allocation reuse; no OS input/display/audio.
#include "ui/document.h"
#include "ui/tree_internal.h"
#include "touch.h"
#include "event.h"
#include <cassert>
#include <cstdio>
#include <string>
namespace gea::framework::app::generated {void drainMicrotasks(){}}
namespace gea::framework::graphics::generated {void ensureLinked(){}}
using namespace gea::embedded::ui;
using namespace gea::framework::events;
static void contact(TouchPhase phase,int pointer=0,int x=20,int y=20){Event e{};e.type=EventType::Touch;e.touchPhase=phase;e.touching=phase!=TouchPhase::Up;e.x=x;e.y=y;e.pointerId=pointer;TouchRuntime::dispatchEvent(e);}
int main(int argc,char**argv){assert(argc==2);std::string mode=argv[1];auto&d=Document::instance();auto&t=Tree::instance();t.clear();auto root=d.createView(),button=d.createButton();button.style().width(120);button.style().height(40);root.appendChild(button);d.mount(root,320,240);
 if(mode=="reentrant-down"){
  int downs=0,cancels=0,clicks=0;button.addEventListener("pointercancel",[&](auto&){++cancels;});button.addEventListener("click",[&](auto&){++clicks;});button.addEventListener("pointerdown",[&](PointerEvent&e){if(++downs==1){root.removeChild(button);root.appendChild(button);contact(TouchPhase::Up);contact(TouchPhase::Down);}else e.preventDefault();});contact(TouchPhase::Down);assert(downs==2&&cancels==1);contact(TouchPhase::Up);assert(clicks==0);
 }else if(mode=="cancel-coordinates"){
  int cancelled=0;button.addEventListener("pointercancel",[&](PointerEvent&e){++cancelled;assert(e.clientX==20&&e.clientY==20);});button.addEventListener("pointerdown",[&](auto&){root.removeChild(button);root.appendChild(button);});contact(TouchPhase::Down);assert(cancelled==1);contact(TouchPhase::Up);
 }else{
  assert(mode=="primary-move"||mode=="primary-up"||mode=="secondary-move"||mode=="secondary-up");int pointer=mode.starts_with("secondary")?1:0;int oldDown=0,oldCancel=0;button.addEventListener("pointerdown",[&](auto&){++oldDown;});button.addEventListener("pointercancel",[&](auto&){++oldCancel;});contact(TouchPhase::Down,pointer);assert(oldDown==1);const int id=button.id();button.remove();assert(!button.valid());auto replacement=d.createButton();assert(replacement.id()==id);replacement.style().width(120);replacement.style().height(40);root.appendChild(replacement);d.mount(root,320,240);int moves=0,ups=0,clicks=0,cancels=0,downs=0;replacement.addEventListener("pointermove",[&](auto&){++moves;});replacement.addEventListener("pointerup",[&](auto&){++ups;});replacement.addEventListener("click",[&](auto&){++clicks;});replacement.addEventListener("pointercancel",[&](auto&){++cancels;});replacement.addEventListener("pointerdown",[&](auto&){++downs;});
  if(mode.ends_with("move")){contact(TouchPhase::Move,pointer,21);assert(moves==0&&downs==0);contact(TouchPhase::Move,pointer,22);assert(moves==0&&downs==0);}
  contact(TouchPhase::Up,pointer,22);assert(ups==0&&clicks==0&&cancels==0&&oldCancel==0);contact(TouchPhase::Down,pointer);contact(TouchPhase::Up,pointer);assert(downs==1&&ups==1&&clicks==1&&cancels==0);
 }
 t.clear();puts("PASS actual TouchRuntime destructive owner/coordinate admission");}

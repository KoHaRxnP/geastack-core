#include "ui/document.h"
#include "ui/tree_internal.h"
#include "ui/tree_events.h"
#include <cassert>
#include <memory>
#include <stdexcept>
#include <vector>
namespace gea::framework::app::generated { void drainMicrotasks() {} }
namespace gea::framework::graphics::generated { void ensureLinked() {} }
using namespace gea::embedded::ui;
using namespace gea::framework::events;
int main() {
 auto &tree=Tree::instance();auto &doc=Document::instance();tree.clear();
 auto root=doc.createElement("main"),node=doc.createElement("button");
 tree.setParent(node.id(),root.id());tree.mount(root.id(),320,240);
 int a,b,c,d,e,f;std::vector<int> order;
 addWindowEventListener("pointerup",&a,[&](PointerEvent &){order.push_back(1);},true);
 addDocumentEventListener("pointerup",&b,[&](PointerEvent &ev){assert(!ev.currentTarget.valid());assert(ev.eventPhase==EventPhase::Capturing);order.push_back(2);ev.preventDefault();},true);
 tree.setEventListener(node.id(),"pointerup",[&](PointerEvent &ev){assert(ev.defaultPrevented);order.push_back(3);});
 addDocumentEventListener("pointerup",&c,[&](PointerEvent &ev){assert(ev.eventPhase==EventPhase::Bubbling);order.push_back(4);});
 addWindowEventListener("pointerup",&d,[&](PointerEvent &){order.push_back(5);});
 PointerEvent up{};up.type=PointerEventType::TouchEnd;up.targetId=node.id();const auto occurrence=up.identity();
 assert(tree.dispatchEvent(up));assert(up.identity()==occurrence);assert((order==std::vector<int>{1,2,3,4,5}));
 assert(!removeDocumentEventListener("touchend",&b,true));assert(removeDocumentEventListener("pointerup",&b,true));
 // Callback identity and capture are independent; duplicate once cannot mutate an existing registration.
 int repeated=0;
 assert(addDocumentEventListener("keydown",&a,[&](PointerEvent &){++repeated;}));
 assert(addDocumentEventListener("keydown",&a,[](PointerEvent &){assert(false);},false,true));
 assert(addDocumentEventListener("keydown",&a,[&](PointerEvent &){++repeated;},true,true));
 PointerEvent key{};key.type=PointerEventType::KeyDown;key.targetId=node.id();
 dispatchRootKeyEvent(key);dispatchRootKeyEvent(key);assert(repeated==3);
 assert(removeDocumentEventListener("keydown",&a));
 // Once is removed before a recursive dispatch; removal skips an unvisited callback.
 resetDocumentEventListeners();int once=0,removed=0;
 addDocumentEventListener("keydown",&b,[&](PointerEvent &ev){++once;removeDocumentEventListener("keydown",&c);dispatchDocumentKeyDown(ev);},false,true);
 addDocumentEventListener("keydown",&c,[&](PointerEvent &){++removed;});
 dispatchDocumentKeyDown(key);assert(once==1&&removed==0);
 // stopPropagation affects later ancestors, not other listeners on this same document.
 resetDocumentEventListeners();order.clear();
 doc.addEventListener("keydown",[&](PointerEvent &ev){order.push_back(1);ev.stopPropagation();});
 doc.addEventListener("keydown",[&](PointerEvent &){order.push_back(2);});
 PointerEvent stopped{};stopped.type=PointerEventType::KeyDown;
 assert(dispatchDocumentKeyDown(stopped));assert((order==std::vector<int>{1,2}));
 // Additions in this phase wait for a later dispatch; retained in-flight callbacks survive removal.
 resetDocumentEventListeners();int late=0;
 addDocumentEventListener("keyup",&a,[&](PointerEvent &){addDocumentEventListener("keyup",&b,[&](PointerEvent &){++late;});});
 PointerEvent release{};release.type=PointerEventType::KeyUp;
 dispatchDocumentKeyUp(release);assert(late==0);dispatchDocumentKeyUp(release);assert(late==1);
 // Document capture cancellation fences the target and window bubble.
 resetDocumentEventListeners();resetWindowEventListeners();int calls=0;
 addDocumentEventListener("click",&a,[&](PointerEvent &ev){++calls;ev.stopPropagation();},true);
 tree.setEventListener(node.id(),"click",[](PointerEvent &){assert(false);});
 addWindowEventListener("click",&b,[](PointerEvent &){assert(false);});
 PointerEvent click{};click.type=PointerEventType::Click;click.targetId=node.id();tree.dispatchEvent(click);assert(calls==1);
 // Capture state is restored on exceptions; no body proxy masquerades as document.currentTarget.
 resetDocumentEventListeners();
 addDocumentEventListener("click",&a,[](PointerEvent &){throw std::runtime_error("authored");},true);
 click.currentTarget=EventTarget(node.id());click.currentTargetId=node.id();click.eventPhase=EventPhase::AtTarget;
 try{dispatchDocumentEvent(click,true);assert(false);}catch(const std::runtime_error &){}
 assert(click.currentTarget.id()==node.id()&&click.currentTargetId==node.id()&&click.eventPhase==EventPhase::AtTarget);
 // Realm retirement releases captures; a destructor cannot populate the successor registry.
 resetDocumentEventListeners();
 struct Reenter {int *identity;bool *refused;~Reenter(){*refused=!addDocumentEventListener("click",identity,[](PointerEvent &){});}};
 bool refused=false;auto owner=std::make_shared<Reenter>();owner->identity=&f;owner->refused=&refused;std::weak_ptr<Reenter> weak=owner;
 addDocumentEventListener("click",&e,[owner](PointerEvent &){});owner.reset();tree.clear();assert(weak.expired()&&refused);
 assert(!dispatchDocumentEvent(click,true)&&!dispatchDocumentEvent(click,false));
 assert(!addDocumentEventListener("not-a-native-event",&a,[](PointerEvent &){}));
 // Cross-target capture release cannot register into the new Tree realm.
 struct CrossReenter {int *identity;bool *refused;~CrossReenter(){*refused=!addWindowEventListener("click",identity,[](PointerEvent &){});}};
 bool crossRefused=false;auto cross=std::make_shared<CrossReenter>();cross->identity=&f;cross->refused=&crossRefused;
 addDocumentEventListener("click",&e,[cross](PointerEvent &){});cross.reset();tree.clear();assert(crossRefused);
 // A callback already executing when the realm is cleared cannot repopulate either target.
 bool oldDocumentRefused=false,oldWindowRefused=false;
 addDocumentEventListener("click",&a,[&](PointerEvent &){tree.clear();oldDocumentRefused=!addDocumentEventListener("click",&b,[](PointerEvent &){});oldWindowRefused=!addWindowEventListener("click",&c,[](PointerEvent &){});});
 PointerEvent retiring{};retiring.type=PointerEventType::Click;
 assert(dispatchDocumentEvent(retiring,false));assert(oldDocumentRefused&&oldWindowRefused);
 // New owner-thread work outside the retired callback may install a successor listener.
 assert(addDocumentEventListener("click",&d,[](PointerEvent &){}));tree.clear();

 // Compatibility spellings have independent identity; cancellation never aliases release.
 int cancelled=0,released=0;
 addDocumentEventListener("pointercancel",&a,[&](PointerEvent &ev){assert(std::string(ev.typeName())=="pointercancel");++cancelled;},false,true);
 addDocumentEventListener("pointerup",&b,[&](PointerEvent &){++released;});
 PointerEvent cancel{};cancel.type=PointerEventType::PointerCancel;
 dispatchDocumentEvent(cancel,false);assert(cancelled==1&&released==0);
 assert(!removeDocumentEventListener("touchcancel",&a));
 tree.clear();
 // Bounded registry admission is explicit; an unknown event or null identity never succeeds.
 int identities[257]{};
 for(int i=0;i<256;++i)assert(addDocumentEventListener("click",&identities[i],[](PointerEvent &){}));
 assert(!addDocumentEventListener("click",&identities[256],[](PointerEvent &){}));
 assert(!addDocumentEventListener("click",nullptr,[](PointerEvent &){}));
 tree.clear();

 // Process exit retirement is permanent, idempotent and releases both targets without dispatch.
 auto finalOwner=std::make_shared<int>(42);std::weak_ptr<int> finalWeak=finalOwner;
 addDocumentEventListener("click",&a,[finalOwner](PointerEvent &){assert(false);});
 addWindowEventListener("click",&b,[finalOwner](PointerEvent &){assert(false);});finalOwner.reset();
 retireNativeEventRealm();retireNativeEventRealm();assert(finalWeak.expired());
 assert(documentEventListenersRetiring()&&windowEventListenersRetiring());
 assert(!addDocumentEventListener("click",&c,[](PointerEvent &){}));
 assert(!addWindowEventListener("click",&d,[](PointerEvent &){}));
 assert(!dispatchDocumentEvent(click,false)&&!dispatchWindowEvent(click,false));

}

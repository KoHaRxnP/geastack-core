// Subscriber projection contract: CPU registry fixture, not an OS producer proof.
#include "ui/document.h"
#include "ui/tree_internal.h"
#include "ui/tree_events.h"
#include <cassert>
#include <stdexcept>
#include <string>
#include <vector>
namespace gea::framework::app::generated { void drainMicrotasks() {} }
namespace gea::framework::graphics::generated { void ensureLinked() {} }
using namespace gea::embedded::ui;
using namespace gea::framework::events;
int main() {
 auto &tree=Tree::instance();auto &doc=Document::instance();tree.clear();
 auto root=doc.createElement("main"),button=doc.createElement("button");
 tree.setParent(button.id(),root.id());tree.mount(root.id(),320,240);
 int identity;std::vector<std::string> names;
 auto callback=[&](PointerEvent &event){names.emplace_back(event.typeName());event.preventDefault();};
 assert(addDocumentEventListener("pointerdown",&identity,callback,false,true));
 assert(addDocumentEventListener("touchstart",&identity,callback));
 PointerEvent down{};down.type=PointerEventType::TouchStart;down.targetId=button.id();
 const auto occurrence=down.identity();
 assert(tree.dispatchEvent(down));
 assert((names==std::vector<std::string>{"pointerdown","touchstart"}));
 assert(down.defaultPrevented&&down.identity()==occurrence);
 assert(std::string(down.typeName())=="touchstart");
 names.clear();tree.dispatchEvent(down);
 assert((names==std::vector<std::string>{"touchstart"}));
 assert(!removeDocumentEventListener("pointerdown",&identity));
 assert(removeDocumentEventListener("touchstart",&identity));
 // Exact-name removal cannot remove the other compatibility subscription.
 assert(addWindowEventListener("pointerup",&identity,callback));
 assert(addWindowEventListener("touchend",&identity,callback));
 assert(removeWindowEventListener("touchend",&identity));names.clear();
 PointerEvent up{};up.type=PointerEventType::TouchEnd;up.targetId=button.id();tree.dispatchEvent(up);
 assert((names==std::vector<std::string>{"pointerup"}));
 assert(std::string(up.typeName())=="touchend");
 // Node listeners also retain exact subscription names; removal is not a phase alias.
 resetWindowEventListeners();resetDocumentEventListeners();names.clear();
 const auto pointerNode=tree.setEventListener(button.id(),"pointerdown",callback);
 const auto touchNode=tree.setEventListener(button.id(),"touchstart",callback);
 assert(!tree.removeEventListener(button.id(),"touchstart",pointerNode));
 tree.dispatchEvent(down);
 assert((names==std::vector<std::string>{"pointerdown","touchstart"}));
 assert(tree.removeEventListener(button.id(),"pointerdown",pointerNode));
 assert(tree.removeEventListener(button.id(),"touchstart",touchNode));
 // Nested dispatch of the SAME occurrence restores the outer subscriber spelling.
 int nestedIdentity,depth=0,nestedCalls=0;
 addDocumentEventListener("pointerdown",&identity,[&](PointerEvent &event){
  assert(std::string(event.typeName())=="pointerdown");
  if(depth++==0){dispatchDocumentEvent(event,false);assert(std::string(event.typeName())=="pointerdown");}
  --depth;
 });
 addDocumentEventListener("touchstart",&nestedIdentity,[&](PointerEvent &event){
  assert(std::string(event.typeName())=="touchstart"&&event.identity()==occurrence);++nestedCalls;
 });
 tree.dispatchEvent(down);assert(nestedCalls==2);
 assert(std::string(down.typeName())=="touchstart");
 // A native by-value event retained by a typed callback owns its observable spelling.
 resetWindowEventListeners();resetDocumentEventListeners();
 PointerEvent retained{};
 addDocumentEventListener("pointerdown",&identity,[&](PointerEvent &event){retained=event;});
 addDocumentEventListener("touchstart",&nestedIdentity,[&](PointerEvent &){assert(std::string(retained.typeName())=="pointerdown");});
 tree.dispatchEvent(down);assert(std::string(retained.typeName())=="pointerdown");
 // Projection restores even when a callback throws; no fresh event identity.
 resetWindowEventListeners();resetDocumentEventListeners();
 addDocumentEventListener("pointerdown",&identity,[&](PointerEvent &event){
  assert(std::string(event.typeName())=="pointerdown"&&event.identity()==occurrence);
  throw std::runtime_error("authored projection exception");
 });
 try{tree.dispatchEvent(down);assert(false);}catch(const std::runtime_error &){}
 assert(std::string(down.typeName())=="touchstart");tree.clear();
}

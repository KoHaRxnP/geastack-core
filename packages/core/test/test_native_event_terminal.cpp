#include "ui/document.h"
#include "ui/tree_internal.h"
#include "ui/tree_events.h"
#include <cassert>
#include <memory>
#include <new>
#include <cstdlib>
#include <cstring>
static bool rejectAllocation=false;
static unsigned rejectedAttempts=0;
void *operator new(std::size_t size){if(rejectAllocation){++rejectedAttempts;throw std::bad_alloc();}if(auto *p=std::malloc(size?size:1))return p;throw std::bad_alloc();}
void *operator new[](std::size_t size){return ::operator new(size);}
void operator delete[](void *p) noexcept{std::free(p);}
void operator delete[](void *p,std::size_t) noexcept{std::free(p);}
void operator delete(void *p) noexcept{std::free(p);}
void operator delete(void *p,std::size_t) noexcept{std::free(p);}
namespace gea::framework::app::generated { void drainMicrotasks() {} }
namespace gea::framework::graphics::generated { void ensureLinked() {} }
using namespace gea::embedded::ui;
using namespace gea::framework::events;
struct Owner {int node;bool *refused;bool clear=false;~Owner(){*refused=Tree::instance().setEventListener(node,"click",[](PointerEvent &){})==kInvalidEventListenerId;if(clear)Tree::instance().clear();}};
int main(int argc,char **argv){
 const bool active=argc>1&&std::strcmp(argv[1],"active")==0;
 const bool noalloc=argc>1&&std::strcmp(argv[1],"noalloc")==0;
 const bool reentrant=argc>1&&std::strcmp(argv[1],"reentrant")==0;
 auto &tree=Tree::instance();auto &doc=Document::instance();tree.clear();
 auto root=doc.createElement("main"),button=doc.createElement("button");tree.setParent(button.id(),root.id());tree.mount(root.id(),320,240);
 bool refused=false;std::shared_ptr<Owner> owner;std::weak_ptr<Owner> weak;
 if(!active){owner=std::make_shared<Owner>();owner->node=button.id();owner->refused=&refused;owner->clear=reentrant;weak=owner;tree.setEventListener(button.id(),"click",[owner](PointerEvent &){});owner.reset();}
 if(active){
  owner=std::make_shared<Owner>();owner->node=button.id();owner->refused=&refused;weak=owner;
  int callbacks=0;
  tree.setEventListener(button.id(),"click",[owner,&callbacks](PointerEvent &){++callbacks;retireNativeEventRealm();});owner.reset();
  tree.setEventListener(button.id(),"click",[&](PointerEvent &){++callbacks;});
  PointerEvent active{};active.type=PointerEventType::Click;active.targetId=button.id();
  assert(tree.dispatchEvent(active));assert(callbacks==1&&weak.expired()&&refused);
 }
 rejectAllocation=noalloc;retireNativeEventRealm();retireNativeEventRealm();rejectAllocation=false;assert(rejectedAttempts==0);assert(weak.expired()&&refused);
 assert(tree.setEventListener(button.id(),"click",[](PointerEvent &){})==kInvalidEventListenerId);
 PointerEvent click{};click.type=PointerEventType::Click;click.targetId=button.id();assert(!tree.dispatchEvent(click));
 tree.clear();assert(tree.setEventListener(0,"click",[](PointerEvent &){})==kInvalidEventListenerId);
}

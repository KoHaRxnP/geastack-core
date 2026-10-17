#define GEA_HOST_DECLARED 1
#include "gea/embedded.h"
#ifndef GEA_TEST_RUNTIME_HEADER
#error Actual shared compiler runtime header required
#endif
#include GEA_TEST_RUNTIME_HEADER
#include "host/window.h"
#include "ui/tree_internal.h"
#include <cassert>
#include <cstdio>
#include <string>
#include <memory>
#include <functional>
#include <stdexcept>
#include <vector>

namespace gea::framework::graphics::generated { void ensureLinked() {} }
using namespace gea::framework::events;
using namespace gea::embedded::ui;
struct Owner {
 static inline int live=0;
 int calls=0;
 EventListener callback;
 Owner(){++live;}
 ~Owner(){--live;}
 friend void geaTraceRefs(const Owner& value,gea::detail::RefVisitor& visitor){gea::detail::traceRefs(value.callback,visitor);}
};
struct Capture {
 gea::Ref<Owner> owner;
 friend void geaTraceRefs(const Capture& value,gea::detail::RefVisitor& visitor){gea::detail::traceRefs(value.owner,visitor);}
};
void original(){
 auto& tree=Tree::instance();auto& document=Document::instance();tree.clear();
 auto root=document.querySelector("#app");document.mount(root,320,240);
 auto button=document.createButton();root.appendChild(button);
 auto owner=gea::makeRef<Owner>();
 gea::CallableObject<void()> callback{+[](void* p){++static_cast<Capture*>(p)->owner->calls;},gea::packEnvironment(Capture{owner})};
 owner->callback=explicitEventCallback([](auto& held,auto&){held.call();},callback);
 gea::jsx::detail::addNodeListener(button,"keydown",callback);
 document.addEventListener(std::string("keyup"),callback);
 gea::host::window.addEventListener("blur",callback);
 PointerEvent key;key.type=PointerEventType::KeyDown;key.targetId=button.id();
 dispatchRootKeyEvent(key);
 key.type=PointerEventType::KeyUp;dispatchRootKeyEvent(key);
 dispatchWindowLifecycleEvent(PointerEventType::WindowBlur);
 assert(owner->calls==3);
 document.removeEventListener("keyup",callback);
 gea::host::window.removeEventListener("blur",callback);
 const int id=button.id();button.remove();gea::jsx::detail::releaseNodeSubscriptions(id);
 callback={};owner={};gea::collectCycles();
 assert(Owner::live==0);
 tree.clear();
 std::puts("PASS actual node/document/window compiled callable bridges and retirement");
}

struct RegisteredCapture {
 gea::Ref<Owner> owner;
 int* observed;
 NodeHandle button;
 int registry; // 0=node keys,1=document,2=window,3=delegated body
 bool retireInside;
 friend void geaTraceRefs(const RegisteredCapture& value,gea::detail::RefVisitor& visitor){gea::detail::traceRefs(value.owner,visitor);}
};
static void retire(int registry,NodeHandle button){
 if(registry==1)resetDocumentEventListeners();
 else if(registry==2)resetWindowEventListeners();
 else {const int id=button.id();button.remove();gea::jsx::detail::releaseNodeSubscriptions(id);}
}
// Ordinary native application ownership: the final capture release invokes
// cleanup that can mutate the same real listener/subscription authority.
struct CleanupCapture {
 int* destroyed;
 std::function<void()> cleanup;
 CleanupCapture(int& count,std::function<void()> action):destroyed(&count),cleanup(std::move(action)){}
 ~CleanupCapture(){++*destroyed;cleanup();}
};
static void destructorReentry(const std::string& mode){
 auto& tree=Tree::instance();auto& document=Document::instance();tree.clear();
 auto root=document.querySelector("#app");document.mount(root,320,240);auto button=document.createButton();root.appendChild(button);
 int destroyed=0;
 if(mode=="erase-destructor"){
  int freshCalls=0,trailingCalls=0;
  auto held=std::make_shared<CleanupCapture>(destroyed,[&]{
   // Force the actual native vector to grow during outgoing capture release.
   for(int i=0;i<64;++i)button.addEventListener("click",[&](auto&){++freshCalls;});
  });
  const auto outgoing=button.addEventListener("click",[held](auto&){});
  button.addEventListener("click",[&](auto&){++trailingCalls;});held.reset();assert(destroyed==0);
  button.removeEventListener("click",outgoing);assert(destroyed==1);
  PointerEvent event;event.type=PointerEventType::Click;event.targetId=button.id();tree.dispatchEvent(event);
  assert(freshCalls==64&&trailingCalls==1);
 }else if(mode=="rare-destructor"||mode=="clear-destructor"){
  NodeHandle replacementRoot,replacement;int freshCalls=0;
  auto held=std::make_shared<CleanupCapture>(destroyed,[&]{
   if(mode=="rare-destructor")tree.clear();replacementRoot=document.createView();replacement=document.createButton();replacementRoot.appendChild(replacement);document.mount(replacementRoot,320,240);
   replacement.setAttribute("data-cleanup-successor","alive");replacement.addEventListener("click",[&](auto&){++freshCalls;});
  });
  button.addEventListener("click",[held](auto&){});held.reset();assert(destroyed==0);
  if(mode=="clear-destructor")tree.clear();else button.remove();assert(destroyed==1);assert(replacementRoot.isConnected()&&replacement.isConnected());
  assert(std::string(replacement.getAttribute("data-cleanup-successor"))=="alive");
  PointerEvent event;event.type=PointerEventType::Click;event.targetId=replacement.id();tree.dispatchEvent(event);assert(freshCalls==1);
 }else{
  assert(mode=="subscription-destructor");constexpr int subscription=777;int outgoingCalls=0,freshCalls=0;
  auto held=std::make_shared<CleanupCapture>(destroyed,[&]{
   gea::jsx::detail::releaseNodeSubscriptions(subscription);
   gea::jsx::detail::recordNodeSubscription(subscription,[&]{++freshCalls;});
  });
  gea::jsx::detail::recordNodeSubscription(subscription,[held,&outgoingCalls]()mutable{++outgoingCalls;held.reset();});held.reset();assert(destroyed==0);
  gea::jsx::detail::releaseNodeSubscriptions(subscription);assert(destroyed==1&&outgoingCalls==1&&freshCalls==0);
  gea::jsx::detail::releaseNodeSubscriptions(subscription);assert(freshCalls==1&&outgoingCalls==1);
 }
 tree.clear();std::printf("PASS application capture-destructor reentry: %s\n",mode.c_str());
}
// Generic native release API accepts throwing std::function cleanup. This
// does not claim any currently emitted listener intentionally throws.
static void orderedCleanupThrow(){
 constexpr int node=778;std::vector<int> order;bool firstPreserved=false;
 gea::jsx::detail::recordNodeSubscription(node,[&]{order.push_back(1);gea::jsx::detail::recordNodeSubscription(node,[&]{order.push_back(9);});throw std::runtime_error("authored first cleanup");});
 gea::jsx::detail::recordNodeSubscription(node,[&]{order.push_back(2);throw std::logic_error("authored later cleanup");});
 gea::jsx::detail::recordNodeSubscription(node,[&]{order.push_back(3);});
 try{gea::jsx::detail::releaseNodeSubscriptions(node);}catch(const std::runtime_error& error){firstPreserved=std::string(error.what())=="authored first cleanup";}
 assert(firstPreserved);assert((order==std::vector<int>{1,2,3}));
 gea::jsx::detail::releaseNodeSubscriptions(node);assert((order==std::vector<int>{1,2,3,9}));
 gea::jsx::detail::releaseNodeSubscriptions(node);assert((order==std::vector<int>{1,2,3,9}));
 std::puts("PASS generic ordered cleanup completes and preserves first exception/fresh registration");
}
int main(int argc,char**argv){
 assert(argc==2);const std::string mode=argv[1];
 if(mode=="ordered-cleanup-throw"){orderedCleanupThrow();return 0;}
 if(mode=="erase-destructor"||mode=="rare-destructor"||mode=="subscription-destructor"||mode=="clear-destructor"){destructorReentry(mode);return 0;}
 if(mode=="original"){original();return 0;}
 const bool inside=mode.ends_with("-self");
 const int registry=mode.starts_with("node-")?0:mode.starts_with("document-")?1:mode.starts_with("window-")?2:3;
 assert(mode=="node-root"||mode=="document-root"||mode=="window-root"||mode=="body-root"||mode=="node-self"||mode=="document-self"||mode=="window-self"||mode=="body-self");
 auto& tree=Tree::instance();auto& document=Document::instance();tree.clear();
 auto root=document.querySelector("#app");document.mount(root,320,240);auto button=document.createButton();root.appendChild(button);
 int observed=0;auto owner=gea::makeRef<Owner>();
 gea::CallableObject<void()> callback{+[](void* p){
  auto& capture=*static_cast<RegisteredCapture*>(p);++*capture.observed;
  if(capture.retireInside){retire(capture.registry,capture.button);gea::collectCycles();assert(Owner::live==1);}
 },gea::packEnvironment(RegisteredCapture{owner,&observed,button,registry,inside})};
 owner->callback=explicitEventCallback([](auto& held,auto&){held.call();},callback);
 if(registry==0)gea::jsx::detail::addNodeListener(button,"keydown",callback);
 else if(registry==1)document.addEventListener(std::string("keyup"),callback);
 else if(registry==2)gea::host::window.addEventListener("blur",callback);
 else gea::jsx::detail::addNodeListener(button,"click",callback);
 callback={};owner={};gea::collectCycles();assert(Owner::live==1); // Sole native registry root.
 PointerEvent event;event.targetId=button.id();
 if(registry==2)dispatchWindowLifecycleEvent(PointerEventType::WindowBlur);
 else if(registry==3){event.type=PointerEventType::Click;tree.dispatchEvent(event);}
 else {event.type=registry==0?PointerEventType::KeyDown:PointerEventType::KeyUp;dispatchRootKeyEvent(event);}
 assert(observed==1);
 if(!inside){gea::collectCycles();assert(Owner::live==1);retire(registry,button);}
 gea::collectCycles();assert(Owner::live==0);
 tree.clear();std::printf("PASS registry-only root and dispatch retirement: %s\n",argv[1]);
}

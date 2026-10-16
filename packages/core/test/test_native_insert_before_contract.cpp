// Same normal CLI consumer plus native-only destructive allocation controls.
#include "ui/document.h"
#include "ui/tree_internal.h"
#include <cassert>
#include <stdexcept>
#include <string>
namespace gea::framework::graphics::generated { void ensureLinked() {} }
extern void __gea_top_level();
int main(){
 using namespace gea::embedded::ui;
 auto& tree=Tree::instance();auto& document=Document::instance();tree.clear();
 auto root=document.querySelector("#app");document.mount(root,320,240);__gea_top_level();
 auto parent=root.querySelector("#insert-before-parent"),a=root.querySelector("#insert-a"),b=root.querySelector("#insert-b"),c=root.querySelector("#insert-c"),x=root.querySelector("#insert-x");
 assert(parent.valid()&&a.valid()&&b.valid()&&c.valid()&&x.valid());
 assert(parent.childAt(0).identity()==a.identity()&&parent.childAt(1).identity()==x.identity());
 assert(static_cast<int>(parent.childAt(2).nodeType)==8&&parent.childAt(3).identity()==b.identity()&&parent.childAt(4).identity()==c.identity());
 int clicks=0;b.addEventListener("click",[&](auto&){++clicks;});
 auto retained=parent.removeChild(b);auto returned=parent.insertBefore(retained,a);
 assert(returned.identity()==b.identity()&&b.isConnected()&&std::string(b.getAttribute("data-retained"))=="yes");
 gea::framework::events::PointerEvent event;event.type=gea::framework::events::PointerEventType::Click;event.targetId=b.id();tree.dispatchEvent(event);assert(clicks==1);
 // Raw native retirement differs from browser remove(): never rebind a retired
 // child, reference or receiver to a subsequently reused allocation slot.
 auto stale=document.createElement("span");const int staleId=stale.id();tree.removeNode(staleId);
 auto replacement=document.createElement("b");assert(replacement.id()==staleId);parent.appendChild(replacement);
 const auto unchangedFirst=parent.firstChildHandle().identity();
 assert(!parent.insertBefore(stale,a).valid());assert(replacement.parentHandle().identity()==parent.identity()&&parent.firstChildHandle().identity()==unchangedFirst);
 auto incoming=document.createElement("u");bool refused=false;
 try{parent.insertBefore(incoming,stale);}catch(const std::invalid_argument&){refused=true;}
 assert(refused&&!incoming.parentHandle().valid()&&replacement.parentHandle().identity()==parent.identity());
 auto oldParent=document.createElement("div");const int parentId=oldParent.id();tree.removeNode(parentId);
 auto freshParent=document.createElement("section");assert(freshParent.id()==parentId);root.appendChild(freshParent);
 assert(!oldParent.insertBefore(incoming,NodeHandle()).valid());assert(!incoming.parentHandle().valid()&&!freshParent.firstChildHandle().valid());
 tree.clear();
}

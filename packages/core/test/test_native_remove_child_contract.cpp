// Actual normal CLI output plus native-only allocation-retirement controls.
#include "ui/document.h"
#include "ui/tree_internal.h"
#include <cassert>
#include <stdexcept>
namespace gea::framework::graphics::generated { void ensureLinked() {} }
extern void __gea_top_level();
int main(){
 using namespace gea::embedded::ui;
 auto& tree=Tree::instance();auto& document=Document::instance();tree.clear();
 auto root=document.querySelector("#app");document.mount(root,320,240);
 __gea_top_level();
 auto parent=root.querySelector("#remove-child-parent"),child=root.querySelector("#remove-child-child");
 assert(parent.valid()&&child.valid()&&child.parentHandle().identity()==parent.identity());
 const auto detached=parent.removeChild(child);
 assert(detached.identity()==child.identity()&&child.valid()&&!child.parentHandle().valid()&&!child.isConnected());
 parent.appendChild(detached);assert(child.isConnected());
 // Browser .remove() detaches; native raw Tree removal retires an allocation.
 // These native-only controls must never pretend to be DOM generation rules.
 const int oldChild=child.id();tree.removeNode(oldChild);
 auto replacement=document.createElement("span");assert(replacement.id()==oldChild);
 parent.appendChild(replacement);
 bool staleChild=false;
 try{parent.removeChild(child);}catch(const std::invalid_argument&){staleChild=true;}
 assert(staleChild&&replacement.parentHandle().identity()==parent.identity());
 const int oldParent=parent.id();tree.removeNode(oldParent);
 auto newParent=document.createElement("div");assert(newParent.id()==oldParent);root.appendChild(newParent);
 auto liveChild=document.createElement("span");newParent.appendChild(liveChild);
 bool staleParent=false;
 try{parent.removeChild(liveChild);}catch(const std::invalid_argument&){staleParent=true;}
 assert(staleParent&&liveChild.parentHandle().identity()==newParent.identity());
 tree.clear();
}

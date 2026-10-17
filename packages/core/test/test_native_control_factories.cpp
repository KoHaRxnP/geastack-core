#include "ui/document.h"
#include "ui/node.h"
#include "ui/selector.h"
#include "ui/tree_internal.h"
#include <cstdio>
#include <string>
namespace gea::framework::app::generated {void drainMicrotasks(){}}
namespace gea::framework::graphics::generated {void ensureLinked(){}}
using namespace gea::embedded::ui;
int main(){
 auto &tree=Tree::instance();tree.clear();auto &doc=Document::instance();int failed=0;
 auto check=[&](bool value,const char *label){if(!value){++failed;std::fprintf(stderr,"FAIL %s\n",label);}};
 NodeHandle raw(tree.createNode(NodeType::Button)), shortcut(tree.createButton());
 auto publicButton=doc.createButton();
 for(const auto &button:{raw,shortcut,NodeHandle(publicButton)}){
  check(std::string(tree.tagName(button.id()))=="button","typed native Button has canonical semantic tag");
  check(button.matches("button:enabled"),"typed native Button initially enabled");
  button.setDisabled(true);
  check(button.disabled(),"own disabled reflection retained");
  check(selector::disabled(tree,button.id()),"typed native Button disabled authority");
  check(button.matches("button:disabled"),"typed native Button selector parity");
  button.setDisabled(false);check(button.matches("button:enabled"),"typed native Button reenables");
 }
 auto fieldset=doc.createElement("fieldset");fieldset.setDisabled(true);tree.setParent(raw.id(),fieldset.id());
 check(selector::disabled(tree,raw.id()),"typed native Button inherits disabled fieldset");
 for(const char*tag:{"input","select","stepper","textarea"}){
  auto control=doc.createElement(tag);control.setDisabled(true);
  check(selector::disabled(tree,control.id()),"tagged native control remains disableable");
 }
 auto view=doc.createElement("div");view.setDisabled(true);
 check(!selector::disabled(tree,view.id())&&!view.matches(":disabled,:enabled"),"disabled div not promoted to native control");
 auto generic=NodeHandle(tree.createView());generic.setDisabled(true);
 check(!selector::disabled(tree,generic.id()),"anonymous View not promoted to native control");
 tree.clear();return failed?1:0;
}

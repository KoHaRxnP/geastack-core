#include "ui/document.h"
#include "ui/tree_internal.h"
#include <cassert>
#include <cstdio>
namespace gea::framework::graphics::generated{void ensureLinked(){}}
template<class D>static int active(D&d){if constexpr(requires{d.activeElement();})return d.activeElement().id();else return -404;}
using namespace gea::embedded::ui;
int main(){auto&t=Tree::instance();auto&d=Document::instance();t.clear();auto root=d.createView();auto input=d.createElement("input");auto second=d.createElement("input");root.appendChild(input);root.appendChild(second);d.mount(root,320,240);assert(active(d)==-1);input.focus();assert(active(d)==input.id());second.blur();assert(active(d)==input.id());second.focus();input.blur();assert(active(d)==second.id());second.blur();assert(active(d)==-1);input.setDisabled(true);input.focus();assert(active(d)==-1);input.setDisabled(false);
 auto detached=d.createElement("input");detached.focus();assert(active(d)==-1);input.focus();int old=input.id();input.remove();auto replacement=d.createElement("input");assert(replacement.id()==old);root.appendChild(replacement);assert(active(d)==-1);input.focus();input.blur();assert(active(d)==-1);replacement.focus();assert(active(d)==replacement.id());input.blur();assert(active(d)==replacement.id());t.clear();auto newRoot=d.createView();auto newInput=d.createElement("input");newRoot.appendChild(newInput);d.mount(newRoot,320,240);assert(active(d)==-1);newInput.focus();assert(active(d)==newInput.id());replacement.blur();assert(active(d)==newInput.id());t.clear();puts("PASS actual native Tree text focus/activeElement; connected, disabled, blur ownership and slot/realm reuse; no OS window");}

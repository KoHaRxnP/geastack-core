#include "ui/document.h"
#include "ui/tree_internal.h"
#include <cassert>
#include <cstdio>
namespace gea::framework::graphics::generated {void ensureLinked(){}}
extern void __gea_top_level();
int main(){using namespace gea::embedded::ui;auto&t=Tree::instance();auto&d=Document::instance();t.clear();assert(!d.activeElement().valid());__gea_top_level();auto root=d.createView();auto input=d.createElement("input");input.setAttribute("id","typed-focus-live");root.appendChild(input);d.mount(root,320,240);input.focus();assert(d.activeElement().id()==input.id());__gea_top_level();input.blur();assert(!d.activeElement().valid());t.clear();puts("PASS actual emitted typed Document activeElement getter, empty and genuine mounted focused node; CPU only");}

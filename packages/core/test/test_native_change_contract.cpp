#include "ui/document.h"
#include "ui/tree_internal.h"
#include "ui/control_commit.h"
#include <cassert>
#include <cstring>
#include <cstdio>
namespace gea::framework::graphics::generated {void ensureLinked(){}}
extern void __gea_top_level();
using namespace gea::embedded::ui;using namespace gea::framework::events;
int main(){auto&t=Tree::instance();auto&d=Document::instance();t.clear();__gea_top_level();int id=-1;for(int i=0;i<t.nodeCount();++i){const char*v=t.getAttribute(i,"id");if(v&&std::strcmp(v,"typed-change-control")==0)id=i;}assert(id>=0);auto root=d.body();root.appendChild(NodeHandle(id));d.mount(root,320,240);
 ControlCommit edit;edit.begin(id);t.setAttribute(id,"value","edited");edit.edited(id);PointerEvent input{};input.type=PointerEventType::Input;input.targetId=id;t.dispatchEvent(input);assert(!selector::disabled(t,id));assert(edit.commit());assert(selector::disabled(t,id));assert(!edit.commit());t.clear();puts("PASS typed emitted JSX change at actual commit; input remains intermediate; native CPU API only");}

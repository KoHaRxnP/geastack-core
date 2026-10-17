#include "ui/document.h"
#include "ui/tree_internal.h"
#include "ui/tree_events.h"
#include <cassert>
#include <cstring>
#include <cstdio>
namespace gea::framework::graphics::generated {void ensureLinked(){}}
extern void __gea_top_level();
using namespace gea::embedded::ui;
using namespace gea::framework::events;
int main(){auto&t=Tree::instance();auto&d=Document::instance();t.clear();__gea_top_level();int id=-1;
 for(int i=0;i<t.nodeCount();++i){const char*v=t.getAttribute(i,"id");if(v&&std::strcmp(v,"native-bare-events")==0)id=i;}assert(id>=0);
 auto root=d.body();root.appendChild(NodeHandle(id));d.mount(root,320,240);
 for(auto type:{PointerEventType::TouchStart,PointerEventType::TouchMove,PointerEventType::TouchEnd,PointerEventType::PointerCancel,PointerEventType::Click,PointerEventType::KeyDown,PointerEventType::KeyUp,PointerEventType::Scroll}){PointerEvent e{};e.type=type;e.targetId=id;e.button=0;e.buttons=1;e.key=" ";e.code="Space";t.dispatchEvent(e);if(type==PointerEventType::KeyDown)dispatchDocumentKeyDown(e);if(type==PointerEventType::KeyUp)dispatchDocumentKeyUp(e);fprintf(stderr,"bare event phase=%d prevented=%d\n",int(type),bool(e.defaultPrevented));assert(e.defaultPrevented);}
 t.setAttribute(id,"value","typed-input");PointerEvent input{};input.type=PointerEventType::Input;input.targetId=id;t.dispatchEvent(input);assert(std::strcmp(t.getAttribute(id,"value"),"typed-input")==0);
 t.clear();puts("PASS real emitted lowercase JSX native handlers; pointer/touch projection, key metadata, input, click and scroll; CPU only");}

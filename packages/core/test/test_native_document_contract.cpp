#include "ui/document.h"
#include "ui/tree_internal.h"
#include "ui/tree_events.h"
#include <cassert>
#include <cstdio>
// This API-only fixture declares no generated font asset.
namespace gea::framework::graphics::generated {void ensureLinked(){}}
extern void __gea_top_level();
using namespace gea::embedded::ui;
using namespace gea::framework::events;
int main(){auto &t=Tree::instance();auto &d=Document::instance();t.clear();auto root=d.createView(),button=d.createButton();root.appendChild(button);d.mount(root,320,240);__gea_top_level();
 auto event=[&](PointerEventType type,int changed,std::uint32_t held){PointerEvent e{};e.type=type;e.targetId=button.id();e.clientX=41;e.button=changed;e.buttons=held;t.dispatchEvent(e);return bool(e.defaultPrevented);};
 assert(event(PointerEventType::TouchEnd,0,0));assert(!event(PointerEventType::TouchEnd,0,0));assert(!event(PointerEventType::TouchMove,-1,1));
 assert(event(PointerEventType::TouchStart,0,1));assert(!event(PointerEventType::TouchStart,0,1));t.clear();
 puts("PASS actual emitted typed Document callback/options/removal/once/spelling/buttons; CPU API fixture only");}

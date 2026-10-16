// Exact pressed-owner ABA color boundaries. Platform retirement port is inert observation only.
#include "ui/document.h"
#include "ui/tree_internal.h"
#include "ui/input_retirement.h"
#include "touch.h"
#include "event.h"
#include "pixel.h"
#include <cassert>
#include <cstdio>
namespace gea::framework::app::generated {void drainMicrotasks(){}}
namespace gea::framework::graphics::generated {void ensureLinked(){}}
using namespace gea::embedded::ui;
static NodeHandle replacement;
static void color(const char *phase){auto&t=Tree::instance();fprintf(stderr,"phase=%s valid=%d id=%d bg=%u hasBg=%d connected=%d\n",phase,replacement.valid(),replacement.id(),replacement.valid()?t.node(replacement.id()).computedStyle().bg_color:0,replacement.valid()?t.node(replacement.id()).computedStyle().has_bg:0,replacement.isConnected());}
namespace gea::embedded::ui {void retirePlatformInputForSubtrees(const InputRetirementRoots &,const InputRetirementRoots &){color("after-real-Core-retirement-before-topology");}}
int main(){using namespace gea::framework::events;auto&d=Document::instance();auto&t=Tree::instance();t.clear();auto root=d.createView(),wrapper=d.createView(),button=d.createButton();auto input=d.createElement("input");button.style().width(120);button.style().height(40);button.style().backgroundColor(1234);root.appendChild(wrapper);wrapper.appendChild(button);wrapper.appendChild(input);d.mount(root,320,240);input.focus();
 Event e{};e.type=EventType::Touch;e.touchPhase=TouchPhase::Down;e.touching=true;e.x=20;e.y=20;TouchRuntime::dispatchEvent(e);
 const int oldId=button.id();const auto pressed=t.node(oldId).computedStyle().bg_color;fprintf(stderr,"phase=held-old id=%d bg=%u hasBg=%d oldIdentity=%p\n",oldId,pressed,t.node(oldId).computedStyle().has_bg,button.identity());assert(pressed!=1234);
 input.addEventListener("focusout",[&](auto&){button.remove();replacement=d.createButton();assert(replacement.id()==oldId);replacement.style().backgroundColor(gea::framework::graphics::pixel::toRgb565(pressed));color("replacement-before-insert");wrapper.insertBefore(replacement,input);color("replacement-after-insert");fprintf(stderr,"oldOwnerStillValid=%d replacementIdentity=%p\n",button.valid(),replacement.identity());});
 root.removeChild(wrapper);color("after-detach-style-recompute");assert(replacement.valid()&&t.node(replacement.id()).computedStyle().has_bg&&t.node(replacement.id()).computedStyle().bg_color==pressed);t.clear();}

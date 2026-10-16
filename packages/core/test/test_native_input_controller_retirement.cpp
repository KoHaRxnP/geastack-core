// Real InputController through Tree APIs. No TouchRuntime/OS/SDL input or devices.
#include "native_test_harness.h"
#include "ui/document.h"
#include "ui/tree_internal.h"
#include "pixel.h"
#include <cassert>
#include <cstdio>
#include <string>
namespace gea::framework::app::generated {void drainMicrotasks(){}}
namespace gea::framework::graphics::generated {void ensureLinked(){}}
using namespace gea::embedded::ui;
using namespace gea::embedded::test;
static NodeHandle scroller(NodeHandle root){auto&d=Document::instance();auto s=d.createView(),content=d.createView();s.style().setProperty("width","120px");s.style().setProperty("height","120px");s.style().setProperty("flex-shrink","0");s.style().setProperty("overflow","auto");content.style().setProperty("height","800px");content.style().setProperty("width","120px");content.style().setProperty("flex-shrink","0");s.appendChild(content);root.appendChild(s);s.setAttribute("momentum","");return s;}
int main(int argc,char**argv){assert(argc==2);std::string mode=argv[1];auto&t=Tree::instance();auto&d=Document::instance();t.clear();auto root=d.createView();
 if(mode=="pressed"){
  auto old=d.createButton();old.style().width(120);old.style().height(40);old.style().backgroundColor(1234);root.appendChild(old);d.mount(root,320,240);const auto normal=t.node(old.id()).computedStyle().bg_color;t.pointerDown(20,20);const auto pressed=t.node(old.id()).computedStyle().bg_color;assert(pressed!=normal);const int id=old.id();old.remove();assert(!old.valid());auto replacement=d.createButton();assert(replacement.id()==id);replacement.style().width(120);replacement.style().height(40);replacement.style().backgroundColor(gea::framework::graphics::pixel::toRgb565(pressed));root.appendChild(replacement);d.refresh(root,320,240);assert(t.node(id).computedStyle().has_bg&&t.node(id).computedStyle().bg_color==pressed);t.pointerUp();assert(t.node(id).computedStyle().has_bg&&t.node(id).computedStyle().bg_color==pressed);t.pointerDown(20,20);assert(t.node(id).computedStyle().bg_color!=pressed);t.pointerUp();assert(t.node(id).computedStyle().bg_color==pressed);
 }else{
  assert(mode=="drag"||mode=="momentum"||mode=="adoption");auto old=scroller(root);d.mount(root,320,240);const int id=old.id();assert(t.node(id).layout.height==120&&t.node(id).layout.scroll_content_height>120);setNativeNowMs(1000);t.pointerDown(60,90);setNativeNowMs(1016);assert(t.pointerMove(60,66)==1);setNativeNowMs(1032);assert(t.pointerMove(60,42)==1);const int dragged=t.scrollTop(id);assert(dragged>0);
  if(mode=="momentum"){t.pointerUp();t.tickInput(1048);assert(t.scrollTop(id)>dragged);}
  old.remove();assert(!old.valid());auto replacement=scroller(root);assert(replacement.id()==id);d.refresh(root,320,240);t.setScrollTop(id,137);assert(t.scrollTop(id)==137);setNativeNowMs(1064);
  if(mode=="drag"){assert(t.pointerMove(60,18)==0);assert(t.scrollTop(id)==137);t.pointerUp();}
  if(mode=="adoption")t.pointerUp();
  t.tickInput(1080);t.tickInput(1096);assert(t.scrollTop(id)==137);
  // A fresh real gesture is admitted; the old-owner guard must not poison input.
  setNativeNowMs(1112);t.pointerDown(60,90);setNativeNowMs(1128);assert(t.pointerMove(60,66)==1);assert(t.scrollTop(id)>137);t.pointerUp();
 }
 t.clear();puts("PASS real InputController destructive reuse; active behavior and fresh gesture proved");}

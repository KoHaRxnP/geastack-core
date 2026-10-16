#include "ui/tree_internal.h"
#include "ui/node.h"
#include <cstdio>
#include <cstring>
namespace gea::framework::app::generated { void drainMicrotasks() {} }
namespace gea::framework::graphics::generated { void ensureLinked() {} }
static int failures=0;
static void check(bool ok,const char *name) { if(!ok){std::printf("FAIL %s\n",name);++failures;} }
static void scenario(bool clear) {
 auto &tree=gea::embedded::ui::Tree::instance(); tree.clear();
 const int slot=tree.createView();
 gea::embedded::ui::NodeHandle old(slot);
 gea::framework::events::EventTarget target(slot);
 const auto classes=old.classList(); const auto style=old.style(); const auto type=old.nodeType;
 old.setAttribute("data-owner","old");
 if(clear)tree.clear();else tree.removeNode(slot);
 const int reused=tree.createText(); check(reused==slot,"actual Tree slot reused");
 gea::embedded::ui::NodeHandle current(reused);
 current.setAttribute("data-owner","new"); current.classList().add("current");current.style().width(17);
 check(!old.valid()&&!target.valid(),"retired handles invalid");
 check(old.id()==-1&&target.id()==-1,"retired raw-id projection refused");
 check(std::strcmp(old.getAttribute("data-owner"),"")==0,"retired node attribute read refused");
 check(std::strcmp(target.getAttribute("data-owner"),"")==0,"retired event target read refused");
 old.setAttribute("data-owner","stale");
 check(std::strcmp(current.getAttribute("data-owner"),"new")==0,"retired attribute mutation refused");
 tree.setActiveInput(-1);old.focus();check(tree.activeInputId()==-1,"retired focus refused");
 check(!classes.contains("current")&&classes.value().empty(),"retained ClassList read refused");
 classes.add("stale");classes.remove("current");
 check(current.classList().contains("current")&&!current.classList().contains("stale"),"retained ClassList mutation refused");
 style.width(999);style.setProperty("width","888px");
 check(tree.node(reused).computedStyle().width==17,"retained Style mutation refused");
 check(style.nodeId()==-1,"retained Style id refused");
 check(static_cast<int>(type)==0,"retained NodeTypeProperty refuses replacement");
 // Public focus needs actual connection; stale-owner assertions above stay unchanged.
 const int body=tree.createView();tree.setParent(reused,body);tree.mount(body,100,100);
 current.focus();check(tree.activeInputId()==reused,"live focus positive");
 check(current.valid()&&target.identity()!=current.identity(),"live current generation positive");
 check(current.classList().contains("current"),"live ClassList positive");
 current.style().width(23);check(tree.node(reused).computedStyle().width==23,"live Style positive");
 check(static_cast<int>(current.nodeType)==3,"live NodeTypeProperty positive");
 old.remove();check(std::strcmp(current.getAttribute("data-owner"),"new")==0,"retired remove refuses replacement");
}
int main() {
 auto &tree=gea::embedded::ui::Tree::instance();tree.clear();
 gea::embedded::ui::NodeHandle beforeBirth(0);
 gea::framework::events::EventTarget targetBeforeBirth(0);
 check(!beforeBirth.valid()&&!targetBeforeBirth.valid(),"numeric id alone has no allocation authority");
 check(tree.createView()==0,"first allocation positive");
 check(!beforeBirth.valid()&&!targetBeforeBirth.valid(),"prebirth handles do not adopt a later generation");
 check(gea::embedded::ui::NodeHandle(0).valid(),"fresh live allocation lookup positive");
 scenario(false);scenario(true);std::printf("native generation failures=%d\n",failures);return failures?1:0; }

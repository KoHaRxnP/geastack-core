#include "native_test_harness.h"
#include "graphics/font.h"
#include "ui/document.h"
#include "ui/node.h"
#include "ui/style.h"
#include "ui/tree_internal.h"
#include <cassert>
#include <cstdio>
namespace gea::framework::app::generated { void drainMicrotasks() {} }
namespace gea::framework::graphics::generated { void ensureLinked() {} const RasterizedFontData *lookupFontForFamily(int,int){return nullptr;} }
using namespace gea::embedded::ui;
int main(){
 gea::embedded::test::resetNativeHost();auto &tree=Tree::instance();auto &sheet=StyleSheet::instance();sheet.clear();
 auto root=Document::instance().createView(),child=Document::instance().createView();root.appendChild(child);
 root.style().setProperty("font-stretch","87.5%");tree.mount(root.id(),320,200);tree.computeLayout(root.id(),320,200);
 assert(tree.node(root.id()).style.font_stretch==875);assert(tree.node(child.id()).style.font_stretch==875);
 child.style().setProperty("font-stretch","expanded");tree.computeLayout(root.id(),320,200);assert(tree.node(child.id()).style.font_stretch==1250);
 child.style().setProperty("font-stretch","0%");tree.computeLayout(root.id(),320,200);assert(tree.node(child.id()).style.font_stretch==1250);
 sheet.registerRule("narrow","font-stretch","75%");sheet.registerRule("wide","font-stretch","125%");
 auto styled=Document::instance().createView();root.appendChild(styled);
 for(int i=0;i<8;i++){styled.classList().set(i%2?"wide":"narrow");tree.computeLayout(root.id(),320,200);assert(tree.node(styled.id()).style.font_stretch==(i%2?1250:750));}
 std::printf("font-stretch inline/inheritance/cached-class/invalid pass sizeofComputedStyle=%zu\n",sizeof(ComputedStyle));
}

#include "node.h"
#include <cassert>
int main() {
 using namespace gea::framework;
 events::PointerEvent first{}, fresh{};
 auto copy = first;
 assert(first.identity() == copy.identity());
 assert(first.identity() != fresh.identity());
 identity::activate(42);
 events::EventTarget target(42);
 gea::embedded::ui::NodeHandle node(target), same(42);
 assert(node.identity() == target.identity());
 assert(node.identity() == same.identity());
 identity::retire(42);
 identity::activate(42);
 gea::embedded::ui::NodeHandle replacement(42), oldTarget(target);
 assert(node.identity() != replacement.identity());
 assert(oldTarget.identity() == node.identity());
 identity::clear();
 assert(!gea::embedded::ui::NodeHandle(42).valid());
 identity::activate(42);
 assert(gea::embedded::ui::NodeHandle(42).identity() != replacement.identity());
}

#define GEA_HOST_DECLARED 1
#include "gea/embedded.h"
#include "ui/child_node_list.h"
#include "ui/tree_internal.h"
#include <cassert>
#include <memory>
#include <cmath>
#include <limits>
#include <vector>

namespace gea::framework::app::generated { void drainMicrotasks() {} }
namespace gea::framework::graphics::generated { void ensureLinked() {} }

// Use the real getter and existing tree lifecycle, never a fabricated identity.
int main()
{
  using gea::embedded::ui::NodeHandle;
  auto &tree = gea::embedded::ui::Tree::instance();
  tree.clear();
  NodeHandle parent(tree.createButton()), other(tree.createButton());
  auto list = parent.childNodesHandle();
  assert(list.identity() && list.identity() != parent.identity());
  assert(list.identity() == parent.childNodesHandle().identity());
  assert(list.identity() != other.childNodesHandle().identity());
  auto copied = list;
  assert(copied.identity() == list.identity());
  NodeHandle identityParent(tree.createButton());
  std::weak_ptr<const void> retainedIdentity;
  const void *retainedAddress = nullptr;
  {
    auto temporary = identityParent.childNodesHandle();
    retainedIdentity = temporary.weakIdentityOwner();
    retainedAddress = temporary.identity();
  }
  assert(!retainedIdentity.expired());
  assert(identityParent.childNodesHandle().identity() == retainedAddress);
  assert(list.length() == 0 && !list.item(0) && !list.indexed(0));
  NodeHandle first(tree.createText()), second(tree.createComment()), third(tree.createButton());
  parent.appendChild(first);
  parent.appendChild(second);
  assert(list.length() == 2 && list.item(0).identity() == first.identity());
  assert(list.indexed(1)->identity() == second.identity());
  for (double index : {0.75, -0.75, -0.0, 4294967296.0, std::numeric_limits<double>::quiet_NaN(),
                       std::numeric_limits<double>::infinity(), -std::numeric_limits<double>::infinity()})
    assert(list.item(index).identity() == first.identity());
  for (double index : {1.75, 4294967297.0, -4294967295.0})
    assert(list.item(index).identity() == second.identity());
  assert(!list.item(-1.0));
  assert(!list.item(2) && !list.indexed(2));
  assert(!list.item(UINT32_MAX) && !list.indexed(UINT32_MAX));
  auto iterator = list.values();
  assert(iterator.next()->identity() == first.identity());
  parent.removeChild(first);
  parent.appendChild(third);
  // Live cursor index1 now observes third, rather than cached second.
  assert(iterator.next()->identity() == third.identity());
  assert(!iterator.next());
  parent.appendChild(first);
  assert(!iterator.next()); // Exhaustion is permanent.
  std::vector<NodeHandle> snapshot;
  auto snapshotIterator = list.values();
  while (auto value = snapshotIterator.next()) snapshot.push_back(*value);
  other.appendChild(second);
  assert(list.length() == 2 && other.childNodesHandle().length() == 1);
  assert(snapshot.size() == 3 && snapshot.front().identity() == second.identity());
  NodeHandle fragment(tree.createDocumentFragment()), destination(tree.createButton());
  auto fragmentList = fragment.childNodesHandle();
  fragment.appendChild(NodeHandle(tree.createText()));
  fragment.appendChild(NodeHandle(tree.createComment()));
  auto fragmentFirst = fragmentList.item(0);
  assert(fragmentList.length() == 2);
  destination.appendChild(fragment);
  assert(fragmentList.length() == 0);
  assert(destination.childNodesHandle().length() == 2);
  assert(destination.childNodesHandle().item(0).identity() == fragmentFirst.identity());
  other.appendChild(parent);
  other.removeChild(parent);
  assert(list.length() == 2); // Ordinary detach preserves the collection.
  const int retiredSlot = parent.id();
  tree.removeNode(retiredSlot);
  assert(list.length() == 0 && !list.item(0) && !list.indexed(0));
  auto staleIterator = list.values();
  assert(!staleIterator.next());
  NodeHandle replacement(tree.createButton());
  assert(replacement.id() == retiredSlot); // Exact allocator slot reuse control.
  assert(replacement.identity() != parent.identity());
  assert(replacement.childNodesHandle().identity() != list.identity());
  replacement.appendChild(NodeHandle(tree.createButton()));
  assert(list.length() == 0);
  tree.clear();
  assert(list.length() == 0 && other.childNodesHandle().length() == 0);
  assert(retainedIdentity.expired());
}

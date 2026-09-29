// SPDX-License-Identifier: Apache-2.0
#include "graphics/font.h"
#include "native_test_harness.h"
#include "ui/tree_state.h"
#include <cassert>
#include <cstdio>
#include <string>
#include <utility>
namespace gea::framework::app::generated {
void drainMicrotasks() {}
} // namespace gea::framework::app::generated
namespace gea::framework::graphics::generated {
void ensureLinked() {}
const RasterizedFontData *lookupFontForFamily(int, int) { return nullptr; }
} // namespace gea::framework::graphics::generated
using namespace gea::embedded::ui;
int main() {
  gea::embedded::test::resetNativeHost();
  NodeClassList list;
  assert(list.empty() && list.at(0) == kInvalidCssAtom);
  list.set("one two one\tthree\vfour\x1f"
           "five six seven");
  assert(list.value() == "one two three four five six seven" &&
         list.size() == 7);
  assert(!list.add("two") && !list.add("bad token"));
  auto copy = list;
  assert(list.remove("one") && copy.contains("one"));
  assert(list.remove("three") && list.remove("five"));
  assert(list.value() == "two four six seven");
  assert(list.toggle("four") == false);
  assert(list.toggle("eight") == true);
  assert(list.value() == "two six seven eight");
  NodeClassList moved = std::move(list);
  assert(list.empty() && moved.value() == "two six seven eight");
  list.set("reused");
  list = moved;
  assert(list.value() == moved.value());
  for (const char *name : {"two", "six", "seven", "eight"}) {
    assert(list.remove(name));
    assert(moved.contains(name));
  }
  assert(list.empty());
  // Exercise release/reuse of overflow slots and shifts across every inline
  // boundary, including copying a previously overflowing now-small list.
  for (int round = 0; round < 30; ++round) {
    list.set("a b c d e f g h");
    for (const char *name : {"a", "c", "e", "g", "b", "d"})
      assert(list.remove(name));
    assert(list.value() == "f h");
    copy = list;
    list.clear();
    assert(copy.value() == "f h");
    list = std::move(copy);
    assert(copy.empty() && list.value() == "f h");
    list.set("one two");
    assert(list.at(0) == internCssAtom("one") &&
           list.at(1) == internCssAtom("two"));
  }
  std::printf(
      "PASS: inline=%u bytes=%zu; copy/move/overflow/reuse retain all tokens\n",
      NodeClassList::kInlineTokenCount, sizeof(NodeClassList));
}

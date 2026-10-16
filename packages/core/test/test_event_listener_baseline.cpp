#include <events.h>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <new>

// Intentionally incomplete: ordinary Core callback storage must not require
// the compiler runtime's collector definition or any runtime link symbols.
namespace gea::detail { struct RefVisitor; }
struct TracedSmall {
  int* calls;
  void operator()(gea::framework::events::PointerEvent&) { ++*calls; }
  friend void geaTraceRefs(const TracedSmall&,gea::detail::RefVisitor&) {}
};
static std::size_t allocations = 0;
void* operator new(std::size_t bytes) {
  ++allocations;
  if (void* value = std::malloc(bytes)) return value;
  throw std::bad_alloc();
}
void operator delete(void* value) noexcept { std::free(value); }
void operator delete(void* value, std::size_t) noexcept { std::free(value); }

template<class Callback, class F>
std::size_t constructionAllocations(F callable) {
  const auto before = allocations;
  Callback callback(callable);
  assert(bool(callback));
  return allocations - before;
}
int main() {
  using Event = gea::framework::events::PointerEvent;
  using Listener = gea::framework::events::EventListener;
  using Standard = std::function<void(Event&)>;
  int calls = 0;
  auto small = [&calls](Event&) { ++calls; };
  const auto standard = constructionAllocations<Standard>(small);
  const auto listener = constructionAllocations<Listener>(small);
  assert(listener == standard);
  const auto traced = constructionAllocations<Listener>(TracedSmall{&calls});
  const auto beforeCopy = allocations;
  Listener traceSource(TracedSmall{&calls});
  const auto afterSource = allocations;
  Listener traceCopy(traceSource);
  const auto copyAllocations=allocations-afterSource;
  assert(traced==1 && afterSource-beforeCopy==1 && copyAllocations==1);
  Listener empty;
  bool badCall = false;
  Event event;
  try { empty(event); } catch (const std::bad_function_call&) { badCall = true; }
  assert(badCall);
  Listener original(small), copy(original);
  original = nullptr;
  copy(event);
  assert(calls == 1 && !original && copy);
  Listener moved(std::move(copy));
  moved(event);
  assert(calls == 2);
  struct ThrowingCopy {
    bool* fail;
    ThrowingCopy(bool* flag):fail(flag){}
    ThrowingCopy(const ThrowingCopy& other):fail(other.fail) { if(*fail) throw 7; }
    void operator()(Event&) const {}
  };
  bool fail = false;
  Listener source{ThrowingCopy{&fail}};
  fail = true;
  bool copyFailed = false;
  try { moved = source; } catch(int value) { assert(value==7); copyFailed=true; }
  assert(copyFailed);
  moved(event); // Failed assignment preserves its old callable.
  assert(calls==3);
  std::printf("Core-only incomplete visitor baseline: listener=%zu standard=%zu small allocations=%zu/%zu traced/copy=%zu/%zu\n",
              sizeof(Listener), sizeof(Standard), listener, standard,traced,copyAllocations);
}

#include "gea_runtime.h"
#include <events.h>
#include <cassert>
#include <cstdio>

using Event = gea::framework::events::PointerEvent;
using Listener = gea::framework::events::EventListener;
struct Owner {
  static inline int live = 0;
  Listener callback;
  Owner() { ++live; }
  ~Owner() { --live; }
  friend void geaTraceRefs(const Owner& value, gea::detail::RefVisitor& visitor) {
    gea::detail::traceRefs(value.callback, visitor);
  }
};
struct Capture {
  gea::Ref<Owner> owner;
  friend void geaTraceRefs(const Capture& value, gea::detail::RefVisitor& visitor) {
    gea::detail::traceRefs(value.owner, visitor);
  }
};
// Explicit captures are traceable before type erasure. This deliberately does
// not use a lambda whose physical captures C++ cannot enumerate.
struct Invoke {
  gea::CallableObject<void()> callable;
  void operator()(Event&) { callable.call(); }
  friend void geaTraceRefs(const Invoke& value, gea::detail::RefVisitor& visitor) {
    gea::detail::traceRefs(value.callable, visitor);
  }
};
int main() {
  {
    auto owner = gea::makeRef<Owner>();
    gea::CallableObject<void()> callable{
      +[](void* environment) { assert(static_cast<Capture*>(environment)->owner); },
      gea::packEnvironment(Capture{owner})};
    owner->callback = Invoke{callable};
    auto retained = owner->callback;
    callable = {};
    owner = {};
    gea::collectCycles();
    assert(Owner::live == 1); // A real copied callback is an external root.
    Event event;
    retained(event);
    retained = nullptr;
  }
  gea::collectCycles();
  assert(Owner::live == 0); // Old std::function erasure hides the backedge.
  {
    auto owner = gea::makeRef<Owner>();
    gea::CallableObject<void()> callable{
      +[](void* environment) { assert(static_cast<Capture*>(environment)->owner); },
      gea::packEnvironment(Capture{owner})};
    auto inner=gea::framework::events::explicitEventCallback([](auto& held,Event&){held.call();},callable);
    owner->callback=gea::framework::events::explicitEventCallback([](auto& held,Event& event){held(event);},std::move(inner));
    auto first=owner->callback;
    auto second=first;
    auto moved=std::move(second);
    callable={};owner={};
    first=nullptr;
    gea::collectCycles();
    assert(Owner::live==1);
    Event event;moved(event);
    moved=nullptr;
  }
  gea::collectCycles();
  assert(Owner::live==0); // Nested adapters/copies report physical edges once.
  {
    auto owner=gea::makeRef<Owner>();
    owner->callback=gea::framework::events::explicitEventCallback([](auto& held,Event&){assert(held);},owner);
    owner={};
  }
  gea::collectCycles();
  assert(Owner::live==0); // Native Ref uses existing TraceEdges, not ADL.
  std::puts("PASS actual CallableObject capture through Core listener erasure");
}

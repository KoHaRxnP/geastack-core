#include <events.h>

// Inspect these object symbols with the target toolchain; no target execution
// is needed to measure storage/alignment on an embedded ABI.
extern "C" {
 char gea_event_listener_storage_bytes[sizeof(gea::framework::events::EventListener)];
 char gea_standard_listener_storage_bytes[sizeof(std::function<void(gea::framework::events::PointerEvent&)>)];
 char gea_event_listener_alignment_bytes[alignof(gea::framework::events::EventListener)];
}

// No compiler runtime, complete RefVisitor, RTTI, exceptions, or platform
// source is needed to instantiate ordinary or explicitly traceable storage.
struct Capture {
  int* calls;
  void operator()(gea::framework::events::PointerEvent&) { ++*calls; }
  friend void geaTraceRefs(const Capture&,gea::detail::RefVisitor&) {}
};
int instantiateEventListeners(gea::framework::events::PointerEvent& event) {
  int calls=0;
  gea::framework::events::EventListener ordinary=[&](auto&){++calls;};
  gea::framework::events::EventListener traced=Capture{&calls};
  auto copied=traced;
  ordinary(event);copied(event);
  auto nested=gea::framework::events::explicitEventCallback([](auto& held,auto& e){held(e);},Capture{&calls});
  gea::framework::events::EventListener nestedListener(std::move(nested));
  nestedListener(event);
  return calls;
}

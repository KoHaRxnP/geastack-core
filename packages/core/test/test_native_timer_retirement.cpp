// Main-thread scheduling contract test. Only the unused worker-context port is
// injected; timer/RAF implementations are the actual SDK production source.
#include "host/timers.h"
#include "host/worker.h"
#include <cassert>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <unistd.h>
namespace gea::host::workers {
bool Context::isWorkerCurrent() { return false; }
std::shared_ptr<Context> Context::current() { std::abort(); }
void Context::runMainPending() {}
double Context::schedule(std::function<void()>, double, bool) { std::abort(); }
void Context::cancel(double) { std::abort(); }
}
namespace {
struct Counts { int destroyed=0, fired=0; double timer=-1, frame=-1; };
struct ReentrantOwner {
 Counts* counts;
 ~ReentrantOwner() {
  ++counts->destroyed;
  counts->timer=gea::host::setTimeout([c=counts]{++c->fired;},0);
  counts->frame=gea::host::requestAnimationFrame([c=counts](int){++c->fired;});
 }
};
void timerOwner(Counts& counts) {
 auto owner=std::make_shared<ReentrantOwner>();owner->counts=&counts;
 gea::host::setTimeout([owner]{},0);
}
void frameOwner(Counts& counts) {
 auto owner=std::make_shared<ReentrantOwner>();owner->counts=&counts;
 gea::host::requestAnimationFrame([owner](int){});
}
}
int main(int argc,char**argv) {
 assert(argc==2);alarm(6);const std::string mode=argv[1];using namespace gea::host;
 runAnimationFrameCallbacks(0); // deterministic scheduler clock, independent of uptime
 Counts counts;
 if(mode=="reset") {
  timerOwner(counts);resetScheduledTimers();
  assert(counts.destroyed==1&&counts.timer==0&&counts.frame==0);
  frameOwner(counts);resetAnimationFrameCallbacks();
  assert(counts.destroyed==2&&counts.timer==0&&counts.frame==0);
  assert(setTimeout([&]{++counts.fired;},0)>0);
  runAnimationFrameCallbacks(2000000000);assert(counts.fired==1);
 } else if(mode=="compact") {
  auto owner=std::make_shared<ReentrantOwner>();owner->counts=&counts;
  const auto id=setTimeout([owner]{},0);owner.reset();clearTimeout(id);
  runAnimationFrameCallbacks(2000000000);
  assert(counts.destroyed==1&&counts.timer>0&&counts.frame>0);
  runAnimationFrameCallbacks(2000000001);assert(counts.fired==2);
 } else if(mode=="terminal") {
  timerOwner(counts);frameOwner(counts);retireScheduledCallbacks();retireScheduledCallbacks();
  assert(counts.destroyed==2&&counts.timer==0&&counts.frame==0);
  assert(setTimeout([&]{++counts.fired;},0)==0);
  assert(setInterval([&]{++counts.fired;},1)==0);
  assert(requestAnimationFrame([&](int){++counts.fired;})==0);
  runAnimationFrameCallbacks(2000000000);assert(counts.fired==0);
 } else if(mode=="timer-active") {
  setTimeout([&]{++counts.fired;retireScheduledCallbacks();},0);
  setTimeout([&]{++counts.fired;},0);
  runAnimationFrameCallbacks(2000000000);assert(counts.fired==1);
 } else if(mode=="frame-active") {
  requestAnimationFrame([&](int){++counts.fired;retireScheduledCallbacks();});
  requestAnimationFrame([&](int){++counts.fired;});
  runAnimationFrameCallbacks(2000000000);assert(counts.fired==1);
 } else { std::abort(); }
 std::cout<<"PASS actual SDK scheduling "<<mode<<"; injected unused worker port, no app/audio/input\n";
}

#include "../css/easing.h"
#include <cstdio>
#include <cmath>
using namespace gea::css;
static int fails=0;
static void chk(const char*n,bool c){ if(!c){printf("FAIL %s\n",n);fails++;} }
static double sq(double t){return t*t;}
int main(){
  auto lin=Easing::linear();
  chk("lin0",std::fabs(lin(0)-0)<1e-9); chk("lin1",std::fabs(lin(1)-1)<1e-9); chk("lin.5",std::fabs(lin(0.5)-0.5)<1e-9);
  auto eio=Easing::easeInOut();
  chk("eio0",std::fabs(eio(0))<1e-6); chk("eio1",std::fabs(eio(1)-1)<1e-6); chk("eio.5",std::fabs(eio(0.5)-0.5)<0.02);
  auto ein=Easing::easeIn(); chk("easeIn slow start", ein(0.5)<0.5);
  auto eout=Easing::easeOut(); chk("easeOut fast start", eout(0.5)>0.5);
  auto cb=Easing::cubicBezier(0.25,0.1,0.25,1.0); chk("cb ends",std::fabs(cb(0))<1e-6&&std::fabs(cb(1)-1)<1e-6);
  // monotonic
  double prev=-1; bool mono=true; for(int i=0;i<=100;i++){double v=cb(i/100.0); if(v<prev-1e-6)mono=false; prev=v;} chk("cb monotonic",mono);
  auto st=Easing::steps(4,StepPosition::JumpEnd);
  chk("steps .24",std::fabs(st(0.24)-0.0)<1e-9); chk("steps .26",std::fabs(st(0.26)-0.25)<1e-9); chk("steps .5",std::fabs(st(0.5)-0.5)<1e-9);
  auto cu=Easing::custom(sq); chk("custom .5",std::fabs(cu(0.5)-0.25)<1e-9);
  chk("clamp neg",std::fabs(lin(-1))<1e-9); chk("clamp over",std::fabs(lin(2)-1)<1e-9);
  printf(fails? "EASING TESTS: %d FAIL\n":"EASING TESTS: ALL PASS\n",fails); return fails?1:0;
}

#include "../css/animation.h"
#include "../css/easing.h"
#include "../css/engine.h"
#include "../css/interpolate.h"

#include <cmath>
#include <cstdio>

using namespace gea::css;
using gea::embedded::ui::Property;

static int fails = 0;
static void chk(const char *n, bool c)
{
  if (!c) {
    printf("FAIL %s\n", n);
    fails++;
  }
}

int main()
{
  chk("lerp mid", std::fabs(lerp(0, 10, 0.5) - 5) < 1e-9);
  chk("lerp end", std::fabs(lerp(0, 255, 1) - 255) < 1e-9);

  // RGB565 channel interpolation.
  chk("col 0", lerpColor565(0x0000, 0xFFFF, 0.0) == 0x0000);
  chk("col 1", lerpColor565(0x0000, 0xFFFF, 1.0) == 0xFFFF);
  int mid = lerpColor565(0x0000, 0xFFFF, 0.5);
  int r = (mid >> 11) & 0x1F, g = (mid >> 5) & 0x3F, b = mid & 0x1F;
  chk("col mid r", r >= 15 && r <= 16);
  chk("col mid g", g >= 31 && g <= 32);
  chk("col mid b", b >= 15 && b <= 16);

  // kindOf classification.
  chk("kind bg color", kindOf(Property::BackgroundColor) == ValueKind::Color);
  chk("kind color", kindOf(Property::Color) == ValueKind::Color);
  chk("kind angle", kindOf(Property::TransformRotate) == ValueKind::Angle);
  chk("kind scalar w", kindOf(Property::Width) == ValueKind::Scalar);
  chk("kind scalar op", kindOf(Property::Opacity) == ValueKind::Scalar);

  // Transition track (linear, exact arithmetic).
  auto t = Animation::transition(1, Property::Opacity, 0, 255, 300, Easing::linear());
  chk("samp 0", std::fabs(sampleTrack(t, 0.0) - 0) < 1e-9);
  chk("samp 1", std::fabs(sampleTrack(t, 1.0) - 255) < 1e-9);
  chk("samp .5", std::fabs(sampleTrack(t, 0.5) - 127.5) < 1e-9);

  // Multi-keyframe (linear).
  Animation m;
  m.nodeId = 1;
  m.property = Property::Top;
  m.kind = ValueKind::Scalar;
  m.easing = Easing::linear();
  m.keyframes = {{0.0, 0}, {0.5, 100}, {1.0, 0}};
  chk("mkf .25", std::fabs(sampleTrack(m, 0.25) - 50) < 1e-9);
  chk("mkf .5", std::fabs(sampleTrack(m, 0.5) - 100) < 1e-9);
  chk("mkf .75", std::fabs(sampleTrack(m, 0.75) - 50) < 1e-9);

  // Color track endpoints.
  auto ct = Animation::transition(1, Property::BackgroundColor, 0x0000, 0xFFFF, 300, Easing::linear());
  chk("ct kind", ct.kind == ValueKind::Color);
  chk("ct 0", static_cast<int>(std::llround(sampleTrack(ct, 0.0))) == 0x0000);
  chk("ct 1", static_cast<int>(std::llround(sampleTrack(ct, 1.0))) == 0xFFFF);

  // computeProgress — transition dur 300, iter 1, normal.
  {
    auto a = Animation::transition(1, Property::Opacity, 0, 255, 300, Easing::linear());
    auto p0 = computeProgress(a, 0);
    chk("cp 0", p0.active && !p0.done && std::fabs(p0.p - 0) < 1e-9);
    auto p1 = computeProgress(a, 150);
    chk("cp 150", p1.active && !p1.done && std::fabs(p1.p - 0.5) < 1e-9);
    auto p2 = computeProgress(a, 300);
    chk("cp 300 done", p2.done && std::fabs(p2.p - 1.0) < 1e-9);
    chk("cp 400 done", computeProgress(a, 400).done);
  }
  // Delay 100.
  {
    auto a = Animation::transition(1, Property::Opacity, 0, 255, 300, Easing::linear(), 100);
    auto pre = computeProgress(a, 50);
    chk("cp delay pre inactive", !pre.active && !pre.done);
    auto post = computeProgress(a, 150);
    chk("cp delay post", post.active && std::fabs(post.p - (50.0 / 300.0)) < 1e-6);
  }
  // Reverse.
  {
    Animation a = Animation::transition(1, Property::Opacity, 0, 255, 100, Easing::linear());
    a.direction = Direction::Reverse;
    chk("rev start p=1", std::fabs(computeProgress(a, 0).p - 1.0) < 1e-9);
    chk("rev mid p=.5", std::fabs(computeProgress(a, 50).p - 0.5) < 1e-9);
    auto d = computeProgress(a, 100);
    chk("rev done p=0", d.done && std::fabs(d.p - 0.0) < 1e-9);
  }
  // Alternate, 2 iterations.
  {
    Animation a = Animation::transition(1, Property::Opacity, 0, 255, 100, Easing::linear());
    a.direction = Direction::Alternate;
    a.iterations = 2;
    chk("alt iter0 .5", std::fabs(computeProgress(a, 50).p - 0.5) < 1e-9);
    chk("alt iter1 rev .5", std::fabs(computeProgress(a, 150).p - 0.5) < 1e-9);
    auto d = computeProgress(a, 250);
    chk("alt done p=0", d.done && std::fabs(d.p - 0.0) < 1e-9);
  }

  // AnimationEngine active-list storage: stays correct across inline and spill tiers.
  {
    AnimationEngine &engine = AnimationEngine::instance();
    engine.clear();
    int handles[GEA_CSS_ANIMATION_INLINE_RUNNING + 2] = {};
    for (int i = 0; i < GEA_CSS_ANIMATION_INLINE_RUNNING + 2; ++i) {
      handles[i] = engine.start(Animation::transition(i, Property::Opacity, 0, 255, 300, Easing::linear()), 0);
    }
    chk("engine spill count", engine.count() == GEA_CSS_ANIMATION_INLINE_RUNNING + 2);
    engine.cancelNode(3);
    chk("engine cancel node", engine.count() == GEA_CSS_ANIMATION_INLINE_RUNNING + 1);
    engine.cancel(handles[0]);
    chk("engine cancel handle", engine.count() == GEA_CSS_ANIMATION_INLINE_RUNNING);
    engine.clear();
    chk("engine clear", engine.count() == 0 && !engine.active());
  }

  printf(fails ? "CSS ANIM TESTS: %d FAIL\n" : "CSS ANIM TESTS: ALL PASS\n", fails);
  return fails ? 1 : 0;
}

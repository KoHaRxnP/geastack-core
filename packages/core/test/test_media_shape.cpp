#include "host/media.h"

#include <cassert>
#include <cstdio>
#include <string>

int main() {
  auto sh = gea::host::media::create_stream();
  gea::host::MediaStream stream(sh);

  auto th = gea::host::media::stream_audio_track(sh);
  gea::host::MediaStreamTrack track(th);

  assert(track.kind() == "audio");
  assert(track.readyState() == "live");
  assert(track.enabled());

  track.stop();
  assert(track.readyState() == "ended");

  gea::host::media::destroy_stream(sh);
  std::puts("media shape OK");
  return 0;
}

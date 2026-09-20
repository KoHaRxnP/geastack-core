#include "host/media.h"

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <vector>

int main() {
  auto sh = gea::host::media::create_stream();
  gea::host::MediaStream stream(sh);

  auto tracks = stream.getAudioTracks();
  assert(tracks.size() == 1);
  auto track = tracks[0];
  assert(track.kind() == "audio");
  assert(track.enabled());

  std::vector<std::int16_t> input(320);
  for (std::size_t i = 0; i < input.size(); ++i) input[i] = static_cast<std::int16_t>(i);
  gea::host::media::track_inject_pcm(track.nativeHandle, input.data(), input.size());

  std::vector<std::int16_t> output(320);
  auto read = gea::host::media::track_read_pcm(track.nativeHandle, output.data(), output.size());
  assert(read == 320);
  for (std::size_t i = 0; i < 320; ++i) assert(output[i] == static_cast<std::int16_t>(i));

  auto read_again = gea::host::media::track_read_pcm(track.nativeHandle, output.data(), output.size());
  assert(read_again == 0);

  track.stop();
  assert(track.readyState() == "ended");

  gea::host::media::destroy_stream(sh);

  std::puts("media lifecycle OK");
  return 0;
}

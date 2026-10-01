// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>

namespace gea::platform::audio {

// The RTC decoder supplies 16 kHz mono PCM. Pulling from independent readers
// avoids copying every incoming packet into another playback queue.
class PcmStreamMixer {
 public:
  using Pull = std::function<std::size_t(std::int16_t*, std::size_t)>;
  using Handle = std::uint64_t;
  static constexpr std::size_t chunkFrames = 512;

  Handle add(Pull pull) {
    if (!pull) return 0;
    std::lock_guard<std::mutex> lock(mutex_);
    const auto id = ++next_;
    sources_.emplace(id, Source{std::move(pull)});
    return id;
  }
  void remove(Handle id) { std::lock_guard<std::mutex> lock(mutex_); sources_.erase(id); }
  void clear() { std::lock_guard<std::mutex> lock(mutex_); sources_.clear(); }
  bool active() const { std::lock_guard<std::mutex> lock(mutex_); return !sources_.empty(); }
  bool settled(Handle id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = sources_.find(id);
    return it == sources_.end() || (!it->second.inFlight && !it->second.failed);
  }
  // Called by the sole output consumer AFTER its driver write returns. A
  // reader being empty only proves its last block was pulled, not submitted.
  void didWrite(bool success) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& [id, source] : sources_) {
      if (source.inFlight && !success) source.failed = true;
      source.inFlight = false;
    }
  }

  // Mix into existing stereo samples (e.g. oscillator output). Return the
  // frames actually read: a short network packet must not acquire silence
  // padding that lengthens speech and accumulates latency.
  std::size_t mix(std::int16_t* stereo, std::size_t frames) {
    frames = std::min(frames, chunkFrames);
    std::array<std::int16_t, chunkFrames> mono;
    std::size_t available = 0;
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& [id, source] : sources_) {
      const auto count = std::min(frames, source.pull(mono.data(), frames));
      source.inFlight = source.inFlight || count > 0;
      available = std::max(available, count);
      for (std::size_t i = 0; i < count; ++i) {
        for (std::size_t channel = 0; channel < 2; ++channel) {
          const auto sum = static_cast<int>(stereo[i * 2 + channel]) + mono[i];
          stereo[i * 2 + channel] = static_cast<std::int16_t>(std::clamp(sum, -32768, 32767));
        }
      }
    }
    return available;
  }
 private:
  mutable std::mutex mutex_;
  struct Source { Pull pull; bool inFlight = false; bool failed = false; };
  std::map<Handle, Source> sources_;
  Handle next_ = 0;
};
}

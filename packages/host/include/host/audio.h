// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <audio.h>
#include "backends.h"

#include <cstdint>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace gea::embedded::ui {
class NodeHandle;
}

namespace gea::host {

using NativeAudioHandle = gea::platform::audio::NativeAudioHandle;

namespace detail {
inline double oscillator_type_from_name(const char *type) {
  if (type == nullptr) return 0.0;
  if (std::strcmp(type, "square") == 0) return 1.0;
  if (std::strcmp(type, "sawtooth") == 0) return 2.0;
  if (std::strcmp(type, "triangle") == 0) return 3.0;
  return 0.0;
}
}  // namespace detail

struct AudioDestinationNode {
  NativeAudioHandle nativeHandle = 0;

  constexpr AudioDestinationNode() = default;
  explicit constexpr AudioDestinationNode(NativeAudioHandle destination) : nativeHandle(destination) {}
  explicit constexpr AudioDestinationNode(double destination) : nativeHandle(static_cast<NativeAudioHandle>(destination)) {}

  constexpr operator double() const { return static_cast<double>(nativeHandle); }
};

struct AudioDestinationProperty {
  operator AudioDestinationNode() const;
  operator double() const;
};

struct AudioContextCurrentTimeProperty {
  operator double() const;
};

struct AudioBuffer {
  std::vector<std::int16_t> samples;
  int sampleRate = 16000;
  int channels = 1;

  constexpr AudioBuffer() = default;
  AudioBuffer(std::vector<std::int16_t> pcm, int rate, int channelCount)
      : samples(std::move(pcm)), sampleRate(rate), channels(channelCount) {}
};

struct AudioParamValueProperty {
  NativeAudioHandle oscillatorHandle = 0;

  constexpr AudioParamValueProperty() = default;
  explicit constexpr AudioParamValueProperty(NativeAudioHandle handle) : oscillatorHandle(handle) {}
  explicit constexpr AudioParamValueProperty(double handle) : oscillatorHandle(static_cast<NativeAudioHandle>(handle)) {}

  const AudioParamValueProperty &operator=(double frequency_hz) const;
  operator double() const;
};

struct AudioParam {
  NativeAudioHandle oscillatorHandle = 0;
  mutable AudioParamValueProperty value;

  constexpr AudioParam() = default;
  explicit constexpr AudioParam(NativeAudioHandle handle) : oscillatorHandle(handle), value(handle) {}
  explicit constexpr AudioParam(double handle) : oscillatorHandle(static_cast<NativeAudioHandle>(handle)), value(handle) {}

  const AudioParam &operator=(double frequency_hz) const;
  void setValueAtTime(double frequency_hz, double start_time) const;
};

struct OscillatorTypeProperty {
  NativeAudioHandle oscillatorHandle = 0;

  constexpr OscillatorTypeProperty() = default;
  explicit constexpr OscillatorTypeProperty(NativeAudioHandle handle) : oscillatorHandle(handle) {}
  explicit constexpr OscillatorTypeProperty(double handle) : oscillatorHandle(static_cast<NativeAudioHandle>(handle)) {}

  const OscillatorTypeProperty &operator=(double type) const;
  const OscillatorTypeProperty &operator=(const char *type) const;
  const OscillatorTypeProperty &operator=(const std::string &type) const;
};

struct OscillatorNode {
  NativeAudioHandle nativeHandle = 0;
  mutable AudioParam frequency;
  mutable OscillatorTypeProperty type;

  constexpr OscillatorNode() = default;
  explicit constexpr OscillatorNode(NativeAudioHandle oscillator) : nativeHandle(oscillator), frequency(oscillator), type(oscillator) {}
  explicit constexpr OscillatorNode(double oscillator) : nativeHandle(static_cast<NativeAudioHandle>(oscillator)), frequency(oscillator), type(oscillator) {}

  constexpr operator double() const { return static_cast<double>(nativeHandle); }

  void connect(AudioDestinationNode destination) const;
  void connect(AudioDestinationProperty destination) const;
  void connect(double destinationHandle) const;
  void start(double when = 0.0) const;
  void stop(double when = 0.0) const;
};

struct AudioBufferSourceNode {
  mutable AudioBuffer buffer;
  mutable bool connected = false;

  AudioDestinationNode connect(AudioDestinationNode destination) const;
  AudioDestinationNode connect(AudioDestinationProperty destination) const;
  AudioDestinationNode connect(double destinationHandle) const;
  void start(double when = 0.0) const;
  void stop(double when = 0.0) const;
};

struct AudioContext {
  AudioDestinationProperty destination;
  AudioContextCurrentTimeProperty currentTime;

  OscillatorNode createOscillator() const;
  AudioBufferSourceNode createBufferSource() const;
  AudioBuffer decodeAudioData(const std::vector<std::uint8_t> &bytes) const;
};

class HTMLAudioElement {
 public:
  HTMLAudioElement() = default;
  explicit HTMLAudioElement(const char *src);
  explicit HTMLAudioElement(const std::string &src);
  explicit HTMLAudioElement(const gea::embedded::ui::NodeHandle &node);

  std::string src() const;
  void setSrc(const std::string &src);
  bool play() const;
  void pause() const;

 private:
  int nodeId_ = -1;
  std::string src_;
};

struct AudioFacade {
  double getVolume() const { return gea::framework::audio::AudioBackend::volume(); }
  void setVolume(double volume) const { gea::framework::audio::AudioBackend::setVolume(volume); }
};

inline constexpr AudioContext audioContext{};
inline constexpr AudioFacade Audio{};

}  // namespace gea::host

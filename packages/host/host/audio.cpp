// SPDX-License-Identifier: Apache-2.0
#include "audio.h"
#include "gea/embedded-host.h"
#include "ui/document.h"  // host/audio.cpp HTMLAudioElement bridge (entangled — moves to elements)

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <utility>
#include <vector>

namespace gea::framework::host {

class AudioHost {
 public:
  static gea::platform::audio::OscillatorType oscillatorType(double value) {
    switch (static_cast<int>(value)) {
      case 1: return gea::platform::audio::OscillatorType::Square;
      case 2: return gea::platform::audio::OscillatorType::Sawtooth;
      case 3: return gea::platform::audio::OscillatorType::Triangle;
      default: return gea::platform::audio::OscillatorType::Sine;
    }
  }

  static gea::platform::audio::AudioContext context() {
    return gea::platform::audio::AudioSystem::sharedContext();
  }

  static gea::platform::audio::OscillatorNode oscillator(gea::host::NativeAudioHandle oscillator) {
    return gea::platform::audio::OscillatorNode(oscillator);
  }

  static gea::platform::audio::AudioDestinationNode destination(gea::host::NativeAudioHandle destination) {
    return gea::platform::audio::AudioDestinationNode(destination);
  }
};

}  // namespace gea::framework::host

namespace gea::framework::audio {

double AudioBackend::volume() {
  return static_cast<double>(gea::platform::audio::AudioSystem::volume());
}

void AudioBackend::setVolume(double volume) {
  gea::platform::audio::AudioSystem::setVolume(static_cast<int>(volume));
}

}  // namespace gea::framework::audio

namespace gea::host {

namespace {

std::uint16_t le16(const std::vector<std::uint8_t> &bytes, std::size_t offset) {
  if (offset + 2 > bytes.size()) return 0;
  return static_cast<std::uint16_t>(bytes[offset] | (static_cast<std::uint16_t>(bytes[offset + 1]) << 8));
}

std::uint32_t le32(const std::vector<std::uint8_t> &bytes, std::size_t offset) {
  if (offset + 4 > bytes.size()) return 0;
  return static_cast<std::uint32_t>(bytes[offset]) |
         (static_cast<std::uint32_t>(bytes[offset + 1]) << 8) |
         (static_cast<std::uint32_t>(bytes[offset + 2]) << 16) |
         (static_cast<std::uint32_t>(bytes[offset + 3]) << 24);
}

bool tagAt(const std::vector<std::uint8_t> &bytes, std::size_t offset, const char *tag) {
  return offset + 4 <= bytes.size() && std::memcmp(bytes.data() + offset, tag, 4) == 0;
}

AudioBuffer decodePcm16Wav(const std::vector<std::uint8_t> &bytes) {
  if (bytes.size() < 44 || !tagAt(bytes, 0, "RIFF") || !tagAt(bytes, 8, "WAVE")) return {};

  int channels = 1;
  int sampleRate = 16000;
  std::size_t dataOffset = 0;
  std::size_t dataBytes = 0;
  std::uint16_t format = 0;
  std::uint16_t bitsPerSample = 0;

  std::size_t cursor = 12;
  while (cursor + 8 <= bytes.size()) {
    const std::uint32_t chunkSize = le32(bytes, cursor + 4);
    const std::size_t payload = cursor + 8;
    if (payload + chunkSize > bytes.size()) break;

    if (tagAt(bytes, cursor, "fmt ") && chunkSize >= 16) {
      format = le16(bytes, payload);
      channels = static_cast<int>(le16(bytes, payload + 2));
      sampleRate = static_cast<int>(le32(bytes, payload + 4));
      bitsPerSample = le16(bytes, payload + 14);
    } else if (tagAt(bytes, cursor, "data")) {
      dataOffset = payload;
      dataBytes = chunkSize;
    }

    cursor = payload + chunkSize + (chunkSize & 1u);
  }

  if (format != 1 || bitsPerSample != 16 || dataOffset == 0 || dataBytes < 2) return {};
  if (channels <= 0) channels = 1;
  if (sampleRate <= 0) sampleRate = 16000;

  const std::size_t sampleCount = dataBytes / sizeof(std::int16_t);
  std::vector<std::int16_t> samples(sampleCount);
  for (std::size_t i = 0; i < sampleCount; ++i) {
    const std::size_t off = dataOffset + i * 2;
    samples[i] = static_cast<std::int16_t>(le16(bytes, off));
  }
  return AudioBuffer(std::move(samples), sampleRate, channels);
}

}  // namespace

AudioDestinationProperty::operator AudioDestinationNode() const {
  return AudioDestinationNode(gea::framework::host::AudioHost::context().destination().nativeId());
}

AudioDestinationProperty::operator double() const {
  return static_cast<double>(gea::framework::host::AudioHost::context().destination().nativeId());
}

AudioContextCurrentTimeProperty::operator double() const {
  return gea::framework::host::AudioHost::context().currentTime();
}

const AudioParamValueProperty &AudioParamValueProperty::operator=(double frequency_hz) const {
  gea::framework::host::AudioHost::oscillator(oscillatorHandle).frequency.setValue(frequency_hz);
  return *this;
}

AudioParamValueProperty::operator double() const {
  return gea::framework::host::AudioHost::oscillator(oscillatorHandle).frequency.value();
}

const AudioParam &AudioParam::operator=(double frequency_hz) const {
  gea::framework::host::AudioHost::oscillator(oscillatorHandle).frequency.setValue(frequency_hz);
  return *this;
}

void AudioParam::setValueAtTime(double frequency_hz, double start_time) const {
  gea::framework::host::AudioHost::oscillator(oscillatorHandle).frequency.setValueAtTime(frequency_hz, start_time);
}

const OscillatorTypeProperty &OscillatorTypeProperty::operator=(double type) const {
  gea::framework::host::AudioHost::oscillator(oscillatorHandle).setType(gea::framework::host::AudioHost::oscillatorType(type));
  return *this;
}

const OscillatorTypeProperty &OscillatorTypeProperty::operator=(const char *type) const {
  return (*this = detail::oscillator_type_from_name(type));
}

const OscillatorTypeProperty &OscillatorTypeProperty::operator=(const std::string &type) const {
  return (*this = type.c_str());
}

void OscillatorNode::connect(AudioDestinationNode destination) const {
  gea::framework::host::AudioHost::oscillator(nativeHandle).connect(gea::framework::host::AudioHost::destination(destination.nativeHandle));
}

void OscillatorNode::connect(AudioDestinationProperty destination) const {
  connect(static_cast<AudioDestinationNode>(destination));
}

void OscillatorNode::connect(double destinationHandle) const {
  connect(AudioDestinationNode(destinationHandle));
}

void OscillatorNode::start(double when) const {
  gea::framework::host::AudioHost::oscillator(nativeHandle).start(when);
}

void OscillatorNode::stop(double when) const {
  gea::framework::host::AudioHost::oscillator(nativeHandle).stop(when);
}

AudioDestinationNode AudioBufferSourceNode::connect(AudioDestinationNode destination) const {
  connected = true;
  return destination;
}

AudioDestinationNode AudioBufferSourceNode::connect(AudioDestinationProperty destination) const {
  return connect(static_cast<AudioDestinationNode>(destination));
}

AudioDestinationNode AudioBufferSourceNode::connect(double destinationHandle) const {
  return connect(AudioDestinationNode(destinationHandle));
}

void AudioBufferSourceNode::start(double when) const {
  (void)when;
  if (!connected || buffer.samples.empty()) return;
  gea::platform::audio::AudioSystem::playPcm(buffer.samples.data(), buffer.samples.size(), buffer.sampleRate, buffer.channels);
}

void AudioBufferSourceNode::stop(double when) const {
  (void)when;
  gea::platform::audio::AudioSystem::stopPlayback();
}

OscillatorNode AudioContext::createOscillator() const {
  return OscillatorNode(gea::framework::host::AudioHost::context().createOscillator().nativeId());
}

AudioBufferSourceNode AudioContext::createBufferSource() const {
  return AudioBufferSourceNode{};
}

AudioBuffer AudioContext::decodeAudioData(const std::vector<std::uint8_t> &bytes) const {
  return decodePcm16Wav(bytes);
}

HTMLAudioElement::HTMLAudioElement(const char *src) : src_(src ? src : "") {}

HTMLAudioElement::HTMLAudioElement(const std::string &src) : src_(src) {}

HTMLAudioElement::HTMLAudioElement(const gea::embedded::ui::NodeHandle &node) : nodeId_(node.id()) {}

std::string HTMLAudioElement::src() const {
  if (nodeId_ >= 0) return std::string(gea::embedded::ui::NodeHandle(nodeId_).getAttribute("src"));
  return src_;
}

void HTMLAudioElement::setSrc(const std::string &src) {
  if (nodeId_ >= 0) gea::embedded::ui::NodeHandle(nodeId_).setAttribute("src", src.c_str());
  src_ = src;
}

bool HTMLAudioElement::play() const {
  return gea::platform::audio::AudioSystem::playFile(src());
}

void HTMLAudioElement::pause() const {
  gea::platform::audio::AudioSystem::stopPlayback();
}

}  // namespace gea::host

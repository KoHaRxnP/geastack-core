// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace gea::host {

using NativeMediaStreamHandle = std::uint32_t;
using NativeMediaTrackHandle = std::uint32_t;
using NativeMediaRecorderHandle = std::uint32_t;

class MediaStreamTrack {
 public:
  NativeMediaTrackHandle nativeHandle = 0;

  constexpr MediaStreamTrack() = default;
  explicit MediaStreamTrack(NativeMediaTrackHandle h) : nativeHandle(h) {}
  explicit MediaStreamTrack(double h) : nativeHandle(static_cast<NativeMediaTrackHandle>(h)) {}
  constexpr operator double() const { return static_cast<double>(nativeHandle); }

  template <typename Value>
  static MediaStreamTrack __gea_native_from_value(const Value &value) {
    if constexpr (requires { static_cast<double>(value); }) return MediaStreamTrack(static_cast<double>(value));
    else return MediaStreamTrack{};
  }

  std::string id() const;
  std::string kind() const;
  std::string readyState() const;
  bool enabled() const;
  void setEnabled(bool value) const;
  void stop() const;
};

class MediaStream {
 public:
  NativeMediaStreamHandle nativeHandle = 0;

  constexpr MediaStream() = default;
  explicit MediaStream(NativeMediaStreamHandle h) : nativeHandle(h) {}
  explicit MediaStream(double h) : nativeHandle(static_cast<NativeMediaStreamHandle>(h)) {}
  constexpr operator double() const { return static_cast<double>(nativeHandle); }

  template <typename Value>
  static MediaStream __gea_native_from_value(const Value &value) {
    if constexpr (requires { static_cast<double>(value); }) return MediaStream(static_cast<double>(value));
    else return MediaStream{};
  }

  std::string id() const;
  std::vector<MediaStreamTrack> getAudioTracks() const;
  std::vector<MediaStreamTrack> getTracks() const;
};

struct GeaAudioBlob {
  std::string path;
  std::string type = "audio/wav";
  double size = 0.0;

  template <typename Value>
  static GeaAudioBlob __gea_native_from_value(const Value &) { return {}; }

  std::vector<std::uint8_t> arrayBuffer() const;
  std::string text() const;
};

struct MediaRecorderDataEvent {
  GeaAudioBlob data;
};

namespace media {
template <typename Options>
std::string recorder_path_from_options(const Options &options);

template <typename Options>
std::string recorder_mime_from_options(const Options &options);
}  // namespace media

struct MediaRecorderStateProperty {
  NativeMediaRecorderHandle nativeHandle = 0;

  constexpr MediaRecorderStateProperty() = default;
  explicit constexpr MediaRecorderStateProperty(NativeMediaRecorderHandle handle) : nativeHandle(handle) {}

  operator std::string() const;
};

class MediaRecorder {
 public:
  NativeMediaRecorderHandle nativeHandle = 0;
  MediaStream stream;
  mutable MediaRecorderStateProperty state;
  std::function<void(MediaRecorderDataEvent)> ondataavailable = nullptr;
  std::function<void()> onstop = nullptr;

  MediaRecorder() = default;
  explicit MediaRecorder(double handle) : nativeHandle(static_cast<NativeMediaRecorderHandle>(handle)), state(nativeHandle) {}
  explicit MediaRecorder(MediaStream mediaStream);

  template <typename Options>
  MediaRecorder(MediaStream mediaStream, const Options &options)
      : MediaRecorder(mediaStream, media::recorder_path_from_options(options), media::recorder_mime_from_options(options)) {}

  MediaRecorder(MediaStream mediaStream, const std::string &path, const std::string &mimeType = "audio/wav");

  std::string mimeType() const;
  void start(double timesliceMs = 0.0) const;
  void stop() const;

  template <typename Value>
  static MediaRecorder __gea_native_from_value(const Value &value) {
    if constexpr (requires { static_cast<double>(value); }) return MediaRecorder(static_cast<double>(value));
    else return MediaRecorder{};
  }
};

namespace media {

NativeMediaStreamHandle create_stream();
void destroy_stream(NativeMediaStreamHandle handle);
NativeMediaTrackHandle stream_audio_track(NativeMediaStreamHandle handle);

void track_inject_pcm(NativeMediaTrackHandle handle, const std::int16_t *samples, std::size_t count);
std::size_t track_read_pcm(NativeMediaTrackHandle handle, std::int16_t *samples, std::size_t max_samples);

NativeMediaStreamHandle get_user_media_audio();
NativeMediaRecorderHandle create_recorder(NativeMediaStreamHandle stream, const std::string &path, const std::string &mimeType);
std::string recorder_state(NativeMediaRecorderHandle handle);
std::string recorder_mime_type(NativeMediaRecorderHandle handle);
void recorder_start(NativeMediaRecorderHandle handle);
GeaAudioBlob recorder_stop(NativeMediaRecorderHandle handle);

template <typename Options>
std::string recorder_path_from_options(const Options &options) {
  if constexpr (requires { options.path; }) {
    return std::string(options.path);
  } else {
    return {};
  }
}

template <typename Options>
std::string recorder_mime_from_options(const Options &options) {
  if constexpr (requires { options.mimeType; }) {
    return std::string(options.mimeType);
  } else {
    return "audio/wav";
  }
}

// Overload accepting a MediaStreamConstraints-shaped record (from
// `navigator.mediaDevices.getUserMedia({...})`). The fields aren't acted on
// yet — there's no per-device picker on the embedded target — but accepting
// the arg lets the geatsc-emitted call site link. The single-arg form
// dispatches to the no-arg implementation.
template <typename Constraints>
NativeMediaStreamHandle get_user_media_audio(const Constraints &) { return get_user_media_audio(); }

}  // namespace media

}  // namespace gea::host

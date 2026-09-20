// SPDX-License-Identifier: Apache-2.0
#include "host/media.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "platform/file_cache.h"

#ifdef ESP_PLATFORM
#include "esp_log.h"
#endif

#if defined(_WIN32)
#include <direct.h>
#else
#include <sys/stat.h>
#include <sys/types.h>
#endif

namespace gea::host {

namespace {

struct TrackState {
  std::string id;
  std::string kind = "audio";
  std::string readyState = "live";
  bool enabled = true;
  std::deque<std::int16_t> ringBuffer;
};

struct StreamState {
  std::string id;
  NativeMediaTrackHandle audioTrack = 0;
};

struct RecorderState {
  std::string id;
  NativeMediaStreamHandle stream = 0;
  NativeMediaTrackHandle track = 0;
  std::string state = "inactive";
  std::string path;
  std::string mimeType = "audio/wav";
  std::vector<std::int16_t> pcm;
  double sampleRate = 16000.0;
  double channels = 1.0;
  FILE *file = nullptr;
  std::size_t samplesWritten = 0;
  bool writeFailed = false;
};

std::unordered_map<NativeMediaStreamHandle, StreamState> &streamTable() {
  static std::unordered_map<NativeMediaStreamHandle, StreamState> table;
  return table;
}

std::unordered_map<NativeMediaTrackHandle, TrackState> &trackTable() {
  static std::unordered_map<NativeMediaTrackHandle, TrackState> table;
  return table;
}

std::unordered_map<NativeMediaRecorderHandle, RecorderState> &recorderTable() {
  static std::unordered_map<NativeMediaRecorderHandle, RecorderState> table;
  return table;
}

std::mutex &mediaMutex() {
  static std::mutex m;
  return m;
}

std::atomic<NativeMediaStreamHandle> &nextStreamHandle() {
  static std::atomic<NativeMediaStreamHandle> id{1};
  return id;
}

std::atomic<NativeMediaTrackHandle> &nextTrackHandle() {
  static std::atomic<NativeMediaTrackHandle> id{1};
  return id;
}

std::atomic<NativeMediaRecorderHandle> &nextRecorderHandle() {
  static std::atomic<NativeMediaRecorderHandle> id{1};
  return id;
}

std::string formatHandleId(const char *prefix, std::uint32_t handle) {
  char buf[64];
  std::snprintf(buf, sizeof(buf), "%s-%u", prefix, static_cast<unsigned>(handle));
  return buf;
}

constexpr std::size_t kCaptureChunkSamples = 2048;
constexpr std::size_t kMaxRingBufferSamples = 16 * kCaptureChunkSamples;
constexpr std::size_t kRecorderFlushSamples = kCaptureChunkSamples;
constexpr double kDefaultRecorderSampleRate = 16000.0;

#ifdef ESP_PLATFORM
constexpr const char *kMediaLogTag = "gea_media";
#endif

void makeParentDirs(const std::string &path) {
  std::size_t pos = 1;
  while ((pos = path.find('/', pos)) != std::string::npos) {
    const std::string dir = path.substr(0, pos);
    if (!dir.empty()) {
#if defined(_WIN32)
      _mkdir(dir.c_str());
#else
      mkdir(dir.c_str(), 0775);
#endif
    }
    ++pos;
  }
}

void writeLe16(FILE *file, std::uint16_t value) {
  std::uint8_t bytes[2] = {
      static_cast<std::uint8_t>(value & 0xffu),
      static_cast<std::uint8_t>((value >> 8) & 0xffu),
  };
  std::fwrite(bytes, 1, sizeof(bytes), file);
}

void writeLe32(FILE *file, std::uint32_t value) {
  std::uint8_t bytes[4] = {
      static_cast<std::uint8_t>(value & 0xffu),
      static_cast<std::uint8_t>((value >> 8) & 0xffu),
      static_cast<std::uint8_t>((value >> 16) & 0xffu),
      static_cast<std::uint8_t>((value >> 24) & 0xffu),
  };
  std::fwrite(bytes, 1, sizeof(bytes), file);
}

std::uint32_t wavDataBytesForSamples(std::size_t sampleCount) {
  const auto bytes = sampleCount * sizeof(std::int16_t);
  return static_cast<std::uint32_t>(bytes);
}

void writeWavHeader(FILE *file, std::uint32_t dataBytes, int sampleRate, int channels) {
  const std::uint32_t riffBytes = 36u + dataBytes;
  const std::uint16_t channelCount = static_cast<std::uint16_t>(channels <= 0 ? 1 : channels);
  const std::uint32_t rate = static_cast<std::uint32_t>(sampleRate <= 0 ? kDefaultRecorderSampleRate : sampleRate);
  const std::uint16_t bitsPerSample = 16;
  const std::uint16_t blockAlign = static_cast<std::uint16_t>(channelCount * (bitsPerSample / 8));
  const std::uint32_t byteRate = rate * blockAlign;

  std::fwrite("RIFF", 1, 4, file);
  writeLe32(file, riffBytes);
  std::fwrite("WAVE", 1, 4, file);
  std::fwrite("fmt ", 1, 4, file);
  writeLe32(file, 16);
  writeLe16(file, 1);
  writeLe16(file, channelCount);
  writeLe32(file, rate);
  writeLe32(file, byteRate);
  writeLe16(file, blockAlign);
  writeLe16(file, bitsPerSample);
  std::fwrite("data", 1, 4, file);
  writeLe32(file, dataBytes);
}

bool patchWavHeader(FILE *file, std::uint32_t dataBytes) {
  if (!file) return false;
  if (std::fflush(file) != 0) return false;
  const std::uint32_t riffBytes = 36u + dataBytes;
  if (std::fseek(file, 4, SEEK_SET) != 0) return false;
  writeLe32(file, riffBytes);
  if (std::fseek(file, 40, SEEK_SET) != 0) return false;
  writeLe32(file, dataBytes);
  return std::fflush(file) == 0;
}

double writeWavFile(const std::string &path, const std::vector<std::int16_t> &samples, int sampleRate, int channels) {
  if (path.empty()) return 0.0;
  gea::platform::storage::ensureMounted();
  makeParentDirs(path);

  FILE *file = std::fopen(path.c_str(), "wb");
  if (!file) {
#ifdef ESP_PLATFORM
    ESP_LOGE(kMediaLogTag, "failed to open WAV for write path=%s errno=%d", path.c_str(), errno);
#endif
    return 0.0;
  }

  const std::uint32_t dataBytes = wavDataBytesForSamples(samples.size());
  writeWavHeader(file, dataBytes, sampleRate, channels);
  std::size_t writtenSamples = 0;
  while (writtenSamples < samples.size()) {
    const std::size_t remaining = samples.size() - writtenSamples;
    const std::size_t chunkSamples = std::min<std::size_t>(remaining, 2048);
    const std::size_t written = std::fwrite(samples.data() + writtenSamples, sizeof(std::int16_t), chunkSamples, file);
    writtenSamples += written;
    if (written != chunkSamples) break;
  }
  const std::uint32_t actualDataBytes = static_cast<std::uint32_t>(writtenSamples * sizeof(std::int16_t));
  if (actualDataBytes != dataBytes) {
    patchWavHeader(file, actualDataBytes);
#ifdef ESP_PLATFORM
    ESP_LOGW(kMediaLogTag,
             "partial WAV write path=%s samples=%u/%u errno=%d",
             path.c_str(),
             static_cast<unsigned>(writtenSamples),
             static_cast<unsigned>(samples.size()),
             errno);
#endif
  }
  const bool ok = std::fclose(file) == 0;
  if (!ok) {
#ifdef ESP_PLATFORM
    ESP_LOGE(kMediaLogTag, "failed to close WAV path=%s errno=%d", path.c_str(), errno);
#endif
    return 0.0;
  }
  return static_cast<double>(44u + actualDataBytes);
}

bool openRecorderFile(RecorderState &recorder) {
  if (recorder.path.empty()) return false;
  gea::platform::storage::ensureMounted();
  makeParentDirs(recorder.path);
  recorder.file = std::fopen(recorder.path.c_str(), "wb");
  recorder.samplesWritten = 0;
  recorder.writeFailed = false;
  if (!recorder.file) {
    recorder.writeFailed = true;
#ifdef ESP_PLATFORM
    ESP_LOGE(kMediaLogTag, "failed to open recorder WAV path=%s errno=%d", recorder.path.c_str(), errno);
#endif
    return false;
  }
  writeWavHeader(recorder.file, 0, static_cast<int>(recorder.sampleRate), static_cast<int>(recorder.channels));
  return true;
}

void closeRecorderFile(RecorderState &recorder) {
  if (!recorder.file) return;
  std::fclose(recorder.file);
  recorder.file = nullptr;
}

bool flushRecorderPcm(RecorderState &recorder) {
  if (!recorder.file || recorder.pcm.empty() || recorder.writeFailed) return !recorder.writeFailed;
  std::size_t writtenSamples = 0;
  while (writtenSamples < recorder.pcm.size()) {
    const std::size_t remaining = recorder.pcm.size() - writtenSamples;
    const std::size_t chunkSamples = std::min<std::size_t>(remaining, 2048);
    const std::size_t written =
        std::fwrite(recorder.pcm.data() + writtenSamples, sizeof(std::int16_t), chunkSamples, recorder.file);
    recorder.samplesWritten += written;
    writtenSamples += written;
    if (written != chunkSamples) {
      recorder.writeFailed = true;
#ifdef ESP_PLATFORM
      ESP_LOGW(kMediaLogTag,
               "recorder WAV write stopped path=%s samples=%u errno=%d",
               recorder.path.c_str(),
               static_cast<unsigned>(recorder.samplesWritten),
               errno);
#endif
      recorder.pcm.clear();
      return false;
    }
  }
  recorder.pcm.clear();
  return true;
}

void appendRecorderPcm(RecorderState &recorder, const std::int16_t *samples, std::size_t count) {
  if (!recorder.file || recorder.writeFailed || !samples || count == 0) return;
  recorder.pcm.insert(recorder.pcm.end(), samples, samples + count);
  if (recorder.pcm.size() >= kRecorderFlushSamples) flushRecorderPcm(recorder);
}

double finalizeRecorderFile(RecorderState &recorder) {
  flushRecorderPcm(recorder);
  if (!recorder.file) return 0.0;
  const auto dataBytes = wavDataBytesForSamples(recorder.samplesWritten);
  const bool patched = patchWavHeader(recorder.file, dataBytes);
  const bool closed = std::fclose(recorder.file) == 0;
  recorder.file = nullptr;
  if (!patched || !closed) {
    recorder.writeFailed = true;
#ifdef ESP_PLATFORM
    ESP_LOGE(kMediaLogTag,
             "failed to finalize recorder WAV path=%s patched=%d closed=%d errno=%d",
             recorder.path.c_str(),
             patched ? 1 : 0,
             closed ? 1 : 0,
             errno);
#endif
    return 0.0;
  }
  return static_cast<double>(44u + dataBytes);
}

std::vector<std::uint8_t> readFileBytes(const std::string &path) {
  if (path.empty()) return {};
  gea::platform::storage::ensureMounted();
  FILE *file = std::fopen(path.c_str(), "rb");
  if (!file) return {};
  std::fseek(file, 0, SEEK_END);
  const long size = std::ftell(file);
  if (size <= 0) {
    std::fclose(file);
    return {};
  }
  std::fseek(file, 0, SEEK_SET);
  std::vector<std::uint8_t> out(static_cast<std::size_t>(size));
  const std::size_t read = std::fread(out.data(), 1, out.size(), file);
  std::fclose(file);
  out.resize(read);
  return out;
}

}  // namespace

namespace media {

#ifdef ESP_PLATFORM
void platform_attach_track(NativeMediaTrackHandle handle);
void platform_detach_track(NativeMediaTrackHandle handle);
#else
__attribute__((weak)) void platform_attach_track(NativeMediaTrackHandle) {}
__attribute__((weak)) void platform_detach_track(NativeMediaTrackHandle) {}
#endif

NativeMediaStreamHandle create_stream() {
  auto streamHandle = nextStreamHandle()++;
  auto trackHandle = nextTrackHandle()++;
  {
    std::lock_guard<std::mutex> lock(mediaMutex());
    trackTable()[trackHandle] = TrackState{
        formatHandleId("track", trackHandle), "audio", "live", true, {}};
    streamTable()[streamHandle] = StreamState{
        formatHandleId("stream", streamHandle), trackHandle};
  }
  platform_attach_track(trackHandle);
  return streamHandle;
}

NativeMediaTrackHandle stream_audio_track(NativeMediaStreamHandle handle) {
  std::lock_guard<std::mutex> lock(mediaMutex());
  auto it = streamTable().find(handle);
  return it == streamTable().end() ? 0 : it->second.audioTrack;
}

void destroy_stream(NativeMediaStreamHandle handle) {
  NativeMediaTrackHandle trackHandle = 0;
  {
    std::lock_guard<std::mutex> lock(mediaMutex());
    auto it = streamTable().find(handle);
    if (it == streamTable().end()) return;
    trackHandle = it->second.audioTrack;
    for (auto &entry : recorderTable()) {
      if (entry.second.track == trackHandle && entry.second.state == "recording") {
        finalizeRecorderFile(entry.second);
        entry.second.state = "inactive";
      }
    }
    trackTable().erase(trackHandle);
    streamTable().erase(it);
  }
  if (trackHandle) platform_detach_track(trackHandle);
}

void track_inject_pcm(NativeMediaTrackHandle handle, const std::int16_t *samples, std::size_t count) {
  std::lock_guard<std::mutex> lock(mediaMutex());
  auto it = trackTable().find(handle);
  if (it == trackTable().end() || !it->second.enabled) return;
  auto &buffer = it->second.ringBuffer;
  for (std::size_t i = 0; i < count; ++i) {
    if (buffer.size() >= kMaxRingBufferSamples) buffer.pop_front();
    buffer.push_back(samples[i]);
  }
  for (auto &entry : recorderTable()) {
    auto &recorder = entry.second;
    if (recorder.track == handle && recorder.state == "recording") {
      appendRecorderPcm(recorder, samples, count);
    }
  }
}

std::size_t track_read_pcm(NativeMediaTrackHandle handle, std::int16_t *samples, std::size_t max_samples) {
  std::lock_guard<std::mutex> lock(mediaMutex());
  auto it = trackTable().find(handle);
  if (it == trackTable().end()) return 0;
  auto &buffer = it->second.ringBuffer;
  std::size_t copied = 0;
  while (copied < max_samples && !buffer.empty()) {
    samples[copied++] = buffer.front();
    buffer.pop_front();
  }
  return copied;
}

NativeMediaStreamHandle get_user_media_audio() {
  return create_stream();
}

NativeMediaRecorderHandle create_recorder(NativeMediaStreamHandle stream, const std::string &path, const std::string &mimeType) {
  const auto recorderHandle = nextRecorderHandle()++;
  const auto trackHandle = stream_audio_track(stream);
  std::string effectivePath = path;
  if (effectivePath.empty()) {
    char buf[96];
    std::snprintf(buf, sizeof(buf), "/sdcard/recordings/recording_%03u.wav", static_cast<unsigned>(recorderHandle));
    effectivePath = buf;
  }
  {
    std::lock_guard<std::mutex> lock(mediaMutex());
    recorderTable()[recorderHandle] = RecorderState{
        formatHandleId("recorder", recorderHandle),
        stream,
        trackHandle,
        "inactive",
        effectivePath,
        mimeType.empty() ? std::string("audio/wav") : mimeType,
        {},
        kDefaultRecorderSampleRate,
        1.0,
    };
  }
  return recorderHandle;
}

std::string recorder_state(NativeMediaRecorderHandle handle) {
  std::lock_guard<std::mutex> lock(mediaMutex());
  auto it = recorderTable().find(handle);
  return it == recorderTable().end() ? std::string("inactive") : it->second.state;
}

std::string recorder_mime_type(NativeMediaRecorderHandle handle) {
  std::lock_guard<std::mutex> lock(mediaMutex());
  auto it = recorderTable().find(handle);
  return it == recorderTable().end() ? std::string("audio/wav") : it->second.mimeType;
}

void recorder_start(NativeMediaRecorderHandle handle) {
  std::lock_guard<std::mutex> lock(mediaMutex());
  auto it = recorderTable().find(handle);
  if (it == recorderTable().end()) return;
  closeRecorderFile(it->second);
  it->second.pcm.clear();
  it->second.samplesWritten = 0;
  it->second.writeFailed = false;
  if (openRecorderFile(it->second)) {
    it->second.state = "recording";
  } else {
    it->second.state = "inactive";
  }
}

GeaAudioBlob recorder_stop(NativeMediaRecorderHandle handle) {
  GeaAudioBlob blob;
  std::lock_guard<std::mutex> lock(mediaMutex());
  auto it = recorderTable().find(handle);
  if (it == recorderTable().end()) return {};
  it->second.state = "inactive";
  blob.path = it->second.path;
  blob.type = it->second.mimeType.empty() ? std::string("audio/wav") : it->second.mimeType;
  blob.size = finalizeRecorderFile(it->second);
  return blob;
}

}  // namespace media

std::vector<std::uint8_t> GeaAudioBlob::arrayBuffer() const {
  return readFileBytes(path);
}

std::string GeaAudioBlob::text() const {
  auto bytes = readFileBytes(path);
  return std::string(bytes.begin(), bytes.end());
}

MediaRecorderStateProperty::operator std::string() const {
  return media::recorder_state(nativeHandle);
}

MediaRecorder::MediaRecorder(MediaStream mediaStream)
    : MediaRecorder(mediaStream, std::string{}, std::string("audio/wav")) {}

MediaRecorder::MediaRecorder(MediaStream mediaStream, const std::string &path, const std::string &mimeType)
    : nativeHandle(media::create_recorder(mediaStream.nativeHandle, path, mimeType)),
      stream(mediaStream),
      state(nativeHandle) {}

std::string MediaRecorder::mimeType() const {
  return media::recorder_mime_type(nativeHandle);
}

void MediaRecorder::start(double timesliceMs) const {
  (void)timesliceMs;
  media::recorder_start(nativeHandle);
}

void MediaRecorder::stop() const {
  const auto blob = media::recorder_stop(nativeHandle);
  if (ondataavailable) ondataavailable(MediaRecorderDataEvent{blob});
  if (onstop) onstop();
}

std::string MediaStreamTrack::id() const {
  std::lock_guard<std::mutex> lock(mediaMutex());
  auto it = trackTable().find(nativeHandle);
  return it == trackTable().end() ? std::string{} : it->second.id;
}

std::string MediaStreamTrack::kind() const {
  std::lock_guard<std::mutex> lock(mediaMutex());
  auto it = trackTable().find(nativeHandle);
  return it == trackTable().end() ? std::string{} : it->second.kind;
}

std::string MediaStreamTrack::readyState() const {
  std::lock_guard<std::mutex> lock(mediaMutex());
  auto it = trackTable().find(nativeHandle);
  return it == trackTable().end() ? std::string("ended") : it->second.readyState;
}

bool MediaStreamTrack::enabled() const {
  std::lock_guard<std::mutex> lock(mediaMutex());
  auto it = trackTable().find(nativeHandle);
  return it == trackTable().end() ? false : it->second.enabled;
}

void MediaStreamTrack::setEnabled(bool value) const {
  std::lock_guard<std::mutex> lock(mediaMutex());
  auto it = trackTable().find(nativeHandle);
  if (it != trackTable().end()) it->second.enabled = value;
}

void MediaStreamTrack::stop() const {
  bool shouldDetach = false;
  {
    std::lock_guard<std::mutex> lock(mediaMutex());
    auto it = trackTable().find(nativeHandle);
    if (it != trackTable().end()) {
      shouldDetach = it->second.readyState != "ended";
      it->second.readyState = "ended";
      it->second.enabled = false;
      for (auto &entry : recorderTable()) {
        if (entry.second.track == nativeHandle && entry.second.state == "recording") {
          entry.second.state = "inactive";
        }
      }
    }
  }
  if (shouldDetach) media::platform_detach_track(nativeHandle);
}

std::string MediaStream::id() const {
  std::lock_guard<std::mutex> lock(mediaMutex());
  auto it = streamTable().find(nativeHandle);
  return it == streamTable().end() ? std::string{} : it->second.id;
}

std::vector<MediaStreamTrack> MediaStream::getAudioTracks() const {
  std::lock_guard<std::mutex> lock(mediaMutex());
  auto it = streamTable().find(nativeHandle);
  if (it == streamTable().end() || it->second.audioTrack == 0) return {};
  return {MediaStreamTrack(it->second.audioTrack)};
}

std::vector<MediaStreamTrack> MediaStream::getTracks() const {
  return getAudioTracks();
}

}  // namespace gea::host

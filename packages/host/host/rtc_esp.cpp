// SPDX-License-Identifier: Apache-2.0
#ifdef ESP_PLATFORM

#include "host/rtc.h"
#include "host/media.h"

#include "esp_log.h"
#include "esp_peer.h"
#include "esp_peer_default.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include <atomic>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace gea::host::rtc {

namespace {

constexpr const char *kTag = "gea::host::rtc";
constexpr int kAudioSampleRate = 16000;
constexpr int kAudioChannels = 1;
constexpr std::size_t kSendFrameSamples = 320;  // 20 ms @ 16 kHz mono

struct EspPeerState {
  esp_peer_handle_t handle = nullptr;
  NativeRtcPeerHandle our_handle = 0;
  TaskHandle_t main_loop_task = nullptr;
  TaskHandle_t audio_send_task = nullptr;

  // create_offer blocks on this until on_msg fires with SDP.
  SemaphoreHandle_t sdp_ready = nullptr;
  std::string pending_sdp;

  NativeMediaTrackHandle local_track = 0;
  NativeMediaTrackHandle remote_track = 0;

  bool open = false;
};

std::unordered_map<NativeRtcPeerHandle, EspPeerState *> &espPeerTable() {
  static std::unordered_map<NativeRtcPeerHandle, EspPeerState *> table;
  return table;
}

std::mutex &espPeerMutex() {
  static std::mutex m;
  return m;
}

EspPeerState *findPeer(NativeRtcPeerHandle handle) {
  std::lock_guard<std::mutex> lock(espPeerMutex());
  auto it = espPeerTable().find(handle);
  return it == espPeerTable().end() ? nullptr : it->second;
}

// Callbacks fire on esp_peer's main-loop task — we marshal events onto our
// JS-side frame-loop pump via the existing rtc::enqueue_* hooks.

int on_state_cb(esp_peer_state_t state, void *ctx) {
  auto *peer_state = static_cast<EspPeerState *>(ctx);
  const char *name = "new";
  switch (state) {
    case ESP_PEER_STATE_CLOSED: name = "closed"; break;
    case ESP_PEER_STATE_DISCONNECTED: name = "disconnected"; break;
    case ESP_PEER_STATE_NEW_CONNECTION:
    case ESP_PEER_STATE_CANDIDATE_GATHERING:
    case ESP_PEER_STATE_PAIRING:
    case ESP_PEER_STATE_PAIRED:
    case ESP_PEER_STATE_CONNECTING:
      name = "connecting";
      break;
    case ESP_PEER_STATE_CONNECTED: name = "connected"; break;
    case ESP_PEER_STATE_CONNECT_FAILED: name = "failed"; break;
    default: name = "new"; break;
  }
  enqueue_connection_state_change(peer_state->our_handle, std::string(name));
  enqueue_ice_connection_state_change(peer_state->our_handle, std::string(name));
  return 0;
}

int on_msg_cb(esp_peer_msg_t *info, void *ctx) {
  auto *peer_state = static_cast<EspPeerState *>(ctx);
  if (!info || !info->data || info->size <= 0) return 0;
  std::string payload(reinterpret_cast<const char *>(info->data), static_cast<std::size_t>(info->size));
  switch (info->type) {
    case ESP_PEER_MSG_TYPE_SDP:
      peer_state->pending_sdp = payload;
      if (peer_state->sdp_ready) xSemaphoreGive(peer_state->sdp_ready);
      break;
    case ESP_PEER_MSG_TYPE_CANDIDATE:
      enqueue_ice_candidate(peer_state->our_handle, payload, std::string("0"), 0);
      break;
    default: break;
  }
  return 0;
}

int on_audio_data_cb(esp_peer_audio_frame_t *frame, void *ctx) {
  auto *peer_state = static_cast<EspPeerState *>(ctx);
  if (!frame || !frame->data || frame->size <= 0) return 0;
  if (!peer_state->remote_track) {
    // Lazily create a remote track stream + track so JS can attach playback.
    auto stream_handle = gea::host::media::create_stream();
    peer_state->remote_track = gea::host::media::stream_audio_track(stream_handle);
    enqueue_track(peer_state->our_handle, peer_state->remote_track);
  }
  // Frame data is decoded PCM int16; track_inject_pcm handles the ring buffer.
  gea::host::media::track_inject_pcm(
      peer_state->remote_track,
      reinterpret_cast<const std::int16_t *>(frame->data),
      static_cast<std::size_t>(frame->size) / sizeof(std::int16_t));
  return 0;
}

void main_loop_trampoline(void *arg) {
  auto *peer_state = static_cast<EspPeerState *>(arg);
  while (peer_state->open) {
    esp_peer_main_loop(peer_state->handle);
    vTaskDelay(pdMS_TO_TICKS(1));
  }
  vTaskDelete(nullptr);
}

void audio_send_trampoline(void *arg) {
  auto *peer_state = static_cast<EspPeerState *>(arg);
  std::int16_t buffer[kSendFrameSamples];
  while (peer_state->open) {
    if (peer_state->local_track == 0) {
      vTaskDelay(pdMS_TO_TICKS(20));
      continue;
    }
    std::size_t count = gea::host::media::track_read_pcm(
        peer_state->local_track, buffer, kSendFrameSamples);
    if (count == 0) {
      vTaskDelay(pdMS_TO_TICKS(5));
      continue;
    }
    esp_peer_audio_frame_t frame = {};
    frame.pts = 0;
    frame.data = reinterpret_cast<uint8_t *>(buffer);
    frame.size = static_cast<int>(count * sizeof(std::int16_t));
    esp_peer_send_audio(peer_state->handle, &frame);
  }
  vTaskDelete(nullptr);
}

EspPeerState *ensurePeer(NativeRtcPeerHandle our_handle) {
  std::lock_guard<std::mutex> lock(espPeerMutex());
  auto it = espPeerTable().find(our_handle);
  if (it != espPeerTable().end()) return it->second;

  auto *state = new EspPeerState{};
  state->our_handle = our_handle;
  state->sdp_ready = xSemaphoreCreateBinary();
  state->open = true;

  static esp_peer_ice_server_cfg_t default_stun = {};
  default_stun.stun_url = const_cast<char *>("stun:stun.l.google.com:19302");

  esp_peer_cfg_t cfg = {};
  cfg.server_lists = &default_stun;
  cfg.server_num = 1;
  cfg.role = ESP_PEER_ROLE_CONTROLLING;
  cfg.ice_trans_policy = ESP_PEER_ICE_TRANS_POLICY_ALL;
  cfg.audio_info.codec = ESP_PEER_AUDIO_CODEC_OPUS;
  cfg.audio_info.sample_rate = kAudioSampleRate;
  cfg.audio_info.channel = kAudioChannels;
  cfg.audio_dir = ESP_PEER_MEDIA_DIR_SEND_RECV;
  cfg.video_dir = ESP_PEER_MEDIA_DIR_NONE;
  cfg.enable_data_channel = false;
  cfg.ctx = state;
  cfg.on_state = on_state_cb;
  cfg.on_msg = on_msg_cb;
  cfg.on_audio_data = on_audio_data_cb;

  int err = esp_peer_open(&cfg, esp_peer_get_default_impl(), &state->handle);
  if (err != 0 || !state->handle) {
    ESP_LOGE(kTag, "esp_peer_open failed: %d", err);
    if (state->sdp_ready) vSemaphoreDelete(state->sdp_ready);
    delete state;
    return nullptr;
  }

  xTaskCreatePinnedToCore(main_loop_trampoline, "gea_rtc_loop", 8192, state, 7, &state->main_loop_task, 1);
  xTaskCreatePinnedToCore(audio_send_trampoline, "gea_rtc_send", 4096, state, 5, &state->audio_send_task, 1);

  espPeerTable()[our_handle] = state;
  return state;
}

void destroyPeerLocked(EspPeerState *state) {
  state->open = false;
  if (state->handle) {
    esp_peer_close(state->handle);
    state->handle = nullptr;
  }
  if (state->sdp_ready) {
    vSemaphoreDelete(state->sdp_ready);
    state->sdp_ready = nullptr;
  }
  // Tasks will exit on the next loop iteration once state->open is false.
  delete state;
}

}  // namespace

NativeRtcPeerHandle platform_create(const std::string &) {
  return 0;
}

void platform_destroy(NativeRtcPeerHandle our_handle) {
  std::lock_guard<std::mutex> lock(espPeerMutex());
  auto it = espPeerTable().find(our_handle);
  if (it == espPeerTable().end()) return;
  auto *state = it->second;
  espPeerTable().erase(it);
  destroyPeerLocked(state);
}

void platform_add_track(NativeRtcPeerHandle our_handle, NativeMediaTrackHandle track) {
  auto *state = ensurePeer(our_handle);
  if (state) state->local_track = track;
}

std::string platform_create_offer(NativeRtcPeerHandle our_handle) {
  auto *state = ensurePeer(our_handle);
  if (!state) return std::string();
  int err = esp_peer_new_connection(state->handle);
  if (err != 0) {
    ESP_LOGE(kTag, "esp_peer_new_connection failed: %d", err);
    return std::string();
  }
  // Block up to 5 s for on_msg(SDP) to fire. Caller (JS code under await) is on a
  // different task than the main_loop_task running the callback.
  if (xSemaphoreTake(state->sdp_ready, pdMS_TO_TICKS(5000)) != pdTRUE) {
    ESP_LOGE(kTag, "create_offer SDP timeout");
    return std::string();
  }
  return state->pending_sdp;
}

std::string platform_create_answer(NativeRtcPeerHandle our_handle) {
  // esp_peer auto-generates the answer on first inbound offer through send_msg.
  // Block until on_msg fires with the locally-generated SDP.
  auto *state = ensurePeer(our_handle);
  if (!state) return std::string();
  if (xSemaphoreTake(state->sdp_ready, pdMS_TO_TICKS(5000)) != pdTRUE) {
    return std::string();
  }
  return state->pending_sdp;
}

void platform_set_local_description(NativeRtcPeerHandle, const std::string &, const std::string &) {
  // esp_peer treats the SDP returned from on_msg as already the local description.
  // No explicit set is required.
}

void platform_set_remote_description(NativeRtcPeerHandle our_handle, const std::string &, const std::string &sdp) {
  auto *state = findPeer(our_handle);
  if (!state || !state->handle) return;
  esp_peer_msg_t msg = {};
  msg.type = ESP_PEER_MSG_TYPE_SDP;
  msg.data = reinterpret_cast<uint8_t *>(const_cast<char *>(sdp.c_str()));
  msg.size = static_cast<int>(sdp.size());
  esp_peer_send_msg(state->handle, &msg);
}

void platform_add_ice_candidate(NativeRtcPeerHandle our_handle, const std::string &candidate, const std::string &, int) {
  auto *state = findPeer(our_handle);
  if (!state || !state->handle) return;
  esp_peer_msg_t msg = {};
  msg.type = ESP_PEER_MSG_TYPE_CANDIDATE;
  msg.data = reinterpret_cast<uint8_t *>(const_cast<char *>(candidate.c_str()));
  msg.size = static_cast<int>(candidate.size());
  esp_peer_send_msg(state->handle, &msg);
}

void platform_close(NativeRtcPeerHandle our_handle) {
  platform_destroy(our_handle);
}

}  // namespace gea::host::rtc

#endif  // ESP_PLATFORM

// SPDX-License-Identifier: Apache-2.0
#include "host/rtc.h"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace gea::host {

namespace {

struct PeerState {
  std::string connectionState = "new";
  std::string iceConnectionState = "new";
  bool open = true;
};

std::unordered_map<NativeRtcPeerHandle, PeerState> &peerTable() {
  static std::unordered_map<NativeRtcPeerHandle, PeerState> table;
  return table;
}

std::mutex &peerMutex() {
  static std::mutex m;
  return m;
}

std::atomic<NativeRtcPeerHandle> &nextHandle() {
  static std::atomic<NativeRtcPeerHandle> id{1};
  return id;
}

enum class EventKind { IceCandidate, Track, ConnectionStateChange, IceConnectionStateChange };

struct PendingEvent {
  NativeRtcPeerHandle handle = 0;
  EventKind kind = EventKind::ConnectionStateChange;
  std::string candidate;
  std::string sdp_mid;
  int sdp_mline_index = 0;
  NativeMediaTrackHandle track = 0;
};

std::mutex &queueMutex() {
  static std::mutex m;
  return m;
}

std::vector<PendingEvent> &eventQueue() {
  static std::vector<PendingEvent> q;
  return q;
}

void enqueue_internal(PendingEvent ev) {
  std::lock_guard<std::mutex> lock(queueMutex());
  eventQueue().push_back(std::move(ev));
}

}  // namespace

namespace rtc {

std::unordered_map<NativeRtcPeerHandle, CallbackTable> &callbackTable() {
  static std::unordered_map<NativeRtcPeerHandle, CallbackTable> table;
  return table;
}

#if defined(ESP_PLATFORM) && !defined(GEA_EMBEDDED_WIFI_DISABLED)
NativeRtcPeerHandle platform_create(const std::string &);
void platform_destroy(NativeRtcPeerHandle);
void platform_add_track(NativeRtcPeerHandle, NativeMediaTrackHandle);
std::string platform_create_offer(NativeRtcPeerHandle);
std::string platform_create_answer(NativeRtcPeerHandle);
void platform_set_local_description(NativeRtcPeerHandle, const std::string &, const std::string &);
void platform_set_remote_description(NativeRtcPeerHandle, const std::string &, const std::string &);
void platform_add_ice_candidate(NativeRtcPeerHandle, const std::string &, const std::string &, int);
void platform_close(NativeRtcPeerHandle);
#else
__attribute__((weak)) NativeRtcPeerHandle platform_create(const std::string &) { return 0; }
__attribute__((weak)) void platform_destroy(NativeRtcPeerHandle) {}
__attribute__((weak)) void platform_add_track(NativeRtcPeerHandle, NativeMediaTrackHandle) {}
__attribute__((weak)) std::string platform_create_offer(NativeRtcPeerHandle) { return "v=0\r\no=- 0 0 IN IP4 0.0.0.0\r\n"; }
__attribute__((weak)) std::string platform_create_answer(NativeRtcPeerHandle) { return "v=0\r\no=- 0 0 IN IP4 0.0.0.0\r\n"; }
__attribute__((weak)) void platform_set_local_description(NativeRtcPeerHandle, const std::string &, const std::string &) {}
__attribute__((weak)) void platform_set_remote_description(NativeRtcPeerHandle, const std::string &, const std::string &) {}
__attribute__((weak)) void platform_add_ice_candidate(NativeRtcPeerHandle, const std::string &, const std::string &, int) {}
__attribute__((weak)) void platform_close(NativeRtcPeerHandle) {}
#endif

NativeRtcPeerHandle create_handle() {
  return create_handle_with_config(std::string());
}

NativeRtcPeerHandle create_handle_with_config(const std::string &ice_servers_json) {
  auto handle = nextHandle()++;
  {
    std::lock_guard<std::mutex> lock(peerMutex());
    peerTable()[handle] = PeerState{};
  }
  platform_create(ice_servers_json);
  return handle;
}

void destroy_handle(NativeRtcPeerHandle handle) {
  platform_destroy(handle);
  std::lock_guard<std::mutex> lock(peerMutex());
  peerTable().erase(handle);
  callbackTable().erase(handle);
}

void runCallbacks() {
  std::vector<PendingEvent> drained;
  {
    std::lock_guard<std::mutex> lock(queueMutex());
    drained.swap(eventQueue());
  }
  for (const auto &ev : drained) {
    auto it = callbackTable().find(ev.handle);
    if (it == callbackTable().end()) continue;
    switch (ev.kind) {
      case EventKind::IceCandidate:
        if (it->second.on_ice_candidate) it->second.on_ice_candidate(ev.candidate, ev.sdp_mid, ev.sdp_mline_index);
        break;
      case EventKind::Track:
        if (it->second.on_track) it->second.on_track(ev.track);
        break;
      case EventKind::ConnectionStateChange:
        if (it->second.on_connection_state_change) it->second.on_connection_state_change();
        break;
      case EventKind::IceConnectionStateChange:
        if (it->second.on_ice_connection_state_change) it->second.on_ice_connection_state_change();
        break;
    }
  }
}

void enqueue_ice_candidate(NativeRtcPeerHandle h, const std::string &candidate, const std::string &sdp_mid, int idx) {
  enqueue_internal(PendingEvent{h, EventKind::IceCandidate, candidate, sdp_mid, idx, 0});
}
void enqueue_track(NativeRtcPeerHandle h, NativeMediaTrackHandle t) {
  enqueue_internal(PendingEvent{h, EventKind::Track, {}, {}, 0, t});
}
void enqueue_connection_state_change(NativeRtcPeerHandle h, const std::string &new_state) {
  {
    std::lock_guard<std::mutex> lock(peerMutex());
    auto it = peerTable().find(h);
    if (it != peerTable().end()) it->second.connectionState = new_state;
  }
  enqueue_internal(PendingEvent{h, EventKind::ConnectionStateChange, {}, {}, 0, 0});
}
void enqueue_ice_connection_state_change(NativeRtcPeerHandle h, const std::string &new_state) {
  {
    std::lock_guard<std::mutex> lock(peerMutex());
    auto it = peerTable().find(h);
    if (it != peerTable().end()) it->second.iceConnectionState = new_state;
  }
  enqueue_internal(PendingEvent{h, EventKind::IceConnectionStateChange, {}, {}, 0, 0});
}

}  // namespace rtc

std::string RTCPeerConnection::connectionState() const {
  std::lock_guard<std::mutex> lock(peerMutex());
  auto it = peerTable().find(nativeHandle);
  return it == peerTable().end() ? std::string("closed") : it->second.connectionState;
}

std::string RTCPeerConnection::iceConnectionState() const {
  std::lock_guard<std::mutex> lock(peerMutex());
  auto it = peerTable().find(nativeHandle);
  return it == peerTable().end() ? std::string("closed") : it->second.iceConnectionState;
}

void RTCPeerConnection::addTrack(MediaStreamTrack track, MediaStream) const {
  rtc::platform_add_track(nativeHandle, track.nativeHandle);
}

std::string RTCPeerConnection::createOffer() const {
  return rtc::platform_create_offer(nativeHandle);
}

std::string RTCPeerConnection::createAnswer() const {
  return rtc::platform_create_answer(nativeHandle);
}

void RTCPeerConnection::setLocalDescription(const std::string &sdp_type, const std::string &sdp) const {
  rtc::platform_set_local_description(nativeHandle, sdp_type, sdp);
}

void RTCPeerConnection::setRemoteDescription(const std::string &sdp_type, const std::string &sdp) const {
  rtc::platform_set_remote_description(nativeHandle, sdp_type, sdp);
}

void RTCPeerConnection::addIceCandidate(const std::string &candidate, const std::string &sdp_mid, double sdp_mline_index) const {
  rtc::platform_add_ice_candidate(nativeHandle, candidate, sdp_mid, static_cast<int>(sdp_mline_index));
}

void RTCPeerConnection::close() const {
  rtc::platform_close(nativeHandle);
  std::lock_guard<std::mutex> lock(peerMutex());
  auto it = peerTable().find(nativeHandle);
  if (it != peerTable().end()) {
    it->second.connectionState = "closed";
    it->second.iceConnectionState = "closed";
  }
}

}  // namespace gea::host

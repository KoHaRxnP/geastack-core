// SPDX-License-Identifier: Apache-2.0
#include "host/websocket.h"

#include <atomic>
#include <functional>
#include <mutex>
#include <unordered_map>
#include <utility>
#include <vector>

namespace gea::host {

namespace {

struct WebSocketInstance {
  std::string url;
  int readyState = WebSocket::CONNECTING;
};

std::unordered_map<NativeWebSocketHandle, WebSocketInstance> &instances() {
  static std::unordered_map<NativeWebSocketHandle, WebSocketInstance> map;
  return map;
}

std::mutex &instancesMutex() {
  static std::mutex m;
  return m;
}

std::atomic<NativeWebSocketHandle> &nextHandle() {
  static std::atomic<NativeWebSocketHandle> id{1};
  return id;
}

enum class EventKind { Open, Message, Close, Error };

struct PendingEvent {
  NativeWebSocketHandle handle = 0;
  EventKind kind = EventKind::Open;
  std::string text;
  int code = 0;
};

std::mutex &queueMutex() {
  static std::mutex m;
  return m;
}

std::vector<PendingEvent> &eventQueue() {
  static std::vector<PendingEvent> q;
  return q;
}

void enqueue(PendingEvent ev) {
  std::lock_guard<std::mutex> lock(queueMutex());
  eventQueue().push_back(std::move(ev));
}

}  // namespace

namespace websocket {

std::unordered_map<NativeWebSocketHandle, CallbackTable> &callbackTable() {
  static std::unordered_map<NativeWebSocketHandle, CallbackTable> table;
  return table;
}

#if defined(ESP_PLATFORM) && !defined(GEA_EMBEDDED_WIFI_DISABLED)
void platform_open(NativeWebSocketHandle, const std::string &);
void platform_send(NativeWebSocketHandle, const std::string &);
void platform_close(NativeWebSocketHandle);
void platform_destroy(NativeWebSocketHandle);
#else
__attribute__((weak)) void platform_open(NativeWebSocketHandle, const std::string &) {}
__attribute__((weak)) void platform_send(NativeWebSocketHandle, const std::string &) {}
__attribute__((weak)) void platform_close(NativeWebSocketHandle) {}
__attribute__((weak)) void platform_destroy(NativeWebSocketHandle) {}
#endif

NativeWebSocketHandle create_handle(const std::string &url) {
  auto handle = nextHandle()++;
  {
    std::lock_guard<std::mutex> lock(instancesMutex());
    instances()[handle] = WebSocketInstance{url, WebSocket::CONNECTING};
  }
  platform_open(handle, url);
  return handle;
}

void destroy_handle(NativeWebSocketHandle handle) {
  platform_destroy(handle);
  std::lock_guard<std::mutex> lock(instancesMutex());
  instances().erase(handle);
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
      case EventKind::Open:
        if (it->second.on_open) it->second.on_open();
        break;
      case EventKind::Message:
        if (it->second.on_message) it->second.on_message(ev.text);
        break;
      case EventKind::Close:
        if (it->second.on_close) it->second.on_close(ev.code, ev.text);
        break;
      case EventKind::Error:
        if (it->second.on_error) it->second.on_error(ev.text);
        break;
    }
  }
}

void test_inject_open(NativeWebSocketHandle handle) {
  enqueue(PendingEvent{handle, EventKind::Open, {}, 0});
}
void test_inject_message(NativeWebSocketHandle handle, const std::string &data) {
  enqueue(PendingEvent{handle, EventKind::Message, data, 0});
}
void test_inject_close(NativeWebSocketHandle handle, int code, const std::string &reason) {
  enqueue(PendingEvent{handle, EventKind::Close, reason, code});
}

void test_set_open_callback(NativeWebSocketHandle handle, std::function<void()> cb) {
  callbackTable()[handle].on_open = std::move(cb);
}
void test_set_message_callback(NativeWebSocketHandle handle, std::function<void(const std::string &)> cb) {
  callbackTable()[handle].on_message = std::move(cb);
}
void test_set_close_callback(NativeWebSocketHandle handle, std::function<void(int, const std::string &)> cb) {
  callbackTable()[handle].on_close = std::move(cb);
}
void test_set_error_callback(NativeWebSocketHandle handle, std::function<void(const std::string &)> cb) {
  callbackTable()[handle].on_error = std::move(cb);
}

}  // namespace websocket

std::string WebSocket::url() const {
  std::lock_guard<std::mutex> lock(instancesMutex());
  auto it = instances().find(nativeHandle);
  return it == instances().end() ? std::string{} : it->second.url;
}

double WebSocket::readyState() const {
  std::lock_guard<std::mutex> lock(instancesMutex());
  auto it = instances().find(nativeHandle);
  return it == instances().end() ? CLOSED : static_cast<double>(it->second.readyState);
}

void WebSocket::send(const std::string &data) const {
  websocket::platform_send(nativeHandle, data);
}

void WebSocket::close() const {
  websocket::platform_close(nativeHandle);
}

void WebSocket::close(double) const {
  websocket::platform_close(nativeHandle);
}

void WebSocket::close(double, const std::string &) const {
  websocket::platform_close(nativeHandle);
}

}  // namespace gea::host

#if defined(ESP_PLATFORM) && !defined(GEA_EMBEDDED_WIFI_DISABLED)

#include "esp_event.h"
#include "esp_log.h"
#include "esp_websocket_client.h"

namespace gea::host::websocket {

namespace {

struct EspWsState {
  esp_websocket_client_handle_t client = nullptr;
};

std::unordered_map<NativeWebSocketHandle, EspWsState> &espStateTable() {
  static std::unordered_map<NativeWebSocketHandle, EspWsState> table;
  return table;
}

void handleWsEvent(void *arg, esp_event_base_t, int32_t event_id, void *data) {
  const auto handle = static_cast<NativeWebSocketHandle>(reinterpret_cast<std::uintptr_t>(arg));
  switch (event_id) {
    case WEBSOCKET_EVENT_CONNECTED: {
      {
        std::lock_guard<std::mutex> lock(gea::host::instancesMutex());
        if (auto it = gea::host::instances().find(handle); it != gea::host::instances().end()) {
          it->second.readyState = WebSocket::OPEN;
        }
      }
      gea::host::enqueue(gea::host::PendingEvent{handle, gea::host::EventKind::Open, {}, 0});
      break;
    }
    case WEBSOCKET_EVENT_DATA: {
      const auto *event = static_cast<esp_websocket_event_data_t *>(data);
      if (event && event->op_code == 0x01 /* text frame */ && event->data_len > 0) {
        gea::host::enqueue(gea::host::PendingEvent{
            handle, gea::host::EventKind::Message,
            std::string(event->data_ptr, event->data_len), 0});
      }
      break;
    }
    case WEBSOCKET_EVENT_DISCONNECTED: {
      {
        std::lock_guard<std::mutex> lock(gea::host::instancesMutex());
        if (auto it = gea::host::instances().find(handle); it != gea::host::instances().end()) {
          it->second.readyState = WebSocket::CLOSED;
        }
      }
      gea::host::enqueue(gea::host::PendingEvent{handle, gea::host::EventKind::Close, std::string("closed"), 1000});
      break;
    }
    case WEBSOCKET_EVENT_ERROR: {
      gea::host::enqueue(gea::host::PendingEvent{handle, gea::host::EventKind::Error, std::string("error"), 0});
      break;
    }
  }
}

}  // namespace

void platform_open(NativeWebSocketHandle handle, const std::string &url) {
  esp_websocket_client_config_t config = {};
  config.uri = url.c_str();

  auto client = esp_websocket_client_init(&config);
  if (!client) {
    ESP_LOGE("gea::host::ws", "init failed for %s", url.c_str());
    return;
  }
  esp_websocket_register_events(
      client, WEBSOCKET_EVENT_ANY, handleWsEvent,
      reinterpret_cast<void *>(static_cast<std::uintptr_t>(handle)));
  esp_websocket_client_start(client);
  espStateTable()[handle] = EspWsState{client};
}

void platform_send(NativeWebSocketHandle handle, const std::string &data) {
  auto it = espStateTable().find(handle);
  if (it == espStateTable().end() || it->second.client == nullptr) return;
  esp_websocket_client_send_text(it->second.client, data.c_str(),
                                  static_cast<int>(data.size()), portMAX_DELAY);
}

void platform_close(NativeWebSocketHandle handle) {
  auto it = espStateTable().find(handle);
  if (it == espStateTable().end() || it->second.client == nullptr) return;
  esp_websocket_client_close(it->second.client, portMAX_DELAY);
}

void platform_destroy(NativeWebSocketHandle handle) {
  auto it = espStateTable().find(handle);
  if (it == espStateTable().end()) return;
  if (it->second.client) {
    esp_websocket_client_stop(it->second.client);
    esp_websocket_client_destroy(it->second.client);
  }
  espStateTable().erase(it);
}

}  // namespace gea::host::websocket

#endif  // ESP_PLATFORM && network capability

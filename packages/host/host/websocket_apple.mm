// SPDX-License-Identifier: Apache-2.0
// Link this provider on Apple desktop targets (Foundation, ARC, blocks).
#if defined(__APPLE__) && !defined(ESP_PLATFORM)
#include "host/websocket_platform.h"
#import <Foundation/Foundation.h>
#include <atomic>
#include <deque>
#include <mutex>
#include <unordered_map>

namespace gea::host::websocket::apple {
struct State;
}
@interface GeaWebSocketDelegate : NSObject <NSURLSessionWebSocketDelegate> {
@public
  std::weak_ptr<gea::host::websocket::apple::State> owner;
}
@end

namespace gea::host::websocket::apple {
constexpr std::size_t maxBuffered = 512 * 1024;
struct State : std::enable_shared_from_this<State> {
  NativeWebSocketHandle handle;
  std::mutex mutex;
  std::deque<std::string> outgoing;
  std::size_t bytes = 0;
  bool sending = false;
  bool opened = false;
  bool stopped = false;
  std::atomic<bool> terminal{false};
  TextProducer producer;
  NSURLSession *session = nil;
  NSURLSessionWebSocketTask *task = nil;
  GeaWebSocketDelegate *delegate = nil;
  dispatch_source_t timer = nil;
  explicit State(NativeWebSocketHandle h) : handle(h) {}
  ~State() { shutdown(); }

  void shutdown() {
    dispatch_source_t oldTimer;
    NSURLSession *oldSession;
    TextProducer retired;
    {
      std::lock_guard lock(mutex);
      stopped = true;
      retired = std::move(producer);
      outgoing.clear();
      bytes = 0;
      oldTimer = timer;
      timer = nil;
      oldSession = session;
      session = nil;
    }
    retired = {};
    if (oldTimer)
      dispatch_source_cancel(oldTimer);
    [oldSession invalidateAndCancel];
  }
  void failed(const char *message) {
    TextProducer retired;
    const auto code = task.closeCode;
    if (code != NSURLSessionWebSocketCloseCodeInvalid) {
      {
        std::lock_guard lock(mutex);
        stopped = true;
        retired = std::move(producer);
        outgoing.clear();
        bytes = 0;
      }
      retired = {};
      NSData *reason = task.closeReason;
      terminal.store(true, std::memory_order_release);
      transport_close(handle, static_cast<int>(code),
                      reason.length
                          ? std::string(static_cast<const char *>(reason.bytes),
                                        reason.length)
                          : std::string{});
      shutdown();
      return;
    }
    bool alreadyStopped;
    {
      std::lock_guard lock(mutex);
      alreadyStopped = stopped;
      stopped = true;
      retired = std::move(producer);
      outgoing.clear();
      bytes = 0;
    }
    retired = {};
    if (alreadyStopped) {
      // Cancelling during the handshake may complete without didClose.
      terminal.store(true, std::memory_order_release);
      transport_close(handle, 1000, "WebSocket closed locally");
      shutdown();
      return;
    }
    terminal.store(true, std::memory_order_release);
    transport_error(handle, message);
    transport_close(handle, 1006, "WebSocket transport failed");
    shutdown();
  }
  bool enqueue(std::string text) {
    bool overflow = false;
    {
      std::lock_guard lock(mutex);
      if (stopped || !opened)
        return false;
      overflow = outgoing.size() + (sending ? 1 : 0) >= 1024 ||
                 text.size() > maxBuffered - bytes;
      if (!overflow) {
        bytes += text.size();
        outgoing.push_back(std::move(text));
      }
    }
    if (overflow) {
      failed("WebSocket send queue exceeds 512 KiB");
      return false;
    }
    pump();
    return true;
  }
  void pump() {
    std::string text;
    {
      std::lock_guard lock(mutex);
      if (stopped || sending || !opened || outgoing.empty())
        return;
      sending = true;
      text = std::move(outgoing.front());
      outgoing.pop_front();
    }
    NSString *value = [[NSString alloc] initWithBytes:text.data()
                                               length:text.size()
                                             encoding:NSUTF8StringEncoding];
    if (!value) {
      failed("WebSocket text must be valid UTF-8");
      return;
    }
    const auto length = text.size();
    std::weak_ptr<State> weak = shared_from_this();
    [task sendMessage:[[NSURLSessionWebSocketMessage alloc]
                          initWithString:value]
        completionHandler:^(NSError *error) {
          if (auto state = weak.lock()) {
            if (error) {
              state->failed("WebSocket send failed");
              return;
            }
            {
              std::lock_guard lock(state->mutex);
              if (state->stopped)
                return;
              state->bytes -= length;
              state->sending = false;
            }
            state->pump();
          }
        }];
  }
  void receive() {
    {
      std::lock_guard lock(mutex);
      if (stopped)
        return;
    }
    std::weak_ptr<State> weak = shared_from_this();
    [task receiveMessageWithCompletionHandler:^(
              NSURLSessionWebSocketMessage *message, NSError *error) {
      if (auto state = weak.lock()) {
        {
          std::lock_guard lock(state->mutex);
          if (state->stopped)
            return;
        }
        if (error) {
          state->failed("WebSocket receive failed");
          return;
        }
        if (message.type == NSURLSessionWebSocketMessageTypeString) {
          NSData *utf8 =
              [message.string dataUsingEncoding:NSUTF8StringEncoding];
          transport_message(
              state->handle,
              utf8.length ? std::string(static_cast<const char *>(utf8.bytes),
                                        utf8.length)
                          : std::string{});
        }
        state->receive(); // The public facade exposes text messages only.
      }
    }];
  }
  void produce() {
    TextProducer next;
    {
      std::lock_guard lock(mutex);
      if (stopped || !opened)
        return;
      next = producer;
    }
    if (!next)
      return;
    try {
      auto text = next();
      if (!text.empty())
        enqueue(std::move(text));
    } catch (...) {
      failed("WebSocket text producer failed");
    }
  }
};
std::mutex tableMutex;
std::unordered_map<NativeWebSocketHandle, std::shared_ptr<State>> table;
std::shared_ptr<State> find(NativeWebSocketHandle handle) {
  std::lock_guard lock(tableMutex);
  auto it = table.find(handle);
  return it == table.end() ? nullptr : it->second;
}
} // namespace gea::host::websocket::apple

@implementation GeaWebSocketDelegate
- (void)URLSession:(NSURLSession *)session
          webSocketTask:(NSURLSessionWebSocketTask *)task
    didOpenWithProtocol:(NSString *)protocol {
  if (auto state = owner.lock()) {
    {
      std::lock_guard lock(state->mutex);
      if (state->stopped)
        return;
      state->opened = true;
    }
    if (gea::host::websocket::transport_open(state->handle)) {
      state->receive();
      state->pump();
    }
  }
}
- (void)URLSession:(NSURLSession *)session
       webSocketTask:(NSURLSessionWebSocketTask *)task
    didCloseWithCode:(NSURLSessionWebSocketCloseCode)code
              reason:(NSData *)reason {
  if (auto state = owner.lock()) {
    gea::host::websocket::TextProducer retired;
    {
      std::lock_guard lock(state->mutex);
      state->stopped = true;
      retired = std::move(state->producer);
      state->outgoing.clear();
      state->bytes = 0;
    }
    retired = {};
    std::string text;
    if (reason.length)
      text.assign(static_cast<const char *>(reason.bytes), reason.length);
    state->terminal.store(true, std::memory_order_release);
    gea::host::websocket::transport_close(state->handle, static_cast<int>(code),
                                          text);
    state->shutdown();
  }
}
- (void)URLSession:(NSURLSession *)session
                    task:(NSURLSessionTask *)task
    didCompleteWithError:(NSError *)error {
  if (error)
    if (auto state = owner.lock())
      state->failed("WebSocket connection failed");
}
@end

namespace gea::host::websocket {
void platform_open(NativeWebSocketHandle handle, const std::string &url,
                   const std::string &protocols, const std::string &headers) {
  @autoreleasepool {
    NSString *raw = [[NSString alloc] initWithBytes:url.data()
                                             length:url.size()
                                           encoding:NSUTF8StringEncoding];
    NSURLComponents *parts =
        raw ? [NSURLComponents componentsWithString:raw] : nil;
    if (!parts ||
        (![parts.scheme isEqualToString:@"ws"] &&
         ![parts.scheme isEqualToString:@"wss"]) ||
        !parts.host.length || parts.user || parts.password || parts.fragment ||
        !parts.URL || url.find('\0') != std::string::npos ||
        protocols.find_first_of("\r\n") != std::string::npos ||
        protocols.find('\0') != std::string::npos) {
      transport_error(handle, "Invalid WebSocket URL or protocols");
      transport_close(handle, 1006, "WebSocket connection rejected");
      return;
    }
    NSMutableURLRequest *request =
        [NSMutableURLRequest requestWithURL:parts.URL];
    request.timeoutInterval = 10;
    if (!protocols.empty())
      [request setValue:[NSString stringWithUTF8String:protocols.c_str()]
          forHTTPHeaderField:@"Sec-WebSocket-Protocol"];
    std::size_t offset = 0;
    while (offset < headers.size()) {
      const auto end = headers.find("\r\n", offset);
      const auto colon = headers.find(':', offset);
      if (end == std::string::npos || colon == std::string::npos ||
          colon >= end || colon == offset) {
        transport_error(handle, "Invalid WebSocket header");
        transport_close(handle, 1006, "WebSocket connection rejected");
        return;
      }
      const auto name = headers.substr(offset, colon - offset);
      const auto value = headers.substr(colon + 1, end - colon - 1);
      if (name.find_first_not_of("!#$%&'*+-.^_`|~"
                                 "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghi"
                                 "jklmnopqrstuvwxyz") != std::string::npos ||
          value.find_first_of("\r\n") != std::string::npos ||
          value.find('\0') != std::string::npos) {
        transport_error(handle, "Invalid WebSocket header");
        transport_close(handle, 1006, "WebSocket connection rejected");
        return;
      }
      NSString *headerValue =
          [[NSString alloc] initWithBytes:value.data()
                                   length:value.size()
                                 encoding:NSUTF8StringEncoding];
      if (!headerValue) {
        transport_error(handle, "Invalid WebSocket header encoding");
        transport_close(handle, 1006, "WebSocket connection rejected");
        return;
      }
      [request setValue:headerValue
          forHTTPHeaderField:[NSString stringWithUTF8String:name.c_str()]];
      offset = end + 2;
    }
    auto state = std::make_shared<apple::State>(handle);
    state->delegate = [GeaWebSocketDelegate new];
    state->delegate->owner = state;
    auto configuration =
        [NSURLSessionConfiguration ephemeralSessionConfiguration];
    configuration.HTTPCookieStorage = nil;
    configuration.URLCredentialStorage = nil;
    auto queue = [NSOperationQueue new];
    queue.maxConcurrentOperationCount = 1;
    state->session = [NSURLSession sessionWithConfiguration:configuration
                                                   delegate:state->delegate
                                              delegateQueue:queue];
    state->task = [state->session webSocketTaskWithRequest:request];
    state->task.maximumMessageSize = 1024 * 1024;
    state->timer = dispatch_source_create(
        DISPATCH_SOURCE_TYPE_TIMER, 0, 0,
        dispatch_queue_create("gea.websocket.producer", DISPATCH_QUEUE_SERIAL));
    std::weak_ptr<apple::State> weak = state;
    dispatch_source_set_event_handler(state->timer, ^{
      if (auto current = weak.lock())
        current->produce();
    });
    dispatch_source_set_timer(state->timer,
                              dispatch_time(DISPATCH_TIME_NOW, 10000000),
                              10000000, 1000000);
    {
      std::lock_guard lock(apple::tableMutex);
      apple::table.emplace(handle, state);
    }
    dispatch_resume(state->timer);
    [state->task resume];
  }
}
std::size_t platform_buffered(NativeWebSocketHandle handle) {
  auto state = apple::find(handle);
  if (!state)
    return 0;
  std::lock_guard lock(state->mutex);
  return state->bytes;
}
void platform_send(NativeWebSocketHandle handle, const std::string &text) {
  auto state = apple::find(handle);
  if (!state || !state->enqueue(text))
    throw std::runtime_error("WebSocket is not open or send queue is full");
}
void platform_close(NativeWebSocketHandle handle) {
  auto state = apple::find(handle);
  if (!state)
    return;
  TextProducer retired;
  {
    std::lock_guard lock(state->mutex);
    if (state->stopped)
      return;
    state->stopped = true;
    retired = std::move(state->producer);
  }
  retired = {};
  transport_closing(handle);
  [state->task cancelWithCloseCode:NSURLSessionWebSocketCloseCodeNormalClosure
                            reason:nil];
}
void platform_destroy(NativeWebSocketHandle handle) {
  std::shared_ptr<apple::State> state;
  {
    std::lock_guard lock(apple::tableMutex);
    auto it = apple::table.find(handle);
    if (it == apple::table.end())
      return;
    state = std::move(it->second);
    apple::table.erase(it);
  }
  state->shutdown();
}
void platform_reap() {
  std::vector<std::shared_ptr<apple::State>> retired;
  {
    std::lock_guard lock(apple::tableMutex);
    for (auto it = apple::table.begin(); it != apple::table.end();) {
      if (it->second->terminal.load(std::memory_order_acquire)) {
        retired.push_back(std::move(it->second));
        it = apple::table.erase(it);
      } else
        ++it;
    }
  }
  // Facade instances retain CLOSED/url metadata; native transport ownership
  // ends.
  for (auto &state : retired)
    state->shutdown();
}
namespace apple {
std::size_t transport_count_for_test() {
  std::lock_guard lock(tableMutex);
  return table.size();
}
} // namespace apple
void platform_set_producer(NativeWebSocketHandle handle,
                           TextProducer producer) {
  auto state = apple::find(handle);
  if (!state)
    throw std::invalid_argument("Closed WebSocket producer");
  TextProducer retired;
  {
    std::lock_guard lock(state->mutex);
    if (state->stopped)
      throw std::invalid_argument("Closed WebSocket producer");
    retired = std::move(state->producer);
    state->producer = std::move(producer);
  }
}
} // namespace gea::host::websocket
#endif

// SPDX-License-Identifier: Apache-2.0
#include "host/websocket.h"
#include "host/worker.h"
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <thread>
using namespace gea::host;
#if defined(__APPLE__) && !defined(GEA_TEST_CURL_WEBSOCKET)
namespace gea::host::websocket::apple {
std::size_t transport_count_for_test();
}
namespace provider = gea::host::websocket::apple;
#else
namespace gea::host::websocket::curl_transport {
std::size_t transport_count_for_test();
}
namespace provider = gea::host::websocket::curl_transport;
#if defined(GEA_TEST_CURL_PARTIAL_SEND)
namespace gea::host::websocket::curl_transport {
unsigned partial_send_count_for_test();
}
#endif
#endif
struct QueryOnRelease {
  NativeWebSocketHandle handle;
  std::atomic<int> *count;
  QueryOnRelease(NativeWebSocketHandle h, std::atomic<int> *c)
      : handle(h), count(c) {}
  ~QueryOnRelease() {
    WebSocket socket(handle);
    (void)socket.bufferedAmount();
    (void)socket.url();
    (void)socket.readyState();
    try {
      websocket::setTextProducer(handle, {});
    } catch (const std::invalid_argument &) {
    }
    ++*count;
  }
};
static void waitFor(const std::function<bool()> &done) {
  auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(8);
  while (!done() && std::chrono::steady_clock::now() < deadline) {
    websocket::runCallbacks();
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  assert(done());
}
int main(int argc, char **argv) {
  assert(argc == 2);
  assert(provider::transport_count_for_test() == 0);
  const auto thread = std::this_thread::get_id();
  auto h =
      websocket::create_handle(std::string(argv[1]), std::string("gea-test"),
                               std::string("X-Gea-Test: loopback\r\n"));
  WebSocket ws(h);
  bool opened = false, closed = false, errored = false;
  int messages = 0;
  std::atomic<int> sinks{0};
  std::atomic<int> nativeReleases{0}, setterReleases{0}, producerReleases{0};
  auto nativeOwner = std::make_shared<QueryOnRelease>(h, &nativeReleases);
  std::weak_ptr<QueryOnRelease> nativeOwnerWeak = nativeOwner;
  auto replacedSink = std::make_shared<QueryOnRelease>(h, &setterReleases);
  websocket::setTextConsumer(
      h, [replacedSink](const std::string &) { return false; });
  replacedSink.reset();
  websocket::setTextConsumer(h, [&, nativeOwner](const std::string &s) {
    if (s == "sink") {
      ++sinks;
      return true;
    }
    return false;
  });
  assert(setterReleases.load() == 1);
  nativeOwner.reset();
  websocket::test_set_open_callback(h, [&] {
    assert(std::this_thread::get_id() == thread &&
           ws.readyState() == WebSocket::OPEN);
    opened = true;
    ws.send("hello");
  });
  websocket::test_set_message_callback(h, [&](const std::string &s) {
    assert(std::this_thread::get_id() == thread);
    if (messages == 0)
      assert(s == "fragmented text");
    else if (messages == 1)
      assert(s == std::string(70000, 'x'));
    else if (messages == 2)
      assert(s.empty());
    else
      assert(false);
    ++messages;
  });
  websocket::test_set_error_callback(
      h, [&](const std::string &) { errored = true; });
  websocket::test_set_close_callback(h, [&](int code, const std::string &) {
    assert(std::this_thread::get_id() == thread);
    assert(code == 1000);
    closed = true;
  });
  waitFor([&] { return closed; });
  assert(opened && !errored && messages == 3 && sinks == 1 &&
         ws.readyState() == WebSocket::CLOSED);
  assert(ws.bufferedAmount() == 0);
  assert(nativeOwnerWeak.expired() && nativeReleases.load() == 1);
  bool refused = false;
  try {
    ws.send("late");
  } catch (...) {
    refused = true;
  }
  assert(refused);
  websocket::runCallbacks();
  waitFor([] { return provider::transport_count_for_test() == 0; });
  assert(ws.url() == argv[1] && ws.readyState() == WebSocket::CLOSED);
  // Deliberately retain the closed facade while reconnecting: no
  // destroy_handle. Native producer runs off the owner thread; locally
  // initiated close settles.
  h = websocket::create_handle(std::string(argv[1]) + "/local");
  ws = WebSocket(h);
  opened = closed = errored = false;
  auto producerOwner = std::make_shared<QueryOnRelease>(h, &producerReleases);
  websocket::setTextProducer(
      h, [producerOwner] { return std::string("produced"); });
  producerOwner.reset();
  websocket::test_set_open_callback(h, [&] { opened = true; });
  websocket::test_set_message_callback(h, [&](const std::string &s) {
    assert(s == "ready");
    websocket::setTextProducer(h, {});
    ws.close();
    assert(ws.readyState() == WebSocket::CLOSING);
  });
  websocket::test_set_error_callback(
      h, [&](const std::string &) { errored = true; });
  websocket::test_set_close_callback(h, [&](int code, const std::string &) {
    assert(code == 1000);
    closed = true;
  });
  waitFor([&] { return closed; });
  assert(opened && !errored);
  waitFor([&] { return producerReleases.load() == 1; });
  websocket::runCallbacks();
  waitFor([] { return provider::transport_count_for_test() == 0; });
  // Deliberately no explicit destroy on this closed connection either.
  // A real rejected upgrade produces error and terminal close, never onopen.
  h = websocket::create_handle(std::string(argv[1]) + "/reject");
  ws = WebSocket(h);
  opened = closed = errored = false;
  websocket::test_set_open_callback(h, [&] { opened = true; });
  websocket::test_set_error_callback(
      h, [&](const std::string &) { errored = true; });
  websocket::test_set_close_callback(h, [&](int code, const std::string &) {
    assert(code == 1006);
    closed = true;
  });
  waitFor([&] { return closed; });
  assert(!opened && errored);
  websocket::destroy_handle(h);
  // Terminal producer retirement may reenter provider/facade query methods.
  std::atomic<int> closeProducerReleases{0};
  h = websocket::create_handle(std::string(argv[1]) + "/producer-close");
  ws = WebSocket(h);
  closed = errored = false;
  auto closingProducer =
      std::make_shared<QueryOnRelease>(h, &closeProducerReleases);
  websocket::setTextProducer(
      h, [closingProducer] { return std::string("closing-produced"); });
  closingProducer.reset();
  websocket::test_set_message_callback(h, [&](const std::string &s) {
    assert(s == "ready");
    ws.close();
  });
  websocket::test_set_error_callback(
      h, [&](const std::string &) { errored = true; });
  websocket::test_set_close_callback(h, [&](int code, const std::string &) {
    assert(code == 1000);
    closed = true;
  });
  waitFor([&] { return closed; });
  assert(!errored);
  waitFor([] { return provider::transport_count_for_test() == 0; });
  assert(closeProducerReleases.load() == 1);
  websocket::destroy_handle(h);
#if defined(GEA_TEST_CURL_PARTIAL_SEND)
  // Close during a genuinely backpressured send must finish that text frame
  // before emitting CLOSE, not inject close bytes into its remaining payload.
  const auto partialBefore = provider::partial_send_count_for_test();
  h = websocket::create_handle(std::string(argv[1]) + "/partial");
  ws = WebSocket(h);
  closed = errored = false;
  websocket::test_set_open_callback(h, [&] {
    ws.send(std::string(500000, 'p'));
    ws.close();
  });
  websocket::test_set_error_callback(
      h, [&](const std::string &) { errored = true; });
  websocket::test_set_close_callback(h, [&](int code, const std::string &) {
    assert(code == 1000);
    closed = true;
  });
  waitFor([&] { return closed; });
  assert(!errored);
  waitFor([] { return provider::transport_count_for_test() == 0; });
  assert(provider::partial_send_count_for_test() > partialBefore);
  std::printf("actual curl short/AGAIN sends: %u\n",
              provider::partial_send_count_for_test() - partialBefore);
  websocket::destroy_handle(h);
#endif
  // Cancellation before the upgrade cannot remain stuck in CLOSING.
  h = websocket::create_handle(std::string(argv[1]) + "/cancel");
  ws = WebSocket(h);
  closed = false;
  websocket::test_set_open_callback(h, [&] { assert(false); });
  websocket::test_set_close_callback(h, [&](int code, const std::string &) {
    assert(code == 1000);
    closed = true;
  });
  ws.close();
  waitFor([&] { return closed; });
  assert(ws.readyState() == WebSocket::CLOSED);
  websocket::destroy_handle(h);
  websocket::runCallbacks();
  waitFor([] { return provider::transport_count_for_test() == 0; });
  // Explicit destruction also releases native captures outside instance locks.
  std::atomic<int> destroyReleases{0};
  h = websocket::create_handle(std::string(argv[1]) + "/destroy");
  auto destroyedOwner = std::make_shared<QueryOnRelease>(h, &destroyReleases);
  websocket::setTextConsumer(
      h, [destroyedOwner](const std::string &) { return false; });
  destroyedOwner.reset();
  websocket::destroy_handle(h);
  assert(destroyReleases.load() == 1);
  // Invalid schemes terminate rather than leaving a fake CONNECTING socket.
  h = websocket::create_handle("http://127.0.0.1/");
  ws = WebSocket(h);
  int errors = 0, closes = 0;
  websocket::test_set_error_callback(h, [&](const std::string &) { ++errors; });
  websocket::test_set_close_callback(h, [&](int code, const std::string &) {
    assert(code == 1006);
    ++closes;
  });
  waitFor([&] { return closes == 1; });
  assert(errors == 1 && ws.readyState() == WebSocket::CLOSED);
  websocket::destroy_handle(h);
  // Destroy before queued rejection: owner event must not invoke a dead
  // callback.
  h = websocket::create_handle("http://127.0.0.1/");
  websocket::test_set_error_callback(
      h, [&](const std::string &) { assert(false); });
  websocket::destroy_handle(h);
  websocket::runCallbacks();
  std::puts("desktop websocket real loopback "
            "lifecycle/text/fragment/ping/large/native sink/owner "
            "dispatch/refusal/disposal PASS");
}

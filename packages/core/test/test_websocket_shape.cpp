#include "host/websocket.h"

#include <cassert>
#include <cstdio>
#include <string>
#include <vector>

extern std::string fake_ws_headers;

int main() {
  static_assert(gea::host::WebSocket::CONNECTING == 0);
  static_assert(gea::host::WebSocket::OPEN == 1);
  static_assert(gea::host::WebSocket::CLOSING == 2);
  static_assert(gea::host::WebSocket::CLOSED == 3);

  auto handle = gea::host::websocket::create_handle(std::string("ws://test/"));
  gea::host::WebSocket ws(handle);

  assert(ws.url() == "ws://test/");
  assert(ws.readyState() == gea::host::WebSocket::CONNECTING);

  struct Header { std::string name; std::string value; };
  struct Options { std::vector<Header> headers; };
  const Options options{{{"Authorization", "Bearer test-token"}}};
  const auto authenticated = gea::host::websocket::create_handle(
      std::string("wss://test/"), std::string(), options);
  assert(fake_ws_headers == "Authorization: Bearer test-token\r\n");
  gea::host::websocket::destroy_handle(authenticated);

  const auto withProtocols = gea::host::websocket::create_handle(
      std::string("wss://test/"), std::vector<std::string>{"example"}, options);
  assert(fake_ws_headers == "Authorization: Bearer test-token\r\n");
  gea::host::websocket::destroy_handle(withProtocols);

  struct RefOptions { std::shared_ptr<std::vector<Header>> headers; };
  const auto referenced = std::make_shared<RefOptions>(RefOptions{
      std::make_shared<std::vector<Header>>(options.headers)});
  const auto withReferences = gea::host::websocket::create_handle(
      std::string("wss://test/"), "", referenced);
  assert(fake_ws_headers == "Authorization: Bearer test-token\r\n");
  gea::host::websocket::destroy_handle(withReferences);

  bool rejected = false;
  try {
    gea::host::websocket::create_handle(std::string("wss://test/"), std::string(),
        Options{{{"Authorization", "Bearer token\r\nInjected: value"}}});
  } catch (const std::invalid_argument &) {
    rejected = true;
  }
  assert(rejected);

  std::puts("websocket shape OK");
  return 0;
}

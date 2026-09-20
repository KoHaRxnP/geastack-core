#include "host/websocket.h"

#include <cassert>
#include <cstdio>
#include <string>

int main() {
  static_assert(gea::host::WebSocket::CONNECTING == 0);
  static_assert(gea::host::WebSocket::OPEN == 1);
  static_assert(gea::host::WebSocket::CLOSING == 2);
  static_assert(gea::host::WebSocket::CLOSED == 3);

  auto handle = gea::host::websocket::create_handle(std::string("ws://test/"));
  gea::host::WebSocket ws(handle);

  assert(ws.url() == "ws://test/");
  assert(ws.readyState() == gea::host::WebSocket::CONNECTING);

  std::puts("websocket shape OK");
  return 0;
}

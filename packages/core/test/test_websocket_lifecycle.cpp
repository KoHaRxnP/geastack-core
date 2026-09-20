#include "host/websocket.h"

#include <cassert>
#include <cstdio>
#include <string>
#include <vector>

struct FakeWsRecord {
  std::string url;
  std::vector<std::string> sends;
  bool closed = false;
};
extern std::vector<FakeWsRecord> fake_ws_records;
extern void fake_ws_reset();

int main() {
  fake_ws_reset();

  auto handle = gea::host::websocket::create_handle(std::string("ws://test/"));
  gea::host::WebSocket ws(handle);

  assert(fake_ws_records.size() == 1);
  assert(fake_ws_records[0].url == "ws://test/");
  assert(ws.url() == "ws://test/");
  assert(ws.readyState() == gea::host::WebSocket::CONNECTING);

  ws.send(std::string("hello"));
  ws.send(std::string("world"));

  assert(fake_ws_records[0].sends.size() == 2);
  assert(fake_ws_records[0].sends[0] == "hello");
  assert(fake_ws_records[0].sends[1] == "world");

  ws.close();
  assert(fake_ws_records[0].closed);

  gea::host::websocket::destroy_handle(handle);

  std::puts("websocket lifecycle OK");
  return 0;
}

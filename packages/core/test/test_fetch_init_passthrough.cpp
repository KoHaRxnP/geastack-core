#include "host/fetch.h"

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

extern std::string fake_last_method;
extern std::string fake_last_url;
extern std::string fake_last_body;
extern std::map<std::string, std::string> fake_last_headers;

int main() {
  gea::host::FetchRequestInit init;
  init.method = "POST";
  init.headers["content-type"] = "application/json";
  init.body = R"({"sdp":"v=0..."})";

  auto response = gea::host::fetch("http://signaling.test/offer", init);

  assert(fake_last_method == "POST");
  assert(fake_last_url == "http://signaling.test/offer");
  assert(fake_last_body == init.body);
  assert(fake_last_headers.at("content-type") == "application/json");

  assert(response.ok);
  assert(response.status == 200);
  assert(response.status_text == "OK");
  assert(response.headers.at("content-type") == "application/json");

  std::vector<std::uint8_t> binary_body = {0, 1, 2, 253, 254, 255};
  init.setBody(binary_body);
  response = gea::host::fetch("http://signaling.test/upload", init);

  assert(fake_last_url == "http://signaling.test/upload");
  assert(fake_last_body.size() == binary_body.size());
  for (std::size_t i = 0; i < binary_body.size(); i++) {
    assert(static_cast<unsigned char>(fake_last_body[i]) == binary_body[i]);
  }
  assert(response.ok);

  std::puts("fetch init passthrough OK");
  return 0;
}

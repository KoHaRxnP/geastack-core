// Test-only fake for the non-ESP fetch path. Captures the last request
// for assertion; returns a canned 200 OK with a content-type header.

#include "host/fetch.h"

#include <map>
#include <string>

std::string fake_last_method;
std::string fake_last_url;
std::string fake_last_body;
std::map<std::string, std::string> fake_last_headers;

namespace gea::framework::host {

void test_record_request(const std::string &url, const gea::host::FetchRequestInit &init) {
  fake_last_url = url;
  fake_last_method = init.method;
  fake_last_body = init.body;
  fake_last_headers = init.headers;
}

gea::host::FetchResponse test_canned_response(const std::string &) {
  gea::host::FetchResponse response;
  response.ok = true;
  response.status = 200;
  response.status_text = "OK";
  response.headers["content-type"] = "application/json";
  response.body = {'{', '}'};
  return response;
}

}  // namespace gea::framework::host

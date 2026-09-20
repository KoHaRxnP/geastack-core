#include "host/fetch.h"
#include <cassert>
#include <cstdio>

int main() {
  gea::host::FetchResponse response;
  response.ok = true;
  response.status = 204;
  response.status_text = "No Content";
  response.headers["content-type"] = "application/json";
  response.body = {'{', '}'};

  assert(response.ok);
  assert(response.status == 204);
  assert(response.status_text == "No Content");
  assert(response.headers.at("content-type") == "application/json");
  assert(response.body.size() == 2);

  gea::host::FetchRequestInit init;
  init.method = "POST";
  init.headers["accept"] = "application/json";
  init.body = "{\"hello\":\"world\"}";

  assert(init.method == "POST");
  assert(init.headers.at("accept") == "application/json");
  assert(init.body == "{\"hello\":\"world\"}");

  std::puts("fetch response shape OK");
  return 0;
}

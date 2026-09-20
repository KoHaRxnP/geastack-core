// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <type_traits>
#include <vector>

namespace gea::host {

using FetchHeaderMap = std::map<std::string, std::string>;

struct FetchResponse {
  bool ok = false;
  double status = 0;
  std::string status_text;
  FetchHeaderMap headers;
  std::vector<std::uint8_t> body;

  std::string text() const {
    return std::string(reinterpret_cast<const char *>(body.data()), body.size());
  }

  // Raw response body. Both spellings return the bytes by value so the typed
  // `await fetch(url).bytes()` / `.arrayBuffer()` flow straight into
  // `loadImage(Uint8Array)` (which lowers to `std::vector<std::uint8_t>`).
  std::vector<std::uint8_t> bytes() const {
    return body;
  }

  std::vector<std::uint8_t> arrayBuffer() const {
    return body;
  }

#ifdef GEA_CPP_VALUE_AVAILABLE
  gea_cpp_value __gea_to_value() const {
    gea_cpp_value out = gea_cpp_value::empty_record();
    out.record_set_literal("ok", gea_cpp_key(ok));
    out.record_set_literal("status", gea_cpp_key(status));
    out.record_set_literal("statusText", gea_cpp_key(status_text));
    out.record_set_literal("status_text", gea_cpp_key(status_text));

    gea_cpp_value header_record = gea_cpp_value::empty_record();
    for (const auto &kv : headers) {
      header_record.record_set_literal(kv.first.c_str(), gea_cpp_key(kv.second));
    }
    out.record_set_literal("headers", header_record);

    out.record_set_literal("text", gea_cpp_value(std::function<std::string()>([body_text = text()]() {
      return body_text;
    })));
    return out;
  }
#endif
};

struct FetchRequestInit {
  std::string method = "GET";
  FetchHeaderMap headers;
  std::string body;
  // Per-request timeout override in ms; 0 = use the builder default. Callers
  // that retry (e.g. the tile loader) set this short so a transient link stall
  // fails fast and is retried, instead of waiting out a ~15s TCP retransmit.
  int timeout_ms = 0;

  void setBody(const std::string &value) {
    body = value;
  }

  void setBody(const std::vector<std::uint8_t> &value) {
    body.assign(reinterpret_cast<const char *>(value.data()), value.size());
  }
};

FetchResponse fetch(const std::string &url);
FetchResponse fetch(const std::string &url, const FetchRequestInit &init);
// Synchronous streaming multipart upload (see fetchUploadFileAsync). Runs on the
// caller's task; fetchUploadFileAsync drives it from a background task.
FetchResponse uploadFile(const std::string &url, const std::string &authorization,
                         const std::string &contentType, const std::string &prefix,
                         const std::string &filePath, const std::string &suffix);
// Synchronous streaming download: GET `url` and write the body to `destPath` in
// chunks (never buffered whole in RAM). `authorization` sets the Authorization
// header (pass "" to skip). Runs on the caller's task; fetchDownloadFileAsync
// drives it from a background task.
FetchResponse downloadFile(const std::string &url, const std::string &authorization,
                           const std::string &destPath);
double fetchAsync(const std::string &url);
double fetchAsync(const std::string &url, const FetchRequestInit &init);
bool fetchReady(double id);
FetchResponse fetchResult(double id);
void fetchRelease(double id);

// Streaming multipart file upload (POST). The request body is sent as
// `prefix` + <`filePath` contents, streamed from disk in chunks> + `suffix`, so
// a large file (e.g. a long WAV) is uploaded without ever loading it into RAM.
// `authorization` / `contentType` set those two request headers (pass "" to
// skip). Async: returns a job id, polled/collected with the same
// fetchReady / fetchResult / fetchRelease as fetchAsync.
double fetchUploadFileAsync(const std::string &url, const std::string &authorization,
                            const std::string &contentType, const std::string &prefix,
                            const std::string &filePath, const std::string &suffix);
// Streaming download (GET) to a device file path, written in chunks off the
// network worker. Async: returns a job id, polled with fetchReady / fetchResult
// / fetchRelease like fetchAsync. Progress shares the fetchUploadProgress /
// fetchUploadSent / fetchUploadTotal counters (one transfer runs at a time).
double fetchDownloadFileAsync(const std::string &url, const std::string &authorization,
                              const std::string &destPath);

// Upload progress for the single in-flight streaming upload (Sync uploads one
// note at a time). The streamer reports bytes via uploadProgressReset/Add; JS
// reads the fraction via fetchUploadProgress. Header-only (function-local
// statics share one instance across TUs) so no namespace-placement juggling.
inline std::atomic<long long> &__uploadSentBytes() {
  static std::atomic<long long> v{0};
  return v;
}
inline std::atomic<long long> &__uploadTotalBytes() {
  static std::atomic<long long> v{0};
  return v;
}
inline void uploadProgressReset(long long total) {
  __uploadSentBytes().store(0);
  __uploadTotalBytes().store(total);
}
inline void uploadProgressAdd(long long bytes) { __uploadSentBytes().fetch_add(bytes); }
// 0..1 fraction of the active upload, or -1 when no upload total is set.
inline double fetchUploadProgress(double /*id*/) {
  const long long total = __uploadTotalBytes().load();
  if (total <= 0) return -1.0;
  double frac = static_cast<double>(__uploadSentBytes().load()) / static_cast<double>(total);
  if (frac < 0.0) frac = 0.0;
  if (frac > 1.0) frac = 1.0;
  return frac;
}
// Raw byte counts for the active upload (for MB / speed readouts). 0 when idle.
inline double fetchUploadSent(double /*id*/) { return static_cast<double>(__uploadSentBytes().load()); }
inline double fetchUploadTotal(double /*id*/) { return static_cast<double>(__uploadTotalBytes().load()); }

#ifdef GEA_CPP_VALUE_AVAILABLE
inline FetchRequestInit fetch_init_from_value(const gea_cpp_value &init_value) {
  FetchRequestInit init;
  if (init_value.record_has(std::string("method"))) {
    init.method = static_cast<std::string>(init_value.record_get(std::string("method")));
  }
  if (init_value.record_has(std::string("body"))) {
    gea_cpp_value body_value = init_value.record_get(std::string("body"));
    if (body_value.kind == gea_cpp_value::kind_t::array_value) {
      const std::vector<gea_cpp_value> values = static_cast<std::vector<gea_cpp_value>>(body_value);
      init.body.resize(values.size());
      for (std::size_t i = 0; i < values.size(); i++) {
        const auto byte = static_cast<int>(static_cast<double>(values[i]));
        init.body[i] = static_cast<char>(byte & 0xff);
      }
    } else {
      init.body = static_cast<std::string>(body_value);
    }
  }
  if (init_value.record_has(std::string("headers"))) {
    auto headers_value = init_value.record_get(std::string("headers"));
    if (headers_value.entries) {
      for (const auto &kv : *headers_value.entries) {
        if (kv.second.kind != gea_cpp_value::kind_t::string) continue;
        std::string lower(kv.first.size(), '\0');
        std::transform(kv.first.begin(), kv.first.end(), lower.begin(),
                       [](unsigned char c) { return std::tolower(c); });
        init.headers[lower] = static_cast<std::string>(kv.second);
      }
    }
  }
  return init;
}

inline FetchResponse fetch(const std::string &url, const gea_cpp_value &init) {
  return fetch(url, fetch_init_from_value(init));
}

template <typename T, typename = void>
struct fetch_has_field_method : std::false_type {};
template <typename T>
struct fetch_has_field_method<T, std::void_t<decltype(std::declval<const T &>().method)>> : std::true_type {};

template <typename T, typename = void>
struct fetch_has_field_headers : std::false_type {};
template <typename T>
struct fetch_has_field_headers<T, std::void_t<decltype(std::declval<const T &>().headers)>> : std::true_type {};

template <typename T, typename = void>
struct fetch_has_field_body : std::false_type {};
template <typename T>
struct fetch_has_field_body<T, std::void_t<decltype(std::declval<const T &>().body)>> : std::true_type {};

template <typename T, typename = void>
struct fetch_has_flag_method : std::false_type {};
template <typename T>
struct fetch_has_flag_method<T, std::void_t<decltype(std::declval<const T &>().__gea_has_method)>> : std::true_type {};

template <typename T, typename = void>
struct fetch_has_flag_headers : std::false_type {};
template <typename T>
struct fetch_has_flag_headers<T, std::void_t<decltype(std::declval<const T &>().__gea_has_headers)>> : std::true_type {};

template <typename T, typename = void>
struct fetch_has_flag_body : std::false_type {};
template <typename T>
struct fetch_has_flag_body<T, std::void_t<decltype(std::declval<const T &>().__gea_has_body)>> : std::true_type {};

template <typename T>
inline bool fetch_field_present_method(const T &record) {
  if constexpr (fetch_has_flag_method<T>::value) return record.__gea_has_method;
  else return true;
}

template <typename T>
inline bool fetch_field_present_headers(const T &record) {
  if constexpr (fetch_has_flag_headers<T>::value) return record.__gea_has_headers;
  else return true;
}

template <typename T>
inline bool fetch_field_present_body(const T &record) {
  if constexpr (fetch_has_flag_body<T>::value) return record.__gea_has_body;
  else return true;
}

inline void fetch_assign_headers(FetchRequestInit &init, const gea_cpp_value &headers_value) {
  if (!headers_value.entries) return;
  for (const auto &kv : *headers_value.entries) {
    if (kv.second.kind != gea_cpp_value::kind_t::string) continue;
    std::string lower(kv.first.size(), '\0');
    std::transform(kv.first.begin(), kv.first.end(), lower.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    init.headers[lower] = static_cast<std::string>(kv.second);
  }
}

template <typename Headers>
inline void fetch_assign_headers(FetchRequestInit &init, const Headers &headers) {
  if constexpr (requires { headers.__gea_to_value(); }) {
    fetch_assign_headers(init, headers.__gea_to_value());
  } else {
    fetch_assign_headers(init, gea_cpp_value::pack_value(headers));
  }
}

template <typename Body>
inline void fetch_assign_body(FetchRequestInit &init, const Body &body) {
  using B = std::decay_t<Body>;
  if constexpr (std::is_same_v<B, std::string>) {
    init.setBody(body);
  } else if constexpr (requires {
                         body.data();
                         body.size();
                         requires std::is_same_v<std::decay_t<decltype(*body.data())>, std::uint8_t>;
                       }) {
    // A byte buffer (Uint8Array / ArrayBuffer). The is_same check lives INSIDE
    // the requires so a body type without `.data()` (e.g. a `gea_cpp_value` from
    // an omitted optional `body`) is SFINAE-false here rather than a hard error.
    init.body.assign(reinterpret_cast<const char *>(body.data()), body.size());
  } else {
    gea_cpp_value body_value = gea_cpp_value::pack_value(body);
    if (body_value.kind == gea_cpp_value::kind_t::array_value) {
      const std::vector<gea_cpp_value> values = static_cast<std::vector<gea_cpp_value>>(body_value);
      init.body.resize(values.size());
      for (std::size_t i = 0; i < values.size(); i++) {
        const auto byte = static_cast<int>(static_cast<double>(values[i]));
        init.body[i] = static_cast<char>(byte & 0xff);
      }
    } else {
      init.body = static_cast<std::string>(body_value);
    }
  }
}

template <typename Init>
inline FetchRequestInit fetch_init_from_record(const Init &record) {
  FetchRequestInit init;
  if constexpr (fetch_has_field_method<Init>::value) {
    if (fetch_field_present_method(record)) init.method = record.method;
  }
  if constexpr (fetch_has_field_headers<Init>::value) {
    if (fetch_field_present_headers(record)) fetch_assign_headers(init, record.headers);
  }
  if constexpr (fetch_has_field_body<Init>::value) {
    if (fetch_field_present_body(record)) fetch_assign_body(init, record.body);
  }
  return init;
}

template <typename Init,
          typename = std::enable_if_t<!std::is_same_v<std::decay_t<Init>, FetchRequestInit> &&
                                      !std::is_same_v<std::decay_t<Init>, gea_cpp_value>>>
inline FetchResponse fetch(const std::string &url, const Init &init) {
  return fetch(url, fetch_init_from_record(init));
}

template <typename Init,
          typename = std::enable_if_t<!std::is_same_v<std::decay_t<Init>, FetchRequestInit> &&
                                      !std::is_same_v<std::decay_t<Init>, gea_cpp_value>>>
inline double fetchAsync(const std::string &url, const Init &init) {
  return fetchAsync(url, fetch_init_from_record(init));
}
#endif

}  // namespace gea::host

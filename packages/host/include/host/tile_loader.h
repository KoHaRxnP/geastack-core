// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <string>

namespace gea::host {

// Async tile/image loader. `request()` queues a load by cache key: the worker
// (its own task, pinned to the SECOND core on esp32) reads the persistent
// cache path first, falls back to `GET url` (persisting the bytes to the path
// after a successful decode), and decodes into the image store. The app drains
// completions with poll()/key()/imageId()/status() — typically once per
// animation frame — so the frame task never blocks on SD, network, or PNG
// decode. Requests are deduplicated by key while in flight.
class TileLoaderService {
 public:
  // `num` is an app-chosen numeric id echoed back via keyNum() — the app's
  // hot per-frame cache lookups then never build/compare strings (std::string
  // tile keys cost 6-14ms per render on a 360MHz MCU).
  void request(const std::string &key,
               double num,
               const std::string &path,
               const std::string &url,
               bool opaque,
               bool rotate90 = false) const;

  // Gate REMOTE loading: while false (an active pan/pinch), workers serve only
  // the persistent cache — an SD miss completes immediately with status 0
  // (retryable) instead of hitting the network. The app re-enables once the
  // gesture settles and re-requests, so downloads never compete with a live
  // gesture for bandwidth/CPU. Cache loads keep streaming mid-gesture.
  void setNetworkAllowed(bool allowed) const;

  // Drop all queued-but-not-started jobs (their keys become requestable
  // again). Call before re-queuing the current visible set so stale zoom
  // levels never starve the tiles actually on screen.
  void purgeQueued() const;

  // One-time maintenance: delete every persistent tiles/<z> level directory
  // whose z is NOT in the comma-separated keep list (e.g. "2,5,8,11,13,15,17,19").
  // Runs on the worker core before the census.
  void pruneLevels(const std::string &keepCsv) const;

  // Advance to the next completed result. False when none are ready.
  bool poll() const;
  // Key of the result selected by the last successful poll().
  std::string key() const;
  // Numeric id passed to request() for that result.
  double keyNum() const;
  // Decoded image-store id of that result; -1 when the load failed.
  double imageId() const;
  // HTTP status of that result (200/404/...); 0 = transport/WiFi failure —
  // retryable: a later request() for the same key tries again.
  double status() const;
};

inline constexpr TileLoaderService tiles{};

}  // namespace gea::host

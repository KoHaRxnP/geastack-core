// SPDX-License-Identifier: Apache-2.0
#include "host/tile_loader.h"

#include "host/fetch.h"
#include "host/display_orientation.h"
#include "host/image.h"
#include "image.h"
#include "platform/file_cache.h"

#include <chrono>
#include <cstdio>
#include <cstdint>
#include <deque>
#include <dirent.h>
#include <mutex>
#include <string>
#include <unistd.h>
#include <unordered_set>
#include <vector>

#if !defined(ESP_PLATFORM) && !defined(__EMSCRIPTEN__)
#include <thread>
#endif

#ifdef ESP_PLATFORM
#include "esp_cache.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"  // xTaskCreatePinnedToCoreWithCaps (PSRAM worker stacks)
#include "freertos/task.h"
#endif

namespace gea::host {

namespace {

constexpr int kTileStatusDecodePressure = -2;

struct TileJob {
  std::string key;
  double num = 0;
  std::string path;
  std::string url;
  bool opaque = false;
  bool rotate90 = false;
  std::int64_t requestedUs = 0;
};

struct TileResult {
  std::string key;
  double num = 0;
  int imageId = -1;
  int status = 0;
};

std::mutex g_tiles_mutex;
std::deque<TileJob> g_jobs;
std::unordered_set<std::string> g_pending;  // keys queued or in flight
std::deque<TileResult> g_results;
TileResult g_current;  // result selected by the last poll()
// While false (mid-gesture), workers serve the persistent cache only; misses
// complete immediately with status 0 so the app can re-request after settle.
volatile bool g_network_allowed = true;

std::int64_t nowUs() {
  return std::chrono::duration_cast<std::chrono::microseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

std::vector<std::uint8_t> readWholeFile(const std::string &path) {
  std::vector<std::uint8_t> out;
  std::FILE *f = std::fopen(path.c_str(), "rb");
  if (!f) return out;
  std::fseek(f, 0, SEEK_END);
  const long size = std::ftell(f);
  std::fseek(f, 0, SEEK_SET);
  if (size > 0) {
    out.resize(static_cast<std::size_t>(size));
    const std::size_t got = std::fread(out.data(), 1, out.size(), f);
    out.resize(got);
  }
  std::fclose(f);
  return out;
}

int decodeBytes(const std::vector<std::uint8_t> &bytes, bool opaque, bool rotate90) {
  auto &store = gea::framework::graphics::ImageStore::instance();
  const int id = opaque ? store.decodeOpaque(bytes.data(), static_cast<int>(bytes.size()), -1)
                        : store.decode(bytes.data(), static_cast<int>(bytes.size()), -1);
#ifdef ESP_PLATFORM
  // Bitmap pre-rotation is only valid when the logical drawing axes are
  // actually swapped 90deg relative to the native panel scanout — i.e. when the
  // effective orientation is landscape (width()==nativeHeight()). A non-square
  // panel that still scans out in its native portrait orientation (e.g. the
  // CO5300 410x502 board, which has no rotation hook so setOrientation pins
  // PortraitPrimary) is NOT swapped: rotating each tile there destroys the
  // shared tile edges and the map looks scrambled. The old "panel is non-square"
  // check mis-fired for exactly that case.
  const bool nativeAxesSwap = gea::framework::display::detail::orientationIsLandscape(
      gea::framework::display::detail::DisplayOrientationState::orientation());
  if (id >= 0 && rotate90 && nativeAxesSwap) store.rotate90(id);
#endif
#ifdef ESP_PLATFORM
  // Write the freshly decoded pixels back to PSRAM from THIS core's cache: the
  // renderer (CPU on core 0, or the PPA's DMA) reads them moments later, and
  // dirty lines stuck in core 1's L1 would otherwise serve stale/garbage data.
  if (id >= 0) {
    const auto *pixels = store.currentPixels(id);
    const auto *alpha = store.currentAlpha(id);
    const std::size_t pixelBytes =
        static_cast<std::size_t>(store.width(id)) * store.height(id) * sizeof(*pixels);
    constexpr int kFlags = ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_TYPE_DATA | ESP_CACHE_MSYNC_FLAG_UNALIGNED;
    if (pixels && pixelBytes > 0) esp_cache_msync(const_cast<void *>(static_cast<const void *>(pixels)), pixelBytes, kFlags);
    if (alpha) esp_cache_msync(const_cast<void *>(static_cast<const void *>(alpha)), pixelBytes / sizeof(*pixels), kFlags);
  }
#endif
  return id;
}

void deliver(const TileJob &job, int imageId, int status) {
  std::lock_guard<std::mutex> lock(g_tiles_mutex);
  g_results.push_back(TileResult{job.key, job.num, imageId, status});
  g_pending.erase(job.key);
}

// SD-first, network-fallback load. Persist the raw bytes only after a
// successful decode, so a truncated download never poisons the cache.
// Every tile logs its phase breakdown: queue wait, SD read, network fetch,
// decode, persist — slow loading must never be guesswork again.
void processJob(const TileJob &job) {
  const std::int64_t startUs = nowUs();
  const std::int64_t queueUs = job.requestedUs > 0 ? startUs - job.requestedUs : 0;
  if (gea::platform::storage::ensureMounted()) {
    const std::int64_t sd0 = nowUs();
    const std::vector<std::uint8_t> cached = readWholeFile(job.path);
    const std::int64_t sdUs = nowUs() - sd0;
    if (!cached.empty()) {
      const std::int64_t dec0 = nowUs();
      const int id = decodeBytes(cached, job.opaque, job.rotate90);
      const std::int64_t decUs = nowUs() - dec0;
      if (id >= 0) {
        deliver(job, id, 200);
        std::printf("[tiles] %s src=sd queue=%lldms sd=%lldms decode=%lldms total=%lldms\n", job.key.c_str(),
                    queueUs / 1000, sdUs / 1000, decUs / 1000, (nowUs() - job.requestedUs) / 1000);
        return;
      }
      // Usually PSRAM/image-store pressure, not corruption: cached files are
      // written only after a successful decode. Keep the file and retry after
      // the app has evicted resident tiles.
      deliver(job, -1, kTileStatusDecodePressure);
      std::printf("[tiles] %s src=sd queue=%lldms sd=%lldms decode=fail total=%lldms\n", job.key.c_str(),
                  queueUs / 1000, sdUs / 1000, (nowUs() - job.requestedUs) / 1000);
      return;
    }
  }

  if (!g_network_allowed) {
    // Mid-gesture: don't touch the network. Status 0 is retryable — the app
    // re-requests once the gesture settles.
    deliver(job, -1, 0);
    return;
  }
  const std::int64_t net0 = nowUs();
  const FetchResponse res = fetch(job.url);
  const std::int64_t netUs = nowUs() - net0;
  if (!res.ok) {
    deliver(job, -1, static_cast<int>(res.status));
    return;
  }
  const std::int64_t dec0 = nowUs();
  const int id = decodeBytes(res.body, job.opaque, job.rotate90);
  const std::int64_t decUs = nowUs() - dec0;
  if (id < 0) {
    deliver(job, -1, kTileStatusDecodePressure);
    std::printf("[tiles] %s src=net queue=%lldms net=%lldms decode=fail total=%lldms\n",
                job.key.c_str(), queueUs / 1000, netUs / 1000, (nowUs() - job.requestedUs) / 1000);
    return;
  }
  // Deliver BEFORE persisting: the SD write costs 30-80ms and the tile is
  // already decoded — make it visible now, write the cache file after.
  deliver(job, id, 200);
  const std::int64_t per0 = nowUs();
  image.writeFile(job.path, res.body);
  std::printf("[tiles] %s src=net queue=%lldms net=%lldms decode=%lldms persist=%lldms total=%lldms\n",
              job.key.c_str(), queueUs / 1000, netUs / 1000, decUs / 1000, (nowUs() - per0) / 1000,
              (nowUs() - job.requestedUs) / 1000);
}

// Pending prune request (set by the app at startup, executed by worker 0
// before the census so the census reports the post-prune state).
std::string g_prune_keep_csv;
volatile bool g_prune_pending = false;

// Delete every tiles/<z> level directory whose z is NOT in the keep CSV.
// Depth-2 tree remove (z/x/y.png) over FAT — worker core only.
void pruneTileLevels(const std::string &root, const std::string &keepCsv) {
  std::unordered_set<std::string> keep;
  std::string token;
  for (char c : keepCsv + ",") {
    if (c == ',') {
      if (!token.empty()) keep.insert(token);
      token.clear();
    } else {
      token += c;
    }
  }
  DIR *zDir = opendir(root.c_str());
  if (!zDir) return;
  std::vector<std::string> doomed;
  while (dirent *zEntry = readdir(zDir)) {
    if (zEntry->d_name[0] == '.') continue;
    if (!keep.count(zEntry->d_name)) doomed.push_back(zEntry->d_name);
  }
  closedir(zDir);
  for (const std::string &z : doomed) {
    const std::string zPath = root + "/" + z;
    int removed = 0;
    DIR *xDir = opendir(zPath.c_str());
    if (!xDir) continue;
    std::vector<std::string> xDirs;
    while (dirent *xEntry = readdir(xDir)) {
      if (xEntry->d_name[0] != '.') xDirs.push_back(xEntry->d_name);
    }
    closedir(xDir);
    for (const std::string &x : xDirs) {
      const std::string xPath = zPath + "/" + x;
      DIR *yDir = opendir(xPath.c_str());
      if (!yDir) continue;
      std::vector<std::string> files;
      while (dirent *yEntry = readdir(yDir)) {
        if (yEntry->d_name[0] != '.') files.push_back(yEntry->d_name);
      }
      closedir(yDir);
      for (const std::string &f : files) {
        if (std::remove((xPath + "/" + f).c_str()) == 0) removed++;
      }
      ::rmdir(xPath.c_str());
    }
    ::rmdir(zPath.c_str());
    std::printf("[tiles] pruned z%s (%d files)\n", z.c_str(), removed);
  }
}

// One-time census of the persistent tile cache: logs how many tiles each zoom
// level holds (tiles/<z>/<x>/<y>.png). Runs once on the worker core before the
// first job — readdir over FAT is slow, so it must never touch the frame task.
void logTileCensus(const std::string &root) {
  DIR *zDir = opendir(root.c_str());
  if (!zDir) return;
  int totalFiles = 0;
  int levels = 0;
  while (dirent *zEntry = readdir(zDir)) {
    if (zEntry->d_name[0] == '.') continue;
    const std::string zPath = root + "/" + zEntry->d_name;
    DIR *xDir = opendir(zPath.c_str());
    if (!xDir) continue;
    int count = 0;
    while (dirent *xEntry = readdir(xDir)) {
      if (xEntry->d_name[0] == '.') continue;
      const std::string xPath = zPath + "/" + xEntry->d_name;
      DIR *yDir = opendir(xPath.c_str());
      if (!yDir) continue;
      while (dirent *yEntry = readdir(yDir)) {
        if (yEntry->d_name[0] != '.') count++;
      }
      closedir(yDir);
    }
    closedir(xDir);
    std::printf("[tiles] census z%s: %d tiles\n", zEntry->d_name, count);
    totalFiles += count;
    levels++;
  }
  closedir(zDir);
  std::printf("[tiles] census total: %d tiles across %d levels\n", totalFiles, levels);
}

#ifdef ESP_PLATFORM

void tileWorker(void *arg) {
  // Worker 0 handles one-time maintenance before serving jobs: wait briefly
  // for the app's prune request (sent during startup), apply it, then log the
  // census of what's left. Worker 1 serves jobs in the meantime.
  if (arg == nullptr && gea::platform::storage::ensureMounted()) {
    for (int i = 0; i < 30 && !g_prune_pending; i++) vTaskDelay(pdMS_TO_TICKS(100));
    if (g_prune_pending) {
      pruneTileLevels("/sdcard/tiles", g_prune_keep_csv);
      g_prune_pending = false;
    }
    logTileCensus("/sdcard/tiles");
  }
  while (true) {
    TileJob job;
    bool haveJob = false;
    {
      std::lock_guard<std::mutex> lock(g_tiles_mutex);
      if (!g_jobs.empty()) {
        job = g_jobs.front();
        g_jobs.pop_front();
        haveJob = true;
      }
    }
    if (!haveJob) {
      vTaskDelay(pdMS_TO_TICKS(15));
      continue;
    }
    processJob(job);
  }
}

void ensureWorker() {
  static bool started = false;
  if (started) return;
  started = true;
  // Second core: the frame task is pinned to core 0
  // (GEA_EMBEDDED_APP_FRAME_TASK_CORE), so SD reads, TLS fetches, and PNG
  // decodes all run on core 1 and never steal frame time. The stack must fit a
  // TLS handshake (~16 KiB peak with the cert bundle). TWO workers: tile
  // fetches are network-latency-bound, so two in flight nearly halve the
  // fill time of an uncached screen (each worker keeps its own thread-local
  // keep-alive connection — two is also the polite per-client cap for OSM).
  // PSRAM-backed stacks: each 20 KiB TLS-handshake stack would otherwise eat
  // internal DRAM (40 KiB for the pair), collapsing the largest contiguous
  // internal block below what the lwIP socket + TLS handshake need (especially
  // once the fatfs/SD stack is also linked). The tile worker only does network,
  // SD-over-SPI, and PSRAM decode — never an internal-flash op — so it is never
  // the task running during a cache-disable window, and a PSRAM stack is safe
  // (IDF freezes it during any other task's flash op). Same pattern as the BLE
  // bring-up's PSRAM-stacked caller.
  static TaskHandle_t s_worker0 = nullptr;
  static TaskHandle_t s_worker1 = nullptr;
  xTaskCreatePinnedToCoreWithCaps(tileWorker, "tile-loader0", 20480, nullptr, 5, &s_worker0, 1,
                                  MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  xTaskCreatePinnedToCoreWithCaps(tileWorker, "tile-loader1", 20480, reinterpret_cast<void *>(1), 5,
                                  &s_worker1, 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

#elif defined(__EMSCRIPTEN__)

// Web sim / tests: no worker — process synchronously at request() time. The
// web fetch bridge is itself async-bridged, and tests use canned responses,
// so blocking here is acceptable.
void ensureWorker() {}

#else

// Desktop targets (macOS, raspberry-pi-os): real blocking network backends
// (libcurl), so the ESP worker model applies — a synchronous fetch at
// request() time would stall the frame thread ~250ms per tile (seconds per
// settled pan, a beachball on macOS). Same two-worker sizing as ESP: tile
// fetches are network-latency-bound, two in flight nearly halve the fill
// time of an uncached screen, and two is the polite per-client cap for OSM.
// ImageStore decode is worker-safe by design (slot reserved under its mutex,
// filled unlocked, id invisible until poll() delivers it).
void tileWorker()
{
  while (true) {
    TileJob job;
    bool haveJob = false;
    {
      std::lock_guard<std::mutex> lock(g_tiles_mutex);
      if (!g_jobs.empty()) {
        job = g_jobs.front();
        g_jobs.pop_front();
        haveJob = true;
      }
    }
    if (!haveJob) {
      std::this_thread::sleep_for(std::chrono::milliseconds(15));
      continue;
    }
    processJob(job);
  }
}

void ensureWorker()
{
  static bool started = false;
  if (started) return;
  started = true;
  std::thread(tileWorker).detach();
  std::thread(tileWorker).detach();
}

#endif

}  // namespace

void TileLoaderService::request(const std::string &key,
                                double num,
                                const std::string &path,
                                const std::string &url,
                                bool opaque,
                                bool rotate90) const {
  {
    std::lock_guard<std::mutex> lock(g_tiles_mutex);
    if (g_pending.count(key)) return;
    // Also dedup against DELIVERED-BUT-UNDRAINED results: a request pass that
    // lands between a worker's deliver() and the app's next-frame drain used
    // to re-request the tile — and immediately SD-hit the file the network
    // load had just persisted, loading nearly every downloaded tile TWICE.
    for (const TileResult &r : g_results) {
      if (r.key == key) return;
    }
    g_pending.insert(key);
    g_jobs.push_back(TileJob{key, num, path, url, opaque, rotate90, nowUs()});
  }
  ensureWorker();
#if !defined(ESP_PLATFORM) && defined(__EMSCRIPTEN__)
  // Synchronous fallback: drain the job we just queued.
  TileJob job;
  bool haveJob = false;
  {
    std::lock_guard<std::mutex> lock(g_tiles_mutex);
    if (!g_jobs.empty()) {
      job = g_jobs.front();
      g_jobs.pop_front();
      haveJob = true;
    }
  }
  if (haveJob) processJob(job);
#endif
}

void TileLoaderService::setNetworkAllowed(bool allowed) const {
  g_network_allowed = allowed;
}

void TileLoaderService::pruneLevels(const std::string &keepCsv) const {
  g_prune_keep_csv = keepCsv;
  g_prune_pending = true;
  ensureWorker();
#ifndef ESP_PLATFORM
  // Synchronous on host builds (tests/sim) — prune immediately.
  pruneTileLevels("/sdcard/tiles", keepCsv);
  g_prune_pending = false;
#endif
}

void TileLoaderService::purgeQueued() const {
  // Drop every job not yet picked up by a worker, releasing their keys for
  // re-request. The app calls this before re-queuing the CURRENT visible set,
  // so tiles from zoom levels you already pinched past never hold the queue
  // hostage (each stale fetch is ~0.5-1s — a few pinches used to pile up
  // minutes of backlog behind which the visible tiles starved).
  std::lock_guard<std::mutex> lock(g_tiles_mutex);
  for (const TileJob &job : g_jobs) g_pending.erase(job.key);
  g_jobs.clear();
}

bool TileLoaderService::poll() const {
  std::lock_guard<std::mutex> lock(g_tiles_mutex);
  if (g_results.empty()) return false;
  g_current = g_results.front();
  g_results.pop_front();
  return true;
}

std::string TileLoaderService::key() const {
  std::lock_guard<std::mutex> lock(g_tiles_mutex);
  return g_current.key;
}

double TileLoaderService::keyNum() const {
  std::lock_guard<std::mutex> lock(g_tiles_mutex);
  return g_current.num;
}

double TileLoaderService::imageId() const {
  std::lock_guard<std::mutex> lock(g_tiles_mutex);
  return static_cast<double>(g_current.imageId);
}

double TileLoaderService::status() const {
  std::lock_guard<std::mutex> lock(g_tiles_mutex);
  return static_cast<double>(g_current.status);
}

}  // namespace gea::host

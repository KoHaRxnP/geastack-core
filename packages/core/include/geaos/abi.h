#pragma once
//
// geaos app ABI — the stable boundary between a dynamically-loaded gea app and
// the geaos core + backends. See docs/geaos-app-platform.md.
//
// This is a **C ABI on purpose**: the loader boundary must not depend on C++
// name mangling or C++ ABI stability. Apps are authored in TSX/C++, but the
// surface they bind to is this plain-C table of function pointers.
//
// BINDING (same surface, two transports):
//   - esp32 (in-process): the core fills this table with thin thunks over the
//     `gea::framework::*Backend` classes — effectively the jump table the
//     loader patches. Zero IPC.
//   - Linux (cross-process): the core fills it with proxy thunks that marshal
//     to the geaos compositor process. Real isolation.
//
// VERSIONING: bump GEAOS_ABI_VERSION on any layout/semantics change. The app
// records the version it was built against; the loader refuses a mismatch.
//
// STRINGS: UTF-8. Inbound strings are caller-owned `const char *`. Outbound
// strings are written into a caller-provided buffer: pass `*len` = capacity,
// it is overwritten with the written length (excluding the NUL). NUMBERS use
// gea's value model (double) except explicit sizes/handles. A NULL function
// pointer (or NULL sub-table) means "unavailable on this target / not granted".

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GEAOS_ABI_VERSION 1u

// --- capabilities -----------------------------------------------------------
// Declared in the app manifest; the loader acquires/grants the matching
// per-target resources (e.g. esp32 brings up the WiFi/BLE backends only when
// granted — see docs §9) and leaves the sub-table NULL when not granted.
typedef enum {
  GEAOS_CAP_NONE    = 0,
  GEAOS_CAP_WIFI    = 1u << 0,
  GEAOS_CAP_BLE     = 1u << 1,
  GEAOS_CAP_SENSORS = 1u << 2,
  GEAOS_CAP_CAMERA  = 1u << 3,
  GEAOS_CAP_AUDIO   = 1u << 4,
  GEAOS_CAP_STORAGE = 1u << 5,
} GeaosCapability;
typedef uint32_t GeaosCapabilityMask;

// --- service tables (mirror gea::framework::*Backend) -----------------------

typedef struct GeaosSystemApi {
  void     (*log)(const char *msg);
  double   (*now_ms)(void);
  uint32_t (*abi_version)(void);
} GeaosSystemApi;

typedef struct GeaosMemoryApi {
  double (*internal_free)(void);
  double (*internal_largest_free_block)(void);
  double (*internal_minimum_free)(void);
  double (*psram_free)(void);
} GeaosMemoryApi;

typedef struct GeaosDisplayApi {
  double (*brightness)(void);
  void   (*set_brightness)(double v);
  // The reactive UI render path (tree mutation + present) is provided by the
  // shared core runtime, not per-call here. Its exact placement relative to a
  // dynamically-loaded app is the Phase 2 decision (doc §5/§13); this sub-table
  // is the device-level display control only.
} GeaosDisplayApi;

typedef struct GeaosInputApi {
  bool (*take_back_pressed)(void);  // one-shot: reads and clears the flag
} GeaosInputApi;

typedef struct GeaosWifiApi {
  bool   (*enabled)(void);
  void   (*set_enabled)(bool enabled);
  bool   (*connected)(void);
  double (*rssi)(void);
  void   (*ssid)(char *out, int *len);
  void   (*ip)(char *out, int *len);
  void   (*mac)(char *out, int *len);
  void   (*configure)(const char *ssid, const char *password);
  void   (*start_scan)(void);
  bool   (*scanning)(void);
  double (*scan_count)(void);
  void   (*scan_ssid_at)(double index, char *out, int *len);
  double (*scan_rssi_at)(double index);
  bool   (*scan_secured_at)(double index);
} GeaosWifiApi;

typedef struct GeaosBluetoothApi {
  void   (*init)(const char *device_name, double appearance, const char *mac);
  bool   (*enabled)(void);
  void   (*set_enabled)(bool enabled);
  void   (*start_advertising)(void);
  void   (*stop_advertising)(void);
  bool   (*connected)(void);
  bool   (*bound)(void);
  double (*battery_level)(void);
  void   (*key_tap)(double hid_code);
  void   (*key_down)(double modifier, double hid_code);
  void   (*key_up)(void);
  void   (*mouse_move)(double dx, double dy, double buttons, double wheel);
  void   (*mouse_click)(double button);
} GeaosBluetoothApi;

typedef struct GeaosSensorsApi {
  void   (*init)(void);
  void   (*close)(void);
  double (*tilt_x)(void);
  double (*tilt_y)(void);
  double (*accel_x)(void);
  double (*accel_y)(void);
  double (*accel_z)(void);
  double (*gyro_x)(void);
  double (*gyro_y)(void);
  double (*gyro_z)(void);
} GeaosSensorsApi;

// The aggregate syscall table handed to the app at init. A sub-table is NULL
// when the target lacks it or the capability was not granted.
typedef struct GeaosServices {
  uint32_t abi_version;  // == GEAOS_ABI_VERSION the core implements
  const GeaosSystemApi    *system;
  const GeaosMemoryApi    *memory;
  const GeaosDisplayApi   *display;
  const GeaosInputApi     *input;
  const GeaosWifiApi      *wifi;       // NULL unless GEAOS_CAP_WIFI granted
  const GeaosBluetoothApi *bluetooth;  // NULL unless GEAOS_CAP_BLE granted
  const GeaosSensorsApi   *sensors;    // NULL unless GEAOS_CAP_SENSORS granted
  // camera / audio / storage / power: formalized in Phase 3 as the surface settles.
} GeaosServices;

// --- app lifecycle (the app implements; the loader drives) ------------------
// Mirrors gea::framework::app::Application (init/frame/toggleSettings) plus a
// teardown hook the dynamic model needs for clean unload.
typedef struct GeaosAppLifecycle {
  void (*init)(const GeaosServices *services, int width, int height, double device_pixel_ratio);
  void (*frame)(int timestamp_ms);
  void (*toggle_settings)(void);  // may be NULL
  void (*teardown)(void);         // called before unload; may be NULL
} GeaosAppLifecycle;

// --- app descriptor (the app exports exactly one) ---------------------------
typedef struct GeaosAppDescriptor {
  uint32_t            abi_version;   // GEAOS_ABI_VERSION the app was built for
  const char         *id;           // stable app id
  const char         *name;         // display name
  GeaosCapabilityMask capabilities; // services the app needs
  GeaosAppLifecycle   lifecycle;
} GeaosAppDescriptor;

// Each app exports this single symbol. The loader resolves it (esp32: via the
// fixed-slot/jump-table binding; Linux: dlsym / the IPC handshake), checks
// abi_version + capabilities, then drives the lifecycle.
#define GEAOS_APP_ENTRY_SYMBOL "geaos_app_descriptor"
const GeaosAppDescriptor *geaos_app_descriptor(void);

#ifdef __cplusplus
}  // extern "C"
#endif

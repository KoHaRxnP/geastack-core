// In-process binding of the geaos app ABI (single-address-space targets, e.g.
// esp32). Fills a GeaosServices table with thin thunks over the
// gea::framework::*Backend classes — this is the "jump table" the slot loader
// hands a dynamically-loaded app. See docs/geaos-app-platform.md §5/§7.
//
// Capability gating (NULLing wifi/bluetooth/sensors until granted + acquiring
// the backend) is the loader/resource-policy job (Phase 4); this layer just
// exposes the full in-process surface.

#include "geaos/abi.h"

#include "host/backends.h"
#include "services/diagnostics.h"

#include <cstring>
#include <string>

namespace gea::geaos {

namespace {

namespace net = gea::framework::network;
namespace bt = gea::framework::bluetooth;
namespace sens = gea::framework::sensors;
namespace disp = gea::framework::display;
namespace mem = gea::framework::memory;
namespace input = gea::framework::input;
using gea::framework::services::DiagnosticsServer;

// Write a std::string into a caller buffer per the ABI string convention:
// *len in = capacity, *len out = written length (excluding NUL).
void writeStr(const std::string &s, char *out, int *len)
{
  const int cap = (out && len) ? *len : 0;
  if (out && cap > 0) {
    int n = static_cast<int>(s.size());
    if (n > cap - 1) n = cap - 1;
    std::memcpy(out, s.data(), static_cast<size_t>(n));
    out[n] = '\0';
    if (len) *len = n;
  } else if (len) {
    *len = 0;
  }
}

// --- system ---
void sys_log(const char *msg) { DiagnosticsServer::print("%s", msg ? msg : ""); }
double sys_now_ms(void) { return 0.0; }  // TODO(phase1): wire a monotonic clock backend
uint32_t sys_abi_version(void) { return GEAOS_ABI_VERSION; }

// --- memory ---
double m_internal_free(void) { return mem::MemoryBackend::internalFree(); }
double m_internal_largest(void) { return mem::MemoryBackend::internalLargestFreeBlock(); }
double m_internal_min(void) { return mem::MemoryBackend::internalMinimumFree(); }
double m_psram_free(void) { return mem::MemoryBackend::psramFree(); }

// --- display ---
double d_brightness(void) { return disp::DisplayBackend::brightness(); }
void d_set_brightness(double v) { disp::DisplayBackend::setBrightness(v); }

// --- input ---
bool in_take_back(void) { return input::InputBackend::consumeBackButton(); }

// --- wifi ---
bool w_enabled(void) { return net::WifiBackend::enabled(); }
void w_set_enabled(bool e) { net::WifiBackend::setEnabled(e); }
bool w_connected(void) { return net::WifiBackend::connected(); }
double w_rssi(void) { return net::WifiBackend::rssi(); }
void w_ssid(char *o, int *l) { writeStr(net::WifiBackend::ssid(), o, l); }
void w_ip(char *o, int *l) { writeStr(net::WifiBackend::ip(), o, l); }
void w_mac(char *o, int *l) { writeStr(net::WifiBackend::mac(), o, l); }
void w_configure(const char *s, const char *p) { net::WifiBackend::configure(s ? s : "", p ? p : ""); }
void w_start_scan(void) { net::WifiBackend::startScan(); }
bool w_scanning(void) { return net::WifiBackend::scanning(); }
double w_scan_count(void) { return net::WifiBackend::scanCount(); }
void w_scan_ssid_at(double i, char *o, int *l) { writeStr(net::WifiBackend::scanSsidAt(i), o, l); }
double w_scan_rssi_at(double i) { return net::WifiBackend::scanRssiAt(i); }
bool w_scan_secured_at(double i) { return net::WifiBackend::scanSecuredAt(i); }

// --- bluetooth (HID) ---
void b_init(const char *n, double a, const char *m) { bt::HidBackend::init(n ? n : "", a, m ? m : ""); }
bool b_enabled(void) { return bt::HidBackend::enabled(); }
void b_set_enabled(bool e) { bt::HidBackend::setEnabled(e); }
void b_start_adv(void) { bt::HidBackend::startAdvertising(); }
void b_stop_adv(void) { bt::HidBackend::stopAdvertising(); }
bool b_connected(void) { return bt::HidBackend::connected(); }
bool b_bound(void) { return bt::HidBackend::bound(); }
double b_battery(void) { return bt::HidBackend::batteryLevel(); }
void b_key_tap(double c) { bt::HidBackend::keyTap(c); }
void b_key_down(double mod, double c) { bt::HidBackend::keyDown(mod, c); }
void b_key_up(void) { bt::HidBackend::keyUp(); }
void b_mouse_move(double dx, double dy, double btn, double wheel) { bt::HidBackend::mouseMove(dx, dy, btn, wheel); }
void b_mouse_click(double btn) { bt::HidBackend::mouseClick(btn); }

// --- sensors (accelerometer/IMU) ---
void s_init(void) { sens::AccelerometerBackend::init(); }
void s_close(void) { sens::AccelerometerBackend::close(); }
double s_tilt_x(void) { return sens::AccelerometerBackend::tiltX(); }
double s_tilt_y(void) { return sens::AccelerometerBackend::tiltY(); }
double s_accel_x(void) { return sens::AccelerometerBackend::accelerationX(); }
double s_accel_y(void) { return sens::AccelerometerBackend::accelerationY(); }
double s_accel_z(void) { return sens::AccelerometerBackend::accelerationZ(); }
double s_gyro_x(void) { return sens::AccelerometerBackend::gyroscopeX(); }
double s_gyro_y(void) { return sens::AccelerometerBackend::gyroscopeY(); }
double s_gyro_z(void) { return sens::AccelerometerBackend::gyroscopeZ(); }

const GeaosSystemApi kSystem = {sys_log, sys_now_ms, sys_abi_version};
const GeaosMemoryApi kMemory = {m_internal_free, m_internal_largest, m_internal_min, m_psram_free};
const GeaosDisplayApi kDisplay = {d_brightness, d_set_brightness};
const GeaosInputApi kInput = {in_take_back};
const GeaosWifiApi kWifi = {w_enabled, w_set_enabled, w_connected, w_rssi, w_ssid, w_ip, w_mac,
                            w_configure, w_start_scan, w_scanning, w_scan_count, w_scan_ssid_at,
                            w_scan_rssi_at, w_scan_secured_at};
const GeaosBluetoothApi kBluetooth = {b_init, b_enabled, b_set_enabled, b_start_adv, b_stop_adv,
                                      b_connected, b_bound, b_battery, b_key_tap, b_key_down,
                                      b_key_up, b_mouse_move, b_mouse_click};
const GeaosSensorsApi kSensors = {s_init, s_close, s_tilt_x, s_tilt_y, s_accel_x, s_accel_y,
                                  s_accel_z, s_gyro_x, s_gyro_y, s_gyro_z};

const GeaosServices kServices = {
    GEAOS_ABI_VERSION, &kSystem, &kMemory, &kDisplay, &kInput, &kWifi, &kBluetooth, &kSensors,
};

}  // namespace

// Returns the in-process syscall table. The slot loader hands this pointer to
// a dynamically-loaded app at init (capability gating applied by the loader).
const GeaosServices *inProcessServices() { return &kServices; }

}  // namespace gea::geaos

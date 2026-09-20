// Compile-smoke for the *app side* of the geaos ABI: a minimal app implemented
// in plain C that binds the GeaosServices table and exports the descriptor the
// loader resolves. Not a shipped app — it exists to keep the ABI header valid,
// stable C, and authorable from the app side. See docs/geaos-app-platform.md.

#include "geaos/abi.h"

static const GeaosServices *g_services;

static void smoke_init(const GeaosServices *services, int width, int height, double dpr)
{
  (void)width;
  (void)height;
  (void)dpr;
  g_services = services;
  if (services && services->system && services->system->log) {
    services->system->log("geaos-abi-smoke: init");
  }
  if (services && services->memory && services->memory->internal_free) {
    (void)services->memory->internal_free();  // exercise a service call
  }
}

static void smoke_frame(int timestamp_ms) { (void)timestamp_ms; }

static void smoke_toggle_settings(void) {}

static void smoke_teardown(void)
{
  if (g_services && g_services->system && g_services->system->log) {
    g_services->system->log("geaos-abi-smoke: teardown");
  }
}

static const GeaosAppDescriptor kDescriptor = {
    GEAOS_ABI_VERSION,
    "geaos-abi-smoke",
    "ABI Smoke",
    GEAOS_CAP_NONE,
    {smoke_init, smoke_frame, smoke_toggle_settings, smoke_teardown},
};

const GeaosAppDescriptor *geaos_app_descriptor(void) { return &kDescriptor; }

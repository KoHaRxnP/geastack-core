// geaos Phase 2 execute-proof: a minimal loadable app, fixed-base linked at the
// vaddr esp_mmu_map deterministically returns (0x43060000, measured on-device).
// Proves: loader maps this blob's flash region -> jumps in -> code runs XIP from
// flash -> calls back into the firmware via a passed pointer -> returns.
//
// Self-contained (no firmware symbol deps yet) so this isolates the
// map/jump/execute/callback mechanism from the --just-symbols question.

#include <stdint.h>

#define GEAOS_SLOT_BASE 0x43060000u

typedef void (*geaos_log_fn)(const char *);

// Real entry. Gets a firmware log callback; returns a sentinel the loader checks.
int app_entry(geaos_log_fn log)
{
  // Full proof: the string is app .rodata, now linked at the DBUS vaddr so the
  // firmware's printf can byte-read it. Exercises execute + rodata-via-data-bus
  // + callback + return all together.
  if (log) log("geaos loaded-app: hello via two-segment IBUS/DBUS mapping");
  return 42;
}

// 16-byte blob header at offset 0 so the loader locates the entry without nm:
// magic, abi version, entry offset (link-time constant), reserved.
__attribute__((section(".geaos_header"), used))
const uint32_t geaos_header[4] = {
    0x47454131u,                                          // 'GEA1'
    1u,                                                   // abi version
    (uint32_t)((uintptr_t)&app_entry - GEAOS_SLOT_BASE),  // entry offset
    0u,
};

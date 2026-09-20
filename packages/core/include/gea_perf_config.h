#pragma once

// ============================================================================
// ONE master switch for ALL gea per-frame perf instrumentation.
//
//   GEA_EMBEDDED_PERF
//
// governs every perf subsystem at once — frame/phase timing, the esp32
// scheduler harvest + "perf:"/"perf-lite:" lines, per-op refresh & canvas 2D
// stats, the requestAnimationFrame queue timing, and heap diagnostics. When it
// is 0, ALL of that is compiled out: no esp_timer/chrono reads, no counter
// increments, no stat resets, no log output — zero machinery, zero cost.
//
// When it is 1 (the development default), each subsystem flag below takes its
// own value, so a build can still enable just one line (e.g. set
// GEA_FRAME_PERF_LOG and leave the rest) for targeted profiling.
//
// A production target flips perf off in ONE place — its CMakeLists / sdkconfig
// sets GEA_EMBEDDED_PERF=0 — instead of remembering to zero each sub-flag.
//
// This header is the single source of truth for the sub-flag defaults; it is
// included by the lib perf consumers directly and by the esp32 perf consumers
// transitively through memory_config.h. The build's own -D defines (seen before
// any header) still win for an individual flag; the master override below then
// forces every flag off when the master is off, regardless of those -D values.
// ============================================================================

#ifndef GEA_EMBEDDED_PERF
#define GEA_EMBEDDED_PERF 1
#endif

// --- individual subsystem flags (consulted only when the master is on) ------

// App "gea.perf:" rolling-window line + its per-phase timing (gea_app_entry.cpp).
#ifndef GEA_FRAME_PERF_LOG
#define GEA_FRAME_PERF_LOG 1
#endif

// esp32 frame-scheduler per-frame harvest + "perf:" line (frame_scheduler.cpp).
#ifndef GEA_EMBEDDED_FRAME_SCHEDULER_PERF_LOG
#define GEA_EMBEDDED_FRAME_SCHEDULER_PERF_LOG 1
#endif

// esp32 frame-scheduler compact "perf-lite:" line.
#ifndef GEA_EMBEDDED_FRAME_SCHEDULER_PERF_LITE
#define GEA_EMBEDDED_FRAME_SCHEDULER_PERF_LITE 0
#endif

// Per-op refresh timing (ScopedRefreshStat; refresh_perf.h + tree_render/render).
#ifndef GEA_EMBEDDED_UI_REFRESH_PERF
#define GEA_EMBEDDED_UI_REFRESH_PERF 1
#endif

// Per-op canvas 2D timing (canvas_element.cpp).
#ifndef GEA_EMBEDDED_CANVAS_PERF_DETAIL
#define GEA_EMBEDDED_CANVAS_PERF_DETAIL 0
#endif

// requestAnimationFrame queue timing (host/timers.cpp).
#ifndef GEA_EMBEDDED_RAF_PERF
#define GEA_EMBEDDED_RAF_PERF 1
#endif

// Heap diagnostics logging (memory_config.h consumers).
#ifndef GEA_EMBEDDED_HEAP_DIAGNOSTICS_LOG
#define GEA_EMBEDDED_HEAP_DIAGNOSTICS_LOG 0
#endif

// The esp32 frame scheduler's "geafps: PRODFPS=" line. Deliberately NOT under
// the master switch below: its whole purpose is to report the true frame rate
// of a build with GEA_EMBEDDED_PERF=0, where every other perf line is stripped.
// It does need a switch of its own, though, because it is otherwise one INFO
// line per second for the life of the device -- which overwrites an app's log
// ring and drowns a serial capture within minutes. On by default so no existing
// board changes behaviour.
#ifndef GEA_EMBEDDED_FRAME_SCHEDULER_FPS_LOG
#define GEA_EMBEDDED_FRAME_SCHEDULER_FPS_LOG 1
#endif

// --- master OFF forces every subsystem OFF ----------------------------------
#if !GEA_EMBEDDED_PERF
#undef GEA_FRAME_PERF_LOG
#define GEA_FRAME_PERF_LOG 0
#undef GEA_EMBEDDED_FRAME_SCHEDULER_PERF_LOG
#define GEA_EMBEDDED_FRAME_SCHEDULER_PERF_LOG 0
#undef GEA_EMBEDDED_FRAME_SCHEDULER_PERF_LITE
#define GEA_EMBEDDED_FRAME_SCHEDULER_PERF_LITE 0
#undef GEA_EMBEDDED_UI_REFRESH_PERF
#define GEA_EMBEDDED_UI_REFRESH_PERF 0
#undef GEA_EMBEDDED_CANVAS_PERF_DETAIL
#define GEA_EMBEDDED_CANVAS_PERF_DETAIL 0
#undef GEA_EMBEDDED_RAF_PERF
#define GEA_EMBEDDED_RAF_PERF 0
#undef GEA_EMBEDDED_HEAP_DIAGNOSTICS_LOG
#define GEA_EMBEDDED_HEAP_DIAGNOSTICS_LOG 0
#endif

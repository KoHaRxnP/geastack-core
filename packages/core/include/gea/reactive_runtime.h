// SPDX-License-Identifier: Apache-2.0
//
// @geastack/core — reactive-apply runtime support.
//
// Source fragment fused into a program's translation unit by the gea plugin
// (geatsc-plugin-gea inserts `#include "gea/reactive_runtime.h"` right after the
// runtime include, ONLY when the program uses reactivity). NOT self-contained:
// it relies on the JS-language runtime already being in scope (gea_cpp_value,
// gea_cpp_key, gea_cpp_queue_microtask, gea::runtime::coerce::to_boolean — all
// defined earlier in the TU via runtime_pch.h). This is the reactivity FEATURE,
// so it lives in the framework (GPL), separate from the GPL+RLE language runtime.

#pragma once

static void __attribute__((noinline)) gea_cpp_run_reactive_apply(const std::shared_ptr<std::function<void()>> &apply) {
  if (apply && *apply) (*apply)();
}
static void __attribute__((noinline)) gea_cpp_schedule_reactive_apply(
  const std::shared_ptr<std::function<void()>> &apply,
  const std::shared_ptr<bool> &pending
) {
  if (!pending || *pending) return;
  *pending = true;
  gea_cpp_queue_microtask([apply, pending]() mutable -> void {
    if (pending) *pending = false;
    gea_cpp_run_reactive_apply(apply);
  });
}
// (Removed gea_cpp_bind_direct_reactive_apply — the dead "observe-direct"
// fast-path of the dynamic-store fallback. Its `__sym_GEA_OBSERVE_DIRECT` gate
// had no setter anywhere in source, so it always returned false and every caller
// fell through to the typed-store path. Deleted 2026-06-27.)

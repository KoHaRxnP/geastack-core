// SPDX-License-Identifier: Apache-2.0
#pragma once

// Placement of the engine's task-only state.
//
// A static object with a nonzero member initializer is .data: the compiler
// folds the initializer into the image and startup copies it into RAM. On
// parts where that copy has to land in internal SRAM while zero-initialized
// .bss can be swept to external RAM by a linker fragment, the sentinels in the
// engine's caches (-1 node ids, a serial of 1, 0xFFFF handles) pin whole
// tables to the memory an app may need for something else.
//
// GEA_EMBEDDED_UI_STATE_DYNAMIC_INIT=1 gives those objects a user-provided,
// never-inlined constructor instead. The compiler can no longer constant-fold
// the initialization, the object is .bss, and the constructor runs at startup
// after external RAM is mapped. Off by default: every initializer stays where
// the compiler puts it and nothing changes. A board turns it on together with
// the sweep; on ESP32 the targets' GEA_EMBEDDED_UI_STATE_EXTERNAL does both.
#ifndef GEA_EMBEDDED_UI_STATE_DYNAMIC_INIT
#define GEA_EMBEDDED_UI_STATE_DYNAMIC_INIT 0
#endif

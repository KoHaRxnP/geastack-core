// SPDX-License-Identifier: Apache-2.0
#pragma once
// Positive whole-program node reachability facts. Older or opaque builds
// retain every native node kind; applications do not opt into compact storage.
#ifndef GEA_UI_IMAGE_NODES
#define GEA_UI_IMAGE_NODES 1
#endif
#ifndef GEA_UI_INPUT_NODES
#define GEA_UI_INPUT_NODES 1
#endif

// Generated from the whole-source class-token bound. Unknown programs retain
// four inline entries. Overflow support is independent of this capacity.
#ifndef GEA_UI_CLASS_INLINE_TOKENS
#define GEA_UI_CLASS_INLINE_TOKENS 4
#endif
static_assert(GEA_UI_CLASS_INLINE_TOKENS >= 1 && GEA_UI_CLASS_INLINE_TOKENS <= 4,
              "Class inline capacity must be between one and four tokens");

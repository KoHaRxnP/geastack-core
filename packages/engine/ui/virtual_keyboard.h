// SPDX-License-Identifier: Apache-2.0
// Built-in on-screen QWERTY keyboard for pixel-backed targets (ESP32 etc.).
//
// Implemented as real document-tree nodes — `keyboard_root → row[0..3] →
// key[0..N] + label` — created the first time an `<input>` element gets
// focus and toggled via Property::Display thereafter. Each key carries a
// reserved pressId; a single body-level "click" listener intercepts taps
// in that pressId range and routes them into the focused input's `value`
// attribute, then dispatches synthetic `input` / `keydown` PointerEvents
// so JSX `onInput` / `onKeyDown` handlers fire the same way they do on
// macOS / browsers.
//
// Native-text-input targets disable this with
// GEA_EMBEDDED_ENABLE_VIRTUAL_KEYBOARD=0 and use their platform keyboard
// instead. Pixel-only targets keep it enabled as their text-entry path.

#pragma once

namespace gea::embedded::ui {

class VirtualKeyboard {
public:
	static VirtualKeyboard &instance();

	// Whether the keyboard is currently visible. Mirrors
	// `Tree::activeInputId() >= 0` but is cheap to query so the renderer
	// doesn't have to chase the Tree singleton.
	bool active() const;

	// Reset everything (called on app teardown so the next app starts
	// with a clean slate — node ids from the previous tree are stale).
	void reset();

	// Diagnostics: the keyboard root node id (-1 before first mount).
	int debugRootNode() const;

	// Reconcile keyboard visibility / contents against Tree state.
	// Called from touch_runtime after focus changes; the keyboard reads
	// `Tree::activeInputId()` to decide whether to show, mount nodes
	// lazily on first show, and updates the existing nodes on subsequent
	// shows. Idempotent — safe to call after every press.
	void sync();

	// True when nodeId is inside the keyboard subtree (root or any
	// descendant — label texts, individual keys, rows). touch_runtime
	// asks before running its focus-management logic so taps that
	// land anywhere on the keyboard don't accidentally clear the
	// focused input (which would close the keyboard mid-typing).
	// pressId-only checks miss this case because hitTestNode returns
	// the deepest descendant (often the label text node), which
	// inherits the key's press id through bubble dispatch but doesn't
	// carry it directly on the attribute.
	bool containsNode(int nodeId) const;

private:
	VirtualKeyboard() = default;
};

}  // namespace gea::embedded::ui

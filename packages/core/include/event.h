#pragma once

namespace gea::framework::events {

enum class EventType {
	Touch,
	Frame,
	Timeout,
	SettingsToggle,
};

enum class TouchPhase {
	Down = 1,
	Move = 2,
	Up = 3,
};

struct Event {
	EventType type = EventType::Frame;
	TouchPhase touchPhase = TouchPhase::Down;
	bool touching = false;
	int data = 0;
	int x = 0;
	int y = 0;
	// Multi-touch pointer index. 0 = primary pointer (full gesture: scroll,
	// click, focus, keyboard). >=1 = additional simultaneous fingers, which fire
	// per-node touch/pointer/press events only (e.g. holding a move button while
	// tapping jump). Single-touch sources (ESP32 panel, mouse) always use 0.
	int pointerId = 0;
};

}  // namespace gea::framework::events

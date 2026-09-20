#pragma once

namespace gea::platform::touch {

enum class Phase {
	Down = 1,
	Move = 2,
	Up = 3,
};

class Touchscreen {
public:
	using Observer = void (*)(Phase phase, bool touching, int x, int y);
	// Multi-finger variant: same event plus a 0-based finger index (0 = primary).
	// Controllers that support simultaneous touches (e.g. the GT911) report every
	// finger through this. Single-touch controllers leave it unimplemented; the
	// weak default in touch_runtime.cpp is a no-op, so those targets keep using
	// the single Observer with no code change.
	using PointerObserver = void (*)(Phase phase, bool touching, int x, int y, int pointerId);

	static void setObserver(Observer observer);
	// Optional: register a multi-finger observer. Default (weak) no-op.
	static void setPointerObserver(PointerObserver observer);
	static bool init();
	// Optional event-loop polling hook. Controllers backed by interrupts or their
	// own driver task use the weak no-op implementation.
	static void poll(int nowMs);
	static int read(int *x, int *y);
	static int readCached(int *x, int *y);
	static void consumeLatestMove(int *x, int *y);
	// Inject a synthetic touch event through the same path as the hardware
	// reader: updates the latest-move cache (so consumeLatestMove returns these
	// coords) and notifies the observer. Used by the debug FIFO so injected
	// drags behave exactly like a real finger.
	static void injectEvent(Phase phase, bool touching, int x, int y);
};

}  // namespace gea::platform::touch

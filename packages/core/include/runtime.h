#pragma once

// Optional application boot hook. An app that ships native sources can define
// `gea_app_native_boot` to bring its own hardware up before the runtime claims
// the display, the radios and the frame loop. It is a weak *declaration* with
// no default definition: an app that does not provide one leaves the symbol
// null and Runtime::run() skips the call.
extern "C" void gea_app_native_boot(void) __attribute__((weak));

// Bytes of internal, DMA-capable RAM to hold back for the display across the
// app's native boot hook. The app hook runs FIRST, by design -- an app that owns
// hardware needs its large contiguous internal allocations before the panel
// takes what is left -- but an app that fills internal RAM leaves the display
// unable to bring its bus up at all: the failure this exists for was 3,236 bytes
// free, largest block 1,600, and "SPI bus init failed: ESP_ERR_NO_MEM" one
// millisecond after the framebuffer landed in PSRAM. So Runtime::boot() takes a
// reserve through gea::platform::memory::Memory before the hook and releases it
// immediately before Display::init(), which turns an out-of-memory panel into
// the app giving up its last few kilobytes. 0 disables the reserve, which is
// every board that has never needed it. Declare it with `gea.defines`.
//
// Reserving for the WHOLE hook is the blunt version, and it is wrong for an app
// whose hook has a mandatory phase and a greedy one. The pedal is the case: its
// model arenas are a 213,472-byte bin-packing problem against 217,592 free, and
// holding anything back there makes the packer fail or time out, while the
// reverb promotion that runs straight afterwards takes 13 KB purely because it
// is there. Such an app leaves this at 0 and calls holdDisplayReserve() itself,
// between the two phases. See that method.
// A board that declares neither a panel nor a canvas has nothing to render to.
// The runtime then builds no framebuffer, no app tree and no frame loop; the
// board's own boot hook still runs, so a headless appliance keeps everything
// that is not the UI. Boards that say nothing keep a display, as they always
// have.
#ifndef GEA_EMBEDDED_NO_DISPLAY
#define GEA_EMBEDDED_NO_DISPLAY 0
#endif

#ifndef GEA_EMBEDDED_DISPLAY_INTERNAL_RESERVE_BYTES
#define GEA_EMBEDDED_DISPLAY_INTERNAL_RESERVE_BYTES 0
#endif

namespace gea::framework {

struct RuntimeOptions {
	int width = 0;
	int height = 0;
};

class Runtime {
public:
	static void run(const RuntimeOptions &options);
	// Runs the app's native boot hook, once. run() calls this itself, so an app
	// never has to; a target calls it first when it wants the hook to run on a
	// different task than the frame loop. That is not a stylistic choice: the
	// hook is where an app initialises flash-backed things (NVS, a filesystem,
	// its own partitions), and a task that performs a flash operation must be on
	// an internal stack, while a frame loop that only renders can live on an
	// external one. A target that keeps the loop on a PSRAM stack therefore runs
	// the hook on the bring-up task and hands the loop over afterwards.
	static void runNativeBoot();

	// Bring-up only: services, storage, radios, display, the app's first mount,
	// the frame scheduler -- everything up to but not including the event loop.
	// It touches flash, so it must run on a task whose stack is in INTERNAL RAM;
	// a target that runs the loop on an external stack calls this from its
	// bring-up task and then lets run() do nothing but loop. Idempotent: run()
	// calls it, so a target that does not care never sees a difference.
	static bool boot(const RuntimeOptions &options);

	// Hands the runtime a block of internal RAM to hold for the display and free
	// immediately before Display::init(). For an app whose native boot hook has
	// a phase that must not see less RAM than it actually has, followed by one
	// that will opportunistically eat whatever is left: take the block with
	// gea::platform::memory::Memory::reserveInternalDma() at the moment in
	// between and pass it here. nullptr is accepted and means no reserve, so a
	// failed reservation needs no check at the call site. Called more than once,
	// the later block is held and the earlier one is freed at once -- the
	// runtime holds exactly one app reserve, in addition to the one
	// GEA_EMBEDDED_DISPLAY_INTERNAL_RESERVE_BYTES asks for.
	static void holdDisplayReserve(void *reserve);
};

}  // namespace gea::framework

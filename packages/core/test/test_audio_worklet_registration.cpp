// Includes the compiler-emitted TypeScript fixture from the existing test output.
#include "test_audio_worklet_compiled.cpp"
#include <cassert>

namespace {
unsigned registrations = 0;
}
namespace gea::host {
AudioWorkletProcessor::AudioWorkletProcessor() = default;
namespace audio_worklet {
workers::MessagePort processorPort() { return {}; }
void registerProcessor(const std::string& name, ProcessorFactory factory) {
  assert(name == "typed-copy" || name == "typed-inherited-copy");
  const auto processor = factory();
  const AudioWorkletBus inputs{{{0.25f, -0.5f, 0.75f}}};
  AudioWorkletBus outputs{{{0.0f, 0.0f, 0.0f}}};
  assert(processor->process(inputs, outputs, {}));
  assert(outputs == inputs);
  ++registrations;
}
}
namespace websocket {
// The runtime clears handlers only through the checked SDK setters; capture
// release on those setters is the provider's own contract (run_websocket_desktop).
unsigned cleared = 0;
void set_open_callback(NativeWebSocketHandle handle, std::function<void()> cb) {
  assert(handle == 77 && !cb);
  ++cleared;
}
void set_message_callback(NativeWebSocketHandle handle, std::function<void(const std::string&)> cb) {
  assert(handle == 77 && !cb);
  ++cleared;
}
void set_close_callback(NativeWebSocketHandle handle, std::function<void(int, const std::string&)> cb) {
  assert(handle == 77 && !cb);
  ++cleared;
}
void set_error_callback(NativeWebSocketHandle handle, std::function<void(const std::string&)> cb) {
  assert(handle == 77 && !cb);
  ++cleared;
}
}
}

int main() {
  // A normal typed processor registers without reading dynamic constructor properties.
  gea_worklet_probe_entry();
  assert(registrations == 2);

  // Standard event handler removal forwards an empty callback to the checked
  // SDK setter for every attribute.
  using namespace gea::runtime::hostevent;
  gea::host::WebSocket socket(gea::host::NativeWebSocketHandle{77});
  setWebSocketOnOpen(socket, nullptr);
  setWebSocketOnMessage(socket, nullptr);
  setWebSocketOnClose(socket, nullptr);
  setWebSocketOnError(socket, nullptr);
  assert(gea::host::websocket::cleared == 4);
}

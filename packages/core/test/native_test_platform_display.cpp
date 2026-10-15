// The native test host's display-orientation hooks, in a translation unit of
// their own that does not include host/display_orientation.h.
//
// That header declares both hooks weak, so a platform without them leaves them
// null. A definition seen after that weak declaration is itself weak; on ELF
// and Mach-O it still wins, but on Windows (COFF) a weak definition collides
// with the weak references every other unit makes ("duplicate symbol"), and
// the camera app pipelines failed to link. Defined here, without the
// declaration, they are the strong definitions those references resolve to on
// every host -- provided, on Windows, that this unit is linked before the units
// that reference them: lld-link takes a weak reference from a second unit for a
// duplicate definition when the strong one comes later (native-test-common.sh
// links this unit first).
namespace gea::framework::display {
enum class DisplayOrientation;
}

namespace gea::platform::display {
void applyOrientation(gea::framework::display::DisplayOrientation /*orientation*/) {}
bool autoRotateAllowed() { return true; }
}  // namespace gea::platform::display

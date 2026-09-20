// Compiles the host camera bridge (packages/host/host/camera.cpp) into the
// native test build so geatsc-generated camera apps can link
// gea::framework::camera::CameraBackend. The bridge transitively includes
// host headers (websocket/http/rtc) that gate gea_cpp_value-flavoured inline
// code on GEA_CPP_VALUE_AVAILABLE; the test build defines that macro
// globally, but only geatsc-generated TUs carry the runtime type — so drop
// it for this TU (same trick as native_test_host.cpp). The platform side is
// the null gea::platform::camera::Camera stub in native_test_host.cpp.
#pragma push_macro("GEA_CPP_VALUE_AVAILABLE")
#undef GEA_CPP_VALUE_AVAILABLE
#include "../../host/host/camera.cpp"
#pragma pop_macro("GEA_CPP_VALUE_AVAILABLE")

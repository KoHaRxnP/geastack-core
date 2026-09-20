// Link canary for multiple gea-vite -> geatsc programs in one binary.
// Each generated app uses --isolate-symbols plus a unique entry symbol.

#include <cstdio>
#include <functional>
#include <string>
#include <utility>
#include <vector>

extern void gea_isolated_counter_top_level();
extern void gea_isolated_launcher_top_level();
extern void gea_host_dom_dispatch_press(int press_id);
extern void gea_host_dom_flush_if_dirty();
extern void gea_host_dom_reset();
extern std::string gea_host_dom_root_text_content();
extern std::vector<std::string> gea_host_dom_button_texts();

namespace {
std::function<void(double)> pending_frame;
double next_frame_handle = 1.0;
std::string last_launched_app;
}  // namespace

double gea_host_request_animation_frame(std::function<void(double)> callback) {
  pending_frame = std::move(callback);
  return next_frame_handle++;
}

double gea_host_math_random() {
  return 0.5;
}

void gea_host_set_viewport(int width, int height) {
  (void)width;
  (void)height;
}

void gea_host_pump_frame(double timestamp_ms) {
  if (!pending_frame) return;
  auto callback = std::move(pending_frame);
  pending_frame = nullptr;
  callback(timestamp_ms);
}

extern "C" int gea_embedded_apps_launch(const char *app_id) {
  last_launched_app = app_id ? app_id : "";
  return 1;
}

void gea_host_ble_init(const std::string &device_name, double appearance, const std::string &mac_address) {
  (void)device_name;
  (void)appearance;
  (void)mac_address;
}

void gea_host_ble_start_advertising() {}

int main() {
  gea_host_dom_reset();
  gea_isolated_counter_top_level();

  const auto buttons = gea_host_dom_button_texts();
  if (buttons.size() != 3 || buttons[0] != "-" || buttons[1] != "+" || buttons[2] != "Reset") {
    std::fprintf(stderr, "[test_gea_multi_app_isolation] expected counter controls, got:");
    for (const auto &button : buttons) std::fprintf(stderr, " \"%s\"", button.c_str());
    std::fputc('\n', stderr);
    return 1;
  }

  gea_host_dom_dispatch_press(1);
  const auto text = gea_host_dom_root_text_content();
  if (text.find("Counter1-+ResetCounting up") == std::string::npos) {
    std::fprintf(stderr, "[test_gea_multi_app_isolation] expected reactive counter text, got: %s\n", text.c_str());
    return 1;
  }

  gea_host_dom_reset();
  gea_isolated_launcher_top_level();
  gea_host_dom_flush_if_dirty();

  const auto launcher_buttons = gea_host_dom_button_texts();
  if (launcher_buttons.size() != 12) {
    std::fprintf(stderr, "[test_gea_multi_app_isolation] expected 12 launcher buttons after reset, got %zu\n", launcher_buttons.size());
    return 1;
  }

  gea_host_dom_dispatch_press(0);
  if (last_launched_app != "analog-clock") {
    std::fprintf(stderr, "[test_gea_multi_app_isolation] expected analog-clock launch, got %s\n", last_launched_app.c_str());
    return 1;
  }

  return 0;
}

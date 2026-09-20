#include "host/device_control.h"

#include <cstdio>
#include <string>
#include <vector>

namespace {
std::vector<std::string> g_execCommands;
}

bool gea_companion_fake_exec_saw(const char *needle)
{
	const std::string text = needle ? needle : "";
	for (const auto &command : g_execCommands) {
		if (command.find(text) != std::string::npos) return true;
	}
	return false;
}

void gea_companion_fake_exec_dump(const char *label)
{
	std::fprintf(stderr, "[fake_companion_device_control] %s commands=%zu\n", label ? label : "exec", g_execCommands.size());
	for (const auto &command : g_execCommands) {
		std::fprintf(stderr, "  %s\n", command.c_str());
	}
}

std::string gea::host::DeviceControlFacade::exec(const std::string &command) const
{
	g_execCommands.push_back(command);
	if (command.find("firmware-helper.mjs") != std::string::npos && command.find(" list") != std::string::npos) {
		return "{\"builds\":[{\"key\":\"watch\",\"appId\":\"watch\",\"name\":\"Watch Firmware\",\"imagePath\":\"/repo/build/watch.bin\",\"sizeLabel\":\"1.4 MB\",\"updatedLabel\":\"just now\"},{\"key\":\"app-launcher\",\"appId\":\"app-launcher\",\"name\":\"Launcher Firmware\",\"imagePath\":\"/repo/build/launcher.bin\",\"sizeLabel\":\"5.7 MB\",\"updatedLabel\":\"1h ago\"}],\"message\":\"2 firmware builds found.\"}";
	}
	if (command.find("usb-monitor-helper.mjs") != std::string::npos && command.find(" status") != std::string::npos) {
		return "{\"running\":true,\"pid\":4242,\"port\":\"auto\",\"startedAt\":\"2026-06-01T00:00:00.000Z\",\"logPath\":\"/repo/docs/gea-companion/usb-monitor.log\",\"statePath\":\"/repo/docs/gea-companion/usb-monitor-state.json\"}";
	}
	if (command.find("usb-monitor-helper.mjs") != std::string::npos && command.find(" tail") != std::string::npos) {
		return "using serial port /dev/cu.usbmodem101\nGEADEV:PONG app=watch\nperf: fps=60.0";
	}
	if (command.find(" brightness ") != std::string::npos) return "";
	if (command.find(" brightness") != std::string::npos) return "73";
	if (command.find(" battery") != std::string::npos) return "88%";
	if (command.find(" facename") != std::string::npos) return "Digital";
	return "";
}

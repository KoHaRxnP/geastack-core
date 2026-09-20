// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <display.h>
#include "backends.h"
#include "ui/document.h"
#include "ui/style.h"
#include "ui/tree_internal.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <type_traits>
#include <vector>

namespace gea::framework::display {

enum class DisplayOrientation {
	PortraitPrimary = 0,
	PortraitSecondary = 1,
	LandscapePrimary = 2,
	LandscapeSecondary = 3,
};

}  // namespace gea::framework::display

namespace gea::platform::display {

#if defined(__GNUC__) || defined(__clang__)
void applyOrientation(gea::framework::display::DisplayOrientation orientation) __attribute__((weak));
bool autoRotateAllowed() __attribute__((weak));
#else
void applyOrientation(gea::framework::display::DisplayOrientation orientation);
bool autoRotateAllowed();
#endif

}  // namespace gea::platform::display

namespace gea::framework::display {

namespace detail {

inline bool orientationIsLandscape(DisplayOrientation orientation)
{
	return orientation == DisplayOrientation::LandscapePrimary || orientation == DisplayOrientation::LandscapeSecondary;
}

inline const char *orientationName(DisplayOrientation orientation)
{
	switch (orientation) {
	case DisplayOrientation::PortraitSecondary:
		return "portrait-secondary";
	case DisplayOrientation::LandscapePrimary:
		return "landscape-primary";
	case DisplayOrientation::LandscapeSecondary:
		return "landscape-secondary";
	case DisplayOrientation::PortraitPrimary:
	default:
		return "portrait-primary";
	}
}

inline DisplayOrientation parseOrientation(const std::string &value, DisplayOrientation fallback = DisplayOrientation::PortraitPrimary)
{
	if (value == "portrait-primary" || value == "portrait") return DisplayOrientation::PortraitPrimary;
	if (value == "portrait-secondary") return DisplayOrientation::PortraitSecondary;
	if (value == "landscape-primary" || value == "landscape") return DisplayOrientation::LandscapePrimary;
	if (value == "landscape-secondary") return DisplayOrientation::LandscapeSecondary;
	return fallback;
}

inline bool supportTokenMatches(const std::string &token, DisplayOrientation orientation)
{
	if (token == "all") return true;
	if (token == "portrait") return !orientationIsLandscape(orientation);
	if (token == "landscape") return orientationIsLandscape(orientation);
	return parseOrientation(token, DisplayOrientation::PortraitPrimary) == orientation && token != "portrait";
}

class DisplayOrientationState {
public:
	static int nativeWidth() { return gea::platform::display::kNativeWidth; }
	static int nativeHeight() { return gea::platform::display::kNativeHeight; }

	static int width() { return orientationIsLandscape(orientation()) ? nativeHeight() : nativeWidth(); }
	static int height() { return orientationIsLandscape(orientation()) ? nativeWidth() : nativeHeight(); }

	static DisplayOrientation orientation() { return orientation_; }
	static std::string orientationString() { return std::string(orientationName(orientation_)); }

	static void setOrientation(const std::string &value)
	{
		setOrientation(parseOrientation(value, orientation_));
	}

	static void setOrientation(DisplayOrientation next)
	{
		// A device that doesn't implement the rotation hook is fixed to its native
		// scanout orientation (PortraitPrimary); ignore other requests so the
		// app still renders natively instead of into a mismatched framebuffer.
		if (!platformSupportsRotation() && next != DisplayOrientation::PortraitPrimary) return;
		if (!supports(next)) return;
		if (orientation_ == next) return;
		orientation_ = next;
		applyMetrics();
	}

	static bool autoRotate() { return autoRotate_; }

	static void setAutoRotate(bool enabled)
	{
		if (enabled && !platformAllowsAutoRotate()) {
			autoRotate_ = false;
			return;
		}
		autoRotate_ = enabled;
		if (autoRotate_) {
			gea::framework::sensors::AccelerometerBackend::init();
			updateAutoRotationFromAccelerometer();
		}
	}

	static std::vector<std::string> supportedOrientations()
	{
		return supportedOrientations_;
	}

	static void setSupportedOrientations(const std::string &orientation)
	{
		setSupportedOrientations(std::vector<std::string>{orientation});
	}

	static void setSupportedOrientations(const std::vector<std::string> &orientations)
	{
		supportedOrientations_.clear();
		for (const auto &orientation : orientations) {
			if (orientation == "all" || orientation == "portrait" || orientation == "landscape" ||
			    orientation == "portrait-primary" || orientation == "portrait-secondary" ||
			    orientation == "landscape-primary" || orientation == "landscape-secondary") {
				if (std::find(supportedOrientations_.begin(), supportedOrientations_.end(), orientation) == supportedOrientations_.end()) {
					supportedOrientations_.push_back(orientation);
				}
			}
		}
		if (supportedOrientations_.empty()) supportedOrientations_.push_back("portrait-primary");
		// Without a platform rotation hook the device only supports its native
		// orientation, regardless of what the app asks for (so setSupported
		// Orientations('landscape') is effectively a no-op and the app renders).
		if (!platformSupportsRotation()) supportedOrientations_.assign(1, "portrait-primary");
		if (!supports(orientation_)) setOrientation(firstSupportedConcrete());
	}

	template <typename Options>
	static void setSupportedOrientationsFrom(const Options &options)
	{
		if constexpr (std::is_convertible_v<Options, std::string>) {
			setSupportedOrientations(static_cast<std::string>(options));
		} else if constexpr (requires { options.size(); options[0]; }) {
			std::vector<std::string> values;
			for (const auto &item : options) values.push_back(static_cast<std::string>(item));
			setSupportedOrientations(values);
		} else {
			setSupportedOrientations(static_cast<std::string>(options));
		}
	}

	static void updateAutoRotationFromAccelerometer()
	{
		if (!autoRotate_) return;
		updateAutoRotation(gea::framework::sensors::AccelerometerBackend::accelerationX(),
		                   gea::framework::sensors::AccelerometerBackend::accelerationY(),
		                   gea::framework::sensors::AccelerometerBackend::accelerationZ());
	}

	static void updateAutoRotation(double accelerationX, double accelerationY, double accelerationZ)
	{
		if (!autoRotate_) return;
		const double planar = std::sqrt(accelerationX * accelerationX + accelerationY * accelerationY);
		if (planar < std::max(3.0, std::fabs(accelerationZ) * 0.35)) return;
		DisplayOrientation next = orientation_;
		if (std::fabs(accelerationX) > std::fabs(accelerationY)) {
			next = accelerationX < 0 ? DisplayOrientation::LandscapePrimary : DisplayOrientation::LandscapeSecondary;
		} else {
			next = accelerationY > 0 ? DisplayOrientation::PortraitPrimary : DisplayOrientation::PortraitSecondary;
		}
		if (supports(next)) setOrientation(next);
	}

	static void mapPanelToViewport(int *x, int *y)
	{
		if (!x || !y) return;
		const int px = *x;
		const int py = *y;
		const int w = nativeWidth();
		const int h = nativeHeight();
		switch (orientation_) {
		case DisplayOrientation::PortraitSecondary:
			*x = w - 1 - px;
			*y = h - 1 - py;
			break;
		case DisplayOrientation::LandscapePrimary:
			*x = h - 1 - py;
			*y = px;
			break;
		case DisplayOrientation::LandscapeSecondary:
			*x = py;
			*y = w - 1 - px;
			break;
		case DisplayOrientation::PortraitPrimary:
		default:
			*x = px;
			*y = py;
			break;
		}
	}

private:
	// A device can leave its native scanout orientation only when the platform
	// provides the rotation hook (a strong gea::platform::display::applyOrientation,
	// e.g. the ESP32-P4 PPA path). The CO5300 QSPI boards don't (the panel has no
	// swap_xy and the S3 has no PPA), so for them the weak symbol stays null,
	// orientation is fixed to native, and setOrientation/setSupportedOrientations
	// become no-ops — a landscape-requesting app (e.g. maps) then renders in the
	// panel's native orientation instead of drawing into a mismatched (black) FB.
	static bool platformSupportsRotation()
	{
#if defined(__GNUC__) || defined(__clang__)
		return gea::platform::display::applyOrientation != nullptr;
#else
		return true;
#endif
	}

	static bool platformAllowsAutoRotate()
	{
#if defined(__GNUC__) || defined(__clang__)
		if (gea::platform::display::autoRotateAllowed) return gea::platform::display::autoRotateAllowed();
		return true;
#else
		return gea::platform::display::autoRotateAllowed();
#endif
	}

	static bool supports(DisplayOrientation orientation)
	{
		return std::any_of(supportedOrientations_.begin(), supportedOrientations_.end(), [orientation](const std::string &token) {
			return supportTokenMatches(token, orientation);
		});
	}

	static DisplayOrientation firstSupportedConcrete()
	{
		for (const auto &token : supportedOrientations_) {
			if (token == "all" || token == "portrait" || token == "portrait-primary") return DisplayOrientation::PortraitPrimary;
			if (token == "portrait-secondary") return DisplayOrientation::PortraitSecondary;
			if (token == "landscape" || token == "landscape-primary") return DisplayOrientation::LandscapePrimary;
			if (token == "landscape-secondary") return DisplayOrientation::LandscapeSecondary;
		}
		return DisplayOrientation::PortraitPrimary;
	}

	static void applyMetrics()
	{
		const int w = width();
		const int h = height();
		gea::embedded::ui::Document::setPreferredMountSize(w, h);
		gea::embedded::ui::setViewportMetrics(w, h, gea::embedded::ui::devicePixelRatio());
#if defined(__GNUC__) || defined(__clang__)
		if (gea::platform::display::applyOrientation) gea::platform::display::applyOrientation(orientation_);
#else
		gea::platform::display::applyOrientation(orientation_);
#endif
		auto &tree = gea::embedded::ui::Tree::instance();
		const int root = tree.mountedRoot();
		if (root >= 0) {
			auto &document = gea::embedded::ui::Document::instance();
			auto body = document.body();
			body.style().width(w);
			body.style().height(h);
			document.mount(body, w, h);
		}
	}

	inline static DisplayOrientation orientation_ = DisplayOrientation::PortraitPrimary;
	inline static bool autoRotate_ = false;
	inline static std::vector<std::string> supportedOrientations_{"portrait-primary"};
};

}  // namespace detail

}  // namespace gea::framework::display

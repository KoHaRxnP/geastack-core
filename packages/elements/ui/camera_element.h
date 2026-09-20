// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "node.h"
#include "pixel.h"

#include <cstdint>
#include <string>

namespace gea::embedded::ui {

struct Node;

// How the platform composites a <camera> leaf's live preview.
enum class CameraPreviewMode : std::uint8_t {
	// The runtime owns an RGB565 buffer that the backend fills each frame; the
	// preview is blitted through the normal display list, so it obeys CSS clip
	// / z-order / border-radius like a <canvas>. (esp32-p4, geaos)
	Framebuffer = 0,
	// The backend owns a native preview surface (e.g. an iOS
	// AVCaptureVideoPreviewLayer) that the runtime positions over the node's
	// computed rect. The preview floats above the gea framebuffer. (iOS)
	NativeOverlay = 1,
};

// CSS `object-fit`-style scaling of the camera frame into the node rect.
enum class CameraPreviewFit : std::uint8_t {
	Cover = 0,    // fill the rect, cropping overflow (default)
	Contain = 1,  // fit inside the rect, letterboxing
	Fill = 2,     // stretch to the rect, ignoring aspect
};

// Board-independent seam between the <camera> leaf node and the platform
// camera backend. Targets with a camera register a provider during init
// (see gea::framework::camera::registerCameraSurface); camera-less targets
// leave it null and a <camera> element simply renders nothing.
class CameraSurfaceProvider {
public:
	virtual ~CameraSurfaceProvider() = default;

	// Ensure the single active camera is open for the requested parameters.
	// Idempotent and cheap once streaming. `device` (a device id) overrides
	// `facing` when non-empty. Returns true when a live stream is available.
	virtual bool ensureOpen(const std::string &facing,
	                        const std::string &device,
	                        int preferredWidth,
	                        int preferredHeight) = 0;

	// True once frames are flowing.
	virtual bool isStreaming() = 0;

	virtual CameraPreviewMode previewMode() = 0;

	// Framebuffer mode: scale + convert the latest frame into the caller's
	// native-pixel buffer of size dstWidth x dstHeight (RGB565 on 16-bit targets,
	// RGBA8888 on full-colour targets like iOS). Returns true if written.
	virtual bool fillPreview(gea::framework::graphics::pixel::native_t *dst,
	                        int dstWidth,
	                        int dstHeight,
	                        CameraPreviewFit fit,
	                        bool mirror) = 0;

	// NativeOverlay mode: position / hide the native preview surface. The rect
	// is in framebuffer pixels (the node's computed layout box).
	virtual void positionPreviewLayer(int x, int y, int width, int height) = 0;
	virtual void hidePreviewLayer() = 0;

	// NativeOverlay mode: present one fresh frame into the positioned rect. Called
	// once per frame by the render tick (in place of marking the node dirty), so
	// the platform can paint the preview directly — e.g. the ESP32-P4 backend
	// PPA-scales the camera frame straight into the display framebuffer at the
	// rect and flushes only that region, skipping the display-list blit/replay.
	// Default no-op: targets whose preview floats above the gea framebuffer (iOS)
	// need nothing here.
	virtual void presentNativeOverlay() {}

	// Release the active stream when the owning <camera> node unmounts.
	virtual void release(int nodeId) = 0;
};

// Process-wide registry for the camera preview seam. Null until a platform
// backend registers a provider.
class CameraSurface {
public:
	static void setProvider(CameraSurfaceProvider *provider);
	static CameraSurfaceProvider *provider();
};

// The <camera> element handle. The surface is the node itself (positioned by
// CSS); the imperative controls (capturePhoto / startRecording / setExposure /
// …) lower to the gea::host::Camera facade since there is one active camera at
// a time, so this handle only needs node creation.
class CameraElement : public NodeHandle {
public:
	using NodeHandle::NodeHandle;
	static CameraElement create();
};

// CameraRenderer (the per-frame preview presentation for a NodeType::Camera
// leaf) is declared in internal.h alongside CanvasRenderer.

}  // namespace gea::embedded::ui

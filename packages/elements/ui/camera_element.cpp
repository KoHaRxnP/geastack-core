// SPDX-License-Identifier: Apache-2.0
#include "camera_element.h"

#include "canvas.h"
#include "internal.h"
#include "tree_internal.h"

#include <cstdlib>
#include <cstring>

namespace gea::embedded::ui {

namespace {

// Process-wide camera preview provider. Null on targets without a camera
// backend, in which case a <camera> element renders nothing.
CameraSurfaceProvider *gCameraProvider = nullptr;

CameraPreviewFit parseFit(const char *fit)
{
	if (fit) {
		if (std::strcmp(fit, "contain") == 0) return CameraPreviewFit::Contain;
		if (std::strcmp(fit, "fill") == 0) return CameraPreviewFit::Fill;
	}
	return CameraPreviewFit::Cover;
}

bool parseBool(const char *value)
{
	return value && (std::strcmp(value, "true") == 0 || std::strcmp(value, "1") == 0);
}

}  // namespace

void CameraSurface::setProvider(CameraSurfaceProvider *provider)
{
	gCameraProvider = provider;
}

CameraSurfaceProvider *CameraSurface::provider()
{
	return gCameraProvider;
}

CameraElement CameraElement::create()
{
	return CameraElement(Tree::instance().createCamera());
}

void CameraRenderer::positionNativeOverlay(const Node &node, CameraSurfaceProvider *provider)
{
	if (!provider) return;
	if (node.layout.width <= 0 || node.layout.height <= 0) return;
	int x0 = node.layout.x;
	int y0 = node.layout.y;
	int x1 = node.layout.x + node.layout.width - 1;
	int y1 = node.layout.y + node.layout.height - 1;
	ViewRenderer::transformedBounds(node, false, &x0, &y0, &x1, &y1);
	const int w = x1 - x0 + 1;
	const int h = y1 - y0 + 1;
	if (w <= 0 || h <= 0) return;
	provider->positionPreviewLayer(x0, y0, w, h);
}

void CameraRenderer::record(const Node &node)
{
	CameraSurfaceProvider *provider = CameraSurface::provider();
	if (!provider) return;

	Tree &tree = Tree::instance();
	const Node *nodes = tree.nodes();
	const int id = static_cast<int>(&node - nodes);

	// `facing`/`device` select the camera; `width`/`height` are capture
	// resolution hints (distinct from the node's on-screen layout size).
	const char *facing = tree.getAttribute(id, "facing");
	const char *device = tree.getAttribute(id, "device");
	const int preferredWidth = std::atoi(tree.getAttribute(id, "width"));
	const int preferredHeight = std::atoi(tree.getAttribute(id, "height"));
	if (!provider->ensureOpen(facing ? facing : "", device ? device : "", preferredWidth, preferredHeight)) return;
	if (!provider->isStreaming()) return;

	const int x = node.layout.x;
	const int y = node.layout.y;
	const int w = node.layout.width;
	const int h = node.layout.height;
	if (w <= 0 || h <= 0) return;

	if (provider->previewMode() == CameraPreviewMode::NativeOverlay) {
		// The platform owns the preview surface; keep it aligned to the node's
		// visual CSS rect, including transforms inherited from ancestors.
		positionNativeOverlay(node, provider);
		return;
	}

	// Framebuffer mode: fill an owned RGB565 buffer sized to the node rect, then
	// blit it like a canvas so the preview honours CSS clip / z-order.
	gea::framework::graphics::Canvas *surface = tree.ensureCameraSurface(id, w, h);
	if (!surface || !surface->pixels()) return;

	const CameraPreviewFit fit = parseFit(tree.getAttribute(id, "fit"));
	const bool mirror = parseBool(tree.getAttribute(id, "mirror"));
	if (!provider->fillPreview(surface->pixels(), surface->width(), surface->height(), fit, mirror)) return;

	DisplayCommand *cmd = DisplayList::instance().append();
	if (!cmd) return;
	cmd->type = DisplayCommandType::BlitImage;
	cmd->bx = static_cast<int16_t>(x);
	cmd->by = static_cast<int16_t>(y);
	cmd->bw = static_cast<int16_t>(w);
	cmd->bh = static_cast<int16_t>(h);
	cmd->blit.pixels = surface->pixels();
	cmd->blit.alpha = nullptr;
	cmd->blit.sourceWidth = static_cast<int16_t>(surface->width());
	cmd->blit.sourceHeight = static_cast<int16_t>(surface->height());
	cmd->blit.dx = static_cast<int16_t>(x);
	cmd->blit.dy = static_cast<int16_t>(y);
}

}  // namespace gea::embedded::ui

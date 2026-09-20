// SPDX-License-Identifier: Apache-2.0
#include "internal.h"
#include "state_init.h"
#include "tree_state.h"
#include <pixel.h>

#include <algorithm>
#include <cmath>
#include <cstdio>

#ifdef quad
#undef quad
#endif

#ifndef GEA_EMBEDDED_RENDER_HOT_SRAM
#define GEA_EMBEDDED_RENDER_HOT_SRAM 0
#endif

#if GEA_EMBEDDED_RENDER_HOT_SRAM
#define GEA_VIEW_HOT_SRAM_SECTION(name) __attribute__((noinline, noclone, section(".time_critical.gea_view." name)))
#else
#define GEA_VIEW_HOT_SRAM_SECTION(name)
#endif

namespace gea::embedded::ui {

constexpr double kPi = 3.14159265358979323846;
constexpr float kPiF = 3.14159265358979323846f;
// Reciprocal constants so the per-corner transform math uses hardware FPU
// multiplies. The ESP32-S3 (Xtensa LX7) FPU has mul/add/sub but NO divide
// instruction, so `/ 1000.0f` lowered to a software __divsf3 call — measured
// as the dominant per-call cost of applyNodeTransform (12 software divides ×
// ~2300 calls/frame on the spinning css-3d-cube). `* kInv1000f` is a single
// hardware mul; sub-pixel accurate for screen coords (the values feeding this
// are integer permille/pixel quantities <= ~512).
constexpr float kInv1000f = 1.0f / 1000.0f;
constexpr float kInv1800f = 1.0f / 1800.0f;
// Number of constant-color strips a transformed (3D/perspective) linear gradient
// is sliced into (each strip is a FillQuad). Measured cheap on-device (~20ms for
// all of css-3d-cube's face strips combined — the frame cost was the full-screen
// background gradient, not these), so favor smoothness.
constexpr int kTransformedGradientSteps = 72;

uint8_t combineAlpha(uint8_t parentAlpha, uint8_t localAlpha)
{
	return static_cast<uint8_t>((static_cast<int>(parentAlpha) * static_cast<int>(localAlpha) + 127) / 255);
}

bool borderIsSameOpaqueSolidBackground(const Node &node, uint8_t parentAlpha)
{
	return node.style.has_bg &&
	       node.style.bg_fill == 0 &&
	       node.style.bg_color == node.style.border_color &&
	       combineAlpha(parentAlpha, node.style.bg_alpha) == combineAlpha(parentAlpha, node.style.border_alpha);
}

void appendAlphaCommand(uint8_t alpha, int bx, int by, int bw, int bh)
{
	DisplayCommand *cmd = DisplayList::instance().append();
	if (!cmd) return;
	cmd->type = DisplayCommandType::SetAlpha;
	cmd->bx = bx; cmd->by = by; cmd->bw = bw; cmd->bh = bh;
	cmd->alpha.alpha = alpha;
}

uint16_t interpolateColor(uint16_t from, uint16_t to, int permille)
{
	if (permille <= 0) return from;
	if (permille >= 1000) return to;
	return gea::framework::graphics::pixel::blend(to, from, (permille * 255 + 500) / 1000);
}

uint8_t interpolateAlpha(uint8_t from, uint8_t to, int permille)
{
	if (permille <= 0) return from;
	if (permille >= 1000) return to;
	return static_cast<uint8_t>((static_cast<int>(from) * (1000 - permille) + static_cast<int>(to) * permille + 500) / 1000);
}

uint16_t interpolatePremultipliedColor(uint16_t fromColor, uint8_t fromAlpha,
                                       uint16_t toColor, uint8_t toAlpha,
                                       int permille)
{
	if (permille <= 0) return fromColor;
	if (permille >= 1000) return toColor;
	int fr = 0, fg = 0, fb = 0, tr = 0, tg = 0, tb = 0;
	gea::framework::graphics::pixel::unpackRgb565(fromColor, &fr, &fg, &fb);
	gea::framework::graphics::pixel::unpackRgb565(toColor, &tr, &tg, &tb);
	fr = (fr * 255 + 15) / 31;
	fg = (fg * 255 + 31) / 63;
	fb = (fb * 255 + 15) / 31;
	tr = (tr * 255 + 15) / 31;
	tg = (tg * 255 + 31) / 63;
	tb = (tb * 255 + 15) / 31;
	const int fromWeight = static_cast<int>(fromAlpha) * (1000 - permille);
	const int toWeight = static_cast<int>(toAlpha) * permille;
	const int weight = fromWeight + toWeight;
	if (weight <= 0) return toColor;
	const int r = (fr * fromWeight + tr * toWeight + weight / 2) / weight;
	const int g = (fg * fromWeight + tg * toWeight + weight / 2) / weight;
	const int b = (fb * fromWeight + tb * toWeight + weight / 2) / weight;
	// Panel-endian-aware: framebuffer pixels are byte-swapped when
	// GEA_EMBEDDED_PIXEL_PANEL_ENDIAN=1 (the endpoints above return the already
	// byte-swapped stop colors). Emitting raw rgb565FromRgb888 here made the
	// interpolated middle of transformed gradient faces land un-swapped → scrambled
	// hues on device. fromRgb888 == rgb565FromRgb888 on host.
	return gea::framework::graphics::pixel::fromRgb888(r, g, b);
}

int clampPermille(int value)
{
	if (value < 0) return 0;
	if (value > 1000) return 1000;
	return value;
}

int gradientToStop(const Node &node)
{
	const int toStop = rstyle(node.style).bg_gradient_to_stop > 0 ? rstyle(node.style).bg_gradient_to_stop : 1000;
	return std::max(1, toStop);
}

int stopRangePermille(int permille, int start, int end)
{
	if (permille <= start) return 0;
	if (permille >= end) return 1000;
	const int span = end - start;
	if (span <= 0) return 1000;
	return ((permille - start) * 1000 + span / 2) / span;
}

uint16_t gradientColorAt(const Node &node, int permille)
{
	permille = clampPermille(permille);
	const int toStop = gradientToStop(node);
	if (rstyle(node.style).bg_gradient_has_mid && rstyle(node.style).bg_gradient_mid_stop > 0 && rstyle(node.style).bg_gradient_mid_stop < toStop) {
		const int stop = rstyle(node.style).bg_gradient_mid_stop;
		if (permille <= stop)
			return interpolatePremultipliedColor(rstyle(node.style).bg_gradient_from_color,
			                                     rstyle(node.style).bg_gradient_from_alpha,
			                                     rstyle(node.style).bg_gradient_mid_color,
			                                     rstyle(node.style).bg_gradient_mid_alpha,
			                                     stopRangePermille(permille, 0, stop));
		return interpolatePremultipliedColor(rstyle(node.style).bg_gradient_mid_color,
		                                     rstyle(node.style).bg_gradient_mid_alpha,
		                                     rstyle(node.style).bg_gradient_to_color,
		                                     rstyle(node.style).bg_gradient_to_alpha,
		                                     stopRangePermille(permille, stop, toStop));
	}
	return interpolatePremultipliedColor(rstyle(node.style).bg_gradient_from_color,
	                                     rstyle(node.style).bg_gradient_from_alpha,
	                                     rstyle(node.style).bg_gradient_to_color,
	                                     rstyle(node.style).bg_gradient_to_alpha,
	                                     stopRangePermille(permille, 0, toStop));
}

uint8_t gradientAlphaAt(const Node &node, int permille)
{
	permille = clampPermille(permille);
	const int toStop = gradientToStop(node);
	if (rstyle(node.style).bg_gradient_has_mid && rstyle(node.style).bg_gradient_mid_stop > 0 && rstyle(node.style).bg_gradient_mid_stop < toStop) {
		const int stop = rstyle(node.style).bg_gradient_mid_stop;
		if (permille <= stop)
			return interpolateAlpha(rstyle(node.style).bg_gradient_from_alpha,
			                        rstyle(node.style).bg_gradient_mid_alpha,
			                        stopRangePermille(permille, 0, stop));
		return interpolateAlpha(rstyle(node.style).bg_gradient_mid_alpha,
		                        rstyle(node.style).bg_gradient_to_alpha,
		                        stopRangePermille(permille, stop, toStop));
	}
	return interpolateAlpha(rstyle(node.style).bg_gradient_from_alpha, rstyle(node.style).bg_gradient_to_alpha, stopRangePermille(permille, 0, toStop));
}

void appendFillRectWithAlpha(int x, int y, int w, int h, uint16_t color, uint8_t alpha, uint8_t parentAlpha, int bx, int by, int bw, int bh)
{
	const uint8_t effectiveAlpha = combineAlpha(parentAlpha, alpha);
	if (effectiveAlpha != parentAlpha) appendAlphaCommand(effectiveAlpha, bx, by, bw, bh);
	DisplayCommand *cmd = DisplayList::instance().append();
	if (cmd) {
		cmd->type = DisplayCommandType::FillRect;
		cmd->bx = bx; cmd->by = by; cmd->bw = bw; cmd->bh = bh;
		cmd->fill.x = x; cmd->fill.y = y;
		cmd->fill.w = w; cmd->fill.h = h;
		cmd->fill.color = color;
	}
	if (effectiveAlpha != parentAlpha) appendAlphaCommand(parentAlpha, bx, by, bw, bh);
}

void appendFillRectRaw(int x, int y, int w, int h, uint16_t color, int bx, int by, int bw, int bh)
{
	DisplayCommand *cmd = DisplayList::instance().append();
	if (!cmd) return;
	cmd->type = DisplayCommandType::FillRect;
	cmd->bx = bx; cmd->by = by; cmd->bw = bw; cmd->bh = bh;
	cmd->fill.x = x; cmd->fill.y = y;
	cmd->fill.w = w; cmd->fill.h = h;
	cmd->fill.color = color;
}

void appendFillQuadWithAlpha(const int16_t *xs, const int16_t *ys, uint16_t color, uint8_t alpha, uint8_t parentAlpha,
                             int bx, int by, int bw, int bh,
                             int lx = 0, int ly = 0, int lw = 0, int lh = 0, uint8_t reprojectMode = 0)
{
	const uint8_t effectiveAlpha = combineAlpha(parentAlpha, alpha);
	if (effectiveAlpha != parentAlpha) appendAlphaCommand(effectiveAlpha, bx, by, bw, bh);
	DisplayCommand *cmd = DisplayList::instance().append();
	if (cmd) {
		cmd->type = DisplayCommandType::FillQuad;
		cmd->bx = bx;
		cmd->by = by;
		cmd->bw = bw;
		cmd->bh = bh;
		cmd->quad.x0 = xs[0]; cmd->quad.y0 = ys[0];
		cmd->quad.x1 = xs[1]; cmd->quad.y1 = ys[1];
		cmd->quad.x2 = xs[2]; cmd->quad.y2 = ys[2];
		cmd->quad.x3 = xs[3]; cmd->quad.y3 = ys[3];
		cmd->quad.lx = static_cast<int16_t>(lx); cmd->quad.ly = static_cast<int16_t>(ly);
		cmd->quad.lw = static_cast<int16_t>(lw); cmd->quad.lh = static_cast<int16_t>(lh);
		cmd->quad.aux = 0;
		cmd->quad.color = color;
		cmd->quad.reprojectMode = reprojectMode;
	}
	if (effectiveAlpha != parentAlpha) appendAlphaCommand(parentAlpha, bx, by, bw, bh);
}

void appendFillQuadRaw(const int16_t *xs, const int16_t *ys, uint16_t color, int bx, int by, int bw, int bh,
                       int lx = 0, int ly = 0, int lw = 0, int lh = 0, uint8_t reprojectMode = 0)
{
	DisplayCommand *cmd = DisplayList::instance().append();
	if (!cmd) return;
	cmd->type = DisplayCommandType::FillQuad;
	cmd->bx = bx;
	cmd->by = by;
	cmd->bw = bw;
	cmd->bh = bh;
	cmd->quad.x0 = xs[0]; cmd->quad.y0 = ys[0];
	cmd->quad.x1 = xs[1]; cmd->quad.y1 = ys[1];
	cmd->quad.x2 = xs[2]; cmd->quad.y2 = ys[2];
	cmd->quad.x3 = xs[3]; cmd->quad.y3 = ys[3];
	cmd->quad.lx = static_cast<int16_t>(lx); cmd->quad.ly = static_cast<int16_t>(ly);
	cmd->quad.lw = static_cast<int16_t>(lw); cmd->quad.lh = static_cast<int16_t>(lh);
	cmd->quad.aux = 0;
	cmd->quad.color = color;
	cmd->quad.reprojectMode = reprojectMode;
}

void appendFillQuadStrokeSegmentRaw(const int16_t *xs, const int16_t *ys, uint16_t color,
                                    int bx, int by, int bw, int bh,
                                    float localX0, float localY0, float localX1, float localY1, float strokeWidth)
{
	DisplayCommand *cmd = DisplayList::instance().append();
	if (!cmd) return;
	cmd->type = DisplayCommandType::FillQuad;
	cmd->bx = bx;
	cmd->by = by;
	cmd->bw = bw;
	cmd->bh = bh;
	cmd->quad.x0 = xs[0]; cmd->quad.y0 = ys[0];
	cmd->quad.x1 = xs[1]; cmd->quad.y1 = ys[1];
	cmd->quad.x2 = xs[2]; cmd->quad.y2 = ys[2];
	cmd->quad.x3 = xs[3]; cmd->quad.y3 = ys[3];
	cmd->quad.lx = static_cast<int16_t>(std::lroundf(localX0 * 8.0f));
	cmd->quad.ly = static_cast<int16_t>(std::lroundf(localY0 * 8.0f));
	cmd->quad.lw = static_cast<int16_t>(std::lroundf(localX1 * 8.0f));
	cmd->quad.lh = static_cast<int16_t>(std::lroundf(localY1 * 8.0f));
	cmd->quad.aux = static_cast<int16_t>(std::lroundf(strokeWidth * 8.0f));
	cmd->quad.color = color;
	cmd->quad.reprojectMode = 2;
}

void boundsFromCorners(const int16_t *xs, const int16_t *ys, int *x0, int *y0, int *x1, int *y1);

void appendLinearGradientRectRaw(const Node &node, int x, int y, int w, int h,
                                 uint16_t fromColor, uint16_t midColor, uint16_t toColor,
                                 uint16_t midStop, uint16_t toStop, int16_t angle, uint8_t fromAlpha,
                                 uint8_t midAlpha, uint8_t toAlpha, uint8_t hasMid)
{
	DisplayCommand *cmd = DisplayList::instance().append();
	if (!cmd) return;
	cmd->type = DisplayCommandType::FillLinearGradient;
	cmd->bx = x;
	cmd->by = y;
	cmd->bw = w;
	cmd->bh = h;
	cmd->gradient.x = x;
	cmd->gradient.y = y;
	cmd->gradient.w = w;
	cmd->gradient.h = h;
	cmd->gradient.tl = node.style.border_radius[0];
	cmd->gradient.tr = node.style.border_radius[1];
	cmd->gradient.br = node.style.border_radius[2];
	cmd->gradient.bl = node.style.border_radius[3];
	cmd->gradient.fromColor = fromColor;
	cmd->gradient.midColor = midColor;
	cmd->gradient.toColor = toColor;
	cmd->gradient.midStop = midStop;
	cmd->gradient.toStop = toStop > 0 ? toStop : 1000;
	cmd->gradient.angle = angle;
	cmd->gradient.fromAlpha = fromAlpha;
	cmd->gradient.midAlpha = midAlpha;
	cmd->gradient.toAlpha = toAlpha;
	cmd->gradient.hasMid = hasMid;
}

void appendLinearGradientRectRaw(const Node &node, int x, int y, int w, int h)
{
	appendLinearGradientRectRaw(node,
	                            x,
	                            y,
	                            w,
	                            h,
	                            rstyle(node.style).bg_gradient_from_color,
	                            rstyle(node.style).bg_gradient_mid_color,
	                            rstyle(node.style).bg_gradient_to_color,
	                            rstyle(node.style).bg_gradient_mid_stop,
	                            rstyle(node.style).bg_gradient_to_stop,
	                            rstyle(node.style).bg_gradient_angle,
	                            rstyle(node.style).bg_gradient_from_alpha,
	                            rstyle(node.style).bg_gradient_mid_alpha,
	                            rstyle(node.style).bg_gradient_to_alpha,
	                            rstyle(node.style).bg_gradient_has_mid);
}

void appendRadialGradientRectRaw(const Node &node, int x, int y, int w, int h)
{
	DisplayCommand *cmd = DisplayList::instance().append();
	if (!cmd) return;
	cmd->type = DisplayCommandType::FillRadialGradient;
	cmd->bx = x;
	cmd->by = y;
	cmd->bw = w;
	cmd->bh = h;
	cmd->radialGradient.x = x;
	cmd->radialGradient.y = y;
	cmd->radialGradient.w = w;
	cmd->radialGradient.h = h;
	cmd->radialGradient.tl = node.style.border_radius[0];
	cmd->radialGradient.tr = node.style.border_radius[1];
	cmd->radialGradient.br = node.style.border_radius[2];
	cmd->radialGradient.bl = node.style.border_radius[3];
	cmd->radialGradient.cxPermille = rstyle(node.style).bg_radial_gradient_cx;
	cmd->radialGradient.cyPermille = rstyle(node.style).bg_radial_gradient_cy;
	cmd->radialGradient.rxPermille = rstyle(node.style).bg_radial_gradient_rx;
	cmd->radialGradient.ryPermille = rstyle(node.style).bg_radial_gradient_ry;
	cmd->radialGradient.fromColor = rstyle(node.style).bg_radial_gradient_from_color;
	cmd->radialGradient.toColor = rstyle(node.style).bg_radial_gradient_to_color;
	cmd->radialGradient.stopPermille = rstyle(node.style).bg_radial_gradient_stop;
	cmd->radialGradient.fromAlpha = rstyle(node.style).bg_radial_gradient_from_alpha;
	cmd->radialGradient.toAlpha = rstyle(node.style).bg_radial_gradient_to_alpha;
}

void appendDrawLineRaw(int x0, int y0, int x1, int y1, uint16_t color)
{
	DisplayCommand *cmd = DisplayList::instance().append();
	if (!cmd) return;
	cmd->type = DisplayCommandType::DrawLine;
	const int bx0 = std::min(x0, x1);
	const int by0 = std::min(y0, y1);
	const int bx1 = std::max(x0, x1);
	const int by1 = std::max(y0, y1);
	cmd->bx = bx0;
	cmd->by = by0;
	cmd->bw = bx1 - bx0 + 1;
	cmd->bh = by1 - by0 + 1;
	cmd->line.x0 = x0;
	cmd->line.y0 = y0;
	cmd->line.x1 = x1;
	cmd->line.y1 = y1;
	cmd->line.color = color;
}

void appendStrokeSegmentBandRaw(float x0, float y0, float x1, float y1, float width, uint16_t color,
                                float localX0 = 0.0f, float localY0 = 0.0f,
                                float localX1 = 0.0f, float localY1 = 0.0f,
                                bool reprojectable = false)
{
	// All-float with a single reciprocal (was double sqrt + 4 double divides per
	// band; called ~144x/frame for the cube's two rings). The Xtensa LX7 has no
	// hardware double and no divide, so this was a heavy chunk of the ring record.
	const float dx = x1 - x0;
	const float dy = y1 - y0;
	const float len2 = dx * dx + dy * dy;
	if (len2 < 1e-6f) return;
	const float inv = 1.0f / std::sqrt(len2);

	const float half = width * 0.5f;
	const float nx = -dy * half * inv;
	const float ny = dx * half * inv;
	const float ex = dx * 0.75f * inv;
	const float ey = dy * 0.75f * inv;
	int16_t xs[4] = {
	    static_cast<int16_t>(std::lroundf(x0 - ex + nx)),
	    static_cast<int16_t>(std::lroundf(x1 + ex + nx)),
	    static_cast<int16_t>(std::lroundf(x1 + ex - nx)),
	    static_cast<int16_t>(std::lroundf(x0 - ex - nx)),
	};
	int16_t ys[4] = {
	    static_cast<int16_t>(std::lroundf(y0 - ey + ny)),
	    static_cast<int16_t>(std::lroundf(y1 + ey + ny)),
	    static_cast<int16_t>(std::lroundf(y1 + ey - ny)),
	    static_cast<int16_t>(std::lroundf(y0 - ey - ny)),
	};
	int bx0, by0, bx1, by1;
	boundsFromCorners(xs, ys, &bx0, &by0, &bx1, &by1);
	if (reprojectable)
		appendFillQuadStrokeSegmentRaw(xs, ys, color, bx0, by0, bx1 - bx0 + 1, by1 - by0 + 1,
		                               localX0, localY0, localX1, localY1, width);
	else
		appendFillQuadRaw(xs, ys, color, bx0, by0, bx1 - bx0 + 1, by1 - by0 + 1);
}

bool gradientAlphaNearlyConstant(const Node &node)
{
	uint8_t minAlpha = std::min(rstyle(node.style).bg_gradient_from_alpha, rstyle(node.style).bg_gradient_to_alpha);
	uint8_t maxAlpha = std::max(rstyle(node.style).bg_gradient_from_alpha, rstyle(node.style).bg_gradient_to_alpha);
	if (rstyle(node.style).bg_gradient_has_mid) {
		minAlpha = std::min(minAlpha, rstyle(node.style).bg_gradient_mid_alpha);
		maxAlpha = std::max(maxAlpha, rstyle(node.style).bg_gradient_mid_alpha);
	}
	return maxAlpha - minAlpha <= 8;
}

bool hasAnyRadius(const Node &node)
{
	return node.style.border_radius[0] || node.style.border_radius[1] ||
	       node.style.border_radius[2] || node.style.border_radius[3] ||
	       node.style.border_radius_percent[0] != kUnset ||
	       node.style.border_radius_percent[1] != kUnset ||
	       node.style.border_radius_percent[2] != kUnset ||
	       node.style.border_radius_percent[3] != kUnset;
}

bool hasAnyPercentRadius(const Node &node)
{
	return node.style.border_radius_percent[0] != kUnset ||
	       node.style.border_radius_percent[1] != kUnset ||
	       node.style.border_radius_percent[2] != kUnset ||
	       node.style.border_radius_percent[3] != kUnset;
}

int integerSqrt(int n)
{
	if (n <= 0) return 0;
	int x = n;
	int y = (x + 1) / 2;
	while (y < x) {
		x = y;
		y = (x + n / x) / 2;
	}
	return x;
}

int normalizedRadius(int radius, int maxRadius)
{
	if (radius < 0) return 0;
	return radius > maxRadius ? maxRadius : radius;
}

void roundedNodeRowSpan(const Node &node, int y, int *x0, int *x1)
{
	const int maxRadius = std::min(node.layout.width / 2, node.layout.height / 2);
	const int tl = normalizedRadius(node.style.border_radius[0], maxRadius);
	const int tr = normalizedRadius(node.style.border_radius[1], maxRadius);
	const int br = normalizedRadius(node.style.border_radius[2], maxRadius);
	const int bl = normalizedRadius(node.style.border_radius[3], maxRadius);
	if ((tl | tr | br | bl) == 0) return;

	const int left = node.layout.x;
	const int right = node.layout.x + node.layout.width - 1;
	const int top = node.layout.y;
	const int bottom = node.layout.y + node.layout.height - 1;
	int rowLeft = left;
	int rowRight = right;

	if (tl > 0 && y >= top && y <= top + tl) {
		const int dy = top + tl - y;
		const int dx = integerSqrt(tl * tl - dy * dy);
		rowLeft = std::max(rowLeft, left + tl - dx);
	}
	if (bl > 0 && y >= bottom - bl && y <= bottom) {
		const int dy = y - (bottom - bl);
		const int dx = integerSqrt(bl * bl - dy * dy);
		rowLeft = std::max(rowLeft, left + bl - dx);
	}
	if (tr > 0 && y >= top && y <= top + tr) {
		const int dy = top + tr - y;
		const int dx = integerSqrt(tr * tr - dy * dy);
		rowRight = std::min(rowRight, right - tr + dx);
	}
	if (br > 0 && y >= bottom - br && y <= bottom) {
		const int dy = y - (bottom - br);
		const int dx = integerSqrt(br * br - dy * dy);
		rowRight = std::min(rowRight, right - br + dx);
	}

	*x0 = std::max(*x0, rowLeft);
	*x1 = std::min(*x1, rowRight);
}

bool isFullyRoundedShape(const Node &node)
{
	if (hasAnyPercentRadius(node)) return false;
	if (std::abs(node.layout.width - node.layout.height) > 1) return false;
	const int halfMin = std::min(node.layout.width, node.layout.height) / 2;
	if (halfMin <= 0) return false;
	const int minRadius = std::max(1, halfMin - 1);
	return node.style.border_radius[0] >= minRadius &&
	       node.style.border_radius[1] >= minRadius &&
	       node.style.border_radius[2] >= minRadius &&
	       node.style.border_radius[3] >= minRadius;
}

void resolvedBorderRadii8(const Node &node, int16_t rx8[4], int16_t ry8[4])
{
	const double width = static_cast<double>(std::max(0, static_cast<int>(node.layout.width)));
	const double height = static_cast<double>(std::max(0, static_cast<int>(node.layout.height)));
	double rx[4]{};
	double ry[4]{};
	for (int i = 0; i < 4; i++) {
		if (node.style.border_radius_percent[i] != kUnset) {
			const double p = static_cast<double>(node.style.border_radius_percent[i]) / 1000.0;
			rx[i] = std::max(0.0, width * p);
			ry[i] = std::max(0.0, height * p);
		} else {
			const double r = static_cast<double>(std::max(0, static_cast<int>(node.style.border_radius[i])));
			rx[i] = r;
			ry[i] = r;
		}
	}

	double scale = 1.0;
	const auto constrain = [&](double limit, double sum) {
		if (limit > 0.0 && sum > limit) scale = std::min(scale, limit / sum);
	};
	constrain(width, rx[0] + rx[1]);
	constrain(width, rx[3] + rx[2]);
	constrain(height, ry[0] + ry[3]);
	constrain(height, ry[1] + ry[2]);

	for (int i = 0; i < 4; i++) {
		rx8[i] = static_cast<int16_t>(std::max(0L, std::min(32767L, std::lround(rx[i] * scale * 8.0))));
		ry8[i] = static_cast<int16_t>(std::max(0L, std::min(32767L, std::lround(ry[i] * scale * 8.0))));
	}
}

bool resolvedCircularBorderRadii(const Node &node, int radii[4])
{
	int16_t rx8[4]{};
	int16_t ry8[4]{};
	resolvedBorderRadii8(node, rx8, ry8);
	for (int i = 0; i < 4; i++) {
		if (std::abs(static_cast<int>(rx8[i]) - static_cast<int>(ry8[i])) > 4) return false;
		const int radius8 = (static_cast<int>(rx8[i]) + static_cast<int>(ry8[i])) / 2;
		radii[i] = std::max(0, std::min(32767, (radius8 + 4) / 8));
	}
	return true;
}

void appendFillRoundedRectWithAlpha(const Node &node, uint8_t parentAlpha, const int *resolvedRadii = nullptr)
{
	const uint8_t effectiveAlpha = combineAlpha(parentAlpha, node.style.bg_alpha);
	if (effectiveAlpha != parentAlpha)
		appendAlphaCommand(effectiveAlpha, node.layout.x, node.layout.y, node.layout.width, node.layout.height);
	DisplayCommand *cmd = DisplayList::instance().append();
	if (cmd) {
		const int tl = resolvedRadii ? resolvedRadii[0] : node.style.border_radius[0];
		const int tr = resolvedRadii ? resolvedRadii[1] : node.style.border_radius[1];
		const int br = resolvedRadii ? resolvedRadii[2] : node.style.border_radius[2];
		const int bl = resolvedRadii ? resolvedRadii[3] : node.style.border_radius[3];
		cmd->type = DisplayCommandType::FillRoundedRect;
		cmd->bx = node.layout.x; cmd->by = node.layout.y; cmd->bw = node.layout.width; cmd->bh = node.layout.height;
		cmd->fillRoundedRect.x = node.layout.x; cmd->fillRoundedRect.y = node.layout.y;
		cmd->fillRoundedRect.w = node.layout.width; cmd->fillRoundedRect.h = node.layout.height;
		cmd->fillRoundedRect.tl = tl;
		cmd->fillRoundedRect.tr = tr;
		cmd->fillRoundedRect.br = br;
		cmd->fillRoundedRect.bl = bl;
		cmd->fillRoundedRect.color = node.style.bg_color;
	}
	if (effectiveAlpha != parentAlpha)
		appendAlphaCommand(parentAlpha, node.layout.x, node.layout.y, node.layout.width, node.layout.height);
}

bool appendResolvedCircularRoundedRectWithAlpha(const Node &node, uint8_t parentAlpha)
{
	int radii[4]{};
	if (!resolvedCircularBorderRadii(node, radii)) return false;
	appendFillRoundedRectWithAlpha(node, parentAlpha, radii);
	return true;
}

bool resolvedRadiiAreCircleLike(int w, int h, const int16_t rx8[4], const int16_t ry8[4])
{
	if (w <= 0 || h <= 0 || std::abs(w - h) > 1)
		return false;
	const int targetRx8 = w * 4;
	const int targetRy8 = h * 4;
	for (int i = 0; i < 4; i++) {
		if (std::abs(static_cast<int>(rx8[i]) - targetRx8) > 8)
			return false;
		if (std::abs(static_cast<int>(ry8[i]) - targetRy8) > 8)
			return false;
	}
	return true;
}

bool transformedCornersCircle(const int16_t xs[4], const int16_t ys[4], int *cx, int *cy, int *r)
{
	if (!cx || !cy || !r)
		return false;
	if (std::abs(static_cast<int>(xs[0]) - static_cast<int>(xs[3])) > 1 ||
	    std::abs(static_cast<int>(xs[1]) - static_cast<int>(xs[2])) > 1 ||
	    std::abs(static_cast<int>(ys[0]) - static_cast<int>(ys[1])) > 1 ||
	    std::abs(static_cast<int>(ys[3]) - static_cast<int>(ys[2])) > 1)
		return false;

	const float left = 0.5f * static_cast<float>(static_cast<int>(xs[0]) + static_cast<int>(xs[3]));
	const float right = 0.5f * static_cast<float>(static_cast<int>(xs[1]) + static_cast<int>(xs[2]));
	const float top = 0.5f * static_cast<float>(static_cast<int>(ys[0]) + static_cast<int>(ys[1]));
	const float bottom = 0.5f * static_cast<float>(static_cast<int>(ys[2]) + static_cast<int>(ys[3]));
	const float width = right - left;
	const float height = bottom - top;
	if (width <= 0.0f || height <= 0.0f || std::fabs(width - height) > 1.5f)
		return false;

	const int radius = static_cast<int>(std::lround(std::min(width, height) * 0.5f));
	if (radius <= 0)
		return false;
	*cx = static_cast<int>(std::lround((left + right) * 0.5f));
	*cy = static_cast<int>(std::lround((top + bottom) * 0.5f));
	*r = radius;
	return true;
}

void appendFillCircleWithAlpha(int cx, int cy, int r, gea::framework::graphics::pixel::native_t color, uint8_t effectiveAlpha, uint8_t parentAlpha)
{
	if (r <= 0)
		return;
	const int x = cx - r;
	const int y = cy - r;
	const int d = r * 2 + 1;
	if (effectiveAlpha != parentAlpha)
		appendAlphaCommand(effectiveAlpha, x, y, d, d);
	DisplayCommand *cmd = DisplayList::instance().append();
	if (cmd) {
		cmd->type = DisplayCommandType::FillCircle;
		cmd->bx = static_cast<int16_t>(x);
		cmd->by = static_cast<int16_t>(y);
		cmd->bw = static_cast<int16_t>(d);
		cmd->bh = static_cast<int16_t>(d);
		cmd->fillCircle.cx = static_cast<int16_t>(cx);
		cmd->fillCircle.cy = static_cast<int16_t>(cy);
		cmd->fillCircle.r = static_cast<int16_t>(r);
		cmd->fillCircle.color = color;
	}
	if (effectiveAlpha != parentAlpha)
		appendAlphaCommand(parentAlpha, x, y, d, d);
}

void appendStrokeWithAlpha(const Node &node, uint8_t parentAlpha)
{
	const uint8_t effectiveAlpha = combineAlpha(parentAlpha, node.style.border_alpha);
	if (effectiveAlpha != parentAlpha)
		appendAlphaCommand(effectiveAlpha, node.layout.x, node.layout.y, node.layout.width, node.layout.height);

	DisplayCommand *cmd = DisplayList::instance().append();
	if (cmd) {
		// Canvas::strokeRect is deliberately the fast one-pixel primitive. Route
		// wider square CSS borders through the width-aware rounded-rect rasterizer
		// with zero radii instead of silently collapsing border-width to 1px.
		if (hasAnyRadius(node) || node.style.border_width > 1) {
			cmd->type = DisplayCommandType::StrokeRoundedRect;
			cmd->bx = node.layout.x; cmd->by = node.layout.y; cmd->bw = node.layout.width; cmd->bh = node.layout.height;
			cmd->strokeRoundedRect.x = node.layout.x; cmd->strokeRoundedRect.y = node.layout.y;
			cmd->strokeRoundedRect.w = node.layout.width; cmd->strokeRoundedRect.h = node.layout.height;
			cmd->strokeRoundedRect.tl = node.style.border_radius[0];
			cmd->strokeRoundedRect.tr = node.style.border_radius[1];
			cmd->strokeRoundedRect.br = node.style.border_radius[2];
			cmd->strokeRoundedRect.bl = node.style.border_radius[3];
			cmd->strokeRoundedRect.lineWidth = node.style.border_width;
			cmd->strokeRoundedRect.color = node.style.border_color;
		} else {
			cmd->type = DisplayCommandType::StrokeRect;
			cmd->bx = node.layout.x; cmd->by = node.layout.y; cmd->bw = node.layout.width; cmd->bh = node.layout.height;
			cmd->stroke.x = node.layout.x; cmd->stroke.y = node.layout.y;
			cmd->stroke.w = node.layout.width; cmd->stroke.h = node.layout.height;
			cmd->stroke.color = node.style.border_color;
		}
	}

	if (effectiveAlpha != parentAlpha)
		appendAlphaCommand(parentAlpha, node.layout.x, node.layout.y, node.layout.width, node.layout.height);
}

void boundsFromCorners(const int16_t *xs, const int16_t *ys, int *x0, int *y0, int *x1, int *y1)
{
	*x0 = *x1 = xs[0];
	*y0 = *y1 = ys[0];
	for (int i = 1; i < 4; i++) {
		if (xs[i] < *x0) *x0 = xs[i];
		if (xs[i] > *x1) *x1 = xs[i];
		if (ys[i] < *y0) *y0 = ys[i];
		if (ys[i] > *y1) *y1 = ys[i];
	}
}

class ViewGeometry {
public:
	// float (single-precision) so the per-corner transform math runs on the
	// ESP32-S3 hardware FPU. double here is software-emulated and was ~60% of a
	// spinning css-3d-cube frame (619ms of dlist record). float is sub-pixel
	// accurate for screen coords (<= ~512px) through the short transform chain.
	struct Point3 {
		float x;
		float y;
		float z;
	};

	static int nodeIndex(const Node &node)
	{
		const Node *nodes = Tree::instance().nodes();
		const auto index = &node - nodes;
		return index >= 0 && index < Tree::instance().nodeCount() ? static_cast<int>(index) : -1;
	}

	static bool nodeHasLocalTransform(const Node &node, bool usePrevious)
	{
		const RareStyle &rs = rstyle(node.style); // one pool lookup, not 10
		const int rotate = usePrevious ? node.render.previous_transform_rotate : rs.transform_rotate;
		const int rotateX = usePrevious ? node.render.previous_transform_rotate_x : rs.transform_rotate_x;
		const int rotateY = usePrevious ? node.render.previous_transform_rotate_y : rs.transform_rotate_y;
		const int tx = usePrevious ? node.render.previous_transform_translate_x : rs.transform_translate_x;
		const int ty = usePrevious ? node.render.previous_transform_translate_y : rs.transform_translate_y;
		const int tz = usePrevious ? node.render.previous_transform_translate_z : rs.transform_translate_z;
		const int txPercent = usePrevious ? node.render.previous_transform_translate_x_percent : rs.transform_translate_x_percent;
		const int tyPercent = usePrevious ? node.render.previous_transform_translate_y_percent : rs.transform_translate_y_percent;
		const int sx = usePrevious ? node.render.previous_transform_scale_x : rs.transform_scale_x;
		const int sy = usePrevious ? node.render.previous_transform_scale_y : rs.transform_scale_y;
		return (rotate % 3600) != 0 ||
		       (rotateX % 3600) != 0 ||
		       (rotateY % 3600) != 0 ||
		       tx != 0 ||
		       ty != 0 ||
		       tz != 0 ||
		       txPercent != 0 ||
		       tyPercent != 0 ||
		       sx != 1000 ||
		       sy != 1000;
	}

	// True iff any node carries a non-identity transform/perspective this frame
	// or last frame. Cached per refresh (keyed on refreshSerial): the DOM doesn't
	// change between refreshes, so one O(n) scan replaces the ancestor-chain walk
	// that hasTransformChain() would otherwise run for every node, every
	// recordBox. When no transform exists anywhere — the overwhelmingly common
	// case (e.g. tilt-breakout: no transforms at all) — the entire keyframe-3D
	// record path is bypassed at O(1).
	static bool GEA_VIEW_HOT_SRAM_SECTION("any_transform_present") anyTransformPresent()
	{
		auto &state = treeState();
		if (state.transformScanSerial == state.refreshSerial) return state.transformPresent;
		// Durable no-transform cache: a transform-free tree stays transform-free until a
		// transform property is set, and both setStyle(transform) sites clear
		// transformScanValid. So a cached `false` holds across mid-refresh refreshSerial
		// bumps with no re-scan — eliminating the scan-per-phase that made the mode gate
		// regress. (A cached `true` is intentionally not held: re-scan per refreshSerial
		// so a transform removal is noticed.)
		if (state.transformScanValid && !state.transformPresent) return false;
		bool present = false;
		for (int i = 0; i < state.nodeCount; i++) {
			const Node &n = state.nodes[i];
			if (nodeHasLocalTransform(n, false) || nodeHasLocalTransform(n, true) ||
			    rstyle(n.style).perspective > 0 || n.render.previous_perspective > 0) {
				present = true;
				break;
			}
		}
		state.transformPresent = present;
		state.transformScanSerial = state.refreshSerial;
		state.transformScanValid = true;
		return present;
	}

	static bool GEA_VIEW_HOT_SRAM_SECTION("has_transform_chain") hasTransformChain(const Node &node, bool usePrevious)
	{
		if (!anyTransformPresent()) return false;
		auto &state = treeState();
		for (int id = nodeIndex(node); id >= 0 && id < state.nodeCount; id = state.nodes[id].parent) {
			if (nodeHasLocalTransform(state.nodes[id], usePrevious)) return true;
			const int perspective = usePrevious ? state.nodes[id].render.previous_perspective : rstyle(state.nodes[id].style).perspective;
			if (perspective > 0) return true;
		}
		return false;
	}

	// Rotations take precomputed sin/cos (cached per node per frame by
	// nodeRotation) so applyNodeTransform doesn't re-evaluate software sin/cos
	// per corner per gradient strip — the dominant cost of rebuilding a
	// transformed subtree's display list each frame.
	static void rotateX(Point3 &p, float s, float c)
	{
		const float y = p.y * c - p.z * s;
		const float z = p.y * s + p.z * c;
		p.y = y;
		p.z = z;
	}

	static void rotateY(Point3 &p, float s, float c)
	{
		const float x = p.x * c + p.z * s;
		const float z = -p.x * s + p.z * c;
		p.x = x;
		p.z = z;
	}

	static void rotateZ(Point3 &p, float s, float c)
	{
		const float x = p.x * c - p.y * s;
		const float y = p.x * s + p.y * c;
		p.x = x;
		p.y = y;
	}

	// Full per-node affine transform for the current frame, precomputed once and
	// reused across every corner/strip projected through it.
	struct NodeTransformCache {
#if GEA_EMBEDDED_UI_STATE_DYNAMIC_INIT
		// Not inlined: the -1 node id would make the cache a .data object (see
		// state_init.h).
		__attribute__((noinline)) NodeTransformCache() {}
#endif
		int nodeId = -1;
		int x = 0, y = 0, w = 0, h = 0;
		int rotate = 0, rotateX = 0, rotateY = 0;
		int tx = 0, ty = 0, tz = 0, txPercent = 0, tyPercent = 0;
		int sx = 1000, sy = 1000, originX = 500, originY = 500;
		float ox = 0.0f, oy = 0.0f;        // transform-origin in absolute coords
		float scaleX = 1.0f, scaleY = 1.0f;
		float transX = 0.0f, transY = 0.0f, transZ = 0.0f;  // translate (incl % of size)
		float sinZ = 0.0f, cosZ = 1.0f, sinY = 0.0f, cosY = 1.0f, sinX = 0.0f, cosX = 1.0f;
		// Previous-frame coefficients (from render.previous_* / layout.previous_*),
		// filled LAZILY on first dirty-bounds (previous-frame) access so the hot
		// reproject path never pays for them. Lets the dirty-collect pass project
		// previous-frame corners with the same pure-FPU cached apply as the current
		// frame instead of re-walking the ancestor chain + recomputing 6 software
		// trig per corner (the css-3d-cube dirty cost).
		bool prevValid = false;
		float pOx = 0.0f, pOy = 0.0f;
		float pScaleX = 1.0f, pScaleY = 1.0f;
		float pTransX = 0.0f, pTransY = 0.0f, pTransZ = 0.0f;
		float pSinZ = 0.0f, pCosZ = 1.0f, pSinY = 0.0f, pCosY = 1.0f, pSinX = 0.0f, pCosX = 1.0f;
	};

	struct AverageDepthCacheEntry {
		int nodeId = -1;
		int depth = 0;
	};

	struct CornerTransformCacheEntry {
		int nodeId = -1;
		int16_t xs[4]{};
		int16_t ys[4]{};
	};

#ifndef GEA_EMBEDDED_UI_TRANSFORM_CACHE_SLOTS
#define GEA_EMBEDDED_UI_TRANSFORM_CACHE_SLOTS 64
#endif
#ifndef GEA_EMBEDDED_UI_DEPTH_CACHE_SLOTS
#define GEA_EMBEDDED_UI_DEPTH_CACHE_SLOTS 64
#endif
#ifndef GEA_EMBEDDED_UI_CORNER_CACHE_SLOTS
#define GEA_EMBEDDED_UI_CORNER_CACHE_SLOTS 64
#endif
	static constexpr int kActiveTransformCacheSlots = GEA_EMBEDDED_UI_TRANSFORM_CACHE_SLOTS;
	static constexpr int kActiveDepthCacheSlots = GEA_EMBEDDED_UI_DEPTH_CACHE_SLOTS;
	static constexpr int kActiveCornerCacheSlots = GEA_EMBEDDED_UI_CORNER_CACHE_SLOTS;
	static_assert(kActiveTransformCacheSlots > 0);
	static_assert(kActiveDepthCacheSlots > 0);
	static_assert(kActiveCornerCacheSlots > 0);
	static_assert((kActiveTransformCacheSlots & (kActiveTransformCacheSlots - 1)) == 0);
	static_assert((kActiveDepthCacheSlots & (kActiveDepthCacheSlots - 1)) == 0);
	static_assert((kActiveCornerCacheSlots & (kActiveCornerCacheSlots - 1)) == 0);

	struct ActiveTransformCache {
#if GEA_EMBEDDED_UI_STATE_DYNAMIC_INIT
		// Not inlined: the all-ones serial would make the cache a .data object
		// (see state_init.h).
		__attribute__((noinline)) ActiveTransformCache() {}
#endif
		uint64_t serial = ~0ull;
		NodeTransformCache entries[kActiveTransformCacheSlots]{};
	};

	struct ActiveDepthCache {
		uint64_t serial = ~0ull;
		AverageDepthCacheEntry entries[kActiveDepthCacheSlots]{};
	};

	struct ActiveCornerCache {
		uint64_t serial = ~0ull;
		CornerTransformCacheEntry entries[kActiveCornerCacheSlots]{};
	};

	static void resetCache(NodeTransformCache *entries, int count)
	{
		for (int i = 0; i < count; i++) entries[i].nodeId = -1;
	}

	static void resetCache(AverageDepthCacheEntry *entries, int count)
	{
		for (int i = 0; i < count; i++) entries[i].nodeId = -1;
	}

	static void resetCache(CornerTransformCacheEntry *entries, int count)
	{
		for (int i = 0; i < count; i++) entries[i].nodeId = -1;
	}

	static void fillNodeTransform(NodeTransformCache &e, const Node &node, int id)
	{
		// Hoist the RareStyle pool lookup out of the per-field reads: rstyle() is an
		// out-of-line std::deque indexing (segmented, double pointer-chase, cache
		// miss on the S3). Calling it once per node instead of ~16x per fill is the
		// difference between a direct-field read and a per-field deque probe — the
		// transform hot path (reproject) re-fills changed nodes every frame.
		const RareStyle &rs = rstyle(node.style);
		e.x = node.layout.x;
		e.y = node.layout.y;
		e.w = node.layout.width;
		e.h = node.layout.height;
		e.rotate = rs.transform_rotate;
		e.rotateX = rs.transform_rotate_x;
		e.rotateY = rs.transform_rotate_y;
		e.tx = rs.transform_translate_x;
		e.ty = rs.transform_translate_y;
		e.tz = rs.transform_translate_z;
		e.txPercent = rs.transform_translate_x_percent;
		e.tyPercent = rs.transform_translate_y_percent;
		e.sx = rs.transform_scale_x;
		e.sy = rs.transform_scale_y;
		e.originX = rs.transform_origin_x;
		e.originY = rs.transform_origin_y;
		const float fw = static_cast<float>(node.layout.width);
		const float fh = static_cast<float>(node.layout.height);
		e.ox = static_cast<float>(node.layout.x) + fw * static_cast<float>(rs.transform_origin_x) * kInv1000f;
		e.oy = static_cast<float>(node.layout.y) + fh * static_cast<float>(rs.transform_origin_y) * kInv1000f;
		e.scaleX = static_cast<float>(rs.transform_scale_x) * kInv1000f;
		e.scaleY = static_cast<float>(rs.transform_scale_y) * kInv1000f;
		e.transX = static_cast<float>(rs.transform_translate_x) +
		           fw * static_cast<float>(rs.transform_translate_x_percent) * kInv1000f;
		e.transY = static_cast<float>(rs.transform_translate_y) +
		           fh * static_cast<float>(rs.transform_translate_y_percent) * kInv1000f;
		e.transZ = static_cast<float>(rs.transform_translate_z);
		const float az = static_cast<float>(rs.transform_rotate) * kPiF * kInv1800f;
		const float ay = static_cast<float>(rs.transform_rotate_y) * kPiF * kInv1800f;
		const float ax = static_cast<float>(rs.transform_rotate_x) * kPiF * kInv1800f;
		e.sinZ = sinf(az); e.cosZ = cosf(az);
		e.sinY = sinf(ay); e.cosY = cosf(ay);
		e.sinX = sinf(ax); e.cosX = cosf(ax);
		// Previous-frame coefficients are filled lazily (only the dirty-bounds pass
		// needs them); mark invalid so the first prev access recomputes for this id.
		e.prevValid = false;
		e.nodeId = id;
	}

	// Previous-frame analogue of fillNodeTransform: precompute one node's transform
	// coefficients from its SNAPSHOTTED previous-frame fields (render.previous_* +
	// layout.previous_*). Lets transformRectCorners' previous-frame path gather the
	// chain ONCE and apply pure-FPU to all 4 corners — instead of re-walking the
	// ancestor chain and recomputing 6 software trig PER CORNER (the css-3d-cube
	// dirty-collect's dominant cost: ~280 applyNodeTransform calls/frame). The math
	// mirrors applyNodeTransform's usePrevious branch exactly, so the projected
	// corners are bit-identical — no visual change.
	static void fillNodeTransformPrev(NodeTransformCache &e, const Node &node)
	{
		const int x = node.layout.previous_x;
		const int y = node.layout.previous_y;
		const int w = node.layout.previous_width;
		const int h = node.layout.previous_height;
		const int rotate = node.render.previous_transform_rotate;
		const int rotateXValue = node.render.previous_transform_rotate_x;
		const int rotateYValue = node.render.previous_transform_rotate_y;
		const int tx = node.render.previous_transform_translate_x;
		const int ty = node.render.previous_transform_translate_y;
		const int tz = node.render.previous_transform_translate_z;
		const int txPercent = node.render.previous_transform_translate_x_percent;
		const int tyPercent = node.render.previous_transform_translate_y_percent;
		const int sx = node.render.previous_transform_scale_x;
		const int sy = node.render.previous_transform_scale_y;
		const int originX = node.render.previous_transform_origin_x;
		const int originY = node.render.previous_transform_origin_y;
		const float fw = static_cast<float>(w);
		const float fh = static_cast<float>(h);
		e.pOx = static_cast<float>(x) + fw * static_cast<float>(originX) * kInv1000f;
		e.pOy = static_cast<float>(y) + fh * static_cast<float>(originY) * kInv1000f;
		e.pScaleX = static_cast<float>(sx) * kInv1000f;
		e.pScaleY = static_cast<float>(sy) * kInv1000f;
		e.pTransX = static_cast<float>(tx) + fw * static_cast<float>(txPercent) * kInv1000f;
		e.pTransY = static_cast<float>(ty) + fh * static_cast<float>(tyPercent) * kInv1000f;
		e.pTransZ = static_cast<float>(tz);
		const float az = static_cast<float>(rotate) * kPiF * kInv1800f;
		const float ay = static_cast<float>(rotateYValue) * kPiF * kInv1800f;
		const float ax = static_cast<float>(rotateXValue) * kPiF * kInv1800f;
		e.pSinZ = sinf(az); e.pCosZ = cosf(az);
		e.pSinY = sinf(ay); e.pCosY = cosf(ay);
		e.pSinX = sinf(ax); e.pCosX = cosf(ax);
	}

	[[maybe_unused]] static bool nodeTransformMatches(const NodeTransformCache &e, const Node &node, int id)
	{
		// One RareStyle pool lookup, not one per compared field (see fillNodeTransform).
		const RareStyle &rs = rstyle(node.style);
		return e.nodeId == id &&
		       e.x == node.layout.x &&
		       e.y == node.layout.y &&
		       e.w == node.layout.width &&
		       e.h == node.layout.height &&
		       e.rotate == rs.transform_rotate &&
		       e.rotateX == rs.transform_rotate_x &&
		       e.rotateY == rs.transform_rotate_y &&
		       e.tx == rs.transform_translate_x &&
		       e.ty == rs.transform_translate_y &&
		       e.tz == rs.transform_translate_z &&
		       e.txPercent == rs.transform_translate_x_percent &&
		       e.tyPercent == rs.transform_translate_y_percent &&
		       e.sx == rs.transform_scale_x &&
		       e.sy == rs.transform_scale_y &&
		       e.originX == rs.transform_origin_x &&
		       e.originY == rs.transform_origin_y;
	}

	static NodeTransformCache *cachedNodeTransform(const Node &node)
	{
		static ActiveTransformCache cache;
		const int id = nodeIndex(node);
		if (id < 0) return nullptr;
		const uint64_t serial = treeState().refreshSerial;
		if (cache.serial != serial) {
			resetCache(cache.entries, kActiveTransformCacheSlots);
			cache.serial = serial;
		}
		NodeTransformCache &e = cache.entries[id & (kActiveTransformCacheSlots - 1)];
		// The cache was reset above on a serial change, and the STYLE half of a
		// transform is fixed within a refresh serial (the serial bumps whenever the
		// tree re-records / a style mutates). So a matching id lets us skip the
		// per-call nodeTransformMatches style comparison (an rstyle() pool lookup + 16
		// field compares against the scattered RareStyle entry) that the reproject
		// otherwise pays ~72x/frame (12 commands x ~6 ancestors) — the dominant
		// residual reproject cost vs spine's inline-ComputedStyle reads.
		//
		// The LAYOUT half is not covered by that argument: transform-origin and
		// percentage translate are resolved against the node's own box, and layout can
		// change without bumping the refresh serial (layout runs inside a refresh, and
		// callers such as hit-testing read geometry between refreshes). A stale ox/oy/
		// transX/transY silently projects the current rect through the PREVIOUS box's
		// coefficients — e.g. translate(-50%,-50%) of a 480x16 button still shifting by
		// (-240,-8) after the button became 47x47, so hit-testing answered for a rect
		// that is nowhere on screen. Four int compares against fields the caller has
		// already touched (node.layout is hot) is cheap next to the deque probe, and it
		// keeps the fast path honest.
		if (e.nodeId == id) {
			if (e.x == node.layout.x && e.y == node.layout.y &&
			    e.w == node.layout.width && e.h == node.layout.height)
				return &e;
			fillNodeTransform(e, node, id);
			return &e;
		}
		if (e.nodeId != -1) return nullptr;
		fillNodeTransform(e, node, id);
		return &e;
	}

	// Current-frame transform coefficients for a node, computed once per refresh
	// and reused across every corner/strip projected through it.
	// transformRectCorners runs 4 corners x N strips per node (e.g. 4x72 for a
	// gradient face), and EACH applyNodeTransform previously recomputed the origin/
	// scale/translate (6 software __divsf3 — the ESP32-S3 FPU has no divide) plus 3
	// software sin/cos. That per-call arithmetic was the dominant cost of rebuilding
	// a transformed subtree's display list (measured ~38ms / ~2300 calls per spinning
	// css-3d-cube frame). Precomputing it per node collapses the hot path to ~12
	// hardware FPU mul/adds + a cache lookup. Keyed on refreshSerial: the transform
	// is fixed within a refresh, and the serial bumps when the tree re-records (the
	// animating cube each frame). Only the current frame is cached; the colder
	// previous-frame (dirty-bounds) path computes directly.
	static const NodeTransformCache &nodeTransform(const Node &node)
	{
		if (NodeTransformCache *e = cachedNodeTransform(node)) return *e;
		static NodeTransformCache fallback;
		fillNodeTransform(fallback, node, nodeIndex(node));
		return fallback;
	}

	// Pure hardware-FPU application of precomputed transform coefficients to a
	// point — no cache lookup, no divides, no trig. transformRectCorners hoists
	// the per-node cache lookup out of its 4-corner loop and calls this directly.
	static void applyCachedTransform(const NodeTransformCache &t, Point3 &p)
	{
		p.x -= t.ox;
		p.y -= t.oy;
		p.x *= t.scaleX;
		p.y *= t.scaleY;
		p.x += t.transX;
		p.y += t.transY;
		p.z += t.transZ;
		rotateZ(p, t.sinZ, t.cosZ);
		rotateY(p, t.sinY, t.cosY);
		rotateX(p, t.sinX, t.cosX);
		p.x += t.ox;
		p.y += t.oy;
	}

	// Previous-frame analogue of applyCachedTransform — pure-FPU application of the
	// lazily-cached previous-frame coefficients. Identical math to applyNodeTransform's
	// usePrevious branch, so the projected corner is bit-identical (no visual change).
	static void applyCachedTransformPrev(const NodeTransformCache &t, Point3 &p)
	{
		p.x -= t.pOx;
		p.y -= t.pOy;
		p.x *= t.pScaleX;
		p.y *= t.pScaleY;
		p.x += t.pTransX;
		p.y += t.pTransY;
		p.z += t.pTransZ;
		rotateZ(p, t.pSinZ, t.pCosZ);
		rotateY(p, t.pSinY, t.pCosY);
		rotateX(p, t.pSinX, t.pCosX);
		p.x += t.pOx;
		p.y += t.pOy;
	}

	// Gather the ancestor chain's cached transform coefficients for the current
	// frame, leaf-first. Returns the chain length, or -1 if it exceeds `cap` (the
	// caller then falls back to the per-ancestor walk). Hoisting this out of a
	// multi-point projection loop turns N points x chain cache-lookups into one
	// chain gather + pure-FPU application per point.
	static constexpr int kMaxChainDepth = 64;  // DOM nesting is shallow; far beyond any real tree
	static int GEA_VIEW_HOT_SRAM_SECTION("gather_transform_chain") gatherTransformChain(const Node &node, const NodeTransformCache **chain)
	{
		auto &state = treeState();
		int depth = 0;
		for (int id = nodeIndex(node); id >= 0 && id < state.nodeCount; id = state.nodes[id].parent) {
			if (depth >= kMaxChainDepth) return -1;
			NodeTransformCache *entry = cachedNodeTransform(state.nodes[id]);
			if (!entry) return -1;
			chain[depth++] = entry;
		}
		return depth;
	}

	// Previous-frame analogue of gatherTransformChain: gathers the same ancestor
	// chain, lazily filling each node's previous-frame coefficients on first access.
	// The dirty-bounds pass projects previous-frame corners through this so the
	// previous-frame projection is O(nodes) cached pure-FPU — matching the current
	// frame — instead of re-walking + recomputing 6 software trig per corner.
	static int gatherTransformChainPrev(const Node &node, const NodeTransformCache **chain)
	{
		auto &state = treeState();
		int depth = 0;
		for (int id = nodeIndex(node); id >= 0 && id < state.nodeCount; id = state.nodes[id].parent) {
			if (depth >= kMaxChainDepth) return -1;
			NodeTransformCache *entry = cachedNodeTransform(state.nodes[id]);
			if (!entry) return -1;
			if (!entry->prevValid) {
				fillNodeTransformPrev(*entry, state.nodes[id]);
				entry->prevValid = true;
			}
			chain[depth++] = entry;
		}
		return depth;
	}

	static void applyNodeTransform(const Node &node, bool usePrevious, Point3 &p)
	{
		// Hot path (current frame): use the per-node precomputed coefficients so
		// the projection is pure hardware-FPU mul/add — no software divides, no
		// int->float conversions, no per-call trig.
		if (!usePrevious) {
			applyCachedTransform(nodeTransform(node), p);
			return;
		}

		// Cold path (previous frame): only the dirty-region bounds pass needs this,
		// and previous-frame fields aren't cached, so compute inline.
		const int x = node.layout.previous_x;
		const int y = node.layout.previous_y;
		const int w = node.layout.previous_width;
		const int h = node.layout.previous_height;
		const int rotate = node.render.previous_transform_rotate;
		const int rotateXValue = node.render.previous_transform_rotate_x;
		const int rotateYValue = node.render.previous_transform_rotate_y;
		const int tx = node.render.previous_transform_translate_x;
		const int ty = node.render.previous_transform_translate_y;
		const int tz = node.render.previous_transform_translate_z;
		const int txPercent = node.render.previous_transform_translate_x_percent;
		const int tyPercent = node.render.previous_transform_translate_y_percent;
		const int sx = node.render.previous_transform_scale_x;
		const int sy = node.render.previous_transform_scale_y;
		const int originX = node.render.previous_transform_origin_x;
		const int originY = node.render.previous_transform_origin_y;

		const float ox = static_cast<float>(x) + static_cast<float>(w) * static_cast<float>(originX) * kInv1000f;
		const float oy = static_cast<float>(y) + static_cast<float>(h) * static_cast<float>(originY) * kInv1000f;
		p.x -= ox;
		p.y -= oy;
		p.x *= static_cast<float>(sx) * kInv1000f;
		p.y *= static_cast<float>(sy) * kInv1000f;
		p.x += static_cast<float>(tx) + static_cast<float>(w) * static_cast<float>(txPercent) * kInv1000f;
		p.y += static_cast<float>(ty) + static_cast<float>(h) * static_cast<float>(tyPercent) * kInv1000f;
		p.z += tz;
		rotateZ(p, sinf(static_cast<float>(rotate) * kPiF * kInv1800f), cosf(static_cast<float>(rotate) * kPiF * kInv1800f));
		rotateY(p, sinf(static_cast<float>(rotateYValue) * kPiF * kInv1800f), cosf(static_cast<float>(rotateYValue) * kPiF * kInv1800f));
		rotateX(p, sinf(static_cast<float>(rotateXValue) * kPiF * kInv1800f), cosf(static_cast<float>(rotateXValue) * kPiF * kInv1800f));
		p.x += ox;
		p.y += oy;
	}

	static int nearestPerspectiveNode(const Node &node, bool usePrevious)
	{
		if (!anyTransformPresent()) return -1;
		auto &state = treeState();
		for (int id = nodeIndex(node); id >= 0 && id < state.nodeCount; id = state.nodes[id].parent) {
			const int perspective = usePrevious ? state.nodes[id].render.previous_perspective : rstyle(state.nodes[id].style).perspective;
			if (perspective > 0) return id;
		}
		return -1;
	}

	static void applyPerspective(int perspectiveNode, bool usePrevious, Point3 &p)
	{
		if (perspectiveNode < 0) return;
		const Node &node = treeState().nodes[perspectiveNode];
		const int x = usePrevious ? node.layout.previous_x : node.layout.x;
		const int y = usePrevious ? node.layout.previous_y : node.layout.y;
		const int w = usePrevious ? node.layout.previous_width : node.layout.width;
		const int h = usePrevious ? node.layout.previous_height : node.layout.height;
		const RareStyle &rs = rstyle(node.style); // one pool lookup, not 3
		const int perspective = usePrevious ? node.render.previous_perspective : rs.perspective;
		const int originX = usePrevious ? node.render.previous_perspective_origin_x : rs.perspective_origin_x;
		const int originY = usePrevious ? node.render.previous_perspective_origin_y : rs.perspective_origin_y;
		if (perspective <= 0) return;
		const float poX = static_cast<float>(x) + static_cast<float>(w) * static_cast<float>(originX) * kInv1000f;
		const float poY = static_cast<float>(y) + static_cast<float>(h) * static_cast<float>(originY) * kInv1000f;
		float scale = static_cast<float>(perspective) / (static_cast<float>(perspective) - p.z);
		if (!std::isfinite(scale)) scale = 1.0f;
		if (scale < 0.05f) scale = 0.05f;
		if (scale > 8.0f) scale = 8.0f;
		p.x = poX + (p.x - poX) * scale;
		p.y = poY + (p.y - poY) * scale;
	}

	// Optional xs8/ys8 receive the same corners in 1/8-px fixed point (sub-pixel),
	// so adjacent faces' shared edges coincide and the rasterizer tiles them
	// watertight instead of cracking at int16 round-to-nearest straddles. Clamped to
	// int16 range (off-screen corners are clipped by the rasterizer regardless).
	static void GEA_VIEW_HOT_SRAM_SECTION("transform_rect_corners") transformRectCorners(const Node &node, bool usePrevious, int x, int y, int w, int h, int16_t *xs, int16_t *ys,
	                                 int16_t *xs8 = nullptr, int16_t *ys8 = nullptr)
	{
		Point3 points[4] = {
		    {(float)x, (float)y, 0.0f},
		    {(float)(x + w), (float)y, 0.0f},
		    {(float)(x + w), (float)(y + h), 0.0f},
		    {(float)x, (float)(y + h), 0.0f},
		};
		auto fx8 = [](float v) -> int16_t {
			long s = std::lroundf(v * 8.0f);
			return (int16_t)(s < -32768 ? -32768 : (s > 32767 ? 32767 : s));
		};
		auto &state = treeState();
		const int perspectiveNode = nearestPerspectiveNode(node, usePrevious);
		const NodeTransformCache *chain[kMaxChainDepth];
		// Both frames now gather the chain ONCE and apply pure-FPU cached coefficients
		// to all 4 corners. The previous-frame path (dirty-bounds collection) used to
		// re-walk the ancestor chain and recompute 6 software trig PER CORNER — the
		// dominant cost of the css-3d-cube dirty pass (~280 applyNodeTransform calls/
		// frame). It now matches the current frame: O(nodes) cached, O(1) per corner.
		const int depth = usePrevious ? gatherTransformChainPrev(node, chain) : gatherTransformChain(node, chain);
		if (depth >= 0) {
			// Apply the gathered chain to all 4 corners with pure FPU math — the
			// cache lookup happened once during the gather, not per corner.
			for (int i = 0; i < 4; i++) {
				for (int k = 0; k < depth; k++) {
					if (usePrevious)
						applyCachedTransformPrev(*chain[k], points[i]);
					else
						applyCachedTransform(*chain[k], points[i]);
				}
				applyPerspective(perspectiveNode, usePrevious, points[i]);
				xs[i] = (int16_t)std::lroundf(points[i].x);
				ys[i] = (int16_t)std::lroundf(points[i].y);
				if (xs8) { xs8[i] = fx8(points[i].x); ys8[i] = fx8(points[i].y); }
			}
			return;
		}
		// Pathologically deep chain (gather returned -1): per-corner walk fallback.
		for (int i = 0; i < 4; i++) {
			for (int id = nodeIndex(node); id >= 0 && id < state.nodeCount; id = state.nodes[id].parent)
				applyNodeTransform(state.nodes[id], usePrevious, points[i]);
			applyPerspective(perspectiveNode, usePrevious, points[i]);
			xs[i] = (int16_t)std::lroundf(points[i].x);
			ys[i] = (int16_t)std::lroundf(points[i].y);
			if (xs8) { xs8[i] = fx8(points[i].x); ys8[i] = fx8(points[i].y); }
		}
	}

	static void transformPoint(const Node &node, bool usePrevious, float x, float y, float z, int16_t *outX, int16_t *outY)
	{
		Point3 point{x, y, z};
		auto &state = treeState();
		const int perspectiveNode = nearestPerspectiveNode(node, usePrevious);
		for (int id = nodeIndex(node); id >= 0 && id < state.nodeCount; id = state.nodes[id].parent)
			applyNodeTransform(state.nodes[id], usePrevious, point);
		applyPerspective(perspectiveNode, usePrevious, point);
		*outX = static_cast<int16_t>(std::lroundf(point.x));
		*outY = static_cast<int16_t>(std::lroundf(point.y));
	}

	// Prepared projector for plotting MANY points through one node's transform
	// (e.g. an ellipse-stroke ring's ~60 segment vertices). Gathers the ancestor
	// chain's cached coefficients ONCE; each projectPoint is then pure FPU math
	// instead of re-walking the chain + cache lookup per point (current frame only).
	struct ChainProjector {
		const NodeTransformCache *chain[kMaxChainDepth];
		int depth = -1;          // -1 => chain too deep, fall back to per-point walk
		int perspectiveNode = -1;
		const Node *node = nullptr;
	};
	static ChainProjector GEA_VIEW_HOT_SRAM_SECTION("prepare_projector") prepareProjector(const Node &node)
	{
		ChainProjector pr;
		pr.node = &node;
		pr.perspectiveNode = nearestPerspectiveNode(node, false);
		pr.depth = gatherTransformChain(node, pr.chain);
		return pr;
	}
	static void GEA_VIEW_HOT_SRAM_SECTION("project_point") projectPoint(const ChainProjector &pr, float x, float y, float z, int16_t *outX, int16_t *outY)
	{
		Point3 p{x, y, z};
		if (pr.depth >= 0) {
			for (int k = 0; k < pr.depth; k++) applyCachedTransform(*pr.chain[k], p);
		} else {
			auto &state = treeState();
			for (int id = nodeIndex(*pr.node); id >= 0 && id < state.nodeCount; id = state.nodes[id].parent)
				applyNodeTransform(state.nodes[id], false, p);
		}
		applyPerspective(pr.perspectiveNode, false, p);
		*outX = static_cast<int16_t>(std::lroundf(p.x));
		*outY = static_cast<int16_t>(std::lroundf(p.y));
	}

	static int GEA_VIEW_HOT_SRAM_SECTION("average_depth_compute") averageDepthCompute(const Node &node, bool usePrevious)
	{
		const int x = usePrevious ? node.layout.previous_x : node.layout.x;
		const int y = usePrevious ? node.layout.previous_y : node.layout.y;
		const int w = usePrevious ? node.layout.previous_width : node.layout.width;
		const int h = usePrevious ? node.layout.previous_height : node.layout.height;
		const float fx = static_cast<float>(x), fy = static_cast<float>(y);
		const float fw = static_cast<float>(w), fh = static_cast<float>(h);
		Point3 points[5] = {
		    {fx, fy, 0.0f},
		    {fx + fw, fy, 0.0f},
		    {fx + fw, fy + fh, 0.0f},
		    {fx, fy + fh, 0.0f},
		    {fx + fw * 0.5f, fy + fh * 0.5f, 0.0f},
		};
		// Gather the ancestor chain once and apply to all 5 depth-probe points with
		// pure FPU math (same hoist as transformRectCorners). This is the z-sort's
		// hot path — without it each point re-did a cache lookup per ancestor.
		const NodeTransformCache *chain[kMaxChainDepth];
		const int depth = usePrevious ? gatherTransformChainPrev(node, chain) : gatherTransformChain(node, chain);
		if (depth >= 0) {
			for (int i = 0; i < 5; i++)
				for (int k = 0; k < depth; k++) {
					if (usePrevious)
						applyCachedTransformPrev(*chain[k], points[i]);
					else
						applyCachedTransform(*chain[k], points[i]);
				}
		} else {
			auto &state = treeState();
			for (int i = 0; i < 5; i++)
				for (int id = nodeIndex(node); id >= 0 && id < state.nodeCount; id = state.nodes[id].parent)
					applyNodeTransform(state.nodes[id], usePrevious, points[i]);
		}
		float sum = 0.0f;
		for (const Point3 &p : points) sum += p.z;
		return static_cast<int>(std::lroundf(sum * 10.0f / 5.0f));
	}

	// Painter's-depth for the current frame, cached per node per refresh. The
	// child z-sort's subtreeDepth() recurses every subtree at every tree level, so
	// without this each node's 5-projection depth was recomputed many times per
	// frame (the dominant ~16ms 'sort' cost on css-3d-cube). The previous-frame
	// path isn't cached (cold, used only for dirty bounds).
	static int GEA_VIEW_HOT_SRAM_SECTION("average_depth") averageDepth(const Node &node, bool usePrevious)
	{
		if (usePrevious) return averageDepthCompute(node, true);
		static ActiveDepthCache cache;
		const int id = nodeIndex(node);
		const uint64_t serial = treeState().refreshSerial;
		if (cache.serial != serial) {
			resetCache(cache.entries, kActiveDepthCacheSlots);
			cache.serial = serial;
		}
		if (id >= 0) {
			AverageDepthCacheEntry &e = cache.entries[id & (kActiveDepthCacheSlots - 1)];
			if (e.nodeId == id) return e.depth;
			if (e.nodeId != -1) return averageDepthCompute(node, false);
			const int d = averageDepthCompute(node, false);
			e.depth = d;
			e.nodeId = id;
			return d;
		}
		return averageDepthCompute(node, false);
	}

	// The node's four projected screen-space corners for the current frame, cached
	// per node per refresh. recordBox projects the same node rect for its bounds
	// AND its fill quad, and nodeOverlapsClip projects it a third time — caching
	// collapses those (and the dirty pass's current-frame half) to one projection.
	static void GEA_VIEW_HOT_SRAM_SECTION("transform_corners") transformCorners(const Node &node, bool usePrevious, int16_t *xs, int16_t *ys)
	{
		if (usePrevious) {
			// Cache previous-frame corners per node per refresh, mirroring the current-frame
			// path below. The dirty-bounds collect projects each node's previous corners
			// REDUNDANTLY — once while walking the dynamic container's whole subtree
			// (transformedSubtreeBoundsRect) and again when the child is visited as its own
			// dirty node — so without this the spinning cube re-projects every face/label's
			// previous corners 2-3x/frame. One cache lookup collapses that to once.
			static ActiveCornerCache prevCache;
			const int pid = nodeIndex(node);
			const uint64_t pserial = treeState().refreshSerial;
			if (prevCache.serial != pserial) {
				resetCache(prevCache.entries, kActiveCornerCacheSlots);
				prevCache.serial = pserial;
			}
			if (pid >= 0) {
				CornerTransformCacheEntry &e = prevCache.entries[pid & (kActiveCornerCacheSlots - 1)];
				if (e.nodeId != pid) {
					if (e.nodeId != -1) {
						// Slot collision with another node this refresh: compute uncached.
						transformRectCorners(node, true, node.layout.previous_x, node.layout.previous_y,
						                     node.layout.previous_width, node.layout.previous_height, xs, ys);
						return;
					}
					transformRectCorners(node, true, node.layout.previous_x, node.layout.previous_y,
					                     node.layout.previous_width, node.layout.previous_height, e.xs, e.ys);
					e.nodeId = pid;
				}
				for (int i = 0; i < 4; i++) { xs[i] = e.xs[i]; ys[i] = e.ys[i]; }
				return;
			}
			transformRectCorners(node, true, node.layout.previous_x, node.layout.previous_y,
			                     node.layout.previous_width, node.layout.previous_height, xs, ys);
			return;
		}
		static ActiveCornerCache cache;
		const int id = nodeIndex(node);
		const uint64_t serial = treeState().refreshSerial;
		if (cache.serial != serial) {
			resetCache(cache.entries, kActiveCornerCacheSlots);
			cache.serial = serial;
		}
		if (id >= 0) {
			CornerTransformCacheEntry &e = cache.entries[id & (kActiveCornerCacheSlots - 1)];
			if (e.nodeId != id) {
				if (e.nodeId != -1) {
					transformRectCorners(node, false, node.layout.x, node.layout.y,
					                     node.layout.width, node.layout.height, xs, ys);
					return;
				}
				transformRectCorners(node, false, node.layout.x, node.layout.y,
				                     node.layout.width, node.layout.height, e.xs, e.ys);
				e.nodeId = id;
			}
			for (int i = 0; i < 4; i++) { xs[i] = e.xs[i]; ys[i] = e.ys[i]; }
			return;
		}
		transformRectCorners(node, false, node.layout.x, node.layout.y,
		                     node.layout.width, node.layout.height, xs, ys);
	}
};

void appendSideBordersWithAlpha(const Node &node, uint8_t parentAlpha)
{
	const int x = node.layout.x;
	const int y = node.layout.y;
	const int w = node.layout.width;
	const int h = node.layout.height;
	if (w <= 0 || h <= 0 || !hasSideBorder(node.style)) return;

	const bool transformed = ViewGeometry::hasTransformChain(node, false);
	for (int side = 0; side < 4; ++side) {
		const int borderWidth = rstyle(node.style).border_side_width[side];
		if (borderWidth <= 0) continue;
		int sx = x;
		int sy = y;
		int sw = w;
		int sh = h;
		if (side == 0) {
			sh = std::min(borderWidth, h);
		} else if (side == 1) {
			sw = std::min(borderWidth, w);
			sx = x + w - sw;
		} else if (side == 2) {
			sh = std::min(borderWidth, h);
			sy = y + h - sh;
		} else {
			sw = std::min(borderWidth, w);
		}
		if (sw <= 0 || sh <= 0) continue;
		if (transformed) {
			int16_t xs[4], ys[4];
			ViewGeometry::transformRectCorners(node, false, sx, sy, sw, sh, xs, ys);
			int bx0, by0, bx1, by1;
			boundsFromCorners(xs, ys, &bx0, &by0, &bx1, &by1);
			appendFillQuadWithAlpha(xs,
			                        ys,
			                        rstyle(node.style).border_side_color[side],
			                        rstyle(node.style).border_side_alpha[side],
			                        parentAlpha,
			                        bx0,
			                        by0,
			                        bx1 - bx0 + 1,
			                        by1 - by0 + 1,
			                        sx,
			                        sy,
			                        sw,
			                        sh,
			                        true);
		} else {
			appendFillRectWithAlpha(sx,
			                        sy,
			                        sw,
			                        sh,
			                        rstyle(node.style).border_side_color[side],
			                        rstyle(node.style).border_side_alpha[side],
			                        parentAlpha,
			                        sx,
			                        sy,
			                        sw,
			                        sh);
		}
	}
}

void recordLinearGradientBackground(const Node &node, uint8_t parentAlpha)
{
	const int x = node.layout.x;
	const int y = node.layout.y;
	const int w = node.layout.width;
	const int h = node.layout.height;
	if (w <= 0 || h <= 0) return;

	if (!ViewGeometry::hasTransformChain(node, false)) {
		appendLinearGradientRectRaw(node, x, y, w, h);
		return;
	}

	// Transformed (3D/perspective) face: emit ONE per-pixel-shaded gradient quad
	// instead of ~72 solid-color strips. The drawer inverse-maps each screen pixel
	// to face-local space and samples the gradient directly — exact (no banding),
	// alpha-correct, and 1 command vs 72 (the strips were ~575 quads/frame on the
	// 6-face spinning cube, dominating both record and replay).
	int16_t xs[4], ys[4], fx[4], fy[4];
	ViewGeometry::transformRectCorners(node, false, x, y, w, h, xs, ys, fx, fy);
	int bx0, by0, bx1, by1;
	boundsFromCorners(xs, ys, &bx0, &by0, &bx1, &by1);
	DisplayCommand *cmd = DisplayList::instance().append();
	if (!cmd) return;
	cmd->type = DisplayCommandType::FillTransformedLinearGradient;
	cmd->bx = bx0;
	cmd->by = by0;
	cmd->bw = bx1 - bx0 + 1;
	cmd->bh = by1 - by0 + 1;
	auto &g = cmd->transformedGradient;
	g.x0 = xs[0]; g.y0 = ys[0];
	g.x1 = xs[1]; g.y1 = ys[1];
	g.x2 = xs[2]; g.y2 = ys[2];
	g.x3 = xs[3]; g.y3 = ys[3];
	g.fx0 = fx[0]; g.fy0 = fy[0];
	g.fx1 = fx[1]; g.fy1 = fy[1];
	g.fx2 = fx[2]; g.fy2 = fy[2];
	g.fx3 = fx[3]; g.fy3 = fy[3];
	g.lx = static_cast<int16_t>(x);
	g.ly = static_cast<int16_t>(y);
	g.lw = static_cast<int16_t>(w);
	g.lh = static_cast<int16_t>(h);
	g.fromColor = rstyle(node.style).bg_gradient_from_color;
	g.midColor = rstyle(node.style).bg_gradient_mid_color;
	g.toColor = rstyle(node.style).bg_gradient_to_color;
	g.midStop = static_cast<uint16_t>(rstyle(node.style).bg_gradient_mid_stop);
	g.toStop = static_cast<uint16_t>(gradientToStop(node));
	g.angle = static_cast<int16_t>(rstyle(node.style).bg_gradient_angle);
	g.fromAlpha = rstyle(node.style).bg_gradient_from_alpha;
	g.midAlpha = rstyle(node.style).bg_gradient_mid_alpha;
	g.toAlpha = rstyle(node.style).bg_gradient_to_alpha;
	g.hasMid = rstyle(node.style).bg_gradient_has_mid ? 1 : 0;
	g.backfaceHidden = node.style.backface_hidden ? 1 : 0;
	// Transformed rectangular borders ride this command as an edge frame (the
	// span rasterizer paints the first/last edgeWidth px of every span, tracing
	// the quad outline). Emitting projected stroke quads instead would disarm
	// the transform-reproject fast path every frame.
	if (node.style.border_width > 0 && node.style.border_alpha > 0 &&
	    !isFullyRoundedShape(node)) {
		g.edgeColor = node.style.border_color;
		g.edgeAlpha = node.style.border_alpha;
		const int ew = node.style.border_width < 1 ? 1 : node.style.border_width;
		g.edgeWidth = static_cast<uint8_t>(ew > 8 ? 8 : ew);
	} else {
		g.edgeColor = 0;
		g.edgeAlpha = 0;
		g.edgeWidth = 0;
	}
}

void recordRadialGradientBackground(const Node &node)
{
	if (!rstyle(node.style).bg_radial_gradient) return;
	const int x = node.layout.x;
	const int y = node.layout.y;
	const int w = node.layout.width;
	const int h = node.layout.height;
	if (w <= 0 || h <= 0) return;
	if (ViewGeometry::hasTransformChain(node, false)) return;
	appendRadialGradientRectRaw(node, x, y, w, h);
}

void recordOverlayLinearGradientBackground(const Node &node)
{
	if (!rstyle(node.style).bg_overlay_gradient) return;
	const int x = node.layout.x;
	const int y = node.layout.y;
	const int w = node.layout.width;
	const int h = node.layout.height;
	if (w <= 0 || h <= 0) return;
	if (ViewGeometry::hasTransformChain(node, false)) return;
	appendLinearGradientRectRaw(node,
	                            x,
	                            y,
	                            w,
	                            h,
	                            rstyle(node.style).bg_overlay_gradient_from_color,
	                            rstyle(node.style).bg_overlay_gradient_mid_color,
	                            rstyle(node.style).bg_overlay_gradient_to_color,
	                            rstyle(node.style).bg_overlay_gradient_mid_stop,
	                            rstyle(node.style).bg_overlay_gradient_to_stop,
	                            rstyle(node.style).bg_overlay_gradient_angle,
	                            rstyle(node.style).bg_overlay_gradient_from_alpha,
	                            rstyle(node.style).bg_overlay_gradient_mid_alpha,
	                            rstyle(node.style).bg_overlay_gradient_to_alpha,
	                            rstyle(node.style).bg_overlay_gradient_has_mid);
}

void recordTransformedEllipseFill(const Node &node, uint8_t parentAlpha)
{
	const int x = node.layout.x;
	const int y = node.layout.y;
	const int w = node.layout.width;
	const int h = node.layout.height;
	if (w <= 0 || h <= 0) return;

	int bx0, by0, bx1, by1;
	ViewRenderer::transformedBounds(node, false, &bx0, &by0, &bx1, &by1);
	const uint8_t effectiveAlpha = combineAlpha(parentAlpha, node.style.bg_alpha);
	if (effectiveAlpha != parentAlpha) appendAlphaCommand(effectiveAlpha, bx0, by0, bx1 - bx0 + 1, by1 - by0 + 1);

	const double cx = static_cast<double>(x) + static_cast<double>(w) * 0.5;
	const double cy = static_cast<double>(y) + static_cast<double>(h) * 0.5;
	const double rx = static_cast<double>(w) * 0.5;
	const double ry = static_cast<double>(h) * 0.5;
	int steps = h < 28 ? h : 28;
	if (steps < 6) steps = 6;
	for (int i = 0; i < steps; i++) {
		const int sy = y + (h * i) / steps;
		const int ey = y + (h * (i + 1)) / steps;
		const int sh = ey - sy;
		if (sh <= 0) continue;
		const double midY = static_cast<double>(sy) + static_cast<double>(sh) * 0.5;
		double t = ry > 0.0 ? (midY - cy) / ry : 0.0;
		if (t < -1.0) t = -1.0;
		if (t > 1.0) t = 1.0;
		const double half = rx * std::sqrt(std::max(0.0, 1.0 - t * t));
		const int sx = static_cast<int>(std::lround(cx - half));
		const int sw = static_cast<int>(std::lround(half * 2.0));
		if (sw <= 0) continue;
		int16_t xs[4], ys[4];
		ViewGeometry::transformRectCorners(node, false, sx, sy, sw, sh, xs, ys);
		int qx0, qy0, qx1, qy1;
		boundsFromCorners(xs, ys, &qx0, &qy0, &qx1, &qy1);
		appendFillQuadRaw(xs, ys, node.style.bg_color, qx0, qy0, qx1 - qx0 + 1, qy1 - qy0 + 1,
		                  sx, sy, sw, sh, true);
	}

	if (effectiveAlpha != parentAlpha) appendAlphaCommand(parentAlpha, bx0, by0, bx1 - bx0 + 1, by1 - by0 + 1);
}

void recordTransformedRoundedRectFill(const Node &node, uint8_t parentAlpha)
{
	const int x = node.layout.x;
	const int y = node.layout.y;
	const int w = node.layout.width;
	const int h = node.layout.height;
	if (w <= 0 || h <= 0) return;

	int16_t xs[4], ys[4];
	ViewGeometry::transformRectCorners(node, false, x, y, w, h, xs, ys);
	int bx0, by0, bx1, by1;
	boundsFromCorners(xs, ys, &bx0, &by0, &bx1, &by1);
	const uint8_t effectiveAlpha = combineAlpha(parentAlpha, node.style.bg_alpha);
	if (effectiveAlpha != parentAlpha) appendAlphaCommand(effectiveAlpha, bx0, by0, bx1 - bx0 + 1, by1 - by0 + 1);
	int16_t rx8[4]{};
	int16_t ry8[4]{};
	resolvedBorderRadii8(node, rx8, ry8);
	DisplayCommand *cmd = DisplayList::instance().append();
	if (cmd) {
		cmd->type = DisplayCommandType::FillTransformedRoundedRect;
		cmd->bx = bx0;
		cmd->by = by0;
		cmd->bw = bx1 - bx0 + 1;
		cmd->bh = by1 - by0 + 1;
		auto &r = cmd->transformedRoundedRect;
		r.x0 = xs[0]; r.y0 = ys[0];
		r.x1 = xs[1]; r.y1 = ys[1];
		r.x2 = xs[2]; r.y2 = ys[2];
		r.x3 = xs[3]; r.y3 = ys[3];
		r.lx = static_cast<int16_t>(x);
		r.ly = static_cast<int16_t>(y);
		r.lw = static_cast<int16_t>(w);
		r.lh = static_cast<int16_t>(h);
		r.tlRx8 = rx8[0]; r.tlRy8 = ry8[0];
		r.trRx8 = rx8[1]; r.trRy8 = ry8[1];
		r.brRx8 = rx8[2]; r.brRy8 = ry8[2];
		r.blRx8 = rx8[3]; r.blRy8 = ry8[3];
		r.color = node.style.bg_color;
		r.backfaceHidden = node.style.backface_hidden ? 1 : 0;
	}

	if (effectiveAlpha != parentAlpha) appendAlphaCommand(parentAlpha, bx0, by0, bx1 - bx0 + 1, by1 - by0 + 1);
}

void recordTransformedEllipseStroke(const Node &node, uint8_t parentAlpha)
{
	const int x = node.layout.x;
	const int y = node.layout.y;
	const int w = node.layout.width;
	const int h = node.layout.height;
	if (w <= 0 || h <= 0 || node.style.border_width <= 0) return;

	int bx0, by0, bx1, by1;
	ViewRenderer::transformedBounds(node, false, &bx0, &by0, &bx1, &by1);
	const uint8_t effectiveAlpha = combineAlpha(parentAlpha, node.style.border_alpha);
	if (effectiveAlpha != parentAlpha) appendAlphaCommand(effectiveAlpha, bx0, by0, bx1 - bx0 + 1, by1 - by0 + 1);

	const float cx = static_cast<float>(x) + static_cast<float>(w) * 0.5f;
	const float cy = static_cast<float>(y) + static_cast<float>(h) * 0.5f;
	const float stroke = static_cast<float>(node.style.border_width < 1 ? 1 : node.style.border_width);
	const float screenStroke = std::max(1.6f, stroke * 1.2f);
	const float rx = std::max(0.0f, static_cast<float>(w) * 0.5f - stroke * 0.5f);
	const float ry = std::max(0.0f, static_cast<float>(h) * 0.5f - stroke * 0.5f);
	// Segment count adaptive to ring size (~1 segment per 4px of diameter), clamped.
	// Was a flat 256; each segment is a transformed stroke band (record + replay),
	// so this is kept as low as stays visually smooth for the cube's thin rings.
	int segments = (w > h ? w : h) / 4;
	if (segments < 14) segments = 14;
	if (segments > 40) segments = 40;
	const float angStep = (2.0f * kPiF) / static_cast<float>(segments);
	const ViewGeometry::ChainProjector projector = ViewGeometry::prepareProjector(node);
	int16_t prevX = 0;
	int16_t prevY = 0;
	float prevLocalX = 0.0f;
	float prevLocalY = 0.0f;
	for (int i = 0; i <= segments; i++) {
		const float a = static_cast<float>(i) * angStep;
		const float localX = cx + rx * std::cos(a);
		const float localY = cy + ry * std::sin(a);
		int16_t px, py;
		ViewGeometry::projectPoint(projector, localX, localY, 0.0f, &px, &py);
		if (i > 0)
			appendStrokeSegmentBandRaw(prevX, prevY, px, py, screenStroke, node.style.border_color,
			                           prevLocalX, prevLocalY, localX, localY, true);
		prevX = px;
		prevY = py;
		prevLocalX = localX;
		prevLocalY = localY;
	}

	if (effectiveAlpha != parentAlpha) appendAlphaCommand(parentAlpha, bx0, by0, bx1 - bx0 + 1, by1 - by0 + 1);
}

void appendBackgroundGridRectRaw(const Node &node, int x, int y, int w, int h)
{
	if (w <= 0 || h <= 0) return;
	if (ViewGeometry::hasTransformChain(node, false)) {
		int16_t xs[4], ys[4];
		ViewGeometry::transformRectCorners(node, false, x, y, w, h, xs, ys);
		// Corners are TL,TR,BR,BL. Near the horizon (or a far perspective edge) a grid
		// strip projects to under 1px and its two long edges round to the same row/col —
		// a degenerate, zero-area quad the rasterizer can't fill, so the line vanishes.
		// Round the thin axis up to a minimum of 1px so every line still draws.
		if (ys[2] - ys[1] < 1) ys[2] = ys[1] + 1; // right edge height (BR below TR)
		if (ys[3] - ys[0] < 1) ys[3] = ys[0] + 1; // left edge height (BL below TL)
		if (xs[1] - xs[0] < 1) xs[1] = xs[0] + 1; // top edge width (TR right of TL)
		if (xs[2] - xs[3] < 1) xs[2] = xs[3] + 1; // bottom edge width (BR right of BL)
		int bx0, by0, bx1, by1;
		boundsFromCorners(xs, ys, &bx0, &by0, &bx1, &by1);
		appendFillQuadRaw(xs, ys, rstyle(node.style).bg_grid_color, bx0, by0, bx1 - bx0 + 1, by1 - by0 + 1,
		                  x, y, w, h, 3);
	} else {
		if (!hasAnyRadius(node)) {
			appendFillRectRaw(x, y, w, h, rstyle(node.style).bg_grid_color, x, y, w, h);
			return;
		}
		int runX0 = 0;
		int runX1 = -1;
		int runY = 0;
		int runH = 0;
		auto flushRun = [&]() {
			if (runH > 0 && runX0 <= runX1)
				appendFillRectRaw(runX0, runY, runX1 - runX0 + 1, runH, rstyle(node.style).bg_grid_color, runX0, runY, runX1 - runX0 + 1, runH);
			runH = 0;
		};
		for (int py = y; py < y + h; ++py) {
			int rowX0 = x;
			int rowX1 = x + w - 1;
			roundedNodeRowSpan(node, py, &rowX0, &rowX1);
			if (rowX0 > rowX1) {
				flushRun();
				continue;
			}
			if (runH > 0 && rowX0 == runX0 && rowX1 == runX1) {
				++runH;
				continue;
			}
			flushRun();
			runX0 = rowX0;
			runX1 = rowX1;
			runY = py;
			runH = 1;
		}
		flushRun();
	}
}

void recordBackgroundGrid(const Node &node, uint8_t parentAlpha)
{
	if (rstyle(node.style).bg_grid_axes == 0) return;
	const int x0 = node.layout.x;
	const int y0 = node.layout.y;
	const int x1 = node.layout.x + node.layout.width;
	const int y1 = node.layout.y + node.layout.height;
	if (x1 <= x0 || y1 <= y0) return;

	int bx0 = x0, by0 = y0, bx1 = x1 - 1, by1 = y1 - 1;
	if (ViewGeometry::hasTransformChain(node, false))
		ViewRenderer::transformedBounds(node, false, &bx0, &by0, &bx1, &by1);
	const uint8_t effectiveAlpha = combineAlpha(parentAlpha, rstyle(node.style).bg_grid_alpha);
	if (effectiveAlpha != parentAlpha) appendAlphaCommand(effectiveAlpha, bx0, by0, bx1 - bx0 + 1, by1 - by0 + 1);

	if ((rstyle(node.style).bg_grid_axes & 1) != 0 && rstyle(node.style).bg_grid_line_x > 0) {
		const int step = rstyle(node.style).bg_grid_step_x > 0 ? rstyle(node.style).bg_grid_step_x : rstyle(node.style).bg_grid_line_x;
		if (step > 0) {
			for (int x = x0; x < x1; x += step) {
				const int w = std::min<int>(rstyle(node.style).bg_grid_line_x, x1 - x);
				appendBackgroundGridRectRaw(node, x, y0, w, y1 - y0);
			}
		}
	}
	if ((rstyle(node.style).bg_grid_axes & 2) != 0 && rstyle(node.style).bg_grid_line_y > 0) {
		const int step = rstyle(node.style).bg_grid_step_y > 0 ? rstyle(node.style).bg_grid_step_y : rstyle(node.style).bg_grid_line_y;
		if (step > 0) {
			for (int y = y0; y < y1; y += step) {
				const int h = std::min<int>(rstyle(node.style).bg_grid_line_y, y1 - y);
				appendBackgroundGridRectRaw(node, x0, y, x1 - x0, h);
			}
		}
	}

	if (effectiveAlpha != parentAlpha) appendAlphaCommand(parentAlpha, bx0, by0, bx1 - bx0 + 1, by1 - by0 + 1);
}

uint8_t insetShadowAlphaAt(int depth, int solidDepth, int blurRadius, uint8_t baseAlpha)
{
	if (baseAlpha == 0) return 0;
	if (depth < solidDepth) return baseAlpha;
	if (blurRadius <= 0) return 0;
	const int blurDepth = depth - solidDepth;
	if (blurDepth >= blurRadius) return 0;
	double t = 1.0 - (static_cast<double>(blurDepth) + 0.5) / static_cast<double>(blurRadius);
	if (t < 0.0) t = 0.0;
	if (t > 1.0) t = 1.0;
	const int alpha = static_cast<int>(static_cast<double>(baseAlpha) * t * t + 0.5);
	return static_cast<uint8_t>(alpha < 0 ? 0 : alpha > 255 ? 255 : alpha);
}

void appendInsetShadowBand(const Node &node, uint8_t parentAlpha, int x, int y, int w, int h, uint8_t alpha)
{
	if (alpha == 0 || w <= 0 || h <= 0) return;
	if (ViewGeometry::hasTransformChain(node, false)) {
		int16_t xs[4], ys[4];
		ViewGeometry::transformRectCorners(node, false, x, y, w, h, xs, ys);
		int bx0, by0, bx1, by1;
		boundsFromCorners(xs, ys, &bx0, &by0, &bx1, &by1);
		appendFillQuadWithAlpha(xs,
		                        ys,
		                        rstyle(node.style).box_shadow_color,
		                        alpha,
		                        parentAlpha,
		                        bx0,
		                        by0,
		                        bx1 - bx0 + 1,
		                        by1 - by0 + 1,
		                        x,
		                        y,
		                        w,
		                        h,
		                        true);
		return;
	}
	if (!hasAnyRadius(node)) {
		appendFillRectWithAlpha(x, y, w, h, rstyle(node.style).box_shadow_color, alpha, parentAlpha, x, y, w, h);
		return;
	}
	int runX0 = 0;
	int runX1 = -1;
	int runY = 0;
	int runH = 0;
	auto flushRun = [&]() {
		if (runH > 0 && runX0 <= runX1)
			appendFillRectWithAlpha(runX0,
			                        runY,
			                        runX1 - runX0 + 1,
			                        runH,
			                        rstyle(node.style).box_shadow_color,
			                        alpha,
			                        parentAlpha,
			                        runX0,
			                        runY,
			                        runX1 - runX0 + 1,
			                        runH);
		runH = 0;
	};
	for (int py = y; py < y + h; ++py) {
		int rowX0 = x;
		int rowX1 = x + w - 1;
		roundedNodeRowSpan(node, py, &rowX0, &rowX1);
		if (rowX0 > rowX1) {
			flushRun();
			continue;
		}
		if (runH > 0 && rowX0 == runX0 && rowX1 == runX1) {
			++runH;
			continue;
		}
		flushRun();
		runX0 = rowX0;
		runX1 = rowX1;
		runY = py;
		runH = 1;
	}
	flushRun();
}

void recordInsetBoxShadow(const Node &node, uint8_t parentAlpha)
{
	if (!rstyle(node.style).box_shadow_inset || rstyle(node.style).box_shadow_alpha == 0) return;
	const int x = node.layout.x;
	const int y = node.layout.y;
	const int w = node.layout.width;
	const int h = node.layout.height;
	if (w <= 0 || h <= 0) return;

	const int blur = std::max<int>(0, rstyle(node.style).box_shadow_blur_radius);
	const int spread = std::max<int>(0, rstyle(node.style).box_shadow_spread);
	const int ox = rstyle(node.style).box_shadow_offset_x;
	const int oy = rstyle(node.style).box_shadow_offset_y;
	const uint8_t baseAlpha = rstyle(node.style).box_shadow_alpha;

	const int leftSolid = spread + std::max(0, ox);
	const int rightSolid = spread + std::max(0, -ox);
	const int topSolid = spread + std::max(0, oy);
	const int bottomSolid = spread + std::max(0, -oy);
	const int leftExtent = std::min(w / 2, leftSolid + blur);
	const int rightExtent = std::min(w / 2, rightSolid + blur);
	const int topExtent = std::min(h / 2, topSolid + blur);
	const int bottomExtent = std::min(h / 2, bottomSolid + blur);

	for (int d = 0; d < topExtent; ++d)
		appendInsetShadowBand(node, parentAlpha, x, y + d, w, 1, insetShadowAlphaAt(d, topSolid, blur, baseAlpha));
	for (int d = 0; d < bottomExtent; ++d)
		appendInsetShadowBand(node, parentAlpha, x, y + h - d - 1, w, 1, insetShadowAlphaAt(d, bottomSolid, blur, baseAlpha));
	for (int d = 0; d < leftExtent; ++d)
		appendInsetShadowBand(node, parentAlpha, x + d, y, 1, h, insetShadowAlphaAt(d, leftSolid, blur, baseAlpha));
	for (int d = 0; d < rightExtent; ++d)
		appendInsetShadowBand(node, parentAlpha, x + w - d - 1, y, 1, h, insetShadowAlphaAt(d, rightSolid, blur, baseAlpha));
}

void GEA_VIEW_HOT_SRAM_SECTION("view_renderer_transformed_bounds") ViewRenderer::transformedBounds(const Node &node, bool use_prev, int *x0, int *y0, int *x1, int *y1)
{
	const Node *n = &node;
	int x = use_prev ? n->layout.previous_x : n->layout.x;
	int y = use_prev ? n->layout.previous_y : n->layout.y;
	int w = use_prev ? n->layout.previous_width : n->layout.width;
	int h = use_prev ? n->layout.previous_height : n->layout.height;

	if (w <= 0 || h <= 0) {
		*x0 = x; *y0 = y; *x1 = x - 1; *y1 = y - 1;
		return;
	}

	// Hot fast path: a node with no RareStyle entry (rare_style < 0) has no local
	// transform/perspective/filter, so in a transform-free tree its current bounds are
	// exactly the layout box. In a transformed tree we must still walk ancestors:
	// otherwise plain children (for example a face label <span>) ignore their
	// transformed parent plane and paint as axis-aligned overlay text.
	if (!use_prev && n->style.rare_style < 0 && !ViewGeometry::anyTransformPresent()) {
		*x0 = x;
		*y0 = y;
		*x1 = x + w - 1;
		*y1 = y + h - 1;
		return;
	}

	int rotate = use_prev ? n->render.previous_transform_rotate : rstyle(n->style).transform_rotate;

	auto expandForBlur = [&]() {
		const int radius = use_prev ? n->render.previous_filter_blur_radius : rstyle(n->style).filter_blur_radius;
		if (radius <= 0) return;
		const int extentX = std::max(1, radius) * 5;
		const int extentY = std::max(1, radius) * 5;
		*x0 -= extentX;
		*y0 -= extentY;
		*x1 += extentX;
		*y1 += extentY;
	};

	if ((rotate % 3600) == 0 && !ViewGeometry::hasTransformChain(node, use_prev)) {
		*x0 = x;
		*y0 = y;
		*x1 = x + w - 1;
		*y1 = y + h - 1;
		expandForBlur();
		return;
	}

	int16_t xs[4], ys[4];
	ViewGeometry::transformCorners(*n, use_prev, xs, ys);
	*x0 = *x1 = xs[0];
	*y0 = *y1 = ys[0];
	for (int i = 1; i < 4; i++) {
		if (xs[i] < *x0) *x0 = xs[i];
		if (xs[i] > *x1) *x1 = xs[i];
		if (ys[i] < *y0) *y0 = ys[i];
		if (ys[i] > *y1) *y1 = ys[i];
	}
	expandForBlur();
}

void GEA_VIEW_HOT_SRAM_SECTION("view_renderer_transformed_corners") ViewRenderer::transformedCorners(const Node &node, bool usePrevious, int16_t *xs, int16_t *ys)
{
	ViewGeometry::transformCorners(node, usePrevious, xs, ys);
}

void GEA_VIEW_HOT_SRAM_SECTION("view_renderer_transformed_point") ViewRenderer::transformedPoint(const Node &node, bool usePrevious, float x, float y, float z, int16_t *outX, int16_t *outY)
{
	ViewGeometry::transformPoint(node, usePrevious, x, y, z, outX, outY);
}

void GEA_VIEW_HOT_SRAM_SECTION("view_renderer_transformed_rect_corners") ViewRenderer::transformedRectCorners(const Node &node, bool usePrevious, int x, int y, int w, int h, int16_t *xs, int16_t *ys,
                                          int16_t *xs8, int16_t *ys8)
{
	ViewGeometry::transformRectCorners(node, usePrevious, x, y, w, h, xs, ys, xs8, ys8);
}

int GEA_VIEW_HOT_SRAM_SECTION("view_renderer_transformed_depth") ViewRenderer::transformedDepth(const Node &node, bool usePrevious)
{
	return ViewGeometry::averageDepth(node, usePrevious);
}

bool GEA_VIEW_HOT_SRAM_SECTION("view_renderer_any_transform_active") ViewRenderer::anyTransformActive()
{
	return ViewGeometry::anyTransformPresent();
}

bool ViewRenderer::recordClipBegin(const Node &node)
{
	const Node *n = &node;
	if (ViewGeometry::hasTransformChain(*n, false)) return 0;
	if (!isViewLikeNodeType(n->type) || n->style.overflow == 0) return 0;
	if (n->first_child < 0) return 0;

	DisplayCommand *cmd = DisplayList::instance().append();
	if (cmd) {
		cmd->type = DisplayCommandType::PushClip;
		cmd->bx = n->layout.x;
		cmd->by = n->layout.y;
		cmd->bw = n->layout.width;
		cmd->bh = n->layout.height;
		cmd->clip.x = n->layout.x;
		cmd->clip.y = n->layout.y;
		cmd->clip.w = n->layout.width;
		cmd->clip.h = n->layout.height;
		cmd->clip.nodeId = static_cast<int16_t>(&node - Tree::instance().nodes());
	}
	return 1;
}

int ViewRenderer::scrollMaxX(const Node &node)
{
	if (node.type == NodeType::VirtualList) return 0;
	int max_x = node.layout.scroll_content_width - node.layout.width;
	return max_x > 0 ? max_x : 0;
}

int ViewRenderer::scrollMaxY(const Node &node)
{
	if (node.type == NodeType::VirtualList) {
		const int id = static_cast<int>(&node - Tree::instance().nodes());
		return VirtualListRenderer::scrollMaxY(id);
	}
	int max_y = node.layout.scroll_content_height - node.layout.height;
	return max_y > 0 ? max_y : 0;
}

void ViewRenderer::recordClipEnd(const Node &node)
{
	const Node *n = &node;
	DisplayCommand *cmd = DisplayList::instance().append();
	if (!cmd) return;
	cmd->type = DisplayCommandType::PopClip;
	cmd->bx = n->layout.x;
	cmd->by = n->layout.y;
	cmd->bw = n->layout.width;
	cmd->bh = n->layout.height;
	cmd->clip.nodeId = static_cast<int16_t>(&node - Tree::instance().nodes());
}

void GEA_VIEW_HOT_SRAM_SECTION("view_renderer_record_box") ViewRenderer::recordBox(const Node &node, uint8_t parentAlpha)
{
	const Node *n = &node;
	int x = n->layout.x;
	int y = n->layout.y;
	int w = n->layout.width;
	int h = n->layout.height;
	if (w <= 0 || h <= 0) return;

	if (n->style.has_bg) {
		if (n->style.bg_fill == 1) {
			recordLinearGradientBackground(*n, parentAlpha);
			recordRadialGradientBackground(*n);
			recordOverlayLinearGradientBackground(*n);
			recordBackgroundGrid(*n, parentAlpha);
		} else if (ViewGeometry::hasTransformChain(*n, false)) {
			int16_t xs[4], ys[4];
			int bx0, by0, bx1, by1;
			ViewRenderer::transformedBounds(*n, false, &bx0, &by0, &bx1, &by1);
			if (hasAnyRadius(*n)) {
				recordTransformedRoundedRectFill(*n, parentAlpha);
			} else {
				ViewGeometry::transformCorners(*n, false, xs, ys);
				appendFillQuadWithAlpha(xs, ys, n->style.bg_color, n->style.bg_alpha, parentAlpha,
				                        bx0, by0, bx1 - bx0 + 1, by1 - by0 + 1,
				                        x, y, w, h, true);
			}
			recordBackgroundGrid(*n, parentAlpha);
		} else {
			if (hasAnyRadius(*n)) {
				if (hasAnyPercentRadius(*n) && !appendResolvedCircularRoundedRectWithAlpha(*n, parentAlpha))
					recordTransformedRoundedRectFill(*n, parentAlpha);
				else if (!hasAnyPercentRadius(*n))
					appendFillRoundedRectWithAlpha(*n, parentAlpha);
			} else {
				appendFillRectWithAlpha(x, y, w, h, n->style.bg_color, n->style.bg_alpha, parentAlpha, x, y, w, h);
			}
			recordBackgroundGrid(*n, parentAlpha);
		}
	}

	recordInsetBoxShadow(*n, parentAlpha);

	if (n->style.border_width > 0 && !borderIsSameOpaqueSolidBackground(*n, parentAlpha)) {
		if (ViewGeometry::hasTransformChain(*n, false)) {
			if (isFullyRoundedShape(*n)) recordTransformedEllipseStroke(*n, parentAlpha);
		} else {
			appendStrokeWithAlpha(*n, parentAlpha);
		}
	}
	appendSideBordersWithAlpha(*n, parentAlpha);
}

void ViewRenderer::recordScrollbar(const Node &node)
{
	const Node *n = &node;
	if (!isViewLikeNodeType(n->type) || (n->type != NodeType::VirtualList && !scrollsOverflowY(n->style))) return;
	if (n->layout.height <= 0 || n->layout.scroll_content_height <= n->layout.height) return;

	int track_h = n->layout.height - 12;
	if (track_h < 24) return;

	int thumb_h = (n->layout.height * track_h) / n->layout.scroll_content_height;
	if (thumb_h < 24) thumb_h = 24;
	if (thumb_h > track_h) thumb_h = track_h;

	int max_scroll = n->layout.scroll_content_height - n->layout.height;
	int thumb_y = n->layout.y + 6;
	if (max_scroll > 0)
		thumb_y += (n->layout.scroll_y * (track_h - thumb_h)) / max_scroll;

	DisplayCommand *cmd = DisplayList::instance().append();
	if (!cmd) return;
	cmd->type = DisplayCommandType::FillRoundedRect;
	cmd->bx = n->layout.x + n->layout.width - 7;
	cmd->by = thumb_y;
	cmd->bw = 3;
	cmd->bh = thumb_h;
	cmd->fillRoundedRect.x = cmd->bx;
	cmd->fillRoundedRect.y = cmd->by;
	cmd->fillRoundedRect.w = cmd->bw;
	cmd->fillRoundedRect.h = cmd->bh;
	cmd->fillRoundedRect.tl = 2;
	cmd->fillRoundedRect.tr = 2;
	cmd->fillRoundedRect.br = 2;
	cmd->fillRoundedRect.bl = 2;
	// Command colours are panel-order natives, so this has to go through the one
	// authoring conversion (nativeFromRrggbbaa): it applies the panel byte-swap a
	// bare fromRgb565 would leave off on 16-bit panels AND narrows to the board's
	// native_t on the grayscale panels, where native_t is a byte and the RGB565
	// constant would not fit. 0x8C8E94 is the exact RGB888 expansion of the RGB565
	// 0x8C72 this used to spell, so 16-bit panels emit the identical pixel.
	cmd->fillRoundedRect.color = gea::framework::graphics::pixel::nativeFromRrggbbaa(0x8C8E94FFu);
}

}  // namespace gea::embedded::ui

// Canvas colour authoring helpers.
//
// Apps express colours in the natural 0xRRGGBBAA order (red in the high byte) —
// the same order you'd write a CSS hex (#RRGGBB) plus an alpha byte. `rgb()` and
// `rgba()` just pack the channels into that 32-bit number; the actual conversion
// to this board's native pixel (RGB565 on esp32 — panel-endian-aware — or
// RGBA8888 on iOS) happens in geatsc's canvas-colour lowering
// (pixel::nativeFromRrggbbaa). Literal calls fold to a constexpr at build time, so
// there is no runtime cost; dynamic channels (e.g. a per-face shaded colour) run
// the same conversion once at the call site. No per-app pixel packing.
//
// `>>> 0` keeps the result an unsigned 32-bit value — the alpha byte sets bit 31,
// which a bitwise OR would otherwise make negative.

export function rgb(r: number, g: number, b: number): number {
  return (((r & 0xff) << 24) | ((g & 0xff) << 16) | ((b & 0xff) << 8) | 0xff) >>> 0
}

export function rgba(r: number, g: number, b: number, a: number): number {
  return (((r & 0xff) << 24) | ((g & 0xff) << 16) | ((b & 0xff) << 8) | (a & 0xff)) >>> 0
}

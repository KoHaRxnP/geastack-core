export interface CanvasColorFoldOptions {
  pixelPanelEndian?: boolean
}

interface CanvasCallRule {
  name: string
  replacement: string
  colorArg: number
}

const canvasCallRules: CanvasCallRule[] = [
  { name: 'gea_ir::canvasSetFillStyle', replacement: 'gea_ir::canvasSetFillStyleRgb565', colorArg: 1 },
  { name: 'gea_ir::canvasFillCircle', replacement: 'gea_ir::canvasFillCircleRgb565', colorArg: 4 },
  { name: 'gea_ir::canvasFillCircleRgb565', replacement: 'gea_ir::canvasFillCircleRgb565', colorArg: 4 },
  { name: 'gea_ir::canvasFillTriangleRgb565', replacement: 'gea_ir::canvasFillTriangleRgb565', colorArg: 7 },
  { name: 'gea_ir::canvasFillCirclesRgb565Uniform', replacement: 'gea_ir::canvasFillCirclesRgb565Uniform', colorArg: 4 },
].sort((a, b) => b.name.length - a.name.length)

export function foldCanvasColorLiteralsInCpp(source: string, options: CanvasColorFoldOptions = {}): string {
  let out = ''
  let index = 0
  while (index < source.length) {
    const rule = canvasCallRules.find((candidate) => source.startsWith(candidate.name, index) && source[index + candidate.name.length] === '(')
    if (!rule) {
      out += source[index]
      index += 1
      continue
    }

    const rewritten = rewriteCanvasCall(source, index, rule, options)
    if (!rewritten) {
      out += source[index]
      index += 1
      continue
    }
    out += rewritten.text
    index = rewritten.end
  }
  return out
}

function rewriteCanvasCall(
  source: string,
  start: number,
  rule: CanvasCallRule,
  options: CanvasColorFoldOptions,
): { text: string; end: number } | null {
  const open = start + rule.name.length
  const close = findMatching(source, open, '(', ')')
  if (close < 0) return null

  const args = splitTopLevelArgs(source.slice(open + 1, close))
  const colorExpression = args[rule.colorArg]
  if (colorExpression === undefined) return null

  const packed = parseKnownColorExpression(colorExpression, options)
  if (packed === null) return null

  args[rule.colorArg] = formatPackedColor(packed)
  return {
    text: `${rule.replacement}(${args.join(', ')})`,
    end: close + 1,
  }
}

function parseKnownColorExpression(expression: string, options: CanvasColorFoldOptions): number | null {
  void options
  const trimmed = expression.trim()
  const css = parseCppStringLiteral(trimmed)
  if (css !== null) return parseCssColor(css, options)

  const rgbArgs = parseRgbCallArgs(trimmed)
  if (rgbArgs) {
    const channels = rgbArgs.map(parseStaticNumber)
    if (channels.every((value): value is number => value !== null)) {
      return packRgba8888(channels[0], channels[1], channels[2], 255)
    }
  }

  return null
}

function parseCppStringLiteral(expression: string): string | null {
  const direct = expression.match(/^("(?:\\.|[^"\\])*")$/)
  if (direct) return decodeCppStringLiteral(direct[1])

  const constructed = expression.match(/^std::string\(\s*("(?:\\.|[^"\\])*")\s*(?:,\s*\d+\s*)?\)$/)
  if (constructed) return decodeCppStringLiteral(constructed[1])

  return null
}

function decodeCppStringLiteral(literal: string): string | null {
  try {
    return JSON.parse(literal) as string
  } catch {
    return null
  }
}

function parseRgbCallArgs(expression: string): string[] | null {
  const call = expression.match(/^(?:__gea_global_)?(?:color|rgb)\s*(?:\(\))?\s*\(([\s\S]*)\)$/)
  if (!call) return null
  const args = splitTopLevelArgs(call[1])
  return args.length === 3 ? args : null
}

function parseStaticNumber(expression: string): number | null {
  const trimmed = expression.trim()
  const literal = trimmed.match(/^-?\d+(?:\.\d+)?$/)
  if (literal) return Number(literal[0])

  const cast = trimmed.match(/^static_cast<[^>]+>\(\s*(-?\d+(?:\.\d+)?)\s*\)$/)
  if (cast) return Number(cast[1])

  return null
}

function parseCssColor(value: string, options: CanvasColorFoldOptions): number | null {
  void options
  const text = value.trim()
  // rgb()/rgba() — alpha is a 0..1 float in CSS.
  const rgb = text.match(/^rgba?\(\s*(-?\d+(?:\.\d+)?)\s*,\s*(-?\d+(?:\.\d+)?)\s*,\s*(-?\d+(?:\.\d+)?)\s*(?:,\s*(-?\d+(?:\.\d+)?)\s*)?\)$/i)
  if (rgb) {
    const a = rgb[4] === undefined ? 255 : clampByte(Number(rgb[4]) * 255)
    return packRgba8888(Number(rgb[1]), Number(rgb[2]), Number(rgb[3]), a)
  }

  // #rrggbbaa (full RGBA8888 authoring)
  const hex8 = text.match(/^#([0-9a-f]{8})$/i)
  if (hex8) {
    const raw = Number.parseInt(hex8[1], 16)
    return packRgba8888((raw >>> 24) & 0xff, (raw >> 16) & 0xff, (raw >> 8) & 0xff, raw & 0xff)
  }

  const longHex = text.match(/^#([0-9a-f]{6})$/i)
  if (longHex) {
    const raw = Number.parseInt(longHex[1], 16)
    return packRgba8888((raw >> 16) & 0xff, (raw >> 8) & 0xff, raw & 0xff, 255)
  }

  // #rgba (short, with alpha nibble)
  const shortHexA = text.match(/^#([0-9a-f])([0-9a-f])([0-9a-f])([0-9a-f])$/i)
  if (shortHexA) {
    const r = Number.parseInt(shortHexA[1], 16)
    const g = Number.parseInt(shortHexA[2], 16)
    const b = Number.parseInt(shortHexA[3], 16)
    const a = Number.parseInt(shortHexA[4], 16)
    return packRgba8888((r << 4) | r, (g << 4) | g, (b << 4) | b, (a << 4) | a)
  }

  const shortHex = text.match(/^#([0-9a-f])([0-9a-f])([0-9a-f])$/i)
  if (shortHex) {
    const r = Number.parseInt(shortHex[1], 16)
    const g = Number.parseInt(shortHex[2], 16)
    const b = Number.parseInt(shortHex[3], 16)
    return packRgba8888((r << 4) | r, (g << 4) | g, (b << 4) | b, 255)
  }

  return null
}

// RGBA8888 packed as the C++ pixel::packRgba8888 layout: R | G<<8 | B<<16 | A<<24.
function packRgba8888(r: number, g: number, b: number, a: number): number {
  return ((clampByte(r) | (clampByte(g) << 8) | (clampByte(b) << 16) | (clampByte(a) << 24)) >>> 0)
}

function clampByte(value: number): number {
  if (!Number.isFinite(value)) return 0
  const integer = Math.trunc(value)
  if (integer < 0) return 0
  if (integer > 255) return 255
  return integer
}

// Emit the ONE colour conversion as a constexpr call: the per-target C++ build
// folds it to RGB565 (esp32) or RGBA8888 (iOS) at compile time — no runtime cost.
function formatPackedColor(value: number): string {
  return `gea::framework::graphics::pixel::nativeColorFromRgba8888(0x${(value >>> 0).toString(16).padStart(8, '0')}u)`
}

function splitTopLevelArgs(args: string): string[] {
  const result: string[] = []
  let start = 0
  let parens = 0
  let brackets = 0
  let braces = 0
  let quote: string | null = null
  let escaped = false

  for (let index = 0; index < args.length; index++) {
    const char = args[index]
    if (quote) {
      if (escaped) escaped = false
      else if (char === '\\') escaped = true
      else if (char === quote) quote = null
      continue
    }
    if (char === '"' || char === "'") {
      quote = char
      continue
    }
    if (char === '(') parens += 1
    else if (char === ')') parens -= 1
    else if (char === '[') brackets += 1
    else if (char === ']') brackets -= 1
    else if (char === '{') braces += 1
    else if (char === '}') braces -= 1
    else if (char === ',' && parens === 0 && brackets === 0 && braces === 0) {
      result.push(args.slice(start, index).trim())
      start = index + 1
    }
  }

  const tail = args.slice(start).trim()
  if (tail.length > 0 || args.length > 0) result.push(tail)
  return result
}

function findMatching(source: string, open: number, openChar: string, closeChar: string): number {
  let depth = 0
  let quote: string | null = null
  let escaped = false
  for (let index = open; index < source.length; index++) {
    const char = source[index]
    if (quote) {
      if (escaped) escaped = false
      else if (char === '\\') escaped = true
      else if (char === quote) quote = null
      continue
    }
    if (char === '"' || char === "'") {
      quote = char
      continue
    }
    if (char === openChar) depth += 1
    else if (char === closeChar) {
      depth -= 1
      if (depth === 0) return index
    }
  }
  return -1
}

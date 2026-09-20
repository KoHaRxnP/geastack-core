import fs from 'node:fs'
import path from 'node:path'
import type { HostBindingAnalysisPatch } from './types.js'

export function capabilitiesToAnalyzePatch(capabilities: string[]): HostBindingAnalysisPatch {
  const features: string[] = []
  const bindings: string[] = []
  for (const capability of capabilities) {
    if (capability === 'https') features.push(capability)
    else bindings.push(capability)
  }
  return { bindings, features }
}

// The source scan runs at CMake configure time, before any bundle or IR
// exists, so it is the only authority the board build has for what the
// firmware must link. It therefore has to answer the IR's `hostCapabilities`
// questions from text alone: which host bindings are imported, which host
// globals are called, and whether any URL needs TLS. A miss is silent and
// expensive -- an app whose `https://` literal is not seen is built without
// the certificate bundle, and ESP-IDF 6's esp-tls then refuses every
// connection with "No server verification option set".
export function analyzeSourceHostBindings(entry: string): HostBindingAnalysisPatch {
  const bindings = new Set<string>()
  const features = new Set<string>()
  for (const file of discoverSourceFiles(entry)) {
    if (!fs.existsSync(file)) continue
    const text = fs.readFileSync(file, 'utf8')
    addBindingsForGeaEmbeddedImports(text, bindings)
    addBindingsForEmbeddedHostNames(text, bindings)
    addBindingsForHostGlobals(text, bindings)
    addFeaturesForUrlSchemes(text, features)
  }
  return { bindings: [...bindings].sort(), features: [...features].sort() }
}

export function mergeAnalyzePatches(...patches: HostBindingAnalysisPatch[]): HostBindingAnalysisPatch {
  const bindings = new Set<string>()
  const features = new Set<string>()
  for (const patch of patches) {
    for (const binding of patch.bindings ?? []) bindings.add(binding)
    for (const feature of patch.features ?? []) features.add(feature)
  }
  return {
    bindings: [...bindings].sort(),
    features: [...features].sort(),
  }
}

const importBindingNames = new Map<string, string>([
  ['Apps', 'apps'],
  ['BLE', 'ble'],
  ['BLEServer', 'ble'],
  ['WiFi', 'wifi'],
  // The device HTTP server runs on lwip; importing `http` pulls in the same
  // WiFi/network bring-up binding so the TCP/IP stack is initialized.
  ['http', 'wifi'],
  ['Accelerometer', 'imu'],
  ['Audio', 'audio'],
  ['Display', 'display'],
  ['Memory', 'memory'],
  ['Input', 'input'],
  ['audioContext', 'audio'],
  ['loadImage', 'image'],
  ['Camera', 'camera'],
  ['CameraView', 'camera'],
])

const embeddedHostNamePrefixes: Array<[prefix: string, binding: string]> = [
  ['__gea_Accelerometer', 'imu'],
  ['__gea_audioContext', 'audio'],
  ['__gea_Audio', 'audio'],
  ['__gea_Display', 'display'],
  ['__gea_Memory', 'memory'],
  ['__gea_Camera', 'camera'],
]

const namespaceImportBindings = ['apps', 'audio', 'ble', 'camera', 'display', 'image', 'imu', 'memory', 'wifi']
// Property access (`document.` / `document[`), plus VALUE positions: an
// interface cast (`document as unknown as EventSourceLike` — sky-hop-canvas
// bindInput's whole DOM usage) and argument/assignment positions. Without the
// value forms the document runtime include is dropped and every facade
// capability silently returns missing at runtime.
const documentHostRegex = /(?:\bdocument\s*(?:\.|\[)|\bwindow\s*\.\s*document\b|\bdocument\s+as\b|[=(,]\s*document\s*[),;])/

function addBindingsForGeaEmbeddedImports(text: string, bindings: Set<string>): void {
  const importRegex = /\bimport\s+([\s\S]*?)\s+from\s+['"](?:gea-embedded|@geastack\/core|@geajs\/core)['"]/g
  let match: RegExpExecArray | null
  while ((match = importRegex.exec(text)) !== null) {
    const clause = match[1]
    if (/\*\s+as\s+/.test(clause)) {
      for (const binding of namespaceImportBindings) bindings.add(binding)
      continue
    }
    const named = clause.match(/\{([\s\S]*?)\}/)
    if (!named) continue
    for (const specifier of named[1].split(',')) {
      const importedName = specifier.trim().split(/\s+as\s+/)[0]?.trim()
      if (!importedName) continue
      const binding = importBindingNames.get(importedName)
      if (binding) bindings.add(binding)
    }
  }
}

// `fetch(...)` and `new WebSocket(...)` are globals, never imports, so the
// import scan cannot see them; both need the network stack. A member call
// (`this.fetch(`, `store.fetch(`) is an app method, not the host global.
const hostGlobalCalls: Array<[pattern: RegExp, binding: string]> = [
  [/(?<![.\w$])fetch\s*\(/, 'fetch'],
  [/(?<![.\w$])new\s+WebSocket\s*\(/, 'websocket'],
]

// A file that declares its own `fetch` (a class method, or a wrapper such as
// image-demo's runtime.ts) calls that one; its network use, if any, shows up
// through the URL scheme scan instead. A declaration is the only `fetch(`
// whose parameter list is followed by a body.
const fetchDeclarationRegex = /\bfunction\s+fetch\b|\bfetch\s*\([^()]*\)\s*(?::[^{;]*)?\{/

function addBindingsForHostGlobals(text: string, bindings: Set<string>): void {
  const shadowed = fetchDeclarationRegex.test(text)
  for (const [pattern, binding] of hostGlobalCalls) {
    if (binding === 'fetch' && shadowed) continue
    if (pattern.test(text)) bindings.add(binding)
  }
}

// Only a quoted `https://` counts: a bare one is almost always a comment
// citing a source, and linking mbedtls for a comment would cost every app
// that documents itself ~100 KB of flash and the TLS heap at first use.
const httpsLiteralRegex = /['"`]https:\/\//
const wssLiteralRegex = /['"`]wss:\/\//

function addFeaturesForUrlSchemes(text: string, features: Set<string>): void {
  if (httpsLiteralRegex.test(text) || wssLiteralRegex.test(text)) features.add('https')
}

function addBindingsForEmbeddedHostNames(text: string, bindings: Set<string>): void {
  for (const [prefix, binding] of embeddedHostNamePrefixes) {
    if (text.includes(prefix)) bindings.add(binding)
  }
  if (documentHostRegex.test(text)) bindings.add('dom')
}

function discoverSourceFiles(entry: string): string[] {
  const visited = new Set<string>()
  const files: string[] = []

  function visit(file: string): void {
    const resolved = path.resolve(file)
    // An entry that is a directory (a configure run without app metadata)
    // is not a program; reading it would throw EISDIR out of the analyzer.
    if (visited.has(resolved) || !fs.existsSync(resolved) || !fs.statSync(resolved).isFile()) return
    visited.add(resolved)
    const text = fs.readFileSync(resolved, 'utf8')
    for (const specifier of relativeModuleSpecifiers(text)) {
      const dependency = resolveRelativeModule(resolved, specifier)
      if (dependency) visit(dependency)
    }
    files.push(resolved)
  }

  visit(entry)
  return files
}

function relativeModuleSpecifiers(text: string): string[] {
  const specifiers: string[] = []
  const staticImportRegex = /\b(?:import|export)\s+(?:[\s\S]*?\s+from\s+)?['"]([^'"]+)['"]/g
  const dynamicImportRegex = /\bimport\s*\(\s*['"]([^'"]+)['"]\s*\)/g
  collectRelativeSpecifiers(text, staticImportRegex, specifiers)
  collectRelativeSpecifiers(text, dynamicImportRegex, specifiers)
  return specifiers
}

function collectRelativeSpecifiers(text: string, regex: RegExp, specifiers: string[]): void {
  let match: RegExpExecArray | null
  while ((match = regex.exec(text)) !== null) {
    const specifier = match[1]
    if (specifier?.startsWith('.')) specifiers.push(specifier)
  }
}

function resolveRelativeModule(importer: string, specifier: string): string | null {
  const base = path.resolve(path.dirname(importer), specifier)
  for (const candidate of moduleCandidates(base)) {
    if (fs.existsSync(candidate) && fs.statSync(candidate).isFile()) return candidate
  }
  return null
}

function moduleCandidates(base: string): string[] {
  const extensions = ['', '.ts', '.tsx', '.js', '.jsx', '.mjs', '.cjs']
  return [
    ...extensions.map((extension) => `${base}${extension}`),
    ...extensions.filter(Boolean).map((extension) => path.join(base, `index${extension}`)),
  ]
}

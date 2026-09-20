// Minimal .env loader for gea app builds — no `dotenv` dependency.
//
// Reads a `.env` file at the root of an app project and turns every variable it
// declares into Vite `define` entries that inline `import.meta.env.<KEY>` and
// the legacy `process.env.<KEY>` spelling as string literals at build time.
// Both the embedded build (build-gea-vite-geatsc.mjs,
// which compiles the Vite bundle to C++ via geatsc) and the web/simulator build
// use this, so an app can just reference `process.env.MY_VAR` and it "just works"
// on device and in the browser — exactly like Node inlines env vars, but resolved
// at compile time. Only keys actually present in the file are injected; nothing
// from the ambient shell environment leaks into the bundle.

import fs from 'node:fs'
import path from 'node:path'

// Parse an env file into a plain { KEY: value } object. Supports comments (#),
// blank lines, optional `export ` prefixes, and single/double-quoted values.
// Returns {} if the file is absent.
export function readDotEnv(dir, file = '.env') {
  const out = {}
  let text
  try {
    text = fs.readFileSync(path.join(dir, file), 'utf8')
  } catch {
    return out
  }
  for (const rawLine of text.split(/\r?\n/)) {
    const line = rawLine.trim()
    if (!line || line.startsWith('#')) continue
    const eq = line.indexOf('=')
    if (eq === -1) continue
    let key = line.slice(0, eq).trim()
    if (key.startsWith('export ')) key = key.slice('export '.length).trim()
    if (!/^[A-Za-z_][A-Za-z0-9_]*$/.test(key)) continue
    let value = line.slice(eq + 1).trim()
    const quoted = value.length >= 2 && ((value[0] === '"' && value.endsWith('"')) || (value[0] === "'" && value.endsWith("'")))
    if (quoted) value = value.slice(1, -1)
    out[key] = value
  }
  return out
}

// Wrap a raw string value in double quotes and escape the characters that would
// otherwise break (or inject into) a JS string literal. The value gets pasted
// verbatim into source code as the replacement for `process.env.<KEY>`, so it
// must already be a valid quoted literal.
function quoteJsString(value) {
  let out = '"'
  for (const ch of String(value)) {
    const code = ch.charCodeAt(0)
    if (ch === '\\') out += '\\\\'
    else if (ch === '"') out += '\\"'
    else if (ch === '\n') out += '\\n'
    else if (ch === '\r') out += '\\r'
    else if (ch === '\t') out += '\\t'
    else if (code < 0x20) out += `\\u${code.toString(16).padStart(4, '0')}`
    else out += ch
  }
  return out + '"'
}

// Vite `define` map that replaces each `process.env.<KEY>` with the value's
// string literal. Vite `define` substitutes the value verbatim into the code, so
// it must already be a quoted literal.
//
// The set of keys is the union of `.env` and `.env.example`: `.env.example`
// declares the contract (which vars the app reads), so any key it lists is always
// defined — defaulting to "" when `.env` is missing or omits it. That keeps builds
// working with no `.env` present (the app just sees empty strings), while `.env`
// supplies the real values.
export function dotEnvDefines(dir) {
  const values = readDotEnv(dir, '.env')
  const keys = new Set([...Object.keys(values), ...Object.keys(readDotEnv(dir, '.env.example'))])
  const defines = {}
  for (const key of keys) {
    const literal = quoteJsString(values[key] ?? '')
    defines[`import.meta.env.${key}`] = literal
    defines[`process.env.${key}`] = literal
  }
  return defines
}

// Bake `process.env.<KEY>` references into a source string as literals, using the
// same map dotEnvDefines() produces. This runs on the STAGED app sources before
// Vite reads them — the embedded build compiles the module-graph snapshots
// (original + transformed) that geatsc captures, and Vite's `define` only rewrites
// the final bundle, not those snapshots. Substituting at the source keeps all three
// (original, transformed, bundle) consistent.
export function inlineProcessEnv(code, defines) {
  let out = code
  for (const [expr, literal] of Object.entries(defines)) {
    // Match the exact member expression, not a longer identifier
    // (process.env.FOO must not match inside process.env.FOOBAR).
    const escaped = expr.replace(/[.*+?^${}()|[\]\\]/g, '\\$&')
    out = out.replace(new RegExp(`${escaped}(?![\\w$])`, 'g'), literal)
  }
  return out
}

import fs from 'node:fs'
import path from 'node:path'
import type { GeaIrBundleV1, PluginOptionMap } from './types.js'
import { isRecord } from './utils.js'

export interface LoadedIr {
  ir: GeaIrBundleV1 | null
  path: string | null
  error: string | null
}

export function loadIr(options: PluginOptionMap, entry: string): LoadedIr {
  const rawPath = options['gea.ir'] ?? options.ir
  if (!rawPath) return { ir: null, path: null, error: null }
  const resolved = path.resolve(path.dirname(entry), rawPath)
  try {
    const parsed = JSON.parse(fs.readFileSync(resolved, 'utf8')) as unknown
    const validationError = validateIrSkeleton(parsed, resolved)
    if (validationError) return { ir: null, path: resolved, error: validationError }
    return { ir: parsed as GeaIrBundleV1, path: resolved, error: null }
  } catch (error) {
    return {
      ir: null,
      path: resolved,
      error: `failed to read gea IR at ${resolved}: ${error instanceof Error ? error.message : String(error)}`,
    }
  }
}

function validateIrSkeleton(value: unknown, file: string): string | null {
  if (!isRecord(value)) return `invalid gea IR at ${file}: expected object`
  if (value.schema !== 'gea-ir') return `invalid gea IR at ${file}: expected schema "gea-ir"`
  if (value.version !== 1) return `unsupported gea IR version at ${file}: expected version 1`
  if (!Array.isArray(value.modules)) return `invalid gea IR at ${file}: modules must be an array`
  if (!Array.isArray(value.components)) return `invalid gea IR at ${file}: components must be an array`
  if (!Array.isArray(value.stores)) return `invalid gea IR at ${file}: stores must be an array`
  if (!Array.isArray(value.hostCapabilities)) return `invalid gea IR at ${file}: hostCapabilities must be an array`
  return null
}

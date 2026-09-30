import fs from 'node:fs'
import path from 'node:path'

/** Native lengths are raw numbers; only an authored CSS unit may introduce scaling. */
export function geaNativeStylePlugin({ runtimeEntry }) {
  if (!runtimeEntry) throw new Error('Native Gea styles require the typed @geajs/core source entry')
  const sourceRoot = path.dirname(fs.realpathSync(runtimeEntry))
  const serializer = path.join(sourceRoot, 'runtime/style-value.ts')
  return {
    name: 'geastack-native-style-units',
    enforce: 'pre',
    // Keep all compiler-generated runtime imports on the same typed source tree.
    // Aliasing only the root left compiler-runtime on the browser dist path.
    resolveId(id) {
      const name = id === '@geajs/core' ? 'index' : id.startsWith('@geajs/core/') ? id.slice('@geajs/core/'.length) : null
      if (name === null) return null
      return [path.join(sourceRoot, `${name}.ts`), path.join(sourceRoot, name, 'index.ts')].find(entry => fs.existsSync(entry)) || null
    },
    load(id) {
      if (id.split('?')[0] !== serializer) return null
      // This module is the browser serialization boundary, not the JSX IR.
      // Replacing it here preserves numeric values in the native IR and prevents
      // browser px defaults from reaching native runtime-backed components/h().
      return `export function styleProp(key: string): string {
  return key.startsWith('--') ? key : key.replace(/[A-Z]/g, c => '-' + c.toLowerCase())
}
export function styleValue(_property: string, value: unknown): string { return String(value) }
`
    },
  }
}

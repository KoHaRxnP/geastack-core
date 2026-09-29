import type ts from 'typescript'

// These two fields start at 255. Only a complete opaque-colour proof removes
// them; background alpha has a transparent default and remains independently live.
export function colorAlphaObserver(features: Set<string>, resolve?: (node: ts.Expression) => string[] | undefined) {
  type Values = string[] | undefined
  const variables = new Map<string, Values[]>()
  const text: Values[] = [], border: Values[] = []
  const values = (value: string | undefined, expression?: ts.Expression): Values =>
    value !== undefined ? [value] : expression ? resolve?.(expression) : undefined
  const strip = (value: string): string => value.trim().replace(/\s*!important\s*$/i, '').trim()
  const wide = /^(?:initial|inherit|unset|revert|revert-layer)$/i
  const opaqueHex = /^(?:#[\da-f]{3}|#[\da-f]{6}|#[\da-f]{3}f|#[\da-f]{6}ff)$/i
  const opaque = (value: string, allowCurrent: boolean, seen = new Set<string>()): boolean => {
    value = strip(value)
    if (wide.test(value) || opaqueHex.test(value)) return true
    if (/^currentcolor$/i.test(value)) return allowCurrent
    const variable = value.match(/^var\(\s*(--[\w-]+)\s*(?:,\s*([^()]+))?\)$/i)
    if (!variable || seen.has(variable[1])) return false
    seen.add(variable[1])
    const definitions = variables.get(variable[1])
    if (!definitions?.length) return variable[2] !== undefined && opaque(variable[2], allowCurrent, new Set(seen))
    return definitions.every(group => group !== undefined && group.every(item => opaque(item, allowCurrent, new Set(seen)))) &&
      (variable[2] === undefined || opaque(variable[2], allowCurrent, new Set(seen)))
  }
  const all = (groups: Values[], allowCurrent: boolean): boolean => groups.every(group => group !== undefined && group.every(value => opaque(value, allowCurrent)))
  return {
    property(name: string | undefined, value: string | undefined, expression?: ts.Expression): void {
      if (!name) { features.add('css-text-alpha'); features.add('css-border-alpha'); return }
      const candidates = values(value, expression)
      if (name.startsWith('--')) {
        const definitions = variables.get(name) ?? []; definitions.push(candidates); variables.set(name, definitions); return
      }
      name = name.replace(/[A-Z]/g, c => `-${c.toLowerCase()}`).toLowerCase()
      if (name === 'color') text.push(candidates)
      if (/^border(?:-(?:top|right|bottom|left|block|inline)(?:-start|-end)?)?-color$/.test(name)) {
        border.push(candidates?.flatMap(value => /var\(/i.test(value) ? [value] : strip(value).split(/\s+/)))
      }
      if (/^border(?:-(?:top|right|bottom|left|block|inline)(?:-start|-end)?)?$/.test(name)) {
        border.push(candidates?.flatMap(value => {
          value = strip(value)
          if (wide.test(value)) return [value]
          // A fully known simple shorthand can omit its width/style tokens.
          // Functions/variables expanding an entire shorthand remain opaque.
          if (/[()]/.test(value)) return [value]
          const colors = value.split(/\s+/).filter(part => !/^(?:none|hidden|solid|dotted|dashed|double|groove|ridge|inset|outset|thin|medium|thick|[+]?(?:\d+(?:\.\d*)?|\.\d+)(?:px|em|rem|vw|vh|vmin|vmax|%)?)$/i.test(part))
          return colors.length ? colors : ['currentcolor']
        }))
      }
      // Native-style spellings and arbitrary CSS-wide mutation are conservative.
      if (name === 'color-alpha') features.add('css-text-alpha')
      if (name === 'border-alpha' || name === 'all') features.add('css-border-alpha')
      if (name === 'all') features.add('css-text-alpha')
    },
    finish(): void {
      const textOpaque = !features.has('css-text-alpha') && all(text, true)
      if (!textOpaque) features.add('css-text-alpha')
      if (!all(border, textOpaque)) features.add('css-border-alpha')
    },
  }
}

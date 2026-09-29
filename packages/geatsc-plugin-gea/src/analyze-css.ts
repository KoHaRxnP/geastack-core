import type ts from 'typescript'
import { colorAlphaObserver } from './analyze-alpha.js'
import { colorVariableAnalysis, type StyleUsageObserver } from './analyze-renderer.js'

export const cssAnalysisVersion = 'css-analysis-v15'
export const cssFeatures = ['css-pseudo-elements', 'css-text-alpha', 'css-border-alpha', 'css-animations', 'css-transforms', 'css-grid', 'css-floats', 'css-writing-mode', 'css-border-relief', 'css-blink', 'css-order', 'css-percent-radius', 'css-percent-gap', 'css-opacity', 'css-text-decoration', 'css-text-transform', 'css-visibility', 'css-pointer-events', 'css-mask', 'css-image-fit', 'css-filters', 'css-box-shadow', 'css-flex-wrap', 'css-justify-items', 'css-align-content', 'css-align-self', 'css-min-width', 'css-height-expressions', 'css-z-index', 'css-aspect-ratio', 'css-margin-trim', 'css-containment', 'css-justify-self', 'css-flex-line-count', 'css-box-expressions', 'css-axis-gap', 'css-corner-radius', 'css-first-line', 'css-side-borders', 'css-background-layers', 'css-line-height-expressions', 'css-scrolling', 'css-flex-basis-expressions', 'css-custom-property-lengths', 'css-max-height', 'css-flex-basis', 'css-overflow-axes', 'css-position-top', 'css-position-top-percent', 'css-position-right', 'css-position-right-percent', 'css-position-bottom', 'css-position-bottom-percent', 'css-position-left', 'css-position-left-percent'] as const

export function addUnknownCssFeatures(features: Set<string>): void {
  for (const feature of cssFeatures) features.add(feature)
}

// These are build facts, not application options. Unknown values retain the
// entire affected family; unknown property names retain all CSS semantics.
// In particular, a dynamic width does not enable transforms or grid layout.
export function cssUsageObserver(features: Set<string>, resolve?: (node: ts.Expression) => string[] | undefined): StyleUsageObserver & { finish(): void } {
  const colors = colorVariableAnalysis()
  const alpha = colorAlphaObserver(features, resolve)
  const deferredColors: Array<[string, string | undefined]> = []
  return {
    unknown: () => { colors.unknown(); addUnknownCssFeatures(features) },
    finish() {
      alpha.finish()
      for (const [feature, value] of deferredColors) if (!colors.isColor(value)) features.add(feature)
    },
    selector: css => {
      if (/::?(?:before|after)\b/i.test(css)) features.add('css-pseudo-elements')
      if (/::?first-line\b/i.test(css)) features.add('css-first-line')
      if (/@(?:-webkit-)?keyframes\b/i.test(css)) features.add('css-animations')
    },
    property(name, value, expression) {
      alpha.property(name, value, expression)
      colors.property(name, value)
      if (!name) { addUnknownCssFeatures(features); return }
      if (name === 'content') features.add('css-pseudo-elements')
      if (name.startsWith('--')) {
        // Quoted CSS strings remain raw text; the length parser never unquotes
        // them. Unknown values still keep the length cache automatically.
        const quoted = value !== undefined && /^(?:"(?:[^"\\\r\n]|\\[^\r\n])*"|'(?:[^'\\\r\n]|\\[^\r\n])*')(?:\s*!important)?$/i.test(value.trim())
        if (!quoted) deferredColors.push(['css-custom-property-lengths', value])
        return
      }
      const originalValue = value
      name = name.replace(/[A-Z]/g, c => `-${c.toLowerCase()}`).toLowerCase()
      value = value?.trim().toLowerCase()
      // Only a proven literal without percentages or functions can omit these
      // alternate representations. var/calc and dynamic values retain them.
      // Equal literal edges need one stored value. Unknown functions/variables
      // may expand to multiple values, and longhands can mutate just one edge.
      const parts = value?.replace(/\s*!important$/, '').split(/\s+/)
      // Preserve each authored physical edge. Logical insets may map to any
      // edge; functions, percentages and unknown values retain percent storage.
      const positionSides = /^(?:top|right|bottom|left)$/.test(name) ? [name]
        : /^inset(?:-|$)/.test(name) ? ['top', 'right', 'bottom', 'left'] : []
      const fixedPosition = !!parts?.length && parts.length <= 4 && parts.every(part =>
        /^(?:(?:auto|initial|inherit|unset|revert|revert-layer)|[+-]?(?:\d+(?:\.\d*)?|\.\d+)(?:px)?)$/.test(part))
      for (const side of positionSides) {
        features.add(`css-position-${side}`)
        if (!fixedPosition) features.add(`css-position-${side}-percent`)
      }

      const uniformLength = !!parts?.length && parts.length <= 4 &&
        parts.every(part => part === parts[0]) &&
        /^(?:(?:normal|initial|inherit|unset|revert|revert-layer)|[+-]?(?:\d+(?:\.\d*)?|\.\d+)(?:px|em|rem|vw|vh|vmin|vmax|%)?)$/.test(parts[0])
      if (/^(?:grid-)?(?:row|column)-gap$/.test(name) ||
          (/^(?:grid-)?gap$/.test(name) && (!uniformLength || parts!.length > 2))) features.add('css-axis-gap')
      if ((name === 'border-radius' && !uniformLength) ||
          /^border-(?:top-left|top-right|bottom-left|bottom-right|start-start|start-end|end-start|end-end)-radius$/.test(name)) features.add('css-corner-radius')
      const mayUsePercent = value === undefined || /[%()]/.test(value)
      if (/^border(?:-(?:top-left|top-right|bottom-left|bottom-right))?-radius$/.test(name) && mayUsePercent) features.add('css-percent-radius')
      if (/^(?:grid-)?(?:(?:row|column)-)?gap$/.test(name) && mayUsePercent) features.add('css-percent-gap')
      if (/^(?:-webkit-)?(?:animation|transition)(?:-|$)/.test(name)) features.add('css-animations')
      if (name === 'blink-interval') features.add('css-blink')
      if (name === 'order') features.add('css-order')
      if (name === 'opacity') features.add('css-opacity')
      if (name === 'text-decoration' || name === 'text-decoration-line') features.add('css-text-decoration')
      if (name === 'text-transform') features.add('css-text-transform')
      if (name === 'visibility') features.add('css-visibility')
      if (name === 'pointer-events') features.add('css-pointer-events')
      if (name === 'mask' || name === 'mask-image' || name === '-webkit-mask' || name === '-webkit-mask-image') features.add('css-mask')
      if (name === 'object-fit') features.add('css-image-fit')
      if (name === 'filter' || name === '-webkit-filter') features.add('css-filters')
      if (name === 'box-shadow' || name === '-webkit-box-shadow') features.add('css-box-shadow')
      if (name === 'flex-wrap' || name === 'flex-flow') features.add('css-flex-wrap')
      if (name === 'justify-items' || name === 'place-items') features.add('css-justify-items')
      if (name === 'align-content' || name === 'place-content') features.add('css-align-content')
      if (name === 'align-self' || name === 'place-self') features.add('css-align-self')
      if (/^min-(?:width|inline-size|block-size)$/.test(name)) features.add('css-min-width')
      // Only simple literal heights prove that no deferred expression is needed.
      // Intrinsic keywords, functions, unknown values and logical sizes retain it.
      if (name === 'block-size' || name === 'inline-size' ||
          (name === 'height' && (value === undefined || !/^(?:(?:auto|initial|inherit|unset|revert|revert-layer)|[+-]?(?:\d+(?:\.\d*)?|\.\d+)(?:px|%)?)(?:\s*!important)?$/.test(value)))) features.add('css-height-expressions')
      if (/^max-(?:height|block-size|inline-size)$/.test(name)) features.add('css-max-height')
      if (name === 'z-index') features.add('css-z-index')
      if (name === 'aspect-ratio') features.add('css-aspect-ratio')
      if (name === 'margin-trim') features.add('css-margin-trim')
      if (name === 'contain' || name === 'content-visibility') features.add('css-containment')
      if (name === 'justify-self' || name === 'place-self') features.add('css-justify-self')
      if (name === 'flex-line-count') features.add('css-flex-line-count')
      // Only fixed pixel/zero edges and CSS-wide defaults can omit deferred
      // lengths. Percentages, relative units, functions and dynamic values keep
      // their storage. Include logical shorthands and longhands conservatively.
      if (/^(?:margin|padding)(?:-|$)/.test(name) && name !== 'margin-trim') {
        const fixed = value?.replace(/\s*!important$/, '').split(/\s+/).every(part =>
          /^(?:(?:auto|initial|inherit|unset|revert|revert-layer)|[+-]?(?:\d+(?:\.\d*)?|\.\d+)(?:px)?)$/.test(part))
        if (!fixed) features.add('css-box-expressions')
      }
      // CSS couples the axes: visible computes to auto next to hidden.
      // Longhands can combine through the cascade, so retain them unless a
      // whole-axis proof is available. A literal shorthand controls both axes.
      if (/^overflow(?:-[xy]|-inline|-block)$/.test(name)) { features.add('css-scrolling'); features.add('css-overflow-axes') }
      if (name === 'overflow') {
        const axes = value?.replace(/\s*!important$/, '').split(/\s+/)
        if (!axes?.length || axes.length > 2 || !axes.every(axis => axis === axes[0]) || !/^(?:visible|hidden|clip|auto|scroll|initial|inherit|unset|revert|revert-layer)$/.test(axes[0])) features.add('css-overflow-axes')
        const known = !!axes?.length && axes.length <= 2 && axes.every(axis => /^(?:visible|hidden|clip|initial|inherit|unset|revert|revert-layer)$/.test(axis))
        if (!known || (axes!.includes('visible') && axes!.includes('hidden'))) features.add('css-scrolling')
      }
      // Local background attachment can observe overflowing content even
      // without a scroll control, so preserve its geometry as well.
      if (/^(?:scroll|overscroll)-/.test(name) || name === 'background-attachment') features.add('css-scrolling')
      if (name === 'background' && !/^(?:none|initial|inherit|unset|revert|revert-layer)(?:\s*!important)?$/.test(value ?? '')) deferredColors.push(['css-scrolling', originalValue])
      const cssWide = value !== undefined && /^(?:initial|inherit|unset|revert|revert-layer)(?:\s*!important)?$/.test(value)
      if (/^border-(?:top|right|bottom|left|block|inline)(?:-|$)/.test(name) && !name.endsWith('-radius')) features.add('css-side-borders')
      if (name === 'border-width' && !cssWide && !(parts?.length && parts.length <= 4 && parts.every(part => part === parts[0]) && /^(?:thin|medium|thick|[+]?(?:\d+(?:\.\d*)?|\.\d+)(?:px|em|rem|vw|vh|vmin|vmax)?)$/.test(parts[0]))) features.add('css-side-borders')
      if (name === 'border-style' && !cssWide && !(parts?.length && parts.length <= 4 && parts.every(part => part === parts[0]) && /^(?:none|hidden|solid|dotted|dashed|double|groove|ridge|inset|outset)$/.test(parts[0]))) features.add('css-side-borders')
      if (name === 'border-color' && !cssWide) deferredColors.push(['css-side-borders', originalValue])
      if (/^background-(?:image|clip|size|position|repeat|attachment|origin)(?:-|$)/.test(name)) features.add('css-background-layers')
      if (name === 'background' && !cssWide && !/^none(?:\s*!important)?$/.test(value ?? '')) deferredColors.push(['css-background-layers', originalValue])
      if ((name === 'line-height' && !cssWide && (value === undefined || !/^(?:normal|[+]?(?:\d+(?:\.\d*)?|\.\d+)(?:px)?)(?:\s*!important)?$/.test(value))) || (name === 'font' && !cssWide)) features.add('css-line-height-expressions')
      // Numeric one/two-factor shorthands and defaults use the implicit auto
      // scalar basis. Any explicit basis or unknown shorthand keeps the field.
      if (name === 'flex-basis' || (name === 'flex' && (value === undefined || !/^(?:(?:none|auto|initial|inherit|unset|revert|revert-layer)|[+]?(?:\d+(?:\.\d*)?|\.\d+)(?:\s+[+]?(?:\d+(?:\.\d*)?|\.\d+))?)(?:\s*!important)?$/.test(value)))) features.add('css-flex-basis')
      // Fixed pixel bases and numeric flex shorthand need no deferred value.
      // Functions, percentages, relative units and dynamic values retain it.
      if (name === 'flex-basis' && (value === undefined || !/^(?:(?:auto|content|initial|inherit|unset|revert|revert-layer)|[+]?(?:\d+(?:\.\d*)?|\.\d+)(?:px)?)(?:\s*!important)?$/.test(value))) features.add('css-flex-basis-expressions')
      if (name === 'flex' && !cssWide && (value === undefined || !/^(?:none|auto|(?:[+]?(?:\d+(?:\.\d*)?|\.\d+)\s+){0,2}(?:[+]?(?:\d+(?:\.\d*)?|\.\d+)(?:px)?|auto|content))(?:\s*!important)?$/.test(value))) features.add('css-flex-basis-expressions')
      if (name === 'all') { addUnknownCssFeatures(features); return }
      if (/^(?:-webkit-)?(?:transform(?:-origin|-style)?|perspective(?:-origin)?|translate|rotate|scale|backface-visibility)$/.test(name)) features.add('css-transforms')
      if (/^grid(?:-|$)/.test(name) || (name === 'display' && (value === undefined || /(?:grid|var\s*\()/i.test(value)))) features.add('css-grid')
      if (name === 'float' || name === 'clear') features.add('css-floats')
      if (name === 'writing-mode' || name === 'direction') features.add('css-writing-mode')
      if (/^border(?:-(?:top|right|bottom|left))?(?:-style)?$/.test(name) &&
          (value === undefined || /(?:inset|outset|groove|ridge|var\s*\()/i.test(value))) features.add('css-border-relief')
    },
  }
}

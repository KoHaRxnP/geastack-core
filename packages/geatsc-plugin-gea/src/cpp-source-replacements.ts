import { sanitizeCppIdentifier } from './utils.js'

export interface RendererReplacement {
  className: string
  rendererName: string
  mountedRendererName?: string
  stateReaderName?: string
  storeExpression?: string
  refFields?: string[]
}

export function replaceGeneratedRenderers(source: string, replacements: RendererReplacement[]): string {
  let next = source
  for (const replacement of replacements) next = replaceGeneratedRenderer(next, replacement)
  return next
}

// The multi-file (module-first) split puts a component class's DECLARATION in
// the module header and its method BODIES out-of-line in the module .cpp
// (`NodeHandle App::<template>(const auto &d) const { … }`). The in-class
// replacement above only ever sees the header, so on that split the boxed
// template body survived untouched next to the native mounted renderer that
// supersedes it — for bubble-grid that meant `__gea_with_boxed_dispatch_arity`
// lambdas, boxed property reads, and a second registration of every touch
// handler through `fn_delegateEvent(..., std::vector<std::vector<gea_cpp_value>>)`.
// Rewrite those out-of-line bodies to the same native mount the header form uses.
export function replaceOutOfLineGeneratedRenderers(source: string, replacements: RendererReplacement[]): string {
  let next = source
  for (const replacement of replacements) next = replaceOutOfLineGeneratedRenderer(next, replacement)
  return next
}

// geatsc encodes a module-scoped member name (`__sym_GEA_CREATE_TEMPLATE` ->
// `gea_m_e_u005f_u005fsym_u005fGEA_u005fCREATE_u005fTEMPLATE`), so match the
// shape rather than either literal spelling.
const OUT_OF_LINE_TEMPLATE_METHOD = '[A-Za-z0-9_]*sym[A-Za-z0-9_]*GEA[A-Za-z0-9_]*(?:CREATE|STATIC)[A-Za-z0-9_]*TEMPLATE'

function replaceOutOfLineGeneratedRenderer(source: string, replacement: RendererReplacement): string {
  if (!replacement.mountedRendererName || !replacement.storeExpression) return source
  const definition = new RegExp(
    `(^|\\n)\\s*([A-Za-z_][\\w:]*(?:\\s*<[^>\\n]*>)?)\\s+${replacement.className}::${OUT_OF_LINE_TEMPLATE_METHOD}\\s*\\([^;{}\\n]*\\)\\s*(?:const\\s*)?\\{`
  )
  const match = definition.exec(source)
  if (!match) return source
  const returnType = match[2]
  const bodyOpen = source.indexOf('{', match.index + match[0].length - 1)
  if (bodyOpen < 0) return source
  const bodyClose = findMatchingBrace(source, bodyOpen)
  if (bodyClose < 0) return source
  const body = rendererBody(replacement, returnType, source)
  return `${source.slice(0, bodyOpen)}${body}${source.slice(bodyClose + 1)}`
}

// A module-first app can contain a compatibility-rendered parent whose child
// already has a native mounted renderer. The parent still constructs the child
// class and calls render()/dispose(), so replacing the child module with an
// empty native stub leaves an incomplete class. Keep the lightweight generated
// class in that boundary module, replace its large static template with the
// native mount, and add the two lifecycle methods the compatibility parent
// expects.
export function bridgeGeneratedComponentRenderers(
  source: string,
  replacements: RendererReplacement[],
  mountedRendererDeclarations: string,
  typedStoreAccessorTypes: ReadonlyMap<string, string> = new Map(),
): string {
  let next = replaceGeneratedRenderers(source, replacements)
  const analysisSource = `${next}\n${mountedRendererDeclarations}`
  for (const replacement of replacements) {
    if (!replacement.mountedRendererName || !replacement.storeExpression) continue
    next = insertComponentBridgeMethods(next, replacement, analysisSource, typedStoreAccessorTypes)
  }
  return next
}

function insertComponentBridgeMethods(
  source: string,
  replacement: RendererReplacement,
  analysisSource: string,
  typedStoreAccessorTypes: ReadonlyMap<string, string>,
): string {
  const classOpen = findClassOpenBrace(source, replacement.className)
  if (classOpen < 0) return source
  const classClose = findMatchingBrace(source, classOpen)
  if (classClose < 0) return source
  const classBody = source.slice(classOpen, classClose)
  if (classBody.includes('__gea_native_mount_disposer')) return source

  const mountCall = `gea_ir::${replacement.mountedRendererName}(${mountStoreArgument(
    replacement,
    analysisSource,
    typedStoreAccessorTypes,
  )}, __gea_native_mount_disposer)`
  const methods = [
    '',
    '  mutable std::shared_ptr<gea_ir::NativeDisposer> __gea_native_mount_disposer;',
    '  mutable gea_cpp_value el = gea_cpp_value::missing();',
    '',
    '  void render(gea_cpp_value parent = gea_cpp_value::missing(), gea_cpp_value _index = gea_cpp_value::missing()) const {',
    '    (void)_index;',
    '    if (__gea_native_mount_disposer) __gea_native_mount_disposer->dispose();',
    '    __gea_native_mount_disposer = std::make_shared<gea_ir::NativeDisposer>();',
    `    auto node = ${mountCall};`,
    '    const int parent_id = gea_ir::nodeIdFromValue(parent);',
    '    if (parent_id >= 0) gea::embedded::ui::NodeHandle(parent_id).appendChild(node);',
    '    el = gea_ir::nodeValue(node);',
    '  }',
    '',
    '  void dispose() {',
    '    if (__gea_native_mount_disposer) __gea_native_mount_disposer->dispose();',
    '    __gea_native_mount_disposer.reset();',
    '    el = gea_cpp_value::missing();',
    '  }',
    '',
  ].join('\n')
  return `${source.slice(0, classClose)}${methods}${source.slice(classClose)}`
}

function replaceGeneratedRenderer(source: string, replacement: RendererReplacement): string {
  const classOpen = findClassOpenBrace(source, replacement.className)
  if (classOpen < 0) return source

  const classClose = findMatchingBrace(source, classOpen)
  if (classClose < 0) return source

  const methodName = '__sym_GEA_STATIC_TEMPLATE'
  const methodNameStart = source.indexOf(methodName, classOpen)
  if (methodNameStart < 0 || methodNameStart > classClose) return source

  const methodOpen = source.indexOf('{', methodNameStart)
  if (methodOpen < 0 || methodOpen > classClose) return source

  const methodClose = findMatchingBrace(source, methodOpen)
  if (methodClose < 0 || methodClose > classClose) return source

  const body = rendererBody(replacement, staticTemplateReturnType(source), source)
  return `${source.slice(0, methodOpen)}${body}${source.slice(methodClose + 1)}`
}

function findClassOpenBrace(source: string, className: string): number {
  const pattern = new RegExp(`\\bclass\\s+${escapeRegExp(className)}\\b`, 'g')
  let match: RegExpExecArray | null
  while ((match = pattern.exec(source)) !== null) {
    const brace = source.indexOf('{', match.index)
    if (brace < 0) return -1
    const semicolon = source.indexOf(';', match.index)
    if (semicolon < 0 || brace < semicolon) return brace
  }
  return -1
}

// Detect the return type geatsc gave `CompiledStaticComponent::__sym_GEA_STATIC_TEMPLATE`.
// When TS `Node` resolves to a concrete type it lowers to `gea::embedded::ui::NodeHandle`
// (the IR `mount_*` renderers already return exactly that), letting templates flow as a
// typed handle with no record bridge. When `Node` is unresolved it falls back to the
// type-erased `gea_cpp_value`. The component overrides must match whichever the base used,
// so derive it from the base class rather than hard-coding either form.
function staticTemplateReturnType(source: string): string {
  const open = findClassOpenBrace(source, 'CompiledStaticComponent')
  if (open < 0) return 'gea_cpp_value'
  const close = findMatchingBrace(source, open)
  if (close < 0) return 'gea_cpp_value'
  const methodStart = source.indexOf('__sym_GEA_STATIC_TEMPLATE', open)
  if (methodStart < 0 || methodStart > close) return 'gea_cpp_value'
  const lineStart = source.lastIndexOf('\n', methodStart) + 1
  const declaration = source.slice(lineStart, methodStart)
  const match = declaration.match(/(?:virtual\s+)?([A-Za-z_][\w:]*(?:\s*<[^>]*>)?)\s*$/)
  return match ? match[1] : 'gea_cpp_value'
}

function returnsNodeHandle(returnType: string): boolean {
  return returnType.includes('NodeHandle')
}

// The mount renderer's typed store-param class, parsed from the mount
// declaration already inserted into the source (the early gea_ir block is in
// by shell-insertion time) — the single source of truth for the param flavor.
// Null = boxed gea_cpp_value param.
function mountStoreParamClass(source: string, mountedRendererName: string): string | null {
  const match = source.match(
    new RegExp(`NodeHandle ${escapeRegExp(mountedRendererName)}\\(const std::shared_ptr<([A-Za-z_][\\w:]*)> &store`),
  )
  return match ? match[1] : null
}

// The return type of the geatsc-core global accessor for `globalName`, read
// from the program's forward declarations or definitions. Some typed store
// globals are first seen as function bodies (`std::shared_ptr<T>& f() { ... }`)
// before this replacement pass inserts its shell declarations.
// 'gea_cpp_value' when absent.
function globalAccessorReturnType(
  source: string,
  globalName: string,
  typedStoreAccessorTypes: ReadonlyMap<string, string> = new Map(),
): string {
  const typedStoreAccessorType = typedStoreAccessorTypes.get(globalName)
  if (typedStoreAccessorType) return typedStoreAccessorType
  const match = source.match(new RegExp(`(?:^|\\n)([^\\n;{}()]+?) &__gea_global_${escapeRegExp(globalName)}\\(\\)\\s*(?:;|\\{)`))
  return match ? match[1].trim() : 'gea_cpp_value'
}

// The argument the shell passes into `mount_<Name>`: the global accessor as
// is when its flavor matches the mount param; boxed through gea_cpp_key when
// a TYPED global cell feeds a boxed mount param (the retained tracked proxy,
// not a snapshot).
function mountStoreArgument(
  replacement: RendererReplacement,
  source: string,
  typedStoreAccessorTypes: ReadonlyMap<string, string> = new Map(),
): string {
  const expression = replacement.storeExpression!
  const expr = rendererStoreExpression(expression)
  if (!replacement.mountedRendererName) return expr
  if (mountStoreParamClass(source, replacement.mountedRendererName)) return expr
  if (isIdentifier(expression) && globalAccessorReturnType(source, sanitizeCppIdentifier(expression), typedStoreAccessorTypes).startsWith('std::shared_ptr<')) {
    return `gea_cpp_key(${expr})`
  }
  return expr
}

// Emit the body that produces the static template's `Node`. The IR `mount_*` renderers
// return a `gea::embedded::ui::NodeHandle`; when the method is typed on it we return that
// straight through (no bridge). When the base fell back to `gea_cpp_value` we wrap it in
// the `nodeValue` record so the override's return type still matches the base.
function rendererBody(
  replacement: RendererReplacement,
  returnType: string,
  source: string,
  typedStoreAccessorTypes: ReadonlyMap<string, string> = new Map(),
): string {
  const wrap = (expression: string): string =>
    returnsNodeHandle(returnType) ? expression : `gea_ir::nodeValue(${expression})`
  if (replacement.mountedRendererName && replacement.storeExpression) {
    return [
      '{',
      '    auto __gea_static_disposer = std::make_shared<gea_ir::NativeDisposer>();',
      `    return ${wrap(`gea_ir::${replacement.mountedRendererName}(${mountStoreArgument(replacement, source, typedStoreAccessorTypes)}, __gea_static_disposer)`)};`,
      '  }',
    ].join('\n')
  }
  if (!replacement.stateReaderName || !replacement.storeExpression) {
    return ['{', `    return ${wrap(`gea_ir::${replacement.rendererName}()`)};`, '  }'].join('\n')
  }
  // Legacy state-reader path: only the boxed `read_<Store>_state(gea_cpp_value)`
  // overload is declared this early, so a typed global cell boxes through
  // gea_cpp_key (identity for boxed cells).
  const stateStoreExpr =
    isIdentifier(replacement.storeExpression) &&
    globalAccessorReturnType(source, sanitizeCppIdentifier(replacement.storeExpression), typedStoreAccessorTypes).startsWith('std::shared_ptr<')
      ? `gea_cpp_key(${rendererStoreExpression(replacement.storeExpression)})`
      : rendererStoreExpression(replacement.storeExpression)
  return [
    '{',
    `    const auto __gea_ir_state = gea_ir::${replacement.stateReaderName}(${stateStoreExpr});`,
    `    return ${wrap(`gea_ir::${replacement.rendererName}(__gea_ir_state)`)};`,
    '  }',
  ].join('\n')
}

function rendererStoreExpression(expression: string): string {
  return isIdentifier(expression) ? `__gea_global_${sanitizeCppIdentifier(expression)}()` : expression
}

function isIdentifier(expression: string): boolean {
  return /^[A-Za-z_$][A-Za-z0-9_$]*$/.test(expression)
}

function findMatchingBrace(source: string, open: number): number {
  let depth = 0
  let quote: string | null = null
  let escaped = false
  for (let index = open; index < source.length; index += 1) {
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
    if (char === '{') depth += 1
    else if (char === '}') {
      depth -= 1
      if (depth === 0) return index
    }
  }
  return -1
}

function escapeRegExp(text: string): string {
  return text.replace(/[.*+?^${}()|[\]\\]/g, '\\$&')
}

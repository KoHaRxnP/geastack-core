import ts from 'typescript'
import type { GeaIrStoreExpr, GeaIrStoreLocalType, GeaIrStoreStmt } from './types.js'

export function parseStoreMethodBody(body: string): GeaIrStoreStmt[] | null {
  const source = ts.createSourceFile('__gea_store_method.ts', `function __gea_store_method() ${body}`, ts.ScriptTarget.Latest, true, ts.ScriptKind.TSX)
  const fn = source.statements.find(ts.isFunctionDeclaration)
  return fn?.body ? lowerStatementList(fn.body.statements) : null
}

function lowerStatementList(statements: readonly ts.Statement[]): GeaIrStoreStmt[] | null {
  const lowered: GeaIrStoreStmt[] = []
  for (let i = 0; i < statements.length; i++) {
    const statement = statements[i]
    // Guard-continue rewrite: `if (cond) continue; <rest>` is equivalent to
    // `if (!cond) { <rest> }`. The supported store-method control-flow IR has no
    // `continue`; without this rewrite a leading guard-continue (a common loop
    // idiom) forces the whole method onto the generic compiled-store fallback,
    // whose value-copy + dangling-ref array lowering is exactly what we want to
    // avoid. Only the early-guard shape (consequent is a bare `continue`, no
    // else) is matched.
    const guard = continueGuard(statement)
    if (guard) {
      const rest = lowerStatementList(statements.slice(i + 1))
      if (rest === null) return null
      // `if (cond) { body…; continue } <rest>` ≡ `if (cond) { body… } else { <rest> }`
      // (and with an empty body, the original `if (!cond) { <rest> }` shape).
      if (guard.body.length > 0) {
        lowered.push({ kind: 'if', test: guard.test, consequent: guard.body, alternate: rest })
      } else {
        lowered.push({ kind: 'if', test: { kind: 'unary', op: '!', arg: guard.test }, consequent: rest })
      }
      return lowered
    }
    const next = lowerStatement(statement)
    if (!next) return null
    lowered.push(...next)
  }
  return lowered
}

function continueGuard(statement: ts.Statement): { test: GeaIrStoreExpr; body: GeaIrStoreStmt[] } | null {
  if (!ts.isIfStatement(statement) || statement.elseStatement) return null
  const then = statement.thenStatement
  const isBareContinue = (s: ts.Statement): boolean => ts.isContinueStatement(s) && !s.label
  let bodyStatements: readonly ts.Statement[] | null = null
  if (isBareContinue(then)) {
    bodyStatements = []
  } else if (ts.isBlock(then) && then.statements.length >= 1 && isBareContinue(then.statements[then.statements.length - 1])) {
    bodyStatements = then.statements.slice(0, -1)
  }
  if (bodyStatements === null) return null
  const test = lowerExpression(statement.expression)
  if (!test) return null
  const body = lowerStatementList(bodyStatements)
  if (body === null) return null
  return { test, body }
}

function lowerStatement(statement: ts.Statement): GeaIrStoreStmt[] | null {
  if (ts.isBlock(statement)) return lowerStatementList(statement.statements)
  if (ts.isVariableStatement(statement)) return lowerVariableDeclarations(statement.declarationList)
  if (ts.isExpressionStatement(statement)) {
    const lowered = lowerExpressionStatement(statement.expression)
    return lowered ? [lowered] : null
  }
  if (ts.isIfStatement(statement)) {
    const lowered = lowerIfStatement(statement)
    return lowered ? [lowered] : null
  }
  if (ts.isForStatement(statement)) {
    const lowered = lowerForStatement(statement)
    return lowered ? [lowered] : null
  }
  if (ts.isWhileStatement(statement)) {
    // `while (cond) body` is `for (; cond; ) body` — the IR's `for` already
    // models optional init/update, so no new statement kind is needed.
    const test = lowerExpression(statement.expression)
    if (!test) return null
    const body = lowerStatement(statement.statement)
    return body ? [{ kind: 'for', test, body }] : null
  }
  if (ts.isReturnStatement(statement)) {
    if (!statement.expression) return [{ kind: 'return' }]
    const value = lowerExpression(statement.expression)
    return value ? [{ kind: 'return', value }] : null
  }
  return null
}

function lowerVariableDeclarations(declarationList: ts.VariableDeclarationList): GeaIrStoreStmt[] | null {
  const mutable = (declarationList.flags & ts.NodeFlags.Const) === 0
  const lowered: GeaIrStoreStmt[] = []
  for (const declaration of declarationList.declarations) {
    if (!ts.isIdentifier(declaration.name)) return null
    const localType = declaration.type ? lowerLocalType(declaration.type) ?? undefined : undefined
    if (!declaration.initializer) {
      lowered.push({ kind: 'var', name: declaration.name.text, mutable, localType })
      continue
    }
    const init = lowerExpression(declaration.initializer)
    if (!init) return null
    lowered.push({ kind: 'var', name: declaration.name.text, mutable, init, localType })
  }
  return lowered
}

function lowerArrayElementType(element: ts.TypeNode): GeaIrStoreLocalType | null {
  if (ts.isTypeReferenceNode(element) && ts.isIdentifier(element.typeName)) {
    return { kind: 'array', elementTypeName: element.typeName.text }
  }
  // Primitive-element arrays (`number[]`, `string[]`, `boolean[]`) lower to a
  // native std::vector of the primitive C++ type. Without this they had no
  // recorded local type and fell back to a boxed std::vector<gea_cpp_value>
  // with a dynamic `.push` probe that fails to compile against std::vector.
  switch (element.kind) {
    case ts.SyntaxKind.NumberKeyword:
      return { kind: 'array', elementPrimitive: 'number' }
    case ts.SyntaxKind.StringKeyword:
      return { kind: 'array', elementPrimitive: 'string' }
    case ts.SyntaxKind.BooleanKeyword:
      return { kind: 'array', elementPrimitive: 'boolean' }
    default:
      return null
  }
}

function lowerLocalType(type: ts.TypeNode): GeaIrStoreLocalType | null {
  if (ts.isArrayTypeNode(type)) {
    return lowerArrayElementType(type.elementType)
  }
  if (ts.isTypeReferenceNode(type) && ts.isIdentifier(type.typeName) && type.typeName.text === 'Array' && type.typeArguments?.length === 1) {
    return lowerArrayElementType(type.typeArguments[0])
  }
  return null
}

function lowerExpressionStatement(expression: ts.Expression): GeaIrStoreStmt | null {
  const assignment = lowerAssignmentExpression(expression)
  if (assignment) return assignment
  const update = lowerUpdateExpressionAsAssignment(expression)
  if (update) return update
  const expr = lowerExpression(expression)
  return expr ? { kind: 'expr', expr } : null
}

function lowerIfStatement(statement: ts.IfStatement): Extract<GeaIrStoreStmt, { kind: 'if' }> | null {
  const test = lowerExpression(statement.expression)
  const consequent = lowerStatement(statement.thenStatement)
  if (!test || !consequent) return null
  if (!statement.elseStatement) return { kind: 'if', test, consequent }
  const alternate = lowerStatement(statement.elseStatement)
  if (!alternate) return null
  return { kind: 'if', test, consequent, alternate }
}

function lowerForStatement(statement: ts.ForStatement): Extract<GeaIrStoreStmt, { kind: 'for' }> | null {
  let init: GeaIrStoreStmt | undefined
  let test: GeaIrStoreExpr | undefined
  let update: GeaIrStoreExpr | undefined
  if (statement.initializer) {
    init = lowerForInit(statement.initializer) ?? undefined
    if (!init) return null
  }
  if (statement.condition) {
    test = lowerExpression(statement.condition) ?? undefined
    if (!test) return null
  }
  if (statement.incrementor) {
    update = lowerExpression(statement.incrementor) ?? undefined
    if (!update) return null
  }
  const body = lowerStatement(statement.statement)
  return body ? { kind: 'for', init, test, update, body } : null
}

function lowerForInit(initializer: ts.ForInitializer): GeaIrStoreStmt | null {
  if (ts.isVariableDeclarationList(initializer)) {
    const declarations = lowerVariableDeclarations(initializer)
    return declarations?.length === 1 ? declarations[0] : null
  }
  const assignment = lowerAssignmentExpression(initializer)
  return assignment?.kind === 'assign' ? assignment : null
}

function lowerAssignmentExpression(expression: ts.Expression): Extract<GeaIrStoreStmt, { kind: 'assign' }> | null {
  if (!ts.isBinaryExpression(expression)) return null
  const assignmentOp = assignmentOperator(expression.operatorToken.kind)
  if (!assignmentOp) return null
  const target = lowerExpression(expression.left)
  const right = lowerExpression(expression.right)
  if (!target || !right) return null
  if (assignmentOp === '=') return { kind: 'assign', target, value: right }
  return { kind: 'assign', target, value: { kind: 'binary', op: assignmentOp, left: target, right } }
}

function lowerUpdateExpressionAsAssignment(expression: ts.Expression): Extract<GeaIrStoreStmt, { kind: 'assign' }> | null {
  if (!ts.isPrefixUnaryExpression(expression) && !ts.isPostfixUnaryExpression(expression)) return null
  const op = expression.operator === ts.SyntaxKind.PlusPlusToken ? '+' : expression.operator === ts.SyntaxKind.MinusMinusToken ? '-' : null
  if (!op) return null
  const target = lowerExpression(expression.operand)
  return target ? { kind: 'assign', target, value: { kind: 'binary', op, left: target, right: { kind: 'number', value: 1 } } } : null
}

function lowerExpression(expression: ts.Expression): GeaIrStoreExpr | null {
  if (ts.isParenthesizedExpression(expression)) return lowerExpression(expression.expression)
  if (ts.isAsExpression(expression) || ts.isTypeAssertionExpression(expression) || ts.isNonNullExpression(expression) || ts.isSatisfiesExpression(expression)) {
    return lowerExpression(expression.expression)
  }
  if (ts.isNumericLiteral(expression)) return { kind: 'number', value: Number(expression.text) }
  if (ts.isStringLiteral(expression) || ts.isNoSubstitutionTemplateLiteral(expression)) return { kind: 'string', value: expression.text }
  if (expression.kind === ts.SyntaxKind.TrueKeyword) return { kind: 'boolean', value: true }
  if (expression.kind === ts.SyntaxKind.FalseKeyword) return { kind: 'boolean', value: false }
  if (expression.kind === ts.SyntaxKind.NullKeyword) return { kind: 'null' }
  if (ts.isIdentifier(expression)) return { kind: 'identifier', name: expression.text }
  if (expression.kind === ts.SyntaxKind.ThisKeyword) return { kind: 'this' }
  if (ts.isPropertyAccessExpression(expression)) {
    const object = lowerExpression(expression.expression)
    return object ? { kind: 'member', object, property: expression.name.text } : null
  }
  if (ts.isElementAccessExpression(expression)) {
    const object = lowerExpression(expression.expression)
    const index = expression.argumentExpression ? lowerExpression(expression.argumentExpression) : null
    return object && index ? { kind: 'index', object, index } : null
  }
  if (ts.isCallExpression(expression)) {
    const callee = lowerExpression(expression.expression)
    const args = expression.arguments.map((arg) => lowerExpression(arg))
    return callee && allExpressions(args) ? { kind: 'call', callee, args } : null
  }
  if (ts.isObjectLiteralExpression(expression)) return lowerObjectLiteral(expression)
  if (ts.isArrayLiteralExpression(expression)) {
    const elements = expression.elements.map((element) => lowerExpression(element))
    return allExpressions(elements) ? { kind: 'array', elements } : null
  }
  if (ts.isPrefixUnaryExpression(expression)) {
    const op = unaryOperator(expression.operator)
    const arg = lowerExpression(expression.operand)
    return op && arg ? { kind: 'unary', op, arg } : null
  }
  if (ts.isPostfixUnaryExpression(expression)) {
    const op = expression.operator === ts.SyntaxKind.PlusPlusToken ? '++' : expression.operator === ts.SyntaxKind.MinusMinusToken ? '--' : null
    const arg = lowerExpression(expression.operand)
    return op && arg ? { kind: 'update', op, arg, prefix: false } : null
  }
  if (ts.isBinaryExpression(expression)) return lowerBinaryExpression(expression)
  if (ts.isConditionalExpression(expression)) {
    const test = lowerExpression(expression.condition)
    const consequent = lowerExpression(expression.whenTrue)
    const alternate = lowerExpression(expression.whenFalse)
    return test && consequent && alternate ? { kind: 'conditional', test, consequent, alternate } : null
  }
  if (ts.isTemplateExpression(expression)) return lowerTemplateExpression(expression)
  return null
}

function lowerObjectLiteral(expression: ts.ObjectLiteralExpression): GeaIrStoreExpr | null {
  const fields: Array<{ name: string; value: GeaIrStoreExpr }> = []
  for (const property of expression.properties) {
    if (!ts.isPropertyAssignment(property)) return null
    const name = propertyName(property.name)
    const value = lowerExpression(property.initializer)
    if (!name || !value) return null
    fields.push({ name, value })
  }
  return { kind: 'object', fields }
}

function lowerBinaryExpression(expression: ts.BinaryExpression): GeaIrStoreExpr | null {
  const op = binaryOperator(expression.operatorToken.kind)
  const left = lowerExpression(expression.left)
  const right = lowerExpression(expression.right)
  if (!op || !left || !right) return null
  return { kind: op === '&&' || op === '||' ? 'logical' : 'binary', op, left, right }
}

function lowerTemplateExpression(expression: ts.TemplateExpression): GeaIrStoreExpr | null {
  const parts: GeaIrStoreExpr[] = []
  if (expression.head.text) parts.push({ kind: 'string', value: expression.head.text })
  for (const span of expression.templateSpans) {
    const expr = lowerExpression(span.expression)
    if (!expr) return null
    parts.push(expr)
    if (span.literal.text) parts.push({ kind: 'string', value: span.literal.text })
  }
  if (parts.length === 0) return { kind: 'string', value: '' }
  return parts.reduce((left, right) => ({ kind: 'binary', op: '+', left, right }))
}

function propertyName(name: ts.PropertyName): string | null {
  if (ts.isIdentifier(name) || ts.isStringLiteral(name) || ts.isNumericLiteral(name)) return name.text
  return null
}

function allExpressions(values: Array<GeaIrStoreExpr | null>): values is GeaIrStoreExpr[] {
  return values.every((value) => value !== null)
}

function assignmentOperator(kind: ts.SyntaxKind): string | null {
  if (kind === ts.SyntaxKind.EqualsToken) return '='
  if (kind === ts.SyntaxKind.PlusEqualsToken) return '+'
  if (kind === ts.SyntaxKind.MinusEqualsToken) return '-'
  if (kind === ts.SyntaxKind.AsteriskEqualsToken) return '*'
  if (kind === ts.SyntaxKind.SlashEqualsToken) return '/'
  if (kind === ts.SyntaxKind.PercentEqualsToken) return '%'
  if (kind === ts.SyntaxKind.BarEqualsToken) return '|'
  if (kind === ts.SyntaxKind.LessThanLessThanEqualsToken) return '<<'
  if (kind === ts.SyntaxKind.GreaterThanGreaterThanEqualsToken) return '>>'
  if (kind === ts.SyntaxKind.GreaterThanGreaterThanGreaterThanEqualsToken) return '>>>'
  return null
}

function binaryOperator(kind: ts.SyntaxKind): string | null {
  if (kind === ts.SyntaxKind.PlusToken) return '+'
  if (kind === ts.SyntaxKind.MinusToken) return '-'
  if (kind === ts.SyntaxKind.AsteriskToken) return '*'
  if (kind === ts.SyntaxKind.SlashToken) return '/'
  if (kind === ts.SyntaxKind.PercentToken) return '%'
  if (kind === ts.SyntaxKind.LessThanToken) return '<'
  if (kind === ts.SyntaxKind.LessThanEqualsToken) return '<='
  if (kind === ts.SyntaxKind.GreaterThanToken) return '>'
  if (kind === ts.SyntaxKind.GreaterThanEqualsToken) return '>='
  if (kind === ts.SyntaxKind.EqualsEqualsToken || kind === ts.SyntaxKind.EqualsEqualsEqualsToken) return '=='
  if (kind === ts.SyntaxKind.ExclamationEqualsToken || kind === ts.SyntaxKind.ExclamationEqualsEqualsToken) return '!='
  if (kind === ts.SyntaxKind.AmpersandAmpersandToken) return '&&'
  if (kind === ts.SyntaxKind.BarBarToken) return '||'
  if (kind === ts.SyntaxKind.BarToken) return '|'
  if (kind === ts.SyntaxKind.LessThanLessThanToken) return '<<'
  if (kind === ts.SyntaxKind.GreaterThanGreaterThanToken) return '>>'
  if (kind === ts.SyntaxKind.GreaterThanGreaterThanGreaterThanToken) return '>>>'
  return null
}

function unaryOperator(kind: ts.PrefixUnaryOperator): string | null {
  if (kind === ts.SyntaxKind.PlusToken) return '+'
  if (kind === ts.SyntaxKind.MinusToken) return '-'
  if (kind === ts.SyntaxKind.ExclamationToken) return '!'
  return null
}

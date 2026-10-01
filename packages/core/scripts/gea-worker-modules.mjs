import fs from "node:fs";
import path from "node:path";
import crypto from "node:crypto";
import { createRequire } from "node:module";
import { pathToFileURL } from "node:url";

const require = createRequire(new URL("../package.json", import.meta.url));
const { parse } = require("@babel/parser");
const traversal = require("@babel/traverse");
const traverse = traversal.default ?? traversal;

/** Follow ordinary imports and native module edges without inspecting unused tests. */
export function collectWorkerModuleSources(
  entry,
  importedFiles,
  readSource = (file) => fs.readFileSync(file, "utf8"),
) {
  const pending = [entry];
  const visited = new Set();
  const sources = new Map();
  while (pending.length) {
    const file = pending.pop();
    if (visited.has(file)) continue;
    visited.add(file);
    pending.push(...importedFiles(file));
    if (!/\.[cm]?[jt]sx?$/.test(file)) continue;
    const resolved = resolveWorkerModules(readSource(file), file);
    sources.set(file, resolved);
    pending.push(...resolved.modules.map((module) => module.entry));
  }
  return sources;
}

/** Native equivalent of a bundler's static worker URL resolution. */
export function resolveWorkerModules(
  code,
  fileName,
  resolveModule = (specifier) =>
    path.resolve(path.dirname(fileName), specifier),
) {
  const source = parse(code, {
    sourceType: "module",
    plugins: ["typescript", "jsx"],
  });
  const modules = [];
  const edits = [];
  const staticURL = (argument) => {
    if (argument?.type === "StringLiteral") return argument.value;
    if (
      argument?.type !== "NewExpression" ||
      argument.callee.type !== "Identifier" ||
      argument.callee.name !== "URL"
    )
      return null;
    const [url, base] = argument.arguments;
    if (
      url?.type !== "StringLiteral" ||
      base?.type !== "MemberExpression" ||
      base.computed ||
      base.property.name !== "url"
    )
      return null;
    if (
      base.object.type !== "MetaProperty" ||
      base.object.meta.name !== "import" ||
      base.object.property.name !== "meta"
    )
      return null;
    return url.value;
  };
  const visit = (astPath) => {
    const node = astPath.node;
    const binding = astPath.scope.getBinding("Worker");
    const typeOnlyBinding =
      binding &&
      (binding.path.node.importKind === "type" ||
        binding.path.parent.importKind === "type");
    const worker =
      node.type === "NewExpression" &&
      node.callee.type === "Identifier" &&
      node.callee.name === "Worker" &&
      (!binding || typeOnlyBinding);
    const call = node.callee;
    const worklet =
      node.type === "CallExpression" &&
      call.type === "MemberExpression" &&
      !call.computed &&
      call.property.name === "addModule" &&
      call.object.type === "MemberExpression" &&
      !call.object.computed &&
      call.object.property.name === "audioWorklet";
    if (!worker && !worklet) return;
    const argument = node.arguments[0];
    const specifier = staticURL(argument);
    if (specifier === null)
      throw new Error(
        `${fileName}: native worker/worklet module URL must be statically resolvable`,
      );
    if (!specifier.startsWith("."))
      throw new Error(
        `${fileName}: native worker/worklet modules must use a relative module URL`,
      );
    const entry = resolveModule(specifier);
    const url = pathToFileURL(entry).href;
    const kind = worker ? "worker" : "worklet";
    const id = crypto
      .createHash("sha256")
      .update(`${kind}:${url}`)
      .digest("hex")
      .slice(0, 16);
    modules.push({
      kind,
      entry,
      url,
      symbol: `gea_${kind}_${id}_top_level`,
      id,
    });
    edits.push({
      start: argument.start,
      end: argument.end,
      text: JSON.stringify(url),
    });
  };
  traverse(source, { NewExpression: visit, CallExpression: visit });
  let transformed = code;
  for (const edit of edits.sort((a, b) => b.start - a.start))
    transformed =
      transformed.slice(0, edit.start) +
      edit.text +
      transformed.slice(edit.end);
  return { code: transformed, modules };
}

export function writeWorkerRegistry(modules, output) {
  const includes = ['#include "host/worker.h"'];
  if (modules.some((module) => module.kind === "worklet"))
    includes.push('#include "host/audio_worklet.h"');
  const declarations = modules.map((module) => `void ${module.symbol}();`);
  const registrations = modules.map((module) => {
    const target = module.kind === "worker" ? "workers" : "audio_worklet";
    return `  gea::host::${target}::registerModule(${JSON.stringify(module.url)}, ${module.symbol});`;
  });
  const content = `${includes.join("\n")}\n${declarations.join("\n")}\nnamespace {\nstruct RegisterWorkerModules {\nRegisterWorkerModules() {\n${registrations.join("\n")}\n}\n};\nRegisterWorkerModules registeredWorkerModules;\n}\n`;
  if (!fs.existsSync(output) || fs.readFileSync(output, "utf8") !== content)
    fs.writeFileSync(output, content);
}

/** Validate the supported native registration subset using symbol/type identity. */
export function validateWorkletRegistrations(ts, entry, implementation) {
  const program = ts.createProgram([entry], {
    target: ts.ScriptTarget.ESNext,
    module: ts.ModuleKind.ESNext,
    moduleResolution: ts.ModuleResolutionKind.Bundler,
    allowJs: true,
    skipLibCheck: true,
    noEmit: true,
  });
  const checker = program.getTypeChecker();
  const platformFile = fs.realpathSync(implementation);
  const fail = (node, message) => {
    const source = node.getSourceFile();
    const location = source.getLineAndCharacterOfPosition(node.getStart());
    throw new Error(`${source.fileName}:${location.line + 1}: ${message}`);
  };
  const visit = (node) => {
    if (ts.isCallExpression(node)) {
      const declaration = checker.getResolvedSignature(node)?.declaration;
      if (
        declaration &&
        fs.realpathSync(declaration.getSourceFile().fileName) === platformFile &&
        declaration.name?.getText() === "registerProcessor"
      ) {
        let constructor = node.arguments[1];
        if (!constructor) fail(node, "registerProcessor requires a processor class");
        while (ts.isParenthesizedExpression(constructor) || ts.isAsExpression(constructor) || ts.isTypeAssertionExpression(constructor))
          constructor = constructor.expression;
        let symbol = checker.getSymbolAtLocation(constructor);
        if (symbol && symbol.flags & ts.SymbolFlags.Alias) symbol = checker.getAliasedSymbol(symbol);
        const classDeclaration = ts.isClassExpression(constructor) ? constructor : symbol?.valueDeclaration;
        if (!classDeclaration || (!ts.isClassDeclaration(classDeclaration) && !ts.isClassExpression(classDeclaration)))
          fail(node, "Native registerProcessor requires a directly named class constructor; indirect constructor values are unsupported");
        const type = checker.getTypeAtLocation(constructor);
        if (type.getProperty("parameterDescriptors"))
          fail(node, "AudioParam parameterDescriptors are not supported by this native AudioWorklet host");
      }
    }
    ts.forEachChild(node, visit);
  };
  for (const source of program.getSourceFiles()) {
    if (!source.isDeclarationFile && fs.realpathSync(source.fileName) !== platformFile) visit(source);
  }
}

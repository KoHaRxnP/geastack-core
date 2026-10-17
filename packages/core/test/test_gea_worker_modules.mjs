import assert from "node:assert/strict";
import path from "node:path";
import test from "node:test";
import { fileURLToPath, pathToFileURL } from "node:url";
import { discoverLocalImport } from "../scripts/gea-bundle-type-hints.mjs";
import {
  resolveWorkerModules,
  collectWorkerModuleSources,
} from "../scripts/gea-worker-modules.mjs";

// The fixtures are host paths: "/app/x" as is on POSIX, "C:\app\x" on Windows,
// where module resolution answers in that spelling.
const at = (file) => path.resolve(file);

test("module discovery follows nested native modules and skips unreachable tests", () => {
  const files = new Map([
    [at("/app/main.ts"), 'new Worker("./worker.ts", {type:"module"})'],
    [at("/app/worker.ts"), 'import "./audio.ts"'],
    [at("/app/audio.ts"), 'context.audioWorklet.addModule("./processor.ts")'],
    [at("/app/processor.ts"), 'registerProcessor("pcm", Processor)'],
    [at("/app/unused.test.ts"), "new Worker(dynamicURL)"],
  ]);
  const sources = collectWorkerModuleSources(
    at("/app/main.ts"),
    (file) => (file === at("/app/worker.ts") ? [at("/app/audio.ts")] : []),
    (file) => files.get(file),
  );
  assert.deepEqual([...sources.keys()].sort(), [
    at("/app/audio.ts"),
    at("/app/main.ts"),
    at("/app/processor.ts"),
    at("/app/worker.ts"),
  ]);
});

test("emitted JavaScript import names resolve back to TypeScript source", () => {
  const files = [];
  discoverLocalImport(
    "../runtime/audio-worklet.js",
    fileURLToPath(import.meta.url),
    files,
  );
  assert.deepEqual(files, [
    fileURLToPath(new URL("../runtime/audio-worklet.ts", import.meta.url)),
  ]);
});

test("worker URLs become registry keys without executing their modules on the UI", () => {
  const source =
    "const worker = new Worker(new URL('./agent.ts', import.meta.url), { type: 'module' })";
  const result = resolveWorkerModules(source, at("/app/src/main.ts"));
  assert.equal(result.modules.length, 1);
  assert.equal(result.modules[0].entry, at("/app/src/agent.ts"));
  assert.equal(result.modules[0].kind, "worker");
  // file:///app/src/agent.ts on POSIX.
  const url = pathToFileURL(at("/app/src/agent.ts")).href;
  assert.equal(
    result.code,
    `const worker = new Worker(${JSON.stringify(url)}, { type: 'module' })`,
  );
  assert.equal(
    result.modules[0].symbol,
    resolveWorkerModules(source, at("/app/src/main.ts")).modules[0].symbol,
  );
});

test("AudioWorklet modules preserve surrounding awaits and use their own entries", () => {
  const result = resolveWorkerModules(
    "await context.audioWorklet.addModule(new URL('./pcm.ts', import.meta.url))",
    at("/app/src/main.ts"),
  );
  assert.equal(result.modules[0].kind, "worklet");
  // file:///app/src/pcm.ts on POSIX.
  const url = pathToFileURL(at("/app/src/pcm.ts")).href;
  assert.equal(
    result.code,
    `await context.audioWorklet.addModule(${JSON.stringify(url)})`,
  );
});

test("native discovery rejects URLs it cannot compile", () => {
  assert.throws(
    () => resolveWorkerModules("new Worker(getURL())", "/app/main.ts"),
    /statically resolvable/,
  );
  assert.throws(
    () =>
      resolveWorkerModules(
        'new Worker("https://example.com/worker.js")',
        "/app/main.ts",
      ),
    /relative module URL/,
  );
});

test("unrelated addModule calls and URL construction are unchanged", () => {
  const source =
    'registry.addModule("./module.ts"); const u = new URL("./data", import.meta.url)';
  assert.equal(resolveWorkerModules(source, "/app/main.ts").code, source);
});

test("type imports do not shadow Worker but local values do", () => {
  assert.equal(
    resolveWorkerModules(
      'import type { Worker } from "@geastack/core"; new Worker("./agent.ts")',
      "/app/main.ts",
    ).modules.length,
    1,
  );
  assert.equal(
    resolveWorkerModules(
      'import { type Worker } from "@geastack/core"; new Worker("./agent.ts")',
      "/app/main.ts",
    ).modules.length,
    1,
  );
  const source = "function f(Worker) { return new Worker(getURL()) }";
  assert.equal(resolveWorkerModules(source, "/app/main.ts").code, source);
});

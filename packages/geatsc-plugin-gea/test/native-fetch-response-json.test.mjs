import assert from 'node:assert/strict'
import { spawnSync } from 'node:child_process'
import { createRequire } from 'node:module'
import { dirname, resolve } from 'node:path'
import test from 'node:test'

import { createGeaHostShims } from '../dist/host-shims.js'

test('native FetchResponse.json decodes response bytes and rejects malformed JSON', () => {
  const packageRoot = resolve(import.meta.dirname, '..')
  const packages = resolve(packageRoot, '..')
  const require = createRequire(resolve(packages, 'core/package.json'))
  const compilerRoot = process.env.GEATSC_ROOT ?? dirname(require.resolve('@geastack/compiler/package.json'))
  const method = createGeaHostShims().nativeMemberMethods.json.find(row =>
    row.receiverTypes.includes('gea::host::FetchResponse')
  )
  assert.ok(method, 'FetchResponse.json must have a native binding')
  const expression = method.emit.replaceAll('{receiver}', 'response')
  const binary = resolve(packageRoot, 'dist/native-fetch-response-json-test')
  const includes = [
    resolve(packages, 'core/include'),
    resolve(packages, 'host/include'),
    resolve(packages, 'engine'),
    resolve(packages, 'engine/ui'),
    resolve(packages, 'elements/ui'),
    resolve(compilerRoot, 'src/targets/cpp/runtime')
  ]
  const source = String.raw`
#define GEA_HOST_DECLARED 1
#include "gea/embedded.h"
#include "gea_runtime.h"
#include <cassert>

gea::Value parse(const std::string& text) {
  gea::host::FetchResponse response;
  response.body.assign(text.begin(), text.end());
  return ${expression};
}

int main() {
  const auto object = parse(R"({"value":42,"nested":{"enabled":true},"items":[1,null,"x"]})");
  assert(object.getProperty(gea::PropertyKey::string("value")).as<double>() == 42);
  const auto nested = object.getProperty(gea::PropertyKey::string("nested"));
  assert(nested.getProperty(gea::PropertyKey::string("enabled")).as<bool>());
  const auto items = object.getProperty(gea::PropertyKey::string("items"));
  assert(items.dynamicArrayLength("JSON response") == 3);
  assert(parse(" \n 150 \t").as<double>() == 150);
  assert(parse("null").tag() == gea::Value::Tag::Null);
  assert(parse("false").as<bool>() == false);
  assert(parse(R"("a\nb")").as<std::string>() == "a\nb");

  const std::string invalid[] = {
    "", " ", "{", R"({"value":42)", R"({"value":42} trailing)",
    R"({"value" 42})", R"({"value":42,})", "[1,]", "[1", "01", "+1", ".5",
    "1.", "1e", "-", "tru", "nul", R"("\x")", R"("\u12G4")",
    std::string("\"control") + char(1) + "\""
  };
  for (const auto& text : invalid) {
    bool syntaxError = false;
    try { parse(text); }
    catch (const gea::Value& error) {
      syntaxError = gea::host::instanceOfRuntimeError(error, "SyntaxError");
    }
    assert(syntaxError);
  }
}
`
  const compiled = spawnSync(
    process.env.CXX ?? 'clang++',
    ['-std=c++20', ...includes.map(path => `-I${path}`), '-x', 'c++', '-', '-o', binary],
    { input: source, encoding: 'utf8' }
  )
  assert.equal(compiled.status, 0, compiled.error?.message ?? compiled.stderr)
  const executed = spawnSync(binary, [], { encoding: 'utf8' })
  assert.equal(executed.status, 0, executed.error?.message ?? executed.stderr)
})

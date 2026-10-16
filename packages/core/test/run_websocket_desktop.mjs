// SPDX-License-Identifier: Apache-2.0
import { spawnSync } from 'node:child_process'
import { fileURLToPath } from 'node:url'
import { resolve } from 'node:path'
if (process.platform !== 'darwin') throw new Error('This test requires macOS (the Apple provider); the Linux provider is not exercised here')
const core = fileURLToPath(new URL('../../../', import.meta.url))
const host = resolve(core, 'packages/host')
const tests = resolve(core, 'packages/core/test')
const binary = resolve(core, '../dist/websocket-desktop-test')
const run = (exe, args, timeout) => {
  const result = spawnSync(exe, args, { stdio: 'inherit', timeout })
  if (result.error) throw result.error
  if (result.status !== 0) throw new Error(`${exe} failed: ${result.status ?? result.signal}`)
}
run('clang++', ['-std=c++20', '-O1', '-fobjc-arc', '-fblocks', '-I', resolve(host, 'include'),
  resolve(tests, 'test_websocket_desktop.cpp'), ...['websocket.cpp', 'websocket_apple.mm', 'worker.cpp'].map(n => resolve(host, 'host', n)),
  '-framework', 'Foundation', '-o', binary], 90000)
run('python3', [resolve(tests, 'test_websocket_desktop_loopback.py'), binary], 30000)

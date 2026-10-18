import assert from 'node:assert/strict'
import fs from 'node:fs'
import os from 'node:os'
import path from 'node:path'
import { build } from 'vite'
import { geaModuleGraphPlugins } from '../scripts/gea-vite-module-graph-plugin.mjs'

// The build driver names where it reads the module graph through
// GEA_VITE_MODULE_GRAPH_OUT. An app's own Vite config that also names an
// output directory (for its standalone runs) must not move the graph away from
// the driver, which would then find no graph to compile.
const root = fs.realpathSync(fs.mkdtempSync(path.join(os.tmpdir(), 'gea-module-graph-out-')))
try {
  fs.writeFileSync(path.join(root, 'index.js'), "import { value } from './value.js'\nconsole.log(value)\n")
  fs.writeFileSync(path.join(root, 'value.js'), 'export const value = 1\n')
  const appConfigured = path.join(root, 'app-graph')
  const driverChosen = path.join(root, 'driver-graph')
  const build_ = async () => build({
    root,
    logLevel: 'silent',
    configFile: false,
    plugins: geaModuleGraphPlugins({ outDir: appConfigured }),
    build: { outDir: path.join(root, 'dist'), emptyOutDir: true, minify: false, rollupOptions: { input: path.join(root, 'index.js') } },
  })

  process.env.GEA_VITE_MODULE_GRAPH_OUT = driverChosen
  await build_()
  assert.ok(fs.existsSync(path.join(driverChosen, 'gea-module-graph.json')), 'the graph is where the driver reads it')
  assert.ok(!fs.existsSync(appConfigured), 'the app config does not redirect a driven build')

  delete process.env.GEA_VITE_MODULE_GRAPH_OUT
  await build_()
  assert.ok(fs.existsSync(path.join(appConfigured, 'gea-module-graph.json')), 'a standalone run uses the configured directory')
  console.log('the build driver decides where the module graph is written')
} finally {
  fs.rmSync(root, { recursive: true, force: true })
}

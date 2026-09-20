import assert from 'node:assert/strict'

import { transformGeaEmbeddedCompatSource } from '../scripts/gea-embedded-compat-transform.mjs'
import {
  COMPAT_STAGING_IGNORED_DIRECTORIES,
  shouldIgnoreCompatStagingDirectory,
} from '../scripts/gea-embedded-compat-staging.mjs'

for (const directory of ['.build-test', '.scratch', '.test-tmp', 'generated-output']) {
  assert.equal(
    shouldIgnoreCompatStagingDirectory(directory),
    true,
    `compat staging should exclude transient directory ${directory}`,
  )
  assert.ok(COMPAT_STAGING_IGNORED_DIRECTORIES.includes(directory))
}
assert.equal(shouldIgnoreCompatStagingDirectory('components'), false)

const componentFromGeastackCore = `
import { Component } from '@geastack/core'

export function App() {
  return <display><text>Hello</text></display>
}
`

const transformedExistingComponent = transformGeaEmbeddedCompatSource(componentFromGeastackCore, '/tmp/App.tsx')

assert.equal(
  transformedExistingComponent.includes('from "gea-embedded"') || transformedExistingComponent.includes("from 'gea-embedded'"),
  true,
  'existing Component import from @geastack/core should move to the resolvable gea-embedded runtime import',
)
assert.match(
  transformedExistingComponent,
  /import \{\s*Component\s*\} from ['"]gea-embedded['"]/,
  'Component should be moved to the Gea runtime import recognized by the IR plugin',
)
assert.match(
  transformedExistingComponent,
  /export class App extends Component/,
  'function components should still become Component subclasses',
)

const coreImportWithoutComponent = `
import { Store } from '@geastack/core'

export const Tile = () => <display><text>Tile</text></display>
`

const transformedMissingComponent = transformGeaEmbeddedCompatSource(coreImportWithoutComponent, '/tmp/Tile.tsx')

assert.match(
  transformedMissingComponent,
  /import \{\s*Store,\s*Component\s*\} from ['"]gea-embedded['"]/,
  'Store and Component should be imported from the Gea runtime package recognized by the IR plugin',
)
assert.equal(
  (transformedMissingComponent.match(/from ['"]gea-embedded['"]/g) ?? []).length,
  1,
  'adding Component to a normalized runtime import must create exactly one gea-embedded import',
)

const storeOnlyModule = `
import { Store } from '@geastack/core'

export class GameStore extends Store {
  cell0 = ' '
}

export const game = new GameStore()
`

const transformedStoreOnlyModule = transformGeaEmbeddedCompatSource(storeOnlyModule, '/tmp/GameStore.tsx')

assert.match(
  transformedStoreOnlyModule,
  /import \{\s*Store\s*\} from ['"]gea-embedded['"]/,
  'non-JSX store modules should still be normalized for the Gea IR plugin',
)

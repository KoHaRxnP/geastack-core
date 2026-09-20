import assert from 'node:assert/strict'
import test from 'node:test'

import { createGeaHostShims } from '../dist/host-shims.js'

test('geolocation snapshot operations publish their non-throwing physical contract', () => {
  const definitions = createGeaHostShims()
  for (const namespace of ['Geolocation', 'geolocation']) {
    const methods = definitions.nativeNamespaceMethods?.[namespace]
    assert.equal(methods?.currentPosition?.noThrow, true, `${namespace}.currentPosition must be sealed noThrow`)
    assert.equal(methods?.coords?.noThrow, true, `${namespace}.coords must be sealed noThrow`)
  }
})

import fs from 'node:fs'
import path from 'node:path'
import { createHash } from 'node:crypto'

// One identity for Vite's native module graph and the firmware lookup table.
export function describeStaticAsset(source) {
  const contents = fs.readFileSync(source)
  const sha256 = createHash('sha256').update(contents).digest('hex')
  return {
    source,
    bytes: contents.length,
    sha256,
    url: `gea-asset://sha256/${sha256}/${encodeURIComponent(path.basename(source))}`,
  }
}

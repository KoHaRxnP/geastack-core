//! compile-only
//! emitted-has: readDynamicValue()
import type { FetchResponse } from '@geastack/core'
const response: FetchResponse = fetchResult(1)
const payload = response.json() as { value: number }
console.log('' + payload.value)

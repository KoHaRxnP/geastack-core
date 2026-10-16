import type { PointerEvent } from '@geastack/core'
const release = (event: PointerEvent) => { event.preventDefault() }
window.addEventListener('pointerup', release)
window.removeEventListener('pointerup', release)

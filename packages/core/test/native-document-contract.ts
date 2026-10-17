import type { PointerEvent } from '@geastack/core'
const release = (event: PointerEvent) => {
  if (event.type === 'pointerup' && event.clientX === 41 && event.button === 0 && event.buttons === 0) event.preventDefault()
}
document.addEventListener('pointerup', release, true)
document.removeEventListener('pointerup', release, true)
document.addEventListener('pointerup', release, { capture: true, once: true })
const legacy = (event: PointerEvent) => {
  if (event.type !== 'touchend') throw new Error('native legacy spelling')
}
document.addEventListener('touchend', legacy, { once: true })
document.addEventListener('pointermove', release)
document.removeEventListener('pointermove', release)
const removed = (event: PointerEvent) => { event.preventDefault() }
document.addEventListener('pointermove', removed, { capture: true, once: true })
document.removeEventListener('pointermove', removed, { capture: true })
document.addEventListener('pointerdown', (event: PointerEvent) => {
  if (event.button === 0 && event.buttons === 1) event.preventDefault()
}, { once: true })
console.log('native document listeners registered')

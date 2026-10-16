import type { PointerEvent } from '@geastack/core'
let released = 0
const release = (event: PointerEvent) => {
  released++
  if (window.innerWidth === 320 && window.innerHeight === 240 && event.clientX === 41) event.preventDefault()
  console.log(event.clientX >= 0 ? 'native release' : 'native release outside')
}
window.addEventListener('pointerup', release, true)
window.removeEventListener('pointerup', release, true)
window.addEventListener('pointerup', release, { capture: true, once: true })
window.addEventListener('pointermove', release)
window.removeEventListener('pointermove', release)
const blur = () => { console.log('native window blur') }
window.addEventListener('blur', blur, { once: true })
window.removeEventListener('blur', blur)
window.addEventListener('resize', () => {
  window.addEventListener('pointerdown', (event: PointerEvent) => {
    if (window.innerWidth === 640 && window.innerHeight === 480 && event.clientX === 41) event.preventDefault()
  }, { once: true })
})
console.log(released === 0 ? 'native listeners registered' : 'unexpected native event')

import type { PointerEvent, KeyEvent, PressEvent, TouchEvent, Event, InputEvent } from '@geastack/core'
const button = <button id="native-bare-events"
 pointerdown={(e: PointerEvent) => { if(e.type === 'pointerdown' && e.button === 0 && e.buttons === 1)e.preventDefault() }}
 pointermove={(e: PointerEvent) => { if(e.type === 'pointermove')e.preventDefault() }}
 touchstart={(e: TouchEvent) => { if(e.type === 'touchstart')e.preventDefault() }}
 touchmove={(e: TouchEvent) => { if(e.type === 'touchmove')e.preventDefault() }}
 touchend={(e: TouchEvent) => { if(e.type === 'touchend')e.preventDefault() }}
 touchcancel={(e: PointerEvent) => { if(e.type === 'touchcancel')e.preventDefault() }}
 scroll={(e: Event) => { e.preventDefault() }}
 input={(e: InputEvent) => { if(e.currentTarget.value !== 'typed-input')throw new Error('input value projection') }}
 pointerup={(e: PointerEvent) => { if(e.type === 'pointerup')e.preventDefault() }}
 pointercancel={(e: PointerEvent) => { if(e.type === 'pointercancel')e.preventDefault() }}
 click={(e: PressEvent) => { if(e.type === 'click')e.preventDefault() }}
 keydown={(e: KeyEvent) => { if(e.key === ' ' && e.code === 'Space')e.preventDefault() }}
 keyup={(e: KeyEvent) => { if(e.key === ' ' && e.code === 'Space')e.preventDefault() }} />
console.log('typed bare JSX event listeners installed')

import type { InputEvent } from '@geastack/core'
const control = <input id="typed-change-control"
 input={(event: InputEvent) => {if(event.currentTarget.value !== 'edited')throw new Error('typed input value')}}
 change={(event: InputEvent) => {if(event.currentTarget.value !== 'edited')throw new Error('typed committed value');event.currentTarget.disabled = true}} />
console.log('actual typed change handler installed')

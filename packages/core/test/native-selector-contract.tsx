import type { Element } from '@geastack/core'

// Real typed JSX producer: native CPU verification must inspect these actual
// emitted nodes, not replace the bool producer with a hand-authored setter.
const caption = <p id="jsx-text-leaf" class="status" data-role="feedback">Caption</p>
const off = <button id="jsx-disabled-false" disabled={false}>Enabled</button>
const on = <button id="jsx-disabled-true" disabled={true}>Disabled</button>

const input = document.createElement('input')
input.setAttribute('id', 'reflected-disabled')
input.addEventListener('input', event => {
  event.currentTarget.disabled = false
  console.log(event.currentTarget.matches('input:not(:disabled)') ? 'enabled input' : 'disabled input')
  console.log(event.currentTarget.closest<Element>('body') !== null ? 'body ancestor' : 'detached input')
})
const root = document.body
root.appendChild(input)
const match = input.matches('input:not(:disabled)')
const ancestor = input.closest<Element>('body')
const found = root.querySelector('input[id^=reflected-]')
const all = document.querySelectorAll('button, input')
console.log(match && ancestor !== null && found !== null && all.length > 0 ? 'native selectors reachable' : 'native selectors not ready')
// Allocation writes are side effects. CPU verification will require both IDs
// in the real Tree, rather than substitute a host carrier or node equality.
console.log('typed JSX allocations created')

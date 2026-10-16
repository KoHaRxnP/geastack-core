import type {InputElement} from '@geastack/core'
const inferred = document.createElement('input')
const input: InputElement = inferred
input.setAttribute('id', 'typed-input-contract')
document.body.appendChild(input)
input.setAttribute('value', 'seed')
console.log(input.value)
inferred.value = 'edited text'
console.log(input.value)
console.log(String(input.disabled))
input.disabled = true
console.log(String(input.disabled && input.hasAttribute('disabled')))
input.setAttribute('disabled', 'false')
console.log(String(input.disabled))
input.disabled = false
console.log(String(input.disabled || input.hasAttribute('disabled')))
input.value = ''
console.log(input.value)

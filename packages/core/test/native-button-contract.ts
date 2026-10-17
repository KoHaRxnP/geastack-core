import type {ButtonElement} from '@geastack/core'
const inferred = document.createElement('button')
const button: ButtonElement = inferred
button.setAttribute('id', 'typed-button-contract')
document.body.appendChild(button)
console.log(String(inferred.disabled))
inferred.disabled = true
console.log(String(inferred.disabled && button.hasAttribute('disabled')))
button.setAttribute('disabled', 'false')
console.log(String(button.disabled))
button.disabled = false
console.log(String(button.disabled || inferred.hasAttribute('disabled')))
inferred.value = 'saved-button-value'
console.log(button.value)
console.log(button.getAttribute('value'))

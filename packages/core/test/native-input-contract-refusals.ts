import type {InputElement} from '@geastack/core'
const input: InputElement = document.createElement('input')
// No invented control-kind, checked, selection or picker contracts.
// @ts-expect-error InputElement has no checked authority
input.checked
// @ts-expect-error InputElement has no input-kind authority
input.type
// @ts-expect-error InputElement has no native selection API
input.select()
// @ts-expect-error InputElement has no native selection position API
input.selectionStart

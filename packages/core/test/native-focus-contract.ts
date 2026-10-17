import type {Element} from '@geastack/core'
const active: Element | null = document.activeElement
if(active !== null){console.log(active.getAttribute('id'));active.focus({preventScroll:true})}
console.log('typed actual active element getter')

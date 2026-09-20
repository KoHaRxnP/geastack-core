import type { TextProps } from '@geastack/core'

export function Text({ class: cls, style, children }: TextProps) {
  return (
    <span class={cls} style={style}>
      {children}
    </span>
  )
}

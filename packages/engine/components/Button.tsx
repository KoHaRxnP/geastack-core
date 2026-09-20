import type { ClassValue, PressHandler, Style } from '@geastack/core'

export function Button({
  class: cls,
  style,
  pressId,
  pressValue,
  onPress,
  onClick,
  children
}: {
  class?: ClassValue
  style?: Style
  pressId?: number
  pressValue?: number
  onPress?: PressHandler
  onClick?: PressHandler
  children?: any
}) {
  return (
    <button
      class={cls}
      style={style}
      pressId={pressId}
      pressValue={pressValue}
      onPress={onPress}
      onClick={onClick}
    >
      {children}
    </button>
  )
}

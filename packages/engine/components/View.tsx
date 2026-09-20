import type { ViewProps } from '@geastack/core'

export function View({
  class: cls,
  style,
  pressId,
  pressValue,
  onPress,
  onClick,
  children
}: ViewProps) {
  return (
    <div
      class={cls}
      style={style}
      pressId={pressId}
      pressValue={pressValue}
      onPress={onPress}
      onClick={onClick}
    >
      {children}
    </div>
  )
}

import type { ImageProps } from '@geastack/core'

export function Image({ class: className, src, style, fit, playing, loop, onLoad, onError, onFrame }: ImageProps) {
  return (
    <img
      class={className}
      src={src}
      style={style}
      fit={fit}
      playing={playing}
      loop={loop}
      onLoad={onLoad}
      onError={onError}
      onFrame={onFrame}
    />
  )
}

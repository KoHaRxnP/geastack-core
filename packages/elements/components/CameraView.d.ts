import { Component } from '@geastack/core'
import type { CameraController, CameraFacing, Style } from '@geastack/core'

export interface CameraViewProps {
  facing?: CameraFacing
  mirror?: boolean
  device?: string
  width?: number
  height?: number
  fit?: 'cover' | 'contain' | 'fill'
  style?: Style
  onReady?: (cam: CameraController) => void
  onError?: (message: string) => void
}

export declare class CameraView extends Component {
  constructor(props: CameraViewProps)
  template(): any
  onAfterRender(): void
  close(): void
}

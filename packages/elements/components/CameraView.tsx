import { Camera, Component } from '@geastack/core'
import type { CameraFacing, CameraController, Style } from '@geastack/core'

export interface CameraViewProps {
  facing?: CameraFacing
  /** Mirror the preview horizontally. Defaults to `true` when facing="front". */
  mirror?: boolean
  /** Optional fixed device id from Camera.getDevices(). Overrides `facing`. */
  device?: string
  /** Capture resolution hint. Closest supported size is selected. */
  width?: number
  height?: number
  /** How the frame fills the element box. Defaults to "cover". */
  fit?: 'cover' | 'contain' | 'fill'
  style?: Style
  /** Called once the camera has opened and is producing frames. */
  onReady?: (cam: CameraController) => void
  /** Called if the camera failed to open. */
  onError?: (message: string) => void
}

/**
 * Declarative preview surface. A thin wrapper over the native `<camera>`
 * element: it sizes/positions the preview with CSS (via `style`) and streams
 * automatically — no manual RAF/`Camera.draw` loop. For captures and controls
 * use the imperative `Camera` class directly.
 */
export class CameraView extends Component {
  private props: CameraViewProps
  private notified = false

  constructor(props: CameraViewProps) {
    super()
    this.props = props
  }

  template() {
    return (
      <camera
        facing={this.props.facing ?? 'back'}
        device={this.props.device ?? ''}
        width={this.props.width ?? 0}
        height={this.props.height ?? 0}
        fit={this.props.fit ?? 'cover'}
        style={this.props.style}
      />
    )
  }

  onAfterRender() {
    if (this.notified) return
    this.notified = true
    if (!Camera.isAvailable()) {
      this.props.onError?.('camera unavailable')
      return
    }
    if (this.props.mirror !== undefined) Camera.setMirror(this.props.mirror)
    this.props.onReady?.(Camera)
  }

  /** Stop the preview and release the camera. */
  close() {
    Camera.close()
  }
}

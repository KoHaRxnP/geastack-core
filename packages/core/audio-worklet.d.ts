import type { AudioContext, AudioDestinationNode } from './index'
import type { MessagePort } from './workers'

export interface AudioContextOptions {
  sampleRate?: number
  latencyHint?: 'interactive' | 'balanced' | 'playback' | number
}

export interface AudioWorklet {
  addModule(moduleURL: string | URL): Promise<void>
}

export interface AudioWorkletNodeOptions {
  numberOfInputs?: number
  numberOfOutputs?: number
  channelCount?: number
  outputChannelCount?: number[]
}

export interface AudioWorkletNode {
  readonly port: MessagePort
  onprocessorerror: ((event: { readonly message: string }) => void) | null
  connect(destination: AudioDestinationNode): AudioDestinationNode
  disconnect(): void
}

export interface AudioWorkletNodeConstructor {
  new (context: AudioContext, name: string, options?: AudioWorkletNodeOptions): AudioWorkletNode
}

export interface MediaStreamAudioSourceNode {
  connect(destination: AudioWorkletNode): AudioWorkletNode
  disconnect(): void
}

declare global {
  const AudioWorkletNode: AudioWorkletNodeConstructor
  abstract class AudioWorkletProcessor {
    readonly port: MessagePort
    constructor()
    abstract process(inputs: Float32Array[][], outputs: Float32Array[][],
      parameters: Record<string, Float32Array>): boolean
  }
  function registerProcessor(name: string, processorCtor: new () => AudioWorkletProcessor): void
  /** AudioWorkletGlobalScope clock, in the context's sample-rate domain. */
  const sampleRate: number
  const currentFrame: number
  const currentTime: number
}

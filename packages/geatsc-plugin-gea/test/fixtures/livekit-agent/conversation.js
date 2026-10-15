// A voice-agent conversation as an app writes it: LiveKit stays an ordinary
// npm dependency, with no signaling server or LiveKit-specific native entry
// point between the app and the room. Its audio, RTC and WebSocket needs come
// from the SDK's own source, which is what the analysis must follow.
import { Room, RoomEvent, Track, createLocalAudioTrack } from 'livekit-client'

export class Conversation {
  constructor(onChange) {
    this.onChange = onChange
    this.room = new Room({ adaptiveStream: false, dynacast: false })
    this.microphone = null
    this.room.on(RoomEvent.TrackSubscribed, (track) => {
      if (track.kind === Track.Kind.Audio) track.attach()
    })
  }

  async join(serverUrl, token) {
    await this.room.connect(serverUrl, token)
    this.microphone = await createLocalAudioTrack()
    await this.room.localParticipant.publishTrack(this.microphone)
    this.onChange('listening')
  }

  async leave() {
    this.microphone?.stop()
    await this.room.disconnect()
    this.onChange('idle')
  }
}

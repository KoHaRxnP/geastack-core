import { Store } from './compiler'
import { BLE } from './host'

const bleServers: BLEServer[] = []
let initializedServerCount = 0

export class BLEServer extends Store {
  deviceName: string
  appearance: number
  macAddress: string

  constructor(deviceName: string, appearance: number, macAddress: string) {
    super()
    this.deviceName = deviceName
    this.appearance = appearance
    this.macAddress = macAddress
  }

  onConnected(): void {}
  onDisconnected(): void {}
  onBound(): void {}

  startAdvertising(): void {
    BLE.startAdvertising()
  }

  stopAdvertising(): void {
    BLE.stopAdvertising()
  }
}

// Registration must happen after the complete derived instance exists. Doing
// this from BLEServer's base constructor snapshots only the base subobject in
// native builds, losing derived configuration and virtual callbacks.
export function registerBleServer(server: BLEServer): void {
  // Embedded builds register the complete derived instance through the
  // BLEServer class-factory hook. Keep this function as a source-compatible
  // API without registering the base-class view a second time.
  void server
}

export function initBleServers(): void {
  while (initializedServerCount < bleServers.length) {
    const server = bleServers[initializedServerCount]
    initializedServerCount++
    BLE.init(server.deviceName, server.appearance, server.macAddress)
    BLE.startAdvertising()
  }
}

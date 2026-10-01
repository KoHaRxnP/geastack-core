/** Standard binary base64 APIs, with the default base64 alphabet and padding. */
declare global {
  interface Uint8Array<TArrayBuffer extends ArrayBufferLike = ArrayBufferLike> {
    toBase64(): string
  }

  interface Uint8ArrayConstructor {
    /** Decode using the standard default (loose) final-chunk handling. */
    fromBase64(string: string): Uint8Array<ArrayBuffer>
  }
}

export {}

# @geastack/engine

The GeaStack rendering engine: the intrinsic JSX primitives an app builds its UI
from, and the C++ that lays them out, styles them and paints them on a native
target. It covers the document tree and layout, style resolution and CSS
animation, text and image rendering, canvas 2D, scrolling, the virtual keyboard
and input routing.

```sh
npm install @geastack/engine
```

```tsx
import { View, Text, Image, Button } from '@geastack/engine'

export default () => (
  <View style={{ padding: 16 }}>
    <Text>Hello</Text>
  </View>
)
```

The component prop types live in `@geastack/core`. The C++ sources are
compiled into a target through `@geastack/core`'s source manifest. Vendored
third-party code is listed in the repository's `THIRD_PARTY.md`.

## License

Apache-2.0. See `LICENSE`.

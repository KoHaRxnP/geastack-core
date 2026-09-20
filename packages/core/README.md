# @geastack/core

The app-facing GeaStack API: the type declarations for Gea JSX elements,
styles and events, the TypeScript runtime helpers, and the C++ framework
runtime that every native target compiles in.

```sh
npm install @geastack/core
```

## How it is used

Apps import the types and the runtime modules:

```tsx
import type { Style } from '@geastack/core'
import { View, Text } from '@geastack/engine'
```

Everything else in the package is consumed by tooling, not by app code. The
`gea` CLI and each target's build script resolve `@geastack/core` from the
app's `node_modules`, read its source manifest to learn which framework sources
and include roots to compile, and run its build driver, which bundles the app
with Vite, compiles it with `geatsc` and the Gea plugin, and hands the emitted
C++ to the target.

## Related packages

- `@geastack/engine` renders the element tree, `@geastack/host` supplies the
  platform services, `@geastack/elements` adds the non-intrinsic elements.
- `@geastack/geatsc-plugin-gea` is the `geatsc` backend the build driver loads.
- `@geastack/compiler` is `geatsc` itself.

## License

Apache-2.0. See `LICENSE`.

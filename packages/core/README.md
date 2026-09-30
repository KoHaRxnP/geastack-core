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

## Component startup

Use `@geastack/core` for applications targeting the native GeaStack hosts.
`Component` is the component base; `ReactiveComponent` makes its own fields
reactive, and `Store` holds shared reactive state. The web target supplies the
matching browser runtime. Direct use of `@geajs/core` belongs to the Gea web
framework and is not the native application contract.

Start loading data or drawing in `onAfterRender()`. The component's `el` is set
before this hook runs. Field initializers and constructors initialize state;
code that needs a rendered canvas belongs in the hook.

```tsx
import { ReactiveComponent } from '@geastack/core'

class Counter extends ReactiveComponent {
  count = 0
  onAfterRender(): void {
    this.count = 1
  }
  template(): JSX.Element {
    return <div>{this.count}</div>
  }
}
```

Compiler/runtime regressions should use generic language or framework cases.
Report compiler issues in `geastack/compiler`, host API and application-runtime
issues in `geastack/core`, Apple rendering/bindings in `geastack/apple`, and web
target integration in `geastack/simulator`. Contributions should include a
reproduction and target-specific validation. Lifecycle and representation
changes need to preserve the shared component and native-type contracts.

## Related packages

- `@geastack/engine` renders the element tree, `@geastack/host` supplies the
  platform services, `@geastack/elements` adds the non-intrinsic elements.
- `@geastack/geatsc-plugin-gea` is the `geatsc` backend the build driver loads.
- `@geastack/compiler` is `geatsc` itself.

## License

Apache-2.0. See `LICENSE`.

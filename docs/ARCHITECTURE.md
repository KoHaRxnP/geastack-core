# Core Architecture

The core repo is the framework center of GeaStack. It keeps the public app API,
reactivity internals, and app runtime shell in `@geastack/core`, while heavier
native implementation layers remain split into host, engine, and elements.

## Layer Map

| Layer | Current path | Responsibility |
| --- | --- | --- |
| Public app API + runtime | `packages/core` | Type declarations, app manifest tools, asset/font helpers, compatibility scripts, reactivity headers, app entry, frame loop, events, and app lifecycle wiring. |
| Compiler plugin | `packages/geatsc-plugin-gea` | Gea-specific code generation and host/runtime integration for `geatsc`. |
| Host | `packages/host` | Platform services such as input, audio, network, geolocation, memory, Bluetooth, and WiFi abstractions. |
| Engine | `packages/engine` | Tree, layout, rendering, canvas, image, font, touch, and intrinsic node behavior. |
| Elements | `packages/elements` | Non-intrinsic JSX elements and component library extraction target. |
| Chips | `chips` | Target-independent chip descriptions and reusable driver cores. |

The split is about ownership and source provenance. It is not a dynamic-linking
split. Targets can still compile sources into one static native program.

## Dependency Direction

The intended direction is:

```text
apps
  -> core
    -> elements, engine, host
      -> compiler runtime surface
```

Targets implement host services. The compiler plugin may emit code that knows
about engine/reactive ABI details, but the generic compiler should not import
Gea UI/device code directly.

## Public Surface

`packages/core` remains the compatibility surface. It contains:

- `index.d.ts`, `jsx.d.ts`, and `css.d.ts`;
- component exports;
- runtime type declarations, reactive headers, and app lifecycle/frame-loop sources;
- app manifest and asset tooling;
- the `gea-embedded`/`geaos` compatibility binary.

Avoid breaking imports from examples unless the examples repo is updated in the
same change.

## Source Fusion

Native targets build by collecting C++ sources from multiple package roots and
compiling them into target firmware or native apps. The important invariant is
that generic compiler runtime sources are included once and framework sources
are included through the target's selected package/source manifest.

`gea_sources.sh` is the current shared source-list helper. Keep it in sync with
package moves until a more formal package resolver replaces it.

## Chip And Target Boundary

Generic chip code belongs under `chips/<category>/<chip>`. Target-specific bus
or RTOS glue belongs in the target repo, such as `targets`.

Good examples:

- register maps and protocol helpers in `chips`;
- ESP-IDF I2C/I2S/GPIO adapters in `targets`;
- AppKit/UIKit renderers in `apple`;
- web display/runtime shims in `simulator`.

## Migration Guidance

When moving code between core packages:

1. Identify whether the code is public API, codegen, host service, engine
   machinery, element behavior, app lifecycle, or target glue.
2. Move tests or add focused tests for the new owner.
3. Update package metadata and source manifests.
4. Update README/docs links in the owning repo.
5. Verify at least one representative app build path.

# Core Development Guide

This guide covers day-to-day framework work in the core repo.

## Package Commands

For the public core package:

```sh
cd packages/core
npm install
node test/test_gea_app_manifest.mjs
./test/run-tests.sh
```

For the Gea compiler plugin:

```sh
cd packages/geatsc-plugin-gea
npm install
npm run build
npm test
```

The scaffold packages do not all have build scripts yet. Treat their
`package.json` files as ownership markers until implementation is moved in.

## Test Strategy

Use focused tests whenever possible:

- app manifest and launcher behavior: `packages/core/test/test_gea_*.mjs`;
- native pipeline behavior: `packages/core/test/run-gea-*-pipeline.sh`;
- rendering or input behavior: source-specific shell tests under
  `packages/core/test`;
- plugin code generation: tests under `packages/geatsc-plugin-gea/test`.

Run broader shell tests before landing changes that affect shared runtime,
layout, event, rendering, or generated app behavior.

## Generated Artifacts

Generated build folders such as `.build-test`, generated app output, and native
build products are disposable. Do not rely on generated output as source unless
a test fixture explicitly owns it.

## App Manifest Tools

The app manifest and launcher catalog helpers live under
`packages/core/tools`. Keep their behavior aligned with the examples repo and
with `@geastack/cli` manifest validation.

## Change Checklist

Before landing core changes:

1. Identify the owning layer from [ARCHITECTURE.md](ARCHITECTURE.md).
2. Keep imports and package metadata aligned with that layer.
3. Run the focused tests for the touched behavior.
4. Run at least one representative app pipeline if runtime/rendering changed.
5. Update docs when package boundaries, commands, or target expectations change.

## Common Pitfalls

- Do not add target-specific ESP-IDF, AppKit/UIKit, GeaOS, or web behavior to a
  generic framework package.
- Do not add Gea UI or device assumptions to the generic compiler runtime.
- Do not change a package boundary without updating source manifests used by
  targets.
- Do not break existing example imports without updating examples in the same
  work.

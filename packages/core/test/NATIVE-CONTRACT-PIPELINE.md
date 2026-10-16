# Typed native DOM contracts

`run-native-remove-child-contract.sh` and `run-native-dom-structural-contract.sh`
use the actual shared or installed compiler CLI. Set these explicit dependencies:

```sh
export GEA_GEATSC_BIN=/absolute/path/to/compiler/dist/cli.js
export GEA_COMPILER_RUNTIME=/absolute/path/to/compiler/src/targets/cpp/runtime
export GEA_JSDOM_PACKAGE=/absolute/path/to/installed/jsdom
bash packages/core/test/run-native-remove-child-contract.sh
```

No arguments regenerates the typed fixture through the normal CLI and built Core
plugin, checks the same TypeScript source against installed JSDOM, then builds and
runs the pure Core CPU contract. Missing tools/dependencies fail explicitly.
`--prepare-only` performs generation and the browser reference without native
compilation or execution. `--build-only` compiles without executing the native
binary; if preparation is absent it first performs that same preparation.

Prepared output is reusable only while its SHA provenance matches the fixture,
project, runner, CPU source, common build helper, Core source, built plugin,
compiler output and explicit runtime source headers/implementations. The generated
source manifest and all emitted C++/headers/support includes are hashed too. Every
top-level runtime header and `gea_runtime_builtins.cpp` must match the explicit
runtime provider. These asset-free contracts refuse the optional generated font
and asset C++ inputs, and refuse engine/elements root overrides. Changed provenance refuses reuse and requires
`--prepare-only` or a normal invocation. A fallback `program.cpp` is refused.
These contracts cover CPU-only DOM behaviour, not a whole application or a GUI platform renderer.

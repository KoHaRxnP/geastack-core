# @geastack/chips

Portable native chip drivers used by GeaStack embedded targets.

Install one package:

```sh
npm install @geastack/chips
```

The package contains the complete driver catalog. A board definition selects
the controllers it uses, and the target adapter compiles only the selected
portable sources and platform bindings.

Use the Gea CLI to inspect and compose the installed catalog:

```sh
gea chips list
gea chips info co5300
gea chips add co5300 --board my-board
gea chips remove co5300 --board my-board
```

`catalog.json` is the machine-readable contract shared by the CLI and target
adapters. It records each driver's category, interfaces, native sources,
configuration fields, and available platform bindings.

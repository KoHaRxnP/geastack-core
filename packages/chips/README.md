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

Categories map to the roles of a composed board definition: `display`,
`touch`, `power`, `imu`, `audio`, `rtc` and `expander`. An `expander` is the
I/O expander a board routes its slow control lines through (a backlight
enable, a touch or panel reset); the definition names which expander pin
carries which line under `chips.expander.outputs`, and the display and touch
bindings look them up there.

A display entry is either a controller IC on a serial bus (`co5300`, `sh8601`
over QSPI) or `rgb_panel`: a bare parallel RGB (DPI) panel with no controller,
described by its timings and 16 data pins and scanned out by the MCU's own LCD
peripheral.

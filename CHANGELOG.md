# Changelog

All notable changes to this project are documented here.

The format follows [Keep a
Changelog](https://keepachangelog.com/en/1.1.0/), and this project adheres
to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [1.1.0] - 2026-11-10

### Added

- **A black wallpaper for the display** (`extras/black-960x400.png`, also
  attached to the release), and steps in the README for setting it on
  that display only, so it stays black from sign-in until the panel
  starts.
- **Program Files and other read-only folders.** If the program cannot
  write in its own folder, it keeps its settings, presets and log in
  `%APPDATA%\System Monitor Display` instead of failing to save. In a
  folder it can write to, nothing changes. `--diagnose` shows which
  folder is in use.

## [1.0.0] - 2026-11-10

The first release.

### Added

- **A full-screen green-screen sensor panel** for the Lamptron HM049
  display (960 x 400 to Windows, 960 x 320 shown): three boxes, each with
  any one HWiNFO64 reading as a number (0 to 2 decimal places) in Fixedsys
  Excelsior and a 16-block bar. By default CPU load, CPU temperature and
  GPU load. Opens as an ordinary window on a PC without a 960 x 400
  screen, moves onto the screen if it appears later, and hides while it is
  gone.
- **CRT effects worked out in linear light:** halation in the glass, beam
  spot growth towards the corners, scanlines with beam bloom, phosphor
  saturation, afterglow, a hum bar, an occasional ripple with static, a
  barrel or pincushion curve over the whole screen, vignette and a glass
  reflection. About 2 ms a frame, and about 0.1 % CPU and 35 MB of memory
  on a Ryzen 9 9950X3D.
- **Readings from HWiNFO64, free or Pro:** shared memory while it is on,
  otherwise the Gadget registry values, chosen on every read. Readings are
  smoothed, matched by label or by sensor and label, and shown as `--`
  once they stop arriving.
- **Two alarm levels a box:** a double border, and at a higher level the
  number and bar blink. Each has a clear level and a hold time.
- **A settings window** with every setting and a tooltip on each: box
  titles, readings, units, decimal places, bar ranges and alarms; phosphor
  colour (presets or any colour), curvature, glow, brightness, scanline
  height, afterglow and frame rate; hum bar and ripple; hidden rows,
  smoothing and Start with Windows; an optional header; and presets, with
  a built-in Default. Changes show at once and are saved as they are made;
  Revert and Cancel put them back.
- **Start with Windows** through a Startup-folder shortcut that follows
  the program if it is moved. Settings, presets and the log are kept
  beside the program.
- **`--diagnose`, `--selftest` and `--preview`** to see what the panel can
  read, check its logic and render a frame.

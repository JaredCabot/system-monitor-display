# Changelog

All notable changes to this project are documented here.

The format follows [Keep a
Changelog](https://keepachangelog.com/en/1.1.0/), and this project adheres
to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [1.4.0] - 2026-11-11

### Added

- **Contrast**, like a CRT's contrast knob, which sets the strength of
  the picture and leaves black alone. Turned up, the glow and the lit
  scanlines get stronger and the beam spot spreads round bright parts,
  as a CRT blooms when driven hard; turned down, the picture dims. The
  range is 0.5 to 2, and 1 looks as before.
- **Languages.** The settings window, its messages and the tray menu
  come in English, German, Spanish, French, Italian, Russian, Japanese
  and Simplified Chinese. **Language** in the General group picks one,
  or **Automatic** follows Windows. A `lang` folder beside the settings
  file can add a language or change the wording of one.
  `--check-translations` lists anything a language is missing.
- **Ripple direction and frequency.** The ripple can run up or down the
  screen, and its frequency can be set, from 10 to 3000 Hz, counting
  each row of the screen as a line of a 15.734 kHz NTSC picture: the
  higher, the closer its wobbles. 874 Hz is 18 rows from crest to
  crest, the wobble 1.3.0 had built in.
- **Ripple drift**: how fast the wobble's crests move through the
  ripple, -6 to 6 Hz, forwards above 0 and back below.
  The default, 0, moves the whole wave as one shape.
- **Hum bar and ripple thickness.** The hum bar can be 8 to 400 pixels
  tall. The ripple's band can be 10 to 300 pixels, measured where its
  wobble is at half strength; 67 is the band 1.3.0 had built in.
- **A ripple to show a change.** Changing one of the ripple's settings
  in the settings window starts a ripple at once, already in sight, or
  carries on with the one that is running. Other changes no longer
  restart the wait for the next ripple; only a change to the ripple gap
  does.
- **Degauss**, in the General group and the tray menu, for fun: for two
  seconds the picture shakes, twists and bulges as the coil's field
  dies away, patches of light drift across it, and the coil thunks and
  buzzes, quietly.
- **Blue phosphor**, a fifth preset beside Fallout, Green, Amber and
  White.
- **Scanline drift.** The scanlines can drift up or down at up to 30
  pixels per second. The default is 0, since a real CRT's scanlines
  stay still.
- **Typed values.** The number beside each slider can be typed, to any
  precision. Enter or leaving the box applies it; a value out of range
  goes to the nearer end. Here and in the boxes' own fields, a comma is
  read as the decimal mark where Windows' regional format uses one.

### Changed

- **Veiling glare.** As on a real tube, the whole background now
  brightens a little as more of the screen is lit: with the bars full
  it is slightly foggier than with them empty. The centre haze is
  turned down to half to make room, so the background is a little
  darker than before with the bars nearly empty and about as bright
  with them full.
- **New defaults.** Scanlines 4 pixels high, afterglow 0.15 seconds,
  hum bar intensity 3 and thickness 200 pixels, and a ripple 40 rows
  thick at 30 Hz and 3 % amplitude, with static 0.3. Both roll up at
  the rate of 60 Hz mains hum on a 59.94 Hz NTSC picture: a hum bar
  every 16.7 seconds, and the ripple 23.3 seconds a pass.
- **Steep ripples are smoothed.** Where the ripple's wave is steep,
  each row is blended from four thin slices of it, as a real beam spot
  would, instead of jumping as one hard step from its neighbours. A
  gentle ripple looks as before.
- **The hum bar is softer and repeats.** It fades as a raised cosine
  rather than a straight line to its edges, and a new band follows a
  screen's height behind the last, as real hum comes round once a
  field.
- **The hum bar and the ripple are timed in seconds**: each one's
  speed is now its period, the time it takes to cross the screen from
  coming into sight to gone for the ripple (`ripple_period_s`, 0.1 to
  60 seconds), and the time each hum band takes to roll the screen's
  height (`hum_bar_period_s`, 1 to 600). A period stays the same when the
  hidden rows or the thickness change. The ripple can now take up to a
  minute, for a slow roll.
- **Ripple intensity is now Ripple amplitude**, a percentage of the
  screen's width (`ripple_amplitude_pct`), 0 to 10, which is up to 94
  pixels each way. 0.2 % is about the 2 pixels of 1.3.0.
- **The hum bar intensity goes up to 10**, from 5.
- **Settings from 1.3.0 carry over.** A settings file or preset that
  has `hum_bar_speed`, `ripple_speed` or `ripple_intensity` is read in
  the new units, as the same speed and amplitude. What 1.3.0 had built
  in comes with them, in place of the new defaults: a hum bar 96 pixels
  thick, and a ripple 67 pixels thick at 874 Hz.
- **The hum bar and ripple run up by default**, as mains-borne
  interference does on an NTSC picture. Existing settings files
  keep their own values, with one exception: a file from 1.3.0 or
  earlier has no ripple direction, so its ripple now runs up. Set the
  list beside **Ripple** to **Down** to have it as it was.
- **Brightness turned down dims the characters too.** At -1 they now
  give about half their light. Before, the glow round them kept them
  near full, and only the faint parts faded. Turning it up is as before.
- **The ripple passes the whole screen.** It used to fade as it
  crossed, and came in and went out close to the edges, so it died
  away at the top and bottom. It now starts and ends off the screen and
  keeps its full strength all the way across.
- **The ripple's wave moves as one shape.** Its sine wave and the
  Gaussian across it now travel together, so the wave keeps its shape
  as it crosses the screen. Before, the crests moved about 8 times a
  second on their own, and at a short wavelength the wave jumped from
  one pattern to the next each frame rather than moving.
- **The ripple gap is one slider with two handles**, shortest and
  longest, with a box on either side to type them, in place
  of two sliders. It makes room for the new ripple settings. The handles push
  each other rather than cross, as the two sliders did, and the arrow
  keys move the one in focus, with Space to change which.
- **The settings window fits smaller screens.** It was taller than a
  1080p screen has room for at 150 % scaling, and with this version's
  new rows at 125 % too, which left OK and Cancel out of reach. On a
  screen too small for it, it is now drawn smaller, down to three
  quarters of its usual size, and it opens in the middle of the screen.
- **The settings window's labels have more room**: the label column grew
  from 128 to 150 pixels so the longer languages fit, and the sliders
  are narrower to match.

### Fixed

- **Pause animation left out a ripple that was running.** The ripple
  vanished and ran on unseen. It now stays where it is, its static held
  still, and goes on from there when the animation resumes.
- **The ripple showed the still picture underneath.** The afterglow
  was worked out after the ripple had moved the picture, so the text's
  unmoved place kept glowing and mostly hid the wobble. The afterglow
  now follows the picture as the ripple moves it, as on a real CRT,
  whose phosphor fades too fast to show a wobble's old place.
- **Output sent to a file.** `--diagnose > out.txt` and the like now
  write to the file. Before, the output went to the console regardless.
- **A header with degree signs was cut short.** A header line is 52
  characters, but it was cut at 52 bytes, and ° takes two. The header's
  boxes in the settings window now stop at 52 characters too.

## [1.3.0] - 2026-11-10

### Changed

- **The preset in use is shown.** When the settings window opens, the
  Presets box shows the preset that matches the current Look and
  Animation settings, or stays empty if none does. Revert and Delete
  update it too.

## [1.2.0] - 2026-11-10

### Added

- **Where the files are.** The settings window's General group shows the
  folder the settings, presets and log are kept in, beside the program or
  in `%APPDATA%\System Monitor Display`. Clicking it opens the folder in
  Explorer.

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

# Settings

Hold Select + Start briefly, or tap the top-right front-touch corner.
Up/Down selects, Left/Right changes, L/R switches page, and Circle or Start
closes and saves. Unavailable options are greyed with a reason; options
marked for restart need an app restart.

Choices are in `ux0:data/infected/settings.txt`. Back it up before changes.
Do not delete it as an update step. A `config.txt` override can lock a value
for that run without changing the saved setting.

## Current Vita Defaults

The recovered `vita/app/src/app_settings.cpp` sets **Split renderer**, a
**30 FPS cap** and a **500 MHz request**. The cap is not a performance promise.
500 MHz requires an already configured clock-unlock plugin; the app falls
back when the system refuses it. Installing a plugin is not a required step.

There is a one-time migration from the older saved 444 MHz setting to 500,
recorded by `cpu_clock_500.done`. A later deliberate 444 selection is kept.
Check the clock option after updating; preserve that marker with your data
backup. This behavior is documented, not changed by the source import.

## Options

| Page | Options |
| --- | --- |
| Graphics | HD textures (Automatic/Off/2x/4x), texture filter, mipmaps, anti-aliasing, pixel lighting, effects resolution, movie scaling, HD movies |
| Performance | Frame limit, automatic frame skip, FPS display, 960x544/720x408 resolution, renderer mode, CPU clock, enemy density, view distance |
| Controls | Modern/Classic layout and hold-to-fire |

Automatic HD mode prefers a compatible 2x pack, otherwise 4x. Use the mixed
Vita pack for 4x, not the full PC pack. HD movies remain experimental. Start
with both HD options off when diagnosing a graphics or memory problem.

Higher resolution, anti-aliasing, pixel lighting and increased game density
can cost performance. Existing cost labels are estimates, not current
device benchmarks. Frame skip only helps the split renderer; it does not
turn skipped frames into rendered frames.

[Controls](../CONTROLS.md) explains the button mapping.
[HD Textures](../HD-TEXTURES.md) explains pack preparation and filenames.
For the full setting definitions, fetch the pinned PortableKit and inspect
`host/settings/port_settings.cpp`. Current hardware acceptance is pending.

# m5dial_clock_sbb

A single, lean ESPHome/LVGL widget: the classic **SBB Bahnhofsuhr** (Swiss
railway station clock, designed in 1944 by Hans Hilfiker) as an analog clock
on an M5Dial (or any other ESP32 + LVGL display). No more five-style kit, no
multi-display sync, no choreography modes - just this one clock, with real
hand kinematics, night mode, date and temperature built in.

## Why rewritten instead of forked

This repo started as an idea to fork `tuct/esphome-lvgl-clock` and trim it
down. Two reasons against that:

1. **No license in the original repo** - without a `LICENSE` file, "all
   rights reserved" applies by default; copying and modifying that code
   would not have been on solid legal ground.
2. Its own README states the component does **"continuous sweep - no
   ticking or stop-to-go pause"** - exactly the mechanism that makes a real
   SBB clock what it is was entirely missing there, and would have had to
   be built from scratch anyway.

The code here is an independent, from-scratch implementation; nothing was
carried over except the public, generic LVGL 9 drawing technique
(`lv_draw_line`, `lv_draw_rect`, `lv_draw_label` on a canvas layer) - that's
the only way *anyone* draws a clock onto an LVGL canvas, regardless of
author.

## The Stop2Go mechanism

Verified online (SBB/Mondaine's own "Stop2Go" explanation): the red second
hand sweeps once around the dial in **58 seconds**, then **pauses for 2
seconds at 12** - waiting for the next minute's electrical impulse - and in
that exact instant the minute hand jumps forward one step. The hour hand, by
contrast, keeps creeping mechanically/continuously.

This component reproduces that:

- Second hand: a linear sweep over `second_hand_sweep` (default `58s`), then
  parked at 12 for the rest of the minute.
- Minute hand: jumps discretely, never creeps.
- Hour hand: creeps continuously along with the minutes/seconds.

`second_hand_sweep` is configurable (1-59s) in case a build wants to
exaggerate or soften the pause - the default is the researched real value.

## Features

- **Scales**: just change `width`/`height` - all geometry (hands, ticks,
  hub) is proportional to the radius. Date/temperature text stays raster-
  sized - pick a matching `date_font`/`temperature_font` yourself at very
  different sizes.
- **Night mode**: `id(clock).set_night_mode(true)` (runtime, no YAML reload
  needed) swaps foreground/background (dial, ticks, hour/minute hands,
  date/temperature text) - black/white inverted. The second hand has its own
  colour (default red, `E74C3C`) and stays unchanged in either mode, exactly
  like the real clock.
- **Date line** at the bottom of the face (`show_date: true`), formatted
  straight from `time_id` - no separate `interval:`/`lambda:` construct
  needed any more.
- **Temperature line** at the top (`show_temperature: true` +
  `temperature_sensor_id:`) - read straight from the sensor and drawn along
  with everything else.
- Only one canvas render pass per frame (dial → ticks → text → hands → hub),
  so **`transparent: true` is no longer needed** just to get text under the
  hands (as the old `clock.yaml` required) - that halves the default canvas
  RAM footprint (RGB565 instead of ARGB8888), unless you actually want a
  background to show through.
- **The tick ring is drawn once, not every frame.** No hand, hub, or text
  ever reaches out to where the 60 tick marks live, so redrawing all of them
  every `render_interval` was pure waste - it's now only (re)drawn on the
  first frame and after a `night_mode` flip; every other frame only erases
  and redraws the small bounding box the hands/hub/date/temperature text
  actually touch that frame (see "Fixed" below for how tight that box is -
  it used to be a fixed inner disc, now it's a real per-frame dirty rect).
  On real hardware this is the difference between the component finishing
  well within its render budget and it tripping ESPHome's "took a long
  time" watchdog. **`transparent: true` gets this too as long as
  `show_face: true` is also set** (fixed 2026-09-06 - see "Fixed" below):
  a face fully covers everything that partial erase ever touches, so
  there's no transparent pixel left for that erase to get wrong. Only
  `transparent: true` with `show_face: false` (ticks/hands floating
  directly over a transparent background, no face circle) still needs
  the full redraw every frame, since a plain rect can't "erase to
  transparent" the way a full canvas clear can.

## Fixed

- **2026-09-06 - uneven/"hopping" second hand at large sizes.** Reported
  on a 450x450, `transparent: true` + `show_face: true` face (`smart-ebl-
  display.yaml`'s Mainscreen clock). Root cause: `transparent_` alone
  used to force the expensive full-canvas-plus-60-ticks redraw on
  *every* `render_interval` tick, not just the first frame/a `night_mode`
  flip - and at that size the canvas is ARGB8888 in PSRAM (see
  `alloc_canvas_buf()`'s own fallback warning), where a full repaint that
  often can take longer than `render_interval` itself. Since the hand's
  angle is always computed from the real wall clock rather than
  accumulated per frame, a delayed/irregular render doesn't drift - it
  *jumps* to catch up the next time it actually runs, which is exactly
  what "hopping" looks like. Fixed: `transparent_` only forces the full
  path now when `show_face_` is off (see the bullet above) - a build
  using `show_face: true` (the only combination any of this project's
  own configs actually use) gets the same cheap steady-state redraw an
  opaque canvas already had. No config changes needed to pick this up -
  it's a behavior fix in the component itself, `external_components:`
  with `ref: main` picks it up on the next fetch.

- **2026-09-06 (round 2) - hopping persisted even with the fix above,
  on real hardware.** The bullet above only removed the "redraw the
  tick ring every frame" cost - the canvas *itself* was still ARGB8888
  in PSRAM (`transparent: true`'s only way to get a corner-punched-out
  round face used to be a real alpha channel), and even the cheap
  inner-disc-only erase/redraw the fix above enabled is still slower on
  a PSRAM buffer than the render budget really wants at this canvas
  size (measured on real ESP32-P4 hardware: `sbb_clock` still logged
  ESPHome's "took a long time" component warning periodically). Added
  `corner_color` (see the config table above) so a build on a fixed
  solid-color page (the normal case) doesn't need `transparent: true` -
  and therefore doesn't need an alpha channel at all - to make the
  square canvas's corners disappear into the page behind it: just paint
  them the same fixed color as that page, opaquely, same as the face
  itself. That drops a 450x450 canvas from ARGB8888 (810112 bytes -
  doesn't fit in this chip's ~432KB of free internal RAM, hence PSRAM)
  to the same native color format as everything else (405000 bytes -
  fits comfortably), eliminating the PSRAM cost at its root rather than
  just shrinking how often it's paid. Requires a config change to adopt
  (`transparent: true` → `corner_color: <a Color id matching your page's
  real background>`) - see `smart-ebl-display.yaml` in
  `smartebl_display_esphome` for the update this shipped alongside.

- **2026-09-06 (round 3) - fixed-size inner-disc erase replaced with a
  real per-frame dirty rect.** Round 1 above stopped redrawing the 60
  tick marks every frame, but the "cheap" path it introduced still
  erased and redrew a *fixed* disc (radius ~0.75 × the widget's radius)
  on every `render_interval` tick, regardless of how little of it the
  hands/hub/text actually touched that frame. That disc's area grows
  with the *square* of the widget's radius, so it's cheap at the
  240×240 M5Dial size this repo targets but expensive at a much larger
  size - reported when scaling this widget up for a 10.1" display in
  another project, where the widget's radius (and therefore the old
  disc's area) is 20-30× larger than on the M5Dial. The component now
  computes the exact bounding box of what each frame actually draws
  (hour/minute/second hand, hub, date/temperature text), unions it with
  what the previous frame drew, and erases/invalidates only that -
  typically on the order of a tenth of the old fixed disc's area, since
  the hands only ever point in one direction each rather than the "any
  direction" worst case the old disc had to cover unconditionally. Also
  switched from invalidating the whole widget every frame
  (`lv_obj_invalidate`) to invalidating just that same small box
  (`lv_obj_invalidate_area`), which matters most on a large panel: it
  keeps the amount of pixel data LVGL flushes to the physical display
  each frame proportional to what actually changed, not the widget's
  full size. No config changes needed - this is a behavior fix in the
  component itself, and applies at every canvas size, including the
  240×240 M5Dial target (a smaller win there, but not a zero one: less
  raster work and a smaller display flush per frame either way).

## Usage

```yaml
external_components:
  - source:
      type: git
      url: https://github.com/CzarofAK/m5dial_clock_sbb
      ref: main
    components: [sbb_clock]

sbb_clock:  # marker key - without it ESPHome won't load the component

color:
  - id: col_ink
    hex: FFFFFF
  - id: col_bg
    hex: "000000"
  - id: col_second
    hex: E74C3C

lvgl:
  widgets:
    - sbb_clock:
        id: clock_face
        time_id: homeassistant_time
        width: 240
        height: 240
        align: CENTER
        foreground: col_ink
        background: col_bg
        second_hand_color: col_second
        show_date: true
        show_temperature: true
        temperature_sensor_id: s_aussentemp
```

Toggling night mode from an automation:

```yaml
sun:
  # ...

time:
  - platform: homeassistant
    id: homeassistant_time
    on_time:
      - seconds: 0
        minutes: 0
        then:
          - lambda: |-
              id(clock_face).set_night_mode(id(my_sun).is_below_horizon());
```

(or bound to a lux sensor, a `binary_sensor`, whatever the given application
defines as "night" - the component only knows the state, not the rule
behind it.)

## Configuration reference

| Key | Type | Default | Meaning |
|---|---|---|---|
| `width`, `height` | int | *(required)* | canvas size in px |
| `time_id` | id | *(required)* | a `time:` element |
| `show_seconds` | bool | `true` | draw the second hand |
| `second_hand_sweep` | time | `58s` | sweep duration; the rest of 60s is the pause at 12 |
| `render_interval` | time | `100ms` | redraw cadence |
| `foreground` | Color id | white | the "ink": hands, ticks, text |
| `background` | Color id | black | canvas background |
| `second_hand_color` | Color id | `E74C3C` | independent of night mode |
| `transparent` | bool | `false` | ARGB8888 canvas instead of RGB565 |
| `show_ticks` | bool | `true` | minute/hour ticks |
| `show_face` | bool | `false` | filled dial circle |
| `face_color` | Color id | `background` | only with `show_face` |
| `corner_color` | Color id | `background` | the square canvas's own corners (outside the round face) - NOT swapped by `night_mode`, unlike `background`. Set this to your page's real background instead of using `transparent: true` when it's a fixed solid color - see "Fixed" below |
| `night_mode` | bool | `false` | initial value; toggle at runtime via `set_night_mode()` |
| `show_date` | bool | `false` | date line at the bottom |
| `date_font` | font | `montserrat_16` | |
| `show_temperature` | bool | `false` | temperature line at the top |
| `temperature_sensor_id` | id | - | required as soon as `show_temperature: true` |
| `temperature_font` | font | `montserrat_16` | |

## Verification status

- Stop2Go mechanism: checked online against the official SBB/Mondaine
  explanation (58s sweep + 2s pause, minute jump during the pause).
- `ESPTime::day_of_week` (Sunday = 1): verified directly from ESPHome's own
  `esphome/core/time.h`.
- Every LVGL function/struct used (`lv_draw_line_dsc_t`, `lv_draw_rect_dsc_t`,
  `lv_draw_label_dsc_t`, `lv_canvas_*`, `lv_draw_buf_*`, `lv_obj_get_coords`,
  `lv_obj_invalidate_area`) checked against the real LVGL 9.5.0 source (the
  version ESPHome pins).
- The Python config side (`components/sbb_clock/__init__.py`) was
  successfully validated with `esphome config` against a real test
  configuration.
- **Not possible in that session:** a full `esphome compile` - that pulls
  packages from PlatformIO's registry, which was blocked by that session's
  network policy. Run `esphome compile` locally once before flashing real
  hardware. This applies to the round-3 dirty-rect change (2026-09-06)
  above too - it's reviewed against the LVGL 9.5.0 API and cross-checked
  against every other call site in this file for matching geometry, but
  not yet built or run on real hardware.

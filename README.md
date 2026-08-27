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
  `lv_draw_label_dsc_t`, `lv_canvas_*`, `lv_draw_buf_*`) checked against the
  real LVGL 9.5.0 source (the version ESPHome pins).
- The Python config side (`components/sbb_clock/__init__.py`) was
  successfully validated with `esphome config` against a real test
  configuration.
- **Not possible in that session:** a full `esphome compile` - that pulls
  packages from PlatformIO's registry, which was blocked by that session's
  network policy. Run `esphome compile` locally once before flashing real
  hardware.

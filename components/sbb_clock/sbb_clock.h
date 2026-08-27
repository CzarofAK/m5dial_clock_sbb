#pragma once

#include "esphome/core/component.h"
#include "esphome/core/color.h"
#include "esphome/components/time/real_time_clock.h"
#include "esphome/components/lvgl/lvgl_esphome.h"
#ifdef USE_SENSOR
#include "esphome/components/sensor/sensor.h"
#endif

#include <cstdint>
#include <string>

namespace esphome {
namespace sbb_clock {

// Allocates the widget's canvas buffer, preferring internal SRAM.
//
// ESPHome points every LVGL allocation at PSRAM first (lv_malloc_core() uses
// MALLOC_CAP_SPIRAM), which is right for big static buffers but wrong here:
// this canvas is redrawn pixel by pixel every render_interval, so PSRAM's
// write latency would dominate the frame time. Falls back to PSRAM only when
// the canvas doesn't fit internally.
void *alloc_canvas_buf(size_t size);

// One SBB "Bahnhofsuhr" face on an LVGL 9 canvas: baton hour/minute hands, a
// lollipop second hand, and the real mechanism's "Stop2Go" kinematics - the
// second hand sweeps the dial and then parks at 12 for the rest of the
// minute while the minute hand jumps (see set_second_hand_sweep_s()).
//
// It's a native `lvgl:` widget: add it under `lvgl: widgets: - sbb_clock:
// ...`, like `canvas` or `line`. It owns its canvas and redraws itself from
// loop() - no `interval:` + lambda glue needed, including for the optional
// date and temperature lines.
class SbbClock : public Component, public lvgl::LvCompound {
 public:
  void setup() override;
  void loop() override;
  void dump_config() override;
  // After LVGL's own core setup, same as other LVGL-driven components -
  // there is nothing to warm up before the first frame.
  float get_setup_priority() const override { return setup_priority::PROCESSOR; }

  // ---- wiring, set once at codegen time -----------------------------------
  void set_time(time::RealTimeClock *t) { this->time_ = t; }
  // Canvas size, known from config at codegen time - render_() uses this
  // rather than lv_obj_get_width/height(this->obj), which isn't reliably
  // resolved yet the first few times loop() runs (LVGL only computes actual
  // layout during its own refresh pass).
  void set_canvas_size(int w, int h) {
    this->canvas_w_ = w;
    this->canvas_h_ = h;
  }
  void set_transparent(bool t) { this->transparent_ = t; }
  void set_render_interval(uint32_t ms) { this->render_interval_ms_ = ms; }
  void set_show_seconds(bool show) { this->show_seconds_ = show; }
  // How many of the 60 seconds the second hand takes to sweep once round the
  // dial; the rest (60 - this) is the pause at 12, waiting for the next
  // minute. 0 is coerced to the verified default (58s sweep + 2s pause).
  void set_second_hand_sweep_s(uint32_t s) { this->second_sweep_s_ = s > 0 ? s : 58; }
  void set_show_ticks(bool on) { this->show_ticks_ = on; }
  void set_show_face(bool on) { this->show_face_ = on; }
  void set_show_date(bool on) { this->show_date_ = on; }
  void set_date_font(const lv_font_t *f) { this->date_font_ = f; }
  void set_show_temperature(bool on) { this->show_temperature_ = on; }
  void set_temperature_font(const lv_font_t *f) { this->temperature_font_ = f; }
#ifdef USE_SENSOR
  void set_temperature_sensor(sensor::Sensor *s) { this->temperature_sensor_ = s; }
#endif
  void set_foreground(Color c) { this->ink_ = c; }
  void set_background(Color c) { this->paper_ = c; }
  void set_second_hand_color(Color c) {
    this->second_ = c;
    this->has_second_color_ = true;
  }
  void set_face_color(Color c) {
    this->face_ = c;
    this->has_face_color_ = true;
  }

  // ---- runtime control ------------------------------------------------------
  // Swaps foreground/background (dial, ticks, hour+minute hands, date/temp
  // text) for a black/white-inverted night look. The second hand ignores
  // this entirely - it always draws in second_color_(), night or day - so a
  // build that leaves it on its default red keeps a red second hand in both
  // modes, exactly like the real clock's always-red "Stop2Go" hand.
  //
  // Plain runtime state, not config: call it from a `lambda:` in an
  // automation (e.g. bound to sun.is_below_horizon or a lux sensor) -
  // `id(my_clock).set_night_mode(true);`.
  void set_night_mode(bool on) { this->night_mode_ = on; }
  bool get_night_mode() const { return this->night_mode_; }

 protected:
  // Colours as configured; night mode is read through these instead of
  // overwriting ink_/paper_, so toggling it and back is always lossless.
  Color ink_now_() const { return this->night_mode_ ? this->paper_ : this->ink_; }
  Color paper_now_() const { return this->night_mode_ ? this->ink_ : this->paper_; }
  Color second_color_() const {
    return this->has_second_color_ ? this->second_ : Color(0xE7, 0x4C, 0x3C);
  }
  Color face_color_() const { return this->has_face_color_ ? this->face_ : this->paper_now_(); }

  // Reads the wall clock once and fills every field render_() needs from
  // it; false while time isn't valid yet. Even then it fills hh/mm/ss with a
  // fixed 00:15:00-ish pose (hands not stacked) plus uptime-driven seconds,
  // so the face looks alive before the first sync instead of sitting on a
  // frozen 00:00:00 - day_of_week/day_of_month/month/year are left at 0 in
  // that case, which format_date_() takes as its cue to print placeholders.
  bool now_or_fallback_(int &hh, int &mm, int &ss, uint8_t &day_of_week, uint8_t &day_of_month,
                        uint8_t &month, uint16_t &year) const;
  // Fraction (0..1) of the way through the current second, from millis() -
  // lets the second hand (and the hour hand's small creep) move smoothly
  // between whole-second ticks instead of jumping.
  float sub_second_(int ss);
  // Maps elapsed seconds-into-the-minute (0..60, fractional) to the second
  // hand's angle: a linear 0->360 sweep across second_sweep_s_ seconds, then
  // parked at 360 (=0, pointing at 12) for the remainder. That remainder is
  // the real clock's wait for the next minute's electrical impulse.
  float second_angle_deg_(float elapsed_s) const;

  void render_();
  void fill_bg_(lv_layer_t *layer);
  void draw_ticks_(lv_layer_t *layer, int cx, int cy, int R, Color color);
  // A flat-ended bar from `start_len` out to `len` px along `angle_deg` (0 =
  // 12 o'clock, clockwise) - the classic SBB hour/minute needle shape, with
  // no taper or rounding at either end.
  void draw_bar_hand_(lv_layer_t *layer, int cx, int cy, int start_len, int len,
                       float angle_deg, int width, Color color);
  // Mondaine/SBB second hand: a thin shaft plus a solid disc ("lollipop")
  // near the tip, with a short counterweight tail on the opposite side.
  void draw_second_hand_(lv_layer_t *layer, int cx, int cy, int R, float angle_deg, Color color);
  void draw_hub_(lv_layer_t *layer, int cx, int cy, int r, Color color);
  // Centres `text` horizontally on `cx`. Vertically, `edge_y` is either the
  // text's top (align_bottom = false) or bottom (align_bottom = true) -
  // used to flush the temperature/date lines against the hour ticks above
  // and below them rather than centering on a guessed offset.
  void draw_text_(lv_layer_t *layer, const std::string &text, const lv_font_t *font, int cx,
                   int edge_y, bool align_bottom, Color color, lv_opa_t opa);
  std::string format_date_(bool time_valid, uint8_t day_of_week, uint8_t day_of_month,
                            uint8_t month, uint16_t year) const;
  std::string format_temperature_() const;

  time::RealTimeClock *time_{nullptr};
  int canvas_w_{0}, canvas_h_{0};
  bool transparent_{false};
  uint32_t render_interval_ms_{100};
  uint32_t last_render_ms_{0};
  bool show_seconds_{true};
  uint32_t second_sweep_s_{58};
  bool show_ticks_{true};
  bool show_face_{false};
  bool night_mode_{false};
  bool show_date_{false};
  const lv_font_t *date_font_{nullptr};
  bool show_temperature_{false};
  const lv_font_t *temperature_font_{nullptr};
#ifdef USE_SENSOR
  sensor::Sensor *temperature_sensor_{nullptr};
#endif

  Color ink_{0xFF, 0xFF, 0xFF};
  Color paper_{0x00, 0x00, 0x00};
  Color second_{0xE7, 0x4C, 0x3C};
  Color face_{0x00, 0x00, 0x00};
  bool has_second_color_{false};
  bool has_face_color_{false};

  int last_sec_{-1};
  uint32_t last_sec_ms_{0};
  bool size_checked_{false};
  bool render_ok_{true};
};

}  // namespace sbb_clock
}  // namespace esphome

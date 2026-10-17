#pragma once

#include "esphome/core/component.h"
#include "esphome/core/color.h"
#include "esphome/components/time/real_time_clock.h"
#include "esphome/components/lvgl/lvgl_esphome.h"
#ifdef USE_SENSOR
#include "esphome/components/sensor/sensor.h"
#endif

#include <cstdint>

namespace esphome {
namespace sbb_clock_lite {

// Canvas-free, dark-only variant of sbb_clock.
//
// sbb_clock renders into its own canvas: 240x240 RGB565 = 115 kB of
// internal SRAM on a board without PSRAM (M5Dial), the single biggest heap
// consumer of the whole firmware. This variant keeps the same face (SBB
// geometry, Stop2Go second hand, date + temperature) but owns NO pixel
// buffer at all: it is a plain lv_obj with an LV_EVENT_DRAW_MAIN callback
// that draws ticks, text and hands straight into LVGL's normal render
// buffer, clipped to whatever area LVGL is refreshing. loop() only
// invalidates the small boxes the hands actually moved through.
//
// Dark only: transparent background (the page behind shows through - make
// it black), foreground-colored ticks/hands/text, red second hand. No
// night_mode, face fill, corner color or transparency options - there is
// nothing to swap and nothing to fill.
//
// RAM: a few hundred bytes (this object + one lv_obj) instead of ~115 kB.
// CPU: every invalidated strip re-runs the draw callback; LVGL clips each
// primitive, so ~70 line/rect calls per strip is cheap at 100 ms.
class SbbClockLite : public Component, public lvgl::LvCompound {
 public:
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::PROCESSOR; }

  void set_time(time::RealTimeClock *t) { this->time_ = t; }
  void set_render_interval(uint32_t ms) { this->render_interval_ms_ = ms; }
  void set_show_seconds(bool show) { this->show_seconds_ = show; }
  void set_second_hand_sweep_s(uint32_t s) { this->second_sweep_s_ = s > 0 ? s : 58; }
  void set_show_ticks(bool on) { this->show_ticks_ = on; }
  void set_show_ring(bool on) { this->show_ring_ = on; }
  void set_show_date(bool on) { this->show_date_ = on; }
  void set_date_font(const lv_font_t *f) { this->date_font_ = f; }
  void set_show_temperature(bool on) { this->show_temperature_ = on; }
  void set_temperature_font(const lv_font_t *f) { this->temperature_font_ = f; }
#ifdef USE_SENSOR
  void set_temperature_sensor(sensor::Sensor *s) { this->temperature_sensor_ = s; }
#endif
  void set_foreground(Color c) { this->ink_ = c; }
  void set_second_hand_color(Color c) { this->second_ = c; }

  // Called from the static LVGL event trampoline.
  void draw_(lv_layer_t *layer);

 protected:
  struct Pose {
    float hour_deg{-1}, minute_deg{-1}, second_deg{-1};
  };

  void attach_();
  void update_();
  bool now_or_fallback_(int &hh, int &mm, int &ss, uint8_t &wday, uint8_t &mday, uint8_t &month,
                        uint16_t &year) const;
  float sub_second_(int ss);
  float second_angle_deg_(float elapsed_s) const;
  void geometry_(int &cx, int &cy, int &R) const;
  void invalidate_rel_(const lv_area_t &a);

  time::RealTimeClock *time_{nullptr};
  uint32_t render_interval_ms_{100};
  uint32_t last_render_ms_{0};
  bool show_seconds_{true};
  uint32_t second_sweep_s_{58};
  bool show_ticks_{true};
  bool show_ring_{true};
  bool show_date_{true};
  const lv_font_t *date_font_{nullptr};
  bool show_temperature_{false};
  const lv_font_t *temperature_font_{nullptr};
#ifdef USE_SENSOR
  sensor::Sensor *temperature_sensor_{nullptr};
#endif

  Color ink_{0xFF, 0xFF, 0xFF};
  Color second_{0xE7, 0x4C, 0x3C};

  bool attached_{false};
  int last_sec_{-1};
  uint32_t last_sec_ms_{0};
  Pose pose_;
  // Fixed buffers - no std::string, no heap churn per frame.
  char date_[24]{0};
  char temp_[16]{0};
  bool date_valid_{false};
  bool temp_valid_{false};
};

}  // namespace sbb_clock_lite
}  // namespace esphome

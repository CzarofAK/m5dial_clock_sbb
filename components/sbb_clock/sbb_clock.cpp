#include "sbb_clock.h"
#include "esphome/core/log.h"
#include "esphome/core/hal.h"
#ifdef USE_ESP32
#include <esp_heap_caps.h>
#endif
#include <algorithm>
#include <cmath>
#include <cstdio>

namespace esphome {
namespace sbb_clock {

static const char *const TAG = "sbb_clock";
static const float PI_F = 3.14159265358979323846f;
// Inner-end radius (as a fraction of R) of the hour ticks, shared by
// draw_ticks_() and the temperature/date text placement below - so the
// text lines stay flush with the actual tick ends even if this changes.
static const float HOUR_TICK_INNER = 0.78f;

void *alloc_canvas_buf(size_t size) {
#ifdef USE_ESP32
  size = LV_ROUND_UP(size, LV_DRAW_BUF_ALIGN);
  void *buf = heap_caps_aligned_alloc(LV_DRAW_BUF_ALIGN, size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  if (buf != nullptr) {
    ESP_LOGD(TAG, "Canvas: %u bytes in internal RAM", (unsigned) size);
    return buf;
  }
  buf = heap_caps_aligned_alloc(LV_DRAW_BUF_ALIGN, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (buf != nullptr) {
    ESP_LOGW(TAG,
             "Canvas: %u bytes in PSRAM - it did not fit in internal RAM, so drawing will be "
             "markedly slower. Shrink the widget or add PSRAM.",
             (unsigned) size);
  }
  return buf;
#else
  return lv_malloc_core(size);
#endif
}

// Point at distance `r` px from (cx, cy) along `angle_deg` (0 = 12 o'clock,
// clockwise) - the one bit of trigonometry every hand, tick and hub position
// on this face is built from.
static lv_point_precise_t polar_point(int cx, int cy, float angle_deg, float r) {
  float rad = angle_deg * PI_F / 180.0f;
  return lv_point_precise_t{(lv_value_precise_t) lroundf(cx + sinf(rad) * r),
                             (lv_value_precise_t) lroundf(cy - cosf(rad) * r)};
}

void SbbClock::setup() {
  // Nothing to warm up: the canvas buffer is allocated by the generated
  // code before setup() runs (see __init__.py's to_code), and the first
  // loop() draws whatever the wall clock (or the no-time-yet fallback pose)
  // says right away.
}

void SbbClock::dump_config() {
  ESP_LOGCONFIG(TAG, "SBB Clock:");
  ESP_LOGCONFIG(TAG, "  Canvas: %dx%d px, render every %ums", this->canvas_w_, this->canvas_h_,
                (unsigned) this->render_interval_ms_);
  ESP_LOGCONFIG(TAG, "  Seconds hand: %s (sweep %us, pause %us)", YESNO(this->show_seconds_),
                (unsigned) this->second_sweep_s_, (unsigned) (60 - this->second_sweep_s_));
  ESP_LOGCONFIG(TAG, "  Date: %s, Temperature: %s", YESNO(this->show_date_),
                YESNO(this->show_temperature_));
}

bool SbbClock::now_or_fallback_(int &hh, int &mm, int &ss, uint8_t &day_of_week,
                                 uint8_t &day_of_month, uint8_t &month, uint16_t &year) const {
  if (this->time_ != nullptr) {
    ESPTime t = this->time_->now();
    if (t.is_valid()) {
      hh = t.hour;
      mm = t.minute;
      ss = t.second;
      day_of_week = t.day_of_week;
      day_of_month = t.day_of_month;
      month = t.month;
      year = t.year;
      return true;
    }
  }
  // No valid time yet - a pose where the hands aren't stacked, with seconds
  // running off uptime so the face still looks alive while waiting to sync.
  hh = 0;
  mm = 15;
  ss = (int) ((millis() / 1000) % 60);
  day_of_week = day_of_month = month = 0;
  year = 0;
  return false;
}

float SbbClock::sub_second_(int ss) {
  uint32_t now = millis();
  if (ss != this->last_sec_) {
    this->last_sec_ = ss;
    this->last_sec_ms_ = now;
  }
  float frac = (now - this->last_sec_ms_) / 1000.0f;
  return frac > 1.0f ? 1.0f : frac;
}

float SbbClock::second_angle_deg_(float elapsed_s) const {
  float sweep = (float) this->second_sweep_s_;
  if (elapsed_s >= sweep)
    return 360.0f;  // parked at 12, waiting for the next minute's impulse
  return (elapsed_s / sweep) * 360.0f;
}

std::string SbbClock::format_date_(bool time_valid, uint8_t day_of_week, uint8_t day_of_month,
                                    uint8_t month, uint16_t year) const {
  if (!time_valid)
    return "--.--.----";
  // ESPTime::day_of_week: Sunday = 1 .. Saturday = 7.
  static const char *const WD[] = {"So", "Mo", "Di", "Mi", "Do", "Fr", "Sa"};
  char buf[24];
  int idx = (day_of_week >= 1 && day_of_week <= 7) ? day_of_week - 1 : 0;
  snprintf(buf, sizeof(buf), "%s, %02u.%02u.%04u", WD[idx], (unsigned) day_of_month,
           (unsigned) month, (unsigned) year);
  return std::string(buf);
}

std::string SbbClock::format_temperature_() const {
#ifdef USE_SENSOR
  if (this->temperature_sensor_ != nullptr && this->temperature_sensor_->has_state()) {
    char buf[16];
    snprintf(buf, sizeof(buf), "%.1f°C", this->temperature_sensor_->state);
    return std::string(buf);
  }
#endif
  return "--.-°C";
}

void SbbClock::fill_bg_(lv_layer_t *layer) {
  if (this->transparent_) {
    lv_canvas_fill_bg(this->obj, lv_color_black(), LV_OPA_TRANSP);
  } else {
    Color bg = this->paper_now_();
    lv_canvas_fill_bg(this->obj, lv_color_make(bg.r, bg.g, bg.b), LV_OPA_COVER);
  }
}

void SbbClock::draw_ticks_(lv_layer_t *layer, int cx, int cy, int R, Color color) {
  lv_draw_line_dsc_t dsc;
  lv_draw_line_dsc_init(&dsc);
  dsc.color = lv_color_make(color.r, color.g, color.b);
  dsc.round_start = dsc.round_end = true;
  for (int i = 0; i < 60; i++) {
    bool hour_pos = (i % 5 == 0);
    dsc.width = hour_pos ? std::max(2, R / 22) : std::max(1, R / 60);
    float inner = hour_pos ? HOUR_TICK_INNER : 0.90f;
    float angle = i * 6.0f;
    dsc.p1 = polar_point(cx, cy, angle, R * inner);
    dsc.p2 = polar_point(cx, cy, angle, R * 0.96f);
    lv_draw_line(layer, &dsc);
  }
}

void SbbClock::draw_bar_hand_(lv_layer_t *layer, int cx, int cy, int start_len, int len,
                               float angle_deg, int width, Color color) {
  // A single flat-ended bar - no taper, no rounding - the classic SBB
  // hour/minute needle. One line with square caps reads as a clean
  // rectangle; two mitred triangles would leave a seam along their shared
  // edge at some angles.
  lv_draw_line_dsc_t dsc;
  lv_draw_line_dsc_init(&dsc);
  dsc.color = lv_color_make(color.r, color.g, color.b);
  dsc.width = width;
  dsc.round_start = dsc.round_end = false;
  dsc.p1 = polar_point(cx, cy, angle_deg, start_len);
  dsc.p2 = polar_point(cx, cy, angle_deg, len);
  lv_draw_line(layer, &dsc);
}

void SbbClock::draw_second_hand_(lv_layer_t *layer, int cx, int cy, int R, float angle_deg,
                                  Color color) {
  int len = (int) (R * 0.86f);
  int ball_dist = (int) (len * 0.66f);
  int ball_r = std::max(2, R / 11);
  int tail_len = (int) (R * 0.22f);

  lv_draw_line_dsc_t dsc;
  lv_draw_line_dsc_init(&dsc);
  dsc.color = lv_color_make(color.r, color.g, color.b);
  dsc.width = std::max(1, R / 45);
  dsc.round_start = dsc.round_end = true;
  // Shaft from the counterweight tail, through the pivot, out to the ball -
  // one line so the two halves never show a butt-join at the centre.
  dsc.p1 = polar_point(cx, cy, angle_deg + 180.0f, tail_len);
  dsc.p2 = polar_point(cx, cy, angle_deg, ball_dist);
  lv_draw_line(layer, &dsc);

  lv_draw_rect_dsc_t dot;
  lv_draw_rect_dsc_init(&dot);
  dot.radius = LV_RADIUS_CIRCLE;
  dot.bg_color = dsc.color;
  dot.bg_opa = LV_OPA_COVER;
  lv_point_precise_t p = polar_point(cx, cy, angle_deg, ball_dist);
  lv_area_t area = {(int32_t) p.x - ball_r, (int32_t) p.y - ball_r, (int32_t) p.x + ball_r,
                     (int32_t) p.y + ball_r};
  lv_draw_rect(layer, &dot, &area);
}

void SbbClock::draw_hub_(lv_layer_t *layer, int cx, int cy, int r, Color color) {
  lv_draw_rect_dsc_t d;
  lv_draw_rect_dsc_init(&d);
  d.radius = LV_RADIUS_CIRCLE;
  d.bg_color = lv_color_make(color.r, color.g, color.b);
  d.bg_opa = LV_OPA_COVER;
  lv_area_t a = {cx - r, cy - r, cx + r, cy + r};
  lv_draw_rect(layer, &d, &a);
}

void SbbClock::draw_text_(lv_layer_t *layer, const std::string &text, const lv_font_t *font,
                           int cx, int edge_y, bool align_bottom, Color color, lv_opa_t opa) {
  if (font == nullptr || text.empty())
    return;
  lv_point_t sz;
  lv_text_get_size(&sz, text.c_str(), font, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
  lv_draw_label_dsc_t ld;
  lv_draw_label_dsc_init(&ld);
  ld.color = lv_color_make(color.r, color.g, color.b);
  ld.font = font;
  ld.text = text.c_str();
  ld.text_local = 1;  // `text` is a stack-local std::string's buffer
  ld.opa = opa;
  int x = cx - sz.x / 2;
  int y = align_bottom ? edge_y - sz.y : edge_y;
  lv_area_t area = {x, y, x + sz.x, y + sz.y};
  lv_draw_label(layer, &ld, &area);
}

void SbbClock::render_() {
  if (this->obj == nullptr)
    return;
  int w = this->canvas_w_, h = this->canvas_h_;
  if (w <= 0 || h <= 0)
    return;
  if (!this->size_checked_) {
    this->size_checked_ = true;
    lv_draw_buf_t *buf = lv_canvas_get_draw_buf(this->obj);
    if (buf == nullptr || buf->data == nullptr) {
      ESP_LOGE(TAG,
               "Canvas draw buffer (%dx%d) failed to allocate - not enough free RAM. Shrink "
               "width/height or add PSRAM. Disabling rendering.",
               w, h);
      this->render_ok_ = false;
    }
  }
  if (!this->render_ok_)
    return;

  int cx = w / 2, cy = h / 2;
  int R = std::min(w, h) / 2 - 1;
  if (R < 6)
    return;

  lv_layer_t layer;
  lv_canvas_init_layer(this->obj, &layer);
  this->fill_bg_(&layer);

  Color ink = this->ink_now_();
  if (this->show_face_) {
    lv_draw_rect_dsc_t face;
    lv_draw_rect_dsc_init(&face);
    face.radius = LV_RADIUS_CIRCLE;
    Color fc = this->face_color_();
    face.bg_color = lv_color_make(fc.r, fc.g, fc.b);
    face.bg_opa = LV_OPA_COVER;
    face.border_color = lv_color_make(ink.r, ink.g, ink.b);
    face.border_width = std::max(1, R / 40);
    face.border_opa = LV_OPA_COVER;
    lv_area_t area = {cx - R, cy - R, cx + R, cy + R};
    lv_draw_rect(&layer, &face, &area);
  }
  if (this->show_ticks_)
    this->draw_ticks_(&layer, cx, cy, R, ink);

  int hh, mm, ss;
  uint8_t wday, mday, month;
  uint16_t year;
  bool time_valid = this->now_or_fallback_(hh, mm, ss, wday, mday, month, year);
  float sub = this->sub_second_(ss);
  float elapsed_s = ss + sub;  // 0..60, continuous, real elapsed time this minute

  // Drawn before the hands, like the printed lines on a real dial - so the
  // hands sweep over the text, never under it.
  //
  // Flush against the hour ticks at the 2/10 o'clock (temperature) and
  // 4/8 o'clock (date) positions - both sit at exactly +/-60 deg from 12,
  // so their inner ends are at the same distance from the centre either
  // way: R * HOUR_TICK_INNER * cos(60 deg) = R * HOUR_TICK_INNER * 0.5.
  int tick_edge_offset = (int) lroundf(R * HOUR_TICK_INNER * 0.5f);
  if (this->show_temperature_) {
    lv_opa_t opa = LV_OPA_COVER;
#ifdef USE_SENSOR
    if (this->temperature_sensor_ == nullptr || !this->temperature_sensor_->has_state())
      opa = LV_OPA_40;
#else
    opa = LV_OPA_40;
#endif
    this->draw_text_(&layer, this->format_temperature_(), this->temperature_font_, cx,
                      cy - tick_edge_offset, /*align_bottom=*/true, ink, opa);
  }
  if (this->show_date_) {
    lv_opa_t opa = time_valid ? LV_OPA_COVER : LV_OPA_40;
    std::string date = this->format_date_(time_valid, wday, mday, month, year);
    this->draw_text_(&layer, date, this->date_font_, cx, cy + tick_edge_offset,
                      /*align_bottom=*/false, ink, opa);
  }

  // Real SBB kinematics: the minute hand only ever holds one of 60 fixed
  // positions - it jumps, it never creeps - and it jumps in the same
  // instant the second hand snaps back to 12 and starts its next sweep
  // (both driven by the same once-a-minute impulse). The hour hand is the
  // one hand that genuinely does creep continuously in the real mechanism.
  float minute_deg = mm * 6.0f;
  float hour_deg = (hh % 12) * 30.0f + minute_deg / 12.0f + (elapsed_s / 60.0f) * 0.5f;
  float second_deg = this->second_angle_deg_(elapsed_s);

  this->draw_bar_hand_(&layer, cx, cy, 0, (int) (R * 0.50f), hour_deg, std::max(2, R / 14), ink);
  this->draw_bar_hand_(&layer, cx, cy, 0, (int) (R * 0.78f), minute_deg, std::max(2, R / 20), ink);
  if (this->show_seconds_)
    this->draw_second_hand_(&layer, cx, cy, R, second_deg, this->second_color_());
  this->draw_hub_(&layer, cx, cy, std::max(2, R / 16), ink);

  lv_canvas_finish_layer(this->obj, &layer);
  lv_obj_invalidate(this->obj);
}

void SbbClock::loop() {
  uint32_t now_ms = millis();
  if (now_ms - this->last_render_ms_ < this->render_interval_ms_)
    return;
  this->last_render_ms_ = now_ms;
  this->render_();
}

}  // namespace sbb_clock
}  // namespace esphome

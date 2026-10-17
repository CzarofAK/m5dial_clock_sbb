#include "sbb_clock_lite.h"
#include "esphome/core/log.h"
#include "esphome/core/hal.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace esphome {
namespace sbb_clock_lite {

static const char *const TAG = "sbb_clock_lite";
static const float PI_F = 3.14159265358979323846f;
// Same proportions as sbb_clock, so both variants look identical.
static const float HOUR_TICK_INNER = 0.78f;
static const float MINUTE_HAND_LEN = 0.72f;
static const float HOUR_HAND_LEN = 0.50f;

static lv_point_precise_t polar(int cx, int cy, float deg, float r) {
  float rad = deg * PI_F / 180.0f;
  return lv_point_precise_t{(lv_value_precise_t) lroundf(cx + sinf(rad) * r),
                             (lv_value_precise_t) lroundf(cy - cosf(rad) * r)};
}

static lv_area_t line_area(lv_point_precise_t a, lv_point_precise_t b, int w) {
  int32_t pad = w / 2 + 3;
  return lv_area_t{(int32_t) std::min(a.x, b.x) - pad, (int32_t) std::min(a.y, b.y) - pad,
                    (int32_t) std::max(a.x, b.x) + pad, (int32_t) std::max(a.y, b.y) + pad};
}

static lv_area_t area_union(const lv_area_t &a, const lv_area_t &b) {
  return lv_area_t{std::min(a.x1, b.x1), std::min(a.y1, b.y1), std::max(a.x2, b.x2),
                    std::max(a.y2, b.y2)};
}

static inline lv_color_t to_lv(Color c) { return lv_color_make(c.r, c.g, c.b); }

// ---- shape geometry, relative to the object's own top-left ------------
struct Dims {
  int hour_len, hour_w, minute_len, minute_w, hub_r, sec_len_tail, sec_ball_dist, sec_ball_r,
      sec_w, text_off;
};

static Dims dims(int R) {
  Dims d;
  d.hour_len = (int) (R * HOUR_HAND_LEN);
  d.hour_w = std::max(2, R / 14);
  d.minute_len = (int) (R * MINUTE_HAND_LEN);
  d.minute_w = std::max(2, R / 20);
  d.hub_r = std::max(2, R / 16);
  int sec_len = (int) (R * 0.86f);
  d.sec_ball_dist = (int) (sec_len * 0.66f);
  d.sec_len_tail = (int) (R * 0.22f);
  d.sec_ball_r = std::max(2, R / 11);
  d.sec_w = std::max(1, R / 45);
  d.text_off = (int) lroundf(R * HOUR_TICK_INNER * 0.5f);
  return d;
}

static lv_area_t hand_area(int cx, int cy, int len, float deg, int w) {
  return line_area(polar(cx, cy, deg, 0), polar(cx, cy, deg, len), w);
}

static lv_area_t second_area(int cx, int cy, const Dims &d, float deg) {
  lv_point_precise_t tail = polar(cx, cy, deg + 180.0f, d.sec_len_tail);
  lv_point_precise_t ball = polar(cx, cy, deg, d.sec_ball_dist);
  lv_area_t a = line_area(tail, ball, d.sec_w);
  int32_t p = d.sec_ball_r + 2;
  lv_area_t b{(int32_t) ball.x - p, (int32_t) ball.y - p, (int32_t) ball.x + p, (int32_t) ball.y + p};
  return area_union(a, b);
}

// ---- LVGL glue --------------------------------------------------------
static void draw_main_cb(lv_event_t *e) {
  auto *self = static_cast<SbbClockLite *>(lv_event_get_user_data(e));
  lv_layer_t *layer = lv_event_get_layer(e);
  if (self != nullptr && layer != nullptr)
    self->draw_(layer);
}

void SbbClockLite::attach_() {
  if (this->attached_ || this->obj == nullptr)
    return;
  // A bare object: no bg, border, padding or scrollbar of its own, and
  // not clickable, so touches/gestures fall through to the page. Local
  // style props only - lv_obj_remove_style_all() would also drop the
  // size/align set from YAML.
  lv_obj_set_style_bg_opa(this->obj, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_border_width(this->obj, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(this->obj, 0, LV_PART_MAIN);
  lv_obj_set_style_radius(this->obj, 0, LV_PART_MAIN);
  lv_obj_set_scrollbar_mode(this->obj, LV_SCROLLBAR_MODE_OFF);
  lv_obj_remove_flag(this->obj, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_remove_flag(this->obj, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_event_cb(this->obj, draw_main_cb, LV_EVENT_DRAW_MAIN, this);
  this->attached_ = true;
  lv_obj_invalidate(this->obj);
}

void SbbClockLite::dump_config() {
  ESP_LOGCONFIG(TAG, "SBB Clock Lite (canvas-free, dark):");
  ESP_LOGCONFIG(TAG, "  Update every %ums, seconds: %s (sweep %us)",
                (unsigned) this->render_interval_ms_, YESNO(this->show_seconds_),
                (unsigned) this->second_sweep_s_);
  ESP_LOGCONFIG(TAG, "  Date: %s, Temperature: %s", YESNO(this->show_date_),
                YESNO(this->show_temperature_));
}

void SbbClockLite::geometry_(int &cx, int &cy, int &R) const {
  int w = lv_obj_get_width(this->obj), h = lv_obj_get_height(this->obj);
  cx = w / 2;
  cy = h / 2;
  R = std::min(w, h) / 2 - 1;
}

void SbbClockLite::invalidate_rel_(const lv_area_t &a) {
  lv_area_t c;
  lv_obj_get_coords(this->obj, &c);
  lv_area_t abs{a.x1 + c.x1, a.y1 + c.y1, a.x2 + c.x1, a.y2 + c.y1};
  lv_obj_invalidate_area(this->obj, &abs);
}

// ---- time / kinematics (identical to sbb_clock) -----------------------
bool SbbClockLite::now_or_fallback_(int &hh, int &mm, int &ss, uint8_t &wday, uint8_t &mday,
                                     uint8_t &month, uint16_t &year) const {
  if (this->time_ != nullptr) {
    ESPTime t = this->time_->now();
    if (t.is_valid()) {
      hh = t.hour;
      mm = t.minute;
      ss = t.second;
      wday = t.day_of_week;
      mday = t.day_of_month;
      month = t.month;
      year = t.year;
      return true;
    }
  }
  hh = 0;
  mm = 15;
  ss = (int) ((millis() / 1000) % 60);
  wday = mday = month = 0;
  year = 0;
  return false;
}

float SbbClockLite::sub_second_(int ss) {
  uint32_t now = millis();
  if (ss != this->last_sec_) {
    this->last_sec_ = ss;
    this->last_sec_ms_ = now;
  }
  float frac = (now - this->last_sec_ms_) / 1000.0f;
  return frac > 1.0f ? 1.0f : frac;
}

float SbbClockLite::second_angle_deg_(float elapsed_s) const {
  float sweep = (float) this->second_sweep_s_;
  if (elapsed_s >= sweep)
    return 360.0f;
  return (elapsed_s / sweep) * 360.0f;
}

// ---- per-tick update: compute pose, invalidate only what moved ---------
void SbbClockLite::update_() {
  int cx, cy, R;
  this->geometry_(cx, cy, R);
  if (R < 6)
    return;
  Dims d = dims(R);

  int hh, mm, ss;
  uint8_t wday, mday, month;
  uint16_t year;
  bool valid = this->now_or_fallback_(hh, mm, ss, wday, mday, month, year);
  float elapsed = ss + this->sub_second_(ss);

  Pose p;
  p.minute_deg = mm * 6.0f;
  p.hour_deg = (hh % 12) * 30.0f + p.minute_deg / 12.0f + (elapsed / 60.0f) * 0.5f;
  p.second_deg = this->show_seconds_ ? this->second_angle_deg_(elapsed) : 0.0f;

  bool first = this->pose_.hour_deg < 0;
  auto moved = [](float a, float b) { return fabsf(a - b) > 0.05f; };

  if (!first && moved(p.hour_deg, this->pose_.hour_deg))
    this->invalidate_rel_(area_union(hand_area(cx, cy, d.hour_len, this->pose_.hour_deg, d.hour_w),
                                     hand_area(cx, cy, d.hour_len, p.hour_deg, d.hour_w)));
  if (!first && moved(p.minute_deg, this->pose_.minute_deg))
    this->invalidate_rel_(
        area_union(hand_area(cx, cy, d.minute_len, this->pose_.minute_deg, d.minute_w),
                   hand_area(cx, cy, d.minute_len, p.minute_deg, d.minute_w)));
  if (!first && this->show_seconds_ && moved(p.second_deg, this->pose_.second_deg)) {
    // Invalidate old and new position separately: at a large step their
    // union would be most of the dial.
    this->invalidate_rel_(second_area(cx, cy, d, this->pose_.second_deg));
    this->invalidate_rel_(second_area(cx, cy, d, p.second_deg));
  }

  // Text: only when the string actually changes (once a day / per sensor update).
  char buf[24];
  if (this->show_date_) {
    if (valid) {
      static const char *const WD[] = {"So", "Mo", "Di", "Mi", "Do", "Fr", "Sa"};
      int i = (wday >= 1 && wday <= 7) ? wday - 1 : 0;
      snprintf(buf, sizeof(buf), "%s, %02u.%02u.%04u", WD[i], (unsigned) mday, (unsigned) month,
               (unsigned) year);
    } else {
      strcpy(buf, "--.--.----");
    }
    if (strcmp(buf, this->date_) != 0 || valid != this->date_valid_) {
      strncpy(this->date_, buf, sizeof(this->date_) - 1);
      this->date_valid_ = valid;
      // Full-width strip below the centre - cheap, happens once a day.
      int h = this->date_font_ ? lv_font_get_line_height(this->date_font_) : 20;
      this->invalidate_rel_(lv_area_t{cx - R, cy + d.text_off - 2, cx + R, cy + d.text_off + h + 2});
    }
  }
  if (this->show_temperature_) {
    bool tv = false;
#ifdef USE_SENSOR
    if (this->temperature_sensor_ != nullptr && this->temperature_sensor_->has_state() &&
        !std::isnan(this->temperature_sensor_->state)) {
      snprintf(buf, sizeof(buf), "%.1f°C", this->temperature_sensor_->state);
      tv = true;
    }
#endif
    if (!tv)
      strcpy(buf, "--.-°C");
    if (strcmp(buf, this->temp_) != 0 || tv != this->temp_valid_) {
      strncpy(this->temp_, buf, sizeof(this->temp_) - 1);
      this->temp_valid_ = tv;
      int h = this->temperature_font_ ? lv_font_get_line_height(this->temperature_font_) : 20;
      this->invalidate_rel_(lv_area_t{cx - R, cy - d.text_off - h - 2, cx + R, cy - d.text_off + 2});
    }
  }

  this->pose_ = p;
  if (first)
    lv_obj_invalidate(this->obj);
}

void SbbClockLite::loop() {
  if (!this->attached_) {
    this->attach_();
    if (!this->attached_)
      return;
  }
  uint32_t now = millis();
  if (now - this->last_render_ms_ < this->render_interval_ms_)
    return;
  this->last_render_ms_ = now;
  // Skip all work while the page isn't on screen.
  if (!lv_obj_is_visible(this->obj) || lv_obj_get_screen(this->obj) != lv_screen_active())
    return;
  this->update_();
}

// ---- drawing (LV_EVENT_DRAW_MAIN, clipped by LVGL) --------------------
void SbbClockLite::draw_(lv_layer_t *layer) {
  if (this->pose_.hour_deg < 0)
    return;  // nothing computed yet
  lv_area_t c;
  lv_obj_get_coords(this->obj, &c);
  int w = lv_area_get_width(&c), h = lv_area_get_height(&c);
  int cx = c.x1 + w / 2, cy = c.y1 + h / 2;
  int R = std::min(w, h) / 2 - 1;
  if (R < 6)
    return;
  Dims d = dims(R);
  lv_color_t ink = to_lv(this->ink_);

  lv_draw_line_dsc_t ln;
  lv_draw_line_dsc_init(&ln);
  ln.color = ink;
  ln.round_start = ln.round_end = false;

  if (this->show_ring_) {
    lv_draw_arc_dsc_t arc;
    lv_draw_arc_dsc_init(&arc);
    arc.color = ink;
    arc.width = std::max(1, R / 40);
    arc.center.x = cx;
    arc.center.y = cy;
    arc.radius = R;
    arc.start_angle = 0;
    arc.end_angle = 360;
    lv_draw_arc(layer, &arc);
  }

  if (this->show_ticks_) {
    for (int i = 0; i < 60; i++) {
      bool hr = (i % 5 == 0);
      ln.width = hr ? std::max(2, R / 22) : std::max(1, R / 60);
      float inner = hr ? HOUR_TICK_INNER : 0.90f;
      ln.p1 = polar(cx, cy, i * 6.0f, R * inner);
      ln.p2 = polar(cx, cy, i * 6.0f, R * 0.96f);
      lv_draw_line(layer, &ln);
    }
  }

  auto text = [&](const char *s, const lv_font_t *f, int edge_y, bool bottom, bool valid) {
    if (f == nullptr || s[0] == 0)
      return;
    lv_point_t sz;
    lv_text_get_size(&sz, s, f, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    int y = bottom ? edge_y - sz.y : edge_y;
    lv_area_t a{cx - sz.x / 2, y, cx - sz.x / 2 + sz.x, y + sz.y};
    lv_draw_label_dsc_t ld;
    lv_draw_label_dsc_init(&ld);
    ld.color = ink;
    ld.font = f;
    ld.text = s;
    ld.text_local = 1;
    ld.opa = valid ? LV_OPA_COVER : LV_OPA_40;
    lv_draw_label(layer, &ld, &a);
  };
  if (this->show_temperature_)
    text(this->temp_, this->temperature_font_, cy - d.text_off, true, this->temp_valid_);
  if (this->show_date_)
    text(this->date_, this->date_font_, cy + d.text_off, false, this->date_valid_);

  ln.width = d.hour_w;
  ln.p1 = polar(cx, cy, this->pose_.hour_deg, 0);
  ln.p2 = polar(cx, cy, this->pose_.hour_deg, d.hour_len);
  lv_draw_line(layer, &ln);

  ln.width = d.minute_w;
  ln.p1 = polar(cx, cy, this->pose_.minute_deg, 0);
  ln.p2 = polar(cx, cy, this->pose_.minute_deg, d.minute_len);
  lv_draw_line(layer, &ln);

  if (this->show_seconds_) {
    lv_color_t sc = to_lv(this->second_);
    ln.color = sc;
    ln.width = d.sec_w;
    ln.round_start = ln.round_end = true;
    lv_point_precise_t ball = polar(cx, cy, this->pose_.second_deg, d.sec_ball_dist);
    ln.p1 = polar(cx, cy, this->pose_.second_deg + 180.0f, d.sec_len_tail);
    ln.p2 = ball;
    lv_draw_line(layer, &ln);

    lv_draw_rect_dsc_t dot;
    lv_draw_rect_dsc_init(&dot);
    dot.radius = LV_RADIUS_CIRCLE;
    dot.bg_color = sc;
    dot.bg_opa = LV_OPA_COVER;
    lv_area_t a{(int32_t) ball.x - d.sec_ball_r, (int32_t) ball.y - d.sec_ball_r,
                (int32_t) ball.x + d.sec_ball_r, (int32_t) ball.y + d.sec_ball_r};
    lv_draw_rect(layer, &dot, &a);
  }

  lv_draw_rect_dsc_t hub;
  lv_draw_rect_dsc_init(&hub);
  hub.radius = LV_RADIUS_CIRCLE;
  hub.bg_color = ink;
  hub.bg_opa = LV_OPA_COVER;
  lv_area_t ha{cx - d.hub_r, cy - d.hub_r, cx + d.hub_r, cy + d.hub_r};
  lv_draw_rect(layer, &hub, &ha);
}

}  // namespace sbb_clock_lite
}  // namespace esphome

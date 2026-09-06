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
// How far the minute hand reaches, as a fraction of R.
static const float MINUTE_HAND_LEN = 0.72f;

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

// ---- dirty-rect bookkeeping -------------------------------------------
// render_() used to erase a fixed-size disc every frame that wasn't a full
// redraw, on the reasoning that hands can point anywhere so nothing
// smaller is safe in the worst case. That's true across ALL possible
// frames, but wildly pessimistic for any ONE frame: the hands only ever
// point in one specific direction each, so what actually needs
// erasing/redrawing is a handful of thin lines plus a small hub and up to
// two text labels - not a full disc.
//
// A first attempt at fixing this unioned every shape's bounding box into
// ONE combined erase rectangle per frame. That's wrong, not just
// pessimistic: each INDIVIDUAL shape's box is safely inside the round face
// (see bar_hand_area()'s own comment for why), but the UNION of several
// such boxes is not - an axis-aligned bounding box can combine the
// left/right extent of one shape with the up/down extent of a completely
// different one, landing a CORNER of the combined box farther from the
// centre than either shape actually reaches (e.g. the hour hand pointing
// straight up unioned with the minute hand pointing right). That corner
// can end up outside the round face altogether, and erasing it with
// face_color_() paints face colour into the square canvas's corner area -
// visible as a stray rectangle wherever that corner colour differs from
// the face (not on this repo's own 240x240 configs, where both happen to
// be the same white - but visible when scaled up for a page/bezel colour
// that differs from the face, as reported against a 10"-class build).
//
// The fix actually used below: track and erase each shape SEPARATELY
// (hour, minute, second, hub, temperature text, date text), not as one
// combined box. Every one of those boxes is individually safe (see
// bar_hand_area()/second_hand_area()'s own comments), so there's no
// combined-box corner to worry about. Erase/redraw is a handful of small
// rects per frame instead of one - still far cheaper than the old fixed
// disc, since each box is tightly sized to its own shape.
static lv_area_t area_union(const lv_area_t &a, const lv_area_t &b) {
  return lv_area_t{std::min(a.x1, b.x1), std::min(a.y1, b.y1), std::max(a.x2, b.x2),
                    std::max(a.y2, b.y2)};
}

static lv_area_t pad_area(lv_area_t a, int32_t pad) {
  return lv_area_t{a.x1 - pad, a.y1 - pad, a.x2 + pad, a.y2 + pad};
}

// Bounding box of a straight line from p1 to p2, `width` px wide, padded a
// few extra px for antialiasing bleed at the edges/joins.
static lv_area_t line_area(lv_point_precise_t p1, lv_point_precise_t p2, int width) {
  int32_t pad = width / 2 + 3;
  int32_t x1 = (int32_t) p1.x, x2 = (int32_t) p2.x;
  int32_t y1 = (int32_t) p1.y, y2 = (int32_t) p2.y;
  return lv_area_t{std::min(x1, x2) - pad, std::min(y1, y2) - pad, std::max(x1, x2) + pad,
                    std::max(y1, y2) + pad};
}

// Bounding box of a filled circle centred on (cx, cy), same AA padding as
// line_area().
static lv_area_t circle_area(int cx, int cy, int r) {
  int32_t pad = r + 2;
  return lv_area_t{cx - pad, cy - pad, cx + pad, cy + pad};
}

// Bounding box of a bar hand (see draw_bar_hand_()) - same p1/p2 math,
// called with the exact same arguments so the two never drift apart.
//
// Safe by construction: p1 is at distance `start_len` and p2 at distance
// `len` from (cx, cy), both along the SAME angle - i.e. this is a segment
// through (or starting at) the centre. For any such segment, every corner
// of its axis-aligned bounding box is within max(|start_len|, |len|) of
// the centre (each corner's x and y coordinates are individually within
// that range of cx/cy, and distance can only shrink when x and y are
// combined) - so as long as the caller never passes a `len` beyond the
// dial radius (true for every hand below), this box never reaches past
// the round face.
static lv_area_t bar_hand_area(int cx, int cy, int start_len, int len, float angle_deg,
                                int width) {
  lv_point_precise_t p1 = polar_point(cx, cy, angle_deg, start_len);
  lv_point_precise_t p2 = polar_point(cx, cy, angle_deg, len);
  return line_area(p1, p2, width);
}

// Shared geometry for the second hand - the single source of truth for
// both draw_second_hand_() (which draws exactly this) and second_hand_area()
// (which bounds it), so the two constants sets can't diverge.
struct SecondHandGeom {
  lv_point_precise_t tail, ball;
  int ball_r;
  int width;
};

static SecondHandGeom second_hand_geom(int cx, int cy, int R, float angle_deg) {
  int len = (int) (R * 0.86f);
  int ball_dist = (int) (len * 0.66f);
  int tail_len = (int) (R * 0.22f);
  SecondHandGeom g;
  g.ball_r = std::max(2, R / 11);
  g.width = std::max(1, R / 45);
  g.tail = polar_point(cx, cy, angle_deg + 180.0f, tail_len);
  g.ball = polar_point(cx, cy, angle_deg, ball_dist);
  return g;
}

// Safe for the same reason as bar_hand_area(): tail and ball both sit on
// the second hand's own single angle through the centre (tail_len < R,
// ball_dist < R), and the ball's own circle is padded around a point
// that's itself within R of the centre - see circle_area()'s docstring.
static lv_area_t second_hand_area(int cx, int cy, int R, float angle_deg) {
  SecondHandGeom g = second_hand_geom(cx, cy, R, angle_deg);
  lv_area_t shaft = line_area(g.tail, g.ball, g.width);
  lv_area_t dot = circle_area((int) g.ball.x, (int) g.ball.y, g.ball_r);
  return area_union(shaft, dot);
}

// Exact label placement box - cx horizontally centred, edge_y is the
// text's top or bottom edge depending on align_bottom. Shared by
// draw_text_() (which draws exactly this box, unpadded) and render_()'s
// dirty-rect accounting (which pads it before using it for erase/
// invalidate) - one source of truth for where the text actually lands.
// Returns false (leaves `out` untouched) when there's nothing to draw.
static bool text_area(lv_area_t &out, const std::string &text, const lv_font_t *font, int cx,
                       int edge_y, bool align_bottom) {
  if (font == nullptr || text.empty())
    return false;
  lv_point_t sz;
  lv_text_get_size(&sz, text.c_str(), font, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
  int x = cx - sz.x / 2;
  int y = align_bottom ? edge_y - sz.y : edge_y;
  out = lv_area_t{x, y, x + sz.x, y + sz.y};
  return true;
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
    // corner_color_(), not paper_now_() directly - see set_corner_color()'s
    // own comment. Falls back to paper_now_() (this call's only behavior
    // before corner_color existed) when unset.
    Color bg = this->corner_color_();
    lv_canvas_fill_bg(this->obj, lv_color_make(bg.r, bg.g, bg.b), LV_OPA_COVER);
  }
}

void SbbClock::draw_ticks_(lv_layer_t *layer, int cx, int cy, int R, Color color) {
  lv_draw_line_dsc_t dsc;
  lv_draw_line_dsc_init(&dsc);
  dsc.color = lv_color_make(color.r, color.g, color.b);
  // Flat caps, not rounded: cheaper to rasterize (no extra antialiased end
  // circle per line) and closer to the real dial's flat rectangular marks.
  dsc.round_start = dsc.round_end = false;
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
  SecondHandGeom g = second_hand_geom(cx, cy, R, angle_deg);

  lv_draw_line_dsc_t dsc;
  lv_draw_line_dsc_init(&dsc);
  dsc.color = lv_color_make(color.r, color.g, color.b);
  dsc.width = g.width;
  dsc.round_start = dsc.round_end = true;
  // Shaft from the counterweight tail, through the pivot, out to the ball -
  // one line so the two halves never show a butt-join at the centre.
  dsc.p1 = g.tail;
  dsc.p2 = g.ball;
  lv_draw_line(layer, &dsc);

  lv_draw_rect_dsc_t dot;
  lv_draw_rect_dsc_init(&dot);
  dot.radius = LV_RADIUS_CIRCLE;
  dot.bg_color = dsc.color;
  dot.bg_opa = LV_OPA_COVER;
  lv_area_t area = {(int32_t) g.ball.x - g.ball_r, (int32_t) g.ball.y - g.ball_r,
                     (int32_t) g.ball.x + g.ball_r, (int32_t) g.ball.y + g.ball_r};
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
  lv_area_t area;
  if (!text_area(area, text, font, cx, edge_y, align_bottom))
    return;
  lv_draw_label_dsc_t ld;
  lv_draw_label_dsc_init(&ld);
  ld.color = lv_color_make(color.r, color.g, color.b);
  ld.font = font;
  ld.text = text.c_str();
  ld.text_local = 1;  // `text` is a stack-local std::string's buffer
  ld.opa = opa;
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

  Color ink = this->ink_now_();

  // ---- read the clock and work out every hand's angle up front -----------
  int hh, mm, ss;
  uint8_t wday, mday, month;
  uint16_t year;
  bool time_valid = this->now_or_fallback_(hh, mm, ss, wday, mday, month, year);
  float sub = this->sub_second_(ss);
  float elapsed_s = ss + sub;  // 0..60, continuous, real elapsed time this minute

  // Real SBB kinematics: the minute hand only ever holds one of 60 fixed
  // positions - it jumps, it never creeps - and it jumps in the same
  // instant the second hand snaps back to 12 and starts its next sweep
  // (both driven by the same once-a-minute impulse). The hour hand is the
  // one hand that genuinely does creep continuously in the real mechanism.
  float minute_deg = mm * 6.0f;
  float hour_deg = (hh % 12) * 30.0f + minute_deg / 12.0f + (elapsed_s / 60.0f) * 0.5f;
  float second_deg = this->second_angle_deg_(elapsed_s);

  int hour_len = (int) (R * 0.50f), hour_w = std::max(2, R / 14);
  int minute_len = (int) (R * MINUTE_HAND_LEN), minute_w = std::max(2, R / 20);
  int hub_r = std::max(2, R / 16);
  // Flush against the hour ticks at the 2/10 o'clock (temperature) and
  // 4/8 o'clock (date) positions - both sit at exactly +/-60 deg from 12,
  // so their inner ends are at the same distance from the centre either
  // way: R * HOUR_TICK_INNER * cos(60 deg) = R * HOUR_TICK_INNER * 0.5.
  int tick_edge_offset = (int) lroundf(R * HOUR_TICK_INNER * 0.5f);

  std::string temp_text, date_text;
  lv_opa_t temp_opa = LV_OPA_COVER, date_opa = LV_OPA_COVER;
  if (this->show_temperature_) {
    temp_text = this->format_temperature_();
#ifdef USE_SENSOR
    if (this->temperature_sensor_ == nullptr || !this->temperature_sensor_->has_state())
      temp_opa = LV_OPA_40;
#else
    temp_opa = LV_OPA_40;
#endif
  }
  if (this->show_date_) {
    date_text = this->format_date_(time_valid, wday, mday, month, year);
    date_opa = time_valid ? LV_OPA_COVER : LV_OPA_40;
  }

  // ---- this frame's per-shape footprints, computed before anything is
  // drawn. Tracked and erased PER SHAPE, not as one combined bounding box
  // - see the block comment above area_union() for why a combined box is
  // unsound here.
  lv_area_t hour_a = bar_hand_area(cx, cy, 0, hour_len, hour_deg, hour_w);
  lv_area_t minute_a = bar_hand_area(cx, cy, 0, minute_len, minute_deg, minute_w);
  lv_area_t hub_a = circle_area(cx, cy, hub_r);
  lv_area_t second_a{0, 0, 0, 0};
  if (this->show_seconds_)
    second_a = second_hand_area(cx, cy, R, second_deg);
  lv_area_t temp_a{0, 0, 0, 0};
  bool has_temp_a =
      this->show_temperature_ &&
      text_area(temp_a, temp_text, this->temperature_font_, cx, cy - tick_edge_offset, true);
  if (has_temp_a)
    temp_a = pad_area(temp_a, 2);
  lv_area_t date_a{0, 0, 0, 0};
  bool has_date_a =
      this->show_date_ &&
      text_area(date_a, date_text, this->date_font_, cx, cy + tick_edge_offset, false);
  if (has_date_a)
    date_a = pad_area(date_a, 2);

  lv_layer_t layer;
  lv_canvas_init_layer(this->obj, &layer);

  // No hand, hub, or text ever reaches past the tick ring, so the dial and
  // the 60 tick lines - the expensive part, and the reason this component
  // used to take the better part of a second per frame - only need
  // (re)drawing on the first frame and after a night_mode flip, not on
  // every render_interval tick. In between, only the small per-shape boxes
  // computed above need erasing and redrawing.
  //
  // Real bug fixed (uneven/"hopping" second hand reported on a 450x450,
  // transparent: true face): `transparent_` alone used to force the FULL
  // path - fill_bg_() to transparent, all 60 ticks, the face circle -
  // every single render_interval tick, not just on the first frame or a
  // night_mode flip. At this canvas's size that's ARGB8888 in PSRAM (see
  // alloc_canvas_buf()'s own fallback warning), and repainting all of
  // that every ~100-200ms can take longer than render_interval itself -
  // the hand's angle is always computed from the real wall clock, not
  // accumulated per frame, so a delayed/irregular render doesn't drift,
  // it *jumps* to catch up next time it finally runs. That's exactly
  // what "hopping" is.
  //
  // The fix: `transparent_` only genuinely needs that full path once.
  // Its entire purpose is punching the round face out of the canvas's
  // square corners (see fill_bg_()'s own comment) - once that's done and
  // `show_face_` is on, every pixel the partial erase below ever touches
  // (well inside the opaque face) is already opaque face fill, not corner
  // transparency. So with a face, the cheap erase-and-redraw path is
  // exactly as correct for a transparent canvas as it already was for an
  // opaque one - only `transparent_ && !show_face_` (ticks/hands floating
  // directly on a transparent background, no face circle at all - a real
  // but currently unused combination) still needs a full redraw every
  // frame, since there the erase step would otherwise paint an opaque
  // patch where the background is supposed to show through.
  bool full_redraw = this->ring_dirty_ || (this->transparent_ && !this->show_face_);
  lv_area_t touched{0, 0, 0, 0};  // union of every rect actually repainted this frame
  bool has_touched = false;

  if (full_redraw) {
    this->fill_bg_(&layer);
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
    this->ring_dirty_ = false;
  } else {
    // Erase each shape's own box (unioned with where that SAME shape was
    // last frame, so its old position gets cleared too) separately - a
    // plain flat rect per shape, not a circular fill or one combined box.
    // The area under each is always a uniform solid color (the face fill
    // or the background), so a rect reproduces it exactly; see the block
    // comment above area_union() for why this has to stay per-shape.
    Color under = this->show_face_ ? this->face_color_() : this->corner_color_();
    lv_draw_rect_dsc_t erase_dsc;
    lv_draw_rect_dsc_init(&erase_dsc);
    erase_dsc.radius = 0;  // plain rect, not a disc
    erase_dsc.bg_color = lv_color_make(under.r, under.g, under.b);
    erase_dsc.bg_opa = LV_OPA_COVER;

    auto erase_shape = [&](lv_area_t &prev, const lv_area_t &cur) {
      lv_area_t e = area_union(prev, cur);
      lv_draw_rect(&layer, &erase_dsc, &e);
      touched = has_touched ? area_union(touched, e) : e;
      has_touched = true;
    };
    erase_shape(this->last_dirty_.hour, hour_a);
    erase_shape(this->last_dirty_.minute, minute_a);
    if (this->show_seconds_)
      erase_shape(this->last_dirty_.second, second_a);
    erase_shape(this->last_dirty_.hub, hub_a);
    if (has_temp_a)
      erase_shape(this->last_dirty_.temp_text, temp_a);
    if (has_date_a)
      erase_shape(this->last_dirty_.date_text, date_a);
  }
  // `transparent: true` clears to fully transparent via a dedicated
  // whole-canvas op (lv_canvas_fill_bg with LV_OPA_TRANSP) - a plain
  // lv_draw_rect can't "erase to transparent" the same way a normal alpha
  // blend would just draw nothing. That's exactly why `transparent_ &&
  // !show_face_` (see full_redraw's own comment above) still has to stay
  // on the full-redraw path every frame - with a face, though, the erase
  // step above never needs to touch a transparent pixel at all, so it's
  // exactly as cheap as the always-opaque case.

  // Drawn before the hands, like the printed lines on a real dial - so the
  // hands sweep over the text, never under it.
  if (this->show_temperature_)
    this->draw_text_(&layer, temp_text, this->temperature_font_, cx, cy - tick_edge_offset,
                      /*align_bottom=*/true, ink, temp_opa);
  if (this->show_date_)
    this->draw_text_(&layer, date_text, this->date_font_, cx, cy + tick_edge_offset,
                      /*align_bottom=*/false, ink, date_opa);

  this->draw_bar_hand_(&layer, cx, cy, 0, hour_len, hour_deg, hour_w, ink);
  this->draw_bar_hand_(&layer, cx, cy, 0, minute_len, minute_deg, minute_w, ink);
  if (this->show_seconds_)
    this->draw_second_hand_(&layer, cx, cy, R, second_deg, this->second_color_());
  this->draw_hub_(&layer, cx, cy, hub_r, ink);

  lv_canvas_finish_layer(this->obj, &layer);

  // Nothing from before a full repaint survives it, so this frame's own
  // per-shape footprint is the whole story for next time - not a union
  // with whatever was dirty before the repaint.
  this->last_dirty_.hour = hour_a;
  this->last_dirty_.minute = minute_a;
  if (this->show_seconds_)
    this->last_dirty_.second = second_a;
  this->last_dirty_.hub = hub_a;
  if (has_temp_a)
    this->last_dirty_.temp_text = temp_a;
  if (has_date_a)
    this->last_dirty_.date_text = date_a;

  if (full_redraw) {
    // Whole widget changed - let LVGL work out its own absolute coords,
    // exactly as this component always has. Kept as the plain whole-object
    // call specifically because it stays correct even if this runs before
    // LVGL has fully settled the widget's on-screen position (e.g. very
    // early after boot) - lv_obj_invalidate() re-derives that from the
    // object itself every time, nothing here needs to guess it.
    lv_obj_invalidate(this->obj);
  } else if (has_touched) {
    // Translate `touched` (canvas-local pixel coords - the union of every
    // shape's erase rect this frame) to the absolute screen coords
    // lv_obj_invalidate_area() expects (the same system lv_obj_get_coords()
    // reports), and invalidate only that - not the whole widget every
    // frame. On a small 240x240 dial this barely matters; on a canvas
    // scaled up for a large panel this is what keeps the SPI/parallel
    // flush to the physical display proportional to what actually changed
    // instead of the widget's full size. Safe to rely on get_coords() here
    // (unlike the full-redraw branch above): this path only ever runs
    // after at least one full-redraw frame has already completed, by
    // which point LVGL's layout has settled. Over-covering this box a
    // little (it's the union of several separate rects, not a tight
    // shape) is harmless for invalidate specifically - unlike for the
    // erase step above, painting a few already-correct pixels again
    // doesn't create any visible artifact.
    lv_area_t obj_coords;
    lv_obj_get_coords(this->obj, &obj_coords);
    lv_area_t abs_area = {touched.x1 + obj_coords.x1, touched.y1 + obj_coords.y1,
                           touched.x2 + obj_coords.x1, touched.y2 + obj_coords.y1};
    lv_obj_invalidate_area(this->obj, &abs_area);
  }
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

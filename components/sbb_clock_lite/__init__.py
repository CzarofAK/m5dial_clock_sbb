"""sbb_clock_lite - canvas-free, dark-only SBB station clock for LVGL.

Same face as `sbb_clock`, but without the canvas pixel buffer (115 kB at
240x240 RGB565): draws straight into LVGL's render buffer from an
LV_EVENT_DRAW_MAIN callback. Meant for boards without PSRAM (M5Dial).
"""

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import sensor, time
from esphome.components.lvgl.defines import CONF_MAIN
from esphome.components.lvgl.lv_validation import lv_font
from esphome.components.lvgl.lvcode import lv_add, lv_expr
from esphome.components.lvgl.types import LvCompound, LvType
from esphome.components.lvgl.widgets import Widget, WidgetType
from esphome.const import CONF_TIME_ID

CODEOWNERS = ["@CzarofAK"]
DEPENDENCIES = ["lvgl"]

# Bare marker key (`sbb_clock_lite:` at top level) so ESPHome imports this
# module and registers the widget type before `lvgl:` validates.
CONFIG_SCHEMA = cv.Schema({})


async def to_code(config):
    pass


ns = cg.esphome_ns.namespace("sbb_clock_lite")
SbbClockLite = ns.class_("SbbClockLite", cg.Component, LvCompound)
lv_sbb_clock_lite_t = LvType(str(SbbClockLite), parents=(LvCompound, cg.Component))
ColorStruct = cg.esphome_ns.struct("Color")

CONF_FOREGROUND = "foreground"
CONF_SECOND_HAND_COLOR = "second_hand_color"
CONF_SHOW_SECONDS = "show_seconds"
CONF_SECOND_HAND_SWEEP = "second_hand_sweep"
CONF_RENDER_INTERVAL = "render_interval"
CONF_SHOW_TICKS = "show_ticks"
CONF_SHOW_RING = "show_ring"
CONF_SHOW_DATE = "show_date"
CONF_DATE_FONT = "date_font"
CONF_SHOW_TEMPERATURE = "show_temperature"
CONF_TEMPERATURE_SENSOR_ID = "temperature_sensor_id"
CONF_TEMPERATURE_FONT = "temperature_font"


def _validate(config):
    if config[CONF_SHOW_TEMPERATURE] and CONF_TEMPERATURE_SENSOR_ID not in config:
        raise cv.Invalid("show_temperature needs a temperature_sensor_id to read from")
    return config


WIDGET_SCHEMA = cv.Schema(
    {
        cv.Required(CONF_TIME_ID): cv.use_id(time.RealTimeClock),
        cv.Optional(CONF_SHOW_SECONDS, default=True): cv.boolean,
        cv.Optional(CONF_SECOND_HAND_SWEEP, default="58s"): cv.All(
            cv.positive_time_period_seconds,
            cv.Range(min=cv.TimePeriod(seconds=1), max=cv.TimePeriod(seconds=59)),
        ),
        cv.Optional(CONF_RENDER_INTERVAL, default="100ms"): cv.positive_time_period_milliseconds,
        cv.Optional(CONF_FOREGROUND): cv.use_id(ColorStruct),
        cv.Optional(CONF_SECOND_HAND_COLOR): cv.use_id(ColorStruct),
        cv.Optional(CONF_SHOW_TICKS, default=True): cv.boolean,
        cv.Optional(CONF_SHOW_RING, default=True): cv.boolean,
        cv.Optional(CONF_SHOW_DATE, default=True): cv.boolean,
        cv.Optional(CONF_DATE_FONT, default="montserrat_16"): lv_font,
        cv.Optional(CONF_SHOW_TEMPERATURE, default=False): cv.boolean,
        cv.Optional(CONF_TEMPERATURE_SENSOR_ID): cv.use_id(sensor.Sensor),
        cv.Optional(CONF_TEMPERATURE_FONT, default="montserrat_16"): lv_font,
    }
).add_extra(_validate)


class SbbClockLiteWidgetType(WidgetType):
    def __init__(self):
        super().__init__(
            "sbb_clock_lite",
            lv_sbb_clock_lite_t,
            (CONF_MAIN,),
            schema=WIDGET_SCHEMA,
            modify_schema={},
            lv_name="obj",
        )

    async def obj_creator(self, parent, config: dict):
        return lv_expr.call("obj_create", parent)

    def get_uses(self):
        return ("label",)

    async def to_code(self, w: Widget, config):
        # Compound widgets aren't registered as components by LVGL itself.
        await cg.register_component(w.var, config)
        lv_add(w.var.set_time(await cg.get_variable(config[CONF_TIME_ID])))
        lv_add(w.var.set_show_seconds(config[CONF_SHOW_SECONDS]))
        lv_add(w.var.set_second_hand_sweep_s(config[CONF_SECOND_HAND_SWEEP].total_seconds))
        lv_add(w.var.set_render_interval(config[CONF_RENDER_INTERVAL].total_milliseconds))
        lv_add(w.var.set_show_ticks(config[CONF_SHOW_TICKS]))
        lv_add(w.var.set_show_ring(config[CONF_SHOW_RING]))
        lv_add(w.var.set_show_date(config[CONF_SHOW_DATE]))
        lv_add(w.var.set_date_font(await lv_font.process(config[CONF_DATE_FONT])))
        lv_add(w.var.set_show_temperature(config[CONF_SHOW_TEMPERATURE]))
        lv_add(w.var.set_temperature_font(await lv_font.process(config[CONF_TEMPERATURE_FONT])))
        if (fg := config.get(CONF_FOREGROUND)) is not None:
            lv_add(w.var.set_foreground(await cg.get_variable(fg)))
        if (sc := config.get(CONF_SECOND_HAND_COLOR)) is not None:
            lv_add(w.var.set_second_hand_color(await cg.get_variable(sc)))
        if (tid := config.get(CONF_TEMPERATURE_SENSOR_ID)) is not None:
            lv_add(w.var.set_temperature_sensor(await cg.get_variable(tid)))


SbbClockLiteWidgetType()

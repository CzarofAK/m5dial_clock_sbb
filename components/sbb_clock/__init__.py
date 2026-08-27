import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import sensor, time
from esphome.components.lvgl.defines import CONF_MAIN, literal
from esphome.components.lvgl.lv_validation import lv_font
from esphome.components.lvgl.lvcode import lv, lv_add, lv_expr
from esphome.components.lvgl.types import LvCompound, LvType
from esphome.components.lvgl.widgets import Widget, WidgetType
from esphome.const import CONF_HEIGHT, CONF_TIME_ID, CONF_WIDTH

CODEOWNERS = ["@CzarofAK"]
# A bare marker key: this only exists so ESPHome imports this module (a
# component's Python only loads when its domain is a top-level YAML key),
# which registers the `sbb_clock` widget type below before `lvgl:` validates
# its `widgets:` list. The clock itself lives under `lvgl: widgets: -
# sbb_clock: ...`.
DEPENDENCIES = ["lvgl"]

CONFIG_SCHEMA = cv.Schema({})


async def to_code(config):
    pass


sbb_clock_ns = cg.esphome_ns.namespace("sbb_clock")
SbbClock = sbb_clock_ns.class_("SbbClock", cg.Component, LvCompound)
# Same fully-qualified type as `SbbClock` above, so `cv.use_id(SbbClock)`
# would resolve against widget ids declared with this type. `cg.Component`
# has to be listed too: only a declared id whose type inherits_from Component
# gets added to CORE.component_ids, and only then does
# cg.register_component() below accept it - WidgetType.create_to_code()
# never registers compound widgets as components on its own, so without this
# SbbClock::setup()/loop() would simply never run.
lv_sbb_clock_t = LvType(str(SbbClock), parents=(LvCompound, cg.Component))
lv_draw_buf_t = LvType("lv_draw_buf_t")
ColorStruct = cg.esphome_ns.struct("Color")

CONF_FOREGROUND = "foreground"  # the "ink": hands, ticks, date/temp text
CONF_BACKGROUND = "background"  # behind everything
CONF_SECOND_HAND_COLOR = "second_hand_color"  # independent of foreground/night_mode
CONF_TRANSPARENT = "transparent"  # clear to transparent instead of background
CONF_SHOW_SECONDS = "show_seconds"
CONF_SECOND_HAND_SWEEP = "second_hand_sweep"
CONF_RENDER_INTERVAL = "render_interval"
CONF_SHOW_TICKS = "show_ticks"
CONF_SHOW_FACE = "show_face"
CONF_FACE_COLOR = "face_color"
CONF_NIGHT_MODE = "night_mode"
CONF_SHOW_DATE = "show_date"
CONF_DATE_FONT = "date_font"
CONF_SHOW_TEMPERATURE = "show_temperature"
CONF_TEMPERATURE_SENSOR_ID = "temperature_sensor_id"
CONF_TEMPERATURE_FONT = "temperature_font"
CONF_DRAW_BUF_ID = "draw_buf_id"

# Verified against SBB/Mondaine's own "Stop2Go" explanation: the second hand
# sweeps the full dial in 58s, then parks at 12 for 2s while the real clock
# waits for the next minute's electrical impulse - the minute hand jumps
# during that pause. Kept configurable in case a build wants to exaggerate or
# soften the pause, but the default is the real, documented value.
_SWEEP_MIN_S = 1
_SWEEP_MAX_S = 59
_SWEEP_DEFAULT_S = "58s"


def _validate(config):
    if config[CONF_SHOW_TEMPERATURE] and CONF_TEMPERATURE_SENSOR_ID not in config:
        raise cv.Invalid(
            "show_temperature needs a temperature_sensor_id to read from"
        )
    return config


WIDGET_SCHEMA = (
    cv.Schema(
        {
            cv.Required(CONF_WIDTH): cv.int_range(min=1),
            cv.Required(CONF_HEIGHT): cv.int_range(min=1),
            cv.Required(CONF_TIME_ID): cv.use_id(time.RealTimeClock),
            cv.GenerateID(CONF_DRAW_BUF_ID): cv.declare_id(lv_draw_buf_t),
            cv.Optional(CONF_SHOW_SECONDS, default=True): cv.boolean,
            cv.Optional(
                CONF_SECOND_HAND_SWEEP, default=_SWEEP_DEFAULT_S
            ): cv.All(
                cv.positive_time_period_seconds,
                cv.Range(
                    min=cv.TimePeriod(seconds=_SWEEP_MIN_S),
                    max=cv.TimePeriod(seconds=_SWEEP_MAX_S),
                ),
            ),
            cv.Optional(
                CONF_RENDER_INTERVAL, default="100ms"
            ): cv.positive_time_period_milliseconds,
            cv.Optional(CONF_FOREGROUND): cv.use_id(ColorStruct),
            cv.Optional(CONF_BACKGROUND): cv.use_id(ColorStruct),
            cv.Optional(CONF_SECOND_HAND_COLOR): cv.use_id(ColorStruct),
            # ARGB8888 canvas so widgets placed behind the clock show through
            # the gaps - not needed just to layer date/temp text any more
            # (this widget now draws those itself, under the hands, in the
            # same pass), only for compositing over something else entirely.
            cv.Optional(CONF_TRANSPARENT, default=False): cv.boolean,
            cv.Optional(CONF_SHOW_TICKS, default=True): cv.boolean,
            cv.Optional(CONF_SHOW_FACE, default=False): cv.boolean,
            cv.Optional(CONF_FACE_COLOR): cv.use_id(ColorStruct),
            # Swaps foreground/background (dial, ticks, hour+minute hands,
            # date/temp text). The second hand keeps its own colour either
            # way - see set_second_hand_color / second_color_() in the .h.
            cv.Optional(CONF_NIGHT_MODE, default=False): cv.boolean,
            cv.Optional(CONF_SHOW_DATE, default=False): cv.boolean,
            cv.Optional(CONF_DATE_FONT, default="montserrat_16"): lv_font,
            cv.Optional(CONF_SHOW_TEMPERATURE, default=False): cv.boolean,
            cv.Optional(CONF_TEMPERATURE_SENSOR_ID): cv.use_id(sensor.Sensor),
            cv.Optional(CONF_TEMPERATURE_FONT, default="montserrat_16"): lv_font,
        }
    ).add_extra(_validate)
)


class SbbClockWidgetType(WidgetType):
    def __init__(self):
        super().__init__(
            "sbb_clock",
            lv_sbb_clock_t,
            (CONF_MAIN,),
            schema=WIDGET_SCHEMA,
            modify_schema={},
            lv_name="canvas",
        )

    async def obj_creator(self, parent, config: dict):
        return lv_expr.call("canvas_create", parent)

    def get_uses(self):
        # lv_canvas_t structurally embeds an lv_image_dsc_t, and LVGL's image
        # widget in turn hard-requires the label widget to be compiled in -
        # both needed even though we never create `image:`/`label:` widgets
        # ourselves (labels are drawn straight onto the canvas, see
        # draw_text_() in the .cpp).
        return ("canvas", "image", "label")

    async def to_code(self, w: Widget, config):
        # WidgetType.create_to_code() only constructs compound widgets - it
        # doesn't register them as ESPHome components, so without this
        # SbbClock::setup()/loop() (and therefore the render loop) never run.
        await cg.register_component(w.var, config)

        width = config[CONF_WIDTH]
        height = config[CONF_HEIGHT]
        transparent = config[CONF_TRANSPARENT]
        cf = "LV_COLOR_FORMAT_ARGB8888" if transparent else "LV_COLOR_FORMAT_NATIVE"
        draw_buf = cg.new_Pvariable(config[CONF_DRAW_BUF_ID])
        buf_size = literal(f"LV_DRAW_BUF_SIZE({width}, {height}, {cf})")
        # Not lv_malloc_core(): that is PSRAM-first, and this canvas is
        # redrawn every frame. alloc_canvas_buf() prefers internal SRAM.
        canvas_buf = literal(
            f"esphome::sbb_clock::alloc_canvas_buf(LV_DRAW_BUF_SIZE({width}, {height}, {cf}))"
        )
        lv.draw_buf_init(draw_buf, width, height, literal(cf), 0, canvas_buf, literal(buf_size))
        lv.draw_buf_set_flag(draw_buf, literal("LV_IMAGE_FLAGS_MODIFIABLE"))
        lv.canvas_set_draw_buf(w.obj, draw_buf)

        lv_add(w.var.set_canvas_size(width, height))
        lv_add(w.var.set_transparent(transparent))
        lv_add(w.var.set_time(await cg.get_variable(config[CONF_TIME_ID])))
        lv_add(w.var.set_show_seconds(config[CONF_SHOW_SECONDS]))
        lv_add(
            w.var.set_second_hand_sweep_s(config[CONF_SECOND_HAND_SWEEP].total_seconds)
        )
        lv_add(w.var.set_render_interval(config[CONF_RENDER_INTERVAL].total_milliseconds))
        lv_add(w.var.set_show_ticks(config[CONF_SHOW_TICKS]))
        lv_add(w.var.set_show_face(config[CONF_SHOW_FACE]))
        lv_add(w.var.set_night_mode(config[CONF_NIGHT_MODE]))
        lv_add(w.var.set_show_date(config[CONF_SHOW_DATE]))
        lv_add(w.var.set_date_font(await lv_font.process(config[CONF_DATE_FONT])))
        lv_add(w.var.set_show_temperature(config[CONF_SHOW_TEMPERATURE]))
        lv_add(
            w.var.set_temperature_font(await lv_font.process(config[CONF_TEMPERATURE_FONT]))
        )

        if (fg := config.get(CONF_FOREGROUND)) is not None:
            lv_add(w.var.set_foreground(await cg.get_variable(fg)))
        if (bg := config.get(CONF_BACKGROUND)) is not None:
            lv_add(w.var.set_background(await cg.get_variable(bg)))
        if (sc := config.get(CONF_SECOND_HAND_COLOR)) is not None:
            lv_add(w.var.set_second_hand_color(await cg.get_variable(sc)))
        if (fc := config.get(CONF_FACE_COLOR)) is not None:
            lv_add(w.var.set_face_color(await cg.get_variable(fc)))
        if (tid := config.get(CONF_TEMPERATURE_SENSOR_ID)) is not None:
            lv_add(w.var.set_temperature_sensor(await cg.get_variable(tid)))


SbbClockWidgetType()

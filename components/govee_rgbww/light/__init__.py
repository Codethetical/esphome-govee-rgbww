import esphome.codegen as cg
from esphome.components import light, text_sensor
import esphome.config_validation as cv
from esphome.const import (
    CONF_COLD_WHITE_COLOR_TEMPERATURE,
    CONF_CONSTANT_BRIGHTNESS,
    CONF_ID,
    CONF_NUM_LEDS,
    CONF_OUTPUT_ID,
    CONF_VERSION,
    CONF_WARM_WHITE_COLOR_TEMPERATURE,
    ENTITY_CATEGORY_DIAGNOSTIC,
    ICON_NEW_BOX,
)
import esphome.final_validate as fv

CODEOWNERS = ["@jahnathan"]

# Unconditional rather than a callable that inspects the config for a
# `version:` key. The dynamic form works, but it ties this component to
# validation-step internals that move between ESPHome releases, and the whole
# cost of getting it wrong is a couple of KB of flash in builds that don't ask
# for the sensor.
AUTO_LOAD = ["text_sensor"]

CONF_STRIP_ID = "strip_id"
CONF_NUM_BULBS = "num_bulbs"
CONF_WW_CHANNEL = "ww_channel"
CONF_CW_CHANNEL = "cw_channel"
CONF_MAX_POWER = "max_power"
CONF_WHITE_WEIGHT = "white_weight"
CONF_COOL_WHITE_RGB = "cool_white_rgb"

# Identity tint for the synthetic cool white. Deliberately not a guess at
# something flattering: three RGB dies will almost certainly out-shine one warm
# LED, but any invented factor has to be calibrated away by eye regardless, and
# a default that is obviously the identity is easier to reason about than one
# that is plausibly wrong. See DEVELOPMENT.md.
DEFAULT_COOL_WHITE_RGB = (1.0, 1.0, 1.0)

# Hard floor on the power budget. Below this the string reads as broken
# rather than dim, which makes a typo (4% for 40%) look like a hardware
# fault. Mirrored by MIN_MAX_POWER in govee_rgbww_light.h for the runtime
# setter.
MIN_MAX_POWER = 0.10

govee_rgbww_ns = cg.esphome_ns.namespace("govee_rgbww")
GoveeRgbwwLightOutput = govee_rgbww_ns.class_("GoveeRgbwwLightOutput", light.AddressableLight)
WhiteChannel = govee_rgbww_ns.enum("WhiteChannel")

# Kept as plain strings in the config and mapped to the enum in to_code(),
# rather than converted by cv.enum in the schema. MockObj overrides __eq__ to
# build a C++ expression, so two converted values always compare truthy - which
# would silently defeat the distinct-channel check below.
WHITE_CHANNELS = {
    "RED": WhiteChannel.WHITE_CHANNEL_RED,
    "GREEN": WhiteChannel.WHITE_CHANNEL_GREEN,
    "BLUE": WhiteChannel.WHITE_CHANNEL_BLUE,
}


def _validate_distinct_white_channels(config):
    # Only meaningful when the string has a real cool white LED; without
    # cw_channel the cool half never touches the white driver at all.
    if CONF_CW_CHANNEL not in config:
        return config
    if config[CONF_WW_CHANNEL] == config[CONF_CW_CHANNEL]:
        raise cv.Invalid(
            f"{CONF_WW_CHANNEL} and {CONF_CW_CHANNEL} must name different channels of the "
            f"white driver (both are '{config[CONF_WW_CHANNEL].lower()}'). Each white LED is "
            f"wired to its own channel.",
            [CONF_CW_CHANNEL],
        )
    return config


def _validate_cool_white_rgb(value):
    value = cv.ensure_list(cv.percentage)(value)
    if len(value) != 3:
        raise cv.Invalid(
            f"{CONF_COOL_WHITE_RGB} takes exactly three values - the red, green and blue "
            f"levels of the synthetic cool white (got {len(value)})."
        )
    return value


def _validate_cool_white_source(config):
    if CONF_CW_CHANNEL in config and CONF_COOL_WHITE_RGB in config:
        raise cv.Invalid(
            f"{CONF_COOL_WHITE_RGB} tints the cool white synthesized from the RGB dies, so it "
            f"means nothing alongside {CONF_CW_CHANNEL}, which points at a real cool white LED. "
            f"Remove whichever one your string doesn't have.",
            [CONF_COOL_WHITE_RGB],
        )
    return config


def _validate_max_power(value):
    # cv.percentage already rejects anything above 100%; this only adds the
    # lower bound.
    value = cv.percentage(value)
    if value < MIN_MAX_POWER:
        raise cv.Invalid(
            f"max_power must be at least {MIN_MAX_POWER:.0%} (got {value:.0%}) - a "
            f"lower budget makes the string look broken rather than dim."
        )
    return value


CONFIG_SCHEMA = cv.All(
    light.ADDRESSABLE_LIGHT_SCHEMA.extend(
        {
            cv.GenerateID(CONF_OUTPUT_ID): cv.declare_id(GoveeRgbwwLightOutput),
            cv.Required(CONF_STRIP_ID): cv.use_id(light.AddressableLightState),
            cv.Required(CONF_NUM_BULBS): cv.positive_not_null_int,
            cv.Optional(CONF_WW_CHANNEL, default="RED"): cv.one_of(*WHITE_CHANNELS, upper=True),
            # Absent means the bulb has no second white LED, which is what the
            # H7039 turned out to be: the cool end of the slider is then
            # synthesized by driving the RGB dies together. Set it only if
            # bring-up shows a second white channel really is wired.
            cv.Optional(CONF_CW_CHANNEL): cv.one_of(*WHITE_CHANNELS, upper=True),
            # Tints that synthetic cool white and matches its brightness against
            # the warm LED. Three dies at equal duty are neither neutral nor
            # level-matched, so without this the Kelvin slider has a visible
            # color and brightness step at the crossover.
            cv.Optional(CONF_COOL_WHITE_RGB): _validate_cool_white_rgb,
            # Bounds of the Kelvin slider Home Assistant shows. Defaults are the
            # usual pairing for this class of string light, not a datasheet figure -
            # they only affect what the slider is labelled and where the crossover
            # sits, so tune them by eye if your LEDs read differently.
            cv.Optional(CONF_COLD_WHITE_COLOR_TEMPERATURE, default="6500 K"): cv.color_temperature,
            cv.Optional(CONF_WARM_WHITE_COLOR_TEMPERATURE, default="2700 K"): cv.color_temperature,
            # Defaults to true, unlike ESPHome's own cwww/rgbww platforms.
            # Without it the middle of the slider runs both halves of the white
            # mix at full - the warm LED and the synthetic cool white, or two
            # white LEDs on a string that has them - so white output peaks in
            # the middle rather than staying flat, and with a real cool LED
            # white draw doubles there too.
            cv.Optional(CONF_CONSTANT_BRIGHTNESS, default=True): cv.boolean,
            cv.Optional(CONF_MAX_POWER, default="100%"): _validate_max_power,
            cv.Optional(CONF_WHITE_WEIGHT, default=1.0): cv.float_range(min=0.0, max=8.0),
            # Opt-in: exposes GOVEE_RGBWW_VERSION as a diagnostic text sensor, so
            # the running component version is visible in Home Assistant without
            # pulling the device's log. Same reasoning as the dump_config() line -
            # both distribution paths can drift silently - but reachable from a bug
            # report.
            cv.Optional(CONF_VERSION): text_sensor.text_sensor_schema(
                entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
                icon=ICON_NEW_BOX,
            ),
        }
    ),
    _validate_distinct_white_channels,
    _validate_cool_white_source,
    light.validate_color_temperature_channels,
)


def _validate_strip_size(config):
    fconf = fv.full_config.get()
    # Path to the physical light's own config block (strip off the trailing
    # key that points at the `id:` field itself).
    path = fconf.get_path_for_id(config[CONF_STRIP_ID])[:-1]
    strip_config = fconf.get_config_for_path(path)

    if CONF_NUM_LEDS in strip_config:
        expected = config[CONF_NUM_BULBS] * 2
        actual = strip_config[CONF_NUM_LEDS]
        if actual != expected:
            raise cv.Invalid(
                f"'{strip_config.get(CONF_ID)}' must have num_leds set to exactly "
                f"2x num_bulbs ({config[CONF_NUM_BULBS]} bulbs -> {expected} leds), "
                f"but it has num_leds: {actual}",
                [CONF_NUM_BULBS],
            )
    return config


FINAL_VALIDATE_SCHEMA = _validate_strip_size


async def to_code(config):
    strip_state = await cg.get_variable(config[CONF_STRIP_ID])
    cw_channel = (
        WHITE_CHANNELS[config[CONF_CW_CHANNEL]]
        if CONF_CW_CHANNEL in config
        else WhiteChannel.WHITE_CHANNEL_NONE
    )
    cool_r, cool_g, cool_b = config.get(CONF_COOL_WHITE_RGB, DEFAULT_COOL_WHITE_RGB)
    var = cg.new_Pvariable(
        config[CONF_OUTPUT_ID],
        strip_state,
        config[CONF_NUM_BULBS],
        WHITE_CHANNELS[config[CONF_WW_CHANNEL]],
        cw_channel,
        config[CONF_COLD_WHITE_COLOR_TEMPERATURE],
        config[CONF_WARM_WHITE_COLOR_TEMPERATURE],
        config[CONF_CONSTANT_BRIGHTNESS],
        config[CONF_MAX_POWER],
        config[CONF_WHITE_WEIGHT],
        cool_r,
        cool_g,
        cool_b,
    )
    await light.register_light(var, config)
    await cg.register_component(var, config)

    if version_config := config.get(CONF_VERSION):
        sens = await text_sensor.new_text_sensor(version_config)
        cg.add(var.set_version_text_sensor(sens))

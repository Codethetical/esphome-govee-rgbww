import esphome.codegen as cg
from esphome.components import light
import esphome.config_validation as cv
from esphome.const import CONF_ID, CONF_NUM_LEDS, CONF_OUTPUT_ID
import esphome.final_validate as fv

CODEOWNERS = ["@jahnathan"]

CONF_STRIP_ID = "strip_id"
CONF_NUM_BULBS = "num_bulbs"
CONF_WW_CHANNEL = "ww_channel"

govee_rgbww_ns = cg.esphome_ns.namespace("govee_rgbww")
GoveeRgbwwLightOutput = govee_rgbww_ns.class_("GoveeRgbwwLightOutput", light.AddressableLight)
WWChannel = govee_rgbww_ns.enum("WWChannel")

WW_CHANNELS = {
    "RED": WWChannel.WW_CHANNEL_RED,
    "GREEN": WWChannel.WW_CHANNEL_GREEN,
    "BLUE": WWChannel.WW_CHANNEL_BLUE,
}

CONFIG_SCHEMA = light.ADDRESSABLE_LIGHT_SCHEMA.extend(
    {
        cv.GenerateID(CONF_OUTPUT_ID): cv.declare_id(GoveeRgbwwLightOutput),
        cv.Required(CONF_STRIP_ID): cv.use_id(light.AddressableLightState),
        cv.Required(CONF_NUM_BULBS): cv.positive_not_null_int,
        cv.Optional(CONF_WW_CHANNEL, default="RED"): cv.enum(WW_CHANNELS, upper=True),
    }
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
    var = cg.new_Pvariable(
        config[CONF_OUTPUT_ID],
        strip_state,
        config[CONF_NUM_BULBS],
        config[CONF_WW_CHANNEL],
    )
    await light.register_light(var, config)
    await cg.register_component(var, config)

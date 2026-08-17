# esphome-govee-rgbww

An [ESPHome](https://esphome.io/) external component for driving the LED
string salvaged from a Govee H7039 (Outdoor String Lights 2) with a generic
ESP32-based replacement board, and controlling it from Home Assistant like
any other addressable light.

## Hardware background

Each physical "bulb" on the H7039 string is actually **two**
WS2811-compatible addressable driver ICs on a single data line:

- the 1st, 3rd, 5th, ... driver (0-indexed: even) drives the bulb's
  **warm-white** LED
- the 2nd, 4th, 6th, ... driver (0-indexed: odd) drives the bulb's **RGB**
  LEDs

So a string of `N` bulbs is electrically one WS2811 chain of `2N` pixels,
alternating warm-white/RGB/warm-white/RGB/... This component presents that
chain to Home Assistant as `N` individually addressable RGBW pixels ("RGBWW"
in Govee's own marketing language — RGB plus one white channel), so each bulb
can be independently colored and animated the same way you'd animate any
addressable RGBW strip in ESPHome.

**Wiring note:** verify the data line's logic level against your specific
board and strip. If your board's data output is 3.3V and the LED strip runs
at 5V+ logic, you likely need a level shifter on the data line — this is
board/strip-specific and isn't something this repo can determine for you.

## How it works

Two `light:` platforms compose together:

1. **Physical layer** — a normal ESPHome
   [`esp32_rmt_led_strip`](https://esphome.io/components/light/esp32_rmt_led_strip.html)
   entry, `chipset: WS2811`, sized to `2 * num_bulbs` pixels. This does the
   actual WS2811/RMT signal generation — no custom code involved. Mark it
   `internal: true` so it doesn't clutter Home Assistant; you never control
   it directly.
2. **Logical layer** — this repo's `govee_rgbww` platform. It's the light
   entity you actually add to Home Assistant. Internally it owns its own
   RGBW pixel buffer (for effects/animations to write into) and, on every
   update, splits each logical bulb's color into the two physical WS2811
   pixels: RGB bytes go to the odd physical pixel, and the white byte goes to
   one configurable channel of the even physical pixel (since only one of
   that driver's three channels is actually wired to an LED).

See [`components/govee_rgbww/light/govee_rgbww_light.cpp`](components/govee_rgbww/light/govee_rgbww_light.cpp)
for the implementation.

## Usage

```yaml
external_components:
  - source:
      type: local
      path: components # or a github:// source once this repo has a remote

light:
  - platform: esp32_rmt_led_strip
    id: govee_physical_strip
    internal: true
    pin: GPIO16
    chipset: WS2811
    num_leds: 60 # must always be exactly 2x num_bulbs below
    rgb_order: RGB

  - platform: govee_rgbww
    name: None
    strip_id: govee_physical_strip
    num_bulbs: 30
    ww_channel: red
    effects:
      - addressable_rainbow:
      - addressable_color_wipe:
```

See [`examples/h7039.yaml`](examples/h7039.yaml) for a complete, runnable
device config (wifi/api/ota included).

### Using with the Home Assistant ESPHome add-on

The `external_components` `local` path above is resolved **relative to the
YAML file**, not the repo. If you compile straight from this repo (as in
`examples/h7039.yaml`, which lives in a subfolder) that's `../components`.
If you're using the Home Assistant ESPHome add-on/dashboard, your device
YAML normally lives directly in `/config/esphome/`, with no copy of this
repo alongside it — so you need to get the component there yourself:

1. Copy the whole `components/govee_rgbww/` folder (keep that folder name)
   into `/config/esphome/components/`, giving you
   `/config/esphome/components/govee_rgbww/...`.
2. In your device's YAML (e.g. `/config/esphome/govee-outdoor-lights.yaml`),
   point at it as a sibling folder instead:
   ```yaml
   external_components:
     - source:
         type: local
         path: components
   ```
3. Whenever you pull an updated version of this component, re-copy
   `components/govee_rgbww/` into the add-on's `components/` folder — the
   add-on won't see repo changes on its own since there's no git link
   between the two.

### Config reference: `govee_rgbww`

Extends ESPHome's standard
[addressable light schema](https://esphome.io/components/light/index.html#addressable-lights-and-effects)
(so `name`, `effects`, `color_correct`, `default_transition_length`,
`gamma_correct`, etc. all work normally), plus:

| Option       | Required | Default | Description                                                                                      |
| ------------ | -------- | ------- | -------------------------------------------------------------------------------------------------- |
| `strip_id`   | yes      | —       | ID of the physical `esp32_rmt_led_strip` (or similar addressable) light carrying the raw WS2811 chain. |
| `num_bulbs`  | yes      | —       | Number of logical bulbs. The referenced strip's `num_leds` must be exactly `2 * num_bulbs` — checked at compile time. |
| `ww_channel` | no       | `red`   | Which channel (`red`/`green`/`blue`) of the warm-white driver IC the white LED is actually wired to. |

## Bring-up / troubleshooting checklist

These are the things that depend on your specific board and string, and
can't be verified without the hardware in hand:

1. **Data line logic level** — confirm your ESP32 board's data output is
   compatible with the strip; add a level shifter if not.
2. **`rgb_order`** — flash with a solid test color (e.g. pure red) and check
   the RGB LEDs actually show red. If the color is wrong, try the other
   `rgb_order` values on the physical strip entry.
3. **`ww_channel`** — turn the white channel on. If the warm-white LEDs stay
   dark, try `green` or `blue` instead of the default `red`.
4. **Bulb ordering / off-by-one** — run the Addressable Scan effect and watch
   whether exactly one bulb lights at a time in physical order. If every
   other bulb is dark or doubled-up, the warm-white/RGB driver pairing
   assumption may not hold for your specific string — double check with a
   multimeter which physical pixel index corresponds to which driver.

## Home Assistant integration

The `govee_rgbww` entity shows up as one addressable RGBW light in Home
Assistant, with full color, brightness, and effect support — no separate
integration needed beyond the standard ESPHome integration.

## License

MIT — see [LICENSE](LICENSE).

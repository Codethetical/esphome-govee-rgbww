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

**Tested board:** this project targets a generic ESP32 board, but the only
board it's actually been tested on is the Gledopto GL-C-310WL (available from
Amazon). The Govee power supply outputs 36V, which is what the original
driver PCB (and LEDs) expect — it is **not** safe to feed directly into an
ESP32 board. Reuse the Govee supply, but add a step-down (buck) converter to
bring 36V down to whatever the replacement board needs (typically 5V) before
powering the ESP32 module.

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
  - source: components # local checkout of this repo; see alternatives below
    components: [ govee_rgbww ]

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
    max_power: 40% # cap total string draw; see "Power limiting" below
    effects:
      - addressable_rainbow:
      - addressable_color_wipe:
      - addressable_scan:
      - addressable_twinkle:
```

These are just the standard ESPHome addressable effects — since `govee_rgbww`
extends the addressable light schema, any other effect from the
[addressable effects list](https://esphome.io/components/light/index.html#addressable-light-effects)
(e.g. `addressable_flicker`, `addressable_fireworks`) works too.

See [`examples/h7039.yaml`](examples/h7039.yaml) for a complete, runnable
device config (wifi/api/ota included).

### Using with the Home Assistant ESPHome add-on

A `local` `external_components` path is resolved **relative to the YAML
file**, not the repo. If you're using the Home Assistant ESPHome
add-on/dashboard, your device YAML normally lives directly in
`/config/esphome/`, with no copy of this repo alongside it, so a local path
needs some setup. Two ways to handle that:

**Option 1 — pull from GitHub (recommended, no manual copying):**

```yaml
external_components:
  - source: github://Codethetical/esphome-govee-rgbww@v0.9.1
    components: [ govee_rgbww ]
```

The add-on fetches the component straight from this repo at compile time.
The `@v0.9.1` suffix pins it to a released tag, so the component only
changes when you change that line — see [Versioning](#versioning) for how to
pick a version. Use `@main` instead to track the latest development state;
the add-on then re-checks for updates on the `refresh` interval (default
`1d`) and can change under you between builds.

**Option 2 — copy the component in locally:**

1. Copy the whole `components/govee_rgbww/` folder (keep that folder name)
   into `/config/esphome/components/`, giving you
   `/config/esphome/components/govee_rgbww/...`.
2. In your device's YAML (e.g. `/config/esphome/govee-outdoor-lights.yaml`),
   point at it as a sibling folder:
   ```yaml
   external_components:
     - source: components
       components: [ govee_rgbww ]
   ```
3. Whenever you pull an updated version of this component, re-copy
   `components/govee_rgbww/` into the add-on's `components/` folder — the
   add-on won't see repo changes on its own since there's no git link
   between the two. This is what [`examples/h7039.yaml`](examples/h7039.yaml)
   is set up for.

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
| `max_power`  | no       | `100%`  | Total power budget for the string, as a fraction of "every wired channel at full". Minimum `10%`. See [Power limiting](#power-limiting). |
| `white_weight` | no     | `1.0`   | Current draw of a bulb's warm-white LED relative to one RGB die, used when estimating load. Only matters if the two differ. |
| `output_id`  | no       | auto    | Explicit ID for the output object (not the light entity). Needed only if you want to call `set_max_power()` / `get_load_fraction()` from a lambda. |

## Power limiting

Driving all 30 bulbs to full white draws far more than a typical replacement
power supply can deliver — enough to sag the rail, brown out the ESP32, and
put it in a boot loop. `max_power` caps the string's total draw so that
"100%" in Home Assistant means "as bright as this supply can safely go"
rather than "as bright as the LEDs can physically go".

### How it works

Every frame, before it's pushed to the WS2811 chain, the component estimates
the load as the sum of all wired channel duty cycles (R+G+B per bulb, plus
the one live warm-white channel, weighted by `white_weight`). If that total
exceeds the budget, **every channel is scaled by the same factor** so the
result lands exactly on budget.

Two consequences worth understanding:

- **Hue and relative brightness are preserved.** Scaling is uniform across
  the whole string, so a limited scene looks identical to the unlimited one,
  just dimmer. Nothing shifts color and no bulb gets singled out.
- **Limiting only kicks in when you're actually over budget.** A dim scene,
  or a saturated single-color scene like pure red (roughly a quarter of the
  load of white), passes through completely untouched at full brightness.
  You only lose brightness where you'd otherwise brown out.

The trade-off of that second point: the top of the Home Assistant brightness
slider becomes a soft dead zone on white-heavy scenes. Dragging from ~85% to
100% on full white produces little visible change, because the limiter
absorbs the increase. It never gets *dimmer* — it just flattens out. That's
the intended behavior, but it's the part that feels unfamiliar first.

Limiting is applied to the final, post-gamma PWM values, which is the
physically correct place for it — duty cycle is what current tracks.

### Finding your number

`max_power` defaults to `100%` (no limiting), so this is entirely opt-in.
The floor is `10%`; anything lower is rejected at compile time, because a
budget that small makes the string look broken rather than dim.

Rather than reflashing for every guess,
[`examples/h7039.yaml`](examples/h7039.yaml) wires up a template `number`
that retunes the budget live from Home Assistant, and a template `sensor`
reporting what the current frame *wants* to draw as a percentage of the
string's theoretical maximum:

```yaml
number:
  - platform: template
    name: "LED Power Limit"
    min_value: 10
    max_value: 100
    step: 5
    update_interval: 10s
    lambda: return id(govee_light_out).get_max_power() * 100.0f;
    set_action:
      - lambda: id(govee_light_out).set_max_power(x / 100.0f);
```

Note `id(govee_light_out)` refers to the light's **`output_id`**, not its
`id:` — the latter names the Home Assistant light entity, which doesn't have
these methods.

Procedure:

1. Set the light to solid white at 100% brightness — the worst case.
2. Walk the slider down until the brownout/boot loop stops.
3. Subtract ~10% for margin (cold LEDs draw more than warm ones, and mains
   voltage sags).
4. Put that value in `max_power:` in your YAML and reflash. **The YAML value
   is the one that survives a reboot** — the number entity is deliberately
   not `restore_value`, so it can't race the light's own restored state and
   let one full-power frame through at boot.

If white-heavy scenes still brown out at a budget that colored scenes
survive, your warm-white LED draws more than one RGB die: raise
`white_weight` (try `2.0`) and recalibrate.

### Caveats

- **Effects may shimmer slightly.** Because the scale factor is recomputed
  per frame, an effect whose total load fluctuates can make the whole string
  pulse a little. `addressable_rainbow` and `addressable_scan` hold a
  near-constant total and are unaffected; `addressable_fireworks` and
  `addressable_twinkle` are the ones to watch.
- **This bounds steady state, not inrush.** If the brownout happens on a fast
  transition but not at the same sustained level, you need a slower
  `default_transition_length`, not a lower budget. Test with
  `default_transition_length: 0s` and a jump from off to full white.
- **Check the hardware too.** If the original Govee driver ran the string at
  full white without trouble, the real cause is somewhere in the replacement
  wiring — buck converter headroom, connector resistance, ground return —
  and the limiter is masking it rather than fixing it.

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
4. **Power budget** — if the controller boot-loops when you go to full
   white, that's the supply browning out. Set `max_power` and calibrate it
   as described in [Power limiting](#power-limiting).
5. **Bulb ordering / off-by-one** — run the Addressable Scan effect and watch
   whether exactly one bulb lights at a time in physical order. If every
   other bulb is dark or doubled-up, the warm-white/RGB driver pairing
   assumption may not hold for your specific string — double check with a
   multimeter which physical pixel index corresponds to which driver.

## Home Assistant integration

The `govee_rgbww` entity shows up as one addressable RGBW light in Home
Assistant, with full color, brightness, and effect support — no separate
integration needed beyond the standard ESPHome integration.

## Versioning

Releases are [git tags](https://github.com/Codethetical/esphome-govee-rgbww/tags)
of the form `vMAJOR.MINOR.PATCH`, and a tag is the unit you consume: pass it
as the `@ref` of a `github://` source, or check it out before copying
`components/govee_rgbww/` in locally. [CHANGELOG.md](CHANGELOG.md) records
what changed in each one.

Because the only public interface here is the YAML you write,
[Semantic Versioning](https://semver.org/spec/v2.0.0.html) is scoped to
that:

- **MAJOR** — a device YAML that worked before now fails to compile, or the
  string behaves materially differently with the same config. Read the
  changelog before upgrading.
- **MINOR** — new opt-in config keys or capabilities. Existing YAML keeps
  compiling and behaving as it did.
- **PATCH** — bug fixes and documentation only.

Note that a `0.x` version means the config schema may still change in a
MINOR bump while the power-limiting defaults are calibrated against real
hardware measurements.

ESPHome itself is a separate moving target: this component tracks current
ESPHome releases and is not tested against older ones, so upgrade ESPHome
and this component together.

**Which version am I running?** The component logs it at boot, so a device
that was flashed months ago can still identify itself:

```
[C][govee_rgbww:150]: Govee RGBWW Light:
[C][govee_rgbww:150]:   Version: 0.9.1
[C][govee_rgbww:150]:   Bulbs: 30
```

That line is worth checking before reporting a problem — a `@main` source
moves on its own, and a locally copied `components/` folder has no git link
back to this repo to tell you it's gone stale.

## License

MIT — see [LICENSE](LICENSE).

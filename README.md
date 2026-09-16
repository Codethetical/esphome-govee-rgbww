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

**There is one white LED per bulb, not two.** Only one channel of the white
driver IC is wired; the other two drive nothing. So the cool end of the color
temperature range is *synthesized* — the bulb's three RGB dies are driven
together, tinted by [`cool_white_rgb`](#config-reference-govee_rgbww), and
cross-faded against the warm LED. If bring-up shows your string does have a
second white LED on that driver, set `cw_channel` and it is used directly
instead.

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
   pixels: RGB bytes go to the odd physical pixel, and the white byte is
   divided between the warm LED — on one configurable channel of the even
   physical pixel — and the cool half, which is added to the odd pixel's RGB
   bytes. The white driver's unwired channels are written as zero every frame.

### The four-channel ceiling

Each bulb has four emitters: red, green, blue and warm white. The buffer has
four channels too, but they are not the same four — ESPHome's `ESPColorView`,
what every addressable effect writes through, is exactly R/G/B/W, and the
component needs to express a *white mix* rather than a fourth emitter.

So the W byte is a bulb's white **level**, and the warm/cool **balance** is a
single light-wide ratio derived from the color temperature. White stays
per-bulb and animatable; its color temperature does not. This is not an
approximation: both branches of ESPHome's own `current_values_as_cwww()`
factor into `gamma(state × brightness) × ratio`, and only the ratio depends on
temperature — which is exactly the term being cached.

The practical limit: an effect can chase white down the string, but it cannot
make one bulb warm while its neighbour is cool.

Because the cool half of that mix is the RGB dies, color and white cannot be
requested at the same time — see
[Color and white are separate modes](#color-and-white-are-separate-modes). An
effect *can* write both at once, and then the cool contribution is added to
the color and clipped at full scale per channel.

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
    gamma_correct: 1.0 # match the stock brightness curve; see "Brightness curve" below
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
  - source: github://Codethetical/esphome-govee-rgbww@v0.9.2
    components: [ govee_rgbww ]
```

The add-on fetches the component straight from this repo at compile time.
The `@v0.9.2` suffix pins it to a released tag, so the component only
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
| `ww_channel` | no       | `red`   | Which channel (`red`/`green`/`blue`) of the white driver IC the **warm** white LED is wired to. |
| `cw_channel` | no       | —       | Set **only** if your string has a second, cool white LED on the white driver. Absent (the default) means the cool end is synthesized from the RGB dies. Must differ from `ww_channel` — checked at compile time. |
| `cool_white_rgb` | no | `[100%, 100%, 100%]` | Red, green and blue levels of the synthetic cool white, tinting its hue and matching its brightness to the warm LED. Rejected alongside `cw_channel`, which has a real LED instead. See [Calibrating the cool white](#calibrating-the-cool-white). |
| `cold_white_color_temperature` | no | `6500 K` | Cold end of Home Assistant's Kelvin slider. With the synthetic cool white this is a label, not a measurement — the real temperature is whatever the dies and `cool_white_rgb` produce. |
| `warm_white_color_temperature` | no | `2700 K` | Temperature of the warm white LED. Sets the warm end. Must be warmer than the cold one. |
| `constant_brightness` | no | `true` | Hold white output flat across the temperature slider, instead of letting the midpoint run both halves of the white mix at full. See [`constant_brightness`](#constant_brightness). |
| `max_power`  | no       | `100%`  | Total power budget for the string, as a fraction of "every wired channel at full". Minimum `10%`. See [Power limiting](#power-limiting). |
| `white_weight` | no     | `1.0`   | Current draw of the warm white LED relative to one RGB die, used when estimating load. Only matters if the two differ. (With `cw_channel` set it describes each of the two white LEDs.) |
| `output_id`  | no       | auto    | Explicit ID for the output object (not the light entity). Needed only if you want to call `set_max_power()` / `get_load_fraction()` from a lambda. |
| `version`    | no       | —       | If present, creates a diagnostic text sensor reporting the component version. Takes the standard [text sensor options](https://esphome.io/components/text_sensor/index.html) (`name`, `id`, `icon`, …). See [Version sensor](#version-sensor). |

#### Brightness curve

`gamma_correct` is ESPHome's standard light option and defaults to `2.8`,
which maps the brightness slider onto PWM duty as `brightness^2.8`. The stock
Govee controller is close to linear. Measured side by side on pure blue, 50%
on the stock string matched about 75% on this component at the default:

| Slider | Duty at `2.8` | Duty at `1.0` |
| ------ | ------------- | ------------- |
| 100%   | 100%          | 100%          |
| 75%    | 45%           | 75%           |
| 50%    | 14%           | 50%           |
| 25%    | 2%            | 25%           |

The example config sets `gamma_correct: 1.0` so the two track each other
across the whole slider; 100% is identical either way. The component's own
default is left at ESPHome's, so a YAML without the key behaves as it always
has. Move it back toward `2.8` if you prefer ESPHome's finer control at the
dim end over matching the original.

#### Calibrating the cool white

Three RGB dies driven together are neither neutral in hue — they read blue or
green depending on the emitters — nor brightness-matched to a single warm LED,
which they will almost certainly out-shine. `cool_white_rgb` corrects both, and
it is set by eye:

1. Put the Kelvin slider at the **cold** end. Pull the three values down until
   the string reads as white rather than tinted. Reducing red and green
   together lowers the level; their ratio to blue sets the hue.
2. Put the slider at the **warm** end, then move between the two. The two ends
   should look about equally bright. If cool is still the brighter, scale all
   three values down together — that preserves the hue you just set.
3. Check the middle of the slider last. With `constant_brightness: true` it
   should not be noticeably brighter or dimmer than either end.

Since it is a compile-time key, each iteration costs a flash cycle. Getting
the hue right at step 1 first, and only then scaling for brightness, keeps the
number of cycles down.

#### Version sensor

The version that `dump_config()` prints can also be exposed as a Home
Assistant entity, so you can check what a device is actually running without
pulling its log:

```yaml
  - platform: govee_rgbww
    # ... rest of the config ...
    version:
      name: "Component Version"
```

That produces a diagnostic text sensor whose state is the component version
string, e.g. `0.9.2`. It is published once at boot and never changes — it is a
compile-time constant, not a runtime reading.

This is worth adding if you track this repo with `@main`, or if you keep a
copied `components/govee_rgbww/` folder in your add-on config, since neither
of those leaves any other trace of which version a device was built from.

## Power limiting

Driving all 30 bulbs to full white draws far more than a typical replacement
power supply can deliver — enough to sag the rail, brown out the ESP32, and
put it in a boot loop. `max_power` caps the string's total draw so that
"100%" in Home Assistant means "as bright as this supply can safely go"
rather than "as bright as the LEDs can physically go".

### How it works

Every frame, before it's pushed to the WS2811 chain, the component estimates
the load as the sum of all wired channel duty cycles (R+G+B per bulb, plus
the live warm-white channel, weighted by `white_weight`). It is measured from
the bytes that are about to go out, so the synthetic cool white is counted
where it actually draws — in the RGB total — and a channel that hit full scale
counts once, not twice. If the total exceeds the budget, **every channel is
scaled by the same factor** so the result lands exactly on budget.

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
`white_weight` (try `2.0`) and recalibrate. Note that the cold end of the
Kelvin slider is the RGB dies at full, so it draws like a saturated white
scene rather than like the warm LED — `white_weight` does not affect it.

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
3. **`ww_channel`** — drag the Kelvin slider to the warm end. It should light
   only the white LEDs, with the RGB dies dark. `red` is confirmed on the
   string this was developed against; if white stays dark, try `green` and
   `blue`.
4. **How many white LEDs your string has** — the cold end of the slider drives
   the RGB dies together, so it should look white-ish and clearly involve the
   color emitters. If your string instead has a *second* white LED on another
   channel of the white driver, set `cw_channel` to it and the RGB dies stay
   out of the white mix entirely.
   [`examples/bringup.yaml`](examples/bringup.yaml) drives the raw WS2811 chain
   one pixel and channel at a time to settle this without involving the
   component at all.
5. **`cool_white_rgb`** — with the synthetic cool white, expect the cold end to
   be tinted and too bright out of the box. Calibrate as described in
   [Calibrating the cool white](#calibrating-the-cool-white).
6. **Power budget** — if the controller boot-loops when you go to full
   white, that's the supply browning out. Set `max_power` and calibrate it
   as described in [Power limiting](#power-limiting).
7. **Brightness compared to the stock controller** — if every mode is dimmer
   than the original at the same slider position but matches at 100%, that
   is `gamma_correct`, not the hardware; see
   [Brightness curve](#brightness-curve). If a pure color at 100% is *also*
   dimmer, nothing in software reduces that frame — look at the supply and
   wiring instead.
8. **Bulb ordering / off-by-one** — run the Addressable Scan effect and watch
   whether exactly one bulb lights at a time in physical order. If every
   other bulb is dark or doubled-up, the warm-white/RGB driver pairing
   assumption may not hold for your specific string — double check with a
   multimeter which physical pixel index corresponds to which driver.

## Home Assistant integration

The `govee_rgbww` entity shows up as one addressable light in Home Assistant,
with full color, brightness, and effect support — no separate integration
needed beyond the standard ESPHome integration.

### Color and white are separate modes

The entity advertises two mutually exclusive color modes, `rgb` and
`color_temp`, rather than a single combined `rgbww` mode. Home Assistant's
more-info dialog shows a color wheel plus a Kelvin slider: picking a color
turns the white mix off, and moving the slider takes the RGB dies over.
Effects still drive all four buffer channels at once — the split only applies
to plain color/brightness control.

With the default, synthetic cool white this is not a preference: the RGB dies
are half of the white mix, so they cannot also show a requested color in the
same frame. On a string with a real cool white LED it stays the right shape
anyway — the white LEDs and the RGB dies are different emitters on different
driver ICs, not white channels inside an RGBWW package, and mixing them is not
a calibrated operation. It's the same shape as ESPHome's own `rgbww` platform
with `color_interlock: true`.

It also avoids a wash-out that the combined mode causes. With `rgbww`, Home
Assistant converts its stored RGB swatches and presets through the
[`color_rgb_to_rgbw()`](https://github.com/home-assistant/core/blob/dev/homeassistant/util/color.py)
family, which takes white as `min(r, g, b)` and then rescales the result so
its maximum matches the input's. For any color less than roughly 50% saturated
the white component *is* that maximum, so the rescale pins it to
`max(r, g, b)` — 255 for a full-value pastel. Light pink `(255, 182, 193)`
arrives as `(102, 0, 15, 255)`. Since the white mix is much brighter than a
single RGB die, it buries the color you asked for. Fully saturated hues have
`min(r, g, b) == 0` and are unaffected, which is why only *some* presets
washed out.

If you want white layered underneath a color, run an effect — effects have
unrestricted access to the RGBW buffer.

#### `constant_brightness`

The Kelvin slider works by cross-fading the two halves of the white mix — the
warm LED against the synthetic cool white, or against a real cool LED if your
string has one. Normalised the way ESPHome does it, the midpoint leaves *both*
at full, so white output peaks in the middle of the slider and falls off
toward either end:

```
constant_brightness: false          constant_brightness: true (default here)
  cold \           / warm             cold \____/ warm
        \  2x   /                           1x
         \    /                       white output flat across the range
       midpoint = worst case
```

This component defaults it to `true`, unlike ESPHome's stock `cwww`/`rgbww`
platforms, because a hump in the middle of a slider is exactly the kind of
surprise [`max_power`](#power-limiting) exists to prevent on a string this
long. The cost is roughly half the peak white brightness at mid slider. Set it
to `false` if you would rather have the brightness and let the power limiter
scale things back.

The power model follows the setting where the setting changes the ceiling —
which is only when `cw_channel` is set: two real white LEDs draw one LED's
worth with `constant_brightness: true` and two with it off. With the synthetic
cool white the ceiling is unchanged either way, because that half draws through
the RGB driver, which is already counted at full.

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
[C][govee_rgbww:xxx]: Govee RGBWW Light:
[C][govee_rgbww:xxx]:   Version: 0.9.2
[C][govee_rgbww:xxx]:   Bulbs: 30
[C][govee_rgbww:xxx]:   WW Channel: RED (2700K)
[C][govee_rgbww:xxx]:   CW Channel: GREEN (6500K)
```

Add the [`version:`](#version-sensor) key to get the same string as a Home
Assistant entity, which is easier to quote in a bug report than a boot log.

Either way it is worth checking before reporting a problem — a `@main` source
moves on its own, and a locally copied `components/` folder has no git link
back to this repo to tell you it's gone stale.

## License

MIT — see [LICENSE](LICENSE).

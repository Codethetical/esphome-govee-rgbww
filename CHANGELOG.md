# Changelog

All notable changes to this component are documented here. The format is
based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and this
project follows [Semantic Versioning](https://semver.org/spec/v2.0.0.html)
as scoped in the [Versioning](README.md#versioning) section of the README.

## [Unreleased]

## [0.9.2] - 2026-09-15

### Added

- Color temperature control. `cold_white_color_temperature` /
  `warm_white_color_temperature` (default `6500 K` / `2700 K`) set the bounds
  of a Kelvin slider in Home Assistant, and the component cross-fades the
  bulb's warm white LED against a cool white it synthesizes by driving the
  three RGB dies together.

  The bulbs have one white LED, not two: only one channel of the white driver
  IC is wired. Synthesizing the cool end is what makes a temperature slider
  possible at all on that hardware.

- `cool_white_rgb` (default `[100%, 100%, 100%]`), the red, green and blue
  levels of that synthetic cool white. Three dies together are neither neutral
  in hue nor brightness-matched to one warm LED, so this tints and levels them;
  without it the slider has a visible color and brightness step at the
  crossover. The default is the identity — expect to calibrate it by eye, as
  described under "Calibrating the cool white" in the README.

- `cw_channel`, for strings that *do* wire a second, cool white LED onto the
  white driver. Setting it routes the cool half there and keeps the RGB dies
  out of the white mix entirely. Absent by default; `cool_white_rgb` is
  rejected alongside it, since a real LED has no mix to tune.

- `constant_brightness` (default `true`), holding white output flat across the
  temperature slider. Defaults the opposite way to ESPHome's stock
  `cwww`/`rgbww` platforms: without it the midpoint of the slider runs both
  halves of the mix at full, so white peaks exactly where nobody would look
  for it — and with `cw_channel` set, white draw doubles there too.

- `examples/bringup.yaml`, a hardware bring-up harness that drives the raw
  WS2811 chain directly and deliberately does not load `govee_rgbww`. Selects
  for target (all / even / odd / single index) and channel, plus an Apply
  button, so the string's real structure — drivers per bulb, which parity
  carries the white driver, which of its channels are wired, and how many
  white LEDs there actually are — can be established without depending on
  anything the component assumes. This is what established that there is one
  white LED per bulb.

- Optional `version:` key on the `govee_rgbww` light platform, creating a
  diagnostic text sensor whose state is the component version. Published once
  at boot. Makes the version visible in the Home Assistant UI rather than only
  in the device's boot log, which matters for the two distribution paths that
  leave no other record of what a device was built from: a `@main` source and
  a hand-copied `components/` folder.

### Changed

- `examples/h7039.yaml` and the README usage snippet now set
  `gamma_correct: 1.0`. ESPHome's default of `2.8` made every brightness step
  below 100% noticeably dimmer than the stock Govee controller, whose scale is
  close to linear in PWM duty — measured side by side on pure blue, 50% on the
  stock string matched about 75% here. The component's own default is
  untouched, so an existing YAML without the key behaves exactly as before;
  add the key to match the original string.

- The light now advertises `RGB` and `COLD_WARM_WHITE` as two mutually
  exclusive color modes instead of the combined `RGB_WHITE`. In Home Assistant
  that means a color wheel plus a Kelvin slider: picking a color turns the
  white mix off, and vice versa. Effects are unaffected and still drive all
  four buffer channels at once.

  With the synthetic cool white the exclusivity is structural — the RGB dies
  cannot show a requested color and carry half the white mix in the same
  frame. It also fixes preset/swatch colors washing out: Home Assistant stores
  those as plain RGB and, for an `rgbw`/`rgbww` light, converts them with
  `color_rgb_to_rgbw()`, which rescales the tuple so its maximum matches the
  input's. For any color under roughly 50% saturation that pins the white
  channel to `max(r, g, b)` — 255 for a full-value pastel — and the white then
  buries the requested color.

- The power model measures load from the bytes about to be written to the
  chain rather than re-deriving it from the buffer, so the synthetic cool
  white is counted where it actually draws (the RGB driver) and a channel that
  saturates is counted once rather than twice.

  A consequence worth knowing when upgrading: with the default configuration
  the modelled ceiling per bulb is `3*255 + 255*white_weight` and no longer
  depends on `constant_brightness`, because the cool half draws through the
  RGB driver, which was always counted in full. That is the same expression
  the previous ceiling used with `constant_brightness: true`, so an existing
  `max_power` calibration carries over unchanged.

- The `WWChannel` enum is now `WhiteChannel` (`WHITE_CHANNEL_RED`, …), since it
  selects the channel for either white LED. Its values double as `Color::raw`
  indices, which replaced the per-channel `switch` in the output path. YAML keys
  are unaffected.

### Fixed

- The white driver's unwired channels are now written explicitly as 0 every
  frame instead of relying on them never having been touched.

- `update_state()` and the default transition now zero the channels that
  don't belong to the active color mode. `LightCall` leaves fields from other
  modes at their previous values and the addressable path doesn't mask them,
  so without this the split above would have left the old white level lit
  under a new color, and the old color lit under white.

- The cold/warm split is now recomputed on the transition path as well as in
  `update_state()`. ESPHome only calls `update_state()` when a transformer
  hands back `LightColorValues`, and `AddressableLightTransformer` writes the
  frame buffer directly and returns nothing — so with the default
  `default_transition_length: 1s` it was never called at all, and the mix
  stayed pinned to the neutral 50/50 balance the constructor seeds. The
  temperature now lands at the start of a fade while the white level still
  fades.

- The default transition sets the white level explicitly in `COLD_WARM_WHITE`
  mode instead of inheriting it from `LightColorValues::white_`, a field that
  belongs to `ColorMode::WHITE` and only happened to hold a usable value
  because nothing in this component's trait set ever writes it.

- The split is now derived from `color_temperature` rather than from
  `LightColorValues`' `cold_white`/`warm_white` pair. Those two fields arrive
  in two different conventions and carry no marker saying which one you got:
  ESPHome writes them gamma-*un*corrected when it derives them from a
  temperature itself, while Home Assistant sends them as plain linear
  fractions — and sending them suppresses ESPHome's own derivation, so both
  reach this component through the same fields. Correcting Home Assistant's
  values a second time crushed the dimmer of the two halves toward zero across
  most of the slider. `color_temperature` is set on every path that can change
  the white balance and has one unambiguous meaning.

  Trade-off: a `light.control` with explicit `cold_white:` / `warm_white:` no
  longer moves the split, because it leaves `color_temperature` untouched. Use
  `color_temperature:` instead.

### Changed (diagnostics)

- `GOVEE_RGBWW_VERSION` is `0.9.2` (`0.9.2-dev` while this work was in
  progress), so `dump_config()` and the optional `version:` sensor distinguish
  a build carrying it from a stale hand-copied `components/` folder.

- `dump_config()` names the cool white source — the RGB dies with their mix, or
  the channel a real cool LED sits on — since that is the first thing to check
  when the Kelvin slider misbehaves.

- The resolved white split is logged at `DEBUG` once per command, with both the
  temperature that arrived and the `cold_white`/`warm_white` the controller
  sent, so a stuck Kelvin slider can be attributed without a rebuild. At
  `VERBOSE`, every frame additionally logs bulb 0's buffer contents and the
  exact bytes written to both of its physical WS2811 pixels.

## [0.9.1] - 2026-08-24

First tagged release. Everything below is what the component ships with, not
a diff against an earlier version.

### Added

- `light:` platform `govee_rgbww`, presenting a Govee H7039-style string as
  `num_bulbs` individually addressable RGBW pixels on top of a separate
  physical `esp32_rmt_led_strip` chain of `2 * num_bulbs` WS2811 pixels.
- `strip_id` / `num_bulbs` config, with a final-validation check that the
  referenced physical strip is sized to exactly `2 * num_bulbs`.
- `ww_channel` (`RED` / `GREEN` / `BLUE`) to select which channel of the
  warm-white driver IC is actually wired to the LED.
- `max_power` power budgeting: total predicted draw is scaled down to the
  configured fraction of the string's theoretical all-channels-full maximum,
  with a 10% floor enforced by both the YAML validator and the runtime
  setter.
- `white_weight` to correct the power model when the warm-white LED draws a
  different current than one RGB channel at the same PWM value.
- Version reporting in `dump_config()`, so a device's ESPHome log identifies
  the exact component version it was built from.
- README covering hardware background, wiring/power warnings, Home Assistant
  add-on setup, config reference, power-limiting calibration procedure, and
  a bring-up checklist; `examples/h7039.yaml` as a working device config.

[Unreleased]: https://github.com/Codethetical/esphome-govee-rgbww/compare/v0.9.2...HEAD
[0.9.2]: https://github.com/Codethetical/esphome-govee-rgbww/releases/tag/v0.9.2
[0.9.1]: https://github.com/Codethetical/esphome-govee-rgbww/releases/tag/v0.9.1

# Changelog

All notable changes to this component are documented here. The format is
based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and this
project follows [Semantic Versioning](https://semver.org/spec/v2.0.0.html)
as scoped in the [Versioning](README.md#versioning) section of the README.

## [Unreleased]

## [0.9.0] - 2026-08-24

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

[Unreleased]: https://github.com/Codethetical/esphome-govee-rgbww/compare/v0.9.0...HEAD
[0.9.0]: https://github.com/Codethetical/esphome-govee-rgbww/releases/tag/v0.9.0

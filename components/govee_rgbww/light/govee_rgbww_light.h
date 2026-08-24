#pragma once

#include "esphome/components/light/addressable_light.h"
#include "esphome/core/component.h"

namespace esphome::govee_rgbww {

// Single source of truth for this component's version; printed by
// dump_config() so a device's log identifies exactly what it's running.
// That matters here because both distribution paths can drift silently: a
// `github://...@main` source moves under you, and a locally copied
// components/ folder goes stale with no git link to notice. Bump together
// with the git tag and CHANGELOG.md - see the release checklist in
// DEVELOPMENT.md.
#define GOVEE_RGBWW_VERSION "0.9.0"

// Which physical channel of the warm-white driver IC the LED is actually
// wired to. Not knowable from software - verify during hardware bring-up.
enum WWChannel : uint8_t {
  WW_CHANNEL_RED = 0,
  WW_CHANNEL_GREEN = 1,
  WW_CHANNEL_BLUE = 2,
};

// Lower bound on the power budget, enforced by the YAML validator and again
// by set_max_power() so a stray runtime value can't dim the string to the
// point where it just looks broken.
static const float MIN_MAX_POWER = 0.10f;

// Presents a Govee H7039-style LED string as `num_bulbs` individually
// addressable RGBW pixels. Electrically the string is one WS2811 chain of
// `2 * num_bulbs` pixels, alternating a warm-white-only driver and a full
// RGB driver per bulb; that physical chain is driven separately (e.g. by
// esp32_rmt_led_strip) and referenced here by `strip_state`.
class GoveeRgbwwLightOutput : public light::AddressableLight {
 public:
  GoveeRgbwwLightOutput(light::LightState *strip_state, int32_t num_bulbs, WWChannel ww_channel, float max_power,
                        float white_weight);

  void write_state(light::LightState *state) override;
  float get_setup_priority() const override;
  void dump_config() override;

  int32_t size() const override { return this->num_bulbs_; }
  light::LightTraits get_traits() override;
  void clear_effect_data() override;

  // Retunes the power budget at runtime and immediately re-pushes the
  // current frame, so a change is visible without touching the light itself.
  // Intended to be driven from a template number while calibrating.
  void set_max_power(float max_power);
  float get_max_power() const { return this->max_power_; }

  // Estimated load of the most recent frame as a fraction of the theoretical
  // maximum (every wired channel at 255). Can exceed max_power - it reports
  // what was *requested*, before limiting. Handy as a template sensor while
  // hunting for the right budget.
  float get_load_fraction() const { return (float) this->last_load_ / (float) this->max_load_; }
  bool is_limiting() const { return this->last_load_ > this->budget_; }

 protected:
  light::ESPColorView get_view_internal(int32_t index) const override;

  // Maps the logical RGBW buffer onto the physical WS2811 chain, applying the
  // power limit on the way. Reads only from buffer_, never from LightState,
  // so it can be replayed at any time - which is what makes runtime budget
  // changes take effect immediately.
  void apply_to_physical_();
  void recompute_budget_();

  light::AddressableLight *physical_;
  int32_t num_bulbs_;
  WWChannel ww_channel_;
  uint8_t *buffer_{nullptr};
  uint8_t *effect_data_{nullptr};

  float max_power_;
  uint16_t white_weight_q8_;  // white_weight as 8.8 fixed point
  uint32_t max_load_{0};      // load with every wired channel at 255
  uint32_t budget_{0};        // max_power_ * max_load_
  uint32_t last_load_{0};
};

}  // namespace esphome::govee_rgbww

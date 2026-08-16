#pragma once

#include "esphome/components/light/addressable_light.h"
#include "esphome/core/component.h"

namespace esphome::govee_rgbww {

// Which physical channel of the warm-white driver IC the LED is actually
// wired to. Not knowable from software - verify during hardware bring-up.
enum WWChannel : uint8_t {
  WW_CHANNEL_RED = 0,
  WW_CHANNEL_GREEN = 1,
  WW_CHANNEL_BLUE = 2,
};

// Presents a Govee H7039-style LED string as `num_bulbs` individually
// addressable RGBW pixels. Electrically the string is one WS2811 chain of
// `2 * num_bulbs` pixels, alternating a warm-white-only driver and a full
// RGB driver per bulb; that physical chain is driven separately (e.g. by
// esp32_rmt_led_strip) and referenced here by `strip_state`.
class GoveeRgbwwLightOutput : public light::AddressableLight {
 public:
  GoveeRgbwwLightOutput(light::LightState *strip_state, int32_t num_bulbs, WWChannel ww_channel);

  void setup() override;
  void write_state(light::LightState *state) override;
  float get_setup_priority() const override;
  void dump_config() override;

  int32_t size() const override { return this->num_bulbs_; }
  light::LightTraits get_traits() override;
  void clear_effect_data() override;

 protected:
  light::ESPColorView get_view_internal(int32_t index) const override;

  light::AddressableLight *physical_;
  int32_t num_bulbs_;
  WWChannel ww_channel_;
  uint8_t *buffer_{nullptr};
  uint8_t *effect_data_{nullptr};
};

}  // namespace esphome::govee_rgbww

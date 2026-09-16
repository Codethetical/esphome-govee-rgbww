#pragma once

#include <memory>

#include "esphome/components/light/addressable_light.h"
#include "esphome/components/text_sensor/text_sensor.h"
#include "esphome/core/component.h"

namespace esphome::govee_rgbww {

// Zeroes whichever channels the active color mode doesn't own.
//
// The light advertises RGB and COLD_WARM_WHITE as two separate modes (see
// get_traits()), so exactly one group of channels is live at a time. The W byte
// covers the bulb's white output however it is produced - see the white-split
// note on cw_ratio_q8_. LightColorValues, however, always carries both:
// LightCall only writes the fields a given call touched, leaving the rest at
// their previous values. ESPHome's own
// LightColorValues::as_rgbw() masks by color mode for that reason, but the
// addressable path (light::color_from_light_color_values) deliberately does
// not - it hands the raw channels to ESPColorView. Without this, picking a
// color would leave the previous white level lit underneath it, and switching
// to white would leave the previous color lit underneath that.
inline void mask_to_color_mode(Color &color, light::ColorMode mode) {
  if (!(mode & light::ColorCapability::RGB))
    color.r = color.g = color.b = 0;
  if (!(mode & light::ColorCapability::COLD_WARM_WHITE))
    color.w = 0;
}

// Single source of truth for this component's version; printed by
// dump_config() so a device's log identifies exactly what it's running.
// That matters here because both distribution paths can drift silently: a
// `github://...@main` source moves under you, and a locally copied
// components/ folder goes stale with no git link to notice. Bump together
// with the git tag and CHANGELOG.md - see the release checklist in
// DEVELOPMENT.md.
#define GOVEE_RGBWW_VERSION "0.9.2-dev"

// Which physical channel of the white driver IC a given white LED is actually
// wired to. Not knowable from software - verify during hardware bring-up.
// Values double as indices into Color::raw[], which is how apply_to_physical_()
// places them.
//
// WHITE_CHANNEL_NONE is the exception and is never used as an index: it means
// the bulb has no second white LED, so cool white is synthesized from the RGB
// dies instead (see cool_mix_q8_). Its value lands on Color::raw[3] - the w
// byte, which the white driver never writes - so writing through it would
// corrupt a real field rather than fail loudly. Every use is behind an
// explicit comparison.
enum WhiteChannel : uint8_t {
  WHITE_CHANNEL_RED = 0,
  WHITE_CHANNEL_GREEN = 1,
  WHITE_CHANNEL_BLUE = 2,
  WHITE_CHANNEL_NONE = 3,
};

// Lower bound on the power budget, enforced by the YAML validator and again
// by set_max_power() so a stray runtime value can't dim the string to the
// point where it just looks broken.
static const float MIN_MAX_POWER = 0.10f;

// Presents a Govee H7039-style LED string as `num_bulbs` individually
// addressable RGBW pixels. Electrically the string is one WS2811 chain of
// `2 * num_bulbs` pixels, alternating a white driver and a full RGB driver per
// bulb; that physical chain is driven separately (e.g. by esp32_rmt_led_strip)
// and referenced here by `strip_state`.
//
// A bulb has four emitters - R, G, B and warm white - and the cool end of the
// color temperature range is synthesized by driving the three RGB dies
// together, tinted by cool_mix_q8_. Strings that do wire a second, cool white
// LED onto the white driver are supported by setting cw_channel; then the
// cool half goes there instead and the RGB dies carry color only.
//
// Either way AddressableLight is fixed at four channels: ESPColorView is
// exactly R/G/B/W, and every addressable effect writes through it. So the
// buffer's W byte is the bulb's white *level*, and the warm/cool balance is a
// single light-wide ratio taken from the color temperature (see
// cw_ratio_q8_). White stays per-bulb and animatable; its color temperature
// does not.
class GoveeRgbwwLightOutput : public light::AddressableLight {
 public:
  // cw_channel is WHITE_CHANNEL_NONE unless the string has a real cool white
  // LED; cool_r/g/b tint the synthetic cool white and are ignored when it
  // does. Appended at the end rather than grouped with cw_channel because
  // every parameter around it is also a float - a mis-ordered argument list
  // would compile silently either way, and the end is where it is easiest to
  // keep this declaration and to_code() in step.
  GoveeRgbwwLightOutput(light::LightState *strip_state, int32_t num_bulbs, WhiteChannel ww_channel,
                        WhiteChannel cw_channel, float cold_mireds, float warm_mireds, bool constant_brightness,
                        float max_power, float white_weight, float cool_r, float cool_g, float cool_b);

  void setup() override;
  void write_state(light::LightState *state) override;
  // Fills buffer_ from the light's current state. Overridden only to apply
  // mask_to_color_mode(); the base class copies every channel regardless of
  // mode. Skipped while an effect is running, exactly as the base does, so
  // effects keep full simultaneous RGB+W control of the buffer.
  void update_state(light::LightState *state) override;
  // Transitions bypass update_state() entirely - the transformer writes into
  // buffer_ itself - so both the masking and the cold/warm split have to be
  // driven from the transformer instead. See GoveeRgbwwTransformer.
  std::unique_ptr<light::LightTransformer> create_default_transition() override;
  float get_setup_priority() const override;
  void dump_config() override;

  int32_t size() const override { return this->num_bulbs_; }
  light::LightTraits get_traits() override;
  void clear_effect_data() override;

  // Recomputes cw_ratio_q8_/ww_ratio_q8_ from an arbitrary LightColorValues.
  // Public because the transition path has to drive it too: ESPHome only calls
  // update_state() when a transformer hands back LightColorValues, and
  // AddressableLightTransformer writes the buffer directly and returns nothing
  // (see LightState::loop()). With the default 1s default_transition_length
  // that is *every* command, so a split computed only in update_state() would
  // stay frozen at whatever the constructor seeded.
  void update_white_split(const light::LightColorValues &val);

  // Retunes the power budget at runtime and immediately re-pushes the
  // current frame, so a change is visible without touching the light itself.
  // Intended to be driven from a template number while calibrating.
  void set_max_power(float max_power);
  float get_max_power() const { return this->max_power_; }

  // Optional diagnostic entity carrying GOVEE_RGBWW_VERSION. Published once
  // from setup(); the value is a compile-time constant, so there is nothing
  // to update afterwards.
  void set_version_text_sensor(text_sensor::TextSensor *version_text_sensor) {
    this->version_text_sensor_ = version_text_sensor;
  }

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
  // Resolves one bulb's buffer entry into the six physical channel values it
  // becomes: rgb_out for the RGB driver (physical pixel 2i+1), white_out for
  // the white driver (2i). Both passes of apply_to_physical_() go through
  // this, so the load estimate is measured from the same bytes that reach the
  // wire - including the saturating clamp, which a re-derived estimate would
  // miss and overstate.
  void physical_bulb_(int32_t index, Color &rgb_out, Color &white_out);
  void recompute_budget_();
  // Quantizes a cold/warm pair into cw_ratio_q8_/ww_ratio_q8_, applying the
  // constant-brightness balance if it is enabled. Inputs are linear (that is,
  // gamma-corrected) levels in [0, 1].
  void set_white_split_(float cold, float warm);

  text_sensor::TextSensor *version_text_sensor_{nullptr};
  light::AddressableLight *physical_;
  int32_t num_bulbs_;
  WhiteChannel ww_channel_;
  WhiteChannel cw_channel_;
  uint8_t *buffer_{nullptr};
  uint8_t *effect_data_{nullptr};

  float cold_mireds_;
  float warm_mireds_;
  bool constant_brightness_;
  float max_power_;
  uint16_t white_weight_q8_;   // white_weight as 8.8 fixed point
  uint16_t cool_mix_q8_[3]{};  // cool_white_rgb as 8.8 fixed point, per RGB die

  // How the buffer's single W byte is divided between the two white LEDs, as
  // 8.8 fixed point (256 == that LED at full for a fully-white bulb). Derived
  // from the color temperature in update_state() and cached rather than read
  // back from LightState, so apply_to_physical_() stays replayable at a new
  // power budget. Both branches of LightState::current_values_as_cwww()
  // factor cleanly into gamma(state * brightness) * ratio, and only the ratio
  // depends on temperature - which is what makes this split legitimate rather
  // than an approximation.
  uint16_t cw_ratio_q8_;
  uint16_t ww_ratio_q8_;
  uint32_t max_load_{0};      // load with every wired channel at 255
  uint32_t budget_{0};        // max_power_ * max_load_
  uint32_t last_load_{0};
};

// AddressableLightTransformer computes its target color with
// light::color_from_light_color_values(), which ignores the color mode. Left
// alone, a transition into RGB mode would fade the white channel toward its
// stale value and simply stay there - nothing calls update_state() again once
// a transition finishes.
//
// It is also the only hook this component gets on the transition path at all,
// which makes it where the cold/warm split has to be refreshed.
class GoveeRgbwwTransformer : public light::AddressableLightTransformer {
 public:
  explicit GoveeRgbwwTransformer(GoveeRgbwwLightOutput &light)
      : light::AddressableLightTransformer(light), parent_(light) {}

  void start() override {
    light::AddressableLightTransformer::start();
    const auto mode = this->target_values_.get_color_mode();
    // Snap the temperature at the start of the fade rather than at the end:
    // the white *level* still fades, so the transition reads correctly, and a
    // temperature-only change lands immediately instead of jumping when the
    // transformer finishes.
    this->parent_.update_white_split(this->target_values_);
    // Same convention as update_state(): in COLD_WARM_WHITE mode the W byte is
    // the bulb white *level*, with the temperature carried by the split. The
    // base start() has already folded brightness into target_color_, so fold it
    // in here too. Set explicitly rather than left to
    // color_from_light_color_values(), which reads white_ - a field belonging
    // to ColorMode::WHITE that only happens to still hold its 1.0 default
    // because nothing in this component's trait set ever writes it.
    if (mode & light::ColorCapability::COLD_WARM_WHITE) {
      this->target_color_.w =
          light::to_uint8_scale(this->target_values_.get_brightness() * this->target_values_.get_state());
    }
    mask_to_color_mode(this->target_color_, mode);
  }

 protected:
  GoveeRgbwwLightOutput &parent_;
};

}  // namespace esphome::govee_rgbww

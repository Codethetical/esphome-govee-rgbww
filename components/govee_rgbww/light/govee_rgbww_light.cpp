#include "govee_rgbww_light.h"
#include <cinttypes>
#include "esphome/core/helpers.h"
#include "esphome/core/log.h"

namespace esphome::govee_rgbww {

static const char *const TAG = "govee_rgbww";

// A default-constructed ESPColorCorrection is an identity transform (no
// gamma table, 255/255 brightness scaling). Used when pushing already
// gamma/brightness-corrected bytes into the physical strip's buffer so they
// aren't corrected a second time.
static const light::ESPColorCorrection IDENTITY_CORRECTION{};

GoveeRgbwwLightOutput::GoveeRgbwwLightOutput(light::LightState *strip_state, int32_t num_bulbs, WWChannel ww_channel,
                                              float max_power, float white_weight)
    : physical_(static_cast<light::AddressableLight *>(strip_state->get_output())),
      num_bulbs_(num_bulbs),
      ww_channel_(ww_channel),
      max_power_(clamp(max_power, MIN_MAX_POWER, 1.0f)),
      white_weight_q8_((uint16_t) (white_weight * 256.0f + 0.5f)) {
  this->recompute_budget_();

  // Allocated here rather than in setup(): LightState::setup() applies the
  // light's initial/restored state immediately, which writes into this
  // buffer via write_state()/get_view_internal() - and LightState's setup()
  // runs before this component's own setup() (both are registered at the
  // same setup priority, LightState first). Allocating in the constructor
  // guarantees the buffer exists before anything can write to it.
  RAMAllocator<uint8_t> allocator;

  this->buffer_ = allocator.allocate(this->num_bulbs_ * 4);
  if (this->buffer_ == nullptr) {
    ESP_LOGE(TAG, "Cannot allocate bulb buffer!");
    this->mark_failed();
    return;
  }
  memset(this->buffer_, 0, this->num_bulbs_ * 4);

  this->effect_data_ = allocator.allocate(this->num_bulbs_);
  if (this->effect_data_ == nullptr) {
    ESP_LOGE(TAG, "Cannot allocate effect data!");
    this->mark_failed();
    return;
  }
  memset(this->effect_data_, 0, this->num_bulbs_);
}

float GoveeRgbwwLightOutput::get_setup_priority() const { return setup_priority::HARDWARE - 1.0f; }

light::LightTraits GoveeRgbwwLightOutput::get_traits() {
  auto traits = light::LightTraits();
  traits.set_supported_color_modes({light::ColorMode::RGB_WHITE, light::ColorMode::WHITE});
  return traits;
}

void GoveeRgbwwLightOutput::clear_effect_data() {
  for (int32_t i = 0; i < this->num_bulbs_; i++)
    this->effect_data_[i] = 0;
}

light::ESPColorView GoveeRgbwwLightOutput::get_view_internal(int32_t index) const {
  uint8_t *base = this->buffer_ + index * 4;
  return {base + 0, base + 1, base + 2, base + 3, &this->effect_data_[index], &this->correction_};
}

// Load is measured in "channel duty units": one unit is one wired channel at
// a PWM value of 1, so a bulb at full white counts 3 * 255 for its RGB driver
// plus 255 * white_weight for its warm-white driver. The two unwired channels
// of the warm-white driver draw nothing and are deliberately not counted.
void GoveeRgbwwLightOutput::recompute_budget_() {
  const uint32_t per_bulb = 3u * 255u + ((255u * this->white_weight_q8_) >> 8);
  this->max_load_ = (uint32_t) this->num_bulbs_ * per_bulb;
  this->budget_ = (uint32_t) (this->max_power_ * (float) this->max_load_);
}

void GoveeRgbwwLightOutput::set_max_power(float max_power) {
  this->max_power_ = clamp(max_power, MIN_MAX_POWER, 1.0f);
  this->recompute_budget_();
  ESP_LOGD(TAG, "Power budget set to %.0f%%", this->max_power_ * 100.0f);
  // Re-map the frame that's already in the buffer so the new budget is
  // visible right away rather than at the next color change.
  this->apply_to_physical_();
}

void GoveeRgbwwLightOutput::apply_to_physical_() {
  if (this->buffer_ == nullptr)
    return;

  // Pass 1: what does the requested frame draw?
  uint32_t load = 0;
  for (int32_t i = 0; i < this->num_bulbs_; i++) {
    auto bulb = this->get(i);
    load += (uint32_t) bulb.get_red_raw() + bulb.get_green_raw() + bulb.get_blue_raw();
    load += ((uint32_t) bulb.get_white_raw() * this->white_weight_q8_) >> 8;
  }
  this->last_load_ = load;

  // Pass 2: if it's over budget, scale every channel by the same factor.
  // Uniform scaling is what keeps hue and the relative brightness between
  // bulbs intact - the scene just gets dimmer. Under budget, num/den is 1/1
  // and every channel passes through byte-exact.
  uint32_t num = 1, den = 1;
  if (load > this->budget_) {
    num = this->budget_;
    den = load;
  }

  for (int32_t i = 0; i < this->num_bulbs_; i++) {
    auto bulb = this->get(i);
    // Round to nearest rather than truncating, so dim channels don't collapse
    // to zero and shift the color.
    auto scale = [num, den](uint8_t v) -> uint8_t { return (uint8_t) (((uint32_t) v * num + den / 2) / den); };

    // Physical index 2*i+1 (the 2nd, 4th, ... driver): the RGB driver for this bulb.
    auto rgb_pixel = this->physical_->get(2 * i + 1);
    rgb_pixel.raw_set_color_correction(&IDENTITY_CORRECTION);
    rgb_pixel.set_rgb(scale(bulb.get_red_raw()), scale(bulb.get_green_raw()), scale(bulb.get_blue_raw()));

    // Physical index 2*i (the 1st, 3rd, ... driver): the warm-white-only driver for this bulb.
    const uint8_t w = scale(bulb.get_white_raw());
    auto ww_pixel = this->physical_->get(2 * i);
    ww_pixel.raw_set_color_correction(&IDENTITY_CORRECTION);
    switch (this->ww_channel_) {
      case WW_CHANNEL_RED:
        ww_pixel.set_red(w);
        break;
      case WW_CHANNEL_GREEN:
        ww_pixel.set_green(w);
        break;
      case WW_CHANNEL_BLUE:
        ww_pixel.set_blue(w);
        break;
    }
  }

  this->physical_->schedule_show();
}

void GoveeRgbwwLightOutput::write_state(light::LightState *state) {
  this->apply_to_physical_();
  this->mark_shown_();
}

void GoveeRgbwwLightOutput::dump_config() {
  const char *channel = this->ww_channel_ == WW_CHANNEL_RED     ? "RED"
                        : this->ww_channel_ == WW_CHANNEL_GREEN ? "GREEN"
                                                                 : "BLUE";
  ESP_LOGCONFIG(TAG,
                "Govee RGBWW Light:\n"
                "  Version: %s\n"
                "  Bulbs: %" PRId32 "\n"
                "  WW Channel: %s\n"
                "  Max Power: %.0f%%\n"
                "  White Weight: %.2f",
                GOVEE_RGBWW_VERSION, this->num_bulbs_, channel, this->max_power_ * 100.0f, this->white_weight_q8_ / 256.0f);
}

}  // namespace esphome::govee_rgbww

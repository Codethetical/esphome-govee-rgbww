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

GoveeRgbwwLightOutput::GoveeRgbwwLightOutput(light::LightState *strip_state, int32_t num_bulbs,
                                              WWChannel ww_channel)
    : physical_(static_cast<light::AddressableLight *>(strip_state->get_output())),
      num_bulbs_(num_bulbs),
      ww_channel_(ww_channel) {
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

void GoveeRgbwwLightOutput::write_state(light::LightState *state) {
  for (int32_t i = 0; i < this->num_bulbs_; i++) {
    auto bulb = this->get(i);
    uint8_t r = bulb.get_red_raw();
    uint8_t g = bulb.get_green_raw();
    uint8_t b = bulb.get_blue_raw();
    uint8_t w = bulb.get_white_raw();

    // Physical index 2*i+1 (the 2nd, 4th, ... driver): the RGB driver for this bulb.
    auto rgb_pixel = this->physical_->get(2 * i + 1);
    rgb_pixel.raw_set_color_correction(&IDENTITY_CORRECTION);
    rgb_pixel.set_rgb(r, g, b);

    // Physical index 2*i (the 1st, 3rd, ... driver): the warm-white-only driver for this bulb.
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
  this->mark_shown_();
}

void GoveeRgbwwLightOutput::dump_config() {
  const char *channel = this->ww_channel_ == WW_CHANNEL_RED     ? "RED"
                        : this->ww_channel_ == WW_CHANNEL_GREEN ? "GREEN"
                                                                 : "BLUE";
  ESP_LOGCONFIG(TAG,
                "Govee RGBWW Light:\n"
                "  Bulbs: %" PRId32 "\n"
                "  WW Channel: %s",
                this->num_bulbs_, channel);
}

}  // namespace esphome::govee_rgbww

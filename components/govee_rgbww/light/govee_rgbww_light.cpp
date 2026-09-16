#include "govee_rgbww_light.h"
#include <algorithm>
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
                                              WhiteChannel ww_channel, WhiteChannel cw_channel, float cold_mireds,
                                              float warm_mireds, bool constant_brightness, float max_power,
                                              float white_weight, float cool_r, float cool_g, float cool_b)
    : physical_(static_cast<light::AddressableLight *>(strip_state->get_output())),
      num_bulbs_(num_bulbs),
      ww_channel_(ww_channel),
      cw_channel_(cw_channel),
      cold_mireds_(cold_mireds),
      warm_mireds_(warm_mireds),
      constant_brightness_(constant_brightness),
      max_power_(clamp(max_power, MIN_MAX_POWER, 1.0f)),
      white_weight_q8_((uint16_t) (white_weight * 256.0f + 0.5f)) {
  // Not in the initialiser list: an array member can't be initialised from
  // three scalars there without a helper that costs more than it saves.
  this->cool_mix_q8_[0] = (uint16_t) (clamp(cool_r, 0.0f, 1.0f) * 256.0f + 0.5f);
  this->cool_mix_q8_[1] = (uint16_t) (clamp(cool_g, 0.0f, 1.0f) * 256.0f + 0.5f);
  this->cool_mix_q8_[2] = (uint16_t) (clamp(cool_b, 0.0f, 1.0f) * 256.0f + 0.5f);

  this->recompute_budget_();
  // Neutral until a color temperature arrives. Matters because an effect can
  // write the W byte while the light is in RGB mode, where no temperature has
  // been set.
  this->set_white_split_(1.0f, 1.0f);

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

// The buffers are allocated in the constructor, not here - see the note there.
// This exists only to publish the version sensor, which has to wait until
// after to_code() has handed us the entity.
void GoveeRgbwwLightOutput::setup() {
  if (this->version_text_sensor_ != nullptr)
    this->version_text_sensor_->publish_state(GOVEE_RGBWW_VERSION);
}

float GoveeRgbwwLightOutput::get_setup_priority() const { return setup_priority::HARDWARE - 1.0f; }

// RGB and COLD_WARM_WHITE are advertised as separate, mutually exclusive modes
// rather than the single combined RGB_COLD_WARM_WHITE mode, because that is
// what the hardware actually is: the white LED sits on its own driver IC and
// is not a white channel mixed into an RGBWW package. Same shape as ESPHome
// own rgbww platform with color_interlock: true.
//
// With cool white synthesized from the RGB dies (cw_channel unset, the
// default) the split is not a preference at all: the dies cannot carry a
// requested color and the cool half of a white mix in the same frame, so the
// two modes have to be mutually exclusive. The reasoning below is why the
// split was adopted before that became true, and why it stays the right shape
// for strings that do have a second white LED.
//
// It also avoids a wash-out that the combined mode causes on the Home
// Assistant side. HA sees RGB_COLD_WARM_WHITE as ColorMode.RGBWW, and its
// color swatches/presets are stored as plain RGB, so every one of them goes
// through a color_rgb_to_rgbw()-style conversion: white is taken as
// min(r, g, b), then the whole tuple is rescaled so its maximum matches the
// input. For any color less than ~50% saturated the white component *is* that
// maximum, so the rescale pins it to max(r, g, b) - 255 for a full-value
// pastel. The white LEDs, being far brighter than a single RGB die, then bury
// the color that was actually asked for. Splitting the modes means HA sends
// rgb_color with no white component at all, and offers a separate color
// temperature slider for the white mix.
light::LightTraits GoveeRgbwwLightOutput::get_traits() {
  auto traits = light::LightTraits();
  traits.set_supported_color_modes({light::ColorMode::RGB, light::ColorMode::COLD_WARM_WHITE});
  // COLD_WARM_WHITE is what gives Home Assistant its Kelvin slider; the range
  // comes from these two. LightCall interpolates a requested temperature
  // between them into the cold_white_/warm_white_ levels this component reads.
  traits.set_min_mireds(this->cold_mireds_);
  traits.set_max_mireds(this->warm_mireds_);
  return traits;
}

void GoveeRgbwwLightOutput::update_state(light::LightState *state) {
  const auto &val = state->current_values;
  // Brightness rides in the color correction rather than in the buffer, so
  // buffer_ keeps full 8-bit resolution at low brightness. Same as the base
  // class - this whole method is AddressableLight::update_state() with the
  // mask added, rather than a call to it, so the buffer is written once.
  this->correction_.set_local_brightness(light::to_uint8_scale(val.get_brightness() * val.get_state()));

  // Cache the cold/warm balance before the effect check, so a temperature
  // change still reaches the string while an effect animates the white level.
  this->update_white_split(val);

  // An active effect owns buffer_ and drives all four channels itself.
  if (this->is_effect_active())
    return;

  const auto mode = val.get_color_mode();
  auto color = light::color_from_light_color_values(val);
  // In COLD_WARM_WHITE mode the W byte is the bulb white *level*, and the
  // temperature lives in the cached split instead. LightColorValues has no
  // combined white level for this mode - white_ belongs to ColorMode::WHITE and
  // would be stale here - so the level is simply "full", with brightness
  // already accounted for by the correction set above.
  if (mode & light::ColorCapability::COLD_WARM_WHITE)
    color.w = 255;
  mask_to_color_mode(color, mode);
  this->all() = color;
  this->schedule_show();
}

void GoveeRgbwwLightOutput::set_white_split_(float cold, float warm) {
  if (this->constant_brightness_) {
    // Same balance as LightState::current_values_as_cwww(): hold cold + warm at
    // max(cold, warm), so total white draw stays flat across the slider instead
    // of doubling at the midpoint where both LEDs would otherwise sit at full.
    const float sum = (cold > 0.0f || warm > 0.0f) ? cold + warm : 1.0f;
    const float peak = std::max(cold, warm);
    cold = peak * cold / sum;
    warm = peak * warm / sum;
  }
  this->cw_ratio_q8_ = (uint16_t) (clamp(cold, 0.0f, 1.0f) * 256.0f + 0.5f);
  this->ww_ratio_q8_ = (uint16_t) (clamp(warm, 0.0f, 1.0f) * 256.0f + 0.5f);
}

// Derived from the color temperature rather than from LightColorValues'
// cold_white_/warm_white_ pair, because those two fields arrive in two
// different conventions and nothing in them says which one you got:
//
//   - LightCall::transform_parameters_() writes them gamma-*un*corrected when
//     it derives them from a temperature itself, so reading them back needs a
//     gamma_correct_lut() round trip to recover linear levels;
//   - Home Assistant instead sends them as plain linear fractions, alongside
//     the temperature (_color_temp_to_cold_warm() in its esphome light
//     platform) - and doing so suppresses ESPHome's own derivation entirely,
//     because transform_parameters_() skips it when the channels are already
//     set. Gamma-correcting those a second time crushes the dimmer of the two
//     LEDs toward zero across most of the slider.
//
// color_temperature has none of that ambiguity: it is set on every path that
// can change the white balance, LightCall clamps it into the traits' mired
// range, and it is exactly what the Kelvin slider means. Note the trade-off -
// a light.control with explicit cold_white:/warm_white: no longer moves the
// split, since it leaves color_temperature untouched.
void GoveeRgbwwLightOutput::update_white_split(const light::LightColorValues &val) {
  // Only COLD_WARM_WHITE carries a temperature. In RGB mode the split is left
  // alone so an effect driving the W byte keeps the last sane balance - the
  // same reason the constructor seeds it neutral.
  if (!(val.get_color_mode() & light::ColorCapability::COLD_WARM_WHITE))
    return;

  const float range = this->warm_mireds_ - this->cold_mireds_;
  if (range <= 0.0f) {
    // Can't happen through the YAML validator, which requires cold < warm.
    this->set_white_split_(1.0f, 1.0f);
    return;
  }

  const float ct = clamp(val.get_color_temperature(), this->cold_mireds_, this->warm_mireds_);
  const float warm = (ct - this->cold_mireds_) / range;
  const float cold = 1.0f - warm;
  // Normalise so the dominant LED sits at full - the same shape LightCall and
  // Home Assistant both produce. peak is never below 0.5, so this is safe.
  // set_white_split_() then applies the constant-brightness balance on top.
  const float peak = std::max(cold, warm);
  this->set_white_split_(cold / peak, warm / peak);

  // Once per command, not per frame. Carries both what arrived and what came
  // out of it, so a log says whether a stuck slider is the controller's fault
  // or this component's.
  ESP_LOGD(TAG, "White split: %.0f mireds (%.0fK) -> cw=%u ww=%u [sent cold_white=%.2f warm_white=%.2f]", ct,
           1000000.0f / ct, this->cw_ratio_q8_, this->ww_ratio_q8_, val.get_cold_white(), val.get_warm_white());
}

std::unique_ptr<light::LightTransformer> GoveeRgbwwLightOutput::create_default_transition() {
  return make_unique<GoveeRgbwwTransformer>(*this);
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
// plus the two white LEDs at white_weight each. The white driver's one unwired
// channel draws nothing and is deliberately not counted.
void GoveeRgbwwLightOutput::recompute_budget_() {
  // Peak combined output of the white driver for a fully-white bulb.
  //
  // With cool white synthesized, only the warm LED hangs off that driver, and
  // the cool half draws through the RGB driver - already counted in full
  // below, and unexceedable because physical_bulb_() clamps each channel at
  // 255. So the ceiling is one LED's worth whatever constant_brightness says.
  //
  // With a real cool LED there are two, and without constant_brightness both
  // sit at full in the middle of the temperature range, so the pair draws
  // twice what one does; with it, the balance holds the pair at one LED's
  // worth across the whole range.
  uint32_t white_peak = 255u;
  if (this->cw_channel_ != WHITE_CHANNEL_NONE && !this->constant_brightness_)
    white_peak = 510u;
  const uint32_t per_bulb = 3u * 255u + ((white_peak * this->white_weight_q8_) >> 8);
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

// Resolves one bulb's buffer entry into the bytes its two physical WS2811
// pixels should carry, before any power scaling. The W byte is a white
// *level*; the cached split says how much of it is cool and how much is warm.
void GoveeRgbwwLightOutput::physical_bulb_(int32_t index, Color &rgb_out, Color &white_out) {
  auto bulb = this->get(index);
  const uint32_t w = bulb.get_white_raw();
  const uint32_t cool = (w * this->cw_ratio_q8_) >> 8;
  const uint32_t warm = (w * this->ww_ratio_q8_) >> 8;

  rgb_out = Color(bulb.get_red_raw(), bulb.get_green_raw(), bulb.get_blue_raw());

  // Zero-initialised, so the white driver's unwired channels are always
  // explicitly dark rather than left at whatever was there before.
  // WhiteChannel's values are Color::raw indices for exactly this.
  white_out = Color(0, 0, 0, 0);
  white_out.raw[this->ww_channel_] = (uint8_t) std::min<uint32_t>(warm, 255);

  if (this->cw_channel_ == WHITE_CHANNEL_NONE) {
    // No second white LED: the cool half is the three RGB dies driven
    // together, tinted by cool_mix_q8_ to correct their hue and match the warm
    // LED's brightness. It adds on top of whatever color the buffer holds,
    // which is nothing at all in COLD_WARM_WHITE mode (mask_to_color_mode()
    // has zeroed RGB) and is only reachable from an effect driving color and
    // the W byte at once. There the sum can clip - accepted, because the
    // alternatives either make white-chasing effects change hue with the
    // light's last mode or dim the color as white rises. See DEVELOPMENT.md.
    for (uint8_t c = 0; c < 3; c++) {
      const uint32_t tinted = (cool * this->cool_mix_q8_[c]) >> 8;
      rgb_out.raw[c] = (uint8_t) std::min<uint32_t>((uint32_t) rgb_out.raw[c] + tinted, 255);
    }
  } else {
    white_out.raw[this->cw_channel_] = (uint8_t) std::min<uint32_t>(cool, 255);
  }
}

void GoveeRgbwwLightOutput::apply_to_physical_() {
  if (this->buffer_ == nullptr)
    return;

  // Pass 1: what does the requested frame draw? Measured from the physical
  // bytes rather than re-derived from the buffer, so a channel that clipped in
  // physical_bulb_() is counted at what it will actually output.
  uint32_t load = 0;
  for (int32_t i = 0; i < this->num_bulbs_; i++) {
    Color rgb, white;
    this->physical_bulb_(i, rgb, white);
    load += (uint32_t) rgb.r + rgb.g + rgb.b;
    // Only the wired channels of the white driver are ever non-zero, so
    // summing all three is the same as naming them.
    load += (((uint32_t) white.r + white.g + white.b) * this->white_weight_q8_) >> 8;
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
  // Round to nearest rather than truncating, so dim channels don't collapse
  // to zero and shift the color.
  auto scale = [num, den](uint8_t v) -> uint8_t { return (uint8_t) (((uint32_t) v * num + den / 2) / den); };

  for (int32_t i = 0; i < this->num_bulbs_; i++) {
    Color rgb, white;
    this->physical_bulb_(i, rgb, white);

    // Physical index 2*i+1 (the 2nd, 4th, ... driver): the RGB driver for this
    // bulb, carrying color, the synthetic cool white, or their sum.
    auto rgb_pixel = this->physical_->get(2 * i + 1);
    rgb_pixel.raw_set_color_correction(&IDENTITY_CORRECTION);
    rgb_pixel.set_rgb(scale(rgb.r), scale(rgb.g), scale(rgb.b));

    // Physical index 2*i (the 1st, 3rd, ... driver): the white driver, carrying
    // the warm white LED and, on strings that have one, a real cool white LED.
    auto white_pixel = this->physical_->get(2 * i);
    white_pixel.raw_set_color_correction(&IDENTITY_CORRECTION);
    white_pixel.set_rgb(scale(white.r), scale(white.g), scale(white.b));

    if (i == 0) {
      auto bulb = this->get(i);
      ESP_LOGV(TAG, "Bulb 0: buffer rgbw=[%u,%u,%u,%u] -> pixel %d (white) [%u,%u,%u], pixel %d (rgb) [%u,%u,%u]",
               bulb.get_red_raw(), bulb.get_green_raw(), bulb.get_blue_raw(), bulb.get_white_raw(), 2 * i,
               scale(white.r), scale(white.g), scale(white.b), 2 * i + 1, scale(rgb.r), scale(rgb.g), scale(rgb.b));
    }
  }

  ESP_LOGV(TAG, "Frame: load %" PRIu32 "/%" PRIu32 ", budget %s", load, this->budget_,
           load > this->budget_ ? "EXCEEDED" : "ok");
  this->physical_->schedule_show();
}

void GoveeRgbwwLightOutput::write_state(light::LightState *state) {
  this->apply_to_physical_();
  this->mark_shown_();
}

static const char *channel_name(WhiteChannel channel) {
  switch (channel) {
    case WHITE_CHANNEL_RED:
      return "RED";
    case WHITE_CHANNEL_GREEN:
      return "GREEN";
    case WHITE_CHANNEL_BLUE:
      return "BLUE";
    default:
      return "NONE";
  }
}

void GoveeRgbwwLightOutput::dump_config() {
  ESP_LOGCONFIG(TAG,
                "Govee RGBWW Light:\n"
                "  Version: %s\n"
                "  Bulbs: %" PRId32 "\n"
                "  WW Channel: %s (%.0fK)\n"
                "  Constant Brightness: %s\n"
                "  Max Power: %.0f%%\n"
                "  White Weight: %.2f",
                GOVEE_RGBWW_VERSION, this->num_bulbs_, channel_name(this->ww_channel_), 1000000.0f / this->warm_mireds_,
                YESNO(this->constant_brightness_), this->max_power_ * 100.0f, this->white_weight_q8_ / 256.0f);
  // Which cool-white source is in play is the first thing to check when the
  // Kelvin slider misbehaves, so name it rather than leaving it to be inferred
  // from the presence of a CW channel line.
  if (this->cw_channel_ == WHITE_CHANNEL_NONE) {
    ESP_LOGCONFIG(TAG, "  CW Source: RGB dies (%.0fK), mix %.2f/%.2f/%.2f", 1000000.0f / this->cold_mireds_,
                  this->cool_mix_q8_[0] / 256.0f, this->cool_mix_q8_[1] / 256.0f, this->cool_mix_q8_[2] / 256.0f);
  } else {
    ESP_LOGCONFIG(TAG, "  CW Source: %s channel (%.0fK)", channel_name(this->cw_channel_),
                  1000000.0f / this->cold_mireds_);
  }
  LOG_TEXT_SENSOR("  ", "Version", this->version_text_sensor_);
}

}  // namespace esphome::govee_rgbww

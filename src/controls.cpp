#include "controls.h"

namespace app
{

// Keep ADC-derived values inside the normalized range expected by smoothing and telemetry.
static float ClampFloat(float value, float lo, float hi)
{
    return value < lo ? lo : (value > hi ? hi : value);
}

DebounceBool::DebounceBool()
: stable_(true), last_sample_(true), last_change_ms_(0)
{
}

// Timer math casts the subtraction to uint32_t so debounce still works when the millisecond counter wraps.
bool DebounceBool::Update(bool sample, uint32_t now_ms, uint32_t debounce_ms)
{
    if(sample != last_sample_)
    {
        last_sample_ = sample;
        last_change_ms_ = now_ms;
    }

    if(sample != stable_ && (uint32_t)(now_ms - last_change_ms_) >= debounce_ms)
    {
        stable_ = sample;
        return true;
    }

    return false;
}

AnalogRotaryLadder::AnalogRotaryLadder()
: centers_(0),
  count_(0),
  hysteresis_(0),
  max_center_error_(0),
  descending_(false),
  valid_(false),
  thresholds_{0, 0, 0, 0, 0},
  candidate_position_(0),
  stable_position_(0),
  candidate_since_ms_(0)
{
}

// The decoder accepts either ascending or descending ladders, because the PCB
// reports descending raw values in logical throw order.
bool AnalogRotaryLadder::Init(const uint16_t* centers, int count, uint16_t hysteresis, uint16_t max_center_error)
{
    centers_ = centers;
    count_ = count;
    hysteresis_ = hysteresis;
    max_center_error_ = max_center_error;
    candidate_position_ = 0;
    stable_position_ = 0;
    candidate_since_ms_ = 0;
    valid_ = false;

    if(!centers_ || count_ < 2 || count_ > config::kRotaryPositionCount)
        return false;

    descending_ = centers_[1] < centers_[0];
    for(int i = 0; i < count_ - 1; ++i)
    {
        if(descending_)
        {
            if(centers_[i] <= centers_[i + 1])
                return false;
        }
        else if(centers_[i] >= centers_[i + 1])
        {
            return false;
        }
        thresholds_[i] = (uint16_t)(((uint32_t)centers_[i] + (uint32_t)centers_[i + 1]) / 2u);
    }

    valid_ = true;
    return true;
}

// Decode by nearest calibrated center, then reject readings that fall too far
// away from every detent.
int AnalogRotaryLadder::Decode(uint16_t value_u16) const
{
    if(!valid_)
        return 0;

    int best_pos = 0;
    uint16_t best_error = 0xffffu;
    for(int i = 0; i < count_; ++i)
    {
        const uint16_t center = centers_[i];
        const uint16_t error = value_u16 > center ? (uint16_t)(value_u16 - center) : (uint16_t)(center - value_u16);
        if(error < best_error)
        {
            best_error = error;
            best_pos = i + 1;
        }
    }

    return best_error <= max_center_error_ ? best_pos : 0;
}

int AnalogRotaryLadder::DecodeWithHysteresis(uint16_t value_u16) const
{
    if(!valid_ || stable_position_ <= 0 || stable_position_ > count_)
        return Decode(value_u16);

    const uint16_t center = centers_[stable_position_ - 1];
    const uint16_t error = value_u16 > center ? (uint16_t)(value_u16 - center) : (uint16_t)(center - value_u16);
    if(error <= (uint16_t)(max_center_error_ + hysteresis_))
        return stable_position_;

    return Decode(value_u16);
}

// Keep the current position until the new value has stayed stable long enough.
int AnalogRotaryLadder::Update(uint16_t value_u16, uint32_t now_ms)
{
    const int decoded = DecodeWithHysteresis(value_u16);
    if(decoded != candidate_position_)
    {
        candidate_position_ = decoded;
        candidate_since_ms_ = now_ms;
    }

    if(decoded != stable_position_
       && (uint32_t)(now_ms - candidate_since_ms_) >= config::kRotaryDebounceMs)
    {
        stable_position_ = decoded;
    }

    return stable_position_;
}
Controls::Controls()
: mailbox_(0),
  foot_(),
  foot_db_(),
  rotary_(),
  initialized_(false),
  foot_down_(false),
  press_count_(0),
  requested_effect_(config::FX_TUNER),
  requested_bypass_(false),
  pot_filtered_{0.0f, 0.0f, 0.0f},
  pot_fixed_{0, 0, 0},
  published_pot_fixed_{0xffffu, 0xffffu, 0xffffu}
{
}

// Initialize the hardware, let the ADC settle, then prime the filters with
// real readings before publishing the first control snapshot.
void Controls::Init(daisy::DaisySeed& hw, ControlMailbox& mailbox)
{
    mailbox_ = &mailbox;

    daisy::AdcChannelConfig adc_cfg[config::ADC_COUNT];
    adc_cfg[config::ADC_POT_0].InitSingle(config::kPotPins[0]);
    adc_cfg[config::ADC_POT_1].InitSingle(config::kPotPins[1]);
    adc_cfg[config::ADC_POT_2].InitSingle(config::kPotPins[2]);
    adc_cfg[config::ADC_ROTARY].InitSingle(config::kRotaryPin);
    hw.adc.Init(adc_cfg, config::ADC_COUNT);
    hw.adc.Start();
    hw.DelayMs(5);

    foot_.Init(config::kFootswitchPin, daisy::GPIO::Mode::INPUT, daisy::GPIO::Pull::PULLUP);
    const bool rotary_ok = rotary_.Init(config::kRotaryCentersU16,
                                        config::kRotaryPositionCount,
                                        config::kRotaryHysteresisU16,
                                        config::kRotaryMaxCenterErrorU16);

    float pot_accum[config::kPhysicalPotCount] = {0.0f, 0.0f, 0.0f};
    float rotary_accum = 0.0f;
    constexpr int kPrimeSamples = 8;
    for(int sample = 0; sample < kPrimeSamples; ++sample)
    {
        for(int i = 0; i < config::kPhysicalPotCount; ++i)
            pot_accum[i] += ClampFloat(hw.adc.GetFloat(i), 0.0f, 1.0f);
        rotary_accum += ClampFloat(hw.adc.GetFloat(config::ADC_ROTARY), 0.0f, 1.0f);
        hw.DelayMs(1);
    }

    for(int i = 0; i < config::kPhysicalPotCount; ++i)
    {
        pot_filtered_[i] = pot_accum[i] / (float)kPrimeSamples;
        pot_fixed_[i] = FloatToU16(pot_filtered_[i]);
    }

    if(rotary_ok)
    {
        const uint16_t rotary_raw = FloatToU16(rotary_accum / (float)kPrimeSamples);
        const uint32_t now = daisy::System::GetNow();
        const int first = rotary_.Update(rotary_raw, now);
        const int stable = rotary_.Update(rotary_raw, now + config::kRotaryDebounceMs);
        uint8_t mapped_effect = requested_effect_;
        if(RotaryPositionToEffect(stable != 0 ? stable : first, mapped_effect))
            requested_effect_ = mapped_effect;
    }

    initialized_ = true;
    Publish();
}

// Poll the controls once per main-loop iteration and publish only meaningful
// changes. The audio callback consumes the latest snapshot separately.
ControlTelemetryState Controls::Process(daisy::DaisySeed& hw, uint32_t now_ms)
{
    ControlTelemetryState state = {};
    state.effect = requested_effect_;
    state.bypass = requested_bypass_;
    state.rotary_position = rotary_.stable_position();
    state.footswitch.occurred = false;

    bool publish_needed = false;
    bool pots_changed = false;

    for(int i = 0; i < config::kPhysicalPotCount; ++i)
    {
        const float raw = ClampFloat(hw.adc.GetFloat(i), 0.0f, 1.0f);
        pot_filtered_[i] += config::kPotSmoothingAlpha * (raw - pot_filtered_[i]);
        pot_fixed_[i] = FloatToU16(pot_filtered_[i]);
        if(PotChangedEnough(pot_fixed_[i], published_pot_fixed_[i]))
        {
            pots_changed = true;
            publish_needed = true;
        }
        state.pots[i] = U16ToFloat(pot_fixed_[i]);
    }
    state.pots[3] = config::kPotPlaceholder4;

    const uint16_t rotary_raw = FloatToU16(ClampFloat(hw.adc.GetFloat(config::ADC_ROTARY), 0.0f, 1.0f));
    state.rotary_raw_u16 = rotary_raw;
    const int position = rotary_.Update(rotary_raw, now_ms);
    state.rotary_position = position;

    uint8_t mapped_effect = requested_effect_;
    if(RotaryPositionToEffect(position, mapped_effect) && mapped_effect != requested_effect_)
    {
        requested_effect_ = mapped_effect;
        requested_bypass_ = false;
        state.effect = requested_effect_;
        state.bypass = requested_bypass_;
        state.effect_changed = true;
        publish_needed = true;
    }

    const bool foot_sample = foot_.Read();
    if(foot_db_.Update(foot_sample, now_ms, config::kFootswitchDebounceMs))
    {
        const bool new_down = !foot_db_.stable();
        if(new_down && !foot_down_)
        {
            foot_down_ = true;
            ++press_count_;
            if(requested_effect_ != config::FX_TUNER)
            {
                requested_bypass_ = !requested_bypass_;
                state.bypass = requested_bypass_;
                publish_needed = true;
            }
            state.footswitch = {true, true, "pressed", press_count_};
        }
        else if(!new_down && foot_down_)
        {
            foot_down_ = false;
            state.footswitch = {true, false, "released", press_count_};
        }
    }

    state.pots_changed = pots_changed;
    if(publish_needed)
        Publish();

    return state;
}

void Controls::ForcePublish()
{
    Publish();
}

// Convert normalized control values into fixed point for the interrupt-safe mailbox.
uint16_t Controls::FloatToU16(float value)
{
    value = ClampFloat(value, 0.0f, 1.0f);
    return (uint16_t)(value * 65535.0f + 0.5f);
}

float Controls::U16ToFloat(uint16_t value)
{
    return (float)value / 65535.0f;
}

// Rotary throws 1..5 select real effects. Position 0 means invalid and position 6 is reserved, so both preserve the previous effect.
bool Controls::RotaryPositionToEffect(int position, uint8_t& out_effect)
{
    if(position >= 1 && position <= 5)
    {
        out_effect = (uint8_t)(position - 1);
        return true;
    }
    return false;
}

// The deadband prevents tiny ADC noise from republishing parameters every main-loop tick.
bool Controls::PotChangedEnough(uint16_t now, uint16_t previous) const
{
    const uint16_t delta = now > previous ? (uint16_t)(now - previous) : (uint16_t)(previous - now);
    return delta >= config::kParameterDeadbandU16;
}

// Publish the compact state after controls are stable enough to affect DSP.
void Controls::Publish()
{
    if(!initialized_ || !mailbox_)
        return;

    ControlSnapshot snapshot;
    snapshot.effect = requested_effect_;
    snapshot.bypass = requested_bypass_ ? 1 : 0;
    for(int i = 0; i < config::kPhysicalPotCount; ++i)
    {
        snapshot.parameters[i] = pot_fixed_[i];
        published_pot_fixed_[i] = pot_fixed_[i];
    }
    mailbox_->Publish(snapshot);
}

} // namespace app

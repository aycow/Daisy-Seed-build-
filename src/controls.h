#pragma once

#include "app_config.h"
#include "control_mailbox.h"
#include "daisy_seed.h"

#include <stdint.h>

namespace app
{

// One debounced footswitch transition. The controls code returns this to the
// foreground loop so telemetry can report press/release edges without involving
// the audio callback.
struct FootswitchEvent
{
    bool occurred;
    bool down;
    const char* edge;
    int press_count;
};

// Small wrap-safe boolean debouncer for a single GPIO input. Update() returns
// true only when the stable output changes after the configured quiet interval.
class DebounceBool
{
  public:
    DebounceBool();
    bool Update(bool sample, uint32_t now_ms, uint32_t debounce_ms);
    bool stable() const { return stable_; }

  private:
    bool stable_;
    bool last_sample_;
    uint32_t last_change_ms_;
};

// Decoder for the six-position resistor-ladder rotary switch. It accepts raw
// 16-bit ADC values, supports ascending or descending calibration centers, and
// returns position 0 while the input is between detents or too far from a center.
class AnalogRotaryLadder
{
  public:
    AnalogRotaryLadder();

    // centers are in logical throw order, not necessarily voltage order. Init()
    // validates strict monotonicity and precomputes midpoint thresholds.
    bool Init(const uint16_t* centers, int count, uint16_t hysteresis, uint16_t max_center_error);

    // Decode() is immediate and stateless except for hysteresis around the current
    // stable position. Update() adds time-based debounce before changing output.
    int Decode(uint16_t value_u16) const;
    int Update(uint16_t value_u16, uint32_t now_ms);
    int stable_position() const { return stable_position_; }

  private:
    int DecodeWithHysteresis(uint16_t value_u16) const;

    const uint16_t* centers_;
    int count_;
    uint16_t hysteresis_;
    uint16_t max_center_error_;
    bool descending_;
    bool valid_;
    uint16_t thresholds_[config::kRotaryPositionCount - 1];
    int candidate_position_;
    int stable_position_;
    uint32_t candidate_since_ms_;
};

// Snapshot of all foreground-control state that may need telemetry after one
// main-loop poll. DSP state itself is published separately through ControlMailbox.
struct ControlTelemetryState
{
    float pots[config::kTelemetryPotCount];
    bool pots_changed;
    bool effect_changed;
    uint8_t effect;
    bool bypass;
    uint16_t rotary_raw_u16;
    int rotary_position;
    FootswitchEvent footswitch;
};

// Owns all slow hardware control work: ADC setup/readout, pot smoothing,
// footswitch debounce, rotary decoding, and publication of the compact requested
// state for the audio interrupt. It deliberately does not touch effect objects.
class Controls
{
  public:
    Controls();

    // Init() configures ADC/GPIO, lets ADC readings settle, primes filters from
    // real samples, and publishes the initial requested state before audio starts.
    void Init(daisy::DaisySeed& hw, ControlMailbox& mailbox);

    // Process() runs from the main loop. It reads controls, updates debounced
    // state, publishes only meaningful changes, and returns telemetry hints.
    ControlTelemetryState Process(daisy::DaisySeed& hw, uint32_t now_ms);
    void ForcePublish();
    void RequestThermalShutdown();

  private:
    static uint16_t FloatToU16(float value);
    static float U16ToFloat(uint16_t value);
    static bool RotaryPositionToEffect(int position, uint8_t& out_effect);
    bool PotChangedEnough(uint16_t now, uint16_t previous) const;
    void Publish();

    ControlMailbox* mailbox_;
    daisy::GPIO foot_;
    DebounceBool foot_db_;
    AnalogRotaryLadder rotary_;
    bool initialized_;
    bool foot_down_;
    int press_count_;
    uint8_t requested_effect_;
    bool requested_bypass_;
    bool thermal_shutdown_requested_;

    // Filtered float values are used only in the main loop. The mailbox carries
    // fixed-point uint16_t parameters so the interrupt boundary never shares floats.
    float pot_filtered_[config::kPhysicalPotCount];
    uint16_t pot_fixed_[config::kPhysicalPotCount];
    uint16_t published_pot_fixed_[config::kPhysicalPotCount];
};

} // namespace app

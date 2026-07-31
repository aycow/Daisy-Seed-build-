#pragma once
#ifndef GRANULARDELAY_MODULE_H
#define GRANULARDELAY_MODULE_H

#include "base_effect_module.h"
#include "daisysp.h"
#include "granularplayermod.h"

namespace bkshepherd
{

// BKShepherd-style effect wrapper for the granular delay. AudioEngine owns one
// instance and calls it only from the audio callback. ParameterChanged() converts
// UI magnitudes into cached DSP values so ProcessMono() can stay deterministic.
class GranularDelayModule : public BaseEffectModule
{
  public:
    // Explicit parameter IDs used by AudioEngine's pot map. The first three are
    // exposed on the pedal pots: size, mix, and pitch.
    enum Param
    {
        SIZE = 0,
        MIX,
        PITCH,
        SPREAD,
        GRAIN_ENV,
        SPEED,
        WIDTH,
        PARAM_COUNT
    };

    // Original upstream history length: 0.5 seconds at 48 kHz. The backing buffer
    // is a plain SDRAM global in the .cpp file, not a constructed object in SDRAM.
    static constexpr int kMaxSamples = 24000;

    GranularDelayModule();
    ~GranularDelayModule();

    void Init(float sample_rate) override;
    void ProcessMono(float in) override;
    void ProcessStereo(float inL, float inR) override;
    float GetBrightnessForLED(int led_id) override;

  protected:
    void ParameterChanged(int parameter_id) override;

  private:
    GranularPlayerMod granular_;
    daisysp::Looper looper_;

    // Cached parameter-derived values. These are written when parameters change
    // and read per sample in ProcessMono().
    float pitch_cents_;
    float size_ms_;
    float mix_;
    float speed_;
    float width_ms_;

    // Smooths grain-size changes slightly so knob movement does not cause abrupt
    // jumps in the granular read head.
    float current_grain_size_ms_;

    // Startup recording state. The looper records one buffer of incoming audio
    // before the granular reader begins using the complete history.
    int first_count_;
    bool loop_recorded_;

    // Reserved for a future hold/freeze control. The current footswitch remains
    // bypass only, so this stays false in normal operation.
    bool hold_;
};

} // namespace bkshepherd
#endif
#pragma once

#include "RuntimeDelayLine.h"
#include "phasor.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

// Adapted from BKShepherd/DaisySeedProjects at commit
// 80feee11f26a401ae324de75d9266e63ed82deb2. It incorporates the DaisySP
// PitchShifter changes proposed in electro-smith/DaisySP#166 and uses caller-
// owned runtime-sized delay storage.

namespace daisysp_modified
{
inline uint32_t PitchHash(uint32_t value)
{
    value ^= value << 13;
    value ^= value >> 17;
    value ^= value << 5;
    return value;
}

class PitchShifter
{
  public:
    PitchShifter()
    : pitch_shift_(1.0f),
      mod_freq_(5.0f),
      delay_size_(1),
      force_recalculation_(false),
      sample_rate_(48000.0f),
      transpose_(0.0f),
      fun_(0.0f),
      mod_a_amount_(0.0f),
      mod_b_amount_(0.0f),
      previous_phase_a_(0.0f),
      previous_phase_b_(0.0f),
      buffer_size_(0),
      quantize_semitones_(false),
      random_state_(1)
    {
        ResetModulationState();
    }

    void Init(float    sample_rate,
              float*   buffer_a,
              float*   buffer_b,
              uint32_t buffer_size,
              bool     quantize_semitones = false)
    {
        // Initialize all state before SetDelSize() calls SetTransposition().
        // The audited upstream source assigned quantize_semitones_ afterward.
        quantize_semitones_  = quantize_semitones;
        pitch_shift_         = 1.0f;
        force_recalculation_ = false;
        sample_rate_         = sample_rate;
        mod_freq_            = 5.0f;
        transpose_           = 0.0f;
        fun_                 = 0.0f;
        mod_a_amount_        = 0.0f;
        mod_b_amount_        = 0.0f;
        previous_phase_a_    = 0.0f;
        previous_phase_b_    = 0.0f;
        buffer_size_         = buffer_size;
        delay_size_          = buffer_size_;
        random_state_        = 1;
        ResetModulationState();

        delay_[0].Init(buffer_a, buffer_size_);
        delay_[1].Init(buffer_b, buffer_size_);
        for(uint8_t i = 0; i < 2; ++i)
        {
            gain_[i] = 0.0f;
            phasor_[i].Init(
                sample_rate_, 50.0f, i == 0 ? 0.0f : 3.14159274101f);
        }

        SetDelSize(delay_size_);
    }

    float Process(float input)
    {
        static constexpr float kPi     = 3.14159274101f;
        float                  phase_a = phasor_[0].Process();
        float                  phase_b = phasor_[1].Process();
        bool                   recalculate_a
            = ((transpose_ >= 0.0f) && (previous_phase_a_ > phase_a))
              || ((transpose_ < 0.0f) && (previous_phase_a_ < phase_a));
        bool recalculate_b
            = ((transpose_ >= 0.0f) && (previous_phase_b_ > phase_b))
              || ((transpose_ < 0.0f) && (previous_phase_b_ < phase_b));

        if(transpose_ >= -0.25f && transpose_ < 0.25f && fun_ > 0.0f
           && NextRandom() % 65536U < 4U)
            recalculate_a = recalculate_b = true;

        if(recalculate_a)
        {
            mod_a_amount_ = fun_ * RandomNormalized() * (delay_size_ * 0.5f);
            mod_coefficient_[0] = 0.0002f + RandomNormalized() * 0.001f;
        }
        if(recalculate_b)
        {
            mod_b_amount_ = fun_ * RandomNormalized() * (delay_size_ * 0.5f);
            mod_coefficient_[1] = 0.0002f + RandomNormalized() * 0.001f;
        }

        slewed_mod_[0]
            += mod_coefficient_[0] * (mod_a_amount_ - slewed_mod_[0]);
        slewed_mod_[1]
            += mod_coefficient_[1] * (mod_b_amount_ - slewed_mod_[1]);
        previous_phase_a_ = phase_a;
        previous_phase_b_ = phase_b;
        phase_a           = 1.0f - phase_a;
        phase_b           = 1.0f - phase_b;
        modulation_[0]    = phase_a * (delay_size_ - 1U);
        modulation_[1]    = phase_b * (delay_size_ - 1U);
        gain_[0]          = sinf(phase_a * kPi);
        gain_[1]          = sinf(phase_b * kPi);

        delay_[0].Write(input);
        delay_[1].Write(input);
        delay_[0].SetDelay(modulation_[0] + slewed_mod_[0]);
        delay_[1].SetDelay(modulation_[1] + slewed_mod_[1]);
        return delay_[0].Read() * gain_[0] + delay_[1].Read() * gain_[1];
    }

    void SetTransposition(float transpose)
    {
        if(transpose_ == transpose && !force_recalculation_)
            return;
        transpose_
            = quantize_semitones_ ? static_cast<int32_t>(transpose) : transpose;
        pitch_shift_ = exp2f(transpose_ * (1.0f / 12.0f));
        mod_freq_    = delay_size_ == 0
                        ? 0.0f
                        : ((pitch_shift_ - 1.0f) * sample_rate_) / delay_size_;
        phasor_[0].SetFreq(mod_freq_);
        phasor_[1].SetFreq(mod_freq_);
        force_recalculation_ = false;
    }

    void SetDelSize(uint32_t size)
    {
        delay_size_ = std::min(size, buffer_size_);
        if(delay_size_ == 0)
            delay_size_ = 1;
        force_recalculation_ = true;
        SetTransposition(transpose_);
    }

    void SetFun(float fun) { fun_ = fun; }

  private:
    uint32_t NextRandom()
    {
        random_state_ = PitchHash(random_state_);
        return random_state_;
    }

    float RandomNormalized()
    {
        return static_cast<float>(NextRandom() % 255U) / 255.0f;
    }

    void ResetModulationState()
    {
        for(uint8_t i = 0; i < 2; ++i)
        {
            gain_[i]            = 0.0f;
            modulation_[i]      = 0.0f;
            slewed_mod_[i]      = 0.0f;
            mod_coefficient_[i] = 0.0002f;
        }
    }

    DelayLine<float> delay_[2];
    float            pitch_shift_;
    float            mod_freq_;
    uint32_t         delay_size_;
    bool             force_recalculation_;
    float            sample_rate_;
    Phasor           phasor_[2];
    float            gain_[2];
    float            modulation_[2];
    float            transpose_;
    float            fun_;
    float            mod_a_amount_;
    float            mod_b_amount_;
    float            previous_phase_a_;
    float            previous_phase_b_;
    float            slewed_mod_[2];
    float            mod_coefficient_[2];
    uint32_t         buffer_size_;
    bool             quantize_semitones_;
    uint32_t         random_state_;
};
} // namespace daisysp_modified

#pragma once

// Adapted from the DaisySP Phasor changes used by BKShepherd at commit
// 80feee11f26a401ae324de75d9266e63ed82deb2.

namespace daisysp_modified
{
class Phasor
{
  public:
    Phasor()
    : frequency_(1.0f), sample_rate_(48000.0f), increment_(0.0f), phase_(0.0f)
    {
    }

    void
    Init(float sample_rate, float frequency = 1.0f, float initial_phase = 0.0f)
    {
        sample_rate_ = sample_rate;
        phase_       = initial_phase;
        SetFreq(frequency);
    }

    float Process()
    {
        static constexpr float kTwoPi = 6.28318548203f;
        const float            output = phase_ / kTwoPi;
        phase_ += increment_;
        while(phase_ >= kTwoPi)
            phase_ -= kTwoPi;
        while(phase_ < 0.0f)
            phase_ += kTwoPi;
        return output;
    }

    void SetFreq(float frequency)
    {
        frequency_                    = frequency;
        static constexpr float kTwoPi = 6.28318548203f;
        increment_
            = sample_rate_ > 0.0f ? kTwoPi * frequency_ / sample_rate_ : 0.0f;
    }

  private:
    float frequency_;
    float sample_rate_;
    float increment_;
    float phase_;
};
} // namespace daisysp_modified

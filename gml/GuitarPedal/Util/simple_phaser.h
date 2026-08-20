#pragma once

#include <cmath>

// Adapted from BKShepherd/DaisySeedProjects SimplePhaser at commit
// 80feee11f26a401ae324de75d9266e63ed82deb2. The all-pass coefficient
// equations come from cycfi/q's biquad.hpp, distributed under the Boost
// Software License 1.0. This local form keeps the firmware on C++14 and avoids
// importing q/infra/gcem.

namespace bkshepherd
{
class SimplePhaser
{
  public:
    static constexpr int kStages = 4;

    SimplePhaser()
    : sample_rate_(48000.0f),
      lfo_phase_(0.0f),
      lfo_freq_hz_(0.3f),
      depth_(0.8f),
      min_freq_(300.0f),
      max_freq_(1500.0f),
      feedback_(0.25f),
      feedback_state_(0.0f),
      lfo_norm_(0.0f),
      sweep_norm_(0.0f)
    {
    }

    void Init(float sample_rate)
    {
        sample_rate_ = sample_rate;
        const Coefficients coefficients
            = CalculateCoefficients(0.5f * (min_freq_ + max_freq_));
        for(int i = 0; i < kStages; ++i)
        {
            allpass_[i].Reset();
            allpass_[i].Configure(coefficients);
        }
    }

    void SetLfoFrequency(float hz) { lfo_freq_hz_ = hz; }
    void SetDepth(float depth) { depth_ = Clamp(depth, 0.0f, 1.0f); }
    void SetFeedback(float feedback)
    {
        feedback_ = Clamp(feedback, 0.0f, 0.7f);
    }
    void SetRange(float min_hz, float max_hz)
    {
        min_freq_ = min_hz < 1.0f ? 1.0f : min_hz;
        max_freq_ = max_hz < min_freq_ + 1.0f ? min_freq_ + 1.0f : max_hz;
    }

    float GetSweepNormalized() const { return sweep_norm_; }

    float Process(float input)
    {
        static constexpr float kTwoPi = 6.28318530718f;
        lfo_phase_ += kTwoPi * lfo_freq_hz_ / sample_rate_;
        if(lfo_phase_ >= kTwoPi)
            lfo_phase_ -= kTwoPi;

        const float lfo = 0.5f * (1.0f + sinf(lfo_phase_));
        lfo_norm_       = lfo;
        sweep_norm_     = depth_ * lfo;

        const float center_frequency
            = min_freq_ * powf(max_freq_ / min_freq_, sweep_norm_);

        // All four stages share a center frequency, so calculate this once.
        const Coefficients coefficients
            = CalculateCoefficients(center_frequency);
        for(int i = 0; i < kStages; ++i)
            allpass_[i].Configure(coefficients);

        float output = input + feedback_state_;
        for(int i = 0; i < kStages; ++i)
            output = allpass_[i].Process(output);

        feedback_state_ = output * feedback_;
        return 0.5f * (input + output);
    }

  private:
    struct Coefficients
    {
        float b0;
        float b1;
        float b2;
        float a1;
        float a2;
    };

    class Allpass
    {
      public:
        Allpass()
        : b0_(1.0f),
          b1_(0.0f),
          b2_(0.0f),
          a1_(0.0f),
          a2_(0.0f),
          x1_(0.0f),
          x2_(0.0f),
          y1_(0.0f),
          y2_(0.0f)
        {
        }

        void Reset() { x1_ = x2_ = y1_ = y2_ = 0.0f; }
        void Configure(const Coefficients& c)
        {
            b0_ = c.b0;
            b1_ = c.b1;
            b2_ = c.b2;
            a1_ = c.a1;
            a2_ = c.a2;
        }
        float Process(float input)
        {
            const float output
                = b0_ * input + b1_ * x1_ + b2_ * x2_ - a1_ * y1_ - a2_ * y2_;
            x2_ = x1_;
            x1_ = input;
            y2_ = y1_;
            y1_ = output;
            return output;
        }

      private:
        float b0_, b1_, b2_, a1_, a2_;
        float x1_, x2_, y1_, y2_;
    };

    static float Clamp(float value, float minimum, float maximum)
    {
        return value < minimum ? minimum : (value > maximum ? maximum : value);
    }

    Coefficients CalculateCoefficients(float frequency) const
    {
        static constexpr float kTwoPi     = 6.28318530718f;
        static constexpr float kQ         = 0.7f;
        const float            omega      = kTwoPi * frequency / sample_rate_;
        const float            sine       = sinf(omega);
        const float            cosine     = cosf(omega);
        const float            alpha      = sine / (2.0f * kQ);
        const float            inverse_a0 = 1.0f / (1.0f + alpha);

        Coefficients c;
        c.b0 = (1.0f - alpha) * inverse_a0;
        c.b1 = (-2.0f * cosine) * inverse_a0;
        c.b2 = 1.0f;
        c.a1 = c.b1;
        c.a2 = c.b0;
        return c;
    }

    Allpass allpass_[kStages];
    float   sample_rate_;
    float   lfo_phase_;
    float   lfo_freq_hz_;
    float   depth_;
    float   min_freq_;
    float   max_freq_;
    float   feedback_;
    float   feedback_state_;
    float   lfo_norm_;
    float   sweep_norm_;
};
} // namespace bkshepherd

#include "phaser_module.h"

#include "diagnostic_config.h"

#include <cmath>

#if DIAG_FX_PHASER
namespace
{
const bkshepherd::ParameterMetaData
    kMetadata[bkshepherd::PhaserModule::PARAM_COUNT]
    = {{"Mix", bkshepherd::ParameterValueType::FloatMagnitude, 0, 0, 64, 0, -1},
       {"Rate",
        bkshepherd::ParameterValueType::FloatMagnitude,
        0,
        0,
        32,
        1,
        -1},
       {"Depth",
        bkshepherd::ParameterValueType::FloatMagnitude,
        0,
        0,
        127,
        2,
        -1},
       {"Feedback",
        bkshepherd::ParameterValueType::FloatMagnitude,
        0,
        0,
        32,
        -1,
        -1}};

float Smooth(float current, float target)
{
    return current + 0.02f * (target - current);
}

float MapRate(float normalized)
{
    return 0.10f * powf(8.0f / 0.10f, normalized);
}
} // namespace

namespace bkshepherd
{
PhaserModule::PhaserModule()
: BaseEffectModule(),
  phaser_(),
  target_rate_(1.0f),
  target_depth_(0.95f),
  smoothed_rate_(1.0f),
  smoothed_depth_(0.95f),
  dry_gain_(0.70710678f),
  wet_gain_(0.70710678f)
{
    m_name          = "Phaser";
    m_paramMetaData = kMetadata;
    InitParams(PARAM_COUNT);
}

PhaserModule::~PhaserModule() {}

void PhaserModule::Init(float sample_rate)
{
    BaseEffectModule::Init(sample_rate);
    phaser_.Init(sample_rate);
    phaser_.SetRange(300.0f, 1500.0f);
    phaser_.SetDepth(0.95f);
    phaser_.SetFeedback(0.20f);
    smoothed_rate_  = 1.0f;
    smoothed_depth_ = 0.95f;
    for(int parameter = 0; parameter < PARAM_COUNT; ++parameter)
        ParameterChanged(parameter);
}

void PhaserModule::ParameterChanged(int parameter_id)
{
    if(parameter_id == MIX)
    {
        static constexpr float kHalfPi = 1.57079632679f;
        const float            mix     = GetParameterAsMagnitude(MIX);
        dry_gain_                      = cosf(mix * kHalfPi);
        wet_gain_                      = sinf(mix * kHalfPi);
    }
    else if(parameter_id == RATE)
        target_rate_ = MapRate(GetParameterAsMagnitude(RATE));
    else if(parameter_id == DEPTH)
    {
        const float depth = GetParameterAsMagnitude(DEPTH);
        target_depth_     = depth > 0.98f ? 0.98f : depth;
    }
    else if(parameter_id == FEEDBACK)
        phaser_.SetFeedback(GetParameterAsMagnitude(FEEDBACK) * 0.45f);
}

void PhaserModule::ProcessMono(float input)
{
    smoothed_rate_  = Smooth(smoothed_rate_, target_rate_);
    smoothed_depth_ = Smooth(smoothed_depth_, target_depth_);
    phaser_.SetLfoFrequency(smoothed_rate_);
    phaser_.SetDepth(smoothed_depth_);

    const float trimmed  = input * 0.85f;
    const float combined = phaser_.Process(trimmed);
    const float wet      = 2.0f * combined - trimmed;
    const float output   = (trimmed * dry_gain_ + wet * wet_gain_) * 0.95f;
    m_audioLeft = m_audioRight = output;
}

void PhaserModule::ProcessStereo(float input_left, float input_right)
{
    ProcessMono(0.5f * (input_left + input_right));
}

float PhaserModule::GetBrightnessForLED(int led_id)
{
    const float base = BaseEffectModule::GetBrightnessForLED(led_id);
    if(led_id != 1)
        return base;
    float sweep = phaser_.GetSweepNormalized();
    sweep       = sweep < 0.0f ? 0.0f : (sweep > 1.0f ? 1.0f : sweep);
    return base * (0.2f + 0.8f * sweep);
}
} // namespace bkshepherd
#endif

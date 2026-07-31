#include "granulardelay_module.h"

#include "daisy_core.h"

using namespace bkshepherd;

namespace
{
// The delayed audio history lives in SDRAM so the pedal keeps internal RAM
// free for stack, DMA state, and other callback-critical objects.
float DSY_SDRAM_BSS buffer_gran_delay[GranularDelayModule::kMaxSamples];
static const char* s_grain_env_names[3] = {"Cos", "SlowAtk", "FastAtk"};

static const ParameterMetaData s_meta_data[GranularDelayModule::PARAM_COUNT] = {
    {"Size(ms)", ParameterValueType::FloatMagnitude, 0, nullptr, 63, 0, 14},
    {"Mix", ParameterValueType::FloatMagnitude, 0, nullptr, 64, 1, 15},
    {"Pitch", ParameterValueType::FloatMagnitude, 0, nullptr, 64, 2, 16},
    {"Spread", ParameterValueType::FloatMagnitude, 0, nullptr, 51, -1, 17},
    {"GrainEnv", ParameterValueType::Binned, 3, s_grain_env_names, 0, -1, 18},
    {"Speed", ParameterValueType::FloatMagnitude, 0, nullptr, 95, -1, 19},
    {"Width", ParameterValueType::FloatMagnitude, 0, nullptr, 64, -1, 20},
};

static float ClampFloat(float value, float lo, float hi)
{
    return value < lo ? lo : (value > hi ? hi : value);
}

static float ParamToRange(float mag, float lo, float hi)
{
    return lo + ClampFloat(mag, 0.0f, 1.0f) * (hi - lo);
}
}

GranularDelayModule::GranularDelayModule()
: BaseEffectModule(),
  granular_(),
  looper_(),
  pitch_cents_(0.0f),
  size_ms_(150.0f),
  mix_(0.5f),
  speed_(0.5f),
  width_ms_(25.0f),
  current_grain_size_ms_(150.0f),
  first_count_(0),
  loop_recorded_(false),
  hold_(false)
{
    m_name = "GranDelay";
    m_paramMetaData = s_meta_data;
    InitParams(PARAM_COUNT);
}

GranularDelayModule::~GranularDelayModule()
{
}

void GranularDelayModule::Init(float sample_rate)
{
    BaseEffectModule::Init(sample_rate);
    for(int i = 0; i < kMaxSamples; ++i)
        buffer_gran_delay[i] = 0.0f;

    looper_.Init(buffer_gran_delay, kMaxSamples);
    looper_.SetMode(static_cast<daisysp::Looper::Mode>(3));
    granular_.Init(buffer_gran_delay, kMaxSamples, sample_rate, 0.0f, 0.5f);

    first_count_ = 0;
    loop_recorded_ = false;
    hold_ = false;
    for(int i = 0; i < PARAM_COUNT; ++i)
        ParameterChanged(i);
}

// Cache the user-facing parameter values here so the audio callback can just
// use the precomputed numbers instead of re-reading metadata every sample.
void GranularDelayModule::ParameterChanged(int parameter_id)
{
    switch(parameter_id)
    {
        case SIZE:
            size_ms_ = ParamToRange(GetParameterAsMagnitude(SIZE), 1.0f, 300.0f);
            break;
        case MIX:
            mix_ = GetParameterAsMagnitude(MIX);
            break;
        case PITCH:
        {
            const float semitones = ParamToRange(GetParameterAsMagnitude(PITCH), -12.0f, 12.0f);
            pitch_cents_ = (float)((int)(semitones + (semitones >= 0.0f ? 0.5f : -0.5f))) * 100.0f;
        }
        break;
        case SPREAD:
            granular_.setStereoSpread(GetParameterAsMagnitude(SPREAD));
            break;
        case GRAIN_ENV:
            granular_.setEnvelopeMode(GetParameterAsBinnedValue(GRAIN_ENV) - 1);
            break;
        case SPEED:
            speed_ = ParamToRange(GetParameterAsMagnitude(SPEED), -2.0f, 2.0f);
            break;
        case WIDTH:
            width_ms_ = ParamToRange(GetParameterAsMagnitude(WIDTH), 0.0f, 50.0f);
            break;
        default:
            break;
    }
}

// This module is processed only from the audio callback, one sample at a time.
// It reads the cached parameters above and writes the current wet output here.
void GranularDelayModule::ProcessMono(float in)
{
    BaseEffectModule::ProcessMono(in);

    if(!loop_recorded_)
    {
        if(first_count_ == 0)
            looper_.TrigRecord();
        if(first_count_ > kMaxSamples)
        {
            loop_recorded_ = true;
            looper_.TrigRecord();
        }
        else
        {
            ++first_count_;
        }
    }

    if(!hold_)
        looper_.Process(in);

    current_grain_size_ms_ += 0.0001f * (size_ms_ - current_grain_size_ms_);
    granular_.Process(speed_, pitch_cents_, current_grain_size_ms_, width_ms_);

    const float gran_left = granular_.getLeftOut();
    const float gran_right = granular_.getRightOut();
    m_audioLeft = gran_left * mix_ + in * (1.0f - mix_);
    m_audioRight = gran_right * mix_ + in * (1.0f - mix_);
}

void GranularDelayModule::ProcessStereo(float inL, float inR)
{
    (void)inR;
    ProcessMono(inL);
}

float GranularDelayModule::GetBrightnessForLED(int led_id)
{
    const float value = BaseEffectModule::GetBrightnessForLED(led_id);
    return led_id == 1 ? value * (hold_ ? 1.0f : 0.0f) : value;
}

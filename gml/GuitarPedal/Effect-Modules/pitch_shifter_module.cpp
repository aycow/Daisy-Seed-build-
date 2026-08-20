#include "pitch_shifter_module.h"

#include "diagnostic_config.h"

#include <cstring>

#if DIAG_FX_PITCH_SHIFTER
namespace
{
const char* kSemitoneNames[]  = {"1", "2", "3", "4", "5", "6", "7", "Oct"};
const char* kDirectionNames[] = {"Down", "Up"};
const char* kModeNames[]      = {"Latch", "Momentary"};

const bkshepherd::ParameterMetaData
    kMetadata[bkshepherd::PitchShifterModule::PARAM_COUNT]
    = {{"Semitone",
        bkshepherd::ParameterValueType::Binned,
        8,
        kSemitoneNames,
        0,
        0,
        -1},
       {"Crossfade",
        bkshepherd::ParameterValueType::FloatMagnitude,
        0,
        0,
        127,
        1,
        -1},
       {"Direction",
        bkshepherd::ParameterValueType::Binned,
        2,
        kDirectionNames,
        0,
        2,
        -1},
       {"Mode",
        bkshepherd::ParameterValueType::Binned,
        2,
        kModeNames,
        0,
        -1,
        -1},
       {"Shift",
        bkshepherd::ParameterValueType::FloatMagnitude,
        0,
        0,
        0,
        -1,
        -1},
       {"Return",
        bkshepherd::ParameterValueType::FloatMagnitude,
        0,
        0,
        0,
        -1,
        -1}};

static constexpr uint32_t kMinimumDelaySamples = 2048;
static constexpr uint32_t kPitchDelaySamples   = 6000;

// CPU-owned cached SDRAM histories. Never use DMA_BUFFER_MEM_SECTION here.
DSY_SDRAM_BSS float pitch_delay_buffer_a[kPitchDelaySamples];
DSY_SDRAM_BSS float pitch_delay_buffer_b[kPitchDelaySamples];
} // namespace

namespace bkshepherd
{
PitchShifterModule::PitchShifterModule()
: BaseEffectModule(),
  pitch_shifter_(),
  crossfade_(),
  direction_down_(true),
  latching_(true)
{
    m_name          = "Pitch Shifter";
    m_paramMetaData = kMetadata;
    InitParams(PARAM_COUNT);
}

PitchShifterModule::~PitchShifterModule() {}

void PitchShifterModule::Init(float sample_rate)
{
    BaseEffectModule::Init(sample_rate);
    std::memset(pitch_delay_buffer_a, 0, sizeof(pitch_delay_buffer_a));
    std::memset(pitch_delay_buffer_b, 0, sizeof(pitch_delay_buffer_b));
    pitch_shifter_.Init(sample_rate,
                        pitch_delay_buffer_a,
                        pitch_delay_buffer_b,
                        kPitchDelaySamples,
                        false);
    crossfade_.Init(daisysp::CROSSFADE_CPOW);

    // No alternate effect footswitch exists. MODE remains fixed to LATCH;
    // SHIFT and RETURN are intentionally absent from the processing path.
    latching_       = true;
    direction_down_ = GetParameterAsBinnedValue(DIRECTION) == 1;
    crossfade_.SetPos(GetParameterAsMagnitude(CROSSFADE));
    UpdateTransposition();
}

void PitchShifterModule::UpdateTransposition()
{
    int semitone = GetParameterAsBinnedValue(SEMITONE);
    if(semitone == 8)
        semitone = 12;

    const float interpolation = (static_cast<float>(semitone) - 1.0f) / 11.0f;
    const float clamped       = interpolation < 0.0f
                              ? 0.0f
                              : (interpolation > 1.0f ? 1.0f : interpolation);
    const float delay_samples
        = static_cast<float>(kMinimumDelaySamples)
          + static_cast<float>(kPitchDelaySamples - kMinimumDelaySamples)
                * clamped;
    uint32_t delay = static_cast<uint32_t>(delay_samples + 0.5f);
    if(delay < kMinimumDelaySamples)
        delay = kMinimumDelaySamples;
    else if(delay > kPitchDelaySamples)
        delay = kPitchDelaySamples;

    pitch_shifter_.SetDelSize(delay);
    const float transpose = direction_down_ ? -static_cast<float>(semitone)
                                            : static_cast<float>(semitone);
    pitch_shifter_.SetTransposition(transpose);
}

void PitchShifterModule::ParameterChanged(int parameter_id)
{
    if(parameter_id == CROSSFADE)
        crossfade_.SetPos(GetParameterAsMagnitude(CROSSFADE));
    else if(parameter_id == DIRECTION)
    {
        direction_down_ = GetParameterAsBinnedValue(DIRECTION) == 1;
        UpdateTransposition();
    }
    else if(parameter_id == SEMITONE)
        UpdateTransposition();
    else if(parameter_id == MODE)
        latching_ = true;
}

void PitchShifterModule::ProcessMono(float input)
{
    float       shifted = pitch_shifter_.Process(input);
    const float output = latching_ ? crossfade_.Process(input, shifted) : input;
    m_audioLeft = m_audioRight = output;
}

void PitchShifterModule::ProcessStereo(float input_left, float input_right)
{
    ProcessMono(0.5f * (input_left + input_right));
}
} // namespace bkshepherd
#endif

#pragma once

#include "app_config.h"
#include "control_mailbox.h"
#include "daisy_seed.h"
#include "tuner.h"

#include "base_effect_module.h"
#include "chorus_module.h"
#include "reverb_module.h"
#include "crusher_module.h"
#include "granulardelay_module.h"

#include <stdint.h>

#if AUDIO_CPU_LOAD_DEBUG
#include "util/CpuLoadMeter.h"
#endif

namespace app
{

// Callback-owned DSP engine. The foreground loop publishes requested controls to
// ControlMailbox; this class consumes one stable snapshot per audio block, owns all
// effect objects, applies parameters, performs transition ramps, and captures input
// samples for the tuner ring.
class AudioEngine
{
  public:
    AudioEngine();
    void Init(float sample_rate, ControlMailbox& mailbox, TunerCapture& tuner_capture);

    // libDaisy calls Process() from the audio interrupt/DMA path. Keep this path
    // bounded: no UART, no ADC reads, no allocation, no locks, and no tuner YIN.
    void Process(daisy::AudioHandle::InputBuffer in,
                 daisy::AudioHandle::OutputBuffer out,
                 size_t size);

#if AUDIO_CPU_LOAD_DEBUG
    // Copied by the main loop when callback-load instrumentation is enabled.
    struct CpuLoadSnapshot
    {
        float average;
        float peak;
        float effect_peak[config::FX_COUNT];
        uint32_t overrun_count;
    };
    CpuLoadSnapshot GetCpuLoadSnapshot() const;
#endif

  private:
    // Maps the three physical pots onto each module's real parameter IDs. This
    // avoids keyword matching inside the callback and keeps parameter changes block
    // based instead of per-sample.
    struct PotParamMap
    {
        int p0;
        int p1;
        int p2;
    };

    static bool StringContainsCi(const char* haystack, const char* needle);
    static int FindParamByKeywords(bkshepherd::BaseEffectModule* fx,
                                   const char* kw1,
                                   const char* kw2,
                                   const char* kw3);
    static int FallbackParamForPot(bkshepherd::BaseEffectModule* fx, int pot_idx);
    static float U16ToFloat(uint16_t value);
    static float ClampOutput(float value);

    void BuildParamMaps();
    void ApplySnapshotIfChanged();
    void StartTransition(uint8_t requested_effect, bool requested_bypass);
    void StepTransition();
    void ProcessEffectSample(bkshepherd::BaseEffectModule* fx, float in, float& out_l, float& out_r);
    void ApplyParameters(uint8_t effect, const uint16_t* parameters);
    bkshepherd::BaseEffectModule* CurrentEffect();

    ControlMailbox* mailbox_;
    TunerCapture* tuner_capture_;

    // Concrete effect instances live here so only the audio callback mutates or
    // processes them. The main loop never calls module setters directly.
    bkshepherd::ChorusModule chorus_;
    bkshepherd::ReverbModule reverb_;
    bkshepherd::CrusherModule crusher_;
    bkshepherd::GranularDelayModule granular_delay_;
    bkshepherd::BaseEffectModule* effects_[config::FX_COUNT];
    PotParamMap maps_[config::FX_COUNT];

    // current_ is the last valid request read from the mailbox. applied_ tracks
    // which parameters have already been pushed into the active module.
    ControlSnapshot current_;
    ControlSnapshot applied_;
    uint32_t mailbox_revision_;

    enum TransitionState : uint8_t
    {
        TRANSITION_STEADY,
        TRANSITION_FADING_OUT,
        TRANSITION_FADING_IN
    };

    // active_* describes what is currently audible. pending_* stores the next
    // requested state while a fade-out is finishing.
    uint8_t active_effect_;
    bool active_bypass_;
    uint8_t pending_effect_;
    bool pending_bypass_;
    TransitionState transition_;
    float wet_gain_;
    float wet_gain_step_;

#if AUDIO_CPU_LOAD_DEBUG
    daisy::CpuLoadMeter cpu_load_;
    float effect_peak_load_[config::FX_COUNT];
    uint32_t overrun_count_;
#endif
};

} // namespace app
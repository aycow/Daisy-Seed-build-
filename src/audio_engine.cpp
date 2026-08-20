#include "audio_engine.h"

namespace app
{
AudioEngine::AudioEngine()
: mailbox_(0),
  tuner_capture_(0)
#if DIAG_FX_PHASER
  ,
  phaser_()
#endif
#if DIAG_FX_REVERB
  ,
  reverb_()
#endif
#if DIAG_FX_PITCH_SHIFTER
  ,
  pitch_shifter_()
#endif
#if DIAG_FX_GRANULAR
  ,
  granular_delay_()
#endif
  ,
  effects_
{
    0,
#if DIAG_FX_PHASER
        &phaser_,
#else
        0,
#endif
#if DIAG_FX_REVERB
        &reverb_,
#else
        0,
#endif
#if DIAG_FX_PITCH_SHIFTER
        &pitch_shifter_,
#else
        0,
#endif
#if DIAG_FX_GRANULAR
        &granular_delay_
#else
        0
#endif
}
, maps_{{-1, -1, -1}, {-1, -1, -1}, {-1, -1, -1}, {-1, -1, -1}, {-1, -1, -1}},
    current_{config::FX_TUNER, 0, (uint8_t)SystemMode::Normal, 0, {0, 0, 0}},
    applied_{0xffu,
             0xffu,
             (uint8_t)SystemMode::Normal,
             0,
             {0xffffu, 0xffffu, 0xffffu}},
    mailbox_revision_(0), active_effect_(config::FX_TUNER),
    active_bypass_(false), pending_effect_(config::FX_TUNER),
    pending_bypass_(false), transition_(TRANSITION_STEADY), wet_gain_(0.0f),
    wet_gain_step_(
        1.0f / (config::kSampleRateHz * (config::kTransitionTimeMs * 0.001f))),
    thermal_audio_state_(THERMAL_AUDIO_NORMAL), thermal_gain_(1.0f),
    thermal_gain_step_(
        1.0f / (config::kSampleRateHz * (config::kThermalAudioFadeMs * 0.001f)))
#if AUDIO_CPU_LOAD_DEBUG
        ,
    cpu_load_(), effect_peak_load_{0.0f, 0.0f, 0.0f, 0.0f, 0.0f},
    overrun_count_(0), cpu_block_start_ticks_(0),
    cpu_ticks_per_block_inv_(0.0f)
#endif
{
}

void AudioEngine::Init(float           sample_rate,
                       ControlMailbox& mailbox,
                       TunerCapture&   tuner_capture)
{
    mailbox_       = &mailbox;
    tuner_capture_ = &tuner_capture;

#if DIAG_FX_PHASER
    phaser_.Init(sample_rate);
    phaser_.SetEnabled(true);
#endif
#if DIAG_FX_REVERB
    reverb_.Init(sample_rate);
    reverb_.SetEnabled(true);
#endif
#if DIAG_FX_PITCH_SHIFTER
    pitch_shifter_.Init(sample_rate);
    pitch_shifter_.SetEnabled(true);
#endif
#if DIAG_FX_GRANULAR
    granular_delay_.Init(sample_rate);
    granular_delay_.SetEnabled(true);
#endif

    BuildParamMaps();
#if AUDIO_CPU_LOAD_DEBUG
    cpu_load_.Init(sample_rate, config::kAudioBlockSize);
    const float seconds_per_block
        = static_cast<float>(config::kAudioBlockSize) / sample_rate;
    cpu_ticks_per_block_inv_
        = 1.0f
          / (static_cast<float>(daisy::System::GetTickFreq())
             * seconds_per_block);
    for(int i = 0; i < config::FX_COUNT; ++i)
        effect_peak_load_[i] = 0.0f;
    overrun_count_         = 0;
    cpu_block_start_ticks_ = 0;
#endif
#if DIAG_ENGINE_USE_MAILBOX
    ApplySnapshotIfChanged();
#endif
#if DIAG_STAGE != DIAG_STAGE_PRODUCTION
    current_.effect = (uint8_t)DIAG_FORCE_EFFECT;
    current_.bypass = 0;
#endif
    active_effect_  = current_.effect;
    active_bypass_  = current_.bypass != 0;
    pending_effect_ = active_effect_;
    pending_bypass_ = active_bypass_;
    wet_gain_
        = (active_effect_ == config::FX_TUNER || active_bypass_) ? 0.0f : 1.0f;
    thermal_audio_state_ = THERMAL_AUDIO_NORMAL;
    thermal_gain_        = 1.0f;
    thermal_gain_step_
        = 1.0f / (sample_rate * (config::kThermalAudioFadeMs * 0.001f));
}

// The callback reads one control snapshot per block, then keeps audio work
// deterministic: capture input, process the current effect, and apply any
// pending fade state sample by sample.
void AudioEngine::Process(daisy::AudioHandle::InputBuffer  in,
                          daisy::AudioHandle::OutputBuffer out,
                          size_t                           size)
{
#if AUDIO_CPU_LOAD_DEBUG
    const uint8_t metered_effect = active_effect_;
    cpu_block_start_ticks_       = daisy::System::GetTick();
    cpu_load_.OnBlockStart();
#endif

#if DIAG_ENGINE_USE_MAILBOX
    ApplySnapshotIfChanged();
#endif

#if DIAG_STAGE == DIAG_STAGE_AUDIOENGINE_PROCESS_DRY
    for(size_t i = 0; i < size; ++i)
    {
        const float x = in[0][i];
        out[0][i]     = x;
        out[1][i]     = x;
    }
#if AUDIO_CPU_LOAD_DEBUG
    FinishCpuLoadMeasurement();
#endif
    return;
#endif

#if DIAG_ENFORCE_THERMAL_AUDIO_SHUTDOWN
    if(thermal_audio_state_ == THERMAL_AUDIO_MUTED)
    {
        for(size_t i = 0; i < size; ++i)
        {
            out[0][i] = 0.0f;
            out[1][i] = 0.0f;
        }
#if AUDIO_CPU_LOAD_DEBUG
        FinishCpuLoadMeasurement();
#endif
        return;
    }
#endif

#if DIAG_ENGINE_CAPTURE_TUNER
    if(tuner_capture_ && thermal_audio_state_ == THERMAL_AUDIO_NORMAL)
        tuner_capture_->WriteBlock(in[0], size);
#endif

    for(size_t i = 0; i < size; ++i)
    {
#if DIAG_ENFORCE_THERMAL_AUDIO_SHUTDOWN
        if(thermal_audio_state_ == THERMAL_AUDIO_MUTED)
        {
            out[0][i] = 0.0f;
            out[1][i] = 0.0f;
            continue;
        }
#endif

        const float x = in[0][i];
#if DIAG_ENGINE_TRANSITIONS
        float wet_l = x;
        float wet_r = x;
#endif
#if DIAG_ENGINE_DISPATCH
        bkshepherd::BaseEffectModule* fx = effects_[active_effect_];

#if DIAG_ENGINE_PROCESS_FX
        if(fx && active_effect_ != config::FX_TUNER
           && !(active_bypass_ && transition_ == TRANSITION_STEADY))
            ProcessEffectSample(fx, x, wet_l, wet_r);
#else
        (void)fx;
#endif
#endif

#if DIAG_ENGINE_TRANSITIONS
#if DIAG_ENFORCE_THERMAL_AUDIO_SHUTDOWN
        out[0][i] = thermal_gain_ * ClampOutput(x + wet_gain_ * (wet_l - x));
        out[1][i] = thermal_gain_ * ClampOutput(x + wet_gain_ * (wet_r - x));
#else
        out[0][i] = ClampOutput(x + wet_gain_ * (wet_l - x));
        out[1][i] = ClampOutput(x + wet_gain_ * (wet_r - x));
#endif
#else
        out[0][i] = x;
        out[1][i] = x;
#endif

#if DIAG_ENFORCE_THERMAL_AUDIO_SHUTDOWN
        if(thermal_audio_state_ == THERMAL_AUDIO_FADING)
        {
            thermal_gain_ -= thermal_gain_step_;
            if(thermal_gain_ <= 0.0f)
            {
                thermal_gain_        = 0.0f;
                thermal_audio_state_ = THERMAL_AUDIO_MUTED;
            }
        }
        else
#endif
#if DIAG_ENGINE_TRANSITIONS
        {
            StepTransition();
        }
#endif
    }

#if AUDIO_CPU_LOAD_DEBUG
    FinishCpuLoadMeasurement();
    const float peak = cpu_load_.GetMaxCpuLoad();
    if(metered_effect < config::FX_COUNT && peak == peak
       && peak > effect_peak_load_[metered_effect])
        effect_peak_load_[metered_effect] = peak;
#endif
}

#if AUDIO_CPU_LOAD_DEBUG
void AudioEngine::FinishCpuLoadMeasurement()
{
    // Preserve libDaisy's average/historical-maximum measurements, then use a
    // separate current-block delta so one old peak cannot count repeatedly.
    cpu_load_.OnBlockEnd();
    const uint32_t elapsed_ticks
        = daisy::System::GetTick() - cpu_block_start_ticks_;
    const float current_block_load
        = static_cast<float>(elapsed_ticks) * cpu_ticks_per_block_inv_;
    if(current_block_load > 1.0f)
        ++overrun_count_;
}
#endif

// Case-insensitive substring helper used only while building parameter maps during initialization.
bool AudioEngine::StringContainsCi(const char* haystack, const char* needle)
{
    if(!haystack || !needle)
        return false;
    for(const char* h = haystack; *h; ++h)
    {
        const char* h2 = h;
        const char* n  = needle;
        while(*h2 && *n)
        {
            char ch = *h2;
            char cn = *n;
            if(ch >= 'A' && ch <= 'Z')
                ch = (char)(ch - 'A' + 'a');
            if(cn >= 'A' && cn <= 'Z')
                cn = (char)(cn - 'A' + 'a');
            if(ch != cn)
                break;
            ++h2;
            ++n;
        }
        if(*n == '\0')
            return true;
    }
    return false;
}

// Find a module parameter by its display name when the imported module does not expose a better typed ID.
int AudioEngine::FindParamByKeywords(bkshepherd::BaseEffectModule* fx,
                                     const char*                   kw1,
                                     const char*                   kw2,
                                     const char*                   kw3)
{
    if(!fx)
        return -1;
    for(int i = 0; i < (int)fx->GetParameterCount(); ++i)
    {
        const char* name = fx->GetParameterName(i);
        if(StringContainsCi(name, kw1) || (kw2 && StringContainsCi(name, kw2))
           || (kw3 && StringContainsCi(name, kw3)))
            return i;
    }
    return -1;
}

// Fall back to the module-defined knob mapping, then to positional parameters, preserving old module behavior.
int AudioEngine::FallbackParamForPot(bkshepherd::BaseEffectModule* fx,
                                     int                           pot_idx)
{
    if(!fx)
        return -1;
    const int pid = fx->GetMappedParameterIDForKnob(pot_idx);
    if(pid >= 0)
        return pid;
    return pot_idx < (int)fx->GetParameterCount() ? pot_idx : -1;
}

float AudioEngine::U16ToFloat(uint16_t value)
{
    return (float)value / 65535.0f;
}

// Preserve the existing simple output limiter so summed dry/wet transitions cannot exceed the expected range.
float AudioEngine::ClampOutput(float value)
{
    return value < -0.95f ? -0.95f : (value > 0.95f ? 0.95f : value);
}

// Map the three pots to the parameters that make sense for each effect.
// New production effects use explicit parameter IDs; imported legacy effects
// retain their existing name-based compatibility mapping.
void AudioEngine::BuildParamMaps()
{
#if DIAG_FX_PHASER
    maps_[config::FX_PHASER].p0 = bkshepherd::PhaserModule::MIX;
    maps_[config::FX_PHASER].p1 = bkshepherd::PhaserModule::RATE;
    maps_[config::FX_PHASER].p2 = bkshepherd::PhaserModule::DEPTH;
#endif

#if DIAG_FX_REVERB
    maps_[config::FX_REVERB].p0
        = FindParamByKeywords(&reverb_, "mix", "wet", 0);
    maps_[config::FX_REVERB].p1
        = FindParamByKeywords(&reverb_, "time", "decay", "feedback");
    maps_[config::FX_REVERB].p2
        = FindParamByKeywords(&reverb_, "lpfreq", "damp", "tone");
    if(maps_[config::FX_REVERB].p0 < 0)
        maps_[config::FX_REVERB].p0 = FallbackParamForPot(&reverb_, 0);
    if(maps_[config::FX_REVERB].p1 < 0)
        maps_[config::FX_REVERB].p1 = FallbackParamForPot(&reverb_, 1);
    if(maps_[config::FX_REVERB].p2 < 0)
        maps_[config::FX_REVERB].p2 = FallbackParamForPot(&reverb_, 2);
#endif

#if DIAG_FX_PITCH_SHIFTER
    maps_[config::FX_PITCH_SHIFTER].p0
        = bkshepherd::PitchShifterModule::SEMITONE;
    maps_[config::FX_PITCH_SHIFTER].p1
        = bkshepherd::PitchShifterModule::CROSSFADE;
    maps_[config::FX_PITCH_SHIFTER].p2
        = bkshepherd::PitchShifterModule::DIRECTION;
#endif

#if DIAG_FX_GRANULAR
    maps_[config::FX_GRANULAR_DELAY].p0 = bkshepherd::GranularDelayModule::SIZE;
    maps_[config::FX_GRANULAR_DELAY].p1 = bkshepherd::GranularDelayModule::MIX;
    maps_[config::FX_GRANULAR_DELAY].p2
        = bkshepherd::GranularDelayModule::PITCH;
#endif
}

// The main loop publishes requested state into the mailbox; the audio thread
// consumes it here and only re-applies parameters when the revision changes.
void AudioEngine::ApplySnapshotIfChanged()
{
    if(!mailbox_)
        return;

    ControlSnapshot snapshot;
    uint32_t        revision = mailbox_revision_;
    if(!mailbox_->Read(snapshot, revision) || revision == mailbox_revision_)
        return;

    mailbox_revision_ = revision;
    current_          = snapshot;
#if DIAG_STAGE != DIAG_STAGE_PRODUCTION
    current_.effect = (uint8_t)DIAG_FORCE_EFFECT;
    current_.bypass = 0;
#endif
#if DIAG_ENFORCE_THERMAL_AUDIO_SHUTDOWN
    if(current_.system_mode == (uint8_t)SystemMode::ThermalShutdown)
    {
        BeginThermalShutdown();
        return;
    }
    if(thermal_audio_state_ != THERMAL_AUDIO_NORMAL)
        return;
#endif
    if(current_.effect >= config::FX_COUNT)
        current_.effect = config::FX_TUNER;

    const bool effect_changed = current_.effect != applied_.effect;
    const bool params_changed
        = current_.parameters[0] != applied_.parameters[0]
          || current_.parameters[1] != applied_.parameters[1]
          || current_.parameters[2] != applied_.parameters[2];

#if DIAG_ENGINE_PROCESS_FX
    if(params_changed || effect_changed)
        ApplyParameters(current_.effect, current_.parameters);
#else
    (void)params_changed;
    (void)effect_changed;
#endif

#if DIAG_ENGINE_TRANSITIONS
    if(current_.effect != pending_effect_
       || (current_.bypass != 0) != pending_bypass_)
        StartTransition(current_.effect, current_.bypass != 0);
#endif

    applied_ = current_;
}

void AudioEngine::BeginThermalShutdown()
{
    if(thermal_audio_state_ != THERMAL_AUDIO_NORMAL)
        return;
    thermal_audio_state_ = THERMAL_AUDIO_FADING;
    thermal_gain_        = 1.0f;
}

// Fade out the old wet signal before changing modes, so bypass and effect
// switches do not click or cut off the previous effect abruptly.
void AudioEngine::StartTransition(uint8_t requested_effect,
                                  bool    requested_bypass)
{
    pending_effect_ = requested_effect;
    pending_bypass_ = requested_bypass;

    const bool audible = wet_gain_ > 0.0f && active_effect_ != config::FX_TUNER
                         && !active_bypass_;
    if(audible)
        transition_ = TRANSITION_FADING_OUT;
    else
    {
        active_effect_ = pending_effect_;
        active_bypass_ = pending_bypass_;
        wet_gain_      = 0.0f;
        transition_    = (active_effect_ != config::FX_TUNER && !active_bypass_)
                          ? TRANSITION_FADING_IN
                          : TRANSITION_STEADY;
    }
}

// Advance the wet/dry ramp once per sample. When the ramp reaches zero or
// one, the engine either switches modes or returns to steady state.
void AudioEngine::StepTransition()
{
    if(transition_ == TRANSITION_FADING_OUT)
    {
        wet_gain_ -= wet_gain_step_;
        if(wet_gain_ <= 0.0f)
        {
            wet_gain_      = 0.0f;
            active_effect_ = pending_effect_;
            active_bypass_ = pending_bypass_;
            transition_
                = (active_effect_ != config::FX_TUNER && !active_bypass_)
                      ? TRANSITION_FADING_IN
                      : TRANSITION_STEADY;
        }
    }
    else if(transition_ == TRANSITION_FADING_IN)
    {
        wet_gain_ += wet_gain_step_;
        if(wet_gain_ >= 1.0f)
        {
            wet_gain_   = 1.0f;
            transition_ = TRANSITION_STEADY;
        }
    }
}

// Process one sample through the active module and retrieve both output channels.
void AudioEngine::ProcessEffectSample(bkshepherd::BaseEffectModule* fx,
                                      float                         in,
                                      float&                        out_l,
                                      float&                        out_r)
{
    fx->ProcessMono(in);
    out_l = fx->GetAudioLeft();
    out_r = fx->GetAudioRight();
}

// Convert mailbox fixed-point pots back to normalized floats only inside the callback-owned engine.
void AudioEngine::ApplyParameters(uint8_t effect, const uint16_t* parameters)
{
    if(effect == config::FX_TUNER || effect >= config::FX_COUNT)
        return;

    bkshepherd::BaseEffectModule* fx = effects_[effect];
    if(!fx)
        return;

    const PotParamMap& map = maps_[effect];
    if(map.p0 >= 0)
        fx->SetParameterAsMagnitude(map.p0, U16ToFloat(parameters[0]));
    if(map.p1 >= 0)
        fx->SetParameterAsMagnitude(map.p1, U16ToFloat(parameters[1]));
    if(map.p2 >= 0)
        fx->SetParameterAsMagnitude(map.p2, U16ToFloat(parameters[2]));
}

bkshepherd::BaseEffectModule* AudioEngine::CurrentEffect()
{
    if(active_effect_ == config::FX_TUNER || active_effect_ >= config::FX_COUNT)
        return 0;
    return effects_[active_effect_];
}

#if AUDIO_CPU_LOAD_DEBUG
AudioEngine::CpuLoadSnapshot AudioEngine::GetCpuLoadSnapshot() const
{
    CpuLoadSnapshot snapshot = {cpu_load_.GetAvgCpuLoad(),
                                cpu_load_.GetMaxCpuLoad(),
                                {0.0f, 0.0f, 0.0f, 0.0f, 0.0f},
                                overrun_count_};
    for(int i = 0; i < config::FX_COUNT; ++i)
        snapshot.effect_peak[i] = effect_peak_load_[i];
    return snapshot;
}
#endif

} // namespace app

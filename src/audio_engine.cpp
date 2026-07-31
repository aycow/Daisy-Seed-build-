#include "audio_engine.h"

namespace app
{

AudioEngine::AudioEngine()
: mailbox_(0),
  tuner_capture_(0),
  chorus_(),
  reverb_(),
  crusher_(),
  granular_delay_(),
  effects_{0, &chorus_, &reverb_, &crusher_, &granular_delay_},
  maps_{{-1, -1, -1}, {-1, -1, -1}, {-1, -1, -1}, {-1, -1, -1}, {-1, -1, -1}},
  current_{config::FX_TUNER, 0, {0, 0, 0}},
  applied_{0xffu, 0xffu, {0xffffu, 0xffffu, 0xffffu}},
  mailbox_revision_(0),
  active_effect_(config::FX_TUNER),
  active_bypass_(false),
  pending_effect_(config::FX_TUNER),
  pending_bypass_(false),
  transition_(TRANSITION_STEADY),
  wet_gain_(0.0f),
  wet_gain_step_(1.0f / (config::kSampleRateHz * (config::kTransitionTimeMs * 0.001f)))
#if AUDIO_CPU_LOAD_DEBUG
  , cpu_load_(),
  effect_peak_load_{0.0f, 0.0f, 0.0f, 0.0f, 0.0f},
  overrun_count_(0)
#endif
{
}

void AudioEngine::Init(float sample_rate, ControlMailbox& mailbox, TunerCapture& tuner_capture)
{
    mailbox_ = &mailbox;
    tuner_capture_ = &tuner_capture;

    chorus_.Init(sample_rate);
    reverb_.Init(sample_rate);
    crusher_.Init(sample_rate);
    granular_delay_.Init(sample_rate);
    chorus_.SetEnabled(true);
    reverb_.SetEnabled(true);
    crusher_.SetEnabled(true);
    granular_delay_.SetEnabled(true);

    BuildParamMaps();
#if AUDIO_CPU_LOAD_DEBUG
    cpu_load_.Init(sample_rate, config::kAudioBlockSize);
    for(int i = 0; i < config::FX_COUNT; ++i)
        effect_peak_load_[i] = 0.0f;
    overrun_count_ = 0;
#endif
    ApplySnapshotIfChanged();
    active_effect_ = current_.effect;
    active_bypass_ = current_.bypass != 0;
    pending_effect_ = active_effect_;
    pending_bypass_ = active_bypass_;
    wet_gain_ = (active_effect_ == config::FX_TUNER || active_bypass_) ? 0.0f : 1.0f;
}

// The callback reads one control snapshot per block, then keeps audio work
// deterministic: capture input, process the current effect, and apply any
// pending fade state sample by sample.
void AudioEngine::Process(daisy::AudioHandle::InputBuffer in,
                          daisy::AudioHandle::OutputBuffer out,
                          size_t size)
{
#if AUDIO_CPU_LOAD_DEBUG
    const uint8_t metered_effect = active_effect_;
    cpu_load_.OnBlockStart();
#endif

    ApplySnapshotIfChanged();

    if(tuner_capture_)
        tuner_capture_->WriteBlock(in[0], size);

    for(size_t i = 0; i < size; ++i)
    {
        const float x = in[0][i];
        float wet_l = x;
        float wet_r = x;
        bkshepherd::BaseEffectModule* fx = effects_[active_effect_];

        if(fx && active_effect_ != config::FX_TUNER && !(active_bypass_ && transition_ == TRANSITION_STEADY))
            ProcessEffectSample(fx, x, wet_l, wet_r);

        out[0][i] = ClampOutput(x + wet_gain_ * (wet_l - x));
        out[1][i] = ClampOutput(x + wet_gain_ * (wet_r - x));

        StepTransition();
    }

#if AUDIO_CPU_LOAD_DEBUG
    cpu_load_.OnBlockEnd();
    const float peak = cpu_load_.GetMaxCpuLoad();
    if(metered_effect < config::FX_COUNT && peak == peak && peak > effect_peak_load_[metered_effect])
        effect_peak_load_[metered_effect] = peak;
    if(peak > 1.0f)
        ++overrun_count_;
#endif
}

// Case-insensitive substring helper used only while building parameter maps during initialization.
bool AudioEngine::StringContainsCi(const char* haystack, const char* needle)
{
    if(!haystack || !needle)
        return false;
    for(const char* h = haystack; *h; ++h)
    {
        const char* h2 = h;
        const char* n = needle;
        while(*h2 && *n)
        {
            char ch = *h2;
            char cn = *n;
            if(ch >= 'A' && ch <= 'Z') ch = (char)(ch - 'A' + 'a');
            if(cn >= 'A' && cn <= 'Z') cn = (char)(cn - 'A' + 'a');
            if(ch != cn) break;
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
                                     const char* kw1,
                                     const char* kw2,
                                     const char* kw3)
{
    if(!fx)
        return -1;
    for(int i = 0; i < (int)fx->GetParameterCount(); ++i)
    {
        const char* name = fx->GetParameterName(i);
        if(StringContainsCi(name, kw1) || (kw2 && StringContainsCi(name, kw2)) || (kw3 && StringContainsCi(name, kw3)))
            return i;
    }
    return -1;
}

// Fall back to the module-defined knob mapping, then to positional parameters, preserving old module behavior.
int AudioEngine::FallbackParamForPot(bkshepherd::BaseEffectModule* fx, int pot_idx)
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
// Granular delay uses explicit parameter IDs so the mapping stays obvious.
void AudioEngine::BuildParamMaps()
{
    maps_[config::FX_CHORUS].p0 = FindParamByKeywords(&chorus_, "mix", "wet", 0);
    maps_[config::FX_CHORUS].p1 = FindParamByKeywords(&chorus_, "rate", "speed", "freq");
    maps_[config::FX_CHORUS].p2 = FindParamByKeywords(&chorus_, "depth", 0, 0);
    if(maps_[config::FX_CHORUS].p0 < 0) maps_[config::FX_CHORUS].p0 = FallbackParamForPot(&chorus_, 0);
    if(maps_[config::FX_CHORUS].p1 < 0) maps_[config::FX_CHORUS].p1 = FallbackParamForPot(&chorus_, 1);
    if(maps_[config::FX_CHORUS].p2 < 0) maps_[config::FX_CHORUS].p2 = FallbackParamForPot(&chorus_, 2);

    maps_[config::FX_REVERB].p0 = FindParamByKeywords(&reverb_, "mix", "wet", 0);
    maps_[config::FX_REVERB].p1 = FindParamByKeywords(&reverb_, "time", "decay", "feedback");
    maps_[config::FX_REVERB].p2 = FindParamByKeywords(&reverb_, "lpfreq", "damp", "tone");
    if(maps_[config::FX_REVERB].p0 < 0) maps_[config::FX_REVERB].p0 = FallbackParamForPot(&reverb_, 0);
    if(maps_[config::FX_REVERB].p1 < 0) maps_[config::FX_REVERB].p1 = FallbackParamForPot(&reverb_, 1);
    if(maps_[config::FX_REVERB].p2 < 0) maps_[config::FX_REVERB].p2 = FallbackParamForPot(&reverb_, 2);

    maps_[config::FX_CRUSHER].p0 = FindParamByKeywords(&crusher_, "level", "gain", "volume");
    maps_[config::FX_CRUSHER].p1 = FindParamByKeywords(&crusher_, "bits", "crush", "depth");
    maps_[config::FX_CRUSHER].p2 = FindParamByKeywords(&crusher_, "cutoff", "filter", "tone");
    if(maps_[config::FX_CRUSHER].p0 < 0) maps_[config::FX_CRUSHER].p0 = FallbackParamForPot(&crusher_, 0);
    if(maps_[config::FX_CRUSHER].p1 < 0) maps_[config::FX_CRUSHER].p1 = FallbackParamForPot(&crusher_, 1);
    if(maps_[config::FX_CRUSHER].p2 < 0) maps_[config::FX_CRUSHER].p2 = FallbackParamForPot(&crusher_, 2);

    maps_[config::FX_GRANULAR_DELAY].p0 = bkshepherd::GranularDelayModule::SIZE;
    maps_[config::FX_GRANULAR_DELAY].p1 = bkshepherd::GranularDelayModule::MIX;
    maps_[config::FX_GRANULAR_DELAY].p2 = bkshepherd::GranularDelayModule::PITCH;
}

// The main loop publishes requested state into the mailbox; the audio thread
// consumes it here and only re-applies parameters when the revision changes.
void AudioEngine::ApplySnapshotIfChanged()
{
    if(!mailbox_)
        return;

    ControlSnapshot snapshot;
    uint32_t revision = mailbox_revision_;
    if(!mailbox_->Read(snapshot, revision) || revision == mailbox_revision_)
        return;

    mailbox_revision_ = revision;
    current_ = snapshot;
    if(current_.effect >= config::FX_COUNT)
        current_.effect = config::FX_TUNER;

    const bool effect_changed = current_.effect != applied_.effect;
    const bool params_changed = current_.parameters[0] != applied_.parameters[0]
        || current_.parameters[1] != applied_.parameters[1]
        || current_.parameters[2] != applied_.parameters[2];

    if(params_changed || effect_changed)
        ApplyParameters(current_.effect, current_.parameters);

    if(current_.effect != pending_effect_ || (current_.bypass != 0) != pending_bypass_)
        StartTransition(current_.effect, current_.bypass != 0);

    applied_ = current_;
}

// Fade out the old wet signal before changing modes, so bypass and effect
// switches do not click or cut off the previous effect abruptly.
void AudioEngine::StartTransition(uint8_t requested_effect, bool requested_bypass)
{
    pending_effect_ = requested_effect;
    pending_bypass_ = requested_bypass;

    const bool audible = wet_gain_ > 0.0f && active_effect_ != config::FX_TUNER && !active_bypass_;
    if(audible)
        transition_ = TRANSITION_FADING_OUT;
    else
    {
        active_effect_ = pending_effect_;
        active_bypass_ = pending_bypass_;
        wet_gain_ = 0.0f;
        transition_ = (active_effect_ != config::FX_TUNER && !active_bypass_) ? TRANSITION_FADING_IN : TRANSITION_STEADY;
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
            wet_gain_ = 0.0f;
            active_effect_ = pending_effect_;
            active_bypass_ = pending_bypass_;
            transition_ = (active_effect_ != config::FX_TUNER && !active_bypass_) ? TRANSITION_FADING_IN : TRANSITION_STEADY;
        }
    }
    else if(transition_ == TRANSITION_FADING_IN)
    {
        wet_gain_ += wet_gain_step_;
        if(wet_gain_ >= 1.0f)
        {
            wet_gain_ = 1.0f;
            transition_ = TRANSITION_STEADY;
        }
    }
}

// Process one sample through the active module and retrieve both output channels.
void AudioEngine::ProcessEffectSample(bkshepherd::BaseEffectModule* fx, float in, float& out_l, float& out_r)
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
    if(map.p0 >= 0) fx->SetParameterAsMagnitude(map.p0, U16ToFloat(parameters[0]));
    if(map.p1 >= 0) fx->SetParameterAsMagnitude(map.p1, U16ToFloat(parameters[1]));
    if(map.p2 >= 0) fx->SetParameterAsMagnitude(map.p2, U16ToFloat(parameters[2]));
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
    CpuLoadSnapshot snapshot = {cpu_load_.GetAvgCpuLoad(), cpu_load_.GetMaxCpuLoad(), {0.0f, 0.0f, 0.0f, 0.0f, 0.0f}, overrun_count_};
    for(int i = 0; i < config::FX_COUNT; ++i)
        snapshot.effect_peak[i] = effect_peak_load_[i];
    return snapshot;
}
#endif

} // namespace app
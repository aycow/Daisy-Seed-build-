#include "reverb_module.h"

using namespace bkshepherd;

static const int s_paramCount = 3;
static const ParameterMetaData s_metaData[s_paramCount] = {{name: "Time", valueType: ParameterValueType::FloatMagnitude, valueBinCount: 0, defaultValue: 57, knobMapping: 0, midiCCMapping: 1},
                                                           {name: "Damp", valueType: ParameterValueType::FloatMagnitude, valueBinCount: 0, defaultValue: 40, knobMapping: 1, midiCCMapping: 21},
                                                           {name: "Mix", valueType: ParameterValueType::FloatMagnitude, valueBinCount: 0, defaultValue: 57, knobMapping: 2, midiCCMapping: 22}};

ReverbModule::ReverbModule() : BaseEffectModule(),
                                        m_timeMin(0.6f),
                                        m_timeMax(1.0f),
                                        m_lpFreqMin(600.0f),
                                        m_lpFreqMax(16000.0f),
                                        m_feedback(0.6f),
                                        m_lpFreq(16000.0f),
                                        m_mix(0.0f)
{
    m_name = "Reverb";
    m_paramMetaData = s_metaData;
    this->InitParams(s_paramCount);
}

ReverbModule::~ReverbModule()
{
}

void ReverbModule::Init(float sample_rate)
{
    BaseEffectModule::Init(sample_rate);
    m_reverbStereo.Init(sample_rate);
    for(int i = 0; i < s_paramCount; ++i)
        ParameterChanged(i);
}

void ReverbModule::ParameterChanged(int parameter_id)
{
    (void)parameter_id;
    m_feedback = m_timeMin + GetParameterAsMagnitude(0) * (m_timeMax - m_timeMin);
    float invertedFreq = 1.0f - GetParameterAsMagnitude(1);
    invertedFreq = invertedFreq * invertedFreq;
    m_lpFreq = m_lpFreqMin + invertedFreq * (m_lpFreqMax - m_lpFreqMin);
    m_mix = GetParameterAsMagnitude(2);

    m_reverbStereo.SetFeedback(m_feedback);
    m_reverbStereo.SetLpFreq(m_lpFreq);
}

void ReverbModule::ProcessMono(float in)
{
    BaseEffectModule::ProcessMono(in);

    float wetl, wetr;
    const float sendl = m_audioLeft;
    const float sendr = m_audioRight;
    m_reverbStereo.Process(sendl, sendr, &wetl, &wetr);
    m_audioLeft = wetl * m_mix + sendl * (1.0f - m_mix);
    m_audioRight = wetr * m_mix + sendr * (1.0f - m_mix);
}

void ReverbModule::ProcessStereo(float inL, float inR)
{
    BaseEffectModule::ProcessStereo(inL, inR);

    float wetl, wetr;
    const float sendl = m_audioLeft;
    const float sendr = m_audioRight;
    m_reverbStereo.Process(sendl, sendr, &wetl, &wetr);
    m_audioLeft = wetl * m_mix + inL * (1.0f - m_mix);
    m_audioRight = wetr * m_mix + inR * (1.0f - m_mix);
}

float ReverbModule::GetBrightnessForLED(int led_id)
{
    float value = BaseEffectModule::GetBrightnessForLED(led_id);
    if (led_id == 1)
        return value;
    return value;
}

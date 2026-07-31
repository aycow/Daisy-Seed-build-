#include "chorus_module.h"

using namespace bkshepherd;

static const int s_paramCount = 5;
static const ParameterMetaData s_metaData[s_paramCount] = {{name: "Wet", valueType: ParameterValueType::FloatMagnitude, valueBinCount: 0, defaultValue: 83, knobMapping: 0, midiCCMapping: 20},
                                                           {name: "Delay", valueType: ParameterValueType::FloatMagnitude, valueBinCount: 0, defaultValue: 64, knobMapping: 1, midiCCMapping: 21},
                                                           {name: "LFO Freq", valueType: ParameterValueType::FloatMagnitude, valueBinCount: 0, defaultValue: 35, knobMapping: 2, midiCCMapping: 22},
                                                           {name: "LFO Depth", valueType: ParameterValueType::FloatMagnitude, valueBinCount: 0, defaultValue: 40, knobMapping: 3, midiCCMapping: 23},
                                                           {name: "Feedback", valueType: ParameterValueType::FloatMagnitude, valueBinCount: 0, defaultValue: 30, knobMapping: 4, midiCCMapping: 24}};

ChorusModule::ChorusModule() : BaseEffectModule(),
                                        m_lfoFreqMin(1.0f),
                                        m_lfoFreqMax(20.0f),
                                        m_wet(0.0f),
                                        m_delay(0.0f),
                                        m_lfoFreq(1.0f),
                                        m_lfoDepth(0.0f),
                                        m_feedback(0.0f)
{
    m_name = "Chorus";
    m_paramMetaData = s_metaData;
    this->InitParams(s_paramCount);
}

ChorusModule::~ChorusModule()
{
}

void ChorusModule::Init(float sample_rate)
{
    BaseEffectModule::Init(sample_rate);
    m_chorus.Init(sample_rate);
    for(int i = 0; i < s_paramCount; ++i)
        ParameterChanged(i);
}

void ChorusModule::ParameterChanged(int parameter_id)
{
    (void)parameter_id;
    m_wet = GetParameterAsMagnitude(0);
    m_delay = GetParameterAsMagnitude(1);
    const float lfo = GetParameterAsMagnitude(2);
    m_lfoFreq = m_lfoFreqMin + (lfo * lfo * (m_lfoFreqMax - m_lfoFreqMin));
    m_lfoDepth = GetParameterAsMagnitude(3);
    m_feedback = GetParameterAsMagnitude(4);

    m_chorus.SetDelay(m_delay);
    m_chorus.SetLfoFreq(m_lfoFreq);
    m_chorus.SetLfoDepth(m_lfoDepth);
    m_chorus.SetFeedback(m_feedback);
}

void ChorusModule::ProcessMono(float in)
{
    BaseEffectModule::ProcessMono(in);
    m_chorus.Process(m_audioLeft);
    m_audioLeft = m_chorus.GetLeft() * m_wet + m_audioLeft * (1.0f - m_wet);
    m_audioRight = m_chorus.GetRight() * m_wet + m_audioRight * (1.0f - m_wet);
}

void ChorusModule::ProcessStereo(float inL, float inR)
{
    ProcessMono(inL);
    BaseEffectModule::ProcessStereo(m_audioLeft, inR);
    m_audioRight = m_chorus.GetRight() * m_wet + m_audioRight * (1.0f - m_wet);
}

float ChorusModule::GetBrightnessForLED(int led_id)
{
    float value = BaseEffectModule::GetBrightnessForLED(led_id);
    if (led_id == 1)
        return value * m_wet;
    return value;
}

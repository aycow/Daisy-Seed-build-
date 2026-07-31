#include "crusher_module.h"

using namespace bkshepherd;

static const int s_paramCount = 3;
static const ParameterMetaData s_metaData[s_paramCount] = {
    {name : "Level", valueType : ParameterValueType::FloatMagnitude, valueBinCount : 0, defaultValue : 40, knobMapping : 0, midiCCMapping : -1},
    {name : "Bits", valueType : ParameterValueType::Binned, valueBinCount : 32, defaultValue : 32, knobMapping : 1, midiCCMapping : -1},
    {name : "Cutoff", valueType : ParameterValueType::FloatMagnitude, valueBinCount : 0, defaultValue : 64, knobMapping : 2, midiCCMapping : -1}};

CrusherModule::CrusherModule() : BaseEffectModule(),
                                  m_levelMin(0.01f),
                                  m_levelMax(20.0f),
                                  m_cutoffMin(500.0f),
                                  m_cutoffMax(20000.0f),
                                  m_level(1.0f),
                                  m_bits(32.0f),
                                  m_cutoff(20000.0f)
{
  m_name = "Crusher";
  m_paramMetaData = s_metaData;
  this->InitParams(s_paramCount);
}

CrusherModule::~CrusherModule()
{
}

void CrusherModule::Init(float sample_rate)
{
  BaseEffectModule::Init(sample_rate);
  m_tone.Init(sample_rate);
  m_bitcrusher.Init();
  for(int i = 0; i < s_paramCount; ++i)
    ParameterChanged(i);
}

void CrusherModule::ParameterChanged(int parameter_id)
{
  (void)parameter_id;
  m_level = m_levelMin + (GetParameterAsMagnitude(0) * (m_levelMax - m_levelMin));
  m_bits = (float)GetParameterAsBinnedValue(1);
  m_cutoff = m_cutoffMin + GetParameterAsMagnitude(2) * (m_cutoffMax - m_cutoffMin);

  m_tone.SetFreq(m_cutoff);
  m_bitcrusher.setNumberOfBits(m_bits);
}

void CrusherModule::ProcessMono(float in)
{
  BaseEffectModule::ProcessMono(in);
  float out = m_bitcrusher.Process(in);
  m_audioRight = m_audioLeft = out * m_level;
}

void CrusherModule::ProcessStereo(float inL, float inR)
{
  BaseEffectModule::ProcessStereo(inL, inR);
  float outL = m_bitcrusher.Process(inL);
  float outR = m_bitcrusher.Process(inR);
  m_audioLeft = outL * m_level;
  m_audioRight = outR * m_level;
}

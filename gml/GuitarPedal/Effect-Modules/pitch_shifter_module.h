#pragma once

#include "../Util/pitch_shifter.h"
#include "base_effect_module.h"
#include "daisysp.h"

namespace bkshepherd
{
class PitchShifterModule : public BaseEffectModule
{
  public:
    enum Param
    {
        SEMITONE = 0,
        CROSSFADE,
        DIRECTION,
        MODE,
        SHIFT,
        RETURN,
        PARAM_COUNT
    };

    PitchShifterModule();
    ~PitchShifterModule();

    void Init(float sample_rate) override;
    void ProcessMono(float input) override;
    void ProcessStereo(float input_left, float input_right) override;

  protected:
    void ParameterChanged(int parameter_id) override;

  private:
    void UpdateTransposition();

    daisysp_modified::PitchShifter pitch_shifter_;
    daisysp::CrossFade             crossfade_;
    bool                           direction_down_;
    bool                           latching_;
};
static_assert(sizeof(PitchShifterModule) == 208,
              "Unexpected ARM PitchShifterModule layout; re-audit SRAM use");
} // namespace bkshepherd

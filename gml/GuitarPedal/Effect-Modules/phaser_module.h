#pragma once

#include "../Util/simple_phaser.h"
#include "base_effect_module.h"

namespace bkshepherd
{
class PhaserModule : public BaseEffectModule
{
  public:
    enum Param
    {
        MIX = 0,
        RATE,
        DEPTH,
        FEEDBACK,
        PARAM_COUNT
    };

    PhaserModule();
    ~PhaserModule();

    void  Init(float sample_rate) override;
    void  ProcessMono(float input) override;
    void  ProcessStereo(float input_left, float input_right) override;
    float GetBrightnessForLED(int led_id) override;

  protected:
    void ParameterChanged(int parameter_id) override;

  private:
    SimplePhaser phaser_;
    float        target_rate_;
    float        target_depth_;
    float        smoothed_rate_;
    float        smoothed_depth_;
    float        dry_gain_;
    float        wet_gain_;
};
static_assert(sizeof(PhaserModule) == 244,
              "Unexpected ARM PhaserModule layout; re-audit SRAM use");
} // namespace bkshepherd

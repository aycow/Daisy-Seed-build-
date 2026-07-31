#pragma once

#include "app_config.h"
#include "controls.h"
#include "daisy_seed.h"
#include "tuner.h"

namespace app
{

// Main-loop UART telemetry owner. Formatting and blocking transmit calls stay out
// of the audio callback so UART latency cannot disturb the DSP schedule.
class Telemetry
{
  public:
    Telemetry();
    void Init();

    // Production protocol messages. Keep prefixes, field order, and scaling stable
    // because the Raspberry Pi side consumes these exact records.
    void SendEffect(int effect);
    void SendPots(const float* pots);
    void SendFootswitch(bool down, const char* edge, int press_count);
    void SendTuner(const TunerResult& result);
    void SendTunerDisabled();

    // Debug-only rotary calibration output. Production builds keep this silent.
    void SendRotaryCalibration(uint16_t raw_u16, int position);
    uint32_t drop_count() const { return drop_count_; }

  private:
    static int ClampInt(int value, int lo, int hi);
    static float ClampFloat(float value, float lo, float hi);
    static int PotToU10k(float value);
    void Send(const char* message);

    daisy::UartHandler uart_;
    uint32_t drop_count_;
};

} // namespace app
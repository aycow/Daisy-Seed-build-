#include "telemetry.h"

#include <cstdio>
#include <cstring>

namespace app
{

Telemetry::Telemetry()
: uart_(),
  drop_count_(0)
{
}

// Telemetry stays in the main loop. The audio callback never touches UART, so
// transmit timing and failures cannot interfere with real-time audio.
void Telemetry::Init()
{
    daisy::UartHandler::Config cfg;
    cfg.periph = daisy::UartHandler::Config::Peripheral::USART_1;
    cfg.mode = daisy::UartHandler::Config::Mode::TX;
    cfg.baudrate = config::kUartBaudrate;
    cfg.pin_config.tx = config::kUartTxPin;
    cfg.pin_config.rx = config::kUartRxPin;
    uart_.Init(cfg);
}

// Effect selection is a compact integer protocol field: E,<fx>.
void Telemetry::SendEffect(int effect)
{
    char buf[32];
    std::snprintf(buf, sizeof(buf), "E,%d\n", effect);
    Send(buf);
}

// Pot telemetry uses integer 0..10000 scaling to avoid float formatting on the UART protocol.
void Telemetry::SendPots(const float* pots)
{
    char buf[96];
    std::snprintf(buf,
                  sizeof(buf),
                  "P,%d,%d,%d,%d\n",
                  PotToU10k(pots[0]),
                  PotToU10k(pots[1]),
                  PotToU10k(pots[2]),
                  PotToU10k(pots[3]));
    Send(buf);
}

// Footswitch telemetry reports both level and edge text so the Pi can update latch and event UI.
void Telemetry::SendFootswitch(bool down, const char* edge, int press_count)
{
    char buf[96];
    std::snprintf(buf, sizeof(buf), "S,%d,%s,%d\n", down ? 1 : 0, edge, press_count);
    Send(buf);
}

// Tuner telemetry preserves the production fields and scales frequency/cents/confidence as integers.
void Telemetry::SendTuner(const TunerResult& result)
{
    const float freq_hz = result.freq_hz < 0.0f ? 0.0f : result.freq_hz;
    const int freq_mhz = (int)(freq_hz * 1000.0f + 0.5f);
    const float cents_scaled = result.cents * 100.0f;
    const int cents_c = (int)(cents_scaled + (cents_scaled >= 0.0f ? 0.5f : -0.5f));
    const int conf_m = (int)(ClampFloat(result.confidence, 0.0f, 1.0f) * 1000.0f + 0.5f);

    char buf[128];
    std::snprintf(buf,
                  sizeof(buf),
                  "T,%d,%d,%s,%d,%d\n",
                  result.valid ? 1 : 0,
                  freq_mhz,
                  result.note,
                  cents_c,
                  conf_m);
    Send(buf);
}

// A disabled tuner packet is an explicit invalid result, not a stale last pitch.
void Telemetry::SendTunerDisabled()
{
    TunerResult result = {};
    std::snprintf(result.note, sizeof(result.note), "--");
    SendTuner(result);
}

void Telemetry::SendThermalFault(float temperature_c)
{
    const float scaled = temperature_c * 100.0f;
    const int centi_c = (int)(scaled + (scaled >= 0.0f ? 0.5f : -0.5f));
    char buf[48];
    std::snprintf(buf, sizeof(buf), "F,THERMAL,%d\n", centi_c);
    Send(buf);
}

void Telemetry::SendTemperatureSensorFault(int error_code)
{
    char buf[48];
    std::snprintf(buf, sizeof(buf), "F,TEMP_SENSOR,%d\n", error_code);
    Send(buf);
}

void Telemetry::SendThermalDebug(float temperature_c, uint16_t raw_adc, ThermalState state)
{
#if THERMAL_DEBUG_TELEMETRY
    const float scaled = temperature_c * 100.0f;
    const int centi_c = (int)(scaled + (scaled >= 0.0f ? 0.5f : -0.5f));
    char buf[64];
    std::snprintf(buf,
                  sizeof(buf),
                  "D,TEMP,%d,%u,%u\n",
                  centi_c,
                  (unsigned)raw_adc,
                  (unsigned)state);
    Send(buf);
#else
    (void)temperature_c;
    (void)raw_adc;
    (void)state;
#endif
}

void Telemetry::SendThermalCalibration(const ThermalCalibrationInfo& calibration)
{
#if THERMAL_DEBUG_TELEMETRY
    char buf[80];
    std::snprintf(buf,
                  sizeof(buf),
                  "D,TEMP_CAL,%04lX,%u,%u,%ld\n",
                  (unsigned long)(calibration.revision_id & 0xffffu),
                  (unsigned)calibration.cal1_raw,
                  (unsigned)calibration.cal2_raw,
                  (long)calibration.cal2_temperature_c);
    Send(buf);
#else
    (void)calibration;
#endif
}

void Telemetry::SendAudioCpuLoad(float average,
                                 float peak,
                                 uint32_t overruns,
                                 uint8_t effect,
                                 float effect_peak)
{
    char buf[96];
    std::snprintf(buf,
                  sizeof(buf),
                  "D,CPU,%d,%d,%lu,%u,%d\n",
                  (int)(average * 10000.0f + 0.5f),
                  (int)(peak * 10000.0f + 0.5f),
                  (unsigned long)overruns,
                  (unsigned)effect,
                  (int)(effect_peak * 10000.0f + 0.5f));
    Send(buf);
}

void Telemetry::SendRotaryCalibration(uint16_t raw_u16, int position)
{
#if ROTARY_CALIBRATION_MODE
    char buf[64];
    std::snprintf(buf, sizeof(buf), "R,%u,%d\n", (unsigned)raw_u16, position);
    Send(buf);
#else
    (void)raw_u16;
    (void)position;
#endif
}

int Telemetry::ClampInt(int value, int lo, int hi)
{
    return value < lo ? lo : (value > hi ? hi : value);
}

float Telemetry::ClampFloat(float value, float lo, float hi)
{
    return value < lo ? lo : (value > hi ? hi : value);
}

// Convert normalized pot values into the UART protocol range while clamping noise or bad inputs.
int Telemetry::PotToU10k(float value)
{
    value = ClampFloat(value, 0.0f, 1.0f);
    return ClampInt((int)(value * 10000.0f + 0.5f), 0, 10000);
}

// Use a short bounded transmit timeout and count drops instead of blocking the
// firmware if the UART or Pi side is slow.
void Telemetry::Send(const char* message)
{
    const size_t len = std::strlen(message);
    const daisy::UartHandler::Result result = uart_.BlockingTransmit((uint8_t*)message, len, config::kUartTimeoutMs);
    if(result != daisy::UartHandler::Result::OK)
        ++drop_count_;
}

} // namespace app

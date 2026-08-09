#pragma once

#include "app_config.h"

#include <stdint.h>

namespace app
{
enum class ThermalState : uint8_t
{
    Normal,
    Warning,
    Shutdown,
    SensorFault
};

enum class ThermalFaultKind : uint8_t
{
    None,
    Thermal,
    Sensor
};

enum ThermalSensorError : int
{
    THERMAL_SENSOR_OK          = 0,
    THERMAL_SENSOR_DISABLED    = 1,
    THERMAL_SENSOR_ADC_INIT    = 2,
    THERMAL_SENSOR_CALIBRATION = 3,
    THERMAL_SENSOR_CONVERSION  = 4,
    THERMAL_SENSOR_IMPLAUSIBLE = 5,
    THERMAL_SENSOR_REVISION    = 6
};

struct ThermalCalibrationInfo
{
    uint32_t revision_id;
    uint16_t cal1_raw;
    uint16_t cal2_raw;
    int32_t  cal2_temperature_c;
};

int32_t ResolveTemperatureCal2C(uint32_t revision_id);

struct ThermalSample
{
    bool     valid;
    float    temperature_c;
    uint16_t raw_temperature;
    uint16_t raw_vref;
    int      error_code;
};

// Concrete backend kept separate from policy so a future external thermistor can
// replace ADC3 acquisition without changing the safety state machine.
class InternalTemperatureSensor
{
  public:
    InternalTemperatureSensor();
    bool          Init();
    ThermalSample Read();

    static bool            ConvertTemperatureC(uint32_t raw_temperature,
                                               uint32_t raw_vref,
                                               uint32_t vref_cal,
                                               uint32_t cal1_raw,
                                               uint32_t cal2_raw,
                                               int32_t  cal1_temperature_c,
                                               int32_t  cal2_temperature_c,
                                               float&   out_temperature_c);
    ThermalCalibrationInfo calibration_info() const;

  private:
    bool ReadChannelAveraged(uint32_t channel, uint16_t& out_raw);

#if THERMAL_MONITOR_ENABLED
    ADC_HandleTypeDef adc_;
#endif
    bool     ready_;
    int      init_error_;
    uint32_t revision_id_;
    uint16_t cal1_raw_;
    uint16_t cal2_raw_;
    uint16_t vref_cal_raw_;
    int32_t  cal2_temperature_c_;
};

class ThermalPolicy
{
  public:
    ThermalPolicy();
    void Reset();
    void Process(const ThermalSample& sample);

    ThermalState state() const { return state_; }
    bool         IsSafetyLatched() const
    {
        return state_ == ThermalState::Shutdown
               || state_ == ThermalState::SensorFault;
    }
    float    filtered_temperature_c() const { return filtered_temperature_c_; }
    uint16_t last_raw_temperature() const { return last_raw_temperature_; }
    int      last_error_code() const { return last_error_code_; }
    ThermalFaultKind TakeFaultNotification();

  private:
    void Latch(ThermalState state, int error_code);

    ThermalState     state_;
    float            filter_[config::kThermalFilterLength];
    int              filter_count_;
    int              filter_index_;
    int              warning_count_;
    int              trip_count_;
    int              invalid_count_;
    float            filtered_temperature_c_;
    uint16_t         last_raw_temperature_;
    int              last_error_code_;
    ThermalFaultKind pending_fault_;
};

class ThermalMonitor
{
  public:
    ThermalMonitor();
    void Init(uint32_t now_ms, bool perform_initial_sample = true);
    bool Service(uint32_t now_ms);

    ThermalState state() const { return policy_.state(); }
    bool         IsSafetyLatched() const { return policy_.IsSafetyLatched(); }
    float        filtered_temperature_c() const
    {
        return policy_.filtered_temperature_c();
    }
    uint16_t last_raw_temperature() const
    {
        return policy_.last_raw_temperature();
    }
    int last_error_code() const { return policy_.last_error_code(); }
    ThermalFaultKind TakeFaultNotification()
    {
        return policy_.TakeFaultNotification();
    }
    bool TakeCalibrationDebug(ThermalCalibrationInfo& out);
    bool FaultLedOn(uint32_t now_ms) const;

  private:
    InternalTemperatureSensor sensor_;
    ThermalPolicy             policy_;
    uint32_t                  last_sample_ms_;
    uint32_t                  latched_at_ms_;
    bool                      initialized_;
    bool                      calibration_debug_pending_;
};

} // namespace app

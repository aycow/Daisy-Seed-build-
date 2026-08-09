#include "thermal_monitor.h"

#include "daisy_seed.h"
#include "stm32h7xx_hal.h"

namespace app
{
int32_t ResolveTemperatureCal2C(uint32_t revision_id)
{
    revision_id &= 0xffffu;
    if(revision_id == 0u || revision_id == 0xffffu)
        return 0;
    return revision_id <= 0x1003u ? 110 : 130;
}

InternalTemperatureSensor::InternalTemperatureSensor()
#if THERMAL_MONITOR_ENABLED
: adc_{},
  ready_(false),
  init_error_(THERMAL_SENSOR_ADC_INIT),
  revision_id_(0),
  cal1_raw_(0),
  cal2_raw_(0),
  vref_cal_raw_(0),
  cal2_temperature_c_(0)
#else
: ready_(false),
  init_error_(THERMAL_SENSOR_DISABLED),
  revision_id_(0),
  cal1_raw_(0),
  cal2_raw_(0),
  vref_cal_raw_(0),
  cal2_temperature_c_(0)
#endif
{
}

bool InternalTemperatureSensor::Init()
{
#if !THERMAL_MONITOR_ENABLED
    ready_      = false;
    init_error_ = THERMAL_SENSOR_DISABLED;
    return false;
#else
    revision_id_        = HAL_GetREVID() & 0xffffu;
    cal2_temperature_c_ = ResolveTemperatureCal2C(revision_id_);
    if(cal2_temperature_c_ == 0)
    {
        init_error_ = THERMAL_SENSOR_REVISION;
        return false;
    }

    cal1_raw_     = *TEMPSENSOR_CAL1_ADDR;
    cal2_raw_     = *TEMPSENSOR_CAL2_ADDR;
    vref_cal_raw_ = *VREFINT_CAL_ADDR;
    if(cal1_raw_ == 0u || cal1_raw_ == 0xffffu || cal2_raw_ == 0u
       || cal2_raw_ == 0xffffu || vref_cal_raw_ == 0u
       || vref_cal_raw_ == 0xffffu || cal2_raw_ <= cal1_raw_)
    {
        init_error_ = THERMAL_SENSOR_CALIBRATION;
        return false;
    }

    // libDaisy owns ADC1 for the four-channel DMA scan. ADC3 is not touched by
    // its AdcHandle; only ADC3 can access the H750 temperature and VREFINT paths.
    __HAL_RCC_ADC3_CLK_ENABLE();
    adc_                               = {};
    adc_.Instance                      = ADC3;
    adc_.Init.ClockPrescaler           = ADC_CLOCK_ASYNC_DIV2;
    adc_.Init.Resolution               = ADC_RESOLUTION_16B;
    adc_.Init.ScanConvMode             = ADC_SCAN_DISABLE;
    adc_.Init.EOCSelection             = ADC_EOC_SINGLE_CONV;
    adc_.Init.LowPowerAutoWait         = DISABLE;
    adc_.Init.ContinuousConvMode       = DISABLE;
    adc_.Init.NbrOfConversion          = 1;
    adc_.Init.DiscontinuousConvMode    = DISABLE;
    adc_.Init.ExternalTrigConv         = ADC_SOFTWARE_START;
    adc_.Init.ExternalTrigConvEdge     = ADC_EXTERNALTRIGCONVEDGE_NONE;
    adc_.Init.ConversionDataManagement = ADC_CONVERSIONDATA_DR;
    adc_.Init.Overrun                  = ADC_OVR_DATA_OVERWRITTEN;
    adc_.Init.LeftBitShift             = ADC_LEFTBITSHIFT_NONE;
    adc_.Init.OversamplingMode         = DISABLE;

    if(HAL_ADC_Init(&adc_) != HAL_OK
       || HAL_ADCEx_Calibration_Start(
              &adc_, ADC_CALIB_OFFSET_LINEARITY, ADC_SINGLE_ENDED)
              != HAL_OK)
    {
        init_error_ = THERMAL_SENSOR_ADC_INIT;
        return false;
    }

    ADC_ChannelConfTypeDef channel = {};
    channel.Channel                = ADC_CHANNEL_TEMPSENSOR;
    channel.Rank                   = ADC_REGULAR_RANK_1;
    channel.SamplingTime           = config::kThermalAdcSamplingTime;
    channel.SingleDiff             = ADC_SINGLE_ENDED;
    channel.OffsetNumber           = ADC_OFFSET_NONE;
    channel.Offset                 = 0;
    if(HAL_ADC_ConfigChannel(&adc_, &channel) != HAL_OK)
    {
        init_error_ = THERMAL_SENSOR_ADC_INIT;
        return false;
    }

    // HAL enables the internal path and waits the official 26 us minimum. The
    // additional millisecond delay is centralized policy margin.
    daisy::System::Delay(config::kThermalSensorStartupDelayMs);
    ready_      = true;
    init_error_ = THERMAL_SENSOR_OK;
    return true;
#endif
}

ThermalCalibrationInfo InternalTemperatureSensor::calibration_info() const
{
    ThermalCalibrationInfo info
        = {revision_id_, cal1_raw_, cal2_raw_, cal2_temperature_c_};
    return info;
}

bool InternalTemperatureSensor::ReadChannelAveraged(uint32_t  channel_id,
                                                    uint16_t& out_raw)
{
#if !THERMAL_MONITOR_ENABLED
    (void)channel_id;
    (void)out_raw;
    return false;
#else
    if(!ready_)
        return false;

    ADC_ChannelConfTypeDef channel = {};
    channel.Channel                = channel_id;
    channel.Rank                   = ADC_REGULAR_RANK_1;
    channel.SamplingTime           = config::kThermalAdcSamplingTime;
    channel.SingleDiff             = ADC_SINGLE_ENDED;
    channel.OffsetNumber           = ADC_OFFSET_NONE;
    channel.Offset                 = 0;
    if(HAL_ADC_ConfigChannel(&adc_, &channel) != HAL_OK)
        return false;

    uint32_t sum = 0;
    for(int i = 0; i < config::kThermalAdcAverageCount; ++i)
    {
        if(HAL_ADC_Start(&adc_) != HAL_OK
           || HAL_ADC_PollForConversion(&adc_, config::kThermalAdcPollTimeoutMs)
                  != HAL_OK)
        {
            HAL_ADC_Stop(&adc_);
            return false;
        }
        sum += HAL_ADC_GetValue(&adc_);
        if(HAL_ADC_Stop(&adc_) != HAL_OK)
            return false;
    }
    out_raw = (uint16_t)((sum + config::kThermalAdcAverageCount / 2)
                         / config::kThermalAdcAverageCount);
    return true;
#endif
}

bool InternalTemperatureSensor::ConvertTemperatureC(uint32_t raw_temperature,
                                                    uint32_t raw_vref,
                                                    uint32_t vref_cal,
                                                    uint32_t cal1_raw,
                                                    uint32_t cal2_raw,
                                                    int32_t  cal1_temperature_c,
                                                    int32_t  cal2_temperature_c,
                                                    float&   out_temperature_c)
{
    if(raw_temperature == 0u || raw_temperature > 0xffffu || raw_vref == 0u
       || raw_vref > 0xffffu || vref_cal == 0u || vref_cal > 0xffffu
       || cal1_raw == 0u || cal1_raw > 0xffffu || cal2_raw > 0xffffu
       || cal2_raw <= cal1_raw || cal2_temperature_c <= cal1_temperature_c)
        return false;

    const double vref_mv
        = ((double)vref_cal * (double)VREFINT_CAL_VREF) / (double)raw_vref;
    const double compensated_raw = ((double)raw_temperature * vref_mv)
                                   / (double)TEMPSENSOR_CAL_VREFANALOG;
    const int64_t denominator = (int64_t)cal2_raw - (int64_t)cal1_raw;
    if(denominator <= 0)
        return false;

    const double temperature_c
        = (double)cal1_temperature_c
          + (compensated_raw - (double)cal1_raw)
                * (double)(cal2_temperature_c - cal1_temperature_c)
                / (double)denominator;
    out_temperature_c = (float)temperature_c;
    return out_temperature_c >= config::kThermalPlausibleMinC
           && out_temperature_c <= config::kThermalPlausibleMaxC;
}

ThermalSample InternalTemperatureSensor::Read()
{
    ThermalSample sample = {false, 0.0f, 0u, 0u, init_error_};
    if(!ready_)
        return sample;

    if(!ReadChannelAveraged(ADC_CHANNEL_TEMPSENSOR, sample.raw_temperature)
       || !ReadChannelAveraged(ADC_CHANNEL_VREFINT, sample.raw_vref))
    {
        sample.error_code = THERMAL_SENSOR_CONVERSION;
        return sample;
    }

    sample.valid = ConvertTemperatureC(sample.raw_temperature,
                                       sample.raw_vref,
                                       vref_cal_raw_,
                                       cal1_raw_,
                                       cal2_raw_,
                                       TEMPSENSOR_CAL1_TEMP,
                                       cal2_temperature_c_,
                                       sample.temperature_c);
    sample.error_code
        = sample.valid ? THERMAL_SENSOR_OK : THERMAL_SENSOR_IMPLAUSIBLE;
    return sample;
}

ThermalPolicy::ThermalPolicy()
{
    Reset();
}

void ThermalPolicy::Reset()
{
    state_ = ThermalState::Normal;
    for(int i = 0; i < config::kThermalFilterLength; ++i)
        filter_[i] = 0.0f;
    filter_count_           = 0;
    filter_index_           = 0;
    warning_count_          = 0;
    trip_count_             = 0;
    invalid_count_          = 0;
    filtered_temperature_c_ = 0.0f;
    last_raw_temperature_   = 0u;
    last_error_code_        = THERMAL_SENSOR_OK;
    pending_fault_          = ThermalFaultKind::None;
}

void ThermalPolicy::Latch(ThermalState state, int error_code)
{
    if(IsSafetyLatched())
        return;
    state_           = state;
    last_error_code_ = error_code;
    pending_fault_ = state == ThermalState::Shutdown ? ThermalFaultKind::Thermal
                                                     : ThermalFaultKind::Sensor;
}

void ThermalPolicy::Process(const ThermalSample& sample)
{
    if(IsSafetyLatched())
        return;

    last_raw_temperature_ = sample.raw_temperature;
    last_error_code_      = sample.error_code;
    if(!sample.valid || sample.temperature_c < config::kThermalPlausibleMinC
       || sample.temperature_c > config::kThermalPlausibleMaxC)
    {
        warning_count_ = 0;
        trip_count_    = 0;
        if(++invalid_count_ >= config::kThermalInvalidConsecutiveSamples)
            Latch(ThermalState::SensorFault,
                  sample.error_code == THERMAL_SENSOR_OK
                      ? THERMAL_SENSOR_IMPLAUSIBLE
                      : sample.error_code);
        return;
    }

    invalid_count_ = 0;
    if(sample.temperature_c >= config::kThermalCriticalC)
    {
        filtered_temperature_c_ = sample.temperature_c;
        Latch(ThermalState::Shutdown, THERMAL_SENSOR_OK);
        return;
    }

    filter_[filter_index_] = sample.temperature_c;
    filter_index_          = (filter_index_ + 1) % config::kThermalFilterLength;
    if(filter_count_ < config::kThermalFilterLength)
        ++filter_count_;

    float sum = 0.0f;
    for(int i = 0; i < filter_count_; ++i)
        sum += filter_[i];
    filtered_temperature_c_ = sum / (float)filter_count_;

    if(filtered_temperature_c_ >= config::kThermalTripC)
    {
        warning_count_ = 0;
        if(++trip_count_ >= config::kThermalTripConsecutiveSamples)
            Latch(ThermalState::Shutdown, THERMAL_SENSOR_OK);
        return;
    }
    trip_count_ = 0;

    if(filtered_temperature_c_ >= config::kThermalWarningC)
    {
        if(++warning_count_ >= config::kThermalWarningConsecutiveSamples)
            state_ = ThermalState::Warning;
    }
    else
    {
        warning_count_ = 0;
        if(state_ == ThermalState::Warning
           && filtered_temperature_c_
                  <= config::kThermalWarningC
                         - config::kThermalWarningHysteresisC)
            state_ = ThermalState::Normal;
    }
}

ThermalFaultKind ThermalPolicy::TakeFaultNotification()
{
    const ThermalFaultKind pending = pending_fault_;
    pending_fault_                 = ThermalFaultKind::None;
    return pending;
}

ThermalMonitor::ThermalMonitor()
: sensor_(),
  policy_(),
  last_sample_ms_(0),
  latched_at_ms_(0),
  initialized_(false),
  calibration_debug_pending_(false)
{
}

void ThermalMonitor::Init(uint32_t now_ms, bool perform_initial_sample)
{
    policy_.Reset();
#if THERMAL_MONITOR_ENABLED
    calibration_debug_pending_ = sensor_.Init();
    last_sample_ms_            = now_ms - config::kThermalSamplePeriodMs;
    latched_at_ms_             = 0;
    initialized_               = true;
    if(perform_initial_sample)
        Service(now_ms);
#else
    (void)now_ms;
    (void)perform_initial_sample;
    initialized_               = false;
    calibration_debug_pending_ = false;
#endif
}

bool ThermalMonitor::TakeCalibrationDebug(ThermalCalibrationInfo& out)
{
    if(!calibration_debug_pending_)
        return false;
    calibration_debug_pending_ = false;
    out                        = sensor_.calibration_info();
    return true;
}

bool ThermalMonitor::Service(uint32_t now_ms)
{
    if(!initialized_ || policy_.IsSafetyLatched()
       || (uint32_t)(now_ms - last_sample_ms_) < config::kThermalSamplePeriodMs)
        return false;

    last_sample_ms_ = now_ms;
    policy_.Process(sensor_.Read());
    if(policy_.IsSafetyLatched() && latched_at_ms_ == 0u)
        latched_at_ms_ = now_ms;
    return true;
}

bool ThermalMonitor::FaultLedOn(uint32_t now_ms) const
{
    if(!policy_.IsSafetyLatched())
        return false;

    const uint32_t burst_ms = 6u * config::kThermalLedShortMs;
    const uint32_t cycle_ms = burst_ms + config::kThermalLedPauseMs;
    const uint32_t phase    = (uint32_t)(now_ms - latched_at_ms_) % cycle_ms;
    return phase < burst_ms
           && ((phase / config::kThermalLedShortMs) % 2u) == 0u;
}

} // namespace app

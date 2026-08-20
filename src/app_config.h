#pragma once

#include "diagnostic_config.h"
#include "daisy_seed.h"

#include <stdint.h>

namespace app
{
namespace config
{
// Audio engine timing. These values must match the hardware/audio callback setup
// so all block-based timing and DSP ramps stay deterministic.
static constexpr float kSampleRateHz = 48000.0f;
static constexpr size_t kAudioBlockSize = 48;

// The pedal has three physical knobs. UART still reports four pot fields because
// the Raspberry Pi protocol expects a fourth placeholder value.
static constexpr int kPhysicalPotCount = 3;
static constexpr int kTelemetryPotCount = 4;
static constexpr float kPotPlaceholder4 = 0.5f;

// Control timing and filtering constants. These are intentionally centralized so
// debounce windows, smoothing, and publication deadbands are easy to tune without
// hunting through callback or main-loop code.
static constexpr int kRotaryPositionCount = 6;
static constexpr uint32_t kFootswitchDebounceMs = 15;
static constexpr uint32_t kRotaryDebounceMs = 20;
static constexpr float kPotSmoothingAlpha = 0.08f;
static constexpr uint16_t kParameterDeadbandU16 = 128;

// Main-loop telemetry cadence. UART is never sent from the audio interrupt; these
// periods control how often the foreground loop emits state when nothing changes.
static constexpr uint32_t kPotTelemetryHeartbeatMs = 500;
static constexpr uint32_t kEffectTelemetryHeartbeatMs = 1000;
static constexpr uint32_t kSafetyTelemetryHeartbeatMs = 1000;
static constexpr uint32_t kTunerAnalysisPeriodMs = 80;
static constexpr uint32_t kTunerDisabledHeartbeatMs = 1000;
static constexpr uint32_t kMainLoopDelayMs = 1;

// Conservative firmware policy thresholds. These are not absolute STM32H750
// device limits and must be validated on the assembled hardware.
#ifndef THERMAL_MONITOR_ENABLED
#define THERMAL_MONITOR_ENABLED 1
#endif
#ifndef THERMAL_DEBUG_TELEMETRY
#define THERMAL_DEBUG_TELEMETRY 0
#endif
static constexpr float kThermalWarningC = 80.0f;
static constexpr float kThermalTripC = 90.0f;
static constexpr float kThermalCriticalC = 100.0f;
static constexpr float kThermalWarningHysteresisC = 5.0f;
static constexpr float kThermalPlausibleMinC = -20.0f;
static constexpr float kThermalPlausibleMaxC = 130.0f;
static constexpr uint32_t kThermalSamplePeriodMs = 100;
static constexpr int kThermalTripConsecutiveSamples = 3;
static constexpr int kThermalWarningConsecutiveSamples = 3;
static constexpr int kThermalInvalidConsecutiveSamples = 3;
static constexpr int kThermalFilterLength = 4;
static constexpr int kThermalAdcAverageCount = 8;
static constexpr float kThermalAudioFadeMs = 10.0f;
static constexpr uint32_t kThermalSensorStartupDelayMs = 1;
static constexpr uint32_t kThermalAdcSamplingTime = ADC_SAMPLETIME_810CYCLES_5;
static constexpr uint32_t kThermalAdcPollTimeoutMs = 2;
static constexpr uint32_t kThermalFaultLoopDelayMs = 10;
static constexpr uint32_t kThermalLedShortMs = 100;
static constexpr uint32_t kThermalLedPauseMs = 700;
static constexpr uint32_t kThermalDebugTelemetryPeriodMs = 500;
static constexpr bool kThermalFaultMessagesEnabled = true;
static constexpr bool kThermalWarningMessagesEnabled = false;

// Effect/bypass transitions use a short linear ramp to avoid clicks while keeping
// switching responsive on stage.
static constexpr float kTransitionTimeMs = 5.0f;

// UART settings for the Raspberry Pi telemetry link. The timeout is bounded but
// long enough for the longest production packet at 115200 baud.
static constexpr uint32_t kUartBaudrate = 115200;
static constexpr uint32_t kUartTimeoutMs = 10;

// Tuner capture and analysis sizes. The callback writes the 8192-sample ring;
// the main loop copies a 4096-sample window and decimates it by four before YIN.
static constexpr int kTunerCaptureRingSize = 8192;
static constexpr int kTunerSourceWindowSize = 4096;
static constexpr int kTunerDecimation = 4;
static constexpr int kTunerWorkSize = kTunerSourceWindowSize / kTunerDecimation;
static constexpr int kTunerSafetySamples = 256;
static constexpr float kTunerMinFrequencyHz = 27.0f;
static constexpr float kTunerMaxFrequencyHz = 1000.0f;
static constexpr float kTunerRmsGate = 0.010f;
static constexpr uint32_t kTunerDetectionGapResetMs = 1200;
static constexpr uint32_t kTunerDefaultAttackMs = 120;

// Initial analog-rotary tolerance. This is conservative for the theoretical
// 9362-count spacing, but it should be confirmed with measured hardware values.
static constexpr uint16_t kRotaryMaxCenterErrorU16 = 2800;

static_assert((kTunerCaptureRingSize & (kTunerCaptureRingSize - 1)) == 0,
              "Tuner capture ring size must be a power of two");

// Stable effect identifiers used internally and in the production E,<fx> UART
// message. Do not reorder without also updating the Raspberry Pi consumer.
enum EffectId : uint8_t
{
    FX_TUNER = 0,
    FX_PHASER = 1,
    FX_REVERB = 2,
    FX_PITCH_SHIFTER = 3,
    FX_GRANULAR_DELAY = 4,
    FX_COUNT = 5
};

// Logical ADC indices used by the controls module. The order must match the ADC
// config array built during Controls::Init().
enum AdcChannel : uint8_t
{
    ADC_POT_0 = 0,
    ADC_POT_1 = 1,
    ADC_POT_2 = 2,
    ADC_ROTARY = 3,
    ADC_COUNT = 4
};

// Physical control wiring. These are libDaisy logical pin names, not schematic
// header numbers.
static constexpr daisy::Pin kPotPins[kPhysicalPotCount] = {
    daisy::seed::A0,
    daisy::seed::A1,
    daisy::seed::A2,
};

// PCB header pin 25 is schematic ADC_3, which is libDaisy analog alias A3.
// It is not libDaisy D25 and it is not A10 on this board.
static constexpr daisy::Pin kRotaryPin = daisy::seed::A3;
static constexpr daisy::Pin kFootswitchPin = daisy::seed::D9;
static constexpr daisy::Pin kUartTxPin = daisy::seed::D13;
static constexpr daisy::Pin kUartRxPin = daisy::seed::D14;

// Theoretical calibration for seven equal 10k resistors. Measure all six detents
// on hardware and replace or confirm these values before treating calibration as
// final. Values are raw normalized ADC centers in logical throw order 1..6; this
// ladder descends as the throw number increases.
#ifndef AUDIO_CPU_LOAD_DEBUG
#if DIAG_ENGINE_PROCESS_FX && DIAG_STAGE != DIAG_STAGE_PRODUCTION
#define AUDIO_CPU_LOAD_DEBUG 1
#else
#define AUDIO_CPU_LOAD_DEBUG 0
#endif
#endif
#ifndef ROTARY_CALIBRATION_MODE
#define ROTARY_CALIBRATION_MODE 0
#endif

static constexpr uint16_t kRotaryCentersU16[kRotaryPositionCount] = {
    56173, 46811, 37449, 28086, 18724, 9362,
};
static constexpr uint16_t kRotaryHysteresisU16 = 900;

} // namespace config
} // namespace app

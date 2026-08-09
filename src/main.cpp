#include "diagnostic_config.h"
#include "daisy_seed.h"

#if DIAG_HAS_MAILBOX
#include "control_mailbox.h"
#endif
#if DIAG_HAS_CONTROLS
#include "controls.h"
#endif
#if DIAG_HAS_TELEMETRY
#include "telemetry.h"
#endif
#if DIAG_HAS_TUNER_OBJECTS
#include "tuner.h"
#endif
#if DIAG_HAS_THERMAL
#include "thermal_monitor.h"
#endif
#if DIAG_HAS_AUDIOENGINE
#include "audio_engine.h"
#endif

#if DIAG_AUDIO_MODE == DIAG_AUDIO_SINE_440
#include <cmath>
#endif

using namespace daisy;

namespace
{
template <typename T>
void RetainDiagnosticObject(T& object)
{
    asm volatile("" : : "r"(&object));
}

DaisySeed hw;

#if DIAG_HAS_MAILBOX
app::ControlMailbox control_mailbox;
#endif
#if DIAG_HAS_CONTROLS
app::Controls controls;
#endif
#if DIAG_HAS_TUNER_OBJECTS
app::TunerCapture  tuner_capture;
app::TunerAnalyzer tuner_analyzer;
#endif
#if DIAG_HAS_AUDIOENGINE
app::AudioEngine audio_engine;
#endif
#if DIAG_HAS_TELEMETRY
app::Telemetry telemetry;
#endif
#if DIAG_HAS_THERMAL
app::ThermalMonitor thermal_monitor;
#endif
#if DIAG_ANALYZE_TUNER
// Foreground CPU scratch, not DMA storage. Keep it out of .sram1_bss so the
// libDaisy audio and ADC DMA buffers remain inside the non-cacheable MPU window.
float tuner_source_window[app::config::kTunerSourceWindowSize];
#endif

void AudioCallback(AudioHandle::InputBuffer  in,
                   AudioHandle::OutputBuffer out,
                   size_t                    size)
{
#if DIAG_CAPTURE_TUNER && !DIAG_PROCESS_AUDIOENGINE
    tuner_capture.WriteBlock(in[0], size);
#endif

#if DIAG_AUDIO_MODE == DIAG_AUDIO_DIRECT_STEREO_BYPASS
    for(size_t i = 0; i < size; ++i)
    {
        out[0][i] = in[0][i];
        out[1][i] = in[1][i];
    }
#elif DIAG_AUDIO_MODE == DIAG_AUDIO_DIRECT_MONO_BYPASS
    for(size_t i = 0; i < size; ++i)
    {
        out[0][i] = in[0][i];
        out[1][i] = in[0][i];
    }
#elif DIAG_AUDIO_MODE == DIAG_AUDIO_SINE_440
    static float phase     = 0.0f;
    const float  increment = 6.28318530718f * 440.0f / 48000.0f;
    for(size_t i = 0; i < size; ++i)
    {
        const float sample = 0.1f * sinf(phase);
        phase += increment;
        if(phase >= 6.28318530718f)
            phase -= 6.28318530718f;
        out[0][i] = sample;
        out[1][i] = sample;
    }
#elif DIAG_AUDIO_MODE == DIAG_AUDIO_SILENCE
    (void)in;
    for(size_t i = 0; i < size; ++i)
    {
        out[0][i] = 0.0f;
        out[1][i] = 0.0f;
    }
#elif DIAG_AUDIO_MODE == DIAG_AUDIO_ENGINE
    audio_engine.Process(in, out, size);
#endif
}
} // namespace

int main(void)
{
    hw.Init();
    hw.SetAudioSampleRate(SaiHandle::Config::SampleRate::SAI_48KHZ);
    hw.SetAudioBlockSize(48);

#if DIAG_STAGE != DIAG_STAGE_PRODUCTION
#if DIAG_HAS_MAILBOX
    RetainDiagnosticObject(control_mailbox);
#endif
#if DIAG_HAS_CONTROLS
    RetainDiagnosticObject(controls);
#endif
#if DIAG_HAS_TELEMETRY
    RetainDiagnosticObject(telemetry);
#endif
#if DIAG_HAS_TUNER_OBJECTS
    RetainDiagnosticObject(tuner_capture);
    RetainDiagnosticObject(tuner_analyzer);
#endif
#if DIAG_HAS_THERMAL
    RetainDiagnosticObject(thermal_monitor);
#endif
#if DIAG_HAS_AUDIOENGINE
    RetainDiagnosticObject(audio_engine);
#endif
#endif

#if DIAG_STAGE == DIAG_STAGE_PRODUCTION
    telemetry.Init();
    controls.Init(hw, control_mailbox);
    thermal_monitor.Init(System::GetNow());
#if THERMAL_DEBUG_TELEMETRY
    app::ThermalCalibrationInfo thermal_calibration;
    if(thermal_monitor.TakeCalibrationDebug(thermal_calibration))
        telemetry.SendThermalCalibration(thermal_calibration);
#endif
    tuner_capture.Init();
    tuner_analyzer.Reset(System::GetNow());
    audio_engine.Init(hw.AudioSampleRate(), control_mailbox, tuner_capture);
#else
#if DIAG_INIT_CONTROLS
    controls.Init(hw, control_mailbox);
#endif
#if DIAG_INIT_TELEMETRY
    telemetry.Init();
#endif
#if DIAG_INIT_TUNER
    tuner_capture.Init();
    tuner_analyzer.Reset(System::GetNow());
#endif
#if DIAG_INIT_THERMAL
    thermal_monitor.Init(System::GetNow(), false);
#if THERMAL_DEBUG_TELEMETRY && DIAG_INIT_TELEMETRY
    app::ThermalCalibrationInfo thermal_calibration;
    if(thermal_monitor.TakeCalibrationDebug(thermal_calibration))
        telemetry.SendThermalCalibration(thermal_calibration);
#endif
#endif
#if DIAG_INIT_AUDIOENGINE
    audio_engine.Init(hw.AudioSampleRate(), control_mailbox, tuner_capture);
#endif
#endif

    hw.StartAudio(AudioCallback);

#if !DIAG_SERVICE_CONTROLS && !DIAG_SERVICE_THERMAL
    while(1) {}
#else
    uint8_t  last_sent_effect       = 0xffu;
    uint8_t  current_effect         = app::config::FX_TUNER;
    bool     previous_tuner_mode    = false;
    uint32_t last_pots_tx           = 0;
    uint32_t last_tuner_tx          = 0;
    uint32_t last_tuner_disabled_tx = 0;
    uint32_t last_rotary_cal_tx     = 0;
    uint32_t last_thermal_debug_tx  = 0;
    uint32_t last_cpu_tx            = 0;

    while(1)
    {
        const uint32_t now             = System::GetNow();

#if DIAG_SERVICE_THERMAL
        const bool     thermal_sampled = thermal_monitor.Service(now);
#if DIAG_STAGE == DIAG_STAGE_PRODUCTION
        if(thermal_monitor.IsSafetyLatched())
        {
            controls.RequestThermalShutdown();
            const app::ThermalFaultKind fault
                = thermal_monitor.TakeFaultNotification();
            if(app::config::kThermalFaultMessagesEnabled)
            {
                if(fault == app::ThermalFaultKind::Thermal)
                    telemetry.SendThermalFault(
                        thermal_monitor.filtered_temperature_c());
                else if(fault == app::ThermalFaultKind::Sensor)
                    telemetry.SendTemperatureSensorFault(
                        thermal_monitor.last_error_code());
            }
            hw.SetLed(thermal_monitor.FaultLedOn(now));
            System::Delay(app::config::kThermalFaultLoopDelayMs);
            continue;
        }
#else
        if(thermal_monitor.IsSafetyLatched())
        {
#if DIAG_ENFORCE_THERMAL_AUDIO_SHUTDOWN
            controls.RequestThermalShutdown();
#endif
            const app::ThermalFaultKind fault
                = thermal_monitor.TakeFaultNotification();
#if DIAG_INIT_TELEMETRY
            if(fault == app::ThermalFaultKind::Thermal)
                telemetry.SendThermalFault(
                    thermal_monitor.filtered_temperature_c());
            else if(fault == app::ThermalFaultKind::Sensor)
                telemetry.SendTemperatureSensorFault(
                    thermal_monitor.last_error_code());
#else
            (void)fault;
#endif
            hw.SetLed(thermal_monitor.FaultLedOn(now));
        }
#endif
#if THERMAL_DEBUG_TELEMETRY && DIAG_INIT_TELEMETRY
        if(thermal_sampled
           && (uint32_t)(now - last_thermal_debug_tx)
                  >= app::config::kThermalDebugTelemetryPeriodMs)
        {
            last_thermal_debug_tx = now;
            telemetry.SendThermalDebug(thermal_monitor.filtered_temperature_c(),
                                       thermal_monitor.last_raw_temperature(),
                                       thermal_monitor.state());
        }
#else
        (void)thermal_sampled;
        (void)last_thermal_debug_tx;
#endif
#else
        (void)last_thermal_debug_tx;
#endif

#if DIAG_SERVICE_CONTROLS
        const app::ControlTelemetryState state = controls.Process(hw, now);
        current_effect                         = state.effect;
        hw.SetLed(state.rotary_position == 1);

#if DIAG_SEND_TELEMETRY
        if(current_effect != last_sent_effect)
        {
            last_sent_effect = current_effect;
            telemetry.SendEffect(last_sent_effect);
        }
        if(state.footswitch.occurred)
            telemetry.SendFootswitch(state.footswitch.down,
                                     state.footswitch.edge,
                                     state.footswitch.press_count);
        if(state.pots_changed
           || (uint32_t)(now - last_pots_tx)
                  >= app::config::kPotTelemetryHeartbeatMs)
        {
            last_pots_tx = now;
            telemetry.SendPots(state.pots);
        }
#if ROTARY_CALIBRATION_MODE
        if((uint32_t)(now - last_rotary_cal_tx) >= 100u)
        {
            last_rotary_cal_tx = now;
            telemetry.SendRotaryCalibration(state.rotary_raw_u16,
                                            state.rotary_position);
        }
#endif
#endif

#if DIAG_ANALYZE_TUNER
        const bool tuner_mode = current_effect == app::config::FX_TUNER;
        if(tuner_mode && !previous_tuner_mode)
        {
            tuner_analyzer.Reset(now);
            last_tuner_tx = 0;
        }
        else if(!tuner_mode && previous_tuner_mode)
        {
            tuner_analyzer.Reset(now);
#if DIAG_SEND_TELEMETRY
            telemetry.SendTunerDisabled();
#endif
            last_tuner_disabled_tx = now;
        }
        previous_tuner_mode = tuner_mode;

        if(tuner_mode
           && (uint32_t)(now - last_tuner_tx)
                  >= app::config::kTunerAnalysisPeriodMs)
        {
            last_tuner_tx = now;
            if(tuner_capture.CopyAnalysisWindow(
                   tuner_source_window, app::config::kTunerSourceWindowSize))
            {
                const app::TunerResult result = tuner_analyzer.Analyze(
                    tuner_source_window,
                    app::config::kTunerSourceWindowSize,
                    state.pots[0],
                    state.pots[1],
                    state.pots[2],
                    now);
#if DIAG_SEND_TELEMETRY
                telemetry.SendTuner(result);
#else
                (void)result;
#endif
            }
        }
#if DIAG_SEND_TELEMETRY
        else if(!tuner_mode
                && (uint32_t)(now - last_tuner_disabled_tx)
                       >= app::config::kTunerDisabledHeartbeatMs)
        {
            last_tuner_disabled_tx = now;
            telemetry.SendTunerDisabled();
        }
#endif
#endif
#endif

#if AUDIO_CPU_LOAD_DEBUG && DIAG_INIT_TELEMETRY
        if((uint32_t)(now - last_cpu_tx) >= 500u)
        {
            last_cpu_tx = now;
            const app::AudioEngine::CpuLoadSnapshot load
                = audio_engine.GetCpuLoadSnapshot();
            const uint8_t effect = (uint8_t)DIAG_FORCE_EFFECT;
            telemetry.SendAudioCpuLoad(load.average,
                                       load.peak,
                                       load.overrun_count,
                                       effect,
                                       load.effect_peak[effect]);
        }
#else
        (void)last_cpu_tx;
#endif

        (void)last_sent_effect;
        (void)last_pots_tx;
        (void)last_tuner_tx;
        (void)last_tuner_disabled_tx;
        (void)last_rotary_cal_tx;
        (void)previous_tuner_mode;
        (void)current_effect;
        System::Delay(app::config::kMainLoopDelayMs);
    }
#endif
}

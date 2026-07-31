#include "app_config.h"
#include "audio_engine.h"
#include "control_mailbox.h"
#include "controls.h"
#include "telemetry.h"
#include "tuner.h"

#include "daisy_seed.h"
#include "daisy_core.h"

using namespace daisy;

namespace
{
DaisySeed hw;
app::ControlMailbox control_mailbox;
app::Controls controls;
app::TunerCapture tuner_capture;
app::TunerAnalyzer tuner_analyzer;
app::AudioEngine audio_engine;
app::Telemetry telemetry;

// Main-loop scratch space for the tuner analysis window. The audio callback only
// fills the capture ring; this buffer is copied and analyzed later here.
float DMA_BUFFER_MEM_SECTION tuner_source_window[app::config::kTunerSourceWindowSize];

// The audio callback stays small: it just hands the block to AudioEngine, which
// owns the DSP objects and tuner capture work for the real-time path.
void AudioCallback(AudioHandle::InputBuffer in, AudioHandle::OutputBuffer out, size_t size)
{
    audio_engine.Process(in, out, size);
}

} // namespace

// Startup happens once here: hardware, controls, tuner state, telemetry, and
// the audio engine are all initialized before audio begins.
int main(void)
{
    hw.Init();
    hw.SetAudioSampleRate(SaiHandle::Config::SampleRate::SAI_48KHZ);
    hw.SetAudioBlockSize(app::config::kAudioBlockSize);

    telemetry.Init();
    controls.Init(hw, control_mailbox);
    tuner_capture.Init();
    tuner_analyzer.Reset(System::GetNow());
    audio_engine.Init(hw.AudioSampleRate(), control_mailbox, tuner_capture);

    hw.StartAudio(AudioCallback);

    uint8_t last_sent_effect = 0xffu;
    uint8_t current_effect = app::config::FX_TUNER;
    bool previous_tuner_mode = false;
    uint32_t last_pots_tx = 0;
    uint32_t last_tuner_tx = 0;
    uint32_t last_tuner_disabled_tx = 0;
    uint32_t last_rotary_cal_tx = 0;

    // Main loop work stays out of the interrupt path: read controls, emit UART
    // telemetry, run tuner analysis, and keep the Pi informed about mode changes.
    while(1)
    {
        const uint32_t now = System::GetNow();
        const app::ControlTelemetryState state = controls.Process(hw, now);
        current_effect = state.effect;

        hw.SetLed(state.rotary_position == 1);

        if(current_effect != last_sent_effect)
        {
            last_sent_effect = current_effect;
            telemetry.SendEffect(last_sent_effect);
        }

        if(state.footswitch.occurred)
            telemetry.SendFootswitch(state.footswitch.down, state.footswitch.edge, state.footswitch.press_count);

        if(state.pots_changed || (uint32_t)(now - last_pots_tx) >= app::config::kPotTelemetryHeartbeatMs)
        {
            last_pots_tx = now;
            telemetry.SendPots(state.pots);
        }

#if ROTARY_CALIBRATION_MODE
        if((uint32_t)(now - last_rotary_cal_tx) >= 100)
        {
            last_rotary_cal_tx = now;
            telemetry.SendRotaryCalibration(state.rotary_raw_u16, state.rotary_position);
        }
#else
        (void)last_rotary_cal_tx;
#endif

        const bool tuner_mode = current_effect == app::config::FX_TUNER;
        if(tuner_mode && !previous_tuner_mode)
        {
            tuner_analyzer.Reset(now);
            last_tuner_tx = 0;
        }
        else if(!tuner_mode && previous_tuner_mode)
        {
            tuner_analyzer.Reset(now);
            telemetry.SendTunerDisabled();
            last_tuner_disabled_tx = now;
        }
        previous_tuner_mode = tuner_mode;

        if(tuner_mode)
        {
            if((uint32_t)(now - last_tuner_tx) >= app::config::kTunerAnalysisPeriodMs)
            {
                last_tuner_tx = now;
                if(tuner_capture.CopyAnalysisWindow(tuner_source_window, app::config::kTunerSourceWindowSize))
                {
                    const app::TunerResult result = tuner_analyzer.Analyze(tuner_source_window,
                                                                           app::config::kTunerSourceWindowSize,
                                                                           state.pots[0],
                                                                           state.pots[1],
                                                                           state.pots[2],
                                                                           now);
                    telemetry.SendTuner(result);
                }
            }
        }
        else if((uint32_t)(now - last_tuner_disabled_tx) >= app::config::kTunerDisabledHeartbeatMs)
        {
            last_tuner_disabled_tx = now;
            telemetry.SendTunerDisabled();
        }

        System::Delay(app::config::kMainLoopDelayMs);
    }
}
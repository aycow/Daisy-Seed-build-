#pragma once

#include "app_config.h"

#include <stddef.h>
#include <stdint.h>

namespace app
{

// Result packet produced by one foreground tuner analysis pass. valid describes
// the current analysis only; the analyzer may keep older frequency state internally
// for smoothing, but telemetry must not claim a stale pitch as valid.
struct TunerResult
{
    bool valid;
    float freq_hz;
    float confidence;
    float cents;
    char note[8];
    float raw_hz;
};

// Real-time-safe capture ring written by the audio callback and copied by the
// main loop. The ring size is power-of-two so the callback uses a cheap mask
// instead of modulo, and the completed index is published once per audio block.
class TunerCapture
{
  public:
    TunerCapture();
    void Init();

    // Called only from the audio callback. Writes raw input samples for later
    // analysis and never runs pitch detection itself.
    void WriteBlock(const float* samples, size_t size);

    uint32_t CompletedWriteIndex() const;

    // Called from the main loop. Copies a stable historical window ending behind
    // the most recent completed block, leaving a safety margin so the callback
    // cannot overwrite the window while it is being copied.
    bool CopyAnalysisWindow(float* out, int count) const;

  private:
    uint32_t write_index_;
    uint32_t completed_write_index_;
};

// Non-real-time tuner analyzer. It consumes copied sample windows in the main
// loop, performs DC/RMS preparation, filtered decimation, YIN detection, smoothing,
// octave protection, and note/cents conversion.
class TunerAnalyzer
{
  public:
    TunerAnalyzer();
    void Reset(uint32_t now_ms);
    TunerResult Analyze(const float* source, int source_count, float strictness, float smoothing, float attack_control, uint32_t now_ms);
    static void FrequencyToNote(float freq_hz, char* out_note, size_t out_size, float& out_cents);

  private:
    bool DetectYin(const float* source,
                   int source_count,
                   float strictness,
                   float& out_freq_hz,
                   float& out_confidence,
                   uint32_t now_ms);
    static float Median5(const float* values);

    // Pitch history is valid only after has_lock_ becomes true. This prevents the
    // old 110 Hz startup placeholder from being reported before a real note exists.
    float freq_history_[5];
    int freq_history_count_;
    float tracked_freq_;
    float last_valid_freq_;
    float rms_previous_;
    uint32_t attack_until_ms_;
    uint32_t last_lock_ms_;
    uint32_t attack_hold_ms_;
    bool has_lock_;
};

} // namespace app
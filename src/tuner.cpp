#include "tuner.h"
#include "daisy_core.h"

#include <cmath>
#include <cstdio>
#include <cstring>

#if DIAG_HAS_TUNER_OBJECTS
namespace app
{

// CPU-owned storage: never add DMA_BUFFER_MEM_SECTION here. These four arrays
// total 45,056 bytes; placing them before libDaisy's real DMA buffers moved
// audio/ADC DMA beyond its 32 KB non-cacheable MPU window and caused confirmed
// hardware audio corruption.
float tuner_capture_ring[config::kTunerCaptureRingSize];
float tuner_work_buffer[config::kTunerWorkSize];
float tuner_difference_buffer[config::kTunerWorkSize];
float tuner_cmnd_buffer[config::kTunerWorkSize];

static float ClampFloat(float value, float lo, float hi)
{
    return value < lo ? lo : (value > hi ? hi : value);
}

static int ClampInt(int value, int lo, int hi)
{
    return value < lo ? lo : (value > hi ? hi : value);
}

// Refine the detected YIN period by fitting a parabola around the best sample.
static void ParabolicDelta(float y0, float y1, float y2, float& delta)
{
    const float denom = (2.0f * y1 - y0 - y2);
    if(fabsf(denom) < 1e-12f)
    {
        delta = 0.0f;
        return;
    }
    delta = 0.5f * (y0 - y2) / denom;
    delta = ClampFloat(delta, -0.5f, 0.5f);
}

TunerCapture::TunerCapture()
: write_index_(0),
  completed_write_index_(0)
{
#if DIAG_STAGE != DIAG_STAGE_PRODUCTION
    // Keep the complete tuner storage present from the object-only stage.
    // This emits no buffer access; it only prevents linker collection.
    asm volatile("" : : "r"(tuner_capture_ring),
                         "r"(tuner_work_buffer),
                         "r"(tuner_difference_buffer),
                         "r"(tuner_cmnd_buffer));
#endif
}

void TunerCapture::Init()
{
    for(int i = 0; i < config::kTunerCaptureRingSize; ++i)
        tuner_capture_ring[i] = 0.0f;
    write_index_ = 0;
    __atomic_store_n(&completed_write_index_, 0u, __ATOMIC_RELEASE);
}

// Called once per audio block. The atomic store happens after all samples are written so the main loop copies only completed blocks.
void TunerCapture::WriteBlock(const float* samples, size_t size)
{
    uint32_t wr = write_index_;
    for(size_t i = 0; i < size; ++i)
    {
        tuner_capture_ring[wr & (config::kTunerCaptureRingSize - 1)] = samples[i];
        ++wr;
    }
    write_index_ = wr;
    __atomic_store_n(&completed_write_index_, wr, __ATOMIC_RELEASE);
}

uint32_t TunerCapture::CompletedWriteIndex() const
{
    return __atomic_load_n(&completed_write_index_, __ATOMIC_ACQUIRE);
}

// Copy a stable historical window that ends safely behind the audio writer.
// If the ring has not yet filled enough, return false instead of inventing data.
bool TunerCapture::CopyAnalysisWindow(float* out, int count) const
{
    if(!out || count <= 0 || count > config::kTunerCaptureRingSize || count > config::kTunerSourceWindowSize)
        return false;

    const uint32_t required = (uint32_t)count + (uint32_t)config::kTunerSafetySamples;
    if(required >= (uint32_t)config::kTunerCaptureRingSize)
        return false;

    const uint32_t completed = CompletedWriteIndex();
    if(completed < required)
        return false;

    const uint32_t end = completed - (uint32_t)config::kTunerSafetySamples;
    const uint32_t start = end - (uint32_t)count;
    const uint32_t mask = config::kTunerCaptureRingSize - 1;
    for(int i = 0; i < count; ++i)
        out[i] = tuner_capture_ring[(start + (uint32_t)i) & mask];

    const uint32_t after = CompletedWriteIndex();
    if((uint32_t)(after - start) >= (uint32_t)config::kTunerCaptureRingSize)
        return false;

    return true;
}

TunerAnalyzer::TunerAnalyzer()
: freq_history_{0.0f, 0.0f, 0.0f, 0.0f, 0.0f},
  freq_history_count_(0),
  tracked_freq_(0.0f),
  last_valid_freq_(0.0f),
  rms_previous_(0.0f),
  attack_until_ms_(0),
  last_lock_ms_(0),
  attack_hold_ms_(config::kTunerDefaultAttackMs),
  has_lock_(false)
{
}

void TunerAnalyzer::Reset(uint32_t now_ms)
{
    for(int i = 0; i < 5; ++i)
        freq_history_[i] = 0.0f;
    freq_history_count_ = 0;
    tracked_freq_ = 0.0f;
    last_valid_freq_ = 0.0f;
    rms_previous_ = 0.0f;
    attack_until_ms_ = now_ms + config::kTunerDefaultAttackMs;
    last_lock_ms_ = 0;
    attack_hold_ms_ = config::kTunerDefaultAttackMs;
    has_lock_ = false;
}

// Analyze only outside the audio callback. This keeps the expensive YIN
// work off the real-time path and lets the capture ring stay simple.
TunerResult TunerAnalyzer::Analyze(const float* source,
                                   int source_count,
                                   float strictness,
                                   float smoothing,
                                   float attack_control,
                                   uint32_t now_ms)
{
    TunerResult result = {};
    std::snprintf(result.note, sizeof(result.note), "--");

    if(has_lock_ && (uint32_t)(now_ms - last_lock_ms_) > config::kTunerDetectionGapResetMs)
        Reset(now_ms);

    float raw_hz = 0.0f;
    float conf = 0.0f;
    attack_hold_ms_ = 60u + (uint32_t)(ClampFloat(attack_control, 0.0f, 1.0f) * 140.0f + 0.5f);
    const bool ok = DetectYin(source, source_count, strictness, raw_hz, conf, now_ms);
    result.raw_hz = raw_hz;

    if(now_ms < attack_until_ms_)
    {
        if(has_lock_)
        {
            result.valid = false;
            result.freq_hz = last_valid_freq_;
            result.confidence = 0.25f;
            FrequencyToNote(last_valid_freq_, result.note, sizeof(result.note), result.cents);
        }
        return result;
    }

    bool accepted = ok;
    if(accepted && has_lock_)
    {
        const float ratio = raw_hz / last_valid_freq_;
        if((ratio > 1.95f || ratio < 0.52f) && conf < 0.90f)
            accepted = false;
    }

    if(!accepted)
    {
        result.valid = false;
        result.confidence = has_lock_ ? 0.20f : 0.0f;
        if(has_lock_)
        {
            result.freq_hz = last_valid_freq_;
            FrequencyToNote(last_valid_freq_, result.note, sizeof(result.note), result.cents);
        }
        return result;
    }

    for(int i = 0; i < 4; ++i)
        freq_history_[i] = freq_history_[i + 1];
    freq_history_[4] = raw_hz;
    if(freq_history_count_ < 5)
        ++freq_history_count_;

    const float med = freq_history_count_ >= 5 ? Median5(freq_history_) : raw_hz;
    const float alpha_fast = 0.45f;
    const float alpha_slow = 0.06f;
    const float alpha = alpha_fast + (alpha_slow - alpha_fast) * ClampFloat(smoothing, 0.0f, 1.0f);
    tracked_freq_ = has_lock_ ? tracked_freq_ + alpha * (med - tracked_freq_) : med;

    const bool stable = conf > 0.40f;
    if(stable)
    {
        has_lock_ = true;
        last_valid_freq_ = tracked_freq_;
        last_lock_ms_ = now_ms;
    }

    result.valid = stable;
    result.freq_hz = tracked_freq_;
    result.confidence = conf;
    FrequencyToNote(tracked_freq_, result.note, sizeof(result.note), result.cents);
    return result;
}

// Convert Hz to note name and cents relative to equal temperament A4=440.
void TunerAnalyzer::FrequencyToNote(float freq_hz, char* out_note, size_t out_size, float& out_cents)
{
    if(freq_hz <= 0.0f)
    {
        std::snprintf(out_note, out_size, "--");
        out_cents = 0.0f;
        return;
    }

    const float midi_f = 69.0f + 12.0f * std::log2(freq_hz / 440.0f);
    const int midi_n = (int)std::round(midi_f);
    static const char* names[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
    const int note_idx = ((midi_n % 12) + 12) % 12;
    const int octave = (midi_n / 12) - 1;
    std::snprintf(out_note, out_size, "%s%d", names[note_idx], octave);
    out_cents = (midi_f - (float)midi_n) * 100.0f;
}

// Convert the 48 kHz source window to a 12 kHz analysis window with a small
// low-pass filter first, then run the YIN-style difference search on that data.
bool TunerAnalyzer::DetectYin(const float* source,
                              int source_count,
                              float strictness,
                              float& out_freq_hz,
                              float& out_confidence,
                              uint32_t now_ms)
{
    out_freq_hz = 0.0f;
    out_confidence = 0.0f;
    if(!source || source_count < config::kTunerSourceWindowSize)
        return false;

    float mean = 0.0f;
    for(int i = 0; i < source_count; ++i)
        mean += source[i];
    mean /= (float)source_count;

    // Short symmetric FIR used before decimation by four. It is intentionally small because this runs often in the main loop.
    static const float kFir[9] = {
        0.029411765f, 0.058823529f, 0.117647059f, 0.176470588f, 0.235294118f,
        0.176470588f, 0.117647059f, 0.058823529f, 0.029411765f,
    };

    float rms_acc = 0.0f;
    for(int i = 0; i < config::kTunerWorkSize; ++i)
    {
        const int center = i * config::kTunerDecimation;
        float decimated = 0.0f;
        for(int tap = 0; tap < 9; ++tap)
        {
            const int source_idx = ClampInt(center + tap - 4, 0, source_count - 1);
            decimated += kFir[tap] * (source[source_idx] - mean);
        }
        tuner_work_buffer[i] = decimated;
        rms_acc += decimated * decimated;
    }

    const float rms_now = sqrtf(rms_acc / (float)config::kTunerWorkSize);
    const float onset_floor = 0.012f;
    const float onset_ratio = 1.65f;
    if(rms_now > onset_floor && rms_previous_ > 1e-6f && (rms_now / rms_previous_) > onset_ratio)
        attack_until_ms_ = now_ms + attack_hold_ms_;
    rms_previous_ = 0.85f * rms_previous_ + 0.15f * rms_now;

    if(rms_now < config::kTunerRmsGate)
        return false;

    const float fs_ds = config::kSampleRateHz / (float)config::kTunerDecimation;
    int min_tau = (int)(fs_ds / config::kTunerMaxFrequencyHz);
    int max_tau = (int)(fs_ds / config::kTunerMinFrequencyHz);
    min_tau = ClampInt(min_tau, 2, config::kTunerWorkSize - 2);
    max_tau = ClampInt(max_tau, min_tau + 2, config::kTunerWorkSize - 2);

    for(int tau = 0; tau <= max_tau; ++tau)
        tuner_difference_buffer[tau] = 0.0f;

    for(int tau = 1; tau <= max_tau; ++tau)
    {
        float sum = 0.0f;
        const int limit = config::kTunerWorkSize - tau;
        for(int j = 0; j < limit; ++j)
        {
            const float diff = tuner_work_buffer[j] - tuner_work_buffer[j + tau];
            sum += diff * diff;
        }
        tuner_difference_buffer[tau] = sum;
    }

    tuner_cmnd_buffer[0] = 1.0f;
    float running_sum = 0.0f;
    for(int tau = 1; tau <= max_tau; ++tau)
    {
        running_sum += tuner_difference_buffer[tau];
        tuner_cmnd_buffer[tau] = running_sum > 1e-9f ? tuner_difference_buffer[tau] * (float)tau / running_sum : 1.0f;
    }

    // Higher strictness lowers the YIN threshold, requiring a clearer period before lock.
    const float threshold = 0.22f - 0.12f * ClampFloat(strictness, 0.0f, 1.0f);
    int tau_est = -1;
    for(int tau = min_tau; tau <= max_tau; ++tau)
    {
        if(tuner_cmnd_buffer[tau] < threshold)
        {
            while(tau + 1 <= max_tau && tuner_cmnd_buffer[tau + 1] < tuner_cmnd_buffer[tau])
                ++tau;
            tau_est = tau;
            break;
        }
    }

    if(tau_est < 0)
        return false;

    float delta = 0.0f;
    if(tau_est > 1 && tau_est + 1 <= max_tau)
        ParabolicDelta(tuner_cmnd_buffer[tau_est - 1], tuner_cmnd_buffer[tau_est], tuner_cmnd_buffer[tau_est + 1], delta);

    const float tau_f = (float)tau_est + delta;
    if(tau_f <= 1.0f)
        return false;

    const float freq = fs_ds / tau_f;
    if(freq < config::kTunerMinFrequencyHz || freq > config::kTunerMaxFrequencyHz)
        return false;

    out_freq_hz = freq;
    out_confidence = ClampFloat(1.0f - tuner_cmnd_buffer[tau_est], 0.0f, 1.0f);
    return true;
}

// Fixed compare-swap median keeps smoothing robust without heap allocation or std::sort.
float TunerAnalyzer::Median5(const float* values)
{
    float a[5] = {values[0], values[1], values[2], values[3], values[4]};
#define SWAP_IF(i, j) if(a[j] < a[i]) { float t = a[i]; a[i] = a[j]; a[j] = t; }
    SWAP_IF(0, 1);
    SWAP_IF(3, 4);
    SWAP_IF(2, 4);
    SWAP_IF(2, 3);
    SWAP_IF(0, 3);
    SWAP_IF(0, 2);
    SWAP_IF(1, 4);
    SWAP_IF(1, 3);
    SWAP_IF(1, 2);
#undef SWAP_IF
    return a[2];
}

} // namespace app
#endif

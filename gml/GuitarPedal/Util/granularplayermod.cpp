#include "granularplayermod.h"
#include <algorithm>

using namespace daisysp;

void GranularPlayerMod::Init(float *sample, int size, float sample_rate, float phase1, float phase2) {
    // Store the caller-provided buffer. This class reads from it only; the wrapper/looper fills it.
    sample_ = sample;
    size_ = size;
    variable_size_ = size_;
    sample_rate_ = sample_rate;
    // Two grains run half a cycle apart so one grain can fade in while the other fades out.
    phs_.Init(sample_rate_, 0, phase1);
    phsImp_.Init(sample_rate_, 0, 0);
    phs2_.Init(sample_rate_, 0, phase2);
    phsImp2_.Init(sample_rate_, 0, 0);
    // Base phasor frequency for scanning one full active buffer per second.
    sample_frequency_ = sample_rate_ / variable_size_;
    // Precompute envelope tables once at startup; the callback indexes them directly.
    for (int i = 0; i < 512; i++) {
        cosEnv_[i] = sinf((i / 512.0f) * M_PI);
    }

    // Slow-attack linear envelope option.

    float c = 0.0;
    for (int i = 0; i < 512; i++) {
        if (i < 384) { // attack to unity 1/4 grain time
            c += 1 / 384.0f;
            linEnv_[i] = c;
        } else { // decay to zero 3/4 grain time
            c -= 1 / 128.0f;
            linEnv_[i] = c;
        }
    }

    // Fast-attack linear envelope option.

    c = 0.0;
    for (int i = 0; i < 512; i++) {
        if (i < 128) { // attack to unity 1/4 grain time
            c += 1 / 128.0f;
            adLinEnv_[i] = c;
        } else { // decay to zero 3/4 grain time
            c -= 1 / 384.0f;
            adLinEnv_[i] = c;
        }
    }

    rand_idx_mod_ = 0.0;
    rand_idx_mod2_ = 0.0;
    switch1 = false;
    switch2 = false;

    env_mode_ = 0; // set to cosine envelope for default

    grain_pan1_ = 0.5; // First grain stereo pan, updated for each new grain envelope
    grain_pan2_ = 0.5; //  range of 0 to 1, 0 is full left, 1 is full right, 0.5 is center

    outl_ = 0.0;
    outr_ = 0.0;
    cached_speed_input_ = 0.0f;
    cached_transposition_input_ = 0.0f;
    cached_speed_hz_ = 0.0f;
    cached_pitch_ratio_ = 1.0f;
    cache_valid_ = false;
}

uint32_t GranularPlayerMod::WrapIdx(uint32_t idx, uint32_t sz) {
    /*wraps idx to sz*/
    if (sz == 0) {
        return 0;
    }
    // The index can exceed sz by more than one full length (long grains plus
    // the random width offset), so a single subtraction isn't enough
    if (idx >= sz) {
        idx %= sz;
    }

    return idx;
}

float GranularPlayerMod::CentsToRatio(float cents) {
    /*converts cents to  ratio*/
    return powf(2.0f, cents / 1200.0f);
}

float GranularPlayerMod::MsToSamps(float ms, float samplerate) {
    /*converts milliseconds to  number of samples*/
    return (ms * 0.001f) * samplerate;
}

float GranularPlayerMod::NegativeInvert(Phasor *phs, float frequency) {
    /*inverts the phase of the phasor if the frequency is negative, mimicking pure data's phasor~ object*/
    return (frequency > 0) ? phs->Process() : ((phs->Process() * -1) + 1);
}

float GranularPlayerMod::newRandIndex(bool isFirstGrain) {
    /* Generates a new random index modifier based on width setting.
        The stereo panning of each grain is also controled by the random
        number genereated here. The farther the width index is from center, the
        more panning is applied. */

    // Generate random float from 0 to 1
    float r = static_cast<float>(rand()) / static_cast<float>(RAND_MAX);

    // Use this 0 to 1 float value to set the grain panning (this is a hacky way to do it, maybe change later)
    if (isFirstGrain) {
        grain_pan1_ = 0.5 + ((r - 0.5) * spread_);
    } else {
        grain_pan2_ = 0.5 + ((r - 0.5) * spread_);
    }

    // Move randomized range to -0.5 to 0.5
    float rand_zero_centered = r - 0.5;
    float width_in_samples = MsToSamps(width_, sample_rate_);

    // Calculate random index modifier (range of -width/2 to + width/2)
    float rand_idx_in_samples_float = width_in_samples * rand_zero_centered;

    return rand_idx_in_samples_float;
}

/* Selects grain envelope mode */
void GranularPlayerMod::setEnvelopeMode(int env_mode) { env_mode_ = env_mode; }

void GranularPlayerMod::setStereoSpread(float spread) { spread_ = spread; }

void GranularPlayerMod::setSampleSize(float sample_size) {
    // Guard against a zero size (division by zero below)
    variable_size_ = std::max(sample_size, 1.0f);
    sample_frequency_ = sample_rate_ / variable_size_;
}

float GranularPlayerMod::getLeftOut() { return outl_; }

float GranularPlayerMod::getRightOut() { return outr_; }

void GranularPlayerMod::Process(float speed, float transposition, float grain_size, float width) {
    // This function runs once per sample from the active effect. It avoids heap work
    // and caches expensive control conversions when inputs have not changed.
    grain_size_ = grain_size;
    width_ = width;

    // Speed changes require updating both source-scan phasors. Steady speed reuses the cached Hz value.
    if(!cache_valid_ || speed != cached_speed_input_) {
        cached_speed_input_ = speed;
        cached_speed_hz_ = speed * sample_frequency_;
        phs_.SetFreq(fabs(cached_speed_hz_));
        phs2_.SetFreq(fabs(cached_speed_hz_));
        cache_valid_ = true;
    }

    // Pitch changes require a cents-to-ratio conversion, so cache it instead of calling powf() unnecessarily.
    if(transposition != cached_transposition_input_) {
        cached_transposition_input_ = transposition;
        cached_pitch_ratio_ = CentsToRatio(transposition);
    }

    speed_ = cached_speed_hz_;
    transposition_ = (cached_pitch_ratio_ - speed) * (grain_size >= 1 ? 1000 / grain_size_ : 1);

    phsImp_.SetFreq(fabs(transposition_ / 2)); // Now dividing by 2 because we process the phasor twice
    phsImp2_.SetFreq(fabs(transposition_ / 2));
    idxSpeed_ =
        NegativeInvert(&phs_, speed_) *
        variable_size_; // Speed phasors control the movement through the entire sample, NegativeInvert function processes the phasor
    idxSpeed2_ = NegativeInvert(&phs2_, speed_) * variable_size_;
    idxTransp_ = (NegativeInvert(&phsImp_, transposition_) * MsToSamps(grain_size_, sample_rate_));
    idxTransp2_ = (NegativeInvert(&phsImp2_, transposition_) * MsToSamps(grain_size_, sample_rate_));
    // The random width offset can make the position negative; wrap it back
    // into range before the unsigned conversion (negative float to unsigned
    // is undefined behavior)
    float idxFloat = idxSpeed_ + idxTransp_ + rand_idx_mod_;
    float idxFloat2 = idxSpeed2_ + idxTransp2_ + rand_idx_mod2_;
    if (idxFloat < 0.0f) {
        idxFloat += variable_size_;
    }
    if (idxFloat2 < 0.0f) {
        idxFloat2 += variable_size_;
    }
    idx_ = WrapIdx((uint32_t)std::max(idxFloat, 0.0f), variable_size_);
    idx2_ = WrapIdx((uint32_t)std::max(idxFloat2, 0.0f), variable_size_);

    // Check for when phase output equals 0.0, then generate a new random index modifier for next grain envelope
    float phase_out1_imp = phsImp_.Process();
    float phase_out2_imp = phsImp2_.Process();

    if (phase_out1_imp < 0.01 && switch1 == true) { // TESTING
        rand_idx_mod_ = newRandIndex(true);
        switch1 = false;
    }
    if (phase_out1_imp > 0.9)
        switch1 = true;

    if (phase_out2_imp < 0.01 && switch2 == true) { // TESTING
        rand_idx_mod2_ = newRandIndex(false);
        switch2 = false;
    }
    if (phase_out2_imp > 0.9)
        switch2 = true;

    // The phasor can return exactly 1.0, which would index one past the end
    // of the 512-entry envelope tables, so clamp to the last entry
    const uint32_t envIdx1 = std::min((uint32_t)(phase_out1_imp * 512), (uint32_t)511);
    const uint32_t envIdx2 = std::min((uint32_t)(phase_out2_imp * 512), (uint32_t)511);

    if (env_mode_ == 0) {

        sig_ = sample_[idx_] * cosEnv_[envIdx1];
        sig2_ = sample_[idx2_] * cosEnv_[envIdx2];

    } else if (env_mode_ == 1) {

        sig_ = sample_[idx_] * linEnv_[envIdx1];
        sig2_ = sample_[idx2_] * linEnv_[envIdx2];

    } else if (env_mode_ == 2) {

        sig_ = sample_[idx_] * adLinEnv_[envIdx1];
        sig2_ = sample_[idx2_] * adLinEnv_[envIdx2];
    }

    // Combine the two overlapped grains into stereo outputs using their per-grain pan positions.
    outl_ = (sig_ * (1.0 - grain_pan1_) + sig2_ * (1.0 - grain_pan2_)) / 2;
    outr_ = (sig_ * grain_pan1_ + sig2_ * grain_pan2_) / 2;
}

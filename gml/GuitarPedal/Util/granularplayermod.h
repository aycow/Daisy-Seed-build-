/*
Copyright (c) 2020 Electrosmith, Corp, Vinicius Fernandes

Use of this source code is governed by an MIT-style
license that can be found in the LICENSE file or at
https://opensource.org/licenses/MIT.

 GranularPlayer class modified by K. Bloemer 2025 (renamed to GranularPlayerMod)
  Changes:
  - Applies the envelope to the individual grains instead of the whole audio sample
  - Adds two additional envelopes (fast attack and slow attack, linear), and envelopes now use 512 points instead of 256
  - Adds a stereo spread for each grain and left/right output getter functions
  - Adds the ability to change the effective audio sample size
  - Adds "width" parameter that starts grains at a random point within the timeframe set by width param (in milliseconds)
*/

#pragma once
#ifndef DSY_GRANULARPLAYERMOD_H
#define DSY_GRANULARPLAYERMOD_H

#include "daisysp.h"
#include <cmath>
#include <stdint.h>
#ifdef __cplusplus
#ifndef M_PI
#define M_PI 3.14159265358979323846 /* pi */
#endif

/** GranularPlayerMod Module

    GranularPlayerMod reads from an existing audio buffer using two overlapping
    grains. One phasor pair controls where each grain reads from the source buffer;
    another phasor pair controls pitch transposition inside each grain envelope.
    The caller is responsible for filling the source buffer, usually with a looper
    or delay-line style recorder.
*/
class GranularPlayerMod {
  public:
    GranularPlayerMod() {}
    ~GranularPlayerMod() {}

    /** Initializes the granular reader.
        \param sample pointer to the source audio buffer
        \param size number of float samples in the source buffer
        \param sample_rate audio engine sample rate
        \param phase1 starting phase from 0 to 1 of first grain, normally 0
        \param phase2 starting phase from 0 to 1 of second grain, normally 0.5
    */
    void Init(float *sample, int size, float sample_rate, float phase1, float phase2);

    /** Processes one output sample.
        \param speed playback speed; negative values read backward
        \param transposition pitch shift in cents, where 100 cents is one semitone
        \param grain_size grain length in milliseconds; values below 1 ms are clamped by caller logic
        \param width random read-position spread in milliseconds around the grain center
    */
    void Process(float speed, float transposition, float grain_size, float width);

    /* Selects the envelope table used for each grain. */
    void setEnvelopeMode(int env_mode);

    /* Sets stereo spread of each grain: 0 is centered, 1 is widest. */
    void setStereoSpread(float spread);

    /* Sets the active source length while keeping the fixed backing buffer. */
    void setSampleSize(float sample_size);

    float getLeftOut();
    float getRightOut();

  private:
    // Wraps an index to the active source length. Width modulation can push a
    // read point more than one full buffer away, so the implementation uses modulo.
    uint32_t WrapIdx(uint32_t idx, uint32_t size);

    // Expensive conversions are cached in Process() when their input controls have
    // not changed, so powf() is not paid every sample for a steady knob value.
    float CentsToRatio(float cents);
    float MsToSamps(float ms, float samplerate);

    // DaisySP Phasor always rises 0..1; this helper mirrors the phase for negative
    // frequencies to mimic Pure Data phasor~ behavior.
    float NegativeInvert(daisysp::Phasor *phs, float frequency);

    /* Generates a random read-position offset and updates the next grain's stereo pan. */
    float newRandIndex(bool isFirstGrain);

    float *sample_;       // Source audio buffer owned by the effect wrapper.
    float sample_rate_;   // Audio engine sample rate.
    int size_;            // Physical size of sample_, in float samples.
    float grain_size_;    // Current grain size in milliseconds.
    float speed_;         // Cached speed converted to phasor frequency.
    float transposition_; // Cached transposition contribution used by pitch phasors.
    float sample_frequency_;

    // Three envelope shapes, each indexed by the transposition phasor phase.
    float cosEnv_[512] = {0};
    float linEnv_[512] = {0};
    float adLinEnv_[512] = {0};

    // Per-sample read-position components for the two overlapping grains.
    float idxTransp_;
    float idxTransp2_;
    float idxSpeed_;
    float idxSpeed2_;
    float sig_;
    float sig2_;

    uint32_t idx_;  // Current sample index for grain 1.
    uint32_t idx2_; // Current sample index for grain 2.

    daisysp::Phasor phs_;     // Grain 1 source-buffer scan phase.
    daisysp::Phasor phsImp_;  // Grain 1 pitch/envelope phase.
    daisysp::Phasor phs2_;    // Grain 2 source-buffer scan phase.
    daisysp::Phasor phsImp2_; // Grain 2 pitch/envelope phase.

    float width_;         // Randomized grain start range in milliseconds.
    float rand_idx_mod_;  // Grain 1 random read offset, refreshed per grain.
    float rand_idx_mod2_; // Grain 2 random read offset, refreshed per grain.
    float phaseImp_out_;  // Last grain 1 pitch phase used for width updates.
    float phaseImp2_out_; // Last grain 2 pitch phase used for width updates.

    bool switch1;
    bool switch2;

    int env_mode_;

    float grain_pan1_; // Grain 1 stereo pan: 0 left, 0.5 center, 1 right.
    float grain_pan2_; // Grain 2 stereo pan: 0 left, 0.5 center, 1 right.

    float outl_;
    float outr_;

    float spread_; // Amount of stereo spread, 0 for none, 1 for widest.

    // Cached control conversions. These reduce repeated powf() and phasor-rate
    // setup when speed and pitch controls are unchanged.
    float cached_speed_input_;
    float cached_transposition_input_;
    float cached_speed_hz_;
    float cached_pitch_ratio_;
    bool cache_valid_;

    // Active source length. This can be smaller than size_ while still using the
    // original fixed backing buffer.
    float variable_size_;
};
#endif
#endif
#pragma once

/**
 * An NE570/571-style compander pair, for `delay_analog`.
 *
 * A BBD's noise floor is poor, so a pedal compresses into it and expands out of it. The
 * expander's release makes the repeats breathe and makes the noise floor pump with the
 * signal rather than sit still; measured, the noise just after a burst is 8x the floor a
 * second later with the compander on, and flat without it.
 *
 * One instance per channel. The caller compresses on the way into the delay line and
 * expands on the way out; the two halves only invert each other if nothing between them
 * adds gain, which is why the analog delay feeds its repeats back *before* expanding.
 */

#include "dsp/LevelTargets.h"
#include "dsp/effects/DelayLineSupport.h"

#include <algorithm>
#include <cmath>

namespace guitarfx::delay_line
{
class Compander
{
  public:
    /// Gains are recomputed this often and smoothed between. The detectors move in
    /// milliseconds, so per-sample exp2/log2 would be paying for resolution nothing uses.
    static constexpr int kControlInterval = 16;

    void Prepare(double sampleRate) noexcept
    {
        const double rate = (sampleRate > 0.0) ? sampleRate : 48000.0;
        mCompressDetector.Prepare(rate);
        mExpandDetector.Prepare(rate);
        mCompressDetector.SetTimes(kAttackMs, kReleaseMs);
        mExpandDetector.SetTimes(kAttackMs, kReleaseMs);
        mSmoothing = static_cast<float>(1.0 - std::exp(-1000.0 / (rate * kGainSmoothingMs)));
    }

    void Reset() noexcept
    {
        mCompressDetector.Reset();
        mExpandDetector.Reset();
        mCompressGain = 1.0f;
        mExpandGain = 1.0f;
        mSmoothedCompressGain = 1.0f;
        mSmoothedExpandGain = 1.0f;
    }

    /// Exponents that invert each other exactly at any amount.
    ///
    /// The compressor multiplies by (env/ref)^-a, so a signal at level L leaves at
    /// ref^a * L^(1-a). For the expander's (env/ref)^e to undo that, e = a/(1-a). At full
    /// companding a = 0.5 and e = 1: two-to-one in, one-to-two out, as an NE571 pair does.
    ///
    /// The clamps have to be matched the same way. The expander sees the compressor's
    /// *output*, whose envelope ratio is the input's raised to (1-a), so its clamp range is
    /// the compressor's raised to (1-a). With both clamped to one range the pair was
    /// measured at +2.8 dB above 0 dBFS, which inside a feedback loop is a runaway.
    void SetAmount(double amount) noexcept
    {
        const double clamped = std::clamp(amount, 0.0, 1.0);
        mCompressExponent = static_cast<float>(0.5 * clamped);
        mExpandExponent = mCompressExponent / (1.0f - mCompressExponent);
        mActive = clamped > 0.0;

        const float outputPower = 1.0f - mCompressExponent;
        mExpandFloor = std::pow(kFloorRatio, outputPower);
        mExpandCeiling = std::pow(kCeilingRatio, outputPower);
    }

    [[nodiscard]] bool Active() const noexcept
    {
        return mActive;
    }

    /// Recompute both gains from their detectors. Call every kControlInterval samples.
    void UpdateGains() noexcept
    {
        const float compressRatio = std::clamp(mCompressDetector.Value() / kReference, kFloorRatio, kCeilingRatio);
        const float expandRatio = std::clamp(mExpandDetector.Value() / kReference, mExpandFloor, mExpandCeiling);
        mCompressGain = std::exp2(-mCompressExponent * std::log2(compressRatio));
        mExpandGain = std::exp2(mExpandExponent * std::log2(expandRatio));
    }

    /// Feed-forward: the detector sees the signal before the gain is applied.
    [[nodiscard]] float Compress(float input) noexcept
    {
        mSmoothedCompressGain += mSmoothing * (mCompressGain - mSmoothedCompressGain);
        (void)mCompressDetector.Process(input);
        return input * mSmoothedCompressGain;
    }

    [[nodiscard]] float Expand(float input) noexcept
    {
        mSmoothedExpandGain += mSmoothing * (mExpandGain - mSmoothedExpandGain);
        (void)mExpandDetector.Process(input);
        return input * mSmoothedExpandGain;
    }

  private:
    /// Detector timing. The release is what makes repeats breathe.
    static constexpr double kAttackMs = 1.0;
    static constexpr double kReleaseMs = 60.0;
    static constexpr double kGainSmoothingMs = 0.5;
    /// Envelope range the law holds over, as a ratio to the reference level.
    static constexpr float kFloorRatio = 0.01f;
    static constexpr float kCeilingRatio = 8.0f;

    /// Unity-gain point: the project's nominal operating level, so the compander leaves a
    /// signal at the level the chain is calibrated to exactly where it found it.
    static inline const float kReference = static_cast<float>(DbToLinearGain(kDefaultNominalOperatingLevelDbfs));

    EnvelopeFollower mCompressDetector;
    EnvelopeFollower mExpandDetector;
    float mCompressExponent = 0.0f;
    float mExpandExponent = 0.0f;
    float mExpandFloor = kFloorRatio;
    float mExpandCeiling = kCeilingRatio;
    float mCompressGain = 1.0f;
    float mExpandGain = 1.0f;
    float mSmoothedCompressGain = 1.0f;
    float mSmoothedExpandGain = 1.0f;
    float mSmoothing = 1.0f;
    bool mActive = false;
};
} // namespace guitarfx::delay_line

#pragma once

#include "dsp/FiniteCheck.h"
#include <algorithm>
#include <array>
#include <cmath>

namespace guitarfx
{
/**
 * What a guitar speaker does when it is pushed hard, which an impulse response cannot
 * capture: an IR is a linear snapshot and sounds the same at any volume. Two effects
 * dominate, and both are modelled here:
 *
 *  - Excursion. Around and below its resonance the cone travels furthest, and at the ends
 *    of that travel the suspension stiffens and the coil leaves the magnet's field. The low
 *    end rounds off and grows odd harmonics. A one-pole crossover splits off the low band,
 *    a cubic soft clip bends it, and the highs pass straight through. Confining the curve to
 *    the low band is also what lets it run without oversampling: a cubic makes only the
 *    third harmonic, and three times the low band is nowhere near Nyquist.
 *  - Power compression. The voice coil heats, its resistance rises and the speaker plays
 *    quieter. Heat builds and drains slowly, so a sustained loud passage sags over a
 *    fraction of a second and recovers after it. A slow average of the signal power drives
 *    a gentle 2:1 gain reduction above a threshold, never more than 6 dB.
 *
 * Drive is the amount of both. At 0 the stage is exactly transparent, so it can sit in
 * front of any cabinet without changing presets that never touch it. Stereo channels are
 * independent, as two speakers are.
 */
class SpeakerDrive
{
  public:
    void Prepare(double sampleRate) noexcept
    {
        mSampleRate = sampleRate;
        mAttack = OnePoleCoefficient(kCompressionAttackSeconds);
        mRelease = OnePoleCoefficient(kCompressionReleaseSeconds);
        mDriveSmoothing = OnePoleCoefficient(kDriveSmoothingSeconds);
        UpdateCrossover();
        Reset();
    }

    void Reset() noexcept
    {
        mLowBand.fill(0.0);
        mPower.fill(0.0);
        mDrive = mTargetDrive;
        UpdatePreGain();
    }

    /// 0 = transparent, 1 = +18 dB into the excursion curve and full power compression.
    void SetDrive(double amount) noexcept
    {
        if (IsFinite(amount))
        {
            mTargetDrive = std::clamp(amount, 0.0, 1.0);
        }
    }

    [[nodiscard]] double GetDrive() const noexcept
    {
        return mTargetDrive;
    }

    /// Centre of the excursion band. Callers that know their cabinet's low resonance pass
    /// something near it; the default suits a typical guitar speaker.
    void SetExcursionCornerHz(double hz) noexcept
    {
        mCornerHz = std::max(20.0, hz);
        UpdateCrossover();
    }

    /// False once drive has settled at zero, when Process() would return its input.
    [[nodiscard]] bool IsActive() const noexcept
    {
        return mTargetDrive > 0.0 || mDrive > kInactiveDrive;
    }

    /// Processes one sample of `channel` (0 or 1). Call once per channel per sample, and
    /// AdvanceSample() once after both.
    double Process(double input, int channel) noexcept
    {
        const double drive = mDrive;

        if (drive <= kInactiveDrive)
        {
            mLowBand[channel] = 0.0;
            mPower[channel] = 0.0;
            return input;
        }

        // A NaN or infinity would stay in the band and power state for good: play it as
        // silence. IsFinite, because the fast-math builds fold std::isfinite away.
        if (!IsFinite(input))
        {
            input = 0.0;
        }

        // Excursion: bend the low band only.
        double& low = mLowBand[channel];
        low += mCrossover * (input - low);
        const double preGain = mPreGain;
        const double bentLow = SoftClip(low * preGain) / preGain;
        const double excursion = input + drive * (bentLow - low);

        // Power compression, in the power domain to avoid a log per sample: for 2:1 above
        // threshold T the output power is T * sqrt(P / T), so the gain is (T / P)^(1/4).
        double& power = mPower[channel];
        const double drivenPower = input * input * preGain * preGain;
        power += (drivenPower > power ? mAttack : mRelease) * (drivenPower - power);
        double gain = 1.0;

        if (power > kCompressionThresholdPower)
        {
            gain = std::max(kMinCompressionGain, std::sqrt(std::sqrt(kCompressionThresholdPower / power)));
        }

        return excursion * (1.0 - drive * (1.0 - gain));
    }

    /// Moves drive one sample towards its target, so automation does not zipper.
    void AdvanceSample() noexcept
    {
        if (mDrive == mTargetDrive)
        {
            return;
        }

        mDrive += mDriveSmoothing * (mTargetDrive - mDrive);

        if (std::abs(mTargetDrive - mDrive) <= kInactiveDrive)
        {
            mDrive = mTargetDrive;
        }

        UpdatePreGain();
    }

  private:
    static constexpr double kPi = 3.14159265358979323846;
    static constexpr double kMaxExcursionDriveDb = 18.0;
    static constexpr double kCompressionAttackSeconds = 0.05;
    static constexpr double kCompressionReleaseSeconds = 0.4;
    static constexpr double kCompressionThresholdPower = 0.0158;    // -18 dBFS
    static constexpr double kMinCompressionGain = 0.50118723362727; // -6 dB, the most it takes
    static constexpr double kDriveSmoothingSeconds = 0.02;
    static constexpr double kInactiveDrive = 1e-6;
    static constexpr double kDefaultCornerHz = 180.0;

    /// u - 4u^3/27, flat beyond |u| = 1.5: unity slope at zero, and it reaches +/-1 with zero
    /// slope, so the join is smooth.
    [[nodiscard]] static double SoftClip(double u) noexcept
    {
        const double clamped = std::clamp(u, -1.5, 1.5);
        return clamped - (4.0 / 27.0) * clamped * clamped * clamped;
    }

    [[nodiscard]] double OnePoleCoefficient(double seconds) const noexcept
    {
        return 1.0 - std::exp(-1.0 / (seconds * mSampleRate));
    }

    void UpdatePreGain() noexcept
    {
        mPreGain = std::pow(10.0, mDrive * kMaxExcursionDriveDb / 20.0);
    }

    void UpdateCrossover() noexcept
    {
        mCrossover = 1.0 - std::exp(-2.0 * kPi * std::min(mCornerHz, mSampleRate * 0.25) / mSampleRate);
    }

    double mSampleRate = 48000.0;
    double mCornerHz = kDefaultCornerHz;
    double mCrossover = 0.0;
    double mAttack = 0.0;
    double mRelease = 0.0;
    double mDriveSmoothing = 0.0;
    double mTargetDrive = 0.0;
    double mDrive = 0.0;
    double mPreGain = 1.0;
    std::array<double, 2> mLowBand = {};
    std::array<double, 2> mPower = {};
};
} // namespace guitarfx

#pragma once

/**
 * The imperfections of a moving tape, for `delay_tape`: the transport's speed error (wow
 * and flutter) and the medium's own wear (dropouts).
 *
 * Kept apart from DelayLineSupport.h because only the tape delay uses them; the primitives
 * they are built from live there.
 */

#include "dsp/effects/DelayLineSupport.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace guitarfx::delay_line
{
/**
 * Wow and flutter: the transport's speed error.
 *
 * Modelled as a *speed* error, not a time error. A capstan turning fractionally fast or
 * slow shifts the pitch of whatever is passing the head by that same fraction, however far
 * apart the heads are — so the pitch deviation each component contributes is the thing to
 * specify, and the delay-time swing it needs follows from it:
 *
 *     amplitude_samples = pitchDeviation * sampleRate / (2 * pi * rateHz)
 *
 * which is the integral of the speed error. That division by rate is the whole point. Give
 * wow and flutter the same swing in milliseconds and flutter, being ten times faster, comes
 * out ten times more violent in pitch — which is the mistake that makes a tape emulation
 * sound like a broken chorus. Measured at full depth: wow swings the delay 3.85 ms for
 * 31 cents, flutter only 0.15 ms for 16 cents, and both are the same at a 60 ms delay as at
 * 1200 ms.
 *
 * Two incommensurate oscillators per band, so nothing ever lines up into a recognisable
 * LFO, plus a filtered-noise scrape term on flutter.
 */
class TapeTransport
{
  public:
    void Prepare(double sampleRate)
    {
        mSampleRate = (sampleRate > 0.0) ? sampleRate : 48000.0;

        for (std::size_t index = 0; index < kComponentCount; ++index)
        {
            mOscillators[index].Prepare(mSampleRate);
            mOscillators[index].SetRate(kRatesHz[index]);
            mAmplitudes[index] = static_cast<float>(kPitchDeviation[index] * mSampleRate / (kTwoPi * kRatesHz[index]));
        }

        mScrapeAmplitude = static_cast<float>(kScrapePitchDeviation * mSampleRate / (kTwoPi * kScrapeCentreHz));
        mScrapeFilter.SetLowpass(kScrapeCentreHz, mSampleRate, 1.4);
        mScrapeHighpass.SetCutoff(kScrapeCentreHz * 0.4, mSampleRate);
        Reset();
    }

    void Reset() noexcept
    {
        // Unequal start phases: at zero they would all peak together on the first cycle.
        for (std::size_t index = 0; index < kComponentCount; ++index)
        {
            mOscillators[index].Reset(kStartPhase[index]);
        }

        mScrapeFilter.Reset();
        mScrapeHighpass.Reset();
        mRng.Seed(0x5eed1234u);
    }

    void SetWow(float amount) noexcept
    {
        mWow = std::clamp(amount, 0.0f, 1.0f);
    }

    void SetFlutter(float amount) noexcept
    {
        mFlutter = std::clamp(amount, 0.0f, 1.0f);
    }

    [[nodiscard]] bool Idle() const noexcept
    {
        return mWow <= 0.0f && mFlutter <= 0.0f;
    }

    /// The delay-time offset in samples for this sample. Add it to the delay; the caller
    /// still has to clamp the sum to what the line can read.
    [[nodiscard]] float Next() noexcept
    {
        float offset = 0.0f;

        for (std::size_t index = 0; index < kWowComponents; ++index)
        {
            offset += mWow * mAmplitudes[index] * mOscillators[index].Next();
        }

        for (std::size_t index = kWowComponents; index < kComponentCount; ++index)
        {
            offset += mFlutter * mAmplitudes[index] * mOscillators[index].Next();
        }

        const float scrape = mScrapeFilter.Process(mScrapeHighpass.Process(mRng.NextBipolar()));
        offset += mFlutter * mScrapeAmplitude * scrape;

        return offset;
    }

  private:
    static constexpr std::size_t kWowComponents = 2;
    static constexpr std::size_t kComponentCount = 4;

    /// Two slow components (reel and capstan eccentricity) and two fast ones (capstan
    /// ripple and idler). The rates are mutually irrational enough not to beat.
    static constexpr double kRatesHz[kComponentCount] = {0.63, 1.17, 7.1, 11.3};
    /// Peak fractional pitch error each contributes at full depth. Wow totals about 1.8%
    /// and flutter about 0.9%, which is a machine well past its service interval — the
    /// defaults sit at a quarter of it.
    static constexpr double kPitchDeviation[kComponentCount] = {0.012, 0.006, 0.003, 0.006};
    static constexpr float kStartPhase[kComponentCount] = {0.0f, 0.37f, 0.61f, 0.19f};

    static constexpr double kScrapeCentreHz = 32.0;
    static constexpr double kScrapePitchDeviation = 0.0025;

    double mSampleRate = 48000.0;
    LfoPhasor mOscillators[kComponentCount];
    float mAmplitudes[kComponentCount] = {0.0f, 0.0f, 0.0f, 0.0f};
    float mScrapeAmplitude = 0.0f;
    Biquad mScrapeFilter;
    OnePoleHp mScrapeHighpass;
    Rng mRng;
    float mWow = 0.0f;
    float mFlutter = 0.0f;
};

/**
 * Dropouts: oxide shedding, as a slow random wander that only dips now and then.
 *
 * A new random target every interval, smoothed; the gain dips only while the smoothed value
 * is above a threshold. Targets are cubed, which piles them up near zero, so most of the
 * time the tape is fine and a dip is an event rather than a tremolo.
 */
class TapeDropout
{
  public:
    void Prepare(double sampleRate) noexcept
    {
        const double rate = (sampleRate > 0.0) ? sampleRate : 48000.0;
        mInterval = std::max(1, static_cast<int>(rate * kIntervalMs * 0.001));
        mCoefficient = static_cast<float>(1.0 - std::exp(-1000.0 / (rate * kSmoothingMs)));
    }

    void Reset() noexcept
    {
        mRng.Seed(0xd809u);
        mState = 0.0f;
        mTarget = 0.0f;
        mCountdown = 0;
    }

    /// 0 is a pristine tape; 1 dips as far as kMaxDepth.
    void SetAmount(float amount) noexcept
    {
        mDepth = std::clamp(amount, 0.0f, 1.0f) * kMaxDepth;
    }

    [[nodiscard]] bool Idle() const noexcept
    {
        return mDepth <= 0.0f;
    }

    /// The gain to apply to playback for this sample, in (1 - kMaxDepth, 1].
    [[nodiscard]] float Next() noexcept
    {
        if (--mCountdown <= 0)
        {
            const float r = mRng.NextBipolar();
            mTarget = r * r * r;
            mCountdown = mInterval;
        }

        mState += mCoefficient * (mTarget - mState);
        const float dip = std::max(0.0f, mState - kThreshold) / (1.0f - kThreshold);
        return 1.0f - mDepth * dip;
    }

  private:
    static constexpr double kIntervalMs = 45.0;
    static constexpr double kSmoothingMs = 60.0;
    static constexpr float kThreshold = 0.3f;
    static constexpr float kMaxDepth = 0.6f;

    Rng mRng;
    float mDepth = 0.0f;
    float mState = 0.0f;
    float mTarget = 0.0f;
    float mCoefficient = 1.0f;
    int mInterval = 1;
    int mCountdown = 0;
};
} // namespace guitarfx::delay_line

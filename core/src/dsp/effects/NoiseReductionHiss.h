#pragma once

#include "dsp/BiquadDesign.h"
#include "dsp/FiniteCheck.h"
#include "dsp/effects/NoiseReductionSupport.h"

#include <algorithm>
#include <array>
#include <cmath>

/**
 * The pickup noise reduction's hiss stage (NoiseReductionEffect.h runs it after the hum
 * canceller): above a pickup-dependent split the signal is two bands, each expanded downward as
 * its level falls toward its own measured noise floor. Below the split nothing is touched.
 *
 * The bands come from two fourth-order Butterworth low-passes by subtraction: low is the first,
 * mid the second less the first, high the input less the second. The three always add back to
 * the input exactly, so at unity gains the stage changes nothing, and the low band's steep skirt
 * leaves the cut above the split as deep as Hiss asks.
 */
namespace guitarfx::noise_reduction
{
/// What the hiss stage's two channels share, worked out from the parameters.
struct HissSettings
{
    std::array<BiquadCoefficients, 2> lowA; ///< the lower split, two sections
    std::array<BiquadCoefficients, 2> lowB; ///< the upper split
    float detectorCoef = 1.0f;
    float floorCoef = 1.0f;
    float gainOpen = 1.0f;
    float gainClose = 1.0f;
    float minGain = 1.0f;         ///< the full cut Hiss asks for
    float noiseRatio = 0.1f;      ///< the noise level over the threshold
    float exponent = 0.0f;        ///< the expansion power law's exponent
    double thresholdRatio = 20.0; ///< a band floor to its threshold, as power

    void Prepare(double sampleRate)
    {
        detectorCoef = static_cast<float>(OnePole(kBandDetectorMs, sampleRate));
        floorCoef = static_cast<float>(OnePole(kBandFloorMs, sampleRate));
        gainOpen = static_cast<float>(OnePole(kGainOpenMs, sampleRate));
    }

    void SetSplits(const PickupVoicing& voicing, double sampleRate)
    {
        for (int section = 0; section < 2; ++section)
        {
            const double q = biquad::ButterworthSectionQ(4, section);
            lowA[section] = biquad::LowPass(voicing.lowSplitHz, q, sampleRate);
            lowB[section] = biquad::LowPass(voicing.highSplitHz, q, sampleRate);
        }
    }

    /// `hiss` is the Hiss amount, 0 to 1; `thresholdDb` how far above the noise a band passes
    /// at full level.
    void SetAmount(double hiss, double thresholdDb)
    {
        minGain = static_cast<float>(std::pow(10.0, -hiss * kMaxHissReductionDb / 20.0));
        const double ratio = std::max(std::pow(10.0, thresholdDb / 10.0), kMinThresholdRatio);
        thresholdRatio = kNoiseOverFloor * ratio;
        noiseRatio = static_cast<float>(1.0 / ratio);
        exponent = static_cast<float>(-std::log(static_cast<double>(minGain)) / std::log(ratio));
    }

    void SetRelease(double milliseconds, double sampleRate)
    {
        gainClose = static_cast<float>(OnePole(milliseconds, sampleRate));
    }

    /// A band's gain for mean square `level`: unity at the threshold and above, the full cut at
    /// the noise level and below, and between them a power law of the level, which is a
    /// straight line in decibels.
    [[nodiscard]] float BandGain(float level, float invThreshold) const
    {
        const float ratio = level * invThreshold;

        if (ratio >= 1.0f)
        {
            return 1.0f;
        }

        if (ratio <= noiseRatio)
        {
            return minGain;
        }

        return std::max(std::exp2(exponent * std::log2(ratio)), minGain);
    }
};

/// One channel of the hiss stage.
class HissExpander
{
  public:
    void Prepare(double sampleRate)
    {
        mMidFloor.Prepare(sampleRate, kHissFloorCeiling);
        mHighFloor.Prepare(sampleRate, kHissFloorCeiling);
        Reset();
    }

    void Reset()
    {
        mLowA = {};
        mLowB = {};
        mMidEnv = mHighEnv = 0.0f;
        mMidSlow = mHighSlow = 0.0f;
        mMidGain = mHighGain = 1.0f;
        mMidFloor.Reset();
        mHighFloor.Reset();
    }

    void BeginBlock(const HissSettings& settings)
    {
        mInvMidThreshold = static_cast<float>(1.0 / (mMidFloor.Floor() * settings.thresholdRatio));
        mInvHighThreshold = static_cast<float>(1.0 / (mHighFloor.Floor() * settings.thresholdRatio));
        // Until the floors have a reading there is nothing to expand toward.
        mReady = mMidFloor.HasReading() && mHighFloor.HasReading();
        mBlockMinMid = 1.0e30f;
        mBlockMinHigh = 1.0e30f;
    }

    [[nodiscard]] float Process(const HissSettings& settings, float input)
    {
        const auto low =
            static_cast<float>(mLowA[1].Process(settings.lowA[1], mLowA[0].Process(settings.lowA[0], input)));
        const auto lowAndMid =
            static_cast<float>(mLowB[1].Process(settings.lowB[1], mLowB[0].Process(settings.lowB[0], input)));
        const float mid = lowAndMid - low;
        const float high = input - lowAndMid;
        const float midPower = mid * mid;
        const float highPower = high * high;
        mMidEnv += settings.detectorCoef * (midPower - mMidEnv);
        mHighEnv += settings.detectorCoef * (highPower - mHighEnv);
        mMidSlow += settings.floorCoef * (midPower - mMidSlow);
        mHighSlow += settings.floorCoef * (highPower - mHighSlow);
        mBlockMinMid = std::min(mBlockMinMid, mMidSlow);
        mBlockMinHigh = std::min(mBlockMinHigh, mHighSlow);

        const float midTarget = mReady ? settings.BandGain(mMidEnv, mInvMidThreshold) : 1.0f;
        const float highTarget = mReady ? settings.BandGain(mHighEnv, mInvHighThreshold) : 1.0f;
        mMidGain = SmoothGain(settings, mMidGain, midTarget);
        mHighGain = SmoothGain(settings, mHighGain, highTarget);

        if (mMidGain < 1.0f || mHighGain < 1.0f)
        {
            return low + mMidGain * mid + mHighGain * high;
        }

        return input;
    }

    /// `takeReadings` false while the levels are still warming up from a reset.
    void EndBlock(int numSamples, bool takeReadings)
    {
        if (takeReadings)
        {
            mMidFloor.Push(mBlockMinMid, numSamples);
            mHighFloor.Push(mBlockMinHigh, numSamples);
        }
    }

    [[nodiscard]] bool IsFinite() const
    {
        return guitarfx::IsFinite(mMidEnv) && guitarfx::IsFinite(mHighEnv) && guitarfx::IsFinite(mLowA[1].s1) &&
               guitarfx::IsFinite(mLowB[1].s1);
    }

    [[nodiscard]] float MidGain() const
    {
        return mMidGain;
    }

    [[nodiscard]] float HighGain() const
    {
        return mHighGain;
    }

  private:
    [[nodiscard]] static float SmoothGain(const HissSettings& settings, float current, float target)
    {
        const float next = current + (target > current ? settings.gainOpen : settings.gainClose) * (target - current);
        return (std::abs(target - next) < 1.0e-6f) ? target : next;
    }

    std::array<biquad::State, 2> mLowA;
    std::array<biquad::State, 2> mLowB;
    float mMidEnv = 0.0f;
    float mHighEnv = 0.0f;
    float mMidSlow = 0.0f; ///< the smoothed levels the band floors are measured from
    float mHighSlow = 0.0f;
    float mMidGain = 1.0f;
    float mHighGain = 1.0f;
    FloorTracker mMidFloor;
    FloorTracker mHighFloor;
    float mInvMidThreshold = 0.0f;
    float mInvHighThreshold = 0.0f;
    float mBlockMinMid = 0.0f;
    float mBlockMinHigh = 0.0f;
    bool mReady = false;
};
} // namespace guitarfx::noise_reduction

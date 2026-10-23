#pragma once

/**
 * VerbalSpring.h — Verbal's Spring machine: one to three spring loops a side, each dispersing on
 * every pass (the shared ChirpDispersion), fed by a tank preamp whose Dwell drives it from clean
 * into hard compression. A spring tank's drive circuit is level-dependent by design: picked
 * harder, it splashes and then squashes.
 */

#include "dsp/reverb/ReverbBlocks.h"

#include <array>
#include <cmath>
#include <cstddef>

namespace guitarfx::reverb
{
class VerbalSpring
{
  public:
    static constexpr std::size_t kSprings = 3;

    void Prepare(double sampleRate)
    {
        mSampleRate = sampleRate;

        for (std::size_t i = 0; i < kSprings; ++i)
        {
            mLoopL[i].Resize(static_cast<std::size_t>((kLoopMsR[i] * 1.1 + 8.0) * 0.001 * sampleRate));
            mLoopR[i].Resize(static_cast<std::size_t>((kLoopMsR[i] * 1.1 + 8.0) * 0.001 * sampleRate));
        }

        for (std::size_t i = 0; i < kInputStages; ++i)
        {
            mInputL[i].Prepare((kInputMs[i] * 1.1 + 2.0) * 0.001 * sampleRate);
            mInputR[i].Prepare((kInputMs[i] * 1.1 + 2.0) * 0.001 * sampleRate);
        }

        mChirp.Prepare(sampleRate);
        mDcCoeff = BandDamping::OnePoleCoeff(30.0, sampleRate);
        mLfo.SetRate(0.7, sampleRate);
        Reset();
    }

    void Reset() noexcept
    {
        for (std::size_t i = 0; i < kSprings; ++i)
        {
            mLoopL[i].Clear();
            mLoopR[i].Clear();
        }

        for (std::size_t i = 0; i < kInputStages; ++i)
        {
            mInputL[i].Reset();
            mInputR[i].Reset();
        }

        mChirp.Reset();
        mDampL.fill(0.0f);
        mDampR.fill(0.0f);
        mDcL.fill(0.0f);
        mDcR.fill(0.0f);
        mHpState = {};
        mLfo.Reset();
    }

    /// Decay in seconds; tone, lowEnd and mod 0-1; dwell 0-3; springs 1-3.
    void Configure(double rt60, double tone, double lowEnd, double mod, int dwell, int springs) noexcept
    {
        mActive = static_cast<std::size_t>(std::clamp(springs, 1, 3));
        // Each extra spring adds its own echoes; scale so the total stays level.
        mOutScale = 1.0f / std::sqrt(static_cast<float>(mActive));
        const double dampHz = 3000.0 * std::pow(4.0, tone);
        mDamp = BandDamping::OnePoleCoeff(dampHz, mSampleRate);
        // The loop's low-pass also costs the mids a little every pass; put that back so Decay
        // still reads as the RT60 in the guitar's midrange (measured at 700 Hz, so the lows do not overshoot).
        const double lossAt1k = 1.0 / std::sqrt(1.0 + std::pow(700.0 / dampHz, 2.0));
        // The first half of Low End is the steep low cut of a real tank's drive circuit.
        const double hpHz = lowEnd < 0.5 ? 420.0 * std::pow(110.0 / 420.0, lowEnd * 2.0)
                                         : 110.0 * std::pow(45.0 / 110.0, (lowEnd - 0.5) * 2.0);
        mHpCoeff = BandDamping::OnePoleCoeff(hpHz, mSampleRate);
        mModDepth = static_cast<float>(0.5 * mod * 0.001 * mSampleRate);

        // Clean only rounds off the loudest peaks; Overdrive squashes everything from a light pick up.
        static constexpr std::array<float, 4> kDrive = {1.0f, 1.4f, 2.6f, 5.5f};
        static constexpr std::array<float, 4> kCeiling = {1.2f, 0.6f, 0.33f, 0.17f};
        const auto d = static_cast<std::size_t>(std::clamp(dwell, 0, 3));
        mDrive = kDrive[d];
        mCeiling = kCeiling[d];

        for (std::size_t i = 0; i < kSprings; ++i)
        {
            // The chirp's group delay adds about a millisecond a pass to each loop.
            const double loopL = (kLoopMsL[i] + 1.0) * 0.001;
            const double loopR = (kLoopMsR[i] + 1.0) * 0.001;
            mGainL[i] = static_cast<float>(std::min(0.999, Rt60LoopGain(loopL, rt60) / lossAt1k));
            mGainR[i] = static_cast<float>(std::min(0.999, Rt60LoopGain(loopR, rt60) / lossAt1k));
        }
    }

    void SetHold(float hold) noexcept
    {
        mHold = hold;
    }

    void Process(float inL, float inR, float& outL, float& outR) noexcept
    {
        // The tank preamp: mono-ish, low-cut twice over, then driven into its rail.
        float mono = 0.5f * (inL + inR);
        const float side = 0.5f * (inL - inR);
        float l = HighPass(0, mono + 0.3f * side);
        float r = HighPass(1, mono - 0.3f * side);
        l = mCeiling * delay_line::FastTanh(l * mDrive / mCeiling);
        r = mCeiling * delay_line::FastTanh(r * mDrive / mCeiling);

        for (std::size_t i = 0; i < kInputStages; ++i)
        {
            l = mInputL[i].Process(l, kInputMs[i] * 0.001 * mSampleRate, 0.58f);
            r = mInputR[i].Process(r, kInputMs[i] * 1.08 * 0.001 * mSampleRate, 0.58f);
        }

        mLfo.Advance();
        const float damp = mDamp + (1.0f - mDamp) * mHold;
        // Held, the DC block drops toward a few hertz: at 30 Hz it bled a held note's fundamental.
        const float dc = mDcCoeff * (1.0f - 0.9f * mHold);
        std::array<float, kSprings> loopL{};
        std::array<float, kSprings> loopR{};

        for (std::size_t i = 0; i < kSprings; ++i)
        {
            const float wobble = mModDepth * ((i & 1u) ? mLfo.Sin() : mLfo.Cos());
            const float yl = mLoopL[i].ReadHermite(kLoopMsL[i] * 0.001 * mSampleRate + wobble);
            const float yr = mLoopR[i].ReadHermite(kLoopMsR[i] * 0.001 * mSampleRate - wobble);
            mDampL[i] = delay_line::FlushDenormal(mDampL[i] + damp * (yl - mDampL[i]));
            mDampR[i] = delay_line::FlushDenormal(mDampR[i] + damp * (yr - mDampR[i]));
            mDcL[i] = delay_line::FlushDenormal(mDcL[i] + dc * (mDampL[i] - mDcL[i]));
            mDcR[i] = delay_line::FlushDenormal(mDcR[i] + dc * (mDampR[i] - mDcR[i]));
            loopL[i] = mDampL[i] - mDcL[i];
            loopR[i] = mDampR[i] - mDcR[i];
        }

        mChirp.ProcessPair(loopL, loopR);
        float sumL = 0.0f;
        float sumR = 0.0f;

        for (std::size_t i = 0; i < kSprings; ++i)
        {
            const float gL = mGainL[i] + (1.0f - mGainL[i]) * mHold;
            const float gR = mGainR[i] + (1.0f - mGainR[i]) * mHold;
            const bool active = i < mActive;
            const float feedL = active ? l * kInject[i] : 0.0f;
            const float feedR = active ? r * kInject[i] : 0.0f;
            // A little of each side crosses to the other, as the two ends of a tank couple. A
            // rotation, so the coupling itself loses nothing: a plain 0.97/0.03 blend cost a
            // quarter of a decibel a pass and capped the tail near 6 s.
            const float wl = gL * (kCouple * loopL[i] + kCross * loopR[i]) + feedL;
            const float wr = gR * (kCouple * loopR[i] - kCross * loopL[i]) + feedR;
            mLoopL[i].Write(delay_line::FlushDenormal(wl));
            mLoopR[i].Write(delay_line::FlushDenormal(wr));

            if (active)
            {
                sumL += loopL[i];
                sumR += loopR[i];
            }
        }

        outL = sumL * mOutScale;
        outR = sumR * mOutScale;
    }

  private:
    static constexpr std::size_t kInputStages = 2;
    static constexpr std::array<double, kSprings> kLoopMsL = {26.8, 38.7, 52.1};
    static constexpr std::array<double, kSprings> kLoopMsR = {27.3, 39.4, 52.9};
    static constexpr float kCross = 0.06f;
    static constexpr float kCouple = 0.99819888f; // sqrt(1 - kCross²)
    static constexpr std::array<double, kInputStages> kInputMs = {1.3, 3.4};
    static constexpr std::array<float, kSprings> kInject = {0.5f, 0.42f, 0.36f};

    [[nodiscard]] float HighPass(std::size_t side, float x) noexcept
    {
        // Two one-pole high-passes in series: 12 dB/octave, as a tank's input network gives.
        auto& s = mHpState[side];
        s[0] = delay_line::FlushDenormal(s[0] + mHpCoeff * (x - s[0]));
        const float first = x - s[0];
        s[1] = delay_line::FlushDenormal(s[1] + mHpCoeff * (first - s[1]));
        return first - s[1];
    }

    double mSampleRate = 48000.0;
    std::array<delay_line::FractionalDelayLine, kSprings> mLoopL;
    std::array<delay_line::FractionalDelayLine, kSprings> mLoopR;
    std::array<FractionalAllpass, kInputStages> mInputL;
    std::array<FractionalAllpass, kInputStages> mInputR;
    ChirpDispersion<kSprings * 2> mChirp;
    std::array<float, kSprings> mDampL{};
    std::array<float, kSprings> mDampR{};
    std::array<float, kSprings> mDcL{};
    std::array<float, kSprings> mDcR{};
    std::array<float, kSprings> mGainL{};
    std::array<float, kSprings> mGainR{};
    std::array<std::array<float, 2>, 2> mHpState{};
    RotatingPhasor mLfo;
    std::size_t mActive = 2;
    float mOutScale = 0.707f;
    float mDamp = 0.5f;
    float mDcCoeff = 0.004f;
    float mHpCoeff = 0.01f;
    float mModDepth = 0.0f;
    float mDrive = 1.6f;
    float mCeiling = 0.8f;
    float mHold = 0.0f;
};
} // namespace guitarfx::reverb

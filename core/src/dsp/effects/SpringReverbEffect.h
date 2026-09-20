#pragma once

#include "dsp/EffectGuids.h"
#include "dsp/EffectProcessor.h"
#include "dsp/EffectRegistry.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <string>
#include <vector>

namespace guitarfx
{
class SpringReverbEffect : public EffectProcessor
{
  public:
    void Prepare(double sampleRate, int maxBlockSize) override
    {
        if (!ValidatePrepare(sampleRate, maxBlockSize))
        {
            return;
        }

        mSampleRate = sampleRate;
        mMaxBlockSize = maxBlockSize;

        const size_t maxInputSamples = DelayMsToSamples(kMaxInputDelayMs + 6.0);
        mInputDelayL.assign(maxInputSamples, 0.0f);
        mInputDelayR.assign(maxInputSamples, 0.0f);
        mInputDelayWrite = 0;

        constexpr double extraTailMs = 12.0;

        for (size_t index = 0; index < kTankCount; ++index)
        {
            const size_t lenL = DelayMsToSamples(kTankDelayMsL[index] * kMaxTensionScale + extraTailMs);
            const size_t lenR = DelayMsToSamples(kTankDelayMsR[index] * kMaxTensionScale + extraTailMs);
            mTankDelayL[index].assign(lenL, 0.0f);
            mTankDelayR[index].assign(lenR, 0.0f);
            mTankWriteL[index] = 0;
            mTankWriteR[index] = 0;
            mTankLowpassStateL[index] = 0.0f;
            mTankLowpassStateR[index] = 0.0f;
        }

        constexpr double extraDispersionMs = 4.0;

        for (size_t index = 0; index < kDispersionCount; ++index)
        {
            const size_t lenL = DelayMsToSamples(kDispersionDelayMsL[index] * kMaxTensionScale + extraDispersionMs);
            const size_t lenR = DelayMsToSamples(kDispersionDelayMsR[index] * kMaxTensionScale + extraDispersionMs);
            mDispersionDelayL[index].assign(lenL, 0.0f);
            mDispersionDelayR[index].assign(lenR, 0.0f);
            mDispersionWriteL[index] = 0;
            mDispersionWriteR[index] = 0;
        }

        mSmoothCoeff = static_cast<float>(1.0 - std::exp(-1.0 / (std::max(1.0, mSampleRate) * 0.015)));
        mTensionSmoothCoeff = static_cast<float>(1.0 - std::exp(-1.0 / (std::max(1.0, mSampleRate) * 0.12)));

        UpdateParameters();
        mFeedback = mFeedbackTarget;
        mDamp = mDampTarget;
        mBrightness = mBrightnessTarget;
        mTensionScale = mTensionScaleTarget;
        mMixSmoothed = static_cast<float>(mMix);
        mDriveSmoothed = static_cast<float>(mDrive);

        Reset();
    }

    void Reset() override
    {
        std::fill(mInputDelayL.begin(), mInputDelayL.end(), 0.0f);
        std::fill(mInputDelayR.begin(), mInputDelayR.end(), 0.0f);
        mInputDelayWrite = 0;

        for (size_t index = 0; index < kTankCount; ++index)
        {
            std::fill(mTankDelayL[index].begin(), mTankDelayL[index].end(), 0.0f);
            std::fill(mTankDelayR[index].begin(), mTankDelayR[index].end(), 0.0f);
            mTankWriteL[index] = 0;
            mTankWriteR[index] = 0;
            mTankLowpassStateL[index] = 0.0f;
            mTankLowpassStateR[index] = 0.0f;
            mTankDcPrevL[index] = 0.0f;
            mTankDcPrevR[index] = 0.0f;
            mTankDcStateL[index] = 0.0f;
            mTankDcStateR[index] = 0.0f;
        }

        for (size_t index = 0; index < kDispersionCount; ++index)
        {
            std::fill(mDispersionDelayL[index].begin(), mDispersionDelayL[index].end(), 0.0f);
            std::fill(mDispersionDelayR[index].begin(), mDispersionDelayR[index].end(), 0.0f);
            mDispersionWriteL[index] = 0;
            mDispersionWriteR[index] = 0;
        }

        mWetToneStateL = 0.0f;
        mWetToneStateR = 0.0f;
        mDripEnv = 0.0f;
        mInputHpPrevL = 0.0f;
        mInputHpPrevR = 0.0f;
        mInputHpStateL = 0.0f;
        mInputHpStateR = 0.0f;
        mBandpassState1L = {};
        mBandpassState1R = {};
        mBandpassState2L = {};
        mBandpassState2R = {};
    }

    void Process(float** inputs, float** outputs, int numSamples) override
    {
        if (!outputs || numSamples <= 0)
        {
            return;
        }

        numSamples = std::min(numSamples, mMaxBlockSize);

        if (mInputDelayL.empty() || mTankDelayL[0].empty())
        {
            CopyInputToOutput(inputs, outputs, numSamples);
            return;
        }

        if (!mEnabled)
        {
            CopyInputToOutput(inputs, outputs, numSamples);
            return;
        }

        for (int sampleIndex = 0; sampleIndex < numSamples; ++sampleIndex)
        {
            const float inL = inputs && inputs[0] ? inputs[0][sampleIndex] : 0.0f;
            const float inR = inputs && inputs[1] ? inputs[1][sampleIndex] : inL;

            mFeedback += (mFeedbackTarget - mFeedback) * mSmoothCoeff;
            mDamp += (mDampTarget - mDamp) * mSmoothCoeff;
            mBrightness += (mBrightnessTarget - mBrightness) * mSmoothCoeff;
            mTensionScale += (mTensionScaleTarget - mTensionScale) * mTensionSmoothCoeff;
            mMixSmoothed += (static_cast<float>(mMix) - mMixSmoothed) * mSmoothCoeff;
            mDriveSmoothed += (static_cast<float>(mDrive) - mDriveSmoothed) * mSmoothCoeff;

            const float dryMix = 1.0f - mMixSmoothed;
            const float driveAmount = mDriveSmoothed;

            const float hpInL = ProcessInputHighpass(inL, mInputHpPrevL, mInputHpStateL);
            const float hpInR = ProcessInputHighpass(inR, mInputHpPrevR, mInputHpStateR);

            const float mono = 0.5f * (hpInL + hpInR);
            const float side = 0.5f * (hpInL - hpInR);
            const float attackLevel = std::max(std::fabs(hpInL), std::fabs(hpInR));
            mDripEnv += (attackLevel - mDripEnv) * (attackLevel > mDripEnv ? mDripAttackCoeff : mDripReleaseCoeff);

            const float drivenL = ApplyDrive(mono + side * 0.35f, driveAmount);
            const float drivenR = ApplyDrive(mono - side * 0.35f, driveAmount);

            mInputDelayL[mInputDelayWrite] = drivenL;
            mInputDelayR[mInputDelayWrite] = drivenR;
            const float delayedInL = ReadFromDelay(mInputDelayL, mInputDelayWrite, mInputDelaySamples);
            const float delayedInR = ReadFromDelay(mInputDelayR, mInputDelayWrite, mInputDelaySamples);

            if (++mInputDelayWrite >= mInputDelayL.size())
            {
                mInputDelayWrite = 0;
            }

            float exciteL = delayedInL;
            float exciteR = delayedInR;

            for (size_t index = 0; index < kDispersionCount; ++index)
            {
                const float delayL = std::clamp(DelayMsToSamplesFloat(kDispersionDelayMsL[index] * mTensionScale), 1.0f,
                                                static_cast<float>(mDispersionDelayL[index].size() - 2));
                const float delayR = std::clamp(DelayMsToSamplesFloat(kDispersionDelayMsR[index] * mTensionScale), 1.0f,
                                                static_cast<float>(mDispersionDelayR[index].size() - 2));
                exciteL = ProcessAllpass(mDispersionDelayL[index], mDispersionWriteL[index], delayL, exciteL, 0.58f);
                exciteR = ProcessAllpass(mDispersionDelayR[index], mDispersionWriteR[index], delayR, exciteR, 0.58f);
            }

            float tankOutL = 0.0f;
            float tankOutR = 0.0f;

            for (size_t tankIndex = 0; tankIndex < kTankCount; ++tankIndex)
            {
                const float delayL = std::clamp(DelayMsToSamplesFloat(kTankDelayMsL[tankIndex] * mTensionScale), 1.0f,
                                                static_cast<float>(mTankDelayL[tankIndex].size() - 2));
                const float delayR = std::clamp(DelayMsToSamplesFloat(kTankDelayMsR[tankIndex] * mTensionScale), 1.0f,
                                                static_cast<float>(mTankDelayR[tankIndex].size() - 2));

                const float delayedL = ReadFromDelayFractional(mTankDelayL[tankIndex], mTankWriteL[tankIndex], delayL);
                const float delayedR = ReadFromDelayFractional(mTankDelayR[tankIndex], mTankWriteR[tankIndex], delayR);

                mTankLowpassStateL[tankIndex] =
                    FlushNearZero(mTankLowpassStateL[tankIndex] + (delayedL - mTankLowpassStateL[tankIndex]) * mDamp);
                mTankLowpassStateR[tankIndex] =
                    FlushNearZero(mTankLowpassStateR[tankIndex] + (delayedR - mTankLowpassStateR[tankIndex]) * mDamp);

                // Block DC in the loop. The saturator below sits inside this feedback path, and a
                // nonlinearity fed asymmetric tank ringing pumps low frequency back round with it —
                // measured 13 dB above mid in the 20-120 Hz band once Drive was up.
                const float filteredL = ProcessTankDcBlock(mTankLowpassStateL[tankIndex], mTankDcPrevL[tankIndex],
                                                           mTankDcStateL[tankIndex]);
                const float filteredR = ProcessTankDcBlock(mTankLowpassStateR[tankIndex], mTankDcPrevR[tankIndex],
                                                           mTankDcStateR[tankIndex]);

                // Self-feedback plus a little cross-coupling. The pair decays at (self + cross), so
                // the old 0.54 ceiling capped the tank at ~0.65 s — a real spring tank rings for
                // 1.5-3 s, and that is most of what makes it sound like one.
                const float feedbackL = filteredL * (mFeedback * (0.90f - 0.04f * static_cast<float>(tankIndex))) +
                                        filteredR * (0.030f + 0.008f * static_cast<float>(tankIndex));
                const float feedbackR = filteredR * (mFeedback * (0.90f - 0.04f * static_cast<float>(tankIndex))) +
                                        filteredL * (0.030f + 0.008f * static_cast<float>(tankIndex));

                const float injectL = exciteL * (0.48f - 0.08f * static_cast<float>(tankIndex));
                const float injectR = exciteR * (0.48f - 0.08f * static_cast<float>(tankIndex));

                const float tankInputL = injectL + feedbackL;
                const float tankInputR = injectR + feedbackR;
                mTankDelayL[tankIndex][mTankWriteL[tankIndex]] =
                    FlushNearZero(ApplySaturation(tankInputL, driveAmount * 0.28f));
                mTankDelayR[tankIndex][mTankWriteR[tankIndex]] =
                    FlushNearZero(ApplySaturation(tankInputR, driveAmount * 0.28f));

                if (++mTankWriteL[tankIndex] >= mTankDelayL[tankIndex].size())
                {
                    mTankWriteL[tankIndex] = 0;
                }

                if (++mTankWriteR[tankIndex] >= mTankDelayR[tankIndex].size())
                {
                    mTankWriteR[tankIndex] = 0;
                }

                tankOutL += filteredL;
                tankOutR += filteredR;
            }

            tankOutL *= (1.0f / static_cast<float>(kTankCount));
            tankOutR *= (1.0f / static_cast<float>(kTankCount));

            const float drip1L =
                ProcessBandpass(tankOutL, mBandpassState1L, mDripBand1B0, mDripBand1B2, mDripBand1A1, mDripBand1A2);
            const float drip1R =
                ProcessBandpass(tankOutR, mBandpassState1R, mDripBand1B0, mDripBand1B2, mDripBand1A1, mDripBand1A2);
            const float drip2L =
                ProcessBandpass(tankOutL, mBandpassState2L, mDripBand2B0, mDripBand2B2, mDripBand2A1, mDripBand2A2);
            const float drip2R =
                ProcessBandpass(tankOutR, mBandpassState2R, mDripBand2B0, mDripBand2B2, mDripBand2A1, mDripBand2A2);

            float wetL = tankOutL * (0.68f + mBrightness * 0.14f) +
                         (drip1L * 0.42f + drip2L * 0.20f) * (0.22f + driveAmount * 0.38f) * mDripEnv;
            float wetR = tankOutR * (0.68f + mBrightness * 0.14f) +
                         (drip1R * 0.42f + drip2R * 0.20f) * (0.22f + driveAmount * 0.38f) * mDripEnv;

            mWetToneStateL = FlushNearZero(mWetToneStateL + (wetL - mWetToneStateL) * mBrightness);
            mWetToneStateR = FlushNearZero(mWetToneStateR + (wetR - mWetToneStateR) * mBrightness);
            wetL = mWetToneStateL;
            wetR = mWetToneStateR;

            const float wetMid = 0.5f * (wetL + wetR);
            const float wetSide = 0.5f * (wetL - wetR);
            wetL = wetMid + wetSide * 0.74f;
            wetR = wetMid - wetSide * 0.74f;

            if (outputs[0])
            {
                outputs[0][sampleIndex] = FlushNearZero(inL * dryMix + wetL * mMixSmoothed);
            }

            if (outputs[1])
            {
                outputs[1][sampleIndex] = FlushNearZero(inR * dryMix + wetR * mMixSmoothed);
            }
        }
    }

    void SetParam(const std::string& key, double value) override
    {
        if (key == "decay")
        {
            mDecay = std::clamp(value, 0.0, 1.0);
        }
        else if (key == "tone")
        {
            mTone = std::clamp(value, 0.0, 1.0);
        }
        else if (key == "drive")
        {
            mDrive = std::clamp(value, 0.0, 1.0);
        }
        else if (key == "mix")
        {
            mMix = std::clamp(value, 0.0, 1.0);
        }
        else
        {
            return;
        }

        UpdateParameters();
    }

    void SetConfig(const std::string&, const std::string&) override
    {
    }

    [[nodiscard]] double GetParam(const std::string& key) const override
    {
        if (key == "decay")
        {
            return mDecay;
        }

        if (key == "tone")
        {
            return mTone;
        }

        if (key == "drive")
        {
            return mDrive;
        }

        if (key == "mix")
        {
            return mMix;
        }

        return 0.0;
    }

    [[nodiscard]] std::string GetType() const override
    {
        return "reverb_spring";
    }

    [[nodiscard]] std::string GetCategory() const override
    {
        return "reverb";
    }

  private:
    static constexpr size_t kTankCount = 3;
    static constexpr size_t kDispersionCount = 6;
    static constexpr double kMaxInputDelayMs = 12.0;
    static constexpr double kMaxTensionScale = 1.35;
    static constexpr double kTwoPi = 6.2831853071795864769;
    static constexpr std::array<double, kTankCount> kTankDelayMsL = {26.8, 38.7, 52.1};
    static constexpr std::array<double, kTankCount> kTankDelayMsR = {28.3, 40.4, 54.2};
    // 6-stage allpass dispersion chain — real spring tanks typically use 6–8 stages;
    // 3 stages produced audible isolated echoes on staccato input.
    static constexpr std::array<double, kDispersionCount> kDispersionDelayMsL = {1.3, 2.1, 3.1, 3.4, 4.6, 5.2};
    static constexpr std::array<double, kDispersionCount> kDispersionDelayMsR = {1.5, 2.4, 3.4, 3.7, 4.9, 5.6};
    // An Accutronics tank rings for roughly 1.5-3 s; that ring is most of what makes it a spring.
    static constexpr double kRt60MinS = 0.5;
    static constexpr double kRt60MaxS = 3.0;
    // Means of the per-tank (0.90 - 0.04·i) self and (0.030 + 0.008·i) cross coefficients below.
    static constexpr double kMeanTankSelf = 0.86;
    static constexpr double kMeanTankCross = 0.038;
    // Holds the worst tank (self 0.90, cross 0.030) below unity so the tank cannot self-oscillate.
    static constexpr double kMaxTankFeedback = 1.02;
    // Corner of the one-pole DC block inside the tank loop.
    static constexpr double kTankDcHz = 30.0;

    size_t DelayMsToSamples(double ms) const
    {
        return std::max<size_t>(1, static_cast<size_t>(ms * mSampleRate * 0.001));
    }

    float DelayMsToSamplesFloat(double ms) const
    {
        return static_cast<float>(std::max(1.0, ms * mSampleRate * 0.001));
    }

    static float ReadFromDelay(const std::vector<float>& buffer, size_t writePos, size_t delaySamples)
    {
        if (buffer.empty())
        {
            return 0.0f;
        }

        const size_t back = std::min(delaySamples, buffer.size() - 1);
        const size_t readPos = (writePos + buffer.size() - back) % buffer.size();
        return buffer[readPos];
    }

    float ReadFromDelayFractional(const std::vector<float>& buffer, size_t writePos, float delaySamples) const
    {
        if (buffer.empty())
        {
            return 0.0f;
        }

        const size_t delayFloor = static_cast<size_t>(delaySamples);
        const float frac = delaySamples - static_cast<float>(delayFloor);
        const float sample0 = ReadFromDelay(buffer, writePos, delayFloor);
        const float sample1 = ReadFromDelay(buffer, writePos, delayFloor + 1);
        return sample0 + frac * (sample1 - sample0);
    }

    float ProcessAllpass(std::vector<float>& buffer, size_t& writePos, float delaySamples, float input, float gain)
    {
        const float delayed = ReadFromDelayFractional(buffer, writePos, delaySamples);
        const float output = delayed - input * gain;
        // The delay line is fed the allpass OUTPUT. Feeding back the delay read instead gives
        // ((1+g²)z^-M - g)/(1 - g·z^-M), which across this six-stage dispersion chain is +10 to
        // +30 dB with 20 dB of comb ripple rather than the flat phase smear it is here for.
        buffer[writePos] = input + output * gain;

        if (++writePos >= buffer.size())
        {
            writePos = 0;
        }

        return output;
    }

    // Padé [2/2] rational tanh approximation, error < 0.5% for |x| ≤ 3.5, clips at ±5.5.
    static float FastTanh(float x) noexcept
    {
        if (x > 5.5f)
        {
            return 1.0f;
        }

        if (x < -5.5f)
        {
            return -1.0f;
        }

        const float x2 = x * x;
        return x * (27.0f + x2) / (27.0f + 9.0f * x2);
    }

    static float ApplyDrive(float sample, float amount)
    {
        if (amount <= 0.0f)
        {
            return sample;
        }

        const float drive = 1.0f + amount * 6.0f;
        const float norm = FastTanh(drive);

        if (norm <= 0.0f)
        {
            return sample;
        }

        return FastTanh(sample * drive) / norm;
    }

    // Same curve, normalised so the small-signal gain stays at 1 instead of rising to drive/tanh(drive).
    // ApplyDrive() normalises by peak, which suits the input stage — there the extra gain is the point —
    // but inside the tank loop it multiplies the feedback: at drive=1 it took the loop gain to 1.07
    // and the tank self-oscillated.
    static float ApplySaturation(float sample, float amount)
    {
        if (amount <= 0.0f)
        {
            return sample;
        }

        const float drive = 1.0f + amount * 6.0f;
        return FastTanh(sample * drive) / drive;
    }

    static void CopyInputToOutput(float** inputs, float** outputs, int numSamples)
    {
        for (int channel = 0; channel < 2; ++channel)
        {
            if (!outputs[channel])
            {
                continue;
            }

            if (inputs && inputs[channel])
            {
                std::copy_n(inputs[channel], numSamples, outputs[channel]);
            }
            else
            {
                std::fill_n(outputs[channel], numSamples, 0.0f);
            }
        }
    }

    static void ComputeBandpass(double sampleRate, double freq, double q, float& b0, float& b2, float& a1, float& a2)
    {
        if (sampleRate <= 0.0)
        {
            b0 = 1.0f;
            b2 = -1.0f;
            a1 = 0.0f;
            a2 = 0.0f;
            return;
        }

        const double w0 = kTwoPi * freq / sampleRate;
        const double cosw0 = std::cos(w0);
        const double sinw0 = std::sin(w0);
        const double alpha = sinw0 / (2.0 * q);
        const double invA0 = 1.0 / (1.0 + alpha);

        b0 = static_cast<float>(alpha * invA0);
        b2 = static_cast<float>(-alpha * invA0);
        a1 = static_cast<float>(-2.0 * cosw0 * invA0);
        a2 = static_cast<float>((1.0 - alpha) * invA0);
    }

    static float ProcessBandpass(float input, std::array<float, 2>& state, float b0, float b2, float a1, float a2)
    {
        const float output = b0 * input + state[0];
        state[0] = -a1 * output + state[1];
        state[1] = b2 * input - a2 * output;
        return output;
    }

    float ProcessInputHighpass(float input, float& prevIn, float& prevOut)
    {
        const float output = mInputHpAlpha * (prevOut + input - prevIn);
        prevIn = input;
        // Flushed: the pole sits above 0.99, so once the input goes quiet the state decays into
        // denormal range and stays there, and denormal arithmetic on the audio thread is slow.
        prevOut = FlushNearZero(output);
        return output;
    }

    float ProcessTankDcBlock(float input, float& prevIn, float& prevOut) const
    {
        const float output = mTankDcAlpha * (prevOut + input - prevIn);
        prevIn = input;
        prevOut = FlushNearZero(output);
        return output;
    }

    void UpdateParameters()
    {
        mInputDelaySamples = DelayMsToSamples(1.5 + mDrive * 3.5);

        if (!mInputDelayL.empty())
        {
            mInputDelaySamples = std::min(mInputDelaySamples, mInputDelayL.size() - 1);
        }

        mTensionScaleTarget =
            static_cast<float>(std::clamp(0.88 + mTone * 0.30 - mDrive * 0.05, 0.82, kMaxTensionScale));

        // Feedback derived from the RT60 Decay is asking for. Each tank decays at roughly
        // (self-feedback + cross-coupling), so the target loop gain is solved back through the
        // mean of those coefficients. Deriving it keeps the knob even in time — RT60 goes as
        // 1/-log10(g), so a gain that tracks the knob linearly bunches up at the top.
        double meanTankMs = 0.0;

        for (size_t index = 0; index < kTankCount; ++index)
        {
            meanTankMs += kTankDelayMsL[index] + kTankDelayMsR[index];
        }

        meanTankMs *= mTensionScaleTarget / (2.0 * static_cast<double>(kTankCount));

        const double rt60S = kRt60MinS * std::pow(kRt60MaxS / kRt60MinS, std::clamp(mDecay, 0.0, 1.0));
        const double loopGain = std::pow(10.0, -3.0 * (meanTankMs * 0.001) / rt60S);
        mFeedbackTarget =
            static_cast<float>(std::clamp((loopGain - kMeanTankCross) / kMeanTankSelf, 0.20, kMaxTankFeedback));
        // Cutoff of the one-pole inside each tank. The old 0.08-0.38 span put it near 1.7 kHz,
        // which with the longer tail above left the tail 40 dB down at 4-10 kHz — a spring tank
        // is metallic, and that brightness has to survive the decay.
        mDampTarget = static_cast<float>(std::clamp(0.22 + mTone * 0.42, 0.18, 0.66));
        mBrightnessTarget = static_cast<float>(std::clamp(0.06 + mTone * 0.22, 0.06, 0.28));

        const double hpHz = std::clamp(130.0 + mTone * 220.0, 120.0, 380.0);
        const double dt = 1.0 / std::max(1.0, mSampleRate);
        const double rc = 1.0 / (2.0 * 3.14159265358979323846 * hpHz);
        mInputHpAlpha = static_cast<float>(rc / (rc + dt));

        const double tankDcRc = 1.0 / (2.0 * 3.14159265358979323846 * kTankDcHz);
        mTankDcAlpha = static_cast<float>(tankDcRc / (tankDcRc + dt));

        const double dripCenter1 = 1100.0 + mTone * 1700.0;
        const double dripCenter2 = std::clamp(dripCenter1 * 1.85, 2000.0, 6200.0);
        ComputeBandpass(mSampleRate, dripCenter1, 0.85 + mDrive * 0.5, mDripBand1B0, mDripBand1B2, mDripBand1A1,
                        mDripBand1A2);
        ComputeBandpass(mSampleRate, dripCenter2, 0.65 + mDrive * 0.35, mDripBand2B0, mDripBand2B2, mDripBand2A1,
                        mDripBand2A2);

        mDripAttackCoeff = static_cast<float>(1.0 - std::exp(-1.0 / (std::max(1.0, mSampleRate) * 0.0025)));
        mDripReleaseCoeff = static_cast<float>(1.0 - std::exp(-1.0 / (std::max(1.0, mSampleRate) * 0.045)));
    }

    static float FlushNearZero(float sample)
    {
        return (std::fabs(sample) < 1.0e-9f) ? 0.0f : sample;
    }

    std::vector<float> mInputDelayL;
    std::vector<float> mInputDelayR;
    size_t mInputDelayWrite = 0;
    size_t mInputDelaySamples = 1;

    std::array<std::vector<float>, kTankCount> mTankDelayL;
    std::array<std::vector<float>, kTankCount> mTankDelayR;
    std::array<size_t, kTankCount> mTankWriteL{};
    std::array<size_t, kTankCount> mTankWriteR{};
    std::array<float, kTankCount> mTankLowpassStateL{};
    std::array<float, kTankCount> mTankLowpassStateR{};
    std::array<float, kTankCount> mTankDcPrevL{};
    std::array<float, kTankCount> mTankDcPrevR{};
    std::array<float, kTankCount> mTankDcStateL{};
    std::array<float, kTankCount> mTankDcStateR{};

    std::array<std::vector<float>, kDispersionCount> mDispersionDelayL;
    std::array<std::vector<float>, kDispersionCount> mDispersionDelayR;
    std::array<size_t, kDispersionCount> mDispersionWriteL{};
    std::array<size_t, kDispersionCount> mDispersionWriteR{};

    double mDecay = 0.42;
    double mTone = 0.52;
    double mDrive = 0.18;
    double mMix = 0.18;

    float mFeedback = 0.70f;
    float mFeedbackTarget = 0.70f;
    float mDamp = 0.20f;
    float mDampTarget = 0.20f;
    float mBrightness = 0.18f;
    float mBrightnessTarget = 0.18f;
    float mTensionScale = 1.0f;
    float mTensionScaleTarget = 1.0f;
    float mMixSmoothed = 0.18f;
    float mDriveSmoothed = 0.18f;
    float mSmoothCoeff = 0.0f;
    float mTensionSmoothCoeff = 0.0f;
    float mWetToneStateL = 0.0f;
    float mWetToneStateR = 0.0f;

    float mInputHpAlpha = 0.95f;
    float mTankDcAlpha = 0.996f;
    float mInputHpPrevL = 0.0f;
    float mInputHpPrevR = 0.0f;
    float mInputHpStateL = 0.0f;
    float mInputHpStateR = 0.0f;

    float mDripEnv = 0.0f;
    float mDripAttackCoeff = 0.0f;
    float mDripReleaseCoeff = 0.0f;

    std::array<float, 2> mBandpassState1L{};
    std::array<float, 2> mBandpassState1R{};
    std::array<float, 2> mBandpassState2L{};
    std::array<float, 2> mBandpassState2R{};
    float mDripBand1B0 = 1.0f;
    float mDripBand1B2 = -1.0f;
    float mDripBand1A1 = 0.0f;
    float mDripBand1A2 = 0.0f;
    float mDripBand2B0 = 1.0f;
    float mDripBand2B2 = -1.0f;
    float mDripBand2A1 = 0.0f;
    float mDripBand2A2 = 0.0f;
};

inline void RegisterSpringReverbEffect()
{
    EffectTypeInfo info;
    info.type = EffectGuids::kReverbSpring;
    info.aliases = {"reverb_spring"};
    info.displayName = "Spring Reverb";
    info.category = "reverb";
    info.description = "Dedicated spring tank reverb with splashy drip and nonlinear drive";
    info.requiresResource = false;
    info.parameters = {{"decay", "Decay", 0.42, 0.0, 1.0, "amount", "spring"},
                       {"tone", "Tone", 0.52, 0.0, 1.0, "amount", "spring"},
                       {"drive", "Drive", 0.18, 0.0, 1.0, "amount", "spring"},
                       {"mix", "Mix", 0.18, 0.0, 1.0, "amount", "spring"}};

    EffectRegistry::Instance().Register(info.type, info, []() { return std::make_unique<SpringReverbEffect>(); });
}
} // namespace guitarfx
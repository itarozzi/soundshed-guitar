#pragma once

#include "dsp/BiquadFrequency.h"
#include "dsp/EffectProcessor.h"
#include "dsp/EffectRegistry.h"
#include "dsp/EffectGuids.h"
#include "dsp/effects/BuiltinAmpOversampling.h"
#include <algorithm>
#include <array>
#include <cmath>

namespace guitarfx
{
/**
 * Built-in amp head. The whole nonlinear path runs at up to 4x with an
 * anti-aliasing half-band decimator; cabinet filtering belongs downstream.
 */
class BuiltinAmpEffect : public EffectProcessor
{
  public:
    void Prepare(double sampleRate, int maxBlockSize) override
    {
        mSampleRate = sampleRate;
        mMaxBlockSize = maxBlockSize;
        mOversamplingFactor = sampleRate < 88200.0 ? 4 : (sampleRate < 176400.0 ? 2 : 1);
        mDspSampleRate = sampleRate * mOversamplingFactor;
        for (auto* bank : {&mUpFirst, &mDownFirst, &mUpSecond, &mDownSecond})
        {
            for (auto& filter : *bank)
            {
                filter.Prepare();
            }
        }

        constexpr double stageHighPass[] = {75.0, 140.0, 180.0, 100.0};
        constexpr double stageLowPass[] = {10500.0, 7500.0, 8500.0, 6500.0};
        for (int stage = 0; stage < kMaxStages; ++stage)
        {
            mStageFilters[stage].Prepare(mDspSampleRate, stageHighPass[stage], stageLowPass[stage]);
        }
        UpdateSmoothing();
        UpdateSagCoefficients();
        UpdatePreFilters();
        UpdatePreEmphasis();
        UpdateToneStack();
        UpdateSpeakerFilters();
        UpdatePostFilters();
        Reset();
    }

    [[nodiscard]] int GetLatencySamples() const override
    {
        return mOversamplingFactor == 4 ? 48 : (mOversamplingFactor == 2 ? 32 : 0);
    }

    void Reset() override
    {
        for (auto* bank : {&mUpFirst, &mDownFirst, &mUpSecond, &mDownSecond})
        {
            for (auto& filter : *bank)
            {
                filter.Reset();
            }
        }
        for (auto& filter : mStageFilters)
        {
            filter.Reset();
        }

        for (int ch = 0; ch < 2; ++ch)
        {
            mLowS1[ch] = mLowS2[ch] = 0.0;
            mMidS1[ch] = mMidS2[ch] = 0.0;
            mContourS1[ch] = mContourS2[ch] = 0.0;
            mTrebleS1[ch] = mTrebleS2[ch] = 0.0;
            mPresenceS1[ch] = mPresenceS2[ch] = 0.0;
            mPreHPS1[ch] = mPreHPS2[ch] = 0.0;
            mPreEmphS1[ch] = mPreEmphS2[ch] = 0.0;
            mDepthS1[ch] = mDepthS2[ch] = 0.0;
            mResonanceS1[ch] = mResonanceS2[ch] = 0.0;
            mDampingS1[ch] = mDampingS2[ch] = 0.0;
            mPostHPS1[ch] = mPostHPS2[ch] = 0.0;
            mSagEnv[ch] = 0.0f;
        }

        mVoiceSmoothed = mVoice;
        mGainSmoothed = mGain;
        mStageGainSmoothed = mStageGainLinear;
        mPowerDriveSmoothed = mPowerDrive;
        mSagSmoothed = mSag;
        mBiasSmoothed = mBias;
        mOutputGainSmoothed = mOutputGainTarget;
        AdvanceFilters(1.0);
    }

    void Process(float** inputs, float** outputs, int numSamples) override
    {
        if (!outputs || numSamples <= 0)
        {
            return;
        }

        const int stageCount = std::clamp(mStageCount, 1, kMaxStages);
        const float smoothStep = 1.0f - mControlSmoothCoef;

        for (int i = 0; i < numSamples; ++i)
        {
            float highInput[2][4] = {};
            float highOutput[2][4] = {};
            for (int ch = 0; ch < 2; ++ch)
            {
                const float* in = inputs ? inputs[ch] : nullptr;
                const float sample = in ? in[i] : 0.0f;
                if (mOversamplingFactor == 1)
                {
                    highInput[ch][0] = sample;
                }
                else
                {
                    float firstEven = 0.0f, firstOdd = 0.0f;
                    mUpFirst[ch].Upsample(sample, firstEven, firstOdd);
                    if (mOversamplingFactor == 2)
                    {
                        highInput[ch][0] = firstEven;
                        highInput[ch][1] = firstOdd;
                    }
                    else
                    {
                        mUpSecond[ch].Upsample(firstEven, highInput[ch][0], highInput[ch][1]);
                        mUpSecond[ch].Upsample(firstOdd, highInput[ch][2], highInput[ch][3]);
                    }
                }
            }

            for (int sub = 0; sub < mOversamplingFactor; ++sub)
            {
                mVoiceSmoothed += smoothStep * (mVoice - mVoiceSmoothed);
                mGainSmoothed += smoothStep * (mGain - mGainSmoothed);
                mStageGainSmoothed += smoothStep * (mStageGainLinear - mStageGainSmoothed);
                mPowerDriveSmoothed += smoothStep * (mPowerDrive - mPowerDriveSmoothed);
                mSagSmoothed += smoothStep * (mSag - mSagSmoothed);
                mBiasSmoothed += smoothStep * (mBias - mBiasSmoothed);
                mOutputGainSmoothed += smoothStep * (mOutputGainTarget - mOutputGainSmoothed);
                AdvanceFilters(1.0 - mFilterSmoothCoef);

                for (int ch = 0; ch < 2; ++ch)
                {
                    highOutput[ch][sub] = ProcessAmpSample(highInput[ch][sub], ch, stageCount);
                }
            }

            for (int ch = 0; ch < 2; ++ch)
            {
                if (!outputs[ch])
                {
                    continue;
                }
                float output = highOutput[ch][0];
                if (mOversamplingFactor == 2)
                {
                    output = mDownFirst[ch].Downsample(highOutput[ch][0], highOutput[ch][1]);
                }
                else if (mOversamplingFactor == 4)
                {
                    const float first = mDownSecond[ch].Downsample(highOutput[ch][0], highOutput[ch][1]);
                    const float second = mDownSecond[ch].Downsample(highOutput[ch][2], highOutput[ch][3]);
                    output = mDownFirst[ch].Downsample(first, second);
                }
                outputs[ch][i] = output;
            }
        }
    }

    void SetParam(const std::string& key, double value) override
    {
        if (key == "voice")
        {
            mVoice = static_cast<float>(std::clamp(value, 0.0, 1.0));
        }
        else if (key == "gain")
        {
            mGain = static_cast<float>(std::clamp(value, 0.0, 1.0));
        }
        else if (key == "bright")
        {
            mBright = (value >= 0.5) ? 1.0f : 0.0f;
            UpdatePreEmphasis();
        }
        else if (key == "preEmphasis")
        {
            mPreEmphasis = static_cast<float>(std::clamp(value, 0.0, 1.0));
            UpdatePreEmphasis();
        }
        else if (key == "bass")
        {
            mBass = std::clamp(value, 0.0, 1.0);
            UpdateToneStack();
        }
        else if (key == "middle")
        {
            mMiddle = std::clamp(value, 0.0, 1.0);
            UpdateToneStack();
        }
        else if (key == "treble")
        {
            mTreble = std::clamp(value, 0.0, 1.0);
            UpdateToneStack();
        }
        else if (key == "contour")
        {
            mContour = std::clamp(value, 0.0, 1.0);
            UpdateToneStack();
        }
        else if (key == "presence")
        {
            mPresence = std::clamp(value, 0.0, 1.0);
            UpdateToneStack();
        }
        else if (key == "output")
        {
            mOutputDb = std::clamp(value, -24.0, 24.0);
            mOutputGainTarget = DbToLinear(mOutputDb);
        }
        else if (key == "stageCount")
        {
            const int count = static_cast<int>(std::round(value));
            mStageCount = std::clamp(count, 1, kMaxStages);
        }
        else if (key == "stageGain" || key == "stage1Gain" || key == "stage2Gain" || key == "stage3Gain" ||
                 key == "stage4Gain" || key == "stage5Gain" || key == "stage6Gain")
        {
            SetStageGain(value);
        }
        else if (key == "powerDrive")
        {
            mPowerDrive = static_cast<float>(std::clamp(value, 0.0, 1.0));
        }
        else if (key == "sag")
        {
            mSag = static_cast<float>(std::clamp(value, 0.0, 1.0));
        }
        else if (key == "bias")
        {
            mBias = static_cast<float>(std::clamp(value, -1.0, 1.0));
        }
        else if (key == "depth")
        {
            mDepth = static_cast<float>(std::clamp(value, 0.0, 1.0));
            UpdateSpeakerFilters();
        }
        else if (key == "resonance")
        {
            mResonance = static_cast<float>(std::clamp(value, 0.0, 1.0));
            UpdateSpeakerFilters();
        }
        else if (key == "damping")
        {
            mDamping = static_cast<float>(std::clamp(value, 0.0, 1.0));
            UpdateSpeakerFilters();
        }
    }

    void SetConfig(const std::string&, const std::string&) override
    {
    }

    [[nodiscard]] double GetParam(const std::string& key) const override
    {
        if (key == "voice")
        {
            return mVoice;
        }

        if (key == "gain")
        {
            return mGain;
        }

        if (key == "bright")
        {
            return mBright;
        }

        if (key == "preEmphasis")
        {
            return mPreEmphasis;
        }

        if (key == "bass")
        {
            return mBass;
        }

        if (key == "middle")
        {
            return mMiddle;
        }

        if (key == "treble")
        {
            return mTreble;
        }

        if (key == "contour")
        {
            return mContour;
        }

        if (key == "presence")
        {
            return mPresence;
        }

        if (key == "output")
        {
            return mOutputDb;
        }

        if (key == "stageCount")
        {
            return mStageCount;
        }

        if (key == "stageGain" || key == "stage1Gain" || key == "stage2Gain" || key == "stage3Gain" ||
            key == "stage4Gain" || key == "stage5Gain" || key == "stage6Gain")
        {
            return mStageGainDb;
        }

        if (key == "powerDrive")
        {
            return mPowerDrive;
        }

        if (key == "sag")
        {
            return mSag;
        }

        if (key == "bias")
        {
            return mBias;
        }

        if (key == "depth")
        {
            return mDepth;
        }

        if (key == "resonance")
        {
            return mResonance;
        }

        if (key == "damping")
        {
            return mDamping;
        }

        return 0.0;
    }

    [[nodiscard]] std::string GetType() const override
    {
        return "amp_builtin";
    }

    [[nodiscard]] std::string GetCategory() const override
    {
        return "amp";
    }

  private:
    static constexpr double kPi = 3.14159265358979323846;
    static constexpr int kMaxStages = 4;

    struct Coefficients
    {
        double b0 = 1.0, b1 = 0.0, b2 = 0.0, a1 = 0.0, a2 = 0.0;
    };

    struct StageFilter
    {
        void Prepare(double sampleRate, double highPassHz, double lowPassHz)
        {
            const double hp = std::min(highPassHz, sampleRate * 0.45);
            const double lp = std::min(lowPassHz, sampleRate * 0.45);
            hpCoefficient = static_cast<float>(std::exp(-2.0 * kPi * hp / sampleRate));
            lpStep = static_cast<float>(1.0 - std::exp(-2.0 * kPi * lp / sampleRate));
            Reset();
        }

        void Reset()
        {
            previousInput.fill(0.0f);
            previousHighPass.fill(0.0f);
            previousLowPass.fill(0.0f);
        }

        float Process(float sample, int channel)
        {
            const float highPassed = hpCoefficient * (previousHighPass[channel] + sample - previousInput[channel]);
            previousInput[channel] = sample;
            previousHighPass[channel] = highPassed;
            previousLowPass[channel] += lpStep * (highPassed - previousLowPass[channel]);
            return previousLowPass[channel];
        }

        float hpCoefficient = 0.0f;
        float lpStep = 1.0f;
        std::array<float, 2> previousInput = {};
        std::array<float, 2> previousHighPass = {};
        std::array<float, 2> previousLowPass = {};
    };

    static float SoftClip(float x)
    {
        return std::tanh(x);
    }

    static float AsymmetricClip(float x, float bias)
    {
        const float offset = SoftClip(bias);
        return (SoftClip(x + bias) - offset) / (1.0f - offset * offset);
    }

    float ProcessAmpSample(float sample, int ch, int stageCount)
    {
        double signal = ProcessBiquad(sample, mPreHPB0, mPreHPB1, mPreHPB2, mPreHPA1, mPreHPA2,
                                      mPreHPS1[ch], mPreHPS2[ch]);
        signal = ProcessBiquad(signal, mPreEmphB0, mPreEmphB1, mPreEmphB2, mPreEmphA1, mPreEmphA2,
                               mPreEmphS1[ch], mPreEmphS2[ch]);

        const float gain = mGainSmoothed;
        const float input = static_cast<float>(signal) * mStageGainSmoothed;
        const float clean = AsymmetricClip(input * (2.0f + 2.5f * gain), 0.07f);
        const float drive = AsymmetricClip(input * (5.0f + 9.0f * gain), 0.15f);
        float stage = (clean + (drive - clean) * mVoiceSmoothed) * 0.9f;
        stage = mStageFilters[0].Process(stage, ch);

        if (stageCount >= 2)
        {
            stage = AsymmetricClip(stage * (1.1f + 1.5f * gain), -0.10f) * 0.85f;
            stage = mStageFilters[1].Process(stage, ch);
        }

        signal = ProcessBiquad(stage, mLowB0, mLowB1, mLowB2, mLowA1, mLowA2, mLowS1[ch], mLowS2[ch]);
        signal = ProcessBiquad(signal, mMidB0, mMidB1, mMidB2, mMidA1, mMidA2, mMidS1[ch], mMidS2[ch]);
        signal = ProcessBiquad(signal, mContourB0, mContourB1, mContourB2, mContourA1, mContourA2,
                               mContourS1[ch], mContourS2[ch]);
        signal = ProcessBiquad(signal, mTrebleB0, mTrebleB1, mTrebleB2, mTrebleA1, mTrebleA2,
                               mTrebleS1[ch], mTrebleS2[ch]);

        stage = static_cast<float>(signal);
        if (stageCount >= 3)
        {
            stage = AsymmetricClip(stage * (1.6f + 1.2f * gain), 0.12f) * 0.8f;
            stage = mStageFilters[2].Process(stage, ch);
        }
        if (stageCount >= 4)
        {
            stage = AsymmetricClip(stage * (1.35f + 0.8f * gain), -0.06f) * 0.8f;
            stage = mStageFilters[3].Process(stage, ch);
        }

        // Frequency-dependent power-stage drive: these controls shape the
        // distortion as well as the level. The external IR supplies cabinet
        // and microphone tone.
        signal = ProcessBiquad(stage, mPresenceB0, mPresenceB1, mPresenceB2, mPresenceA1, mPresenceA2,
                               mPresenceS1[ch], mPresenceS2[ch]);
        signal = ProcessBiquad(signal, mDepthB0, mDepthB1, mDepthB2, mDepthA1, mDepthA2,
                               mDepthS1[ch], mDepthS2[ch]);
        signal = ProcessBiquad(signal, mResonanceB0, mResonanceB1, mResonanceB2, mResonanceA1, mResonanceA2,
                               mResonanceS1[ch], mResonanceS2[ch]);
        signal = ProcessBiquad(signal, mDampingB0, mDampingB1, mDampingB2, mDampingA1, mDampingA2,
                               mDampingS1[ch], mDampingS2[ch]);

        float powerInput = static_cast<float>(signal);
        const float detector = std::abs(powerInput);
        const float sagCoefficient = detector > mSagEnv[ch] ? mSagAttackCoef : mSagReleaseCoef;
        mSagEnv[ch] = sagCoefficient * mSagEnv[ch] + (1.0f - sagCoefficient) * detector;
        const float headroom = 1.0f / (1.0f + 0.6f * mSagSmoothed * mSagEnv[ch]);
        powerInput *= headroom;

        const float driveAmount = mPowerDriveSmoothed;
        const float powerGain = 1.0f + 3.0f * driveAmount;
        const float bias = 0.08f * mBiasSmoothed;
        const float clipped = headroom *
                              (SoftClip(powerGain * (powerInput / headroom + bias)) - SoftClip(powerGain * bias)) /
                              SoftClip(powerGain);
        const float powered = powerInput + driveAmount * (clipped - powerInput);
        signal = ProcessBiquad(powered, mPostHPB0, mPostHPB1, mPostHPB2, mPostHPA1, mPostHPA2,
                               mPostHPS1[ch], mPostHPS2[ch]);
        return static_cast<float>(signal) * mOutputGainSmoothed;
    }

    static double ProcessBiquad(double input, double b0, double b1, double b2, double a1, double a2, double& s1,
                                double& s2)
    {
        const double output = b0 * input + s1;
        s1 = b1 * input - a1 * output + s2;
        s2 = b2 * input - a2 * output;
        return output;
    }

    static float DbToLinear(double db)
    {
        return static_cast<float>(std::pow(10.0, db * 0.05));
    }

    void SetStageGain(double value)
    {
        const double clamped = std::clamp(value, -24.0, 24.0);
        mStageGainDb = clamped;
        mStageGainLinear = DbToLinear(clamped);
    }

    static void Approach(double& current, double target, double step)
    {
        current += step * (target - current);
    }

    static void SmoothCoefficients(double& b0, double& b1, double& b2, double& a1, double& a2,
                                   const Coefficients& target, double step)
    {
        Approach(b0, target.b0, step);
        Approach(b1, target.b1, step);
        Approach(b2, target.b2, step);
        Approach(a1, target.a1, step);
        Approach(a2, target.a2, step);
    }

    void AdvanceFilters(double step)
    {
        SmoothCoefficients(mPreHPB0, mPreHPB1, mPreHPB2, mPreHPA1, mPreHPA2, mPreHPTarget, step);
        SmoothCoefficients(mPreEmphB0, mPreEmphB1, mPreEmphB2, mPreEmphA1, mPreEmphA2, mPreEmphTarget, step);
        SmoothCoefficients(mLowB0, mLowB1, mLowB2, mLowA1, mLowA2, mLowTarget, step);
        SmoothCoefficients(mMidB0, mMidB1, mMidB2, mMidA1, mMidA2, mMidTarget, step);
        SmoothCoefficients(mContourB0, mContourB1, mContourB2, mContourA1, mContourA2, mContourTarget, step);
        SmoothCoefficients(mTrebleB0, mTrebleB1, mTrebleB2, mTrebleA1, mTrebleA2, mTrebleTarget, step);
        SmoothCoefficients(mPresenceB0, mPresenceB1, mPresenceB2, mPresenceA1, mPresenceA2, mPresenceTarget, step);
        SmoothCoefficients(mDepthB0, mDepthB1, mDepthB2, mDepthA1, mDepthA2, mDepthTarget, step);
        SmoothCoefficients(mResonanceB0, mResonanceB1, mResonanceB2, mResonanceA1, mResonanceA2,
                           mResonanceTarget, step);
        SmoothCoefficients(mDampingB0, mDampingB1, mDampingB2, mDampingA1, mDampingA2, mDampingTarget, step);
        SmoothCoefficients(mPostHPB0, mPostHPB1, mPostHPB2, mPostHPA1, mPostHPA2, mPostHPTarget, step);
    }

    void UpdateSmoothing()
    {
        if (mDspSampleRate <= 0.0)
        {
            mControlSmoothCoef = 0.0f;
            mFilterSmoothCoef = 0.0;
            return;
        }

        mControlSmoothCoef = static_cast<float>(std::exp(-1.0 / (0.01 * mDspSampleRate)));
        mFilterSmoothCoef = std::exp(-1.0 / (0.005 * mDspSampleRate));
    }

    void UpdateSagCoefficients()
    {
        if (mSampleRate <= 0.0)
        {
            mSagAttackCoef = 0.0f;
            mSagReleaseCoef = 0.0f;
            return;
        }

        const double attackTau = 0.01;  // 10 ms
        const double releaseTau = 0.18; // 180 ms
        mSagAttackCoef = static_cast<float>(std::exp(-1.0 / (attackTau * mDspSampleRate)));
        mSagReleaseCoef = static_cast<float>(std::exp(-1.0 / (releaseTau * mDspSampleRate)));
    }

    void UpdateToneStack()
    {
        if (mSampleRate <= 0.0)
        {
            return;
        }

        const double bassGain = (mBass - 0.5) * 18.0;
        const double midGain = (mMiddle - 0.5) * 18.0;
        const double trebleGain = (mTreble - 0.5) * 18.0;
        const double contourGain = -12.0 * mContour;
        const double presenceGain = (mPresence - 0.5) * 12.0;

        ComputeLowShelf(120.0, 0.8, bassGain, mLowTarget.b0, mLowTarget.b1, mLowTarget.b2, mLowTarget.a1,
                        mLowTarget.a2);
        ComputePeakingEQ(750.0, 0.9, midGain, mMidTarget.b0, mMidTarget.b1, mMidTarget.b2, mMidTarget.a1,
                         mMidTarget.a2);
        ComputePeakingEQ(600.0, 0.7, contourGain, mContourTarget.b0, mContourTarget.b1, mContourTarget.b2,
                         mContourTarget.a1, mContourTarget.a2);
        ComputeHighShelf(3500.0, 0.9, trebleGain, mTrebleTarget.b0, mTrebleTarget.b1, mTrebleTarget.b2,
                         mTrebleTarget.a1, mTrebleTarget.a2);
        ComputePeakingEQ(4000.0, 1.2, presenceGain, mPresenceTarget.b0, mPresenceTarget.b1, mPresenceTarget.b2,
                         mPresenceTarget.a1, mPresenceTarget.a2);
    }

    void UpdatePreFilters()
    {
        if (mSampleRate <= 0.0)
        {
            return;
        }

        ComputeHighPass(60.0, 0.707, mPreHPTarget.b0, mPreHPTarget.b1, mPreHPTarget.b2, mPreHPTarget.a1,
                        mPreHPTarget.a2);
    }

    void UpdatePreEmphasis()
    {
        if (mSampleRate <= 0.0)
        {
            return;
        }

        const double brightBoost = (mBright > 0.5f) ? 3.0 : 0.0;
        const double emphasisBoost = static_cast<double>(mPreEmphasis) * 6.0;
        const double gainDb = brightBoost + emphasisBoost;

        ComputeHighShelf(2500.0, 0.8, gainDb, mPreEmphTarget.b0, mPreEmphTarget.b1, mPreEmphTarget.b2,
                         mPreEmphTarget.a1, mPreEmphTarget.a2);
    }

    void UpdateSpeakerFilters()
    {
        if (mSampleRate <= 0.0)
        {
            return;
        }

        const double depthGain = static_cast<double>(mDepth) * 6.0;
        const double resonanceGain = static_cast<double>(mResonance) * 6.0;
        const double dampingGain = static_cast<double>(mDamping) * -6.0;

        ComputeLowShelf(120.0, 0.9, depthGain, mDepthTarget.b0, mDepthTarget.b1, mDepthTarget.b2,
                        mDepthTarget.a1, mDepthTarget.a2);
        ComputePeakingEQ(120.0, 1.0, resonanceGain, mResonanceTarget.b0, mResonanceTarget.b1,
                         mResonanceTarget.b2, mResonanceTarget.a1, mResonanceTarget.a2);
        ComputeHighShelf(3500.0, 0.9, dampingGain, mDampingTarget.b0, mDampingTarget.b1, mDampingTarget.b2,
                         mDampingTarget.a1, mDampingTarget.a2);
    }

    void UpdatePostFilters()
    {
        if (mSampleRate <= 0.0)
        {
            return;
        }

        ComputeHighPass(25.0, 0.707, mPostHPTarget.b0, mPostHPTarget.b1, mPostHPTarget.b2,
                        mPostHPTarget.a1, mPostHPTarget.a2);
    }

    void ComputeHighPass(double freq, double Q, double& b0, double& b1, double& b2, double& a1, double& a2)
    {
        const double w0 = 2.0 * kPi * ClampBiquadFrequency(freq, mDspSampleRate) / mDspSampleRate;
        const double cosw0 = std::cos(w0);
        const double sinw0 = std::sin(w0);
        const double alpha = sinw0 / (2.0 * Q);

        const double a0 = 1.0 + alpha;
        b0 = (1.0 + cosw0) / 2.0 / a0;
        b1 = -(1.0 + cosw0) / a0;
        b2 = (1.0 + cosw0) / 2.0 / a0;
        a1 = (-2.0 * cosw0) / a0;
        a2 = (1.0 - alpha) / a0;
    }

    void ComputeLowShelf(double freq, double slope, double gainDb, double& b0, double& b1, double& b2, double& a1,
                         double& a2)
    {
        const double A = std::pow(10.0, gainDb / 40.0);
        const double w0 = 2.0 * kPi * ClampBiquadFrequency(freq, mDspSampleRate) / mDspSampleRate;
        const double cosw0 = std::cos(w0);
        const double sinw0 = std::sin(w0);
        const double sqrtA = std::sqrt(A);
        const double alpha = sinw0 / 2.0 * std::sqrt((A + 1.0 / A) * (1.0 / slope - 1.0) + 2.0);

        const double a0 = (A + 1.0) + (A - 1.0) * cosw0 + 2.0 * sqrtA * alpha;
        b0 = A * ((A + 1.0) - (A - 1.0) * cosw0 + 2.0 * sqrtA * alpha) / a0;
        b1 = 2.0 * A * ((A - 1.0) - (A + 1.0) * cosw0) / a0;
        b2 = A * ((A + 1.0) - (A - 1.0) * cosw0 - 2.0 * sqrtA * alpha) / a0;
        a1 = -2.0 * ((A - 1.0) + (A + 1.0) * cosw0) / a0;
        a2 = ((A + 1.0) + (A - 1.0) * cosw0 - 2.0 * sqrtA * alpha) / a0;
    }

    void ComputeHighShelf(double freq, double slope, double gainDb, double& b0, double& b1, double& b2, double& a1,
                          double& a2)
    {
        const double A = std::pow(10.0, gainDb / 40.0);
        const double w0 = 2.0 * kPi * ClampBiquadFrequency(freq, mDspSampleRate) / mDspSampleRate;
        const double cosw0 = std::cos(w0);
        const double sinw0 = std::sin(w0);
        const double sqrtA = std::sqrt(A);
        const double alpha = sinw0 / 2.0 * std::sqrt((A + 1.0 / A) * (1.0 / slope - 1.0) + 2.0);

        const double a0 = (A + 1.0) - (A - 1.0) * cosw0 + 2.0 * sqrtA * alpha;
        b0 = A * ((A + 1.0) + (A - 1.0) * cosw0 + 2.0 * sqrtA * alpha) / a0;
        b1 = -2.0 * A * ((A - 1.0) + (A + 1.0) * cosw0) / a0;
        b2 = A * ((A + 1.0) + (A - 1.0) * cosw0 - 2.0 * sqrtA * alpha) / a0;
        a1 = 2.0 * ((A - 1.0) - (A + 1.0) * cosw0) / a0;
        a2 = ((A + 1.0) - (A - 1.0) * cosw0 - 2.0 * sqrtA * alpha) / a0;
    }

    void ComputePeakingEQ(double freq, double Q, double gainDb, double& b0, double& b1, double& b2, double& a1,
                          double& a2)
    {
        const double A = std::pow(10.0, gainDb / 40.0);
        const double w0 = 2.0 * kPi * ClampBiquadFrequency(freq, mDspSampleRate) / mDspSampleRate;
        const double cosw0 = std::cos(w0);
        const double sinw0 = std::sin(w0);
        const double alpha = sinw0 / (2.0 * Q);

        const double a0 = 1.0 + alpha / A;
        b0 = (1.0 + alpha * A) / a0;
        b1 = (-2.0 * cosw0) / a0;
        b2 = (1.0 - alpha * A) / a0;
        a1 = (-2.0 * cosw0) / a0;
        a2 = (1.0 - alpha / A) / a0;
    }

    double mSampleRate = 44100.0;
    double mDspSampleRate = 44100.0;
    int mMaxBlockSize = 0;
    int mOversamplingFactor = 1;
    std::array<BuiltinAmpHalfband2x, 2> mUpFirst = {}, mDownFirst = {}, mUpSecond = {}, mDownSecond = {};
    std::array<StageFilter, kMaxStages> mStageFilters = {};

    float mVoice = 0.0f;
    float mVoiceSmoothed = 0.0f;
    float mControlSmoothCoef = 0.0f;
    double mFilterSmoothCoef = 0.0;
    float mGain = 0.45f;
    float mGainSmoothed = 0.45f;
    double mBass = 0.5;
    double mMiddle = 0.5;
    double mTreble = 0.5;
    double mContour = 0.2;
    double mPresence = 0.5;
    double mOutputDb = 0.0;
    float mOutputGainTarget = 1.0f;
    float mOutputGainSmoothed = 1.0f;
    int mStageCount = 2;
    double mStageGainDb = 0.0;
    float mStageGainLinear = 1.0f;
    float mStageGainSmoothed = 1.0f;
    float mBright = 0.0f;
    float mPreEmphasis = 0.0f;
    float mPowerDrive = 0.0f;
    float mPowerDriveSmoothed = 0.0f;
    float mSag = 0.0f;
    float mSagSmoothed = 0.0f;
    float mBias = 0.0f;
    float mBiasSmoothed = 0.0f;
    float mDepth = 0.4f;
    float mResonance = 0.4f;
    float mDamping = 0.5f;
    float mSagAttackCoef = 0.0f;
    float mSagReleaseCoef = 0.0f;

    Coefficients mPreHPTarget, mPreEmphTarget, mLowTarget, mMidTarget, mContourTarget, mTrebleTarget;
    Coefficients mPresenceTarget, mDepthTarget, mResonanceTarget, mDampingTarget, mPostHPTarget;

    double mPreHPB0 = 1.0, mPreHPB1 = 0.0, mPreHPB2 = 0.0, mPreHPA1 = 0.0, mPreHPA2 = 0.0;
    double mPreEmphB0 = 1.0, mPreEmphB1 = 0.0, mPreEmphB2 = 0.0, mPreEmphA1 = 0.0, mPreEmphA2 = 0.0;
    double mDepthB0 = 1.0, mDepthB1 = 0.0, mDepthB2 = 0.0, mDepthA1 = 0.0, mDepthA2 = 0.0;
    double mResonanceB0 = 1.0, mResonanceB1 = 0.0, mResonanceB2 = 0.0, mResonanceA1 = 0.0, mResonanceA2 = 0.0;
    double mDampingB0 = 1.0, mDampingB1 = 0.0, mDampingB2 = 0.0, mDampingA1 = 0.0, mDampingA2 = 0.0;
    double mPostHPB0 = 1.0, mPostHPB1 = 0.0, mPostHPB2 = 0.0, mPostHPA1 = 0.0, mPostHPA2 = 0.0;

    double mLowB0 = 1.0, mLowB1 = 0.0, mLowB2 = 0.0, mLowA1 = 0.0, mLowA2 = 0.0;
    double mMidB0 = 1.0, mMidB1 = 0.0, mMidB2 = 0.0, mMidA1 = 0.0, mMidA2 = 0.0;
    double mContourB0 = 1.0, mContourB1 = 0.0, mContourB2 = 0.0, mContourA1 = 0.0, mContourA2 = 0.0;
    double mTrebleB0 = 1.0, mTrebleB1 = 0.0, mTrebleB2 = 0.0, mTrebleA1 = 0.0, mTrebleA2 = 0.0;
    double mPresenceB0 = 1.0, mPresenceB1 = 0.0, mPresenceB2 = 0.0, mPresenceA1 = 0.0, mPresenceA2 = 0.0;

    std::array<double, 2> mLowS1 = {}, mLowS2 = {};
    std::array<double, 2> mMidS1 = {}, mMidS2 = {};
    std::array<double, 2> mContourS1 = {}, mContourS2 = {};
    std::array<double, 2> mTrebleS1 = {}, mTrebleS2 = {};
    std::array<double, 2> mPresenceS1 = {}, mPresenceS2 = {};
    std::array<double, 2> mPreHPS1 = {}, mPreHPS2 = {};
    std::array<double, 2> mPreEmphS1 = {}, mPreEmphS2 = {};
    std::array<double, 2> mDepthS1 = {}, mDepthS2 = {};
    std::array<double, 2> mResonanceS1 = {}, mResonanceS2 = {};
    std::array<double, 2> mDampingS1 = {}, mDampingS2 = {};
    std::array<double, 2> mPostHPS1 = {}, mPostHPS2 = {};
    std::array<float, 2> mSagEnv = {0.0f, 0.0f};
};

inline void RegisterBuiltinAmpEffect()
{
    EffectTypeInfo info;
    info.type = EffectGuids::kAmpBuiltin;
    info.aliases = {"amp_builtin"};
    info.displayName = "Heavy American";
    info.category = "amp";
    info.description = "High-gain amp head for use with a separate cabinet or IR";
    info.requiresResource = false;
    info.parameters = {{"voice", "Voice", 0.0, 0.0, 1.0, "toggle", "Input"},
                       {"gain", "Gain", 0.45, 0.0, 1.0, "amount", "Input"},
                       {"bright", "Bright", 0.0, 0.0, 1.0, "toggle", "Input"},
                       {"preEmphasis", "Pre Emphasis", 0.0, 0.0, 1.0, "amount", "Input", true},
                       {"stageCount", "Preamp Stages", 2.0, 1.0, 4.0, "amount", "Input", false, 1.0},
                       {"stageGain", "Input Trim", 0.0, -24.0, 24.0, "dB", "Input"},
                       {"bass", "Bass", 0.5, 0.0, 1.0, "amount", "Tone"},
                       {"middle", "Middle", 0.5, 0.0, 1.0, "amount", "Tone"},
                       {"treble", "Treble", 0.5, 0.0, 1.0, "amount", "Tone"},
                       {"contour", "Contour", 0.2, 0.0, 1.0, "amount", "Tone"},
                       {"presence", "Presence", 0.5, 0.0, 1.0, "amount", "Tone"},
                       {"output", "Output", 0.0, -24.0, 24.0, "dB", "Output"},
                       {"powerDrive", "Power Drive", 0.0, 0.0, 1.0, "amount", "Power", true},
                       {"sag", "Sag", 0.0, 0.0, 1.0, "amount", "Power", true},
                       {"bias", "Bias", 0.0, -1.0, 1.0, "amount", "Power", true},
                       {"depth", "Depth", 0.4, 0.0, 1.0, "amount", "Power", true},
                       {"resonance", "Resonance", 0.4, 0.0, 1.0, "amount", "Power", true},
                       {"damping", "Damping", 0.5, 0.0, 1.0, "amount", "Power", true}};

    EffectRegistry::Instance().Register(info.type, info, []() { return std::make_unique<BuiltinAmpEffect>(); });
}
} // namespace guitarfx

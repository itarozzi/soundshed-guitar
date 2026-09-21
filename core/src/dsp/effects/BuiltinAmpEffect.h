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

        UpdateStageFilters();
        UpdateSmoothing();
        UpdateSagCoefficients();
        UpdatePreFilters();
        UpdatePreEmphasis();
        UpdateToneStack();
        UpdateVoicingFilter();
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
            mVoicingS1[ch] = mVoicingS2[ch] = 0.0;
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
        mCharacterSmoothed = mCharacter;
        mOutputGainSmoothed = mOutputGainTarget;
        AdvanceFilters(1.0);
        mVoicingStale = true;
        mRightChannelStale = false;
        UpdateVoicing(std::clamp(mStageCount, 1, kMaxStages));
    }

    void Process(float** inputs, float** outputs, int numSamples) override
    {
        if (!outputs || numSamples <= 0)
        {
            return;
        }

        if (mRightChannelStale)
        {
            // The right channel sat still while the input was mono. Its input
            // was the left channel's all along, so the left state is exactly
            // where it would have got to.
            CopyChannelState(0, 1);
            mRightChannelStale = false;
        }

        const float* in[2] = {inputs ? inputs[0] : nullptr, inputs ? inputs[1] : nullptr};
        Render<2>(in, outputs, numSamples);
    }

    // A guitar is mono, and the executor hands a mono signal to effects that
    // can take it. Nearly all of this amp's work is per channel (4x resampling
    // and the whole nonlinear chain), so running one channel halves the cost.
    [[nodiscard]] bool SupportsMonoProcessing() const override
    {
        return true;
    }

    void ProcessMono(float* input, float* output, int numSamples) override
    {
        if (!output || numSamples <= 0)
        {
            return;
        }

        const float* in[1] = {input};
        float* out[1] = {output};
        Render<1>(in, out, numSamples);
        mRightChannelStale = true;
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
        else if (key == "character")
        {
            mCharacter = static_cast<float>(std::clamp(value, 0.0, 1.0));
            UpdateStageFilters();
            UpdatePreFilters();
            UpdatePreEmphasis();
            UpdateVoicingFilter();
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

        if (key == "character")
        {
            return mCharacter;
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

    /**
     * The output trim that keeps Gain and Preamp Stages from being volume
     * controls, in dB (see kHeardLevelDb). Public so the level table can be
     * re-measured with the makeup taken back out:
     * BuiltinAmpEffectTests --measure-levels.
     */
    [[nodiscard]] static float LevelMakeupDb(float gain, float voice, int stages)
    {
        const float clean = HeardLevelDb(0, kDefaultStages, kDefaultGain) - HeardLevelDb(0, stages, gain);
        const float drive = HeardLevelDb(1, kDefaultStages, kDefaultGain) - HeardLevelDb(1, stages, gain);
        return clean + voice * (drive - clean);
    }

  private:
    static constexpr double kPi = 3.14159265358979323846;
    static constexpr int kMaxStages = 4;

    // Interstage coupling at the default Character. The corners tighten
    // monotonically down the chain so the most saturated stages see the least
    // low end, which is what keeps a palm-muted low string defined instead of
    // intermodulating. They have to stay this low, and stay in order: these are
    // one-poles in series, so a corner up near 180 Hz partway down the chain
    // costs a low E most of its fundamental before the tone stack ever sees it,
    // and a stage that is looser than the one before it takes harmonics back
    // out instead of adding them. Character scales the whole set together, so
    // the order holds at every setting.
    static constexpr double kStageHighPass[kMaxStages] = {38.0, 70.0, 100.0, 120.0};
    static constexpr double kStageLowPass[kMaxStages] = {12000.0, 9000.0, 7000.0, 6000.0};

    // Clipper operating points, before Character scales how far off centre
    // they sit. Stage one has a clean and a drive leg, crossfaded by voice.
    enum ClipIndex
    {
        kClipClean,
        kClipDrive,
        kClipStage2,
        kClipStage3,
        kClipStage4,
        kClipCount
    };
    static constexpr float kClipBias[kClipCount] = {0.07f, 0.15f, -0.10f, 0.12f, -0.06f};

    // Every piece of per-channel state; the same list Reset() clears. If a
    // new filter or envelope is added to one, add it here too, or the right
    // channel comes back from a mono stretch out of step with the left
    // (TestMonoPath catches that).
    void CopyChannelState(int from, int to)
    {
        for (auto* bank : {&mUpFirst, &mDownFirst, &mUpSecond, &mDownSecond})
        {
            (*bank)[to] = (*bank)[from];
        }
        for (auto& filter : mStageFilters)
        {
            filter.CopyChannel(from, to);
        }
        for (auto* state : {&mLowS1, &mLowS2, &mMidS1, &mMidS2, &mContourS1, &mContourS2, &mTrebleS1, &mTrebleS2,
                            &mPresenceS1, &mPresenceS2, &mVoicingS1, &mVoicingS2, &mPreHPS1, &mPreHPS2,
                            &mPreEmphS1, &mPreEmphS2, &mDepthS1, &mDepthS2, &mResonanceS1, &mResonanceS2,
                            &mDampingS1, &mDampingS2, &mPostHPS1, &mPostHPS2})
        {
            (*state)[to] = (*state)[from];
        }
        mSagEnv[to] = mSagEnv[from];
    }

    // The processing loop for one channel (ProcessMono) or two (Process).
    // Controls and voicing are shared and advance once per sample either way,
    // so a mono block and a stereo block leave them in the same place.
    template <int Channels>
    void Render(const float* const* inputs, float* const* outputs, int numSamples)
    {
        const int stageCount = std::clamp(mStageCount, 1, kMaxStages);
        const float smoothStep = 1.0f - mControlSmoothCoef;

        for (int i = 0; i < numSamples; ++i)
        {
            float highInput[Channels][4] = {};
            float highOutput[Channels][4] = {};
            for (int ch = 0; ch < Channels; ++ch)
            {
                const float* in = inputs[ch];
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

            // Controls glide at the host rate: 5-10 ms ramps gain nothing from
            // being stepped four times as often, and this is a lot of work
            // (every tone filter's coefficients) to repeat per oversampled
            // sample.
            Glide(mVoiceSmoothed, mVoice, smoothStep);
            Glide(mGainSmoothed, mGain, smoothStep);
            mStageGainSmoothed += smoothStep * (mStageGainLinear - mStageGainSmoothed);
            Glide(mPowerDriveSmoothed, mPowerDrive, smoothStep);
            mSagSmoothed += smoothStep * (mSag - mSagSmoothed);
            Glide(mBiasSmoothed, mBias, smoothStep);
            Glide(mCharacterSmoothed, mCharacter, smoothStep);
            mOutputGainSmoothed += smoothStep * (mOutputGainTarget - mOutputGainSmoothed);
            AdvanceFilters(1.0 - mFilterSmoothCoef);
            UpdateVoicing(stageCount);

            for (int sub = 0; sub < mOversamplingFactor; ++sub)
            {
                for (int ch = 0; ch < Channels; ++ch)
                {
                    highOutput[ch][sub] = ProcessAmpSample(highInput[ch][sub], ch, stageCount);
                }
            }

            for (int ch = 0; ch < Channels; ++ch)
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

    struct Coefficients
    {
        double b0 = 1.0, b1 = 0.0, b2 = 0.0, a1 = 0.0, a2 = 0.0;
    };

    struct StageFilter
    {
        // Sets where the corners are heading; Advance glides there so a
        // Character sweep never steps the interstage coupling mid-note.
        void SetCorners(double sampleRate, double highPassHz, double lowPassHz)
        {
            const double hp = std::min(highPassHz, sampleRate * 0.45);
            const double lp = std::min(lowPassHz, sampleRate * 0.45);
            hpTarget = static_cast<float>(std::exp(-2.0 * kPi * hp / sampleRate));
            lpTarget = static_cast<float>(1.0 - std::exp(-2.0 * kPi * lp / sampleRate));
        }

        void Advance(float step)
        {
            hpCoefficient += step * (hpTarget - hpCoefficient);
            lpStep += step * (lpTarget - lpStep);
        }

        void Reset()
        {
            hpCoefficient = hpTarget;
            lpStep = lpTarget;
            previousInput.fill(0.0f);
            previousHighPass.fill(0.0f);
            previousLowPass.fill(0.0f);
        }

        void CopyChannel(int from, int to)
        {
            previousInput[to] = previousInput[from];
            previousHighPass[to] = previousHighPass[from];
            previousLowPass[to] = previousLowPass[from];
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
        float hpTarget = 0.0f;
        float lpTarget = 1.0f;
        std::array<float, 2> previousInput = {};
        std::array<float, 2> previousHighPass = {};
        std::array<float, 2> previousLowPass = {};
    };

    // The three clipper knees Character morphs between. All have unit slope at
    // zero and saturate at ±1, so the blend changes the shape of the knee and
    // the harmonic balance, not the small-signal gain.
    //
    // Soft never quite arrives: it is already compressing at a tenth of full
    // scale and still rising at ten times it, the spongy, singing knee of a
    // fuzz or an old cascaded preamp. It is x / (1 + |x|) with the corner at
    // zero rounded off, because that corner is a curvature step every cycle
    // crosses and it buzzes. Hard is close to linear to about half of full
    // scale and then locks flat at 1.875, a sharper edge with more upper-order
    // content. It is a quintic that arrives with zero slope and zero
    // curvature: a cubic that only matched the slope left a curvature step at
    // the corner, whose harmonics fall off slowly enough to fold back. tanh
    // sits between the two and is exactly the old voicing.
    static constexpr float kSoftRound = 0.3f;
    static constexpr float kHardKnee = 1.875f;
    static constexpr float kHardCubic = 2.0f / (3.0f * kHardKnee * kHardKnee);
    static constexpr float kHardQuintic = 0.2f / (kHardKnee * kHardKnee * kHardKnee * kHardKnee);

    static float SoftKneeShape(float x)
    {
        const float r = std::sqrt(x * x + kSoftRound * kSoftRound);
        return x / (1.0f - kSoftRound + r);
    }

    static float SoftKneeSlope(float x)
    {
        const float r = std::sqrt(x * x + kSoftRound * kSoftRound);
        const float d = 1.0f - kSoftRound + r;
        return (d - x * x / r) / (d * d);
    }

    static float HardKneeShape(float x)
    {
        const float c = std::clamp(x, -kHardKnee, kHardKnee);
        const float c2 = c * c;
        return c * (1.0f - c2 * (kHardCubic - kHardQuintic * c2));
    }

    static float HardKneeSlope(float x)
    {
        if (std::abs(x) >= kHardKnee)
        {
            return 0.0f;
        }
        const float x2 = x * x;
        return 1.0f - x2 * (3.0f * kHardCubic - 5.0f * kHardQuintic * x2);
    }

    // At most two knees are ever live, and the weights only change with
    // Character, so these branches predict perfectly and the default costs the
    // one tanh it always did.
    float Shape(float x) const
    {
        if (mKneeTanh == 1.0f)
        {
            return std::tanh(x);
        }
        float y = 0.0f;
        if (mKneeTanh > 0.0f)
        {
            y += mKneeTanh * std::tanh(x);
        }
        if (mKneeSoft > 0.0f)
        {
            y += mKneeSoft * SoftKneeShape(x);
        }
        if (mKneeHard > 0.0f)
        {
            y += mKneeHard * HardKneeShape(x);
        }
        return y;
    }

    float ShapeSlope(float x) const
    {
        float slope = 0.0f;
        if (mKneeTanh > 0.0f)
        {
            const float t = std::tanh(x);
            slope += mKneeTanh * (1.0f - t * t);
        }
        if (mKneeSoft > 0.0f)
        {
            slope += mKneeSoft * SoftKneeSlope(x);
        }
        if (mKneeHard > 0.0f)
        {
            slope += mKneeHard * HardKneeSlope(x);
        }
        return slope;
    }

    // One biased stage: the offset keeps silence at zero and the slope keeps
    // the small-signal gain at one, whatever knee and bias Character picked.
    float BiasedClip(float x, int index) const
    {
        return (Shape(x + mClipBias[index]) - mClipOffset[index]) * mClipInvSlope[index];
    }

    /**
     * Everything derived from the smoothed controls that is the same for both
     * channels and all four oversampled steps, worked out once per host sample
     * instead of once per channel per stage.
     */
    void UpdateVoicing(int stageCount)
    {
        // Each group is only refreshed while its controls are moving; the
        // glides settle exactly on their targets, so a static setting costs a
        // handful of compares here, not tanh and exp2 every sample.
        const bool kneeMoved = mVoicingStale || mCharacterSmoothed != mVoicedCharacter;
        if (kneeMoved)
        {
            mVoicedCharacter = mCharacterSmoothed;
            const float character = mCharacterSmoothed;
            if (character <= 0.5f)
            {
                mKneeSoft = 1.0f - 2.0f * character;
                mKneeTanh = 2.0f * character;
                mKneeHard = 0.0f;
            }
            else
            {
                mKneeSoft = 0.0f;
                mKneeTanh = 2.0f - 2.0f * character;
                mKneeHard = 2.0f * character - 1.0f;
            }

            // Vintage stages sit further off centre, which is where the even
            // harmonics and the wooly, octave-leaning fuzz come from. Modern
            // ones are close to symmetric, so the spectrum is odd-order and
            // tight. Exactly 1.0 at the default.
            const float offCentre = 0.5f - character;
            const float biasScale = 1.0f + 2.2f * offCentre + 1.2f * offCentre * offCentre;
            for (int i = 0; i < kClipCount; ++i)
            {
                const float bias = kClipBias[i] * biasScale;
                mClipBias[i] = bias;
                mClipOffset[i] = Shape(bias);
                mClipInvSlope[i] = 1.0f / std::max(ShapeSlope(bias), 0.1f);
            }
        }

        if (kneeMoved || mPowerDriveSmoothed != mVoicedPowerDrive || mBiasSmoothed != mVoicedBias)
        {
            mVoicedPowerDrive = mPowerDriveSmoothed;
            mVoicedBias = mBiasSmoothed;
            mPowerGain = 1.0f + 3.0f * mPowerDriveSmoothed;
            mPowerBias = 0.08f * mBiasSmoothed;
            mPowerOffset = Shape(mPowerGain * mPowerBias);
            mPowerInvScale = 1.0f / std::max(Shape(mPowerGain), 0.1f);
        }

        if (mVoicingStale || mGainSmoothed != mVoicedGain || mVoiceSmoothed != mVoicedVoice ||
            stageCount != mVoicedStages)
        {
            mVoicedGain = mGainSmoothed;
            mVoicedVoice = mVoiceSmoothed;
            mVoicedStages = stageCount;
            mLevelMakeup = LevelMakeup(mGainSmoothed, mVoiceSmoothed, stageCount);
        }
        mVoicingStale = false;
    }

    // A one-pole glide that lands exactly on its target instead of creeping
    // towards it forever, so the voicing cache can tell when a control stops.
    static void Glide(float& value, float target, float step)
    {
        value += step * (target - value);
        if (std::abs(target - value) < 1.0e-6f)
        {
            value = target;
        }
    }

    /**
     * Drive law for one gain stage. A linear law spends the whole sweep getting
     * to crunch and never reaches a saturated high-gain cascade, because each
     * tanh bounds its own output and the next stage only sees a couple of times
     * that. The quartic term is the top of a log-taper pot: it barely moves
     * below about 0.6 and then opens the stage up by the ~15 dB that an
     * American high-gain preamp needs. Scaling it by the voice blend keeps the
     * clean channel exactly where it was.
     */
    static float StageDrive(float gain, float voice, float base, float linear, float top)
    {
        const float squared = gain * gain;
        return base + linear * gain + top * voice * squared * squared;
    }

    float ProcessAmpSample(float sample, int ch, int stageCount)
    {
        double signal = ProcessBiquad(sample, mPreHPB0, mPreHPB1, mPreHPB2, mPreHPA1, mPreHPA2,
                                      mPreHPS1[ch], mPreHPS2[ch]);
        signal = ProcessBiquad(signal, mPreEmphB0, mPreEmphB1, mPreEmphB2, mPreEmphA1, mPreEmphA2,
                               mPreEmphS1[ch], mPreEmphS2[ch]);

        const float gain = mGainSmoothed;
        const float voice = mVoiceSmoothed;
        const float input = static_cast<float>(signal) * mStageGainSmoothed;
        const float clean = BiasedClip(input * (2.0f + 2.5f * gain), kClipClean);
        const float drive = BiasedClip(input * StageDrive(gain, 1.0f, 5.0f, 9.0f, 55.0f), kClipDrive);
        float stage = (clean + (drive - clean) * voice) * 0.9f;
        stage = mStageFilters[0].Process(stage, ch);

        if (stageCount >= 2)
        {
            stage = BiasedClip(stage * StageDrive(gain, voice, 1.1f, 1.5f, 7.0f), kClipStage2) * 0.85f;
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
            stage = BiasedClip(stage * StageDrive(gain, voice, 1.6f, 1.2f, 8.0f), kClipStage3) * 0.8f;
            stage = mStageFilters[2].Process(stage, ch);
        }
        if (stageCount >= 4)
        {
            stage = BiasedClip(stage * StageDrive(gain, voice, 1.35f, 0.8f, 6.0f), kClipStage4) * 0.8f;
            stage = mStageFilters[3].Process(stage, ch);
        }

        signal = ProcessBiquad(stage, mVoicingB0, mVoicingB1, mVoicingB2, mVoicingA1, mVoicingA2, mVoicingS1[ch],
                               mVoicingS2[ch]);

        // Frequency-dependent power-stage drive: these controls shape the
        // distortion as well as the level. The external IR supplies cabinet
        // and microphone tone.
        signal = ProcessBiquad(signal, mPresenceB0, mPresenceB1, mPresenceB2, mPresenceA1, mPresenceA2,
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
        const float clipped =
            headroom * (Shape(mPowerGain * (powerInput / headroom + mPowerBias)) - mPowerOffset) * mPowerInvScale;
        const float powered = powerInput + driveAmount * (clipped - powerInput);
        signal = ProcessBiquad(powered, mPostHPB0, mPostHPB1, mPostHPB2, mPostHPA1, mPostHPA2,
                               mPostHPS1[ch], mPostHPS2[ch]);
        return static_cast<float>(signal) * mOutputGainSmoothed * mLevelMakeup;
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
        SmoothCoefficients(mVoicingB0, mVoicingB1, mVoicingB2, mVoicingA1, mVoicingA2, mVoicingTarget, step);
        SmoothCoefficients(mDepthB0, mDepthB1, mDepthB2, mDepthA1, mDepthA2, mDepthTarget, step);
        SmoothCoefficients(mResonanceB0, mResonanceB1, mResonanceB2, mResonanceA1, mResonanceA2,
                           mResonanceTarget, step);
        SmoothCoefficients(mDampingB0, mDampingB1, mDampingB2, mDampingA1, mDampingA2, mDampingTarget, step);
        SmoothCoefficients(mPostHPB0, mPostHPB1, mPostHPB2, mPostHPA1, mPostHPA2, mPostHPTarget, step);
        for (auto& filter : mStageFilters)
        {
            filter.Advance(static_cast<float>(step));
        }
    }

    void UpdateStageFilters()
    {
        if (mDspSampleRate <= 0.0)
        {
            return;
        }

        // Loose and dark at the vintage end, so low strings bloom and smear
        // into the next stage and the top is round; tight and open at the
        // modern end. Both scales are exactly 1.0 at the default.
        const double character = mCharacter;
        const double highPassScale = 0.5 + 1.2 * character - 0.4 * character * character;
        const double lowPassScale = 0.55 + 1.15 * character - 0.5 * character * character;
        for (int stage = 0; stage < kMaxStages; ++stage)
        {
            mStageFilters[stage].SetCorners(mDspSampleRate, kStageHighPass[stage] * highPassScale,
                                            kStageLowPass[stage] * lowPassScale);
        }
    }

    // Upper-mid voicing after the preamp. Whatever is fed to a clipper comes
    // out as more or less distortion rather than a different colour, so the
    // fuzz-to-modern brightness is set here, on the finished preamp signal:
    // wooly and set back at the vintage end, forward bite at the modern end.
    // Flat at the default Character.
    void UpdateVoicingFilter()
    {
        if (mSampleRate <= 0.0)
        {
            return;
        }

        const double gainDb = (static_cast<double>(mCharacter) - 0.5) * 8.0;
        ComputePeakingEQ(1800.0, 0.8, gainDb, mVoicingTarget.b0, mVoicingTarget.b1, mVoicingTarget.b2,
                         mVoicingTarget.a1, mVoicingTarget.a2);
    }

    /**
     * Gain and Preamp Stages have to change how hard the stages are pushed,
     * not how loud the amp is. Driving a cascade harder raises its small-signal
     * gain long before it saturates, so without this the top of the gain
     * control was 7 to 17 dB louder than the bottom and every comparison was
     * really a loudness comparison.
     *
     * This is the heard level of the amp at a nominal input (0.1 peak: a
     * single A3, an E2+B2 power chord and an E5, averaged), measured through a
     * generic cab band (2nd-order 90 Hz high pass, 4.5 kHz low pass) and the
     * BS.1770 K-weighting shelf, by [voice][stages - 1] at gain 0, 0.25, 0.5,
     * 0.75 and 1, averaged over Character, which moves it by well under 1 dB.
     * The makeup is the inverse, so the stages are driven exactly as hard as
     * before and only the final level is trimmed. It is static on purpose: a
     * level follower would flatten the playing dynamics the amp is meant to
     * keep.
     *
     * The reference is each voice at the default gain and stage count, so a
     * default preset is exactly as loud as it was, and Clean stays quieter than
     * Drive the way two channels at the same settings would. Re-measure this if
     * the voicing changes (BuiltinAmpEffectTests --measure-levels prints a
     * replacement); TestLevelTracksGain fails when it goes stale.
     */
    static constexpr float kDefaultGain = 0.45f;
    static constexpr int kDefaultStages = 2;
    static constexpr float kHeardLevelDb[2][kMaxStages][5] = {
        {{-19.73f, -17.43f, -15.66f, -14.23f, -13.04f},
         {-21.02f, -16.31f, -12.84f, -10.23f, -8.28f},
         {-20.04f, -14.03f, -9.84f, -7.04f, -5.26f},
         {-20.93f, -14.16f, -9.76f, -7.13f, -5.62f}},
        {{-11.89f, -8.81f, -5.36f, -2.50f, -1.24f},
         {-13.57f, -8.71f, -4.52f, -2.37f, -1.78f},
         {-12.84f, -7.45f, -3.52f, -1.87f, -1.44f},
         {-14.17f, -8.62f, -4.78f, -3.13f, -2.46f}}};

    static float HeardLevelDb(int voice, int stages, float gain)
    {
        const float position = std::clamp(gain, 0.0f, 1.0f) * 4.0f;
        const int index = std::min(static_cast<int>(position), 3);
        const float fraction = position - static_cast<float>(index);
        const auto& row = kHeardLevelDb[voice][stages - 1];
        return row[index] + fraction * (row[index + 1] - row[index]);
    }

    static float LevelMakeup(float gain, float voice, int stages)
    {
        constexpr float dbToLog2 = 0.166096405f; // 1 / (20 log10 2)
        return std::exp2(LevelMakeupDb(gain, voice, stages) * dbToLog2);
    }

    void UpdateSmoothing()
    {
        if (mSampleRate <= 0.0)
        {
            mControlSmoothCoef = 0.0f;
            mFilterSmoothCoef = 0.0;
            return;
        }

        // Stepped once per host sample (see Process), so host-rate constants.
        mControlSmoothCoef = static_cast<float>(std::exp(-1.0 / (0.01 * mSampleRate)));
        mFilterSmoothCoef = std::exp(-1.0 / (0.005 * mSampleRate));
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

        // Modern high gain tightens the low end before anything clips, the
        // same job a boost pedal in front does; vintage lets it all in. Exactly
        // 60 Hz at the default Character.
        const double character = mCharacter;
        const double scale = 0.75 + 0.25 * character + 0.5 * character * character;
        ComputeHighPass(60.0 * scale, 0.707, mPreHPTarget.b0, mPreHPTarget.b1, mPreHPTarget.b2, mPreHPTarget.a1,
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
        // What goes into the first clipper decides where the harmonics land. A
        // soft knee distorts at every level and would otherwise out-fizz the
        // hard one, so Character tilts the input too: darker into the vintage
        // stages, brighter into the modern ones. Zero at the default.
        const double characterTilt = (static_cast<double>(mCharacter) - 0.5) * 8.0;
        const double gainDb = brightBoost + emphasisBoost + characterTilt;

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
    float mGain = kDefaultGain;
    float mGainSmoothed = kDefaultGain;
    double mBass = 0.5;
    double mMiddle = 0.5;
    double mTreble = 0.5;
    double mContour = 0.2;
    double mPresence = 0.5;
    double mOutputDb = 0.0;
    float mOutputGainTarget = 1.0f;
    float mOutputGainSmoothed = 1.0f;
    int mStageCount = kDefaultStages;
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
    float mCharacter = 0.5f;
    float mCharacterSmoothed = 0.5f;
    bool mVoicingStale = true;
    bool mRightChannelStale = false;
    float mVoicedCharacter = 0.0f;
    float mVoicedPowerDrive = 0.0f;
    float mVoicedBias = 0.0f;
    float mVoicedGain = 0.0f;
    float mVoicedVoice = 0.0f;
    int mVoicedStages = 0;
    float mKneeSoft = 0.0f;
    float mKneeTanh = 1.0f;
    float mKneeHard = 0.0f;
    std::array<float, kClipCount> mClipBias = {};
    std::array<float, kClipCount> mClipOffset = {};
    std::array<float, kClipCount> mClipInvSlope = {1.0f, 1.0f, 1.0f, 1.0f, 1.0f};
    float mPowerGain = 1.0f;
    float mPowerBias = 0.0f;
    float mPowerOffset = 0.0f;
    float mPowerInvScale = 1.0f;
    float mLevelMakeup = 1.0f;
    float mDepth = 0.4f;
    float mResonance = 0.4f;
    float mDamping = 0.5f;
    float mSagAttackCoef = 0.0f;
    float mSagReleaseCoef = 0.0f;

    Coefficients mPreHPTarget, mPreEmphTarget, mLowTarget, mMidTarget, mContourTarget, mTrebleTarget;
    Coefficients mPresenceTarget, mDepthTarget, mResonanceTarget, mDampingTarget, mPostHPTarget, mVoicingTarget;

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
    double mVoicingB0 = 1.0, mVoicingB1 = 0.0, mVoicingB2 = 0.0, mVoicingA1 = 0.0, mVoicingA2 = 0.0;

    std::array<double, 2> mLowS1 = {}, mLowS2 = {};
    std::array<double, 2> mMidS1 = {}, mMidS2 = {};
    std::array<double, 2> mContourS1 = {}, mContourS2 = {};
    std::array<double, 2> mTrebleS1 = {}, mTrebleS2 = {};
    std::array<double, 2> mPresenceS1 = {}, mPresenceS2 = {};
    std::array<double, 2> mVoicingS1 = {}, mVoicingS2 = {};
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
                       {"character", "Character", 0.5, 0.0, 1.0, "amount", "Input"},
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

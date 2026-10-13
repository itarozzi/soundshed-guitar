#pragma once

#include "dsp/EffectGuids.h"
#include "dsp/EffectParamSpec.h"
#include "dsp/EffectProcessor.h"
#include "dsp/EffectRegistry.h"
#include "dsp/FiniteCheck.h"
#include "dsp/effects/NoiseReductionHiss.h"
#include "dsp/effects/NoiseReductionSupport.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <string>

namespace guitarfx
{
/**
 * Noise reduction for guitar and bass pickups: mains hum and buzz taken out by cancellation,
 * hiss by a band-split downward expander. No latency, and nothing like a gate: the noise is
 * reduced under the playing as well as between notes, and a decaying note is never chopped.
 *
 *   in -+- hum canceller (adaptive, per harmonic) -+- low band ----------------------+- out
 *       |        ^ learns only while quiet         +- mid band  x expander gain ------+
 *       |        ^ frequency-locked to the mains   +- high band x expander gain ------+
 *
 * - Hum. A pickup is a coil in the mains' magnetic field, so its noise is the mains frequency
 *   and its harmonics, and a single coil's buzz reaches well into the kilohertz. A bank of
 *   adaptive cancellers (noise_reduction::HarmonicBank) learns each harmonic's amplitude and
 *   phase while the strings are quiet and subtracts that estimate all the time; while a note
 *   sounds, learning stops and the estimate is held, so what is played is never learned away.
 *   A frequency-locked loop keeps the bank on the mains as it drifts. Auto works out whether
 *   the mains is 50 or 60 Hz from what it hears.
 * - Hiss. Above a pickup-dependent split the signal is two bands, each expanded downward when
 *   its level falls toward its own measured noise floor (noise_reduction::HissExpander): the top
 *   end closes as a note's brightness decays into hiss and opens again at the next pick. Below
 *   the split nothing is touched, so the body of every note is left alone.
 * - Noise floors are measured, not set (noise_reduction::FloorTracker), so Threshold is how far
 *   above the floor a band has to be to pass at full level, whatever the interface's gain.
 * - Listen: Removed Noise plays only what was taken away, to check that it is only noise.
 */
class NoiseReductionEffect : public EffectProcessor
{
  public:
    NoiseReductionEffect()
    {
        PrepareState();
    }

    void Prepare(double sampleRate, int maxBlockSize) override
    {
        if (!ValidatePrepare(sampleRate, maxBlockSize))
        {
            return;
        }

        mSampleRate = sampleRate;
        PrepareState();
    }

    void Reset() override
    {
        for (auto& channel : mChannels)
        {
            ResetChannel(channel);
        }

        mStarted = false;
    }

    /// Each channel has its own canceller, detectors and floors, so identical sides stay
    /// identical (EffectProcessor::CanWiden) and neither hears the other
    /// (EffectProcessor::KeepsChannelsSeparate).
    [[nodiscard]] bool CanWiden() const override
    {
        return false;
    }

    [[nodiscard]] bool KeepsChannelsSeparate() const override
    {
        return true;
    }

    [[nodiscard]] bool SupportsMonoProcessing() const override
    {
        return true;
    }

    /// A mono path turning stereo hands the right side everything the left has learned, so it
    /// starts cancelling at once rather than learning the hum again.
    void SetInputLayout(ChannelLayout layout) override
    {
        if (layout == ChannelLayout::Stereo && mLayout == ChannelLayout::Mono)
        {
            mChannels[1] = mChannels[0];
        }

        mLayout = layout;
    }

    void Process(float** inputs, float** outputs, int numSamples) override
    {
        if (!inputs || !outputs || numSamples <= 0)
        {
            return;
        }

        const float* inL = inputs[0] ? inputs[0] : inputs[1];
        const float* inR = inputs[1] ? inputs[1] : inL;

        if (!inL)
        {
            for (int ch = 0; ch < 2; ++ch)
            {
                if (outputs[ch])
                {
                    std::fill_n(outputs[ch], numSamples, 0.0f);
                }
            }

            return;
        }

        Render(inL, inR, outputs[0], outputs[1], numSamples);
    }

    void ProcessMono(float* input, float* output, int numSamples) override
    {
        if (!output || numSamples <= 0)
        {
            return;
        }

        if (!input)
        {
            std::fill_n(output, numSamples, 0.0f);
            return;
        }

        Render(input, nullptr, output, nullptr, numSamples);
    }

    void SetParam(const std::string& key, double value) override
    {
        if (!IsFinite(value))
        {
            return;
        }

        const std::size_t index = FindParamSpec(noise_reduction::kParams, key);

        if (index == noise_reduction::kParamCount)
        {
            return;
        }

        mValues[index] = NormaliseParamValue(noise_reduction::kParams[index], value);
    }

    void SetConfig(const std::string&, const std::string&) override
    {
    }

    [[nodiscard]] double GetParam(const std::string& key) const override
    {
        if (const std::size_t index = FindParamSpec(noise_reduction::kParams, key);
            index != noise_reduction::kParamCount)
        {
            return mValues[index];
        }

        // Read-only state of the left channel, for tests and any future display.
        const Channel& channel = mChannels[0];

        if (key == "mainsHz")
        {
            return channel.fundamentalHz;
        }

        if (key == "humLevelDb")
        {
            return 10.0 * std::log10(channel.hum.Power() + 1.0e-20);
        }

        if (key == "harmonics")
        {
            return static_cast<double>(channel.hum.ActiveCount());
        }

        if (key == "noiseFloorDb")
        {
            return 10.0 * std::log10(channel.residualFloor.Floor());
        }

        if (key == "midGainDb")
        {
            return 20.0 * std::log10(std::max(channel.hiss.MidGain(), 1.0e-6f));
        }

        if (key == "highGainDb")
        {
            return 20.0 * std::log10(std::max(channel.hiss.HighGain(), 1.0e-6f));
        }

        return 0.0;
    }

    [[nodiscard]] std::string GetType() const override
    {
        return "noise_reduction";
    }

    [[nodiscard]] std::string GetCategory() const override
    {
        return "dynamics";
    }

  private:
    using Mains = noise_reduction::Mains;

    struct Channel
    {
        noise_reduction::HarmonicBank<noise_reduction::kMaxHarmonics> hum;
        noise_reduction::HarmonicBank<noise_reduction::kProbeHarmonics> probe50;
        noise_reduction::HarmonicBank<noise_reduction::kProbeHarmonics> probe60;
        Mains mains = Mains::Hz50; ///< never Auto: what the canceller is running at
        double fundamentalHz = 50.0;
        float depth = 0.0f;

        float dc = 0.0f;       ///< the detector's DC estimate
        float detector = 0.0f; ///< mean square of what is left after cancelling
        float inputDc = 0.0f;
        float inputDetector = 0.0f; ///< mean square of the input itself
        noise_reduction::FloorTracker inputFloor;
        float invInputQuietLevel = 0.0f;
        noise_reduction::FloorTracker residualFloor;
        float invQuietLevel = 0.0f; ///< 1 / (kQuietRatio x floor), for this block

        noise_reduction::HissExpander hiss;
        int warmUp = 0; ///< samples before the floors take readings

        // This block's minima and how quiet it was.
        float blockMinResidual = 0.0f;
        float blockMinInput = 0.0f;
        double quietSum = 0.0;

        // The frequency-locked loop's interval, and Auto's evidence.
        double fllQuiet = 0.0;
        int fllSamples = 0;
        double probePower50 = 0.0;
        double probePower60 = 0.0;
        double probeWeight = 0.0;
        int decisionSamples = 0;
    };

    /// Learning stops below this much of its full rate.
    static constexpr float kMinLearn = 1.0e-3f;

    void PrepareState()
    {
        using namespace noise_reduction;

        mHumMu = static_cast<float>(2.0 / (kHumLearnMs * 0.001 * mSampleRate));
        mProbeMu = static_cast<float>(2.0 / (kProbeLearnMs * 0.001 * mSampleRate));
        mDetectorCoef = static_cast<float>(OnePole(kDetectorMs, mSampleRate));
        mDcCoef = static_cast<float>(OnePole(kDetectorDcMs, mSampleRate));
        mDepthCoef = static_cast<float>(OnePole(kDepthSmoothingMs, mSampleRate));
        mHissSettings.Prepare(mSampleRate);
        mFllInterval = std::max(1, static_cast<int>(kFllIntervalSeconds * mSampleRate));
        mDecisionInterval = std::max(1, static_cast<int>(kMainsDecisionSeconds * mSampleRate));
        mWarmUpSamples = static_cast<int>(kFloorWarmUpSeconds * mSampleRate);

        for (auto& channel : mChannels)
        {
            channel.residualFloor.Prepare(mSampleRate, kResidualFloorCeiling);
            channel.inputFloor.Prepare(mSampleRate, kResidualFloorCeiling);
            channel.hiss.Prepare(mSampleRate);
        }

        mAppliedPickup = -1;
        mAppliedRelease = -1.0;
        Reset();
    }

    void ResetChannel(Channel& channel)
    {
        channel.hum.ClearWeights();
        channel.probe50.ClearWeights();
        channel.probe60.ClearWeights();
        const auto mains = static_cast<Mains>(static_cast<int>(mValues[noise_reduction::kMains]));
        channel.mains = mains == Mains::Hz60 ? Mains::Hz60 : Mains::Hz50;
        channel.fundamentalHz = noise_reduction::NominalMainsHz(channel.mains);
        channel.depth = 0.0f;
        channel.dc = 0.0f;
        channel.detector = 0.0f;
        channel.inputDc = 0.0f;
        channel.inputDetector = 0.0f;
        channel.residualFloor.Reset();
        channel.inputFloor.Reset();
        channel.hiss.Reset();
        channel.fllQuiet = 0.0;
        channel.fllSamples = 0;
        channel.probePower50 = channel.probePower60 = channel.probeWeight = 0.0;
        channel.decisionSamples = 0;
        channel.warmUp = mWarmUpSamples;
        TuneChannel(channel);
        TuneProbes(channel);
    }

    /// The canceller's harmonic count is fixed by the pickup and the nominal mains, so drift
    /// cannot move a harmonic in and out of the bank.
    void TuneChannel(Channel& channel)
    {
        using namespace noise_reduction;

        const double nominal = NominalMainsHz(channel.mains);
        const double ceilingHz = VoicingFor(mValues[kPickup]).humCeilingHz;
        const auto count = std::min(kMaxHarmonics, static_cast<std::size_t>(ceilingHz / nominal + 1.0e-9));
        channel.hum.Tune(channel.fundamentalHz, mSampleRate, kHarmonicMultiples.data(), count,
                         kHarmonicCeilingFraction * mSampleRate);
    }

    void TuneProbes(Channel& channel)
    {
        using namespace noise_reduction;

        const double ceiling = kHarmonicCeilingFraction * mSampleRate;
        channel.probe50.Tune(50.0, mSampleRate, kProbeMultiples.data(), kProbeHarmonics, ceiling);
        channel.probe60.Tune(60.0, mSampleRate, kProbeMultiples.data(), kProbeHarmonics, ceiling);
    }

    void SwitchMains(Channel& channel, Mains mains)
    {
        channel.mains = mains;
        channel.fundamentalHz = noise_reduction::NominalMainsHz(mains);
        channel.hum.ClearWeights();
        channel.fllQuiet = 0.0;
        channel.fllSamples = 0;
        TuneChannel(channel);
    }

    /// Everything the parameters ask for, read once per block. SetParam lands between blocks,
    /// so nothing finer is lost.
    void ApplyParams()
    {
        using namespace noise_reduction;

        const int pickup = static_cast<int>(mValues[kPickup]);

        if (pickup != mAppliedPickup)
        {
            mAppliedPickup = pickup;
            mHissSettings.SetSplits(VoicingFor(mValues[kPickup]), mSampleRate);

            for (auto& channel : mChannels)
            {
                TuneChannel(channel);
            }
        }

        const auto mains = static_cast<Mains>(static_cast<int>(mValues[kMains]));
        const bool autoMains = mains == Mains::Auto;

        for (auto& channel : mChannels)
        {
            if (!autoMains && channel.mains != mains)
            {
                SwitchMains(channel, mains);
            }
        }

        // The probes stop while Auto is off, so their phases are stale when it comes back.
        if (autoMains && !mAutoMains)
        {
            for (auto& channel : mChannels)
            {
                channel.probe50.ClearWeights();
                channel.probe60.ClearWeights();
                channel.probePower50 = channel.probePower60 = channel.probeWeight = 0.0;
                channel.decisionSamples = 0;
            }
        }

        mAutoMains = autoMains;
        mDepthTarget = static_cast<float>(mValues[kHum]);
        mHissSettings.SetAmount(mValues[kHiss], mValues[kThreshold]);
        mListenRemoved = static_cast<Listen>(static_cast<int>(mValues[kListen])) == Listen::Removed;

        if (mValues[kRelease] != mAppliedRelease)
        {
            mAppliedRelease = mValues[kRelease];
            mHissSettings.SetRelease(mAppliedRelease, mSampleRate);
        }

        if (!mStarted)
        {
            for (auto& channel : mChannels)
            {
                channel.depth = mDepthTarget;
            }

            mStarted = true;
        }

        // The canceller runs while Hum is up or still fading out. Its oscillators stand still
        // while it is off, so it starts again from nothing rather than from a stale phase.
        const bool humActive = mDepthTarget > 0.0f || mChannels[0].depth > 1.0e-5f;

        if (humActive && !mHumActive)
        {
            for (auto& channel : mChannels)
            {
                channel.hum.ClearWeights();
                channel.probe50.ClearWeights();
                channel.probe60.ClearWeights();
            }
        }

        if (!humActive)
        {
            for (auto& channel : mChannels)
            {
                channel.depth = 0.0f;
            }
        }

        mHumActive = humActive;
    }

    void BeginBlock(Channel& channel)
    {
        channel.invQuietLevel =
            static_cast<float>(1.0 / (noise_reduction::kQuietRatio * channel.residualFloor.Floor()));
        channel.invInputQuietLevel =
            static_cast<float>(1.0 / (noise_reduction::kInputQuietRatio * channel.inputFloor.Floor()));
        channel.blockMinResidual = 1.0e30f;
        channel.blockMinInput = 1.0e30f;
        channel.quietSum = 0.0;
        channel.hiss.BeginBlock(mHissSettings);
    }

    [[nodiscard]] float RenderSample(Channel& channel, float input)
    {
        float estimate = 0.0f;
        float probe50 = 0.0f;
        float probe60 = 0.0f;

        if (mHumActive)
        {
            estimate = channel.hum.Step();

            if (mAutoMains)
            {
                probe50 = channel.probe50.Step();
                probe60 = channel.probe60.Step();
            }
        }

        // Quiet detection, on what is left once the hum is out and on the input as it came.
        const float residual = input - estimate;
        channel.dc += mDcCoef * (residual - channel.dc);
        const float centred = residual - channel.dc;
        channel.detector += mDetectorCoef * (centred * centred - channel.detector);
        channel.blockMinResidual = std::min(channel.blockMinResidual, channel.detector);
        channel.inputDc += mDcCoef * (input - channel.inputDc);
        const float inputCentred = input - channel.inputDc;
        channel.inputDetector += mDetectorCoef * (inputCentred * inputCentred - channel.inputDetector);
        channel.blockMinInput = std::min(channel.blockMinInput, channel.inputDetector);
        float cleaned = input;

        if (mHumActive)
        {
            // Quiet by either measure. The residual alone can lock learning out: let the mains drift
            // while learning is off and the residual rises above the floor it set when it was
            // cancelling well, so it reads as playing and learning never resumes. The input's level
            // does not depend on the canceller, so it cannot do that.
            const float ratio =
                std::min(channel.detector * channel.invQuietLevel, channel.inputDetector * channel.invInputQuietLevel);
            const float learn = 1.0f / (1.0f + ratio * ratio);
            channel.quietSum += learn;

            if (learn > kMinLearn)
            {
                channel.hum.Adapt(mHumMu * learn * residual);

                if (mAutoMains)
                {
                    channel.probe50.Adapt(mProbeMu * learn * (input - probe50));
                    channel.probe60.Adapt(mProbeMu * learn * (input - probe60));
                }
            }

            channel.depth += mDepthCoef * (mDepthTarget - channel.depth);
            cleaned = input - channel.depth * estimate;
        }

        const float output = channel.hiss.Process(mHissSettings, cleaned);
        return mListenRemoved ? input - output : output;
    }

    void EndBlock(Channel& channel, int numSamples)
    {
        using namespace noise_reduction;

        const bool takeReadings = channel.warmUp <= 0;
        channel.warmUp -= takeReadings ? 0 : numSamples;
        channel.hiss.EndBlock(numSamples, takeReadings);

        if (takeReadings)
        {
            channel.residualFloor.Push(channel.blockMinResidual, numSamples);
            channel.inputFloor.Push(channel.blockMinInput, numSamples);
        }

        if (mHumActive)
        {
            SteerFrequency(channel, numSamples);

            if (mAutoMains)
            {
                WeighMains(channel, numSamples);
            }

            channel.hum.Renormalise();
            channel.probe50.Renormalise();
            channel.probe60.Renormalise();
        }

        // A non-finite input poisons the canceller and the filters for good. Start the channel
        // again so it recovers on the next block instead of emitting NaN until a reload.
        if (!IsFinite(channel.detector) || !IsFinite(channel.inputDetector) || !channel.hiss.IsFinite() ||
            !channel.hum.IsFinite() || !channel.probe50.IsFinite() || !channel.probe60.IsFinite())
        {
            const float depth = channel.depth;
            ResetChannel(channel);
            channel.depth = depth;
        }
    }

    /// The frequency-locked loop: once an interval, if learning was on through it and there is
    /// hum to read, move the fundamental toward where the harmonics' phases say it is.
    void SteerFrequency(Channel& channel, int numSamples)
    {
        using namespace noise_reduction;

        channel.fllQuiet += channel.quietSum;
        channel.fllSamples += numSamples;

        if (channel.fllSamples < mFllInterval)
        {
            return;
        }

        const bool quiet = channel.fllQuiet >= kFllMinQuiet * channel.fllSamples;
        const double minWeight = kFllMinSnr * mHumMu * channel.residualFloor.Floor();
        double driftHz = 0.0;

        if (quiet && channel.hum.MeasureDrift(channel.fllSamples / mSampleRate, minWeight, driftHz))
        {
            const double nominal = NominalMainsHz(channel.mains);
            const double fundamental =
                std::clamp(channel.fundamentalHz + kFllGain * driftHz, nominal - kMaxDriftHz, nominal + kMaxDriftHz);

            if (fundamental != channel.fundamentalHz)
            {
                channel.fundamentalHz = fundamental;
                TuneChannel(channel);
            }
        }

        channel.hum.Snapshot();
        channel.fllQuiet = 0.0;
        channel.fllSamples = 0;
    }

    /// Auto: weighs the 50 Hz and 60 Hz probes by how quiet each block was, and every decision
    /// interval switches the canceller if the other mains is clearly the one there.
    void WeighMains(Channel& channel, int numSamples)
    {
        using namespace noise_reduction;

        const double weight = channel.quietSum;
        channel.probePower50 += weight * channel.probe50.Power();
        channel.probePower60 += weight * channel.probe60.Power();
        channel.probeWeight += weight;
        channel.decisionSamples += numSamples;

        if (channel.decisionSamples < mDecisionInterval)
        {
            return;
        }

        if (channel.probeWeight > 0.0)
        {
            const double power50 = channel.probePower50 / channel.probeWeight;
            const double power60 = channel.probePower60 / channel.probeWeight;

            if (channel.mains == Mains::Hz50 && power60 > kMainsSwitchRatio * power50 && power60 > kMainsMinPower)
            {
                SwitchMains(channel, Mains::Hz60);
            }
            else if (channel.mains == Mains::Hz60 && power50 > kMainsSwitchRatio * power60 && power50 > kMainsMinPower)
            {
                SwitchMains(channel, Mains::Hz50);
            }
        }

        // Halved rather than cleared, so the evidence spans about the last second.
        channel.probePower50 *= 0.5;
        channel.probePower60 *= 0.5;
        channel.probeWeight *= 0.5;
        channel.decisionSamples = 0;
    }

    /// `inR` null runs the left channel alone. Each sample is read on both sides before either
    /// is written, so the buffers may alias.
    void Render(const float* inL, const float* inR, float* outL, float* outR, int numSamples)
    {
        ApplyParams();
        const bool stereo = inR != nullptr;
        BeginBlock(mChannels[0]);

        if (stereo)
        {
            BeginBlock(mChannels[1]);
        }

        for (int i = 0; i < numSamples; ++i)
        {
            const float left = RenderSample(mChannels[0], inL[i]);
            const float right = stereo ? RenderSample(mChannels[1], inR[i]) : left;

            if (outL)
            {
                outL[i] = left;
            }

            if (stereo && outR)
            {
                outR[i] = right;
            }
        }

        EndBlock(mChannels[0], numSamples);

        if (stereo)
        {
            EndBlock(mChannels[1], numSamples);
        }
    }

    noise_reduction::ParamValues mValues = noise_reduction::kDefaultValues;
    double mSampleRate = 48000.0;
    std::array<Channel, 2> mChannels{};
    ChannelLayout mLayout = ChannelLayout::Stereo;

    noise_reduction::HissSettings mHissSettings;
    int mAppliedPickup = -1;
    double mAppliedRelease = -1.0;
    bool mStarted = false;
    bool mHumActive = false;
    bool mAutoMains = false;
    bool mListenRemoved = false;
    float mDepthTarget = 1.0f;

    float mHumMu = 0.0f;
    float mProbeMu = 0.0f;
    float mDetectorCoef = 1.0f;
    float mDcCoef = 1.0f;
    float mDepthCoef = 1.0f;
    int mFllInterval = 4800;
    int mDecisionInterval = 24000;
    int mWarmUpSamples = 12000;
};

inline void RegisterNoiseReductionEffect()
{
    EffectTypeInfo info;
    info.type = EffectGuids::kNoiseReduction;
    info.aliases = {"noise_reduction"};
    info.displayName = "Noise Reduction";
    info.category = "dynamics";
    info.description = "Takes mains hum, single-coil buzz and hiss out of a guitar or bass pickup without gating: "
                       "the hum is learned between notes and cancelled under them, and the top end closes only as a "
                       "note decays into hiss. No latency.";
    info.requiresResource = false;
    info.parameters = BuildParameterDefs(noise_reduction::kParams);
    info.presets = noise_reduction::FactoryPresets(info.parameters);

    EffectRegistry::Instance().Register(info.type, info, []() { return std::make_unique<NoiseReductionEffect>(); });
}
} // namespace guitarfx

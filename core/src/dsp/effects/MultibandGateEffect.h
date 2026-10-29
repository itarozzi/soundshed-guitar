#pragma once

#include "dsp/ChannelLayout.h"
#include "dsp/EffectGuids.h"
#include "dsp/EffectParamSpec.h"
#include "dsp/EffectProcessor.h"
#include "dsp/EffectRegistry.h"
#include "dsp/FiniteCheck.h"
#include "dsp/effects/FactoryPresetSupport.h"
#include "dsp/effects/MultibandGateSupport.h"
#include "dsp/effects/NoiseGateLearn.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <string>

namespace guitarfx
{
/**
 * Multi-band Gate: a noise gate in six frequency bands, each turned down only once its own
 * level falls below its own threshold.
 *
 *   in ─ six-band LR4 split ─┬─ band 1 ─ × gain 1 ─┐
 *                            ├─ …                   ├─ Σ ─ out
 *                            └─ band 6 ─ × gain 6 ─┘
 *          each band's mean square ─ expander ─┘
 *
 * A single-band gate shuts everything at once, so a decaying note is cut off at one moment,
 * hiss and body together. Split into bands, the hiss bands close as the note's top end fades
 * into them while its lower bands, still well above their noise, play on. And each band is a
 * soft downward expander rather than a switch: below its threshold a band's gain falls 4 dB
 * for every dB the level does, through a 6 dB knee, down to Range, so nothing chatters and
 * there is no hysteresis or hold to set.
 *
 * - The split is fourth-order Linkwitz-Riley crossovers with all-pass compensation
 *   (dsp/LinkwitzRileyCrossover.h): open, the bands add up to a flat all-pass of the input.
 * - One detector per band drives both channels, so a stereo image cannot pull apart, except
 *   in dual mono, where each side has its own.
 * - Learn listens to the six detectors with the strings muted and suggests a threshold per
 *   band (GateLearnService, through NoiseLearnHost). It only measures; the UI sets the
 *   thresholds as ordinary edits. Offset moves all six together and is left to the player.
 * - No latency.
 */
class MultibandGateEffect : public EffectProcessor, public NoiseLearnHost
{
  public:
    static constexpr int kBands = multiband_gate::kBands;

    MultibandGateEffect()
    {
        for (std::size_t index = 0; index < multiband_gate::kParamCount; ++index)
        {
            mValues[index].store(static_cast<float>(multiband_gate::kParams[index].defaultValue),
                                 std::memory_order_relaxed);
        }

        PrepareRate();
        Reset();
    }

    [[nodiscard]] bool KeepsChannelsSeparate() const override
    {
        return true;
    }

    void SetDualMono(bool dualMono) override
    {
        mDualMono.store(dualMono, std::memory_order_relaxed);
    }

    /// Identical sides in, identical sides out (EffectProcessor::CanWiden).
    [[nodiscard]] bool CanWiden() const override
    {
        return false;
    }

    [[nodiscard]] bool SupportsMonoProcessing() const override
    {
        return true;
    }

    /// A mono path turning stereo hands the right side the left's filters, levels and gains,
    /// so it carries on from where the left is instead of opening from rest.
    void SetInputLayout(ChannelLayout layout) override
    {
        if (layout == ChannelLayout::Stereo && mLayout == ChannelLayout::Mono)
        {
            mChannels[1] = mChannels[0];
        }

        mLayout = layout;
    }

    void Prepare(double sampleRate, int maxBlockSize) override
    {
        if (!ValidatePrepare(sampleRate, maxBlockSize))
        {
            return;
        }

        mSampleRate = sampleRate;
        mMaxBlockSize = maxBlockSize;
        PrepareRate();
        Reset();
    }

    void Reset() override
    {
        for (auto& channel : mChannels)
        {
            channel.crossover.Reset();
            channel.level.fill(0.0);
            // Open: a note arriving straight after a reset (a preset switch) is not faded in.
            // In silence the bands close at the Release rate.
            channel.gain.fill(1.0f);
            channel.target.fill(1.0f);
        }

        mControlCountdown = 0;
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
            for (int channel = 0; channel < 2; ++channel)
            {
                if (outputs[channel])
                {
                    std::fill_n(outputs[channel], numSamples, 0.0f);
                }
            }

            return;
        }

        Render<2>({inL, inR}, {outputs[0], outputs[1]}, numSamples);
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

        Render<1>({input, input}, {output, nullptr}, numSamples);
    }

    void SetParam(const std::string& key, double value) override
    {
        if (!IsFinite(value))
        {
            return;
        }

        const std::size_t index = FindParamSpec(multiband_gate::kParams, key);

        if (index == multiband_gate::kParamCount)
        {
            return;
        }

        mValues[index].store(static_cast<float>(NormaliseParamValue(multiband_gate::kParams[index], value)),
                             std::memory_order_relaxed);
    }

    void SetConfig(const std::string&, const std::string&) override
    {
    }

    [[nodiscard]] double GetParam(const std::string& key) const override
    {
        if (const std::size_t index = FindParamSpec(multiband_gate::kParams, key); index != multiband_gate::kParamCount)
        {
            return mValues[index].load(std::memory_order_relaxed);
        }

        return 0.0;
    }

    [[nodiscard]] std::string GetType() const override
    {
        return "dynamics_multiband_gate";
    }

    [[nodiscard]] std::string GetCategory() const override
    {
        return "dynamics";
    }

    /// The gain band `band` of `channel` is applied at now. For tests and metering.
    [[nodiscard]] float BandGain(int channel, int band) const
    {
        return mChannels[static_cast<std::size_t>(channel)].gain[static_cast<std::size_t>(band)];
    }

    [[nodiscard]] int NoiseLearnBandCount() const override
    {
        return kBands;
    }

    [[nodiscard]] GateNoiseLearner& NoiseLearner(int band) override
    {
        return mLearners[static_cast<std::size_t>(std::clamp(band, 0, kBands - 1))];
    }

    [[nodiscard]] double LearnedThresholdDb(int /*band*/, double noiseFloorDb) const override
    {
        return std::clamp(noiseFloorDb + multiband_gate::kLearnMarginDb, multiband_gate::kMinThresholdDb,
                          multiband_gate::kMaxThresholdDb);
    }

  private:
    struct Channel
    {
        multiband_gate::Crossover crossover;
        std::array<double, kBands> level{}; ///< each band's smoothed mean square
        std::array<float, kBands> gain{};   ///< applied, ramping toward target
        std::array<float, kBands> target{}; ///< from the gain curve, every kControlInterval

        [[nodiscard]] bool IsFinite() const
        {
            for (int band = 0; band < kBands; ++band)
            {
                if (!guitarfx::IsFinite(level[static_cast<std::size_t>(band)]) ||
                    !guitarfx::IsFinite(gain[static_cast<std::size_t>(band)]))
                {
                    return false;
                }
            }

            return crossover.IsFinite();
        }
    };

    /// The parameters as this block uses them.
    struct Settings
    {
        std::array<double, kBands> thresholdPower{};
        std::array<double, kBands> thresholdDb{};
        double rangeDb = -40.0;
        float attackCoef = 1.0f;
        float releaseCoef = 1.0f;
        bool linked = true;
    };

    void PrepareRate()
    {
        mDesign.Prepare(multiband_gate::kSplitHz, mSampleRate);

        for (std::size_t band = 0; band < kBands; ++band)
        {
            mDetectorCoef[band] = OnePole(multiband_gate::kDetectorMs[band]);
            mLearners[band].Prepare(mSampleRate);
        }
    }

    [[nodiscard]] double OnePole(double milliseconds) const
    {
        return 1.0 - std::exp(-1.0 / (std::max(milliseconds, 0.001) * 0.001 * mSampleRate));
    }

    [[nodiscard]] float Value(multiband_gate::Param param) const
    {
        return mValues[param].load(std::memory_order_relaxed);
    }

    [[nodiscard]] Settings LoadSettings() const
    {
        Settings settings;
        const double offset = Value(multiband_gate::kOffset);

        for (std::size_t band = 0; band < kBands; ++band)
        {
            const double db = Value(static_cast<multiband_gate::Param>(multiband_gate::kThreshold1 + band)) + offset;
            settings.thresholdDb[band] = db;
            settings.thresholdPower[band] = std::pow(10.0, db / 10.0);
        }

        settings.rangeDb = Value(multiband_gate::kRange);
        settings.attackCoef = static_cast<float>(OnePole(Value(multiband_gate::kAttack)));
        settings.releaseCoef = static_cast<float>(OnePole(Value(multiband_gate::kRelease)));
        settings.linked = !mDualMono.load(std::memory_order_relaxed);
        return settings;
    }

    /// One non-finite sample would poison the filters and levels for good. Checked once a block.
    void GuardState()
    {
        for (const auto& channel : mChannels)
        {
            if (!channel.IsFinite())
            {
                Reset();
                return;
            }
        }
    }

    /// The gain curve's targets for one channel's levels.
    static void UpdateTargets(const Settings& settings, const std::array<double, kBands>& level,
                              std::array<float, kBands>& target)
    {
        for (std::size_t band = 0; band < kBands; ++band)
        {
            if (level[band] >= settings.thresholdPower[band])
            {
                target[band] = 1.0f;
                continue;
            }

            const double levelDb = 10.0 * std::log10(std::max(level[band], 1.0e-30));
            const double gainDb = multiband_gate::GainDb(settings.thresholdDb[band] - levelDb, settings.rangeDb);
            target[band] = static_cast<float>(std::pow(10.0, gainDb / 20.0));
        }
    }

    template <int kChannels>
    void Render(std::array<const float*, 2> inputs, std::array<float*, 2> outputs, int numSamples)
    {
        GuardState();

        const Settings settings = LoadSettings();
        const bool linked = kChannels == 1 || settings.linked;
        bool learning = false;

        for (auto& learner : mLearners)
        {
            learning = learner.BeginBlock() || learning;
        }

        std::array<std::array<double, kBands>, kChannels> bands{};

        for (int i = 0; i < numSamples; ++i)
        {
            for (int ch = 0; ch < kChannels; ++ch)
            {
                Channel& channel = mChannels[static_cast<std::size_t>(ch)];
                auto& split = bands[static_cast<std::size_t>(ch)];
                channel.crossover.Split(mDesign, inputs[static_cast<std::size_t>(ch)][i], split);

                for (std::size_t band = 0; band < kBands; ++band)
                {
                    channel.level[band] += mDetectorCoef[band] * (split[band] * split[band] - channel.level[band]);
                }
            }

            if (--mControlCountdown <= 0)
            {
                mControlCountdown = multiband_gate::kControlInterval;

                if (linked)
                {
                    UpdateTargets(settings, LinkedLevel<kChannels>(), mChannels[0].target);

                    if constexpr (kChannels == 2)
                    {
                        mChannels[1].target = mChannels[0].target;
                    }
                }
                else
                {
                    for (int ch = 0; ch < kChannels; ++ch)
                    {
                        Channel& channel = mChannels[static_cast<std::size_t>(ch)];
                        UpdateTargets(settings, channel.level, channel.target);
                    }
                }
            }

            for (int ch = 0; ch < kChannels; ++ch)
            {
                Channel& channel = mChannels[static_cast<std::size_t>(ch)];
                const auto& split = bands[static_cast<std::size_t>(ch)];
                double sum = 0.0;

                for (std::size_t band = 0; band < kBands; ++band)
                {
                    float& gain = channel.gain[band];
                    const float target = channel.target[band];
                    gain += (target > gain ? settings.attackCoef : settings.releaseCoef) * (target - gain);
                    sum += gain * split[band];
                }

                if (float* out = outputs[static_cast<std::size_t>(ch)])
                {
                    out[i] = static_cast<float>(sum);
                }
            }

            if (learning)
            {
                const auto level = LinkedLevel<kChannels>();

                for (std::size_t band = 0; band < kBands; ++band)
                {
                    mLearners[band].Feed(static_cast<float>(std::sqrt(level[band])));
                }
            }
        }

        if (learning)
        {
            for (auto& learner : mLearners)
            {
                learner.EndBlock();
            }
        }
    }

    /// Each band's level as the linked detector reads it: the louder side's.
    template <int kChannels> [[nodiscard]] std::array<double, kBands> LinkedLevel() const
    {
        if constexpr (kChannels == 1)
        {
            return mChannels[0].level;
        }
        else
        {
            std::array<double, kBands> level{};

            for (std::size_t band = 0; band < kBands; ++band)
            {
                level[band] = std::max(mChannels[0].level[band], mChannels[1].level[band]);
            }

            return level;
        }
    }

    // Parameters, written from any thread and read on the audio thread.
    std::array<std::atomic<float>, multiband_gate::kParamCount> mValues;
    std::atomic<bool> mDualMono{false};

    // Derived from the sample rate by PrepareRate().
    multiband_gate::CrossoverDesign mDesign;
    std::array<double, kBands> mDetectorCoef{};

    // Audio thread only.
    std::array<Channel, 2> mChannels;
    int mControlCountdown = 0;
    ChannelLayout mLayout = ChannelLayout::Mono;
    std::array<GateNoiseLearner, kBands> mLearners;
};

inline void RegisterMultibandGateEffect()
{
    EffectTypeInfo info;
    info.type = EffectGuids::kMultibandGate;
    info.aliases = {"dynamics_multiband_gate"};
    info.displayName = "Multi-band Gate";
    info.category = "dynamics";
    info.description = "Six-band noise gate: each band is turned down only once it falls below its own threshold";
    info.requiresResource = false;
    info.parameters = BuildParameterDefs(multiband_gate::kParams);

    // The thresholds and Offset belong to the player's pickups and room, as the noise gate's
    // Threshold does, so no preset touches them.
    const factory_presets::Builder b(info.parameters, {"threshold1", "threshold2", "threshold3", "threshold4",
                                                       "threshold5", "threshold6", "offset"});
    info.presets = {
        b.Defaults("standard", "Standard"),
        // Just takes the edge off the hiss: a band never drops by more than 12 dB.
        b.Make("subtle", "Subtle", {{"attack", 1.0}, {"release", 150.0}, {"range", -12.0}}),
        b.Make("gentle", "Gentle", {{"attack", 2.0}, {"release", 250.0}, {"range", -20.0}}),
        b.Make("tight", "Tight", {{"attack", 0.5}, {"release", 50.0}, {"range", -60.0}}),
        // High-gain palm mutes: the bands clamp down between chugs.
        b.Make("staccato", "Staccato", {{"attack", 0.2}, {"release", 25.0}, {"range", -70.0}}),
        b.Make("slow-fade", "Slow Fade", {{"attack", 2.0}, {"release", 500.0}, {"range", -30.0}}),
    };

    EffectRegistry::Instance().Register(info.type, info, []() { return std::make_unique<MultibandGateEffect>(); });
}
} // namespace guitarfx

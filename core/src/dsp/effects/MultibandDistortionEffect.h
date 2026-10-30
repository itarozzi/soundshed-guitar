#pragma once

#include "dsp/ChannelLayout.h"
#include "dsp/EffectGuids.h"
#include "dsp/EffectParamSpec.h"
#include "dsp/EffectProcessor.h"
#include "dsp/EffectRegistry.h"
#include "dsp/FiniteCheck.h"
#include "dsp/HalfBandIir.h"
#include "dsp/LevelTargets.h"
#include "dsp/effects/DriveOutputLimiter.h"
#include "dsp/effects/FactoryPresetSupport.h"
#include "dsp/effects/MultibandDistortionSupport.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <span>
#include <string>

namespace guitarfx
{
/**
 * Multi-band Distortion: four frequency bands, each driven into a clipper of its own with its
 * own Drive, Character and Level.
 *
 *   in ─ up ×N ─ four-band LR4 split ─┬─ band 1 ─ clip₁ ─ mix ─ × level 1 ─┐
 *                                     ├─ …                                  ├─ Σ ─ down ×N ─ DC ─ out
 *                                     └─ band 4 ─ clip₄ ─ mix ─ × level 4 ─┘
 *
 * One clipper fed a whole chord intermodulates every note with every other, and the low
 * strings push it hardest. Split first and each band only intermodulates with itself: the
 * lows can stay tight while the mids scream, and a chord keeps its notes. The node panel draws
 * the bands over the live spectrum, before and after, with how hard each band is driven
 * (ReadBandMeters).
 *
 * - The input is oversampled once (minimum-phase half-bands, dsp/HalfBandIir.h), split,
 *   clipped and summed at the high rate, and brought down once, so every band and its clean
 *   copy share one phase. About 192 kHz: 4× at 44.1/48 kHz, 2× at 88.2/96, 1× above.
 * - The split is the Multi-band Compressor's: fourth-order Linkwitz-Riley crossovers with
 *   all-pass compensation (dsp/LinkwitzRileyCrossover.h), at the same three movable
 *   crossovers. With every band clean they add up to a flat all-pass.
 * - Each clipper bends kKneeAboveNominalDb over the nominal operating level, and is
 *   antialiased (drive::ClipCurve::Antialiased). Drive is level-matched (LevelMatch): a
 *   nominal note comes out as loud as it went in.
 * - A Character change crossfades the band between the two curves.
 * - Each channel runs on its own. No latency is reported, as for the drive pedals.
 */
class MultibandDistortionEffect : public EffectProcessor
{
  public:
    static constexpr int kBands = multiband_distortion::kBands;
    static constexpr int kSplits = multiband_distortion::kSplits;

    MultibandDistortionEffect() : mLevelMatch(multiband_distortion::SharedLevelMatch())
    {
        for (std::size_t index = 0; index < multiband_distortion::kParamCount; ++index)
        {
            mValues[index].store(static_cast<float>(multiband_distortion::kParams[index].defaultValue),
                                 std::memory_order_relaxed);
        }

        PrepareRate();
        Reset();
    }

    [[nodiscard]] bool KeepsChannelsSeparate() const override
    {
        return true;
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

    /// A mono path turning stereo hands the right side the left's filters and clippers, so it
    /// carries on from where the left is instead of starting from rest.
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
        PrepareRate();
        Reset();
    }

    void Reset() override
    {
        for (auto& channel : mChannels)
        {
            channel.Reset();
        }

        mControlCountdown = 0;
        // The first control step lands on every gain rather than gliding to it.
        mPrimed = false;

        for (std::size_t band = 0; band < kBands; ++band)
        {
            mFade[band] = 1.0;
            mPeak[band] = 0.0;
            mMeterDriveDb[band].store(0.0f, std::memory_order_relaxed);
            mMeterLevelDb[band].store(-120.0f, std::memory_order_relaxed);
        }
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

        const std::size_t index = FindParamSpec(multiband_distortion::kParams, key);

        if (index == multiband_distortion::kParamCount)
        {
            return;
        }

        mValues[index].store(static_cast<float>(NormaliseParamValue(multiband_distortion::kParams[index], value)),
                             std::memory_order_relaxed);
    }

    void SetConfig(const std::string&, const std::string&) override
    {
    }

    [[nodiscard]] double GetParam(const std::string& key) const override
    {
        if (const std::size_t index = FindParamSpec(multiband_distortion::kParams, key);
            index != multiband_distortion::kParamCount)
        {
            return mValues[index].load(std::memory_order_relaxed);
        }

        return 0.0;
    }

    [[nodiscard]] std::string GetType() const override
    {
        return "multiband_distortion";
    }

    [[nodiscard]] std::string GetCategory() const override
    {
        return "drive";
    }

    [[nodiscard]] int BandMeterCount() const override
    {
        return kBands;
    }

    /// Per band: the crossovers in use; as "gain reduction", how far its peaks were driven past
    /// the clipper's knee since the last read, in dB; its input's peak level; and the input
    /// level at which its clipper starts to bend.
    int ReadBandMeters(std::span<BandMeter> out) override
    {
        const int count = std::min(kBands, static_cast<int>(out.size()));

        for (int band = 0; band < count; ++band)
        {
            const auto index = static_cast<std::size_t>(band);
            BandMeter& meter = out[index];
            meter.lowHz = band > 0 ? mMeterSplitHz[index - 1].load(std::memory_order_relaxed) : 0.0;
            meter.highHz = band < kSplits ? mMeterSplitHz[index].load(std::memory_order_relaxed) : 0.0;
            meter.gainReductionDb = mMeterDriveDb[index].exchange(0.0f, std::memory_order_relaxed);
            meter.levelDb = mMeterLevelDb[index].load(std::memory_order_relaxed);
            meter.thresholdDb = mMeterKneeDb[index].load(std::memory_order_relaxed);
        }

        return count;
    }

    /// The crossovers in use, after ordering. For tests.
    [[nodiscard]] std::array<double, kSplits> SplitsInUse() const
    {
        return mSplitHz;
    }

    /// How many times the host rate the bands are clipped at. For tests.
    [[nodiscard]] int OversamplingFactor() const
    {
        return mChannels[0].oversampler.Factor();
    }

  private:
    /// Ramp slot 0 is a band's current Character, slot 1 the one it is fading from.
    static constexpr std::size_t kSlots = 2;

    struct Channel
    {
        halfband::Oversampler oversampler;
        multiband_distortion::Crossover crossover;
        std::array<std::array<drive::AntialiasMemory, kSlots>, kBands> clip{};
        double dcIn = 0.0;
        double dcOut = 0.0;

        void Reset()
        {
            oversampler.Reset();
            crossover.Reset();
            clip = {};
            dcIn = 0.0;
            dcOut = 0.0;
        }
    };

    /// A gain that moves linearly to its next target over one control interval.
    struct Ramp
    {
        double value = 0.0;
        double step = 0.0;

        void Toward(double target, bool snap)
        {
            if (snap)
            {
                value = target;
                step = 0.0;
            }
            else
            {
                step = (target - value) / multiband_distortion::kControlInterval;
            }
        }
    };

    /// One band's gains, shared by both channels.
    struct BandGains
    {
        Ramp in;                      ///< into the clipper: Drive over the knee
        std::array<Ramp, kSlots> wet; ///< out of it, per slot: level-matched, Level, Mix, Output
        Ramp dry;                     ///< the clean band: Level, 1 − Mix, Output

        void Advance()
        {
            in.value += in.step;
            wet[0].value += wet[0].step;
            wet[1].value += wet[1].step;
            dry.value += dry.step;
        }
    };

    /// The oversampled rate stays near 192 kHz.
    [[nodiscard]] static int FactorFor(double sampleRate) noexcept
    {
        return sampleRate < 60000.0 ? 4 : (sampleRate < 120000.0 ? 2 : 1);
    }

    void PrepareRate()
    {
        const int factor = FactorFor(mSampleRate);
        mOversampledRate = mSampleRate * factor;

        for (auto& channel : mChannels)
        {
            channel.oversampler.Prepare(factor);
        }

        mSplitHz = CurrentSplits();
        mDesign.Prepare(mSplitHz, mOversampledRate);
        PublishSplits();

        mSmoothingCoef = OnePole(multiband_distortion::kSmoothingMs, multiband_distortion::kControlInterval);
        mFadeStep = 1.0 / std::max(1.0, multiband_distortion::kCharacterFadeMs * 0.001 * mSampleRate);
        mDcCoef = 1.0 - 2.0 * drive::kPi * 2.0 / mSampleRate;

        for (std::size_t band = 0; band < kBands; ++band)
        {
            mCharacter[band] = CharacterParam(static_cast<int>(band));
            mPreviousCharacter[band] = mCharacter[band];
        }
    }

    /// A one-pole's coefficient for a time constant, stepped every `interval` samples.
    [[nodiscard]] double OnePole(double milliseconds, double interval) const
    {
        return 1.0 - std::exp(-interval / (std::max(milliseconds, 0.001) * 0.001 * mSampleRate));
    }

    [[nodiscard]] float Value(multiband_distortion::Param param) const
    {
        return mValues[param].load(std::memory_order_relaxed);
    }

    [[nodiscard]] int CharacterParam(int band) const
    {
        return std::clamp(static_cast<int>(std::lround(Value(multiband_distortion::BandParam(band, 1)))), 0,
                          multiband_distortion::kCharacterCount - 1);
    }

    [[nodiscard]] std::array<double, kSplits> CurrentSplits() const
    {
        return lr_crossover::OrderedSplits<kSplits>({Value(multiband_distortion::kCrossover1),
                                                     Value(multiband_distortion::kCrossover2),
                                                     Value(multiband_distortion::kCrossover3)},
                                                    0.45 * mSampleRate);
    }

    void PublishSplits()
    {
        for (std::size_t split = 0; split < kSplits; ++split)
        {
            mMeterSplitHz[split].store(mSplitHz[split], std::memory_order_relaxed);
        }
    }

    /// Redesigns any crossover that has moved since the last block.
    void UpdateSplits()
    {
        const auto splits = CurrentSplits();
        bool moved = false;

        for (std::size_t split = 0; split < kSplits; ++split)
        {
            if (splits[split] != mSplitHz[split])
            {
                mDesign.PrepareSplit(static_cast<int>(split), splits[split], mOversampledRate);
                moved = true;
            }
        }

        if (moved)
        {
            mSplitHz = splits;
            PublishSplits();
        }
    }

    /// Starts a band crossing to a new Character, once any crossing it is in has finished, so
    /// a knob swept through the choices never jumps.
    void UpdateCharacters()
    {
        for (std::size_t band = 0; band < kBands; ++band)
        {
            const int wanted = CharacterParam(static_cast<int>(band));

            if (wanted == mCharacter[band] || mFade[band] < 1.0)
            {
                continue;
            }

            mPreviousCharacter[band] = mCharacter[band];
            mCharacter[band] = wanted;
            mFade[band] = mPrimed ? 0.0 : 1.0;
            mGains[band].wet[1] = mGains[band].wet[0];

            for (auto& channel : mChannels)
            {
                channel.clip[band][1] = channel.clip[band][0];
                channel.clip[band][0] = {};
            }
        }
    }

    /// Drive, Level, Mix and Output one control step closer to the controls, and every band's
    /// gains set ramping to what they now make.
    void ControlStep()
    {
        using namespace multiband_distortion;
        const bool snap = !mPrimed;
        const double glide = snap ? 1.0 : mSmoothingCoef;
        const double knee = std::pow(10.0, (GetNominalOperatingLevelDbfs() + kKneeAboveNominalDb) / 20.0);

        mMix += glide * (Value(kMix) - mMix);
        mOutputDb += glide * (Value(kOutput) - mOutputDb);
        const double output = std::pow(10.0, mOutputDb / 20.0);

        for (std::size_t band = 0; band < kBands; ++band)
        {
            const int index = static_cast<int>(band);
            mDriveDb[band] += glide * (Value(BandParam(index, 0)) - mDriveDb[band]);
            mLevelDb[band] += glide * (Value(BandParam(index, 2)) - mLevelDb[band]);

            const double drive = std::pow(10.0, mDriveDb[band] / 20.0);
            const double level = output * std::pow(10.0, mLevelDb[band] / 20.0);
            // Out of the clipper, back to the signal's scale and level-matched.
            const double wet = level * mMix * knee / drive;
            BandGains& gains = mGains[band];
            gains.in.Toward(drive / knee, snap);
            gains.wet[0].Toward(wet * mLevelMatch.Gain(mCharacter[band], mDriveDb[band]), snap);
            gains.wet[1].Toward(wet * mLevelMatch.Gain(mPreviousCharacter[band], mDriveDb[band]), snap);
            gains.dry.Toward(level * (1.0 - mMix), snap);
        }

        mPrimed = true;
    }

    template <int kChannels>
    void Render(std::array<const float*, 2> inputs, std::array<float*, 2> outputs, int numSamples)
    {
        UpdateSplits();
        UpdateCharacters();

        const int factor = mChannels[0].oversampler.Factor();
        std::array<double, kBands> blockPeak{};
        std::array<bool, kChannels> corrupted{};

        for (int i = 0; i < numSamples; ++i)
        {
            if (--mControlCountdown <= 0)
            {
                mControlCountdown = multiband_distortion::kControlInterval;
                ControlStep();
            }

            bool fading = false;

            for (std::size_t band = 0; band < kBands; ++band)
            {
                mGains[band].Advance();

                if (mFade[band] < 1.0)
                {
                    mFade[band] = std::min(1.0, mFade[band] + mFadeStep);
                    fading = true;
                }
            }

            for (int ch = 0; ch < kChannels; ++ch)
            {
                Channel& channel = mChannels[static_cast<std::size_t>(ch)];
                const float raw = inputs[static_cast<std::size_t>(ch)][i];
                double lanes[halfband::Oversampler::kMaxFactor];
                channel.oversampler.Up(IsFinite(raw) ? static_cast<double>(raw) : 0.0, lanes);

                for (int lane = 0; lane < factor; ++lane)
                {
                    std::array<double, kBands> bands{};
                    channel.crossover.Split(mDesign, lanes[lane], bands);
                    double sum = 0.0;

                    for (std::size_t band = 0; band < kBands; ++band)
                    {
                        const double x = bands[band];
                        const BandGains& gains = mGains[band];
                        const double driven = x * gains.in.value;
                        double shaped =
                            multiband_distortion::kCurves[static_cast<std::size_t>(mCharacter[band])].Antialiased(
                                driven, channel.clip[band][0]) *
                            gains.wet[0].value;

                        if (fading && mFade[band] < 1.0)
                        {
                            const double old =
                                multiband_distortion::kCurves[static_cast<std::size_t>(mPreviousCharacter[band])]
                                    .Antialiased(driven, channel.clip[band][1]) *
                                gains.wet[1].value;
                            shaped = old + mFade[band] * (shaped - old);
                        }

                        sum += shaped + x * gains.dry.value;
                        blockPeak[band] = std::max(blockPeak[band], std::abs(x));
                    }

                    lanes[lane] = sum;
                }

                // The lopsided curves make DC; a 2 Hz blocker takes it out.
                const double down = channel.oversampler.Down(lanes);
                channel.dcOut = down - channel.dcIn + mDcCoef * channel.dcOut;
                channel.dcIn = down;
                double out = channel.dcOut;

                if (!IsFinite(out))
                {
                    out = 0.0;
                    corrupted[static_cast<std::size_t>(ch)] = true;
                }

                if (float* dest = outputs[static_cast<std::size_t>(ch)])
                {
                    dest[i] = drive_output_limiter::SoftClipNearCeiling(static_cast<float>(out));
                }
            }
        }

        // One non-finite value would poison the filters for good.
        for (int ch = 0; ch < kChannels; ++ch)
        {
            if (corrupted[static_cast<std::size_t>(ch)])
            {
                mChannels[static_cast<std::size_t>(ch)].Reset();
            }
        }

        PublishMeters(blockPeak, numSamples);
    }

    /// Each band's peak, falling back over kPeakReleaseMs; how far past the knee this block
    /// drove it, held until the panel reads it; and where its knee is, as an input level.
    void PublishMeters(const std::array<double, kBands>& blockPeak, int numSamples)
    {
        const double release = std::exp(-numSamples / (multiband_distortion::kPeakReleaseMs * 0.001 * mSampleRate));

        for (std::size_t band = 0; band < kBands; ++band)
        {
            if (!IsFinite(mPeak[band]))
            {
                mPeak[band] = 0.0;
            }

            mPeak[band] = std::max(blockPeak[band], mPeak[band] * release);
            const double in = std::max(mGains[band].in.value, 1.0e-12);
            const auto driveDb =
                static_cast<float>(std::max(0.0, 20.0 * std::log10(std::max(blockPeak[band] * in, 1.0e-12))));
            auto& held = mMeterDriveDb[band];
            float current = held.load(std::memory_order_relaxed);

            while (driveDb > current && !held.compare_exchange_weak(current, driveDb, std::memory_order_relaxed))
            {
            }

            mMeterLevelDb[band].store(static_cast<float>(20.0 * std::log10(std::max(mPeak[band], 1.0e-6))),
                                      std::memory_order_relaxed);
            mMeterKneeDb[band].store(static_cast<float>(-20.0 * std::log10(in)), std::memory_order_relaxed);
        }
    }

    // Parameters, written from any thread and read on the audio thread.
    std::array<std::atomic<float>, multiband_distortion::kParamCount> mValues;

    // Meters, written by the audio thread once a block and read by the message thread.
    std::array<std::atomic<double>, kSplits> mMeterSplitHz{};
    std::array<std::atomic<float>, kBands> mMeterDriveDb{};
    std::array<std::atomic<float>, kBands> mMeterLevelDb{};
    std::array<std::atomic<float>, kBands> mMeterKneeDb{};

    const multiband_distortion::LevelMatch& mLevelMatch;

    // Derived from the sample rate by PrepareRate().
    double mOversampledRate = 192000.0;
    double mSmoothingCoef = 1.0;
    double mFadeStep = 1.0;
    double mDcCoef = 0.9997;

    // Audio thread only.
    multiband_distortion::CrossoverDesign mDesign;
    std::array<double, kSplits> mSplitHz{};
    std::array<Channel, 2> mChannels;
    std::array<BandGains, kBands> mGains{};
    std::array<int, kBands> mCharacter{};
    std::array<int, kBands> mPreviousCharacter{};
    std::array<double, kBands> mFade{};
    std::array<double, kBands> mDriveDb{};
    std::array<double, kBands> mLevelDb{};
    std::array<double, kBands> mPeak{};
    double mMix = 1.0;
    double mOutputDb = 0.0;
    bool mPrimed = false;
    int mControlCountdown = 0;
    ChannelLayout mLayout = ChannelLayout::Mono;
};

inline void RegisterMultibandDistortionEffect()
{
    EffectTypeInfo info;
    info.type = EffectGuids::kMultibandDistortion;
    info.aliases = {"multiband_distortion"};
    info.displayName = "Multi-band Distortion";
    info.category = "drive";
    info.description = "Four-band distortion: each band has its own drive, clipping character and level";
    info.requiresResource = false;
    info.parameters = BuildParameterDefs(multiband_distortion::kParams);

    const factory_presets::Builder b(info.parameters, {});
    constexpr double kTape = 0.0;
    constexpr double kTube = 1.0;
    constexpr double kDiode = 2.0;
    constexpr double kHard = 3.0;
    constexpr double kFuzz = 4.0;
    info.presets = {
        b.Defaults("standard", "Standard"),
        // Palm mutes that stay tight: the low band nearly clean, the mids doing the work.
        b.Make("tight-crunch", "Tight Crunch",
               {{"crossover1", 160.0},
                {"drive1", 8.0},
                {"drive2", 30.0},
                {"drive3", 36.0},
                {"drive4", 22.0},
                {"character3", kDiode},
                {"level1", 1.5}}),
        // Every band driven alike, so a chord breaks up without its notes smearing together.
        b.Make("chord-clarity", "Chord Clarity",
               {{"drive1", 24.0},
                {"drive2", 24.0},
                {"drive3", 24.0},
                {"drive4", 20.0},
                {"character1", kTape},
                {"character4", kTape}}),
        // A thick, saturated bottom under a moderate top.
        b.Make("fat-lows", "Fat Lows",
               {{"crossover1", 250.0},
                {"drive1", 34.0},
                {"character1", kTape},
                {"level1", 2.0},
                {"drive2", 24.0},
                {"drive3", 28.0},
                {"drive4", 18.0},
                {"level4", -3.0}}),
        // Lead tone: the bite band driven hard and pushed forward.
        b.Make("mid-scream", "Mid Scream",
               {{"crossover2", 700.0},
                {"crossover3", 2500.0},
                {"drive1", 14.0},
                {"drive2", 36.0},
                {"drive3", 44.0},
                {"character3", kDiode},
                {"level3", 2.0},
                {"drive4", 20.0},
                {"level4", -4.0}}),
        // Plenty of gain with the fizz band kept nearly clean and turned down.
        b.Make("fizz-free-lead", "Fizz-free Lead",
               {{"crossover3", 3500.0},
                {"drive1", 20.0},
                {"drive2", 38.0},
                {"drive3", 42.0},
                {"drive4", 6.0},
                {"character4", kTape},
                {"level4", -6.0}}),
        // Hard grit under the clean signal, which keeps the attack and the dynamics.
        b.Make("parallel-grit", "Parallel Grit",
               {{"drive1", 22.0},
                {"drive2", 40.0},
                {"drive3", 46.0},
                {"drive4", 34.0},
                {"character2", kHard},
                {"character3", kHard},
                {"character4", kHard},
                {"mix", 0.4}}),
        // A gated, broken-speaker top over a solid body.
        b.Make("splatter", "Splatter",
               {{"drive1", 18.0},
                {"drive2", 32.0},
                {"drive3", 48.0},
                {"character3", kFuzz},
                {"drive4", 46.0},
                {"character4", kFuzz},
                {"level4", -3.0}}),
    };

    EffectRegistry::Instance().Register(info.type, info,
                                        []() { return std::make_unique<MultibandDistortionEffect>(); });
}
} // namespace guitarfx

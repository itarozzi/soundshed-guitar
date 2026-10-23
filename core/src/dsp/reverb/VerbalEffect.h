#pragma once

/**
 * Verbal: a multi-machine reverb. Twelve machines — Room, Hall, Plate, Spring, Swell, Bloom,
 * Cloud, Choir, Shimmer, Tape Echo, Nonlinear and Reflections — behind one set of common
 * controls (Decay as a calibrated RT60 in seconds, Pre-Delay to 1.5 s, Tone, Mod, Low End,
 * Mix) plus each machine's own, shown only under that machine.
 *
 * Behaviours worth knowing:
 *   - Mix adds reverb to an untouched dry: the dry stays at unity to nine tenths of the knob
 *     and fades out over the last tenth, so the top of the knob is 100% wet.
 *   - Hold sustains the tail. Infinite keeps adding what is played; Freeze closes the input,
 *     so the held tail sits under whatever comes next. Both engage and release on ramps.
 *   - Changing machine lets the old tail ring out on a second engine while the new one starts.
 *
 * See docs/fx-library.md, "Verbal".
 */

#include "dsp/EffectGuids.h"
#include "dsp/EffectProcessor.h"
#include "dsp/EffectRegistry.h"
#include "dsp/reverb/VerbalEngine.h"
#include "dsp/reverb/VerbalPresets.h"

#include <array>
#include <cmath>
#include <memory>
#include <string>

namespace guitarfx
{
class VerbalEffect : public EffectProcessor
{
  public:
    VerbalEffect()
    {
        for (std::size_t i = 0; i < verbal::kParamCount; ++i)
        {
            mValues[i] = verbal::kParams[i].defaultValue;
        }
    }

    void Prepare(double sampleRate, int maxBlockSize) override
    {
        if (!ValidatePrepare(sampleRate, maxBlockSize))
        {
            return;
        }

        mSampleRate = sampleRate;
        mMaxBlockSize = maxBlockSize;

        for (auto& engine : mEngines)
        {
            engine.Prepare(sampleRate);
        }

        mSwell.Prepare(sampleRate);
        mMix.SetTime(0.02, sampleRate);
        mOutput.SetTime(0.02, sampleRate);
        mHold.SetTime(0.03, sampleRate);
        mIntake.SetTime(0.02, sampleRate);
        mDryL.SetTime(0.03, sampleRate);
        mDryR.SetTime(0.03, sampleRate);
        mPrepared = true;
        ApplySettings(true);
        Reset();
    }

    void Reset() override
    {
        for (auto& engine : mEngines)
        {
            engine.Reset();
        }

        mSwell.Reset();
        mTailActive = false;
        SnapSmoothers();
    }

    void Process(float** inputs, float** outputs, int numSamples) override
    {
        if (!outputs || numSamples <= 0)
        {
            return;
        }

        numSamples = std::min(numSamples, mMaxBlockSize);

        if (!mEnabled || !mPrepared)
        {
            CopyStereoInputToOutput(inputs, outputs, numSamples);
            return;
        }

        if (mDirty)
        {
            mDirty = false;
            ApplySettings(false);
        }

        verbal::Engine& active = mEngines[mActive];
        verbal::Engine& tail = mEngines[1 - mActive];
        active.BeginBlock(numSamples);

        if (mTailActive)
        {
            tail.BeginBlock(numSamples);
        }

        const bool swell = Machine() == verbal::Machine::Swell;
        const bool swellDry = swell && mValues[verbal::kSwellMode] >= 0.5;
        float tailPeak = 0.0f;

        for (int i = 0; i < numSamples; ++i)
        {
            const float inL = reverb::ScrubNonFinite(inputs && inputs[0] ? inputs[0][i] : 0.0f);
            const float inR = reverb::ScrubNonFinite(inputs && inputs[1] ? inputs[1][i] : inL);
            const float mix = mMix.Next();
            const float hold = mHold.Next();
            const float intake = mIntake.Next();
            const float swellGain = swell ? mSwell.Process(0.5f * (inL + inR)) : 1.0f;
            // Swell Dry rocks a volume pedal ahead of everything; Swell Wet only behind the tank.
            const float feedGain = swellDry ? swellGain : 1.0f;
            const float wetSwell = swell && !swellDry ? swellGain : 1.0f;

            float wetL = 0.0f;
            float wetR = 0.0f;
            active.Process(inL * feedGain, inR * feedGain, intake, hold, wetL, wetR);

            if (mTailActive)
            {
                float tl = 0.0f;
                float tr = 0.0f;
                // The outgoing machine takes no more input, and keeps its own decay.
                tail.Process(0.0f, 0.0f, 0.0f, 0.0f, tl, tr);
                wetL += tl;
                wetR += tr;
                tailPeak = std::max(tailPeak, std::fabs(tl) + std::fabs(tr));
            }

            const float dryGain = DryGain(mix) * feedGain;
            const float wetGain = WetGain(mix) * wetSwell * mOutput.Next();
            const float dryL = mDryL.Next();
            const float dryR = mDryR.Next();

            if (outputs[0])
            {
                outputs[0][i] = inL * dryGain * dryL + wetL * wetGain;
            }

            if (outputs[1])
            {
                outputs[1][i] = inR * dryGain * dryR + wetR * wetGain;
            }
        }

        // The spilled-over tail retires once it is inaudible, and is then free for the next change.
        if (mTailActive && tailPeak < 1.0e-6f && tail.Energy() < 1.0e-6f)
        {
            mTailActive = false;
        }

        // One non-finite value inside a loop would circulate for ever; clear it instead.
        if (!active.Finite() || (mTailActive && !tail.Finite()))
        {
            Reset();
        }
    }

    void SetParam(const std::string& key, double value) override
    {
        const std::size_t index = verbal::FindParam(key);

        if (index == verbal::kParamCount || !IsFinite(value))
        {
            return;
        }

        const double normalised = NormaliseParamValue(verbal::kParams[index], value);

        if (normalised == mValues[index])
        {
            return;
        }

        mValues[index] = normalised;
        mDirty = true;
    }

    void SetConfig(const std::string&, const std::string&) override
    {
    }

    [[nodiscard]] double GetParam(const std::string& key) const override
    {
        const std::size_t index = verbal::FindParam(key);
        return index == verbal::kParamCount ? 0.0 : mValues[index];
    }

    [[nodiscard]] std::string GetType() const override
    {
        return "reverb_verbal";
    }

    [[nodiscard]] std::string GetCategory() const override
    {
        return "reverb";
    }

    /// The wet level the Mix law gives at `mix`: silent at 0, -24 dB at a tenth, unity from nine
    /// tenths. Between, it follows a curve through -15 dB at 0.3 and -10 dB at a half, so small
    /// settings are usable and the middle of the knob sits the reverb behind the dry.
    [[nodiscard]] static float WetGain(float mix) noexcept
    {
        static constexpr std::array<float, 6> kAt = {0.0f, 0.1f, 0.3f, 0.5f, 0.7f, 0.9f};
        static constexpr std::array<float, 6> kDb = {-60.0f, -24.0f, -15.0f, -10.0f, -4.7f, 0.0f};

        if (mix <= 0.0f)
        {
            return 0.0f;
        }

        if (mix >= 0.9f)
        {
            return 1.0f;
        }

        if (mix < 0.1f)
        {
            // Linear in gain from silence, so the very bottom of the knob fades out cleanly.
            return mix * 10.0f * std::pow(10.0f, kDb[1] / 20.0f);
        }

        std::size_t k = 1;

        while (k + 1 < kAt.size() && mix > kAt[k + 1])
        {
            ++k;
        }

        const float t = (mix - kAt[k]) / (kAt[k + 1] - kAt[k]);
        return std::pow(10.0f, (kDb[k] + t * (kDb[k + 1] - kDb[k])) / 20.0f);
    }

    /// The dry level at `mix`: unity to nine tenths, then out over the last tenth.
    [[nodiscard]] static float DryGain(float mix) noexcept
    {
        return mix <= 0.9f ? 1.0f : std::max(0.0f, (1.0f - mix) * 10.0f);
    }

  private:
    [[nodiscard]] verbal::Machine Machine() const noexcept
    {
        return static_cast<verbal::Machine>(static_cast<int>(mValues[verbal::kMachine]));
    }

    [[nodiscard]] verbal::Settings BuildSettings() const noexcept
    {
        const auto& v = mValues;
        const auto i = [&v](std::size_t p) { return static_cast<int>(std::lround(v[p])); };
        verbal::Settings s;
        s.machine = Machine();
        s.decay = v[verbal::kDecay];
        s.preDelayMs = v[verbal::kPreDelay];
        s.tone = v[verbal::kTone];
        s.mod = v[verbal::kMod];
        s.lowEnd = v[verbal::kLowEnd];
        s.roomSize = i(verbal::kRoomSize);
        s.hallSize = i(verbal::kHallSize);
        s.plateSize = i(verbal::kPlateSize);
        s.dwell = i(verbal::kDwell);
        s.springs = i(verbal::kSprings);
        s.midEq = v[verbal::kMidEq];
        s.bloomLength = v[verbal::kBloomLength];
        s.bloomFeedback = v[verbal::kBloomFeedback];
        s.diffusion = v[verbal::kDiffusion];
        s.vowel = i(verbal::kVowel);
        s.resonance = i(verbal::kResonance);
        s.shimmerMode = i(verbal::kShimmerMode);
        s.shift1 = i(verbal::kShift1);
        s.shift2 = i(verbal::kShift2);
        s.shimmerAmount = v[verbal::kShimmerAmount];
        s.echoTimeMs = v[verbal::kEchoTime];
        s.echoFeedback = v[verbal::kEchoFeedback];
        s.heads = i(verbal::kHeads);
        s.spacing = i(verbal::kSpacing);
        s.shape = i(verbal::kShape);
        s.shapeTimeMs = v[verbal::kShapeTime];
        s.shapeFeedback = v[verbal::kShapeFeedback];
        s.lateLevel = v[verbal::kLateLevel];
        s.modSpeed = v[verbal::kModSpeed];
        s.size = v[verbal::kSize];
        s.roomShape = i(verbal::kRoomShape);
        s.listenerX = v[verbal::kListenerX];
        s.listenerY = v[verbal::kListenerY];
        return s;
    }

    /// Hands every control to the engines. On the audio thread when automation drives it, so
    /// nothing here allocates; a machine change resets the incoming engine's buffers, which
    /// is the one costly step and only happens on a deliberate change of machine.
    void ApplySettings(bool initial)
    {
        if (!mPrepared)
        {
            return;
        }

        const verbal::Settings settings = BuildSettings();
        verbal::Engine& active = mEngines[mActive];

        if (!initial && settings.machine != active.CurrentSettings().machine && !mTailActive)
        {
            // Spill over: the old machine rings out where it is; the other engine takes the new.
            // While an earlier tail is still ringing the change is made in place instead, since
            // clearing that engine mid-tail would click.
            mActive = 1 - mActive;
            mTailActive = true;
            mEngines[mActive].Reset();
            mEngines[mActive].Configure(settings, true);
        }
        else
        {
            active.Configure(settings, initial);

            if (initial)
            {
                mEngines[1 - mActive].Configure(settings, true);
            }
        }

        mSwell.SetRiseSeconds(mValues[verbal::kRiseTime]);
        const bool hold = mValues[verbal::kHold] >= 0.5;
        const bool freeze = hold && mValues[verbal::kHoldMode] >= 0.5;
        mHold.SetTarget(hold ? 1.0f : 0.0f);
        mIntake.SetTarget(freeze ? 0.0f : 1.0f);
        mMix.SetTarget(static_cast<float>(mValues[verbal::kMix]));
        mOutput.SetTarget(static_cast<float>(std::pow(10.0, mValues[verbal::kOutputGain] / 20.0)));

        // Reflections places the player in the room: X pans the dry, and further back (Y) the
        // dry is quieter and more central against the reflections.
        float dryL = 1.0f;
        float dryR = 1.0f;

        if (settings.machine == verbal::Machine::Reflections)
        {
            const auto x = static_cast<float>(settings.listenerX);
            const auto y = static_cast<float>(settings.listenerY);
            const float panL = 1.0f - 0.6f * x;
            const float panR = 0.4f + 0.6f * x;
            const float back = 1.0f - 0.25f * y;
            dryL = back * (panL + (0.7f - panL) * 0.4f * y);
            dryR = back * (panR + (0.7f - panR) * 0.4f * y);
        }

        mDryL.SetTarget(dryL);
        mDryR.SetTarget(dryR);

        if (initial)
        {
            SnapSmoothers();
        }
    }

    void SnapSmoothers() noexcept
    {
        mMix.Snap(mMix.Target());
        mOutput.Snap(mOutput.Target());
        mHold.Snap(mHold.Target());
        mIntake.Snap(mIntake.Target());
        mDryL.Snap(mDryL.Target());
        mDryR.Snap(mDryR.Target());
    }

    std::array<double, verbal::kParamCount> mValues{};
    std::array<verbal::Engine, 2> mEngines;
    std::size_t mActive = 0;
    bool mTailActive = false;
    bool mDirty = true;
    bool mPrepared = false;
    reverb::SwellEnvelope mSwell;
    reverb::Smoothed mMix, mOutput, mHold, mIntake, mDryL, mDryR;
};

inline void RegisterVerbalEffect()
{
    EffectTypeInfo info;
    info.type = EffectGuids::kReverbVerbal;
    info.aliases = {"reverb_verbal"};
    info.displayName = "Verbal";
    info.category = "reverb";
    info.description = "Twelve reverb machines in one: rooms, halls, plates, springs, swells, blooms, clouds, a choir, "
                       "shimmer, tape echo, nonlinear shapes and small-room reflections, with Hold and Freeze.";
    info.requiresResource = false;
    info.parameters = BuildParameterDefs(verbal::kParams);

    auto& machine = info.parameters[verbal::kMachine];
    machine.control = "picker";

    for (std::size_t i = 0; i < verbal::kMachineCount; ++i)
    {
        machine.labelInfo.push_back({"", "", "", verbal::kMachineDetails[i]});
    }

    info.presets = verbal::FactoryPresets(info.parameters);

    EffectRegistry::Instance().Register(info.type, info, []() { return std::make_unique<VerbalEffect>(); });
}
} // namespace guitarfx

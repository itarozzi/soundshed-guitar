#pragma once

#include "dsp/EffectGuids.h"
#include "dsp/EffectParamSpec.h"
#include "dsp/EffectProcessor.h"
#include "dsp/EffectRegistry.h"
#include "dsp/FiniteCheck.h"
#include "dsp/NoteTracker.h"
#include "dsp/PickAttackDetector.h"
#include "dsp/PitchTracker.h"
#include "dsp/effects/SplitterSupport.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <string>
#include <vector>

namespace guitarfx
{
/**
 * The Splitter: the node a split's branches start from, and what each branch gets.
 *
 * In Copy mode every branch gets everything, which is all a split ever did, and the executor reads
 * the splitter's own buffer for each of them as it always has. In the other modes the executor
 * hands the splitter's input to Route() once a block, which writes one buffer per branch, and each
 * branch reads its own (SignalGraphExecutor, "A splitter's branches"). Branches are numbered in
 * the order the chain draws them, top to bottom.
 *
 * - Blend: Position walks from branch 1 to the last branch, crossfading each pair of neighbours,
 *   at equal power or linearly. An expression pedal's mode.
 * - Switch: one branch at a time, an A/B box. Every branch keeps running, so a delay or reverb on
 *   the branch left behind rings out.
 * - Dynamics: soft playing to branch 1, hard to branch 2, crossfaded across Width around
 *   Threshold. Pick judges each note by the peak of its pick and holds that until the next pick,
 *   so a hard note stays on branch 2 as it rings; Level follows the level as it goes, so a note
 *   moves back as it decays, and so does rolling the guitar's volume back. Branches after the
 *   second get everything.
 * - Pitch: notes by range, at the split notes (one fewer than the branches). NoteTracker decides
 *   once per note, so a bend or vibrato across a split note never moves it; a legato move to a
 *   new note does. A pick's pitch is known some 20-60 ms after it, and a chord has none to a mono
 *   tracker, so until then, and through a chord, the signal goes where Unpitched says: every
 *   branch at equal power, the branch the last note went to, or a fixed one.
 * - Stereo: the left channel to branch 1, the right to branch 2, each as mono; later branches get
 *   both. The executor gives those two branches a mono layout (SplitsChannels()).
 *
 * Every change of branch ramps over Crossfade, on each branch's power, so a switch between two
 * rigs neither dips nor bumps. A bypassed splitter copies, as a split did before it had modes.
 * Process() copies too: the executor never calls it, and it keeps the type's channel claims
 * honest for whatever does.
 *
 * Detection runs on the mid of the two sides, and only in the mode that needs it. Prepare()
 * allocates; nothing else does, and nothing locks.
 */
class SplitterEffect : public EffectProcessor
{
  public:
    SplitterEffect()
    {
        Prepare(mSampleRate, mMaxBlockSize);
    }

    void Prepare(double sampleRate, int maxBlockSize) override
    {
        if (!ValidatePrepare(sampleRate, maxBlockSize))
        {
            return;
        }

        mSampleRate = sampleRate;
        mMaxBlockSize = maxBlockSize;

        for (auto& side : mPorts)
        {
            side.left.assign(static_cast<std::size_t>(maxBlockSize), 0.0f);
            side.right.assign(static_cast<std::size_t>(maxBlockSize), 0.0f);
        }

        mNotes.Prepare(sampleRate);
        mAppliedLowestNote = -1;
        mPick.Prepare(sampleRate);
        mPickWindowSamples = std::max(1, static_cast<int>(std::lround(splitter::kPickWindowSeconds * sampleRate)));
        Reset();
    }

    void Reset() override
    {
        mPowers.fill(1.0f);
        mStarted = false;
        mRouted = false;
        mLastMode = splitter::Mode::Copy;
        ResetDetectors();
    }

    void Process(float** inputs, float** outputs, int numSamples) override
    {
        CopyStereoInputToOutput(inputs, outputs, numSamples);
    }

    [[nodiscard]] bool CanWiden() const override
    {
        return false;
    }

    [[nodiscard]] bool KeepsChannelsSeparate() const override
    {
        return true;
    }

    void SetParam(const std::string& key, double value) override
    {
        if (!IsFinite(value))
        {
            return;
        }

        const std::size_t index = FindParamSpec(splitter::kParams, key);

        if (index == splitter::kParamCount)
        {
            return;
        }

        mValues[index] = NormaliseParamValue(splitter::kParams[index], value);
    }

    void SetConfig(const std::string& /*key*/, const std::string& /*value*/) override
    {
    }

    [[nodiscard]] double GetParam(const std::string& key) const override
    {
        if (const std::size_t index = FindParamSpec(splitter::kParams, key); index != splitter::kParamCount)
        {
            return mValues[index];
        }

        return 0.0;
    }

    [[nodiscard]] std::string GetType() const override
    {
        return EffectGuids::kSplitter;
    }

    [[nodiscard]] std::string GetCategory() const override
    {
        return "utility";
    }

    // ── The executor's side ─────────────────────────────────────────────────────────

    /// Left to branch 1 and right to branch 2, each mono: those two branches' layout is mono.
    [[nodiscard]] bool SplitsChannels() const noexcept
    {
        return IsEnabled() && splitter::ModeOf(mValues) == splitter::Mode::Stereo;
    }

    /// Writes this block's signal for each of `branches` branches from the splitter's input, unless
    /// every branch would get the input unchanged (Copy, settled), when it writes nothing and the
    /// branches read the input itself. Returns whether it wrote, which RoutedThisBlock() then says.
    bool Route(const float* left, const float* right, int branches, int numSamples)
    {
        mRouted = false;
        const int count = std::clamp(branches, 1, splitter::kMaxBranches);
        numSamples = std::min(numSamples, static_cast<int>(mPorts[0].left.size()));

        if (!left || !right || numSamples <= 0)
        {
            return false;
        }

        const splitter::Mode mode = IsEnabled() ? splitter::ModeOf(mValues) : splitter::Mode::Copy;

        if (mode != mLastMode)
        {
            // A detector that has not been listening holds whatever it last heard.
            ResetDetectors();
            mLastMode = mode;
        }

        if (mode == splitter::Mode::Stereo)
        {
            RouteChannels(left, right, count, numSamples);
            mPowers.fill(1.0f);
            mStarted = true;
            mRouted = true;
            return true;
        }

        if (mode == splitter::Mode::Copy && Settled(count))
        {
            // Running at unity, so a change of mode from here ramps rather than snaps.
            mStarted = true;
            return false;
        }

        ApplyLowestNote();
        const double rampPerSample = 1.0 / std::max(1.0, mValues[splitter::kCrossfade] * 0.001 * mSampleRate);

        for (int start = 0; start < numSamples; start += splitter::kControlSamples)
        {
            const int n = std::min(splitter::kControlSamples, numSamples - start);
            const splitter::Gains targets = Targets(mode, left + start, right + start, n, count);

            if (!mStarted)
            {
                for (std::size_t branch = 0; branch < splitter::kMaxBranches; ++branch)
                {
                    mPowers[branch] = targets[branch] * targets[branch];
                }

                mStarted = true;
            }

            for (int branch = 0; branch < count; ++branch)
            {
                Ramp(static_cast<std::size_t>(branch), targets[static_cast<std::size_t>(branch)],
                     static_cast<float>(rampPerSample), left + start, right + start, start, n);
            }
        }

        mRouted = true;
        return true;
    }

    [[nodiscard]] bool RoutedThisBlock() const noexcept
    {
        return mRouted;
    }

    [[nodiscard]] const float* PortLeft(int branch) const noexcept
    {
        return mPorts[static_cast<std::size_t>(std::clamp(branch, 0, splitter::kMaxBranches - 1))].left.data();
    }

    [[nodiscard]] const float* PortRight(int branch) const noexcept
    {
        return mPorts[static_cast<std::size_t>(std::clamp(branch, 0, splitter::kMaxBranches - 1))].right.data();
    }

  private:
    struct Port
    {
        std::vector<float> left;
        std::vector<float> right;
    };

    void ResetDetectors()
    {
        mNotes.Reset();
        mWasPending = false;
        mPitchBranch = UnpitchedBranch(0);
        mPick.Reset();
        mNotePeak = 0.0f;
        mHeldHardness = 0.0;
        mHardness = 0.0;
        mPickWindowLeft = 0;
        mLevel = 0.0f;
    }

    /// Whether every branch has reached unity: a Copy that has finished ramping.
    [[nodiscard]] bool Settled(int count) const noexcept
    {
        for (int branch = 0; branch < count; ++branch)
        {
            if (mPowers[static_cast<std::size_t>(branch)] != 1.0f)
            {
                return false;
            }
        }

        return true;
    }

    void ApplyLowestNote()
    {
        const auto choice = static_cast<int>(std::lround(mValues[splitter::kLowestNote]));

        if (choice != mAppliedLowestNote)
        {
            mAppliedLowestNote = choice;
            mNotes.SetLowestFrequency(pitch_tracker::LowestNoteHz(choice));
        }
    }

    void RouteChannels(const float* left, const float* right, int count, int numSamples)
    {
        std::copy_n(left, numSamples, mPorts[0].left.data());
        std::copy_n(left, numSamples, mPorts[0].right.data());

        if (count > 1)
        {
            std::copy_n(right, numSamples, mPorts[1].left.data());
            std::copy_n(right, numSamples, mPorts[1].right.data());
        }

        for (int branch = 2; branch < count; ++branch)
        {
            std::copy_n(left, numSamples, mPorts[static_cast<std::size_t>(branch)].left.data());
            std::copy_n(right, numSamples, mPorts[static_cast<std::size_t>(branch)].right.data());
        }
    }

    [[nodiscard]] splitter::Gains Targets(splitter::Mode mode, const float* left, const float* right, int n, int count)
    {
        using splitter::Mode;

        switch (mode)
        {
        case Mode::Blend:
            return splitter::BlendGains(mValues[splitter::kPosition], count,
                                        static_cast<splitter::Curve>(std::lround(mValues[splitter::kCurve])));
        case Mode::Switch:
            return splitter::OneBranch(static_cast<int>(std::lround(mValues[splitter::kBranch])), count);
        case Mode::Dynamics:
            return splitter::DynamicsGains(DetectHardness(left, right, n), count);
        case Mode::Pitch:
            return PitchTargets(left, right, n, count);
        default:
            break;
        }

        splitter::Gains unity{};
        unity.fill(1.0f);
        return unity;
    }

    /// Moves branch `branch`'s power towards its target gain's, at most `step` a sample, and
    /// writes its signal. A switch between two branches then crossfades at equal power.
    void Ramp(std::size_t branch, float target, float step, const float* left, const float* right, int offset, int n)
    {
        float* outLeft = mPorts[branch].left.data() + offset;
        float* outRight = mPorts[branch].right.data() + offset;
        const float targetPower = target * target;
        float power = mPowers[branch];

        if (power == targetPower)
        {
            for (int i = 0; i < n; ++i)
            {
                outLeft[i] = left[i] * target;
                outRight[i] = right[i] * target;
            }

            return;
        }

        for (int i = 0; i < n; ++i)
        {
            power = power < targetPower ? std::min(targetPower, power + step) : std::max(targetPower, power - step);
            const float gain = std::sqrt(power);
            outLeft[i] = left[i] * gain;
            outRight[i] = right[i] * gain;
        }

        mPowers[branch] = power;
    }

    [[nodiscard]] static float Mid(const float* left, const float* right, int i) noexcept
    {
        const float mid = 0.5f * (left[i] + right[i]);
        return IsFinite(mid) ? mid : 0.0f;
    }

    [[nodiscard]] static double LevelDb(float level) noexcept
    {
        return 20.0 * std::log10(std::max(static_cast<double>(level), 1.0e-6));
    }

    /// Dynamics: how far into the hard branch this chunk is.
    [[nodiscard]] double DetectHardness(const float* left, const float* right, int n)
    {
        const double threshold = mValues[splitter::kThreshold];
        const double width = mValues[splitter::kWidth];

        if (static_cast<splitter::Detect>(std::lround(mValues[splitter::kDetect])) == splitter::Detect::Level)
        {
            const auto release =
                static_cast<float>(std::exp(-1.0 / (std::max(1.0, mValues[splitter::kRelease]) * 0.001 * mSampleRate)));

            for (int i = 0; i < n; ++i)
            {
                const float magnitude = std::abs(Mid(left, right, i));
                mLevel = magnitude > mLevel ? magnitude : mLevel * release;
            }

            return splitter::Hardness(LevelDb(mLevel), threshold, width);
        }

        // Pick: the peak since the last pick. For the first few milliseconds of a pick the note
        // before it keeps its branch, unless this one is already over the threshold, so a hard
        // pick goes hard at once and a soft one after a hard one does not dip on the way.
        for (int i = 0; i < n; ++i)
        {
            const float x = Mid(left, right, i);

            if (mPick.Process(x))
            {
                mHeldHardness = mHardness;
                mNotePeak = std::abs(x);
                mPickWindowLeft = mPickWindowSamples;
            }
            else
            {
                mNotePeak = std::max(mNotePeak, std::abs(x));
                mPickWindowLeft = std::max(0, mPickWindowLeft - 1);
            }
        }

        const double now = splitter::Hardness(LevelDb(mNotePeak), threshold, width);
        mHardness = mPickWindowLeft > 0 ? std::max(mHeldHardness, now) : now;
        return mHardness;
    }

    /// Where an unpitched attack goes: -1 for every branch, else a branch.
    [[nodiscard]] int UnpitchedBranch(int last) const noexcept
    {
        const auto rule = static_cast<int>(std::lround(mValues[splitter::kUnpitchedTo]));

        if (rule == static_cast<int>(splitter::Unpitched::All))
        {
            return -1;
        }

        if (rule == static_cast<int>(splitter::Unpitched::Last))
        {
            return last;
        }

        return rule - static_cast<int>(splitter::Unpitched::Branch1);
    }

    [[nodiscard]] splitter::Gains PitchTargets(const float* left, const float* right, int n, int count)
    {
        std::array<float, splitter::kControlSamples> mid{};

        for (int i = 0; i < n; ++i)
        {
            mid[static_cast<std::size_t>(i)] = Mid(left, right, i);
        }

        const splitter::SplitNotes notes = splitter::ActiveSplitNotes(mValues, count);

        mNotes.Process(mid.data(), n, [&](const NoteTracker::Event& event) {
            if (event.type == NoteTracker::Event::Type::Start || event.type == NoteTracker::Event::Type::Legato)
            {
                mPitchBranch = splitter::BranchForPitch(event.pitch, notes);
            }
        });

        const bool pending = mNotes.IsPending();

        if (pending && !mWasPending)
        {
            mPitchBranch = UnpitchedBranch(mPitchBranch < 0 ? 0 : mPitchBranch);
        }

        mWasPending = pending;
        return mPitchBranch < 0 ? splitter::AllBranches(count) : splitter::OneBranch(mPitchBranch, count);
    }

    splitter::ParamValues mValues = splitter::kDefaultValues;
    std::array<Port, splitter::kMaxBranches> mPorts;
    /// Each branch's gain, squared, as it ramps: 1 is the whole input.
    std::array<float, splitter::kMaxBranches> mPowers{};
    bool mStarted = false;
    bool mRouted = false;
    splitter::Mode mLastMode = splitter::Mode::Copy;

    NoteTracker mNotes;
    int mAppliedLowestNote = -1;
    bool mWasPending = false;
    int mPitchBranch = -1; ///< -1: every branch

    PickAttackDetector mPick;
    int mPickWindowSamples = 1;
    int mPickWindowLeft = 0;
    float mNotePeak = 0.0f;
    double mHeldHardness = 0.0;
    double mHardness = 0.0;
    float mLevel = 0.0f;
};

inline void RegisterSplitterEffect()
{
    EffectTypeInfo info;
    info.type = EffectGuids::kSplitter;
    info.aliases = {"splitter"};
    info.displayName = "Splitter";
    info.category = "utility";
    info.description = "Splits the signal into parallel branches: a copy to each, or routed by a blend, a switch, "
                       "how hard you pick, the notes you play, or left and right.";
    info.requiresResource = false;
    info.parameters = BuildParameterDefs(splitter::kParams);

    EffectRegistry::Instance().Register(info.type, info, []() { return std::make_unique<SplitterEffect>(); });
}
} // namespace guitarfx

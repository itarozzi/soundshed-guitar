#pragma once

#include "dsp/EffectParamSpec.h"
#include "dsp/PitchTracker.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <iterator>

/**
 * What the Splitter is, as opposed to how it runs (SplitterEffect.h): its modes, its parameter
 * table, and the arithmetic that turns a mode's decision into a gain for each branch.
 */
namespace guitarfx::splitter
{
/// One branch per mixer input (MixerEffect::kMaxInputs, SplitBranchSupport's kMaxSplitBranches).
inline constexpr int kMaxBranches = 4;

enum class Mode : int
{
    Copy,     ///< every branch gets everything, as a split always did
    Blend,    ///< Position crossfades across the branches
    Switch,   ///< one branch at a time
    Dynamics, ///< soft playing to branch 1, hard to branch 2
    Pitch,    ///< notes to branches by range
    Stereo,   ///< left to branch 1, right to branch 2
};

enum class Curve : int
{
    EqualPower,
    Linear,
};

enum class Detect : int
{
    Pick,  ///< how hard each note was picked, held until the next pick
    Level, ///< the level as it goes, so a decaying note moves back
};

/// Where an attack goes before its pitch is known, and a chord, which has none.
enum class Unpitched : int
{
    All,
    Last,
    Branch1,
};

inline constexpr const char* kModeLabels[] = {"Copy", "Blend", "Switch", "Dynamics", "Pitch", "Stereo"};
inline constexpr const char* kCurveLabels[] = {"Equal power", "Linear"};
inline constexpr const char* kBranchLabels[] = {"Branch 1", "Branch 2", "Branch 3", "Branch 4"};
inline constexpr const char* kDetectLabels[] = {"Pick", "Level"};
inline constexpr const char* kUnpitchedLabels[] = {"All branches", "Last branch", "Branch 1",
                                                   "Branch 2",     "Branch 3",    "Branch 4"};

/// A split note's choices, E1 to E6: from under a low-tuned eight-string's lowest note to past
/// the 24th fret of the high E. The value is the index; the note is kLowestSplitMidi above it.
inline constexpr int kLowestSplitMidi = 28;
inline constexpr const char* kNoteLabels[] = {
    "E1",  "F1",  "F#1", "G1",  "G#1", "A1",  "A#1", "B1",  "C2",  "C#2", "D2",  "D#2", "E2",  "F2",  "F#2", "G2",
    "G#2", "A2",  "A#2", "B2",  "C3",  "C#3", "D3",  "D#3", "E3",  "F3",  "F#3", "G3",  "G#3", "A3",  "A#3", "B3",
    "C4",  "C#4", "D4",  "D#4", "E4",  "F4",  "F#4", "G4",  "G#4", "A4",  "A#4", "B4",  "C5",  "C#5", "D5",  "D#5",
    "E5",  "F5",  "F#5", "G5",  "G#5", "A5",  "A#5", "B5",  "C6",  "C#6", "D6",  "D#6", "E6"};

inline constexpr int kBlendModes[] = {static_cast<int>(Mode::Blend)};
inline constexpr int kSwitchModes[] = {static_cast<int>(Mode::Switch)};
inline constexpr int kDynamicsModes[] = {static_cast<int>(Mode::Dynamics)};
inline constexpr int kPitchModes[] = {static_cast<int>(Mode::Pitch)};
inline constexpr int kCrossfadeModes[] = {static_cast<int>(Mode::Blend), static_cast<int>(Mode::Switch),
                                          static_cast<int>(Mode::Dynamics), static_cast<int>(Mode::Pitch)};

enum Param : std::size_t
{
    kMode,
    kPosition,
    kCurve,
    kBranch,
    kDetect,
    kThreshold,
    kWidth,
    kRelease,
    kSplitNote1,
    kSplitNote2,
    kSplitNote3,
    kLowestNote,
    kUnpitchedTo,
    kCrossfade,
    kParamCount
};

inline constexpr std::array<EffectParamSpec, kParamCount> kParams = {{
    {"mode", "Mode", 0.0, 0.0, 5.0, "enum", "Routing", false, 1.0, kModeLabels},
    {"position",
     "Position",
     0.0,
     0.0,
     1.0,
     "amount",
     "Routing",
     false,
     0.0,
     {},
     ParamTaper::Linear,
     "mode",
     kBlendModes},
    {"curve", "Curve", 0.0, 0.0, 1.0, "enum", "Routing", false, 1.0, kCurveLabels, ParamTaper::Linear, "mode",
     kBlendModes},
    {"branch", "Branch", 0.0, 0.0, 3.0, "enum", "Routing", false, 1.0, kBranchLabels, ParamTaper::Linear, "mode",
     kSwitchModes},
    {"detect", "Detect", 0.0, 0.0, 1.0, "enum", "Routing", false, 1.0, kDetectLabels, ParamTaper::Linear, "mode",
     kDynamicsModes},
    {"threshold",
     "Threshold",
     -18.0,
     -60.0,
     0.0,
     "dB",
     "Routing",
     false,
     0.0,
     {},
     ParamTaper::Linear,
     "mode",
     kDynamicsModes},
    {"width", "Width", 6.0, 0.0, 24.0, "dB", "Routing", false, 0.0, {}, ParamTaper::Linear, "mode", kDynamicsModes},
    LogTaper({"release",
              "Release",
              300.0,
              10.0,
              2000.0,
              "ms",
              "Routing",
              false,
              0.0,
              {},
              ParamTaper::Linear,
              "mode",
              kDynamicsModes}),
    {"splitNote1", "Split Note", 32.0, 0.0, 60.0, "enum", "Routing", false, 1.0, kNoteLabels, ParamTaper::Linear,
     "mode", kPitchModes},
    {"splitNote2", "Split Note 2", 44.0, 0.0, 60.0, "enum", "Routing", false, 1.0, kNoteLabels, ParamTaper::Linear,
     "mode", kPitchModes},
    {"splitNote3", "Split Note 3", 56.0, 0.0, 60.0, "enum", "Routing", false, 1.0, kNoteLabels, ParamTaper::Linear,
     "mode", kPitchModes},
    {"lowestNote", "Lowest Note", 0.0, 0.0, 3.0, "enum", "Routing", true, 1.0, pitch_tracker::kLowestNoteLabels,
     ParamTaper::Linear, "mode", kPitchModes},
    {"unpitched", "Unpitched", 0.0, 0.0, 5.0, "enum", "Routing", false, 1.0, kUnpitchedLabels, ParamTaper::Linear,
     "mode", kPitchModes},
    LogTaper({"crossfade",
              "Crossfade",
              10.0,
              1.0,
              200.0,
              "ms",
              "Routing",
              false,
              0.0,
              {},
              ParamTaper::Linear,
              "mode",
              kCrossfadeModes}),
}};

static_assert(kParams[kMode].maxValue == static_cast<double>(std::size(kModeLabels) - 1));
static_assert(kParams[kCurve].maxValue == static_cast<double>(std::size(kCurveLabels) - 1));
static_assert(kParams[kBranch].maxValue == static_cast<double>(std::size(kBranchLabels) - 1));
static_assert(kParams[kDetect].maxValue == static_cast<double>(std::size(kDetectLabels) - 1));
static_assert(kParams[kSplitNote1].maxValue == static_cast<double>(std::size(kNoteLabels) - 1));
static_assert(kParams[kLowestNote].maxValue == static_cast<double>(std::size(pitch_tracker::kLowestNoteLabels) - 1));
static_assert(kParams[kUnpitchedTo].maxValue == static_cast<double>(std::size(kUnpitchedLabels) - 1));

/// Every parameter's value, indexed by `Param`, always normalised (NormaliseParamValue).
using ParamValues = std::array<double, kParamCount>;
inline constexpr ParamValues kDefaultValues = DefaultParamValues(kParams);

/// One gain per branch.
using Gains = std::array<float, kMaxBranches>;

/// Detection, decisions and gain targets run once per this many samples; the gains ramp every
/// sample. A third of a millisecond at 48 kHz.
inline constexpr int kControlSamples = 16;

/// A pick's strength is the peak of its first few milliseconds. Until then the branch the note
/// before it was on keeps it, unless the new pick is already over the threshold.
inline constexpr double kPickWindowSeconds = 0.005;

/// A gain within this of its target is put on it, so a settled split runs on fixed gains.
inline constexpr float kSettledGain = 1.0e-4f;

/// The share of the signal each branch gets, as a gain. Equal power keeps the sum's loudness
/// steady when the branches sound different (two rigs), linear when they sound alike.
[[nodiscard]] inline float ShareToGain(double share, Curve curve) noexcept
{
    const double clamped = std::clamp(share, 0.0, 1.0);
    return static_cast<float>(curve == Curve::Linear ? clamped : std::sqrt(clamped));
}

/// Blend's targets: Position walks from branch 1 (0) to the last branch (1), crossfading between
/// each pair of neighbours on the way.
[[nodiscard]] inline Gains BlendGains(double position, int branches, Curve curve) noexcept
{
    Gains gains{};
    const int count = std::clamp(branches, 1, kMaxBranches);
    const double x = std::clamp(position, 0.0, 1.0) * static_cast<double>(count - 1);
    const int lower = std::min(static_cast<int>(std::floor(x)), count - 1);
    const int upper = std::min(lower + 1, count - 1);
    const double fraction = x - static_cast<double>(lower);

    if (upper == lower)
    {
        gains[static_cast<std::size_t>(lower)] = 1.0f;
        return gains;
    }

    gains[static_cast<std::size_t>(lower)] = ShareToGain(1.0 - fraction, curve);
    gains[static_cast<std::size_t>(upper)] = ShareToGain(fraction, curve);
    return gains;
}

/// One branch at full level, the rest silent; a branch past the split's count is its last.
[[nodiscard]] inline Gains OneBranch(int branch, int branches) noexcept
{
    Gains gains{};
    const int count = std::clamp(branches, 1, kMaxBranches);
    gains[static_cast<std::size_t>(std::clamp(branch, 0, count - 1))] = 1.0f;
    return gains;
}

/// Every branch an equal share, at equal power: two rigs at -3 dB each add up to one at 0 dB.
[[nodiscard]] inline Gains AllBranches(int branches) noexcept
{
    Gains gains{};
    const int count = std::clamp(branches, 1, kMaxBranches);
    const float share = ShareToGain(1.0 / static_cast<double>(count), Curve::EqualPower);

    for (int branch = 0; branch < count; ++branch)
    {
        gains[static_cast<std::size_t>(branch)] = share;
    }

    return gains;
}

/// How far into the hard branch a level of `levelDb` is: 0 under the threshold's window, 1 over
/// it, and a straight line in dB across a Width centred on the threshold.
[[nodiscard]] inline double Hardness(double levelDb, double thresholdDb, double widthDb) noexcept
{
    if (widthDb <= 0.01)
    {
        return levelDb >= thresholdDb ? 1.0 : 0.0;
    }

    return std::clamp((levelDb - thresholdDb) / widthDb + 0.5, 0.0, 1.0);
}

/// Dynamics' targets: branch 1 soft, branch 2 hard, crossfaded by `hardness`. Branches after the
/// second get everything, as they would from a plain split.
[[nodiscard]] inline Gains DynamicsGains(double hardness, int branches) noexcept
{
    Gains gains{};
    const int count = std::clamp(branches, 1, kMaxBranches);
    const double hard = std::clamp(hardness, 0.0, 1.0);
    gains[0] = ShareToGain(1.0 - hard, Curve::EqualPower);

    if (count > 1)
    {
        gains[1] = ShareToGain(hard, Curve::EqualPower);
    }

    for (int branch = 2; branch < count; ++branch)
    {
        gains[static_cast<std::size_t>(branch)] = 1.0f;
    }

    return gains;
}

/// The split notes in use, lowest first: one fewer than the branches. Each is the lowest note of
/// the branch above it.
struct SplitNotes
{
    std::array<int, kMaxBranches - 1> midi{};
    int count = 0;
};

[[nodiscard]] inline SplitNotes ActiveSplitNotes(const ParamValues& values, int branches) noexcept
{
    SplitNotes notes;
    notes.count = std::clamp(branches, 1, kMaxBranches) - 1;
    const Param keys[] = {kSplitNote1, kSplitNote2, kSplitNote3};

    for (int i = 0; i < notes.count; ++i)
    {
        notes.midi[static_cast<std::size_t>(i)] =
            kLowestSplitMidi + static_cast<int>(std::lround(values[keys[static_cast<std::size_t>(i)]]));
    }

    std::sort(notes.midi.begin(), notes.midi.begin() + notes.count);
    return notes;
}

/// The branch a note belongs to: how many split notes it is at or above. `pitch` is a fractional
/// MIDI note number; a quarter-tone flat still counts as the note.
[[nodiscard]] inline int BranchForPitch(double pitch, const SplitNotes& notes) noexcept
{
    const auto note = static_cast<int>(std::lround(pitch));
    int branch = 0;

    for (int i = 0; i < notes.count; ++i)
    {
        if (note >= notes.midi[static_cast<std::size_t>(i)])
        {
            branch = i + 1;
        }
    }

    return branch;
}

[[nodiscard]] inline Mode ModeOf(const ParamValues& values) noexcept
{
    return static_cast<Mode>(std::clamp(static_cast<int>(std::lround(values[kMode])), 0, 5));
}
} // namespace guitarfx::splitter

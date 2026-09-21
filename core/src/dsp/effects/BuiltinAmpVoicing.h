#pragma once

#include "dsp/EffectParamSpec.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <string_view>

/**
 * What the Heavy American is, as opposed to how it runs (BuiltinAmpEffect.h): its parameter
 * table, where the preamp stages sit, the clipper knees Character morphs between, the drive law,
 * and the level table that keeps Gain from being a volume control.
 */
namespace guitarfx::builtin_amp
{
inline constexpr double kPi = 3.14159265358979323846;
inline constexpr int kMaxStages = 4;

// ---------------------------------------------------------------------------------------
// Parameters
// ---------------------------------------------------------------------------------------

enum Param : int
{
    kVoice,
    kGain,
    kCharacter,
    kBright,
    kPreEmphasis,
    kStageCount,
    kStageGain,
    kBass,
    kMiddle,
    kTreble,
    kContour,
    kPresence,
    kOutput,
    kPowerDrive,
    kSag,
    kBias,
    kDepth,
    kResonance,
    kDamping,
    kParamCount
};

/// In `Param` order, which is also the order the UI lays the controls out in.
inline constexpr std::array<EffectParamSpec, kParamCount> kParams = {{
    {"voice", "Voice", 0.0, 0.0, 1.0, "toggle", "Input", false, 0.0, {}},
    {"gain", "Gain", 0.45, 0.0, 1.0, "amount", "Input", false, 0.0, {}},
    {"character", "Character", 0.5, 0.0, 1.0, "amount", "Input", false, 0.0, {}},
    {"bright", "Bright", 0.0, 0.0, 1.0, "toggle", "Input", false, 0.0, {}},
    {"preEmphasis", "Pre Emphasis", 0.0, 0.0, 1.0, "amount", "Input", true, 0.0, {}},
    {"stageCount", "Preamp Stages", 2.0, 1.0, 4.0, "amount", "Input", false, 1.0, {}},
    {"stageGain", "Input Trim", 0.0, -24.0, 24.0, "dB", "Input", false, 0.0, {}},
    {"bass", "Bass", 0.5, 0.0, 1.0, "amount", "Tone", false, 0.0, {}},
    {"middle", "Middle", 0.5, 0.0, 1.0, "amount", "Tone", false, 0.0, {}},
    {"treble", "Treble", 0.5, 0.0, 1.0, "amount", "Tone", false, 0.0, {}},
    {"contour", "Contour", 0.2, 0.0, 1.0, "amount", "Tone", false, 0.0, {}},
    {"presence", "Presence", 0.5, 0.0, 1.0, "amount", "Tone", false, 0.0, {}},
    {"output", "Output", 0.0, -24.0, 24.0, "dB", "Output", false, 0.0, {}},
    {"powerDrive", "Power Drive", 0.0, 0.0, 1.0, "amount", "Power", true, 0.0, {}},
    {"sag", "Sag", 0.0, 0.0, 1.0, "amount", "Power", true, 0.0, {}},
    {"bias", "Bias", 0.0, -1.0, 1.0, "amount", "Power", true, 0.0, {}},
    {"depth", "Depth", 0.4, 0.0, 1.0, "amount", "Power", true, 0.0, {}},
    {"resonance", "Resonance", 0.4, 0.0, 1.0, "amount", "Power", true, 0.0, {}},
    {"damping", "Damping", 0.5, 0.0, 1.0, "amount", "Power", true, 0.0, {}},
}};

/// Presets saved when each preamp stage had a gain of its own still carry these. There is one
/// Input Trim now, and any of them sets it.
inline constexpr const char* kLegacyStageGainKeys[] = {"stage1Gain", "stage2Gain", "stage3Gain",
                                                       "stage4Gain", "stage5Gain", "stage6Gain"};

/// The index of `key` in kParams, or kParamCount when it is not one of them.
[[nodiscard]] inline std::size_t FindParam(std::string_view key) noexcept
{
    const std::size_t index = FindParamSpec(kParams, key);

    if (index != kParamCount)
    {
        return index;
    }

    for (const char* legacy : kLegacyStageGainKeys)
    {
        if (key == legacy)
        {
            return kStageGain;
        }
    }

    return kParamCount;
}

// ---------------------------------------------------------------------------------------
// The preamp stages
// ---------------------------------------------------------------------------------------

// Interstage coupling at the default Character. The corners tighten
// monotonically down the chain so the most saturated stages see the least
// low end, which is what keeps a palm-muted low string defined instead of
// intermodulating. They have to stay this low, and stay in order: these are
// one-poles in series, so a corner up near 180 Hz partway down the chain
// costs a low E most of its fundamental before the tone stack ever sees it,
// and a stage that is looser than the one before it takes harmonics back
// out instead of adding them. Character scales the whole set together, so
// the order holds at every setting.
inline constexpr double kStageHighPass[kMaxStages] = {38.0, 70.0, 100.0, 120.0};
inline constexpr double kStageLowPass[kMaxStages] = {12000.0, 9000.0, 7000.0, 6000.0};

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

inline constexpr float kClipBias[kClipCount] = {0.07f, 0.15f, -0.10f, 0.12f, -0.06f};

/**
 * Drive law for one gain stage. A linear law spends the whole sweep getting
 * to crunch and never reaches a saturated high-gain cascade, because each
 * tanh bounds its own output and the next stage only sees a couple of times
 * that. The quartic term is the top of a log-taper pot: it barely moves
 * below about 0.6 and then opens the stage up by the ~15 dB that an
 * American high-gain preamp needs. Scaling it by the voice blend keeps the
 * clean channel exactly where it was.
 */
[[nodiscard]] inline float StageDrive(float gain, float voice, float base, float linear, float top)
{
    const float squared = gain * gain;
    return base + linear * gain + top * voice * squared * squared;
}

// ---------------------------------------------------------------------------------------
// Clipper knees
// ---------------------------------------------------------------------------------------

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
inline constexpr float kSoftRound = 0.3f;
inline constexpr float kHardKnee = 1.875f;
inline constexpr float kHardCubic = 2.0f / (3.0f * kHardKnee * kHardKnee);
inline constexpr float kHardQuintic = 0.2f / (kHardKnee * kHardKnee * kHardKnee * kHardKnee);

[[nodiscard]] inline float SoftKneeShape(float x)
{
    const float r = std::sqrt(x * x + kSoftRound * kSoftRound);
    return x / (1.0f - kSoftRound + r);
}

[[nodiscard]] inline float SoftKneeSlope(float x)
{
    const float r = std::sqrt(x * x + kSoftRound * kSoftRound);
    const float d = 1.0f - kSoftRound + r;
    return (d - x * x / r) / (d * d);
}

[[nodiscard]] inline float HardKneeShape(float x)
{
    const float c = std::clamp(x, -kHardKnee, kHardKnee);
    const float c2 = c * c;
    return c * (1.0f - c2 * (kHardCubic - kHardQuintic * c2));
}

[[nodiscard]] inline float HardKneeSlope(float x)
{
    if (std::abs(x) >= kHardKnee)
    {
        return 0.0f;
    }

    const float x2 = x * x;
    return 1.0f - x2 * (3.0f * kHardCubic - 5.0f * kHardQuintic * x2);
}

/// The knee Character has picked: a blend of at most two of the three shapes.
struct KneeBlend
{
    float softWeight = 0.0f;
    float tanhWeight = 1.0f;
    float hardWeight = 0.0f;

    /// Soft to tanh over the lower half of Character, tanh to hard over the upper.
    void SetCharacter(float character)
    {
        if (character <= 0.5f)
        {
            softWeight = 1.0f - 2.0f * character;
            tanhWeight = 2.0f * character;
            hardWeight = 0.0f;
        }
        else
        {
            softWeight = 0.0f;
            tanhWeight = 2.0f - 2.0f * character;
            hardWeight = 2.0f * character - 1.0f;
        }
    }

    // At most two knees are ever live, and the weights only change with
    // Character, so these branches predict perfectly and the default costs the
    // one tanh it always did.
    [[nodiscard]] float Shape(float x) const
    {
        if (tanhWeight == 1.0f)
        {
            return std::tanh(x);
        }

        float y = 0.0f;

        if (tanhWeight > 0.0f)
        {
            y += tanhWeight * std::tanh(x);
        }

        if (softWeight > 0.0f)
        {
            y += softWeight * SoftKneeShape(x);
        }

        if (hardWeight > 0.0f)
        {
            y += hardWeight * HardKneeShape(x);
        }

        return y;
    }

    [[nodiscard]] float Slope(float x) const
    {
        float slope = 0.0f;

        if (tanhWeight > 0.0f)
        {
            const float t = std::tanh(x);
            slope += tanhWeight * (1.0f - t * t);
        }

        if (softWeight > 0.0f)
        {
            slope += softWeight * SoftKneeSlope(x);
        }

        if (hardWeight > 0.0f)
        {
            slope += hardWeight * HardKneeSlope(x);
        }

        return slope;
    }
};

/**
 * The preamp and power-stage clippers as Character, Power Drive and Bias have
 * set them up. None of it depends on the channel or the oversampled step, so
 * the effect sets it once per host sample, and only while those controls move.
 */
struct Clippers
{
    /// Picks the knee, and how far off centre each preamp stage sits on it.
    /// The power stage shares the knee, so SetPowerStage has to follow.
    void SetCharacter(float character)
    {
        knee.SetCharacter(character);

        // Vintage stages sit further off centre, which is where the even
        // harmonics and the wooly, octave-leaning fuzz come from. Modern
        // ones are close to symmetric, so the spectrum is odd-order and
        // tight. Exactly 1.0 at the default.
        const float offCentre = 0.5f - character;
        const float biasScale = 1.0f + 2.2f * offCentre + 1.2f * offCentre * offCentre;

        for (int i = 0; i < kClipCount; ++i)
        {
            const float bias = kClipBias[i] * biasScale;
            clipBias[i] = bias;
            clipOffset[i] = knee.Shape(bias);
            clipInvSlope[i] = 1.0f / std::max(knee.Slope(bias), 0.1f);
        }
    }

    void SetPowerStage(float drive, float bias)
    {
        powerGain = 1.0f + 3.0f * drive;
        powerBias = 0.08f * bias;
        powerOffset = knee.Shape(powerGain * powerBias);
        powerInvScale = 1.0f / std::max(knee.Shape(powerGain), 0.1f);
    }

    // One biased stage: the offset keeps silence at zero and the slope keeps
    // the small-signal gain at one, whatever knee and bias Character picked.
    [[nodiscard]] float Clip(float x, int index) const
    {
        return (knee.Shape(x + clipBias[index]) - clipOffset[index]) * clipInvSlope[index];
    }

    /// The power stage, fully driven, on an input already scaled by the sag's `headroom`.
    [[nodiscard]] float PowerClip(float input, float headroom) const
    {
        return headroom * (knee.Shape(powerGain * (input / headroom + powerBias)) - powerOffset) * powerInvScale;
    }

    KneeBlend knee;
    std::array<float, kClipCount> clipBias = {};
    std::array<float, kClipCount> clipOffset = {};
    std::array<float, kClipCount> clipInvSlope = {1.0f, 1.0f, 1.0f, 1.0f, 1.0f};
    float powerGain = 1.0f;
    float powerBias = 0.0f;
    float powerOffset = 0.0f;
    float powerInvScale = 1.0f;
};

// ---------------------------------------------------------------------------------------
// Level makeup
// ---------------------------------------------------------------------------------------

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
inline constexpr float kDefaultGain = 0.45f;
inline constexpr int kDefaultStages = 2;
inline constexpr float kHeardLevelDb[2][kMaxStages][5] = {{{-19.73f, -17.43f, -15.66f, -14.23f, -13.04f},
                                                           {-21.02f, -16.31f, -12.84f, -10.23f, -8.28f},
                                                           {-20.04f, -14.03f, -9.84f, -7.04f, -5.26f},
                                                           {-20.93f, -14.16f, -9.76f, -7.13f, -5.62f}},
                                                          {{-11.89f, -8.81f, -5.36f, -2.50f, -1.24f},
                                                           {-13.57f, -8.71f, -4.52f, -2.37f, -1.78f},
                                                           {-12.84f, -7.45f, -3.52f, -1.87f, -1.44f},
                                                           {-14.17f, -8.62f, -4.78f, -3.13f, -2.46f}}};

[[nodiscard]] inline float HeardLevelDb(int voice, int stages, float gain)
{
    const float position = std::clamp(gain, 0.0f, 1.0f) * 4.0f;
    const int index = std::min(static_cast<int>(position), 3);
    const float fraction = position - static_cast<float>(index);
    const auto& row = kHeardLevelDb[voice][stages - 1];
    return row[index] + fraction * (row[index + 1] - row[index]);
}

/**
 * The output trim that keeps Gain and Preamp Stages from being volume
 * controls, in dB (see kHeardLevelDb). BuiltinAmpEffectTests --measure-levels
 * uses it to take the makeup back out when it re-measures the table.
 */
[[nodiscard]] inline float LevelMakeupDb(float gain, float voice, int stages)
{
    const float clean = HeardLevelDb(0, kDefaultStages, kDefaultGain) - HeardLevelDb(0, stages, gain);
    const float drive = HeardLevelDb(1, kDefaultStages, kDefaultGain) - HeardLevelDb(1, stages, gain);
    return clean + voice * (drive - clean);
}

[[nodiscard]] inline float LevelMakeup(float gain, float voice, int stages)
{
    constexpr float dbToLog2 = 0.166096405f; // 1 / (20 log10 2)
    return std::exp2(LevelMakeupDb(gain, voice, stages) * dbToLog2);
}
} // namespace guitarfx::builtin_amp

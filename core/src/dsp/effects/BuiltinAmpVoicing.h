#pragma once

#include "dsp/EffectParamSpec.h"
#include "dsp/LevelTargets.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <string_view>

/**
 * What the Heavy American is, as opposed to how it runs (BuiltinAmpEffect.h): its parameter
 * table, where the preamp stages sit, the clipper knees Character morphs between, the drive law,
 * and the level makeup that holds the amp at the nominal level whatever Gain, Preamp Stages,
 * Character and Power Drive are set to.
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

    /// The power stage, fully driven, with the sag holding its ceiling down to `headroom`: the
    /// knee sees the input against that ceiling, so the same input clips harder as it drops.
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

/// How far full Sag pulls the power stage's ceiling down, per unit of its envelope.
inline constexpr float kSagDepth = 0.6f;

/// The power stage's ceiling, the supply giving way under `envelope`: the stage's recent peaks.
[[nodiscard]] inline float SagHeadroom(float sag, float envelope)
{
    return 1.0f / (1.0f + kSagDepth * sag * envelope);
}

// ---------------------------------------------------------------------------------------
// Level makeup
// ---------------------------------------------------------------------------------------

/**
 * The amp's level is the Output control's alone. A guitar at the nominal operating level comes out
 * at that level, as heard through a cab, with Output at 0 dB, whatever Voice, Gain, Preamp Stages,
 * Character and Power Drive are set to: those choose how it distorts. Driving a cascade harder
 * raises its small-signal gain long before it saturates, so without makeup the amp ran 2 to 17 dB
 * over its input, and every comparison was really a loudness comparison.
 *
 * This is the amp's heard gain with no makeup and no Power Drive, by [voice][stages - 1]
 * [Character 0, 0.25 .. 1][Gain 0, 0.25 .. 1]: the demo DI and both demo riffs, each at -18 dBFS
 * RMS while it plays, through a generic cab band (2nd-order 90 Hz high pass, 4.5 kHz low pass)
 * and the BS.1770 K-weighting shelf, against the same measure of the input. The makeup is its
 * inverse, so the stages are driven exactly as hard as before and only the final level moves.
 *
 * It has to be measured on guitar, at the nominal level. An amp is a compressor: the clean end
 * follows its input and the high-gain end sits at its ceiling. A table once taken on quieter sine
 * tones (0.1 peak) put the two level there, and on a real guitar Gain, stages and Power Drive
 * then each cost up to 4-5 dB, the Drive voice ran 6 dB over the Clean, and the default came out
 * 7.5 dB over the input. Character moves the level by up to 3 dB, so it is in the table too.
 *
 * It is static on purpose: a level follower would flatten the playing dynamics the amp is meant
 * to keep. So the level holds at the nominal level, and playing softer or harder than that, the
 * clean end follows you and the high-gain end compresses, as an amp does. Re-measure this if the
 * voicing changes (BuiltinAmpEffectTests --measure-levels prints a replacement);
 * TestLevelHoldsAtNominal fails when it goes stale.
 */
inline constexpr float kDefaultGain = 0.45f;
inline constexpr int kDefaultStages = 2;
inline constexpr float kHeardGainDb[2][kMaxStages][5][5] = {{{{2.18f, 3.90f, 5.14f, 6.09f, 6.85f},
                                                              {2.59f, 4.36f, 5.65f, 6.63f, 7.41f},
                                                              {3.26f, 5.12f, 6.47f, 7.49f, 8.29f},
                                                              {3.59f, 5.50f, 6.88f, 7.92f, 8.73f},
                                                              {3.92f, 5.88f, 7.30f, 8.37f, 9.20f}},
                                                             {{1.17f, 4.74f, 7.05f, 8.66f, 9.85f},
                                                              {1.21f, 4.78f, 7.13f, 8.77f, 9.95f},
                                                              {1.85f, 5.58f, 8.01f, 9.65f, 10.81f},
                                                              {2.13f, 5.96f, 8.45f, 10.09f, 11.23f},
                                                              {2.49f, 6.42f, 8.97f, 10.63f, 11.76f}},
                                                             {{2.51f, 6.92f, 9.65f, 11.45f, 12.70f},
                                                              {2.21f, 6.46f, 9.15f, 10.94f, 12.18f},
                                                              {2.82f, 7.24f, 9.96f, 11.72f, 12.91f},
                                                              {3.09f, 7.66f, 10.45f, 12.20f, 13.35f},
                                                              {3.47f, 8.21f, 11.07f, 12.83f, 13.97f}},
                                                             {{1.90f, 6.27f, 8.84f, 10.45f, 11.53f},
                                                              {1.63f, 6.16f, 8.91f, 10.68f, 11.86f},
                                                              {2.31f, 7.15f, 10.00f, 11.79f, 12.97f},
                                                              {2.50f, 7.54f, 10.48f, 12.29f, 13.45f},
                                                              {2.87f, 8.12f, 11.15f, 12.97f, 14.14f}}},
                                                            {{{9.43f, 11.37f, 13.48f, 15.70f, 17.23f},
                                                              {8.58f, 10.42f, 12.37f, 14.36f, 15.66f},
                                                              {8.86f, 10.66f, 12.53f, 14.38f, 15.53f},
                                                              {9.25f, 11.02f, 12.85f, 14.69f, 15.85f},
                                                              {9.73f, 11.50f, 13.34f, 15.21f, 16.40f}},
                                                             {{6.71f, 9.75f, 12.65f, 15.15f, 16.34f},
                                                              {6.12f, 9.29f, 12.27f, 14.56f, 15.45f},
                                                              {6.72f, 10.00f, 12.98f, 15.07f, 15.62f},
                                                              {7.18f, 10.51f, 13.45f, 15.44f, 15.99f},
                                                              {7.76f, 11.17f, 14.08f, 16.02f, 16.57f}},
                                                             {{7.75f, 11.18f, 14.26f, 16.56f, 17.53f},
                                                              {6.66f, 10.28f, 13.60f, 15.64f, 16.19f},
                                                              {7.23f, 10.97f, 14.27f, 15.96f, 16.11f},
                                                              {7.67f, 11.52f, 14.75f, 16.28f, 16.42f},
                                                              {8.32f, 12.26f, 15.44f, 16.83f, 16.96f}},
                                                             {{6.10f, 9.40f, 12.33f, 14.47f, 15.46f},
                                                              {5.50f, 9.26f, 12.69f, 14.70f, 15.29f},
                                                              {6.30f, 10.31f, 13.82f, 15.56f, 15.78f},
                                                              {6.69f, 10.84f, 14.34f, 15.94f, 16.13f},
                                                              {7.34f, 11.61f, 15.09f, 16.53f, 16.69f}}}};

/**
 * The table holds for a guitar at -18 dBFS. The nominal operating level (Settings) says where the
 * player's guitar really sits, so the amp meets it there: the input is scaled to -18 dBFS before
 * the first stage, so each stage clips where it was voiced to, and the output is scaled back, so
 * it comes out at the nominal level. Exactly 1 at the default, as the drive pedals' volts are.
 */
struct NominalLevel
{
    /// Picks up a change of the setting; read once per block.
    void Follow()
    {
        const double dbfs = GetNominalOperatingLevelDbfs();

        if (dbfs != followed)
        {
            followed = dbfs;
            inputGain = static_cast<float>(DbToLinearGain(kDefaultNominalOperatingLevelDbfs - dbfs));
        }
    }

    double followed = kDefaultNominalOperatingLevelDbfs;
    float inputGain = 1.0f;
};

/// One voice's heard gain, interpolated across the table's Character and Gain steps.
[[nodiscard]] inline float HeardGainDb(int voice, int stages, float character, float gain)
{
    const float across = std::clamp(character, 0.0f, 1.0f) * 4.0f;
    const float along = std::clamp(gain, 0.0f, 1.0f) * 4.0f;
    const int row = std::min(static_cast<int>(across), 3);
    const int column = std::min(static_cast<int>(along), 3);
    const float rowFraction = across - static_cast<float>(row);
    const float columnFraction = along - static_cast<float>(column);
    const auto& grid = kHeardGainDb[voice][stages - 1];
    const auto atGain = [&](int r) {
        return grid[r][column] + columnFraction * (grid[r][column + 1] - grid[r][column]);
    };
    const float low = atGain(row);
    return low + rowFraction * (atGain(row + 1) - low);
}

/// The amp's heard gain before makeup and the power stage, the voices blended while Voice glides.
[[nodiscard]] inline float PreampGainDb(float gain, float voice, float character, int stages)
{
    const float clean = HeardGainDb(0, stages, character, gain);
    return clean + voice * (HeardGainDb(1, stages, character, gain) - clean);
}

/**
 * The output trim that holds the amp at the nominal level, in dB (see kHeardGainDb).
 * BuiltinAmpEffectTests --measure-levels uses it to take the makeup back out when it re-measures
 * the table.
 */
[[nodiscard]] inline float LevelMakeupDb(float gain, float voice, float character, int stages)
{
    return -PreampGainDb(gain, voice, character, stages);
}

[[nodiscard]] inline float LevelMakeup(float gain, float voice, float character, int stages)
{
    constexpr float dbToLog2 = 0.166096405f; // 1 / (20 log10 2)
    return std::exp2(LevelMakeupDb(gain, voice, character, stages) * dbToLog2);
}

/**
 * The makeup while Preamp Stages fades stages in or out, `stageIn[k]` being how far stage k + 1
 * is in. Each stage brings its own step of the table in with it, so going from one count to
 * another the makeup moves straight there. Passing through the counts in between instead
 * follows the table's zigzag, and every corner is a kink in the level.
 */
[[nodiscard]] inline float LevelMakeup(float gain, float voice, float character,
                                       const std::array<float, kMaxStages>& stageIn)
{
    float db = LevelMakeupDb(gain, voice, character, 1);

    for (int stage = 1; stage < kMaxStages; ++stage)
    {
        if (stageIn[stage] > 0.0f)
        {
            db += stageIn[stage] *
                  (LevelMakeupDb(gain, voice, character, stage + 1) - LevelMakeupDb(gain, voice, character, stage));
        }
    }

    constexpr float dbToLog2 = 0.166096405f; // 1 / (20 log10 2)
    return std::exp2(db * dbToLog2);
}

/**
 * Power Drive has to change how hard the power stage is pushed, not how loud the amp is. Fully
 * driven, the stage is a 12 dB small-signal boost into its clipper, so without this full Power
 * Drive was up to 9 dB louder than none on a guitar: the most where the preamp runs clean, the
 * least where it is already saturated.
 *
 * What the stage adds depends on how hot it is fed, and the level table already says that. A
 * guitar there behaves like a sine whose peak sits kPowerFeedDb + kPowerFeedSlope x the table's
 * heard gain (dBFS, for a guitar at -18 dBFS). The slope is under 1 because a hotter preamp is a
 * more saturated one, its peaks closer to its average: a clean preamp's peaks reach far further
 * into the stage than its heard level says. The makeup takes out that sine's RMS gain through
 * the power stage as the Clippers have it set up, so it follows Character's knee and Bias as well
 * as the drive.
 *
 * Sag is in it too. Its envelope follows the stage's peaks, so on the sine it holds the ceiling
 * at SagHeadroom(sag, peak), and that alone predicts what Sag costs on the guitar (up to 4.6 dB at
 * full Sag and Power Drive) to 0.2 dB RMS. Static, so Sag keeps its feel: loud playing is still
 * squeezed harder than soft, around a level that holds.
 *
 * Fitted on the same guitar as the table over voice, stages, gain, Character, Power Drive, Bias
 * and Sag, it holds to 0.35 dB RMS and 1.2 dB at worst. TestLevelHoldsAtNominal fails when this
 * goes stale.
 */
inline constexpr float kPowerFeedDb = -11.20f;
inline constexpr float kPowerFeedSlope = 0.74f;

/// The level the power stage adds at `drive` and `sag`, in dB, fed like a sine peaking at
/// `sinePeakDb`.
[[nodiscard]] inline float PowerStageGainDb(const Clippers& clippers, float drive, float sag, float sinePeakDb)
{
    constexpr int kPoints = 16;
    constexpr float dbToLog2 = 0.166096405f; // 1 / (20 log10 2)
    const float amplitude = std::exp2(sinePeakDb * dbToLog2);
    const float headroom = SagHeadroom(sag, amplitude);
    std::array<float, kPoints> out = {};
    float in = 0.0f, mean = 0.0f;

    for (int i = 0; i < kPoints; ++i)
    {
        const float x = amplitude * static_cast<float>(std::sin(2.0 * kPi * (i + 0.5) / kPoints));
        out[i] = x + drive * (clippers.PowerClip(x, headroom) - x);
        in += x * x;
        mean += out[i] / kPoints;
    }

    // Bias makes the stage asymmetric; the DC that adds is filtered out after it, and unheard.
    float power = 0.0f;

    for (const float y : out)
    {
        power += (y - mean) * (y - mean);
    }

    return 10.0f * std::log10(power / in);
}

/// The trim that keeps Power Drive and Sag from being volume controls, as a gain, for the controls
/// as set. Exactly 1 with no Power Drive, where nothing clips for Sag to act on.
[[nodiscard]] inline float PowerDriveMakeup(float gain, float voice, int stages, float character, float drive,
                                            float bias, float sag)
{
    if (drive <= 0.0f)
    {
        return 1.0f;
    }

    Clippers clippers;
    clippers.SetCharacter(character);
    clippers.SetPowerStage(drive, bias);
    const float sinePeakDb = kPowerFeedDb + kPowerFeedSlope * PreampGainDb(gain, voice, character, stages);
    constexpr float dbToLog2 = 0.166096405f; // 1 / (20 log10 2)
    return std::exp2(-PowerStageGainDb(clippers, drive, sag, sinePeakDb) * dbToLog2);
}
} // namespace guitarfx::builtin_amp

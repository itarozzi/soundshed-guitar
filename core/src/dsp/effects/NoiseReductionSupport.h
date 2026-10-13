#pragma once

#include "dsp/EffectParamSpec.h"
#include "dsp/EffectRegistry.h"
#include "dsp/FiniteCheck.h"
#include "dsp/effects/FactoryPresetSupport.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <vector>

/**
 * What the pickup noise reduction is, as opposed to how it runs (NoiseReductionEffect.h): its
 * constants, its parameter table, the per-pickup voicings, the two building blocks (a bank of
 * adaptive harmonic cancellers and a minimum-statistics noise floor) and the factory presets.
 */
namespace guitarfx::noise_reduction
{
constexpr double kPi = 3.14159265358979323846;

// ── Hum ──────────────────────────────────────────────────────────────────────────

/// The most mains harmonics the canceller follows: 4.8 kHz of buzz at 50 Hz, 5.76 kHz at 60.
constexpr std::size_t kMaxHarmonics = 96;
/// No harmonic is followed above this fraction of the sample rate.
constexpr double kHarmonicCeilingFraction = 0.45;
/// Each harmonic's amplitude and phase converge with this time constant while the input is
/// quiet. Its notch is then about 1 Hz wide, narrow enough to leave a note a few hertz away
/// alone even while it learns.
constexpr double kHumLearnMs = 300.0;
/// The amount of Hum glides over this long, so turning it never clicks.
constexpr double kDepthSmoothingMs = 20.0;

/// The frequency-locked loop: every interval, the rotation of each harmonic's learned phase
/// says how far the oscillators are off the real mains frequency, and the fundamental moves
/// part of the way there. Mains wanders by hundredths of a hertz; at the 96th harmonic that is
/// a few hertz, far wider than a notch, so without this the buzz would never be cancelled.
constexpr double kFllIntervalSeconds = 0.1;
constexpr double kFllGain = 0.5;
/// The fundamental stays within this far of 50 or 60 Hz.
constexpr double kMaxDriftHz = 0.5;
/// The loop only steers from an interval that was this quiet on average: learning was on.
constexpr double kFllMinQuiet = 0.6;
/// A harmonic whose phase moved more than this in one interval is too far off to read.
constexpr double kFllMaxStepRadians = 2.5;
/// A harmonic only counts toward the loop once its learned power is this many times what noise
/// alone leaves in a weight pair: about mu x residual power, the LMS misadjustment. Weights
/// that are only noise turn at random and would walk the fundamental about.
constexpr double kFllMinSnr = 10.0;

/// Auto mains: two small banks listen for 50 Hz and 60 Hz hum, on harmonics the two do not
/// share, and learn faster than the canceller so mains drift cannot hide either.
constexpr std::size_t kProbeHarmonics = 6;
inline constexpr std::array<float, kProbeHarmonics> kProbeMultiples = {1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 7.0f};
constexpr double kProbeLearnMs = 80.0;
/// How often Auto reconsiders, how much stronger the other mains must be to switch to it, and
/// the least hum power, as a mean square, worth switching for (-100 dBFS).
constexpr double kMainsDecisionSeconds = 0.5;
constexpr double kMainsSwitchRatio = 4.0;
constexpr double kMainsMinPower = 1.0e-10;

// ── Quiet detection ──────────────────────────────────────────────────────────────

/// The input counts as quiet, and the hum is learned, while what is left after cancelling sits
/// within this power ratio (6 dB) of its own noise floor. Above it learning fades out steeply,
/// so a note being played is never learned as hum.
constexpr double kQuietRatio = 4.0;
/// The same test on the input as it came, which still holds the hum, so it is held closer
/// (3 dB): playing at the hum's own level already halves learning.
constexpr double kInputQuietRatio = 2.0;
/// The detector's mean-square smoothing, and its DC blocker, which keeps an offset on the input
/// from reading as a raised floor.
constexpr double kDetectorMs = 15.0;
constexpr double kDetectorDcMs = 200.0;

/// Noise floors are minimum statistics: the lowest smoothed level over the last
/// kFloorSubwindows x kFloorSubwindowSeconds. Any pause, palm mute or decay in that time shows
/// the floor; only five seconds of unbroken playing can lift it, and the ceilings below cap
/// how far.
constexpr double kFloorSubwindowSeconds = 0.5;
constexpr std::size_t kFloorSubwindows = 10;
/// The broadband floor's ceiling, as a mean square: -40 dBFS.
constexpr double kResidualFloorCeiling = 1.0e-4;
/// The hiss bands' ceiling: -50 dBFS.
constexpr double kHissFloorCeiling = 1.0e-5;
constexpr double kFloorMinimum = 1.0e-14;
/// The smoothed levels start from zero, so for this long after a reset they read low and the
/// floors take nothing from them. Five of the slowest smoothing's time constants.
constexpr double kFloorWarmUpSeconds = 0.25;

// ── Hiss ─────────────────────────────────────────────────────────────────────────

/// A band's noise level is taken as this power ratio (3 dB) over its floor, since a minimum of a
/// smoothed level sits below the level's mean. At the noise level a band gets the full cut
/// Hiss asks for; at Threshold above it, unity; between, the gain follows a power law, so the
/// expansion ratio is whatever spans the two.
constexpr double kNoiseOverFloor = 2.0;
/// Threshold is held at least this far (1 dB, as a power ratio) above the noise level, so the
/// power law always has a span to work across.
constexpr double kMinThresholdRatio = 1.2589254;
/// Hiss at 100% cuts a band by up to this much.
constexpr double kMaxHissReductionDb = 30.0;
/// The bands' level detectors smooth their power over this long, the same both ways. A pick
/// still opens a band within a millisecond, since its top end lands tens of decibels above the
/// threshold; a faster attack would ride the peaks of the hiss itself and hold the band open.
constexpr double kBandDetectorMs = 5.0;
/// The smoothing behind each band's noise floor estimate.
constexpr double kBandFloorMs = 50.0;
/// A band's gain opens over this long; Release sets how it closes.
constexpr double kGainOpenMs = 2.0;

enum Param : std::size_t
{
    kPickup,
    kHum,
    kMains,
    kHiss,
    kThreshold,
    kRelease,
    kListen,
    kParamCount
};

enum class Pickup : int
{
    SingleCoil,
    Humbucker,
    Active,
    Bass
};

enum class Mains : int
{
    Auto,
    Hz50,
    Hz60
};

enum class Listen : int
{
    Output,
    Removed
};

inline constexpr const char* kPickupLabels[] = {"Single Coil", "Humbucker", "Active", "Bass"};
inline constexpr const char* kMainsLabels[] = {"Auto", "50 Hz", "60 Hz"};
inline constexpr const char* kListenLabels[] = {"Output", "Removed Noise"};

/// In `Param` order, which is also the order the UI lays the controls out in.
inline constexpr std::array<EffectParamSpec, kParamCount> kParams = {{
    {"pickup", "Pickup", 0.0, 0.0, 3.0, "enum", "Pickup", false, 1.0, kPickupLabels},
    {"hum", "Hum", 1.0, 0.0, 1.0, "amount", "Hum", false, 0.0, {}},
    {"mains", "Mains", 0.0, 0.0, 2.0, "enum", "Hum", false, 1.0, kMainsLabels},
    {"hiss", "Hiss", 0.5, 0.0, 1.0, "amount", "Hiss", false, 0.0, {}},
    {"threshold", "Threshold", 10.0, 0.0, 24.0, "dB", "Hiss", false, 0.0, {}},
    {"release", "Release", 100.0, 20.0, 500.0, "ms", "Hiss", true, 0.0, {}},
    {"listen", "Listen", 0.0, 0.0, 1.0, "enum", "Output", true, 1.0, kListenLabels},
}};

using ParamValues = std::array<double, kParamCount>;
inline constexpr ParamValues kDefaultValues = DefaultParamValues(kParams);

/// What each pickup needs. A single coil picks up mains as buzz, a spray of harmonics well into
/// the kilohertz; a humbucker cancels most of that and leaves a little hum; active pickups are
/// humbuckers too, with a preamp whose hiss sits a little higher. A bass's own content stops
/// lower, so its hiss bands start lower.
struct PickupVoicing
{
    double humCeilingHz; ///< the highest harmonic the canceller follows
    double lowSplitHz;   ///< below this the hiss reduction never touches the signal
    double highSplitHz;  ///< the split between the two bands it expands
};

inline constexpr std::array<PickupVoicing, 4> kPickupVoicings = {{
    {4800.0, 2000.0, 5000.0}, // Single Coil
    {1200.0, 2000.0, 5000.0}, // Humbucker
    {600.0, 2500.0, 6000.0},  // Active
    {2400.0, 1200.0, 3500.0}, // Bass
}};

[[nodiscard]] inline const PickupVoicing& VoicingFor(double pickup)
{
    const auto index = static_cast<std::size_t>(std::clamp(static_cast<int>(pickup), 0, 3));
    return kPickupVoicings[index];
}

[[nodiscard]] inline double NominalMainsHz(Mains mains)
{
    return mains == Mains::Hz60 ? 60.0 : 50.0;
}

/// The one-pole coefficient for a `milliseconds` time constant at `sampleRate`.
[[nodiscard]] inline double OnePole(double milliseconds, double sampleRate)
{
    return 1.0 - std::exp(-1.0 / (milliseconds * 0.001 * sampleRate));
}

/**
 * A bank of adaptive sinusoid cancellers (Widrow's adaptive noise canceller, with the
 * reference generated rather than measured): one cosine and sine pair per harmonic, each pair
 * weighted to match that harmonic's amplitude and phase in the input by LMS.
 *
 * The harmonics are independent, so each weight pair converges on its own with a time constant
 * of 2 / mu samples. With the weights held, the bank is a fixed sum of sinusoids subtracted from
 * the input: it takes away the hum it learned and nothing else, whatever is played over it.
 * Each oscillator is a rotating phasor, renormalised once a block; arrays rather than structs,
 * so the per-sample loops vectorise.
 */
template <std::size_t Capacity> class HarmonicBank
{
  public:
    /// Puts harmonic k at multiples[k] x fundamentalHz, for as many as sit below `ceilingHz`.
    /// Harmonics already running keep their phase and weights; ones joining start silent.
    void Tune(double fundamentalHz, double sampleRate, const float* multiples, std::size_t count,
              double ceilingHz) noexcept
    {
        std::size_t active = 0;

        for (; active < std::min(count, Capacity); ++active)
        {
            const double hz = static_cast<double>(multiples[active]) * fundamentalHz;

            if (hz >= ceilingHz)
            {
                break;
            }

            const double step = 2.0 * kPi * hz / sampleRate;
            mStepCos[active] = static_cast<float>(std::cos(step));
            mStepSin[active] = static_cast<float>(std::sin(step));
            mMultiple[active] = multiples[active];
        }

        for (std::size_t k = mActive; k < active; ++k)
        {
            ClearHarmonic(k);
        }

        mActive = active;
    }

    void ClearWeights() noexcept
    {
        for (std::size_t k = 0; k < Capacity; ++k)
        {
            ClearHarmonic(k);
        }
    }

    [[nodiscard]] std::size_t ActiveCount() const noexcept
    {
        return mActive;
    }

    /// Advances every oscillator one sample and returns the bank's estimate for that sample.
    [[nodiscard]] float Step() noexcept
    {
        const std::size_t n = mActive;
        float sum = 0.0f;

        for (std::size_t k = 0; k < n; ++k)
        {
            const float c = mCos[k] * mStepCos[k] - mSin[k] * mStepSin[k];
            const float s = mCos[k] * mStepSin[k] + mSin[k] * mStepCos[k];
            mCos[k] = c;
            mSin[k] = s;
            sum += mA[k] * c + mB[k] * s;
        }

        return sum;
    }

    /// The LMS update for the sample Step() just produced: `gain` is mu times the error.
    void Adapt(float gain) noexcept
    {
        const std::size_t n = mActive;

        for (std::size_t k = 0; k < n; ++k)
        {
            mA[k] += gain * mCos[k];
            mB[k] += gain * mSin[k];
        }
    }

    /// Pulls each phasor back onto the unit circle. Float rotation drifts by about 1e-7 a
    /// sample, so once a block is plenty.
    void Renormalise() noexcept
    {
        const std::size_t n = mActive;

        for (std::size_t k = 0; k < n; ++k)
        {
            const float scale = 1.5f - 0.5f * (mCos[k] * mCos[k] + mSin[k] * mSin[k]);
            mCos[k] *= scale;
            mSin[k] *= scale;
        }
    }

    /// The mean-square power of everything the bank has learned.
    [[nodiscard]] double Power() const noexcept
    {
        double power = 0.0;

        for (std::size_t k = 0; k < mActive; ++k)
        {
            power += 0.5 * (static_cast<double>(mA[k]) * mA[k] + static_cast<double>(mB[k]) * mB[k]);
        }

        return power;
    }

    /// How far, in hertz, the real fundamental sits above the oscillators', from how far each
    /// harmonic's learned phase has turned since the last Snapshot(), `seconds` ago: a weighted
    /// least-squares fit of turn = 2 pi x multiple x drift x seconds. Only harmonics whose
    /// amplitudes, multiplied across the two snapshots, exceed `minWeight` count. False when none
    /// do.
    [[nodiscard]] bool MeasureDrift(double seconds, double minWeight, double& driftHz) const noexcept
    {
        double numerator = 0.0;
        double denominator = 0.0;

        for (std::size_t k = 0; k < mActive; ++k)
        {
            // Each pair as the phasor a - jb: its angle is the harmonic's phase.
            const double a = mA[k];
            const double b = mB[k];
            const double pa = mPrevA[k];
            const double pb = mPrevB[k];
            const double re = a * pa + b * pb;
            const double im = a * pb - b * pa;
            const double weight = std::sqrt(re * re + im * im);

            if (weight <= minWeight)
            {
                continue;
            }

            const double turn = std::atan2(im, re);

            if (std::abs(turn) > kFllMaxStepRadians)
            {
                continue;
            }

            const double multiple = mMultiple[k];
            numerator += weight * multiple * turn;
            denominator += weight * multiple * multiple;
        }

        if (denominator <= 1.0e-24 || seconds <= 0.0)
        {
            return false;
        }

        driftHz = numerator / denominator / (2.0 * kPi * seconds);
        return true;
    }

    void Snapshot() noexcept
    {
        mPrevA = mA;
        mPrevB = mB;
    }

    [[nodiscard]] bool IsFinite() const noexcept
    {
        const double power = Power();
        return guitarfx::IsFinite(power) && power < 1.0e6;
    }

  private:
    void ClearHarmonic(std::size_t k) noexcept
    {
        mCos[k] = 1.0f;
        mSin[k] = 0.0f;
        mA[k] = 0.0f;
        mB[k] = 0.0f;
        mPrevA[k] = 0.0f;
        mPrevB[k] = 0.0f;
    }

    std::array<float, Capacity> mCos = {};
    std::array<float, Capacity> mSin = {};
    std::array<float, Capacity> mStepCos = {};
    std::array<float, Capacity> mStepSin = {};
    std::array<float, Capacity> mA = {};
    std::array<float, Capacity> mB = {};
    std::array<float, Capacity> mPrevA = {};
    std::array<float, Capacity> mPrevB = {};
    std::array<float, Capacity> mMultiple = {};
    std::size_t mActive = 0;
};

/**
 * A noise floor by minimum statistics: the lowest level pushed over the last
 * kFloorSubwindows subwindows, capped at a ceiling. Fed one minimum per block, so it costs
 * nothing per sample.
 */
class FloorTracker
{
  public:
    void Prepare(double sampleRate, double ceiling) noexcept
    {
        mSubwindowSamples = std::max(1, static_cast<int>(kFloorSubwindowSeconds * sampleRate));
        mCeiling = ceiling;
        Reset();
    }

    void Reset() noexcept
    {
        mMinima.fill(kUnset);
        mRunning = kUnset;
        mCount = 0;
        mNext = 0;
    }

    void Push(double blockMinimum, int samples) noexcept
    {
        mRunning = std::min(mRunning, blockMinimum);
        mCount += samples;

        if (mCount >= mSubwindowSamples)
        {
            mMinima[mNext] = mRunning;
            mNext = (mNext + 1) % kFloorSubwindows;
            mRunning = kUnset;
            mCount = 0;
        }
    }

    [[nodiscard]] bool HasReading() const noexcept
    {
        return mRunning < kUnset || mMinima[(mNext + kFloorSubwindows - 1) % kFloorSubwindows] < kUnset;
    }

    /// The ceiling until anything is pushed.
    [[nodiscard]] double Floor() const noexcept
    {
        double floor = mRunning;

        for (const double minimum : mMinima)
        {
            floor = std::min(floor, minimum);
        }

        return std::clamp(floor, kFloorMinimum, mCeiling);
    }

  private:
    static constexpr double kUnset = 1.0e30; ///< not infinity: fast-math may fold that away

    std::array<double, kFloorSubwindows> mMinima = {};
    double mRunning = kUnset;
    double mCeiling = 1.0;
    int mSubwindowSamples = 1;
    int mCount = 0;
    std::size_t mNext = 0;
};

/// The canceller's harmonic numbers, 1 to kMaxHarmonics.
inline constexpr std::array<float, kMaxHarmonics> kHarmonicMultiples = [] {
    std::array<float, kMaxHarmonics> multiples = {};

    for (std::size_t k = 0; k < kMaxHarmonics; ++k)
    {
        multiples[k] = static_cast<float>(k + 1);
    }

    return multiples;
}();

/// Mains and Listen are the player's: where they plug in, and what they are checking.
[[nodiscard]] inline std::vector<EffectPresetDefinition> FactoryPresets(const std::vector<ParameterDef>& params)
{
    const factory_presets::Builder b(params, {"mains", "listen"});

    return {
        b.Defaults("single-coil", "Single Coil"),
        b.Make("humbucker", "Humbucker", {{"pickup", 1.0}}),
        b.Make("active", "Active Pickups", {{"pickup", 2.0}, {"hiss", 0.6}}),
        b.Make("bass", "Bass", {{"pickup", 3.0}, {"hiss", 0.4}, {"release", 160.0}}),
        b.Make("hum-only", "Hum Only", {{"hiss", 0.0}}),
        b.Make("high-gain", "High Gain", {{"hiss", 0.85}, {"threshold", 14.0}, {"release", 70.0}}),
        b.Make("gentle", "Gentle", {{"hum", 0.7}, {"hiss", 0.25}, {"threshold", 6.0}, {"release", 200.0}}),
    };
}
} // namespace guitarfx::noise_reduction

#pragma once

#include "dsp/EffectGuids.h"
#include "dsp/EffectParamSpec.h"
#include "dsp/effects/WahEffect.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <string>

namespace guitarfx::wah_legacy
{
/**
 * Brings Auto-Wah nodes up to the Wah that replaced it.
 *
 * The Auto-Wah was folded into the Wah's Auto Wah control. A stored auto-wah node's type is one
 * of the wah's aliases now, so it runs as a wah, and it is given the wah's parameters here. Only
 * a node without a `control` key is touched: every wah node written since the merge carries one,
 * and so does a node this has already migrated. A loader may have resolved the node's type to
 * the wah's before it gets here (PresetStorage does, and so does the web UI for the legacy string
 * id), so an old node is also known by the old effect's keys, which no wah node ever carried.
 *
 * The old effect's filter did not do what its knobs said, so the mapping follows what it did,
 * from its transfer function and checked on its impulse response (core/tests/helpers/
 * LegacyAutoWah.h keeps it):
 * - Its state-variable filter resonated at half the frequency it was set to, so Min Freq and
 *   Max Freq become Heel Freq and Toe Freq at half their values.
 * - Its damping was the knob's 1/Q plus the integrator gain tan(pi f / fs), so its Q fell short
 *   of the knob, more so the higher the filter sat: Q 10 set to 300 Hz gave 8.4, set to 5 kHz
 *   gave 2.3. Its bandpass output peaked at that Q times the input, where the wah's peaks at
 *   2 sqrt(Q). So Q and Level are set to the Q and peak it had at the heel, and Toe Q Scale and
 *   Toe Gain to those it had at the toe, all at 48 kHz: the dependence on the sample rate is
 *   slight by comparison.
 * - It swept linearly in Hz where the wah sweeps in octaves. Taper is set so the two agree at
 *   mid-travel, which keeps them within about 10% of each other from a quarter open to the toe.
 *   Nearer the heel the taper's curve is steep, so on a faint signal (a note's last decay, hiss)
 *   the wah sits higher above the heel than the old filter did.
 * - Its envelope followed each channel on its own; the wah's one detector follows the louder.
 * - Its Sensitivity read the level 1 + 9 x Sensitivity times hotter; the wah's is a gain of 0 to
 *   40 dB. The setting is mapped to the same gain, so a migrated node opens as far as it did.
 * Its fixed attack and release are set explicitly, Pedal Position becomes the rest the envelope
 * opens from, and the wah's tone shaping (Low End, Treble, Saturation) is turned off.
 */

/// The old filter's resonance sat at this fraction of the frequency it was set to.
constexpr double kOldCentreScale = 0.5;
constexpr double kOldAttackMs = 5.0;
constexpr double kOldReleaseMs = 80.0;
/// The sample rate the old filter's Q is taken at.
constexpr double kReferenceSampleRate = 48000.0;
constexpr double kPi = 3.14159265358979323846;

/// The old effect's defaults, and the ranges it clamped its parameters to.
constexpr double kOldDefaultSensitivity = 0.6;
constexpr double kOldDefaultMinFreq = 300.0;
constexpr double kOldDefaultMaxFreq = 2800.0;
constexpr double kOldDefaultResonance = 2.5;
constexpr double kOldMinFreqRange[2] = {200.0, 1000.0};
constexpr double kOldMaxFreqRange[2] = {800.0, 5000.0};
constexpr double kOldResonanceRange[2] = {0.5, 10.0};

/// The old effect's type, by canonical id or legacy alias; the registry need not be populated.
[[nodiscard]] inline bool IsAutoWah(const std::string& type) noexcept
{
    return type == EffectGuids::kAutoWah || type == "auto_wah";
}

/// Whether a stored node is an old auto-wah still to be migrated: by its type, or, once a loader
/// has resolved that to the wah's, by the old effect's keys.
[[nodiscard]] inline bool IsLegacyAutoWahNode(const std::string& type, const std::map<std::string, double>& params)
{
    if (params.find("control") != params.end())
    {
        return false;
    }

    if (IsAutoWah(type))
    {
        return true;
    }

    const bool wah = type == EffectGuids::kWah || type == "wah";
    return wah && (params.count("minFreq") != 0 || params.count("maxFreq") != 0 || params.count("resonance") != 0);
}

/// The Q the old filter had when set to `resonance` at `nominalHz`, which is also its peak gain.
[[nodiscard]] inline double OldEffectiveQ(double resonance, double nominalHz) noexcept
{
    return 1.0 / (1.0 / resonance + std::tan(kPi * nominalHz / kReferenceSampleRate));
}

/// The Taper at which the wah's octave sweep from `heelHz` to `toeHz` passes mid-travel where a
/// sweep linear in Hz does. A sweep that does not rise gets an even taper, and the widest sweeps
/// run past the taper's range and are clamped, about 1% out at mid-travel.
[[nodiscard]] inline double LinearSweepTaper(double heelHz, double toeHz) noexcept
{
    if (!(heelHz > 0.0) || !(toeHz > heelHz * 1.001))
    {
        return 0.0;
    }

    // Mid-travel of the linear sweep, as a fraction of the octave span...
    const double ratio = toeHz / heelHz;
    const double warped = std::log(0.5 * (1.0 + ratio)) / std::log(ratio);
    // ...is where WarpPosition must send position 0.5: 0.5^exponent, with exponent = 3^-taper.
    const double exponent = std::log(warped) / std::log(0.5);
    return std::clamp(-std::log(exponent) / std::log(3.0), -1.0, 1.0);
}

/// The wah's Sensitivity for the gain the old detector read the level with, 1 + 9 x Sensitivity.
[[nodiscard]] inline double MatchedSensitivity(double oldSensitivity) noexcept
{
    return 20.0 * std::log10(1.0 + 9.0 * oldSensitivity) / wah::kSensitivityRangeDb;
}

/// The Level at which the wah's 2 sqrt(Q) peak matches the old filter's peak of `oldQ`, at Q
/// `oldQ`.
[[nodiscard]] inline double MatchedLevelDb(double oldQ) noexcept
{
    return 20.0 * std::log10(std::sqrt(oldQ) / wah::kPeakGainScale);
}

inline bool MigrateParams(const std::string& type, std::map<std::string, double>& params)
{
    if (!IsLegacyAutoWahNode(type, params))
    {
        return false;
    }

    const auto stored = [&params](const char* key, double fallback) {
        const auto it = params.find(key);
        return it != params.end() ? it->second : fallback;
    };

    // The old effect clamped its parameters to these ranges, so an out-of-range value sounded as
    // the clamped one did.
    const double sensitivity = std::clamp(stored("sensitivity", kOldDefaultSensitivity), 0.0, 1.0);
    const double minFreq = std::clamp(stored("minFreq", kOldDefaultMinFreq), kOldMinFreqRange[0], kOldMinFreqRange[1]);
    const double maxFreq = std::clamp(stored("maxFreq", kOldDefaultMaxFreq), kOldMaxFreqRange[0], kOldMaxFreqRange[1]);
    const double resonance =
        std::clamp(stored("resonance", kOldDefaultResonance), kOldResonanceRange[0], kOldResonanceRange[1]);
    const double mix = std::clamp(stored("mix", 1.0), 0.0, 1.0);

    params.erase("minFreq");
    params.erase("maxFreq");
    params.erase("resonance");

    const double heelHz = minFreq * kOldCentreScale;
    const double toeHz = maxFreq * kOldCentreScale;
    const double heelQ = OldEffectiveQ(resonance, minFreq);
    const double toeQ = OldEffectiveQ(resonance, maxFreq);

    const auto set = [&params](wah::Param param, double value) {
        const auto& spec = wah::kParams[param];
        params[spec.id] = NormaliseParamValue(spec, value);
    };

    set(wah::kControl, static_cast<double>(wah::Control::Envelope));
    set(wah::kPosition, 0.0);
    set(wah::kResponse, wah::kParams[wah::kResponse].defaultValue);
    set(wah::kAutoEngage, 0.0);
    set(wah::kSensitivity, MatchedSensitivity(sensitivity));
    set(wah::kAttack, kOldAttackMs);
    set(wah::kRelease, kOldReleaseMs);
    set(wah::kHeelFreq, heelHz);
    set(wah::kToeFreq, toeHz);
    set(wah::kTaper, LinearSweepTaper(heelHz, toeHz));
    set(wah::kQ, heelQ);
    set(wah::kToeQScale, toeQ / heelQ);
    // The wah's peak already moves with the square root of its Q; the rest of the old peak's fall
    // toward the toe is the tilt.
    set(wah::kToeGain, MatchedLevelDb(toeQ) - MatchedLevelDb(heelQ));
    set(wah::kLowEnd, 0.0);
    set(wah::kTreble, 0.0);
    set(wah::kSaturation, 0.0);
    set(wah::kLevel, MatchedLevelDb(heelQ));
    set(wah::kMix, mix);
    return true;
}
} // namespace guitarfx::wah_legacy

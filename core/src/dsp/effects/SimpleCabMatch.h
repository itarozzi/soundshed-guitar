#pragma once

#include "dsp/ImpulseResponseAnalysis.h"
#include "dsp/effects/SimpleCabVoicing.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <span>
#include <vector>

/**
 * Matching the Simple Cabinet to a measured response, usually a cabinet IR's.
 *
 * An IR carries one particular speaker, mic and room, notches and all; the Simple Cab can
 * only reach its broad shape. The match finds the cabinet type, mic type and tone settings
 * whose response has the closest shape to the target, so a favourite IR's character can be
 * had without convolution: cheaper on a phone, and every knob still works afterwards.
 */
namespace guitarfx::simple_cab
{
/// Where shapes are compared: 1/6 octave apart across the band that decides a cab's
/// character. Below 70 Hz and above 8 kHz an IR says more about the room and the mic's own
/// limits than about the cabinet.
[[nodiscard]] inline std::vector<double> MatchFrequencies()
{
    constexpr double kLowHz = 70.0;
    constexpr double kHighHz = 8000.0;
    constexpr double kStepsPerOctave = 6.0;
    const int count = static_cast<int>(std::floor(std::log2(kHighHz / kLowHz) * kStepsPerOctave)) + 1;
    std::vector<double> frequencies(static_cast<std::size_t>(count));

    for (int i = 0; i < count; ++i)
    {
        frequencies[static_cast<std::size_t>(i)] = kLowHz * std::exp2(i / kStepsPerOctave);
    }

    return frequencies;
}

struct MatchResult
{
    ParamValues values = kDefaultValues;
    double rmsErrorDb = 0.0; ///< shape error across MatchFrequencies(), after removing level; -1 = no match
};

/// RMS difference in dB between the shape of `values`' response and `targetDb`, with the
/// average level difference removed: a match is about shape, and Output handles level.
[[nodiscard]] inline double ShapeErrorDb(const ParamValues& values, std::span<const double> frequencies,
                                         std::span<const double> targetDb, const Voicer& voicer)
{
    const Design design = voicer.Build(ToSettings(values));
    const std::size_t count = frequencies.size();
    double meanDifference = 0.0;
    double sumSquares = 0.0;

    for (std::size_t i = 0; i < count; ++i)
    {
        const double difference = voicer.MagnitudeDb(design, frequencies[i]) - targetDb[i];
        meanDifference += difference;
        sumSquares += difference * difference;
    }

    meanDifference /= static_cast<double>(count);
    return std::sqrt(std::max(0.0, sumSquares / static_cast<double>(count) - meanDifference * meanDifference));
}

/**
 * The settings whose response shape best matches `targetDb`, measured in dB at
 * MatchFrequencies() at any level.
 *
 * Every cabinet and mic type is tried, and for each a coordinate search tunes the continuous
 * tone controls from their defaults, halving its step each time no single move helps.
 * Deterministic, and quick enough for the message thread (a few thousand response
 * evaluations). Everything the match does not model starts from its default: one close mic,
 * no spread, no dry mix. Output, Auto Level and Speaker Drive are left to the caller, who
 * should keep the node's own.
 */
[[nodiscard]] inline MatchResult MatchResponse(std::span<const double> targetDb, const Voicer& voicer)
{
    const std::vector<double> frequencies = MatchFrequencies();
    MatchResult best;

    if (targetDb.size() != frequencies.size())
    {
        best.rmsErrorDb = -1.0;
        return best;
    }

    constexpr std::array<Param, 6> kTuned = {kSize, kBass, kMids, kPresence, kBrightness, kMicPosition};
    constexpr std::array<double, 5> kSteps = {0.25, 0.125, 0.0625, 0.03125, 0.015625};
    constexpr int kMaxPassesPerStep = 8;
    best.rmsErrorDb = ShapeErrorDb(best.values, frequencies, targetDb, voicer);

    for (int cabinet = 0; cabinet < static_cast<int>(Cabinet::Count); ++cabinet)
    {
        for (int mic = 0; mic < static_cast<int>(MicType::Count); ++mic)
        {
            ParamValues values = kDefaultValues;
            values[kCabinet] = cabinet;
            values[kMicType] = mic;
            double error = ShapeErrorDb(values, frequencies, targetDb, voicer);

            for (const double step : kSteps)
            {
                for (int pass = 0; pass < kMaxPassesPerStep; ++pass)
                {
                    bool improved = false;

                    for (const Param param : kTuned)
                    {
                        for (const double direction : {-1.0, 1.0})
                        {
                            ParamValues candidate = values;
                            candidate[param] = NormaliseParamValue(kParams[param], values[param] + direction * step);

                            if (candidate[param] == values[param])
                            {
                                continue;
                            }

                            const double candidateError = ShapeErrorDb(candidate, frequencies, targetDb, voicer);

                            if (candidateError < error)
                            {
                                values = candidate;
                                error = candidateError;
                                improved = true;
                            }
                        }
                    }

                    if (!improved)
                    {
                        break;
                    }
                }
            }

            if (error < best.rmsErrorDb)
            {
                best.values = values;
                best.rmsErrorDb = error;
            }
        }
    }

    return best;
}

/// The parameters a match decides. The rest (Output, Auto Level, Speaker Drive) are about
/// level and playing feel rather than the cabinet's shape, so a match leaves the node's own.
[[nodiscard]] constexpr bool IsSetByMatch(int param) noexcept
{
    return param != kOutputGain && param != kAutoLevel && param != kSpeakerDrive;
}

/// Matches the cab to an impulse response: analyses its smoothed shape (see
/// SmoothedMagnitudeDb) and runs MatchResponse against it at the analysis rate.
[[nodiscard]] inline MatchResult MatchImpulseResponse(std::span<const float> impulse, double impulseSampleRate,
                                                      double modelSampleRate = 48000.0)
{
    const std::vector<double> target = SmoothedMagnitudeDb(impulse, impulseSampleRate, MatchFrequencies());

    if (target.empty())
    {
        MatchResult failed;
        failed.rmsErrorDb = -1.0;
        return failed;
    }

    return MatchResponse(target, Voicer(modelSampleRate));
}
} // namespace guitarfx::simple_cab

/**
 * @file BuiltinAmpGlideTests.cpp
 * @brief The Heavy American's control glides land exactly on their targets, so an amp moved live
 *        onto a setting plays exactly like one set there.
 *
 * Each control glides a fixed share of what is left every host sample. In float, a step that small
 * stops changing anything short of an upward target: about 1.4e-5 short of 1 at 48 kHz, four times
 * that at 192 kHz. builtin_amp::Glide snapped only within 1e-6, so it stopped there. After a live
 * switch Voice sat at 0.999986, and both stage-one legs were clipped and crossfaded for good; after
 * Character came back to 0.5, two knees ran for good. The single-leg and single-knee paths only
 * applied after a reset or a preset load.
 *
 * The move is made during silence, so that neither amp's filters hold anything, and then both have
 * to play the same note bit for bit. Bias is left out because away from 0 the power stage passes
 * DC through silence, and Character because it also moves filter corners, whose glides stop where
 * a step changes nothing rather than on the target.
 */

#include "dsp/effects/BuiltinAmpEffect.h"

#include <cmath>
#include <cstddef>
#include <iostream>
#include <utility>
#include <vector>

namespace
{
constexpr double kPi = 3.14159265358979323846;
constexpr int kBlock = 64;

using Setting = std::vector<std::pair<const char*, double>>;

struct Move
{
    const char* label;
    Setting from;
    Setting to;
};

void Apply(guitarfx::BuiltinAmpEffect& amp, const Setting& setting)
{
    for (const auto& [key, value] : setting)
    {
        amp.SetParam(key, value);
    }
}

struct Comparison
{
    bool silent = true;
    int differing = 0;
};

/// An amp set to `move.to` against one set to `move.from` and moved there live: 200 ms of silence,
/// which covers the slowest glide (about 140 ms), then a tenth of a second of a note.
Comparison CompareWithLiveMove(const Move& move, double sampleRate)
{
    guitarfx::BuiltinAmpEffect set, moved;
    Apply(set, move.to);
    Apply(moved, move.from);
    set.Prepare(sampleRate, kBlock);
    moved.Prepare(sampleRate, kBlock);
    Apply(moved, move.to);

    const int silence = static_cast<int>(0.2 * sampleRate);
    const int frames = silence + static_cast<int>(0.1 * sampleRate);
    std::vector<float> input(kBlock), setOutput(kBlock), movedOutput(kBlock);
    Comparison result;

    for (int start = 0; start < frames; start += kBlock)
    {
        for (int i = 0; i < kBlock; ++i)
        {
            const int frame = start + i;
            input[static_cast<std::size_t>(i)] =
                frame < silence ? 0.0f : static_cast<float>(0.1 * std::sin(2.0 * kPi * 110.0 * frame / sampleRate));
        }

        set.ProcessMono(input.data(), setOutput.data(), kBlock);
        moved.ProcessMono(input.data(), movedOutput.data(), kBlock);

        for (std::size_t i = 0; i < setOutput.size(); ++i)
        {
            result.silent = result.silent && (start >= silence || (setOutput[i] == 0.0f && movedOutput[i] == 0.0f));
            result.differing += setOutput[i] != movedOutput[i] ? 1 : 0;
        }
    }

    return result;
}
} // namespace

int main()
{
    const Move moves[] = {
        {"Voice Clean to Drive", {{"voice", 0.0}}, {{"voice", 1.0}}},
        {"Voice Drive to Clean", {{"voice", 1.0}}, {{"voice", 0.0}}},
        {"Gain 0.45 to 0.9", {{"gain", 0.45}}, {{"gain", 0.9}}},
        {"Power Drive 0.3 to 0.6", {{"powerDrive", 0.3}}, {{"powerDrive", 0.6}}},
        {"Sag 0 to 0.3", {{"powerDrive", 0.5}, {"sag", 0.0}}, {{"powerDrive", 0.5}, {"sag", 0.3}}},
        {"Input Trim 0 to +3 dB", {{"stageGain", 0.0}}, {{"stageGain", 3.0}}},
        {"Output 0 to -9 dB", {{"output", 0.0}}, {{"output", -9.0}}},
    };
    int failures = 0;

    for (const double sampleRate : {48000.0, 192000.0})
    {
        for (const Move& move : moves)
        {
            const Comparison result = CompareWithLiveMove(move, sampleRate);
            std::cout << move.label << " at " << sampleRate << " Hz: " << result.differing
                      << " samples differ from an amp set there\n";

            if (!result.silent)
            {
                std::cerr << "FAIL: " << move.label << " at " << sampleRate
                          << " Hz: silence did not come out silent, so the two amps' filters differ\n";
                ++failures;
            }
            else if (result.differing != 0)
            {
                std::cerr << "FAIL: " << move.label << " at " << sampleRate
                          << " Hz: moved live, the amp does not land exactly on the setting\n";
                ++failures;
            }
        }
    }

    if (failures != 0)
    {
        std::cerr << failures << " check(s) failed\n";
        return 1;
    }

    std::cout << "BuiltinAmpGlideTests passed\n";
    return 0;
}

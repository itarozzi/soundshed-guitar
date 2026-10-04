#pragma once

/**
 * BuiltinAmpEffectTests --measure-levels: re-measures the Heavy American's level makeup on the demo
 * guitar at the nominal level, and prints what BuiltinAmpVoicing.h needs, laid out to paste in:
 * kHeardGainDb, kTrimGainDb, and the power stage's kPowerFeedDb and kPowerFeedSlope. Each table is
 * measured with the makeup the amp applied taken back out, so it describes the amp's own voicing.
 */

#include "helpers/BuiltinAmpTestSupport.h"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <iostream>
#include <string>
#include <vector>

namespace builtin_amp_test
{
// Prints kHeardGainDb for BuiltinAmpVoicing.h, measured with the makeup taken
// back out, since the table has to describe the amp's own voicing.
inline void MeasureLevelTable(const std::vector<std::vector<float>>& guitar)
{
    constexpr int kCells = 2 * 4 * 5 * 5;
    std::vector<double> cells(kCells);
    ParallelFor(kCells, [&](int cell) {
        const int voice = cell / 100, stages = cell / 25 % 4 + 1, row = cell / 5 % 5, column = cell % 5;
        cells[cell] = RawGainDb({column * 0.25, static_cast<double>(voice), row * 0.25, stages}, guitar);
    });

    // Laid out as clang-format leaves it, so it pastes in as it is.
    const std::string declaration = "inline constexpr float kHeardGainDb[2][kMaxStages][5][5] = ";
    std::cout << std::fixed << std::setprecision(2);
    for (int cell = 0; cell < kCells; ++cell)
    {
        if (cell % 5 == 0)
        {
            const std::size_t opened = cell % 100 == 0 ? 3 : cell % 25 == 0 ? 2 : 1;
            std::cout << (cell == 0 ? declaration + "{" : std::string(declaration.size() + 4 - opened, ' '))
                      << std::string(opened, '{');
        }
        std::cout << cells[cell] << "f" << (cell % 5 < 4 ? ", " : "}");
        if (cell % 5 == 4)
        {
            std::cout << (cell % 25 != 24      ? ",\n"
                          : cell % 100 != 99   ? "},\n"
                          : cell != kCells - 1 ? "}},\n"
                                               : "}}};\n");
        }
    }
}

// Prints kTrimGainDb for BuiltinAmpVoicing.h: the heard level at each Input Trim against the same
// settings untrimmed, at the default Character and no Power Drive, where no makeup depends on it.
inline void MeasureTrimTable(const std::vector<std::vector<float>>& guitar)
{
    using namespace guitarfx::builtin_amp;
    constexpr auto kRows = static_cast<int>(kTrimGainRows), kColumns = static_cast<int>(kTrimColumns);
    constexpr int kCells = 2 * kMaxStages * kRows * kColumns;
    std::vector<double> levels(kCells);
    ParallelFor(kCells, [&](int cell) {
        const int voice = cell / (kMaxStages * kRows * kColumns), stages = cell / (kRows * kColumns) % kMaxStages + 1;
        const int row = cell / kColumns % kRows, column = cell % kColumns;
        Voicing v{TrimGainAtRow(static_cast<std::size_t>(row)), static_cast<double>(voice), 0.5, stages};
        v.trimDb = kTrimMinDb + (kTrimMaxDb - kTrimMinDb) * column / (kColumns - 1);
        double sum = 0.0;
        for (const auto& take : guitar)
        {
            sum += HeardGainDb(v, take);
        }
        levels[cell] = sum / static_cast<double>(guitar.size());
    });

    std::cout << "// clang-format off\n// *INDENT-OFF*\ninline constexpr TrimGrid kTrimGainDb = {\n" << std::fixed;
    for (int cell = 0; cell < kCells; cell += kColumns)
    {
        const int voice = cell / (kMaxStages * kRows * kColumns), stages = cell / (kRows * kColumns) % kMaxStages + 1;
        const int row = cell / kColumns % kRows;
        if (row == 0 && stages == 1)
        {
            std::cout << "    { // " << (voice == 0 ? "Clean" : "Drive") << "\n";
        }
        if (row == 0)
        {
            std::cout << "        { // " << stages << (stages == 1 ? " stage" : " stages") << "\n";
        }
        std::cout << "            {";
        for (int column = 0; column < kColumns; ++column)
        {
            const double change = levels[cell + column] - levels[cell + kColumns / 2];
            std::cout << (column ? ", " : "") << std::setprecision(2) << (std::fabs(change) < 0.005 ? 0.0 : change)
                      << "f";
        }
        std::cout << "}" << (row + 1 < kRows ? "," : "") << " // Gain " << std::setprecision(3)
                  << TrimGainAtRow(static_cast<std::size_t>(row)) << "\n";
        if (row + 1 == kRows)
        {
            std::cout << "        }" << (stages < kMaxStages ? "," : "") << "\n";
        }
        if (row + 1 == kRows && stages == kMaxStages)
        {
            std::cout << "    }" << (voice == 0 ? "," : "") << "\n";
        }
    }
    std::cout << "};\n// *INDENT-ON*\n// clang-format on\n";
}

// Prints a re-fitted kPowerFeedDb and kPowerFeedSlope for BuiltinAmpVoicing.h,
// after the table, which they read. Each point is the heard level Power Drive
// and Sag add with their makeup taken back out: what the power stage added.
inline void FitPowerFeed(const std::vector<std::vector<float>>& guitar)
{
    using namespace guitarfx::builtin_amp;
    std::vector<Voicing> voicings;
    for (const double voice : {0.0, 1.0})
    {
        for (const int stages : {1, 2, 4})
        {
            for (const double gain : {0.0, 0.5, 1.0})
            {
                for (const double character : {0.0, 0.5, 1.0})
                {
                    voicings.push_back({gain, voice, character, stages});
                    for (const double drive : {0.5, 1.0})
                    {
                        for (const double bias : {-1.0, 0.0, 1.0})
                        {
                            voicings.push_back({gain, voice, character, stages, drive, 0.0, bias});
                        }
                        voicings.push_back({gain, voice, character, stages, drive, 1.0});
                    }
                }
            }
        }
    }
    std::vector<double> raw(voicings.size());
    ParallelFor(static_cast<int>(voicings.size()), [&](int i) { raw[i] = RawGainDb(voicings[i], guitar); });

    struct Point
    {
        Clippers clippers;
        float drive, sag, preampDb;
        double addedDb;
    };

    std::vector<Point> points;
    double undriven = 0.0;
    for (std::size_t i = 0; i < voicings.size(); ++i)
    {
        const Voicing& v = voicings[i];
        if (v.powerDrive == 0.0)
        {
            undriven = raw[i];
            continue;
        }
        Point point{{}, static_cast<float>(v.powerDrive), static_cast<float>(v.sag), 0.0f, raw[i] - undriven};
        point.preampDb = PreampGainDb(static_cast<float>(v.gain), static_cast<float>(v.voice),
                                      static_cast<float>(v.character), v.stages);
        point.clippers.SetCharacter(static_cast<float>(v.character));
        point.clippers.SetPowerStage(point.drive, static_cast<float>(v.bias));
        points.push_back(point);
    }

    float bestFeed = 0.0f, bestSlope = 0.0f;
    double bestRms = 1.0e9, bestWorst = 0.0;
    for (int feed = -200; feed <= 0; ++feed)
    {
        for (int slope = 40; slope <= 100; ++slope)
        {
            double squares = 0.0, worst = 0.0;
            for (const Point& point : points)
            {
                const float sinePeakDb =
                    0.1f * static_cast<float>(feed) + 0.01f * static_cast<float>(slope) * point.preampDb;
                const double error =
                    PowerStageGainDb(point.clippers, point.drive, point.sag, sinePeakDb) - point.addedDb;
                squares += error * error;
                worst = std::max(worst, std::abs(error));
            }
            if (std::sqrt(squares / static_cast<double>(points.size())) < bestRms)
            {
                bestRms = std::sqrt(squares / static_cast<double>(points.size()));
                bestWorst = worst;
                bestFeed = 0.1f * static_cast<float>(feed);
                bestSlope = 0.01f * static_cast<float>(slope);
            }
        }
    }
    std::cout << std::fixed << std::setprecision(2) << "inline constexpr float kPowerFeedDb = " << bestFeed
              << "f;\ninline constexpr float kPowerFeedSlope = " << bestSlope << "f; // " << bestRms << " dB RMS, "
              << bestWorst << " dB at worst\n";
}
} // namespace builtin_amp_test

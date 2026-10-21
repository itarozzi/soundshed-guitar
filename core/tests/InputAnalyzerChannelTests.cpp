/**
 * @file InputAnalyzerChannelTests.cpp
 * @brief The Signal Analyzer's Channels reading holds still: stereo detection and per-side
 * activity follow the source over seconds, not whatever the last ~1 ms block held.
 *
 *  - Content whose block-by-block side difference swings across both thresholds reads stereo
 *    throughout, where the per-block decision used to flip every few milliseconds.
 *  - Silence between notes changes neither reading.
 *  - A side that goes quiet for a moment still counts as active; one quiet for longer than
 *    the hold does not, and counts again as soon as it plays.
 *  - Identical sides read as such; stereo that turns identical falls back only after the
 *    release time; a mono input is one channel and never stereo.
 */

#include <cmath>
#include <cstdint>
#include <functional>
#include <iostream>
#include <string>
#include <vector>

#include "dsp/effects/InputAnalyzerEffect.h"

using namespace guitarfx;

namespace
{
constexpr double kPi = 3.14159265358979323846;
constexpr double kSr = 48000.0;
constexpr int kBlock = 64;

int gFailures = 0;

bool Check(bool condition, const std::string& what, const std::string& detail = {})
{
    std::cout << (condition ? "[PASS] " : "[FAIL] ") << what;

    if (!detail.empty())
    {
        std::cout << " (" << detail << ")";
    }

    std::cout << "\n";

    if (!condition)
    {
        ++gFailures;
    }

    return condition;
}

using Signal = std::function<double(double seconds)>;

double Note(double t, double hz)
{
    return 0.3 * std::sin(2.0 * kPi * hz * t);
}

/// One reading per block that carried signal: the time at the block's end and the snapshot.
struct Reading
{
    double seconds;
    InputAnalyzerEffect::TelemetrySnapshot snapshot;
};

/// Plays `left` (and `right`, or no right input at all) through an analyzer in 64-sample blocks.
std::vector<Reading> Run(const Signal& left, const Signal* right, double seconds)
{
    InputAnalyzerEffect analyzer;
    analyzer.Prepare(kSr, kBlock);
    std::vector<float> inL(kBlock), inR(kBlock), outL(kBlock), outR(kBlock);
    std::vector<Reading> readings;
    const auto total = static_cast<std::size_t>(seconds * kSr);

    for (std::size_t at = 0; at + kBlock <= total; at += kBlock)
    {
        for (int i = 0; i < kBlock; ++i)
        {
            const double t = static_cast<double>(at + static_cast<std::size_t>(i)) / kSr;
            inL[static_cast<std::size_t>(i)] = static_cast<float>(left(t));
            inR[static_cast<std::size_t>(i)] = right ? static_cast<float>((*right)(t)) : 0.0f;
        }

        float* inputs[2] = {inL.data(), right ? inR.data() : nullptr};
        float* outputs[2] = {outL.data(), outR.data()};
        analyzer.Process(inputs, outputs, kBlock);
        const auto snapshot = analyzer.GetTelemetrySnapshot();

        if (snapshot.valid)
        {
            readings.push_back({static_cast<double>(at + kBlock) / kSr, snapshot});
        }
    }

    return readings;
}

/// Counts readings in [from, to) that fail `ok`, and how often `stereo` changes in that span.
struct Span
{
    int readings = 0;
    int failing = 0;
    int stereoFlips = 0;
};

Span Inspect(const std::vector<Reading>& readings, double from, double to,
             const std::function<bool(const InputAnalyzerEffect::TelemetrySnapshot&)>& ok)
{
    Span span;
    const InputAnalyzerEffect::TelemetrySnapshot* previous = nullptr;

    for (const auto& reading : readings)
    {
        if (reading.seconds < from || reading.seconds >= to)
        {
            continue;
        }

        ++span.readings;
        span.failing += ok(reading.snapshot) ? 0 : 1;

        if (previous && previous->stereo != reading.snapshot.stereo)
        {
            ++span.stereoFlips;
        }

        previous = &reading.snapshot;
    }

    return span;
}

std::string Describe(const Span& span)
{
    return std::to_string(span.failing) + " of " + std::to_string(span.readings) + " readings off, " +
           std::to_string(span.stereoFlips) + " stereo flips";
}

bool StereoOnTwo(const InputAnalyzerEffect::TelemetrySnapshot& s)
{
    return s.stereo && s.activeChannelCount == 2;
}

void TestSwingingDifferenceStaysStereo()
{
    std::cout << "\nA side difference that swings block to block\n";
    // The right side is the left plus a second tone switched between full and a fifth every
    // 5 ms: per block the difference ratio is ~0.08 (over the 0.05 entry) then ~0.016 (under
    // the 0.02 exit), which the old per-block decision followed.
    const Signal left = [](double t) { return Note(t, 220.0); };
    const Signal right = [](double t) {
        const bool full = static_cast<int>(t / 0.005) % 2 == 0;
        return Note(t, 220.0) + (full ? 0.08 : 0.016) * Note(t, 337.0);
    };
    const auto readings = Run(left, &right, 4.0);
    const auto span = Inspect(readings, 0.5, 4.0, StereoOnTwo);
    Check(span.readings > 0 && span.failing == 0 && span.stereoFlips == 0, "reads stereo throughout", Describe(span));
}

void TestGapsBetweenNotes()
{
    std::cout << "\nNotes with silence between them\n";
    // 0.5 s notes, 0.3 s of digital silence between, different tones on each side; the right
    // side also starts each note 100 ms late, as a ping-pong delay would.
    const auto playing = [](double t, double delay) {
        const double inCycle = std::fmod(t, 0.8);
        return inCycle >= delay && inCycle < 0.5;
    };
    const Signal left = [&](double t) { return playing(t, 0.0) ? Note(t, 196.0) : 0.0; };
    const Signal right = [&](double t) { return playing(t, 0.1) ? Note(t, 294.0) : 0.0; };
    const auto readings = Run(left, &right, 6.0);
    const auto span = Inspect(readings, 0.2, 6.0, StereoOnTwo);
    Check(span.readings > 0 && span.failing == 0 && span.stereoFlips == 0,
          "stereo on two sides across every gap and late start", Describe(span));
}

void TestSideDropout()
{
    std::cout << "\nOne side going quiet\n";
    // The right side stops from 1 s to 4.5 s while the left keeps playing.
    const Signal left = [](double t) { return Note(t, 220.0); };
    const Signal right = [](double t) { return (t >= 1.0 && t < 4.5) ? 0.0 : Note(t, 330.0); };
    const auto readings = Run(left, &right, 6.0);

    const auto held = Inspect(readings, 1.0, 2.9, [](const auto& s) { return s.activeChannelCount == 2; });
    Check(held.readings > 0 && held.failing == 0, "a side quiet for under the hold still counts", Describe(held));

    const auto dropped = Inspect(readings, 3.1, 4.5, [](const auto& s) { return s.activeChannelCount == 1 && !s.stereo; });
    Check(dropped.readings > 0 && dropped.failing == 0, "past the hold it is one side, not stereo", Describe(dropped));

    const auto back = Inspect(readings, 4.51, 6.0, StereoOnTwo);
    Check(back.readings > 0 && back.failing == 0, "and stereo on two sides again as soon as it plays", Describe(back));
}

void TestIdenticalSides()
{
    std::cout << "\nIdentical sides\n";
    const Signal both = [](double t) { return Note(t, 220.0) + 0.5 * Note(t, 661.0); };
    const auto readings = Run(both, &both, 3.0);
    const auto span = Inspect(readings, 0.0, 3.0, [](const auto& s) { return !s.stereo && s.activeChannelCount == 2; });
    Check(span.readings > 0 && span.failing == 0, "two active sides, never stereo", Describe(span));
}

void TestStereoReleasesAfterHold()
{
    std::cout << "\nStereo that turns identical\n";
    // Different sides for 1 s, then the same signal on both. The windowed difference takes
    // ~1.3 s to fall under the exit ratio from fully different sides, then the 2 s release
    // runs, so the reading changes at ~4.3 s.
    const Signal left = [](double t) { return Note(t, 220.0); };
    const Signal right = [](double t) { return t < 1.0 ? Note(t, 330.0) : Note(t, 220.0); };
    const auto readings = Run(left, &right, 6.0);

    const auto kept = Inspect(readings, 0.5, 3.0, StereoOnTwo);
    Check(kept.readings > 0 && kept.failing == 0, "stays stereo for the release time", Describe(kept));

    const auto released = Inspect(readings, 4.6, 6.0, [](const auto& s) { return !s.stereo && s.activeChannelCount == 2; });
    Check(released.readings > 0 && released.failing == 0, "then reads identical sides", Describe(released));
}

void TestMonoInput()
{
    std::cout << "\nA mono input\n";
    const Signal left = [](double t) { return Note(t, 220.0); };
    const auto readings = Run(left, nullptr, 2.0);
    const auto span = Inspect(readings, 0.0, 2.0, [](const auto& s) { return !s.stereo && s.activeChannelCount == 1; });
    Check(span.readings > 0 && span.failing == 0, "one channel, never stereo", Describe(span));
}
} // namespace

int main()
{
    TestSwingingDifferenceStaysStereo();
    TestGapsBetweenNotes();
    TestSideDropout();
    TestIdenticalSides();
    TestStereoReleasesAfterHold();
    TestMonoInput();

    std::cout << "\nInput analyzer channel tests: "
              << (gFailures == 0 ? "all passed" : std::to_string(gFailures) + " failed") << "\n";
    return gFailures == 0 ? 0 : 1;
}

/**
 * @file LoudnessTests.cpp
 * @brief The BS.1770 loudness helper (dsp/Loudness.h) against known signals and the content
 * pipeline's reference, and the Signal Analyzer reading the same through it.
 *
 *  - The K-weighting designed at 48 kHz is the coefficient table BS.1770 publishes.
 *  - A 997 Hz sine at -20 dBFS reads -20.0 LUFS on both channels and -23.0 on one, at 44.1, 48
 *    and 96 kHz; its sample peak is -20 dBFS and its RMS -23 dBFS.
 *  - Signals with gaps, quiet passages and different channels match what loudness.mjs (the
 *    pipeline's -18 LUFS balancing) gives for the same samples. The expected values below were
 *    printed by that script; the signal formulas are written identically on both sides.
 *  - Pushing a signal in pieces gives the answer pushing it whole does; silence has no loudness.
 *  - The Signal Analyzer, which now designs its filter and gates through the helper, reads the
 *    integrated loudness it read before the helper existed, to the last digit printed.
 */

#include <cmath>
#include <cstdio>
#include <functional>
#include <iostream>
#include <string>
#include <vector>

#include "dsp/Loudness.h"
#include "dsp/effects/InputAnalyzerEffect.h"

using namespace guitarfx;

namespace
{
constexpr double kPi = 3.14159265358979323846;

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

std::string Num(double value, int digits = 6)
{
    char text[64];
    std::snprintf(text, sizeof(text), "%.*f", digits, value);
    return text;
}

bool Near(const std::optional<double>& value, double expected, double tolerance)
{
    return value.has_value() && std::abs(*value - expected) <= tolerance;
}

std::string Show(const std::optional<double>& value)
{
    return value ? Num(*value) : std::string("none");
}

/// a * sin(2 pi f n / sr), written in the order loudness-fixtures.mjs evaluates it.
double Tone(double n, double f, double a, double sr)
{
    return a * std::sin(2.0 * kPi * f * n / sr);
}

std::vector<float> Make(std::size_t frames, const std::function<double(double)>& fn)
{
    std::vector<float> out(frames);

    for (std::size_t n = 0; n < frames; ++n)
    {
        out[n] = static_cast<float>(fn(static_cast<double>(n)));
    }

    return out;
}

constexpr double kSr = 48000.0;

/// The Signal Analyzer's integrated readings of the program and burst signals below, printed by
/// this test built against the analyzer as it was before it shared dsp/Loudness.h.
constexpr double kAnalyzerProgramBefore = -11.470727736417;
constexpr double kAnalyzerBurstBefore = -6.124554202931;

double ProgramLeft(double n)
{
    const double t = n / kSr;

    if (t < 2.0)
    {
        return Tone(n, 220.0, 0.5, kSr);
    }

    if (t < 4.0)
    {
        return Tone(n, 3000.0, 0.02, kSr);
    }

    if (t < 5.0)
    {
        return 0.0;
    }

    return Tone(n, 60.0, 0.25, kSr) + Tone(n, 5000.0, 0.1, kSr);
}

double ProgramRight(double n)
{
    return Tone(n, 440.0, 0.3, kSr);
}

double Burst(double n)
{
    return (n >= 2.0 * kSr && n < 3.0 * kSr) ? Tone(n, 1000.0, 0.8, kSr) : Tone(n, 1000.0, 0.001, kSr);
}

void TestKWeightingMatchesTheStandardTable()
{
    std::cout << "\nK-weighting at 48 kHz\n";
    // BS.1770-4, Table 1 and Table 2 (as loudness.mjs carries them).
    const auto shelf = loudness::DesignKWeightingHighShelf(48000.0);
    const auto highPass = loudness::DesignKWeightingHighPass(48000.0);
    const auto close = [](double a, double b) { return std::abs(a - b) < 1.0e-8; };

    Check(close(shelf.b0, 1.53512485958697) && close(shelf.b1, -2.69169618940638) &&
              close(shelf.b2, 1.19839281085285) && close(shelf.a1, -1.69065929318241) &&
              close(shelf.a2, 0.73248077421585),
          "the pre-filter shelf is the published one");
    // The RLB numerator is 1, -2, 1 in the table, left unnormalised (see DesignKWeightingHighPass).
    Check(highPass.b0 == 1.0 && highPass.b1 == -2.0 && highPass.b2 == 1.0 && close(highPass.a1, -1.99004745483398) &&
              close(highPass.a2, 0.99007225036621),
          "the RLB high-pass is the published one");
}

void TestSineReadsItsLevel()
{
    std::cout << "\n997 Hz sine at -20 dBFS\n";

    for (const double sr : {44100.0, 48000.0, 96000.0})
    {
        const auto frames = static_cast<std::size_t>(sr * 5.0);
        const auto sine = Make(frames, [sr](double n) { return Tone(n, 997.0, 0.1, sr); });
        const std::vector<float> silence(frames, 0.0f);
        const std::string rate = Num(sr / 1000.0, 1) + " kHz";

        const auto stereo = loudness::MeasureIntegratedLoudness(sine, sine, sr);
        Check(Near(stereo.integratedLufs, -20.0, 0.02), "both channels read -20.0 LUFS at " + rate,
              Show(stereo.integratedLufs));
        Check(Near(stereo.peakDbfs, -20.0, 0.01), "its sample peak is -20 dBFS at " + rate, Show(stereo.peakDbfs));
        Check(Near(stereo.rmsDbfs, -23.0103, 0.01), "its RMS is -23.0 dBFS at " + rate, Show(stereo.rmsDbfs));

        const auto leftOnly = loudness::MeasureIntegratedLoudness(sine, silence, sr);
        Check(Near(leftOnly.integratedLufs, -23.0103, 0.02), "one channel reads -23.0 LUFS at " + rate,
              Show(leftOnly.integratedLufs));

        const auto mono = loudness::MeasureIntegratedLoudness(sine.data(), nullptr, frames, sr);
        Check(Near(mono.integratedLufs, *stereo.integratedLufs, 1.0e-9),
              "a mono buffer reads as the same signal on both channels at " + rate, Show(mono.integratedLufs));
    }
}

void TestMatchesThePipelineReference()
{
    std::cout << "\nAgainst loudness.mjs (content/preset-packs/scripts/lib)\n";
    constexpr double kTolerance = 0.005;
    const auto frames = [](double seconds) { return static_cast<std::size_t>(seconds * kSr); };

    const auto sine = Make(frames(5.0), [](double n) { return Tone(n, 997.0, 0.1, kSr); });
    const std::vector<float> silence(frames(5.0), 0.0f);
    const auto programL = Make(frames(8.0), ProgramLeft);
    const auto programR = Make(frames(8.0), ProgramRight);
    const auto burst = Make(frames(6.0), Burst);

    const struct
    {
        const char* name;
        loudness::LevelMeasurement measured;
        double reference;
    } cases[] = {
        {"sine997Stereo", loudness::MeasureIntegratedLoudness(sine, sine, kSr), -19.999974},
        {"sine997LeftOnly", loudness::MeasureIntegratedLoudness(sine, silence, kSr), -23.010274},
        {"program", loudness::MeasureIntegratedLoudness(programL, programR, kSr), -11.427451},
        {"programMono", loudness::MeasureIntegratedLoudness(programL.data(), nullptr, programL.size(), kSr), -9.872485},
        {"burst", loudness::MeasureIntegratedLoudness(burst.data(), nullptr, burst.size(), kSr), -3.070977},
    };

    for (const auto& c : cases)
    {
        Check(Near(c.measured.integratedLufs, c.reference, kTolerance),
              std::string(c.name) + " matches the reference " + Num(c.reference), Show(c.measured.integratedLufs));
    }
}

void TestPiecesAndSilence()
{
    std::cout << "\nStreaming and silence\n";
    const auto left = Make(static_cast<std::size_t>(kSr * 8.0), ProgramLeft);
    const auto right = Make(static_cast<std::size_t>(kSr * 8.0), ProgramRight);
    const auto whole = loudness::MeasureIntegratedLoudness(left, right, kSr);

    loudness::IntegratedLoudnessMeter meter(kSr);
    std::size_t at = 0;
    std::size_t piece = 1;

    while (at < left.size())
    {
        const std::size_t count = std::min(piece, left.size() - at);
        meter.Push(left.data() + at, right.data() + at, count);
        at += count;
        piece = (piece * 7 + 3) % 1500 + 1; // ragged pieces, none lined up with a block
    }

    const auto pieces = meter.Result();
    Check(whole.integratedLufs && pieces.integratedLufs &&
              std::abs(*whole.integratedLufs - *pieces.integratedLufs) < 1.0e-9 && whole.frames == pieces.frames,
          "pushing in ragged pieces gives what pushing it whole does",
          Show(whole.integratedLufs) + " vs " + Show(pieces.integratedLufs));
    Check(std::abs(whole.DurationSeconds() - 8.0) < 1.0e-9, "the duration is the frames over the rate");

    const std::vector<float> silence(static_cast<std::size_t>(kSr * 2.0), 0.0f);
    const auto quiet = loudness::MeasureIntegratedLoudness(silence, silence, kSr);
    Check(!quiet.integratedLufs && !quiet.peakDbfs && !quiet.rmsDbfs, "silence has no loudness, peak or RMS");

    const auto tooShort = loudness::MeasureIntegratedLoudness(left.data(), right.data(), 1000, kSr);
    Check(!tooShort.integratedLufs && tooShort.peakDbfs.has_value(),
          "under one 400 ms block has a peak but no integrated loudness");

    Check(!loudness::GatedLoudness(nullptr, 0), "no blocks, no loudness");
    const double belowGate[] = {loudness::LufsToMeanSquare(-75.0), loudness::LufsToMeanSquare(-80.0)};
    Check(!loudness::GatedLoudness(belowGate, 2), "blocks all under the absolute gate have no loudness");
}

/// The analyzer's integrated reading after `left`/`right` played through it in 512-sample blocks.
std::optional<double> AnalyzerIntegrated(const std::vector<float>& left, const std::vector<float>* right)
{
    InputAnalyzerEffect analyzer;
    constexpr int kBlock = 512;
    analyzer.Prepare(kSr, kBlock);
    std::vector<float> outL(kBlock), outR(kBlock);
    std::vector<float> inL(kBlock), inR(kBlock);

    for (std::size_t at = 0; at < left.size(); at += kBlock)
    {
        const int count = static_cast<int>(std::min<std::size_t>(kBlock, left.size() - at));
        std::copy_n(left.data() + at, count, inL.data());
        std::copy_n(right ? right->data() + at : left.data() + at, count, inR.data());
        float* inputs[2] = {inL.data(), right ? inR.data() : nullptr};
        float* outputs[2] = {outL.data(), outR.data()};
        analyzer.Process(inputs, outputs, count);
    }

    return analyzer.GetTelemetrySnapshot().integratedLufs;
}

void TestAnalyzerReadsThroughTheHelper()
{
    std::cout << "\nThe Signal Analyzer\n";
    const auto left = Make(static_cast<std::size_t>(kSr * 8.0), ProgramLeft);
    const auto right = Make(static_cast<std::size_t>(kSr * 8.0), ProgramRight);
    const auto burst = Make(static_cast<std::size_t>(kSr * 6.0), Burst);

    const auto stereo = AnalyzerIntegrated(left, &right);
    const auto leftAlone = AnalyzerIntegrated(burst, nullptr);
    std::cout << "  analyzer program " << (stereo ? Num(*stereo, 12) : "none") << ", burst (left only) "
              << (leftAlone ? Num(*leftAlone, 12) : "none") << "\n";

    // What the analyzer read with its own copy of the filter and gating, before the helper, moved
    // by exactly the gain its normalised high-pass left out: 20 log10(a0). The move itself was
    // first checked against these readings to the last digit with that normalisation kept; the
    // gain is the one deliberate change (see DesignKWeightingHighPass).
    const double K = std::tan(kPi * 38.13547087613982 / kSr);
    const double correction = 20.0 * std::log10(1.0 + K / 0.5003270373253953 + K * K);
    Check(Near(stereo, kAnalyzerProgramBefore + correction, 1.0e-9),
          "the stereo reading is the old one plus the RLB gain correction (" + Num(correction, 4) + " LU)",
          Show(stereo) + " vs " + Num(kAnalyzerProgramBefore, 10));
    Check(Near(leftAlone, kAnalyzerBurstBefore + correction, 1.0e-9), "the one-channel reading likewise",
          Show(leftAlone) + " vs " + Num(kAnalyzerBurstBefore, 10));

    // The live reading and the offline one are the same measurement. The analyzer reads a missing
    // right channel as silent, so its one-channel reading is the offline one with a silent side.
    const auto offline = loudness::MeasureIntegratedLoudness(left, right, kSr);
    const std::vector<float> silence(burst.size(), 0.0f);
    const auto offlineLeftAlone = loudness::MeasureIntegratedLoudness(burst, silence, kSr);
    Check(Near(stereo, *offline.integratedLufs, 1.0e-6), "the analyzer agrees with the offline meter",
          Show(stereo) + " vs " + Show(offline.integratedLufs));
    Check(Near(leftAlone, *offlineLeftAlone.integratedLufs, 1.0e-6), "and on one channel",
          Show(leftAlone) + " vs " + Show(offlineLeftAlone.integratedLufs));
}
} // namespace

int main()
{
    TestKWeightingMatchesTheStandardTable();
    TestSineReadsItsLevel();
    TestMatchesThePipelineReference();
    TestPiecesAndSilence();
    TestAnalyzerReadsThroughTheHelper();

    std::cout << "\nLoudness tests: " << (gFailures == 0 ? "all passed" : std::to_string(gFailures) + " failed")
              << "\n";
    return gFailures == 0 ? 0 : 1;
}

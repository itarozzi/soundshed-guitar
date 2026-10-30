/**
 * @file PitchTrackingEqEffectTests.cpp
 * @brief Tests for the pitch-tracking EQ.
 *
 * These measure what the effect claims to do:
 *   - registration, the parameter table and the factory presets
 *   - before a first note it is flat, sample for sample
 *   - a band lands on the same harmonic whatever note is played, and leaves its neighbours
 *   - the low-pass is key-tracked: the same harmonic is cut by the same amount on any note
 *   - the bands hold the last note through silence, and glide to a new one
 *   - a band pushed past the top of the band fades out rather than piling up there
 *   - on the demo DI guitar, every preset follows the riff without adding clicks or zipper noise
 *   - the mono path matches the stereo one, it recovers from a non-finite input, it stays finite
 *     at low sample rates, and prints what a block costs
 */

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <iostream>
#include <limits>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "DemoAudio.h"
#include "dsp/BiquadDesign.h"
#include "dsp/EffectGuids.h"
#include "dsp/EffectRegistry.h"
#include "dsp/FiniteCheck.h"
#include "dsp/effects/PitchTrackingEqEffect.h"

namespace
{
constexpr double kSampleRate = 48000.0;
constexpr int kBlockSize = 256;
constexpr double kPi = 3.14159265358979323846;
constexpr int kHarmonics = 10;

using guitarfx::PitchTrackingEqEffect;
using Params = std::vector<std::pair<std::string, double>>;

int gFailures = 0;
int gChecks = 0;

void Check(bool condition, const std::string& what, const std::string& detail = "")
{
    ++gChecks;

    if (!condition)
    {
        ++gFailures;
    }

    std::cout << (condition ? "  [PASS] " : "  [FAIL] ") << what;

    if (!detail.empty())
    {
        std::cout << "  (" << detail << ")";
    }

    std::cout << std::endl;
}

std::string Num(double v, int precision = 2)
{
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), "%.*f", precision, v);
    return std::string(buffer);
}

double ToDb(double ratio)
{
    return 20.0 * std::log10(std::max(ratio, 1.0e-12));
}

/// Every band flat and the low-pass off, so a test turns on only what it measures.
const Params kFlat = {{"gain1", 0.0}, {"gain2", 0.0}, {"gain3", 0.0}, {"lowPass", 0.0}};

std::unique_ptr<PitchTrackingEqEffect> MakeEq(const Params& params, double sampleRate = kSampleRate)
{
    auto eq = std::make_unique<PitchTrackingEqEffect>();
    eq->Prepare(sampleRate, kBlockSize);

    for (const auto& [key, value] : kFlat)
    {
        eq->SetParam(key, value);
    }

    for (const auto& [key, value] : params)
    {
        eq->SetParam(key, value);
    }

    return eq;
}

/// A note with its first kHarmonics harmonics at equal level, each with its own phase, so the
/// tracker has a guitar-like tone to follow and every harmonic can be measured on its own.
std::vector<float> Note(double f0, int samples, double sampleRate = kSampleRate, double amplitude = 0.05)
{
    std::vector<float> x(static_cast<std::size_t>(samples));

    for (int i = 0; i < samples; ++i)
    {
        double sum = 0.0;

        for (int h = 1; h <= kHarmonics; ++h)
        {
            if (h * f0 < 0.45 * sampleRate)
            {
                sum += std::sin(2.0 * kPi * h * f0 * i / sampleRate + 0.7 * h);
            }
        }

        x[static_cast<std::size_t>(i)] = static_cast<float>(amplitude * sum);
    }

    return x;
}

/// Runs `input` through in blocks; `right` null runs the mono path.
std::vector<float> Run(PitchTrackingEqEffect& eq, const std::vector<float>& input,
                       std::vector<float>* rightOut = nullptr, bool stereo = true)
{
    std::vector<float> outL(input.size());
    std::vector<float> outR(input.size());

    for (std::size_t start = 0; start < input.size(); start += kBlockSize)
    {
        const int n = static_cast<int>(std::min<std::size_t>(kBlockSize, input.size() - start));
        float* in[2] = {const_cast<float*>(input.data() + start),
                        stereo ? const_cast<float*>(input.data() + start) : nullptr};
        float* out[2] = {outL.data() + start, outR.data() + start};
        eq.Process(in, out, n);
    }

    if (rightOut)
    {
        *rightOut = std::move(outR);
    }

    return outL;
}

/// The amplitude of `hz` in the last `length` samples of `x` (a single-bin DFT, Hann-windowed).
double Amplitude(const std::vector<float>& x, double hz, int length, double sampleRate = kSampleRate)
{
    const std::size_t first = x.size() - static_cast<std::size_t>(length);
    double re = 0.0;
    double im = 0.0;
    double windowSum = 0.0;

    for (int i = 0; i < length; ++i)
    {
        const double w = 0.5 - 0.5 * std::cos(2.0 * kPi * i / (length - 1));
        const double phase = 2.0 * kPi * hz * i / sampleRate;
        re += w * x[first + static_cast<std::size_t>(i)] * std::cos(phase);
        im += w * x[first + static_cast<std::size_t>(i)] * std::sin(phase);
        windowSum += w;
    }

    return 2.0 * std::sqrt(re * re + im * im) / windowSum;
}

/// Each harmonic's change in dB, from input to output, over the last half second.
std::vector<double> HarmonicGainsDb(const std::vector<float>& in, const std::vector<float>& out, double f0)
{
    constexpr int kLength = 24000;
    std::vector<double> gains(kHarmonics + 1, 0.0);

    for (int h = 1; h <= kHarmonics; ++h)
    {
        gains[static_cast<std::size_t>(h)] = ToDb(Amplitude(out, h * f0, kLength) / Amplitude(in, h * f0, kLength));
    }

    return gains;
}

void TestRegistration()
{
    std::cout << "\nRegistration and parameters" << std::endl;
    auto& registry = guitarfx::EffectRegistry::Instance();
    const auto info = registry.GetTypeInfo(guitarfx::EffectGuids::kEqPitchTracking);

    Check(info.has_value(), "registered under its UUID");
    Check(registry.Resolve("eq_pitch_tracking") == guitarfx::EffectGuids::kEqPitchTracking,
          "the eq_pitch_tracking alias resolves to it");
    Check(registry.Create("eq_pitch_tracking")->GetType() == "eq_pitch_tracking",
          "the registry creates one from the alias");

    if (!info)
    {
        return;
    }

    Check(info->category == "eq" && !info->requiresResource, "an EQ that needs no resource");
    Check(info->parameters.size() == guitarfx::pitch_eq::kParamCount, "declares every parameter");
    Check(info->presets.size() == 6 && info->presets.front().isDefault, "six presets, the default first");

    const auto* cutoff = registry.FindParameter(guitarfx::EffectGuids::kEqPitchTracking, "cutoff");
    Check(cutoff && cutoff->visibleWhenKey == "lowPass", "Cutoff shows only with the low-pass on");

    PitchTrackingEqEffect eq;
    eq.SetParam("gain1", 99.0);
    eq.SetParam("harmonic2", 3.3);
    eq.SetParam("width3", std::nan(""));
    Check(eq.GetParam("gain1") == 18.0, "Gain clamps to its range");
    Check(eq.GetParam("harmonic2") == 3.5, "Harmonic snaps to half-harmonic steps");
    Check(eq.GetParam("width3") == 0.5, "a non-finite value is ignored");
}

void TestFlatBeforeANote()
{
    std::cout << "\nFlat until a note is found" << std::endl;
    auto eq = MakeEq({{"gain1", 12.0}, {"gain2", -12.0}, {"lowPass", 1.0}, {"cutoff", 2.0}});

    // The tracker needs a window and two agreeing detections before it accepts a first note.
    const auto input = Note(110.0, 480);
    const auto output = Run(*eq, input);
    Check(output == input, "the first 10 ms of a note pass sample for sample");
    Check(eq->GetParam("fundamental") == 0.0 && eq->GetParam("bandFrequency1") == 0.0,
          "no fundamental and no band frequency yet");
}

void TestBandFollowsTheNote()
{
    std::cout << "\nA band stays on its harmonic" << std::endl;

    for (const double f0 : {110.0, 196.0, 330.0})
    {
        auto eq = MakeEq({{"harmonic1", 3.0}, {"gain1", 12.0}, {"width1", 0.5}});
        const auto input = Note(f0, 72000);
        const auto output = Run(*eq, input);
        const auto gains = HarmonicGainsDb(input, output, f0);
        const std::string at = " at " + Num(f0, 0) + " Hz";

        Check(std::abs(eq->GetParam("fundamental") - f0) < 0.01 * f0, "follows the note" + at,
              Num(eq->GetParam("fundamental")) + " Hz");
        Check(std::abs(eq->GetParam("bandFrequency1") - 3.0 * f0) < 0.03 * f0, "band 1 sits on the 3rd harmonic" + at);
        Check(std::abs(gains[3] - 12.0) < 0.5, "the 3rd harmonic is lifted 12 dB" + at, Num(gains[3]) + " dB");
        Check(std::abs(gains[2]) < 3.0 && std::abs(gains[4]) < 3.0, "its neighbours move under 3 dB" + at,
              Num(gains[2]) + " / " + Num(gains[4]) + " dB");
        Check(std::abs(gains[1]) < 0.5 && std::abs(gains[6]) < 0.5, "the 1st and 6th are untouched" + at,
              Num(gains[1]) + " / " + Num(gains[6]) + " dB");
    }

    // A cut works the same way, on all three bands at once.
    const double f0 = 147.0;
    auto eq = MakeEq({{"harmonic1", 2.0},
                      {"gain1", -12.0},
                      {"harmonic2", 4.0},
                      {"gain2", -12.0},
                      {"harmonic3", 6.0},
                      {"gain3", -12.0},
                      {"width1", 0.3},
                      {"width2", 0.3},
                      {"width3", 0.3}});
    const auto input = Note(f0, 72000);
    const auto gains = HarmonicGainsDb(input, Run(*eq, input), f0);
    Check(gains[2] < -10.0 && gains[4] < -10.0 && gains[6] < -10.0, "three bands cut the even harmonics",
          Num(gains[2]) + " / " + Num(gains[4]) + " / " + Num(gains[6]) + " dB");
    Check(std::abs(gains[3]) < 1.5 && std::abs(gains[5]) < 1.5, "and leave the odd ones",
          Num(gains[3]) + " / " + Num(gains[5]) + " dB");
}

void TestKeyTrackedLowPass()
{
    std::cout << "\nThe low-pass is key-tracked" << std::endl;
    std::vector<double> eighth;

    for (const double f0 : {98.0, 392.0})
    {
        auto eq = MakeEq({{"lowPass", 1.0}, {"cutoff", 4.0}});
        const auto input = Note(f0, 72000);
        const auto gains = HarmonicGainsDb(input, Run(*eq, input), f0);
        eighth.push_back(gains[8]);
        Check(std::abs(gains[1]) < 0.3, "the fundamental passes at " + Num(f0, 0) + " Hz", Num(gains[1]) + " dB");
        Check(std::abs(eq->GetParam("lowPassFrequency") - 4.0 * f0) < 0.04 * f0, "cutoff at 4 x f0");
    }

    // Two octaves apart, the 8th harmonic (an octave above the cutoff) loses the same amount.
    Check(eighth[0] < -10.0 && std::abs(eighth[0] - eighth[1]) < 1.0, "the 8th harmonic is cut alike on both notes",
          Num(eighth[0]) + " / " + Num(eighth[1]) + " dB");
}

void TestHoldAndGlide()
{
    std::cout << "\nHolds through silence, glides to a new note" << std::endl;
    auto eq = MakeEq({{"gain1", 6.0}, {"glide", 50.0}});
    Run(*eq, Note(110.0, 24000));
    Run(*eq, std::vector<float>(48000, 0.0f));
    Check(std::abs(eq->GetParam("fundamental") - 110.0) < 1.5, "a second of silence keeps the last note",
          Num(eq->GetParam("fundamental")) + " Hz");

    Run(*eq, Note(220.0, 4800));
    const double during = eq->GetParam("fundamental");
    Run(*eq, Note(220.0, 24000));
    const double after = eq->GetParam("fundamental");
    Check(after > 215.0 && after < 225.0, "settles on the new note", Num(after) + " Hz");
    Check(during > 110.0 && during < after, "and glides there rather than jumping", Num(during) + " Hz at 100 ms");
}

/// The RMS of `x` above 12 kHz (an 8th-order Butterworth high-pass), from `from` on. A DI guitar
/// has little up there, so clicks or zipper noise from moving filters show plainly.
double HighBandRms(const std::vector<float>& x, std::size_t from)
{
    std::array<guitarfx::BiquadCoefficients, 4> sections;
    std::array<guitarfx::biquad::State, 4> states{};

    for (int s = 0; s < 4; ++s)
    {
        sections[static_cast<std::size_t>(s)] =
            guitarfx::biquad::HighPass(12000.0, guitarfx::biquad::ButterworthSectionQ(8, s), kSampleRate);
    }

    double sum = 0.0;

    for (std::size_t i = 0; i < x.size(); ++i)
    {
        double y = x[i];

        for (std::size_t s = 0; s < sections.size(); ++s)
        {
            y = states[s].Process(sections[s], y);
        }

        if (i >= from)
        {
            sum += y * y;
        }
    }

    return std::sqrt(sum / static_cast<double>(x.size() - from));
}

void TestDemoGuitar()
{
    std::cout << "\nThe demo DI guitar" << std::endl;
    double sampleRate = 0.0;
    auto guitar = guitarfx::test::LoadDemoClipMono(guitarfx::test::kDemoDiGuitar, sampleRate);

    if (sampleRate != kSampleRate || guitar.size() < static_cast<std::size_t>(30 * kSampleRate))
    {
        std::cout << "  [SKIP] the demo DI did not load at 48 kHz" << std::endl;
        return;
    }

    // 20 s of the riff, from 10 s in.
    guitar = std::vector<float>(guitar.begin() + static_cast<std::ptrdiff_t>(10 * kSampleRate),
                                guitar.begin() + static_cast<std::ptrdiff_t>(30 * kSampleRate));
    const std::size_t settle = static_cast<std::size_t>(kSampleRate);
    const double inputHigh = HighBandRms(guitar, settle);
    const auto info = guitarfx::EffectRegistry::Instance().GetTypeInfo(guitarfx::EffectGuids::kEqPitchTracking);

    for (const auto& preset : info->presets)
    {
        PitchTrackingEqEffect eq;
        eq.Prepare(kSampleRate, kBlockSize);

        for (const auto& [key, value] : preset.parameters)
        {
            eq.SetParam(key, value);
        }

        // Block by block, counting how often the fundamental it follows moves by a semitone or more.
        std::vector<float> output(guitar.size());
        double previous = 0.0;
        int noteChanges = 0;
        int inRange = 0;
        int blocks = 0;

        for (std::size_t start = 0; start < guitar.size(); start += kBlockSize)
        {
            const int n = static_cast<int>(std::min<std::size_t>(kBlockSize, guitar.size() - start));
            float* in[2] = {guitar.data() + start, nullptr};
            float* out[2] = {output.data() + start, nullptr};
            eq.Process(in, out, n);

            const double f0 = eq.GetParam("fundamental");
            ++blocks;
            inRange += (f0 >= 70.0 && f0 <= 1500.0) ? 1 : 0;

            if (previous > 0.0 && f0 > 0.0 && std::abs(12.0 * std::log2(f0 / previous)) >= 1.0)
            {
                ++noteChanges;
            }

            previous = f0;
        }

        const double highDb = ToDb(HighBandRms(output, settle) / inputHigh);
        Check(highDb < 3.0, preset.displayName + ": nothing added above 12 kHz",
              Num(highDb) + " dB; " + std::to_string(noteChanges) + " note changes, " +
                  Num(100.0 * inRange / blocks, 0) + "% of blocks on a guitar pitch");
    }
}

void TestTopFade()
{
    std::cout << "\nBands past the top fade out" << std::endl;
    // 16 x 1.4 kHz is 22.4 kHz, past 0.45 of 48 kHz: the band must do nothing.
    auto eq = MakeEq({{"harmonic1", 16.0}, {"gain1", 18.0}, {"width1", 2.0}});
    const auto input = Note(1400.0, 48000);
    const auto output = Run(*eq, input);
    double peakIn = 0.0;
    double peakOut = 0.0;

    for (std::size_t i = 24000; i < input.size(); ++i)
    {
        peakIn = std::max(peakIn, static_cast<double>(std::abs(input[i])));
        peakOut = std::max(peakOut, static_cast<double>(std::abs(output[i])));
    }

    Check(std::abs(ToDb(peakOut / peakIn)) < 0.5, "an out-of-range band leaves the level alone",
          Num(ToDb(peakOut / peakIn)) + " dB");
}

void TestMonoMatchesStereo()
{
    std::cout << "\nMono and stereo" << std::endl;
    const auto input = Note(165.0, 24000);
    auto stereo = MakeEq({{"harmonic1", 2.0}, {"gain1", 9.0}, {"lowPass", 1.0}});
    auto mono = MakeEq({{"harmonic1", 2.0}, {"gain1", 9.0}, {"lowPass", 1.0}});
    std::vector<float> right;
    std::vector<float> monoRight;
    const auto left = Run(*stereo, input, &right);
    const auto monoLeft = Run(*mono, input, &monoRight, false);
    Check(left == monoLeft, "the mono path matches stereo's left sample for sample");
    Check(left == right && monoLeft == monoRight, "both channels match on a mono input, and mono fills the right");
}

void TestNonFiniteRecovery()
{
    std::cout << "\nRecovers from a non-finite input" << std::endl;
    auto eq = MakeEq({{"gain1", 12.0}, {"lowPass", 1.0}});
    Run(*eq, Note(110.0, 24000));
    auto poisoned = Note(110.0, kBlockSize);
    poisoned[10] = std::numeric_limits<float>::quiet_NaN();
    Run(*eq, poisoned);
    const auto after = Run(*eq, Note(110.0, 4800));
    const bool finite =
        std::all_of(after.begin() + kBlockSize, after.end(), [](float v) { return guitarfx::IsFinite(v); });
    Check(finite, "output is finite again after the next block");
}

void TestLowSampleRates()
{
    std::cout << "\nLow sample rates" << std::endl;

    for (const double rate : {8000.0, 22050.0})
    {
        auto eq = MakeEq({{"harmonic1", 16.0}, {"gain1", 18.0}, {"lowPass", 1.0}, {"cutoff", 32.0}}, rate);
        const auto output = Run(*eq, Note(330.0, static_cast<int>(rate), rate));
        const bool finite = std::all_of(output.begin(), output.end(), [](float v) { return guitarfx::IsFinite(v); });
        const float peak = std::abs(*std::max_element(output.begin(), output.end(),
                                                      [](float a, float b) { return std::abs(a) < std::abs(b); }));
        Check(finite && peak < 4.0f, "finite and bounded at " + Num(rate, 0) + " Hz", "peak " + Num(peak, 3));
    }
}

void ReportCost()
{
    auto eq = MakeEq({{"gain1", 6.0}, {"gain2", 4.0}, {"gain3", -4.0}, {"lowPass", 1.0}});
    const auto input = Note(110.0, 48000 * 4);
    const auto start = std::chrono::steady_clock::now();
    Run(*eq, input);
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    const double perBlockUs = seconds * 1.0e6 / (static_cast<double>(input.size()) / 64.0);
    std::cout << "\nCost: " << Num(perBlockUs, 2) << " us per 64-sample stereo block at 48 kHz" << std::endl;
}
} // namespace

int main()
{
    std::cout << "=== PitchTrackingEqEffectTests ===" << std::endl;
    guitarfx::RegisterPitchTrackingEqEffect();

    TestRegistration();
    TestFlatBeforeANote();
    TestBandFollowsTheNote();
    TestKeyTrackedLowPass();
    TestHoldAndGlide();
    TestDemoGuitar();
    TestTopFade();
    TestMonoMatchesStereo();
    TestNonFiniteRecovery();
    TestLowSampleRates();
    ReportCost();

    std::cout << "\n" << (gChecks - gFailures) << "/" << gChecks << " checks passed" << std::endl;
    return gFailures == 0 ? 0 : 1;
}

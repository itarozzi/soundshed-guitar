/**
 * @file MultibandDistortionEffectTests.cpp
 * @brief The Multi-band Distortion: the oversampled four-band split, the per-band clippers and
 *        the band meters its node panel draws.
 *
 *   - clean bands add back to flat, at the default crossovers and moved ones, at 48 and 96 kHz
 *   - a low tone stays clean with only the top band driven; driving its own band adds harmonics
 *   - Drive is level-matched for every Character
 *   - a hard-clipped high tone's fold-backs stay well under it
 *   - a Character change mid-note does not step the output
 *   - Mix 0 is clean; Level and Output move the level by what they say
 *   - the band meters report the crossovers in use, the knee and how far past it a band is driven
 *   - the mono path matches the stereo one; a non-finite sample does not latch
 */

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <iostream>
#include <limits>
#include <memory>
#include <string>
#include <vector>

#include "dsp/effects/BuiltinEffects.h"
#include "dsp/effects/MultibandDistortionEffect.h"

namespace
{
using namespace guitarfx;

constexpr double kSampleRate = 48000.0;
constexpr int kBlockSize = 128;
constexpr double kPi = 3.14159265358979323846;
constexpr int kBands = MultibandDistortionEffect::kBands;
constexpr double kHard = 3.0;

/// A sine whose RMS is the default nominal operating level (-18 dBFS) peaks 3 dB above it.
constexpr double kNominalPeakDb = -18.0 + 3.0103;

int gFailures = 0;
int gChecks = 0;

void Check(bool condition, const std::string& what, const std::string& detail = "")
{
    ++gChecks;
    gFailures += condition ? 0 : 1;
    std::cout << (condition ? "  [PASS] " : "  [FAIL] ") << what;

    if (!detail.empty())
    {
        std::cout << "  (" << detail << ")";
    }

    std::cout << std::endl;
}

std::string Num(double value, int precision = 1)
{
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), "%.*f", precision, value);
    return buffer;
}

double Amplitude(double db)
{
    return std::pow(10.0, db / 20.0);
}

std::vector<float> Tone(double peakDb, double seconds, double frequency, double sampleRate = kSampleRate)
{
    std::vector<float> signal(static_cast<std::size_t>(sampleRate * seconds));

    for (std::size_t i = 0; i < signal.size(); ++i)
    {
        signal[i] = static_cast<float>(Amplitude(peakDb) *
                                       std::sin(2.0 * kPi * frequency * static_cast<double>(i) / sampleRate));
    }

    return signal;
}

/// RMS in dB of `signal` from `from` (seconds) to the end.
double RmsDb(const std::vector<float>& signal, double from = 0.0, double sampleRate = kSampleRate)
{
    const auto start = static_cast<std::size_t>(from * sampleRate);
    double sum = 0.0;

    for (std::size_t i = start; i < signal.size(); ++i)
    {
        sum += static_cast<double>(signal[i]) * signal[i];
    }

    const double mean = sum / static_cast<double>(std::max<std::size_t>(1, signal.size() - start));
    return 10.0 * std::log10(std::max(mean, 1.0e-30));
}

/// The peak level in dB of the `hz` component of `signal` from `from` (seconds) to the end, by
/// correlation over a whole number of its cycles.
double ToneDb(const std::vector<float>& signal, double hz, double from)
{
    const auto start = static_cast<std::size_t>(from * kSampleRate);
    const auto cycles = static_cast<std::size_t>(std::floor((signal.size() - start) * hz / kSampleRate));
    const auto count = static_cast<std::size_t>(std::round(cycles * kSampleRate / hz));
    double re = 0.0;
    double im = 0.0;

    for (std::size_t i = 0; i < count; ++i)
    {
        const double phase = 2.0 * kPi * hz * static_cast<double>(start + i) / kSampleRate;
        re += signal[start + i] * std::cos(phase);
        im += signal[start + i] * std::sin(phase);
    }

    const double amplitude = 2.0 * std::sqrt(re * re + im * im) / static_cast<double>(std::max<std::size_t>(count, 1));
    return 20.0 * std::log10(std::max(amplitude, 1.0e-15));
}

/// Every band's drive and character at once.
void SetBands(MultibandDistortionEffect& effect, double driveDb, double character)
{
    for (int band = 1; band <= kBands; ++band)
    {
        effect.SetParam("drive" + std::to_string(band), driveDb);
        effect.SetParam("character" + std::to_string(band), character);
    }
}

std::unique_ptr<MultibandDistortionEffect> MakeDistortion(double driveDb, double character,
                                                          double sampleRate = kSampleRate)
{
    auto effect = std::make_unique<MultibandDistortionEffect>();
    SetBands(*effect, driveDb, character);
    effect->Prepare(sampleRate, kBlockSize);
    return effect;
}

/// Block by block, the same signal on both sides (or mono). Returns the left output.
std::vector<float> Run(MultibandDistortionEffect& effect, const std::vector<float>& input, bool mono = false)
{
    std::vector<float> out(input.size(), 0.0f);
    std::vector<float> inL(kBlockSize);
    std::vector<float> inR(kBlockSize);
    std::vector<float> outL(kBlockSize);
    std::vector<float> outR(kBlockSize);

    for (std::size_t offset = 0; offset < input.size(); offset += kBlockSize)
    {
        const auto count = static_cast<int>(std::min<std::size_t>(kBlockSize, input.size() - offset));
        const auto at = static_cast<std::ptrdiff_t>(offset);
        std::copy_n(input.begin() + at, count, inL.begin());
        std::copy_n(input.begin() + at, count, inR.begin());

        if (mono)
        {
            effect.ProcessMono(inL.data(), outL.data(), count);
        }
        else
        {
            float* inputs[2] = {inL.data(), inR.data()};
            float* outputs[2] = {outL.data(), outR.data()};
            effect.Process(inputs, outputs, count);
        }

        std::copy_n(outL.begin(), count, out.begin() + at);
    }

    return out;
}

/// The worst level change over quiet tones across `frequencies`, with every band clean.
template <typename Setup>
double WorstFlatness(Setup setup, std::initializer_list<double> frequencies, std::string& where,
                     double sampleRate = kSampleRate)
{
    double worst = 0.0;

    for (const double hz : frequencies)
    {
        auto effect = MakeDistortion(0.0, kHard, sampleRate);
        setup(*effect);
        const auto tone = Tone(-40.0, 0.5, hz, sampleRate);
        const double change = RmsDb(Run(*effect, tone), 0.25, sampleRate) - RmsDb(tone, 0.25, sampleRate);

        if (std::abs(change) > std::abs(worst))
        {
            worst = change;
            where = Num(hz, 0) + " Hz";
        }
    }

    return worst;
}

void TestCleanIsFlat()
{
    std::cout << "\n-- clean bands add back to flat --" << std::endl;

    const auto none = [](MultibandDistortionEffect&) {};
    const auto audioBand = {40.0, 100.0, 200.0, 350.0, 800.0, 1500.0, 3000.0, 5000.0, 8000.0};
    std::string where;
    double worst = WorstFlatness(none, audioBand, where);
    Check(std::abs(worst) < 0.1, "default crossovers: 40 Hz to 8 kHz keep their level",
          "worst " + Num(worst, 3) + " dB at " + where);

    // The clippers' antialiasing averages successive oversampled samples, which rolls off the
    // very top a little: about a third of a dB at 16 kHz.
    worst = WorstFlatness(none, {12000.0, 16000.0}, where);
    Check(worst < 0.0 && worst > -0.5, "12 and 16 kHz roll off by under half a dB",
          "worst " + Num(worst, 3) + " dB at " + where);

    worst = WorstFlatness(
        [](MultibandDistortionEffect& effect) {
            effect.SetParam("crossover1", 90.0);
            effect.SetParam("crossover2", 2000.0);
            effect.SetParam("crossover3", 9000.0);
        },
        audioBand, where);
    Check(std::abs(worst) < 0.1, "moved crossovers: still flat", "worst " + Num(worst, 3) + " dB at " + where);

    worst = WorstFlatness(none, audioBand, where, 96000.0);
    Check(std::abs(worst) < 0.1, "at 96 kHz (2x) too", "worst " + Num(worst, 3) + " dB at " + where);

    // Mix 0 at full drive is the clean all-pass, even at the nominal level.
    auto effect = MakeDistortion(48.0, kHard);
    effect->SetParam("mix", 0.0);
    const auto tone = Tone(kNominalPeakDb, 0.5, 440.0);
    const double change = RmsDb(Run(*effect, tone), 0.25) - RmsDb(tone, 0.25);
    Check(std::abs(change) < 0.1, "Mix 0 at full drive leaves the level as it was", Num(change, 3) + " dB");
}

void TestOversampling()
{
    std::cout << "\n-- clipped near 192 kHz --" << std::endl;

    Check(MakeDistortion(0.0, kHard, 48000.0)->OversamplingFactor() == 4, "4x at 48 kHz");
    Check(MakeDistortion(0.0, kHard, 96000.0)->OversamplingFactor() == 2, "2x at 96 kHz");
    Check(MakeDistortion(0.0, kHard, 192000.0)->OversamplingFactor() == 1, "1x at 192 kHz");
}

void TestBandIsolation()
{
    std::cout << "\n-- each band is driven on its own --" << std::endl;

    // 60 Hz sits under the default 200 Hz crossover, so it is all in the low band. Hard is
    // exactly linear below its knee, 9 dB over a nominal note's peaks.
    const auto low = Tone(kNominalPeakDb, 1.0, 60.0);
    auto effect = MakeDistortion(0.0, kHard);
    effect->SetParam("drive4", 48.0);
    auto out = Run(*effect, low);
    const double cleanThird = ToneDb(out, 180.0, 0.5) - ToneDb(out, 60.0, 0.5);
    Check(cleanThird < -60.0, "only the top band driven: the low tone stays clean",
          "3rd harmonic " + Num(cleanThird) + " dB");

    effect = MakeDistortion(0.0, kHard);
    effect->SetParam("drive1", 30.0);
    out = Run(*effect, low);
    const double drivenThird = ToneDb(out, 180.0, 0.5) - ToneDb(out, 60.0, 0.5);
    Check(drivenThird > -20.0, "its own band driven: harmonics", "3rd harmonic " + Num(drivenThird) + " dB");
}

void TestLevelMatched()
{
    std::cout << "\n-- Drive changes the dirt, not the level --" << std::endl;

    // 400 Hz is in the Low Mid band, and only that band is driven: what leaks of it into the
    // bands either side, 24 dB down, would be driven flat-out too and come up to meet it, as a
    // multi-band clipper's bands all do at high drive.
    const auto tone = Tone(kNominalPeakDb, 0.6, 400.0);

    for (int character = 0; character < multiband_distortion::kCharacterCount; ++character)
    {
        auto clean = MakeDistortion(0.0, character);
        auto driven = MakeDistortion(0.0, character);
        driven->SetParam("drive2", 36.0);
        driven->Prepare(kSampleRate, kBlockSize);
        const double cleanDb = RmsDb(Run(*clean, tone), 0.3);
        const double drivenDb = RmsDb(Run(*driven, tone), 0.3);
        Check(std::abs(drivenDb - cleanDb) < 1.5,
              std::string(multiband_distortion::kCharacterLabels[character]) + ": 36 dB of Drive keeps the level",
              Num(drivenDb - cleanDb, 2) + " dB");
    }
}

void TestAliasing()
{
    std::cout << "\n-- a hard-clipped high tone does not fold back --" << std::endl;

    // 5 kHz into a hard clipper at full drive is near a square wave. Its 9th, 11th and 37th
    // harmonics would fold to 3, 7 and 7 kHz, its 7th to 13 kHz.
    auto effect = MakeDistortion(48.0, kHard);
    const auto out = Run(*effect, Tone(kNominalPeakDb, 0.6, 5000.0));
    const double fundamental = ToneDb(out, 5000.0, 0.2);
    double worst = -200.0;
    std::string where;

    for (const double alias : {3000.0, 7000.0, 13000.0})
    {
        const double level = ToneDb(out, alias, 0.2) - fundamental;

        if (level > worst)
        {
            worst = level;
            where = Num(alias, 0) + " Hz";
        }
    }

    Check(worst < -45.0, "the fold-backs stay 45 dB under the tone", Num(worst) + " dB at " + where);
}

void TestCharacterSwitch()
{
    std::cout << "\n-- a Character change mid-note does not step --" << std::endl;

    const auto tone = Tone(kNominalPeakDb, 0.6, 300.0);
    const auto maxStep = [](const std::vector<float>& signal, std::size_t from, std::size_t to) {
        double worst = 0.0;

        for (std::size_t i = from + 1; i < to; ++i)
        {
            worst = std::max(worst, static_cast<double>(std::abs(signal[i] - signal[i - 1])));
        }

        return worst;
    };

    auto effect = MakeDistortion(24.0, 1.0);
    std::vector<float> first(tone.begin(), tone.begin() + 14400);
    std::vector<float> second(tone.begin() + 14400, tone.end());
    auto out = Run(*effect, first);
    effect->SetParam("character2", kHard);
    const auto after = Run(*effect, second);
    out.insert(out.end(), after.begin(), after.end());

    const double before = maxStep(out, 4800, 14400);
    const double crossing = maxStep(out, 14400, 14400 + 960);
    const double settled = maxStep(out, 14400 + 4800, out.size());
    Check(crossing <= 1.15 * std::max(before, settled), "the crossing moves no faster than either curve",
          Num(crossing, 4) + " against " + Num(before, 4) + " and " + Num(settled, 4));
}

void TestLevelAndOutput()
{
    std::cout << "\n-- Level and Output --" << std::endl;

    const auto low = Tone(-40.0, 0.5, 60.0);
    const double in = RmsDb(low, 0.25);

    auto effect = MakeDistortion(0.0, kHard);
    effect->SetParam("level1", 6.0);
    const double level = RmsDb(Run(*effect, low), 0.25) - in;
    Check(std::abs(level - 6.0) < 0.1, "Level raises its band by what it says", Num(level, 3) + " dB");

    effect = MakeDistortion(0.0, kHard);
    effect->SetParam("level4", 6.0);
    const double other = RmsDb(Run(*effect, low), 0.25) - in;
    Check(std::abs(other) < 0.1, "and not another band", Num(other, 3) + " dB");

    effect = MakeDistortion(0.0, kHard);
    effect->SetParam("output", -6.0);
    const double output = RmsDb(Run(*effect, low), 0.25) - in;
    Check(std::abs(output + 6.0) < 0.1, "Output trims the whole", Num(output, 3) + " dB");
}

void TestBandMeters()
{
    std::cout << "\n-- the band meters --" << std::endl;

    // The low band's knee is 12 dB over the nominal -18 dBFS, less its 30 dB of Drive.
    auto effect = MakeDistortion(0.0, kHard);
    effect->SetParam("drive1", 30.0);
    Run(*effect, Tone(-15.0, 0.5, 60.0));

    std::array<BandMeter, kBands> meters{};
    const int count = effect->ReadBandMeters(meters);
    Check(count == kBands, "four band meters", std::to_string(count));
    Check(std::abs(meters[0].thresholdDb - -36.0) < 0.1, "the low band's knee is at -36 dBFS",
          Num(meters[0].thresholdDb, 2) + " dBFS");
    Check(std::abs(meters[3].thresholdDb - -6.0) < 0.1, "the clean high band's at -6 dBFS",
          Num(meters[3].thresholdDb, 2) + " dBFS");
    Check(std::abs(meters[0].gainReductionDb - 21.0) < 1.0, "the low band is driven 21 dB past it",
          Num(meters[0].gainReductionDb, 2) + " dB");
    Check(meters[3].gainReductionDb < 0.1, "the high band not at all", Num(meters[3].gainReductionDb, 2) + " dB");
    Check(std::abs(meters[0].levelDb - -15.0) < 1.0, "the low band's level is the tone's peak",
          Num(meters[0].levelDb, 2) + " dBFS");
    Check(meters[0].lowHz == 0.0 && meters[0].highHz == 200.0 && meters[1].lowHz == 200.0 &&
              meters[1].highHz == 800.0 && meters[2].highHz == 3000.0 && meters[3].lowHz == 3000.0 &&
              meters[3].highHz == 0.0,
          "the meters carry the crossovers in use");

    effect->ReadBandMeters(meters);
    Check(meters[0].gainReductionDb == 0.0f, "a read starts the next hold", Num(meters[0].gainReductionDb, 2) + " dB");
}

void TestMonoMatchesStereo()
{
    std::cout << "\n-- mono and stereo agree --" << std::endl;

    auto signal = Tone(kNominalPeakDb, 0.5, 60.0);
    const auto high = Tone(-24.0, 0.5, 2000.0);

    for (std::size_t i = 0; i < signal.size(); ++i)
    {
        signal[i] += high[i];
    }

    auto stereo = MakeDistortion(24.0, 1.0);
    auto mono = MakeDistortion(24.0, 1.0);
    const auto a = Run(*stereo, signal);
    const auto b = Run(*mono, signal, true);
    double worst = 0.0;

    for (std::size_t i = 0; i < a.size(); ++i)
    {
        worst = std::max(worst, static_cast<double>(std::abs(a[i] - b[i])));
    }

    Check(worst < 1.0e-6, "the mono path matches the stereo one", Num(worst, 9));
}

void TestNonFiniteDoesNotLatch()
{
    std::cout << "\n-- a non-finite sample does not latch --" << std::endl;

    auto effect = MakeDistortion(24.0, 1.0);
    auto poisoned = Tone(kNominalPeakDb, 0.2, 60.0);
    poisoned[100] = std::numeric_limits<float>::quiet_NaN();
    poisoned[200] = std::numeric_limits<float>::infinity();
    Run(*effect, poisoned);
    const auto after = Run(*effect, Tone(kNominalPeakDb, 0.5, 60.0));
    const bool finite = std::all_of(after.begin(), after.end(), [](float value) { return std::isfinite(value); });
    Check(finite && RmsDb(after, 0.3) > -40.0, "the next notes come out", Num(RmsDb(after, 0.3), 1) + " dB");
}
} // namespace

int main()
{
    std::cout << "=== MultibandDistortionEffectTests ===" << std::endl;
    RegisterAllEffects();

    TestCleanIsFlat();
    TestOversampling();
    TestBandIsolation();
    TestLevelMatched();
    TestAliasing();
    TestCharacterSwitch();
    TestLevelAndOutput();
    TestBandMeters();
    TestMonoMatchesStereo();
    TestNonFiniteDoesNotLatch();

    std::cout << "\n" << (gChecks - gFailures) << "/" << gChecks << " checks passed" << std::endl;
    return gFailures == 0 ? 0 : 1;
}

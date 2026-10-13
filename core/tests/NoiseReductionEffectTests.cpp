/**
 * @file NoiseReductionEffectTests.cpp
 * @brief Tests for the pickup noise reduction.
 *
 * These measure what the effect claims to do, rather than only that it makes sound:
 *   - registration, the parameter table, clamping and the factory presets
 *   - mains hum and its harmonics come out, at 50 and 60 Hz, with Auto picking the right one
 *   - the frequency-locked loop follows mains that is off nominal, buzz harmonics included
 *   - a loud note sitting exactly on a hum harmonic is left alone: learning stops under it
 *   - hiss above the split is reduced by what Hiss asks for, the low band is not touched,
 *     and a loud bright signal passes at full level
 *   - on the real DI take with hum and hiss added, the noise falls and the playing keeps its level
 *   - Hum and Hiss at zero pass the input untouched, and Removed Noise is exactly what was taken
 *   - the mono path matches the stereo one, identical sides stay identical, a NaN is recovered
 *     from, other sample rates work, and it prints what a block costs
 */

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "DemoAudio.h"
#include "dsp/EffectGuids.h"
#include "dsp/EffectProcessor.h"
#include "dsp/EffectRegistry.h"
#include "dsp/FiniteCheck.h"
#include "dsp/effects/NoiseReductionEffect.h"
#include "util/Wav.h"

namespace
{
using guitarfx::NoiseReductionEffect;
using Params = std::vector<std::pair<std::string, double>>;

constexpr double kSampleRate = 48000.0;
constexpr int kBlockSize = 128;
constexpr double kPi = 3.14159265358979323846;

int gFailures = 0;
int gChecks = 0;

void Check(bool condition, const std::string& what, const std::string& detail = "")
{
    ++gChecks;
    std::cout << (condition ? "  [PASS] " : "  [FAIL] ") << what;

    if (!condition)
    {
        ++gFailures;
    }

    if (!detail.empty())
    {
        std::cout << "  (" << detail << ")";
    }

    std::cout << std::endl;
}

std::string Db(double value)
{
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%.1f dB", value);
    return buffer;
}

std::string Fixed(double value, int decimals = 3)
{
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%.*f", decimals, value);
    return buffer;
}

std::unique_ptr<NoiseReductionEffect> Make(double sampleRate = kSampleRate, const Params& params = {})
{
    auto effect = std::make_unique<NoiseReductionEffect>();
    effect->Prepare(sampleRate, kBlockSize);

    for (const auto& [key, value] : params)
    {
        effect->SetParam(key, value);
    }

    return effect;
}

std::vector<float> RunMono(NoiseReductionEffect& effect, const std::vector<float>& input)
{
    std::vector<float> output(input.size());
    std::vector<float> block(kBlockSize);

    for (std::size_t start = 0; start < input.size(); start += kBlockSize)
    {
        const int n = static_cast<int>(std::min<std::size_t>(kBlockSize, input.size() - start));
        std::copy_n(input.data() + start, n, block.data());
        effect.ProcessMono(block.data(), output.data() + start, n);
    }

    return output;
}

/// Pickup hum: the mains fundamental and its harmonics up to `topHz`, odd ones stronger as a
/// single coil's buzz is, each at a fixed scattered phase. `level` is the fundamental's peak.
std::vector<float> Hum(double mainsHz, double level, double topHz, double seconds, double sampleRate)
{
    const auto frames = static_cast<std::size_t>(seconds * sampleRate);
    std::vector<float> out(frames, 0.0f);

    for (int k = 1; k * mainsHz < topHz; ++k)
    {
        const double amplitude = level * ((k % 2) ? 1.0 : 0.5) / std::sqrt(static_cast<double>(k));
        const double phase = 2.0 * kPi * std::fmod(0.618034 * k * k, 1.0);
        const double step = 2.0 * kPi * k * mainsHz / sampleRate;

        for (std::size_t i = 0; i < frames; ++i)
        {
            out[i] += static_cast<float>(amplitude * std::sin(phase + step * static_cast<double>(i)));
        }
    }

    return out;
}

std::vector<float> WhiteNoise(double rms, double seconds, double sampleRate, std::uint32_t seed = 12345)
{
    const auto frames = static_cast<std::size_t>(seconds * sampleRate);
    std::vector<float> out(frames);
    // Uniform in [-1, 1) has an RMS of 1/sqrt(3).
    const double scale = rms * std::sqrt(3.0);

    for (auto& sample : out)
    {
        seed = seed * 1664525u + 1013904223u;
        sample = static_cast<float>(scale * (static_cast<double>(seed >> 8) / 8388608.0 - 1.0));
    }

    return out;
}

std::vector<float> Sum(const std::vector<float>& a, const std::vector<float>& b)
{
    std::vector<float> out(std::max(a.size(), b.size()), 0.0f);

    for (std::size_t i = 0; i < out.size(); ++i)
    {
        out[i] = (i < a.size() ? a[i] : 0.0f) + (i < b.size() ? b[i] : 0.0f);
    }

    return out;
}

/// The peak amplitude of the component at `hz` over [start, end), Hann-windowed.
double Amplitude(const std::vector<float>& x, double hz, std::size_t start, std::size_t end, double sampleRate)
{
    double re = 0.0;
    double im = 0.0;
    double windowSum = 0.0;
    const double n = static_cast<double>(end - start);

    for (std::size_t i = start; i < end; ++i)
    {
        const double t = static_cast<double>(i - start);
        const double w = 0.5 - 0.5 * std::cos(2.0 * kPi * t / n);
        const double angle = 2.0 * kPi * hz * static_cast<double>(i) / sampleRate;
        re += w * x[i] * std::cos(angle);
        im -= w * x[i] * std::sin(angle);
        windowSum += w;
    }

    return 2.0 * std::sqrt(re * re + im * im) / windowSum;
}

/// Total power of the harmonics of `mainsHz` below `topHz`, over [start, end).
double HarmonicPower(const std::vector<float>& x, double mainsHz, double topHz, std::size_t start, std::size_t end,
                     double sampleRate)
{
    double power = 0.0;

    for (int k = 1; k * mainsHz < topHz; ++k)
    {
        const double a = Amplitude(x, k * mainsHz, start, end, sampleRate);
        power += 0.5 * a * a;
    }

    return power;
}

double MeanSquare(const std::vector<float>& x, std::size_t start, std::size_t end)
{
    double sum = 0.0;

    for (std::size_t i = start; i < end; ++i)
    {
        sum += static_cast<double>(x[i]) * x[i];
    }

    return end > start ? sum / static_cast<double>(end - start) : 0.0;
}

/// A fourth-order Butterworth high-pass (two RBJ sections), for band measurements.
std::vector<float> HighPassed(const std::vector<float>& x, double hz, double sampleRate)
{
    std::vector<float> out(x);

    for (const double q : {0.5411961, 1.3065630})
    {
        const auto c = guitarfx::biquad::HighPass(hz, q, sampleRate);
        guitarfx::biquad::State state;

        for (auto& sample : out)
        {
            sample = static_cast<float>(state.Process(c, sample));
        }
    }

    return out;
}

std::vector<float> LowPassed(const std::vector<float>& x, double hz, double sampleRate)
{
    std::vector<float> out(x);

    for (const double q : {0.5411961, 1.3065630})
    {
        const auto c = guitarfx::biquad::LowPass(hz, q, sampleRate);
        guitarfx::biquad::State state;

        for (auto& sample : out)
        {
            sample = static_cast<float>(state.Process(c, sample));
        }
    }

    return out;
}

double ToDb(double powerRatio)
{
    return 10.0 * std::log10(std::max(powerRatio, 1.0e-30));
}

void TestRegistration()
{
    std::cout << "\nRegistration and parameters" << std::endl;
    auto& registry = guitarfx::EffectRegistry::Instance();
    const auto info = registry.GetTypeInfo(guitarfx::EffectGuids::kNoiseReduction);

    Check(info.has_value(), "registered under its UUID");
    Check(registry.Resolve("noise_reduction") == guitarfx::EffectGuids::kNoiseReduction,
          "the noise_reduction alias resolves to it");
    Check(registry.Create("noise_reduction")->GetType() == "noise_reduction", "the registry creates one");

    if (!info)
    {
        return;
    }

    Check(info->category == "dynamics" && !info->requiresResource, "a dynamics effect with no resource");

    bool ordered = info->parameters.size() == guitarfx::noise_reduction::kParamCount;

    for (std::size_t i = 0; ordered && i < info->parameters.size(); ++i)
    {
        ordered = info->parameters[i].id == guitarfx::noise_reduction::kParams[i].id;
    }

    Check(ordered, "parameters are declared in table order");

    NoiseReductionEffect effect;
    effect.SetParam("hiss", 4.0);
    Check(effect.GetParam("hiss") == 1.0, "Hiss clamps to 100%");
    effect.SetParam("pickup", 2.4);
    Check(effect.GetParam("pickup") == 2.0, "an enum snaps to a whole choice");
    effect.SetParam("threshold", 6.0);
    effect.SetParam("threshold", std::nan(""));
    Check(effect.GetParam("threshold") == 6.0, "a NaN is ignored");
    Check(effect.GetLatencySamples() == 0, "no latency");
    Check(!effect.CanWiden() && effect.KeepsChannelsSeparate() && effect.SupportsMonoProcessing(),
          "mono-capable, never widens, keeps its channels apart");

    Check(info->presets.size() >= 5 && info->presets.front().isDefault, "ships factory presets, default first",
          std::to_string(info->presets.size()));

    bool leavesPlayersControls = true;

    for (const auto& preset : info->presets)
    {
        leavesPlayersControls =
            leavesPlayersControls && !preset.parameters.contains("mains") && !preset.parameters.contains("listen");
    }

    Check(leavesPlayersControls, "no preset sets Mains or Listen");
}

struct HumResult
{
    double reductionDb = 0.0;
    double fundamentalReductionDb = 0.0;
    double topReductionDb = 0.0;
    double mainsHz = 0.0;
};

/// `seconds` of hum and faint noise; the reduction is measured over the last second.
HumResult MeasureHum(double mainsHz, double topHz, double seconds, double sampleRate, const Params& params)
{
    const auto input = Sum(Hum(mainsHz, 0.01, topHz, seconds, sampleRate), WhiteNoise(3.0e-5, seconds, sampleRate));
    auto effect = Make(sampleRate, params);
    const auto output = RunMono(*effect, input);
    const std::size_t end = input.size();
    const std::size_t start = end - static_cast<std::size_t>(sampleRate);
    const int top = static_cast<int>((topHz - 1.0) / mainsHz);

    HumResult result;
    result.reductionDb = ToDb(HarmonicPower(input, mainsHz, topHz, start, end, sampleRate) /
                              HarmonicPower(output, mainsHz, topHz, start, end, sampleRate));
    result.fundamentalReductionDb = 20.0 * std::log10(Amplitude(input, mainsHz, start, end, sampleRate) /
                                                      Amplitude(output, mainsHz, start, end, sampleRate));
    result.topReductionDb = 20.0 * std::log10(Amplitude(input, top * mainsHz, start, end, sampleRate) /
                                              Amplitude(output, top * mainsHz, start, end, sampleRate));
    result.mainsHz = effect->GetParam("mainsHz");
    return result;
}

void TestHumRemoval()
{
    std::cout << "\nHum and buzz" << std::endl;

    // Hiss off, so only the canceller acts.
    const auto at60 = MeasureHum(60.0, 3000.0, 4.0, kSampleRate, {{"hiss", 0.0}});
    Check(std::abs(at60.mainsHz - 60.0) < 0.05, "Auto finds 60 Hz mains", Fixed(at60.mainsHz) + " Hz");
    Check(at60.reductionDb > 30.0, "60 Hz hum and buzz to 3 kHz fall by over 30 dB", Db(at60.reductionDb));
    Check(at60.topReductionDb > 20.0, "the top buzz harmonic too", Db(at60.topReductionDb));

    const auto at50 = MeasureHum(50.0, 3000.0, 4.0, kSampleRate, {{"hiss", 0.0}});
    Check(std::abs(at50.mainsHz - 50.0) < 0.05, "Auto stays on 50 Hz mains", Fixed(at50.mainsHz) + " Hz");
    Check(at50.reductionDb > 30.0, "50 Hz hum and buzz fall by over 30 dB", Db(at50.reductionDb));

    // Mains off nominal: at the 59th harmonic 0.2 Hz is 12 Hz, far outside any notch, so this
    // only passes if the loop has moved the oscillators onto the real frequency.
    const auto drifted = MeasureHum(50.2, 3000.0, 6.0, kSampleRate, {{"hiss", 0.0}});
    Check(std::abs(drifted.mainsHz - 50.2) < 0.01, "the loop locks onto 50.2 Hz mains",
          Fixed(drifted.mainsHz, 4) + " Hz");
    Check(drifted.reductionDb > 25.0 && drifted.topReductionDb > 15.0, "and cancels it to the top harmonic",
          Db(drifted.reductionDb) + ", top " + Db(drifted.topReductionDb));

    const auto forced = MeasureHum(60.0, 3000.0, 4.0, kSampleRate, {{"hiss", 0.0}, {"mains", 2.0}});
    Check(forced.reductionDb > 30.0, "Mains set to 60 Hz works without detection", Db(forced.reductionDb));

    const auto humbucker = MeasureHum(60.0, 3000.0, 4.0, kSampleRate, {{"hiss", 0.0}, {"pickup", 1.0}});
    Check(humbucker.fundamentalReductionDb > 30.0 && humbucker.topReductionDb < 3.0,
          "Humbucker follows the hum but leaves buzz above 1.2 kHz to the hiss stage",
          Db(humbucker.fundamentalReductionDb) + ", top " + Db(humbucker.topReductionDb));

    const auto half = MeasureHum(60.0, 3000.0, 4.0, kSampleRate, {{"hiss", 0.0}, {"hum", 0.5}});
    Check(std::abs(half.reductionDb - 6.02) < 0.5, "Hum at 50% halves it", Db(half.reductionDb));

    for (const double rate : {44100.0, 96000.0})
    {
        const auto result = MeasureHum(60.0, 3000.0, 4.0, rate, {{"hiss", 0.0}});
        Check(result.reductionDb > 30.0, "works at " + Fixed(rate / 1000.0, 1) + " kHz", Db(result.reductionDb));
    }
}

void TestNoteOnHarmonicIsKept()
{
    std::cout << "\nA note on a hum harmonic" << std::endl;

    // 300 Hz is the 5th harmonic of 60 Hz. Hum learned first, then a loud sustained note at
    // exactly that frequency over it: learning must stop, or the note is cancelled as hum.
    const double learn = 3.0;
    const double play = 4.0;
    const auto hum = Hum(60.0, 0.01, 3000.0, learn + play, kSampleRate);
    auto input = Sum(hum, WhiteNoise(3.0e-5, learn + play, kSampleRate));
    const auto noteStart = static_cast<std::size_t>(learn * kSampleRate);
    const double noteLevel = 0.25;

    for (std::size_t i = noteStart; i < input.size(); ++i)
    {
        input[i] += static_cast<float>(noteLevel * std::sin(2.0 * kPi * 300.0 * static_cast<double>(i) / kSampleRate));
    }

    auto effect = Make(kSampleRate, {{"hiss", 0.0}});
    const auto output = RunMono(*effect, input);

    // What the output should be at 300 Hz is the note alone: the input less its hum.
    std::vector<float> wanted(input.size());

    for (std::size_t i = 0; i < input.size(); ++i)
    {
        wanted[i] = input[i] - hum[i];
    }

    const std::size_t end = input.size();
    const std::size_t start = end - static_cast<std::size_t>(kSampleRate);
    const double noteOut = Amplitude(output, 300.0, start, end, kSampleRate);
    const double noteWanted = Amplitude(wanted, 300.0, start, end, kSampleRate);
    const double changeDb = 20.0 * std::log10(noteOut / noteWanted);
    Check(std::abs(changeDb) < 0.2, "a 4 s note at 300 Hz keeps its level", Db(changeDb));

    const double humOut = HarmonicPower(output, 60.0, 280.0, start, end, kSampleRate);
    const double humIn = HarmonicPower(input, 60.0, 280.0, start, end, kSampleRate);
    Check(ToDb(humIn / humOut) > 25.0, "while the hum below it stays cancelled", Db(ToDb(humIn / humOut)));
}

void TestHiss()
{
    std::cout << "\nHiss" << std::endl;
    const double seconds = 3.0;
    const auto hiss = WhiteNoise(3.0e-4, seconds, kSampleRate); // about -70 dBFS
    const std::size_t end = hiss.size();
    const std::size_t start = end - static_cast<std::size_t>(kSampleRate);

    for (const double amount : {0.5, 1.0})
    {
        auto effect = Make(kSampleRate, {{"hum", 0.0}, {"hiss", amount}});
        const auto output = RunMono(*effect, hiss);
        const double topCut = ToDb(MeanSquare(HighPassed(hiss, 6000.0, kSampleRate), start, end) /
                                   MeanSquare(HighPassed(output, 6000.0, kSampleRate), start, end));
        const double lowChange = ToDb(MeanSquare(LowPassed(output, 800.0, kSampleRate), start, end) /
                                      MeanSquare(LowPassed(hiss, 800.0, kSampleRate), start, end));
        const double wantDb = amount * guitarfx::noise_reduction::kMaxHissReductionDb;
        Check(topCut > wantDb - 3.0 && topCut < wantDb + 1.0,
              "Hiss " + Fixed(amount * 100.0, 0) + "% cuts steady hiss above 6 kHz by about " + Db(wantDb), Db(topCut));
        Check(std::abs(lowChange) < 0.5, "and leaves below 800 Hz alone", Db(lowChange));
    }

    // A loud bright signal after the floor is known: the bands open and it passes whole.
    auto effect = Make(kSampleRate, {{"hum", 0.0}, {"hiss", 1.0}});
    auto input = hiss;
    const auto loud = WhiteNoise(0.05, 1.0, kSampleRate, 777);
    input.insert(input.end(), loud.begin(), loud.end());
    const auto output = RunMono(*effect, input);
    const std::size_t loudStart = hiss.size() + static_cast<std::size_t>(0.1 * kSampleRate);
    const double topChange = ToDb(MeanSquare(HighPassed(output, 6000.0, kSampleRate), loudStart, input.size()) /
                                  MeanSquare(HighPassed(input, 6000.0, kSampleRate), loudStart, input.size()));
    Check(std::abs(topChange) < 0.5, "a loud bright signal passes at full level", Db(topChange));
}

void TestRealGuitar()
{
    std::cout << "\nThe DI take, with hum and hiss added" << std::endl;
    double clipRate = 0.0;
    auto clip = guitarfx::test::LoadDemoClipMono(guitarfx::test::kDemoDiGuitar, clipRate);

    if (clip.empty() || clipRate != kSampleRate)
    {
        Check(false, "the demo DI clip loads at 48 kHz");
        return;
    }

    // Two seconds before the playing, as there is between plugging in and playing, then 20 s of
    // riffs with single-coil buzz at 60 Hz (the take carries a little 50 Hz hum of its own) and
    // hiss about 74 dB down.
    const auto lead = static_cast<std::size_t>(2.0 * kSampleRate);
    std::vector<float> clean(lead, 0.0f);
    clean.insert(clean.end(), clip.begin(), clip.begin() + static_cast<std::ptrdiff_t>(20.0 * kSampleRate));
    const double seconds = static_cast<double>(clean.size()) / kSampleRate;
    const auto hum = Hum(60.0, 0.003, 4000.0, seconds, kSampleRate);
    const auto input = Sum(Sum(clean, hum), WhiteNoise(2.0e-4, seconds, kSampleRate));

    // The canceller alone: the hum comes out from under the playing, and nothing else changes.
    auto humOnly = Make(kSampleRate, {{"hiss", 0.0}});
    const auto humOut = RunMono(*humOnly, Sum(clean, hum));
    double humIn = 0.0;
    double humLeft = 0.0;

    for (std::size_t i = lead; i < input.size(); ++i)
    {
        humIn += static_cast<double>(hum[i]) * hum[i];
        humLeft += static_cast<double>(humOut[i] - clean[i]) * (humOut[i] - clean[i]);
    }

    // What is left includes the take's own 50 Hz hum, which the canceller does not follow once
    // it has settled on 60, so this is a floor on the reduction rather than the reduction.
    Check(ToDb(humIn / humLeft) > 10.0, "hum and buzz under the playing fall by over 10 dB", Db(ToDb(humIn / humLeft)));
    Check(std::abs(humOnly->GetParam("mainsHz") - 60.0) < 0.05, "Auto found the 60 Hz mains under the playing",
          Fixed(humOnly->GetParam("mainsHz")) + " Hz");

    // Both stages at their defaults.
    auto effect = Make(kSampleRate);
    const auto output = RunMono(*effect, input);
    const auto window = static_cast<std::size_t>(0.05 * kSampleRate);
    double gapNoiseIn = 0.0;
    double gapNoiseOut = 0.0;
    double worstDb = 0.0;
    int loudWindows = 0;
    int gapWindows = 0;

    for (std::size_t start = lead; start + window <= input.size(); start += window)
    {
        const double cleanPower = MeanSquare(clean, start, start + window);

        if (cleanPower >= 1.0e-3) // -30 dBFS: playing
        {
            ++loudWindows;
            const double change = ToDb(MeanSquare(output, start, start + window) / cleanPower);
            worstDb = std::abs(change) > std::abs(worstDb) ? change : worstDb;
        }
        else if (cleanPower < 1.0e-5) // -50 dBFS: between the riffs
        {
            ++gapWindows;

            // Noise is whatever differs from the clean take, so quiet playing is not counted.
            for (std::size_t i = start; i < start + window; ++i)
            {
                gapNoiseIn += static_cast<double>(input[i] - clean[i]) * (input[i] - clean[i]);
                gapNoiseOut += static_cast<double>(output[i] - clean[i]) * (output[i] - clean[i]);
            }
        }
    }

    Check(gapWindows >= 5 && ToDb(gapNoiseIn / gapNoiseOut) > 20.0, "between the riffs the noise falls by over 20 dB",
          std::to_string(gapWindows) + " gaps, " + Db(ToDb(gapNoiseIn / gapNoiseOut)));
    Check(loudWindows > 50 && std::abs(worstDb) < 0.5, "loud playing keeps its level to 0.5 dB",
          std::to_string(loudWindows) + " windows, worst " + Db(worstDb));

    // NR_WAV_DIR=<folder> writes the take before and after, to listen to.
    if (const char* folder = std::getenv("NR_WAV_DIR"))
    {
        const std::filesystem::path dir(folder);
        const bool written = guitarfx::util::WriteStereo16BitWav(dir / "nr-in.wav", input, input, 48000) &&
                             guitarfx::util::WriteStereo16BitWav(dir / "nr-out.wav", output, output, 48000);
        std::cout << "  wrote " << (dir / "nr-in.wav").string() << (written ? "" : " (FAILED)") << std::endl;
    }
}

void TestTransparencyAndListen()
{
    std::cout << "\nTransparency and Listen" << std::endl;
    const auto input = Sum(Hum(60.0, 0.01, 3000.0, 2.0, kSampleRate), WhiteNoise(0.05, 2.0, kSampleRate));

    auto off = Make(kSampleRate, {{"hum", 0.0}, {"hiss", 0.0}});
    const auto passed = RunMono(*off, input);
    float worst = 0.0f;

    for (std::size_t i = 0; i < input.size(); ++i)
    {
        worst = std::max(worst, std::abs(passed[i] - input[i]));
    }

    Check(worst == 0.0f, "Hum and Hiss at zero pass the input untouched", Fixed(worst, 9));

    const auto quiet = Sum(Hum(60.0, 0.01, 3000.0, 3.0, kSampleRate), WhiteNoise(3.0e-4, 3.0, kSampleRate));
    auto normal = Make(kSampleRate);
    auto removed = Make(kSampleRate, {{"listen", 1.0}});
    const auto cleaned = RunMono(*normal, quiet);
    const auto noise = RunMono(*removed, quiet);
    worst = 0.0f;

    for (std::size_t i = 0; i < quiet.size(); ++i)
    {
        worst = std::max(worst, std::abs(cleaned[i] + noise[i] - quiet[i]));
    }

    Check(worst < 1.0e-6f, "Removed Noise is exactly the input less the output", Fixed(worst, 9));
}

void TestChannelsAndRecovery()
{
    std::cout << "\nChannels and recovery" << std::endl;
    const auto input = Sum(Hum(60.0, 0.01, 3000.0, 2.0, kSampleRate), WhiteNoise(3.0e-4, 2.0, kSampleRate));

    auto mono = Make();
    auto stereo = Make();
    const auto monoOut = RunMono(*mono, input);
    std::vector<float> left(input.size()), right(input.size());
    bool same = true;
    bool identical = true;

    for (std::size_t start = 0; start < input.size(); start += kBlockSize)
    {
        const int n = static_cast<int>(std::min<std::size_t>(kBlockSize, input.size() - start));
        std::vector<float> inL(input.begin() + static_cast<std::ptrdiff_t>(start),
                               input.begin() + static_cast<std::ptrdiff_t>(start + n));
        std::vector<float> inR = inL;
        float* ins[2] = {inL.data(), inR.data()};
        float* outs[2] = {left.data() + start, right.data() + start};
        stereo->Process(ins, outs, n);
    }

    for (std::size_t i = 0; i < input.size(); ++i)
    {
        same = same && left[i] == monoOut[i];
        identical = identical && left[i] == right[i];
    }

    Check(same, "the mono path matches the stereo left channel sample for sample");
    Check(identical, "identical sides stay identical");

    // In place, with both channels on one buffer.
    auto inPlace = Make();
    std::vector<float> buffer(input);

    for (std::size_t start = 0; start < buffer.size(); start += kBlockSize)
    {
        const int n = static_cast<int>(std::min<std::size_t>(kBlockSize, buffer.size() - start));
        float* io[2] = {buffer.data() + start, buffer.data() + start};
        inPlace->Process(io, io, n);
    }

    bool inPlaceSame = true;

    for (std::size_t i = 0; i < input.size(); ++i)
    {
        inPlaceSame = inPlaceSame && buffer[i] == monoOut[i];
    }

    Check(inPlaceSame, "in place on one shared buffer gives the same result");

    auto effect = Make();
    auto poisoned = input;
    poisoned[1000] = std::nanf("");
    const auto out = RunMono(*effect, poisoned);
    bool finiteAfter = true;

    for (std::size_t i = 2 * kBlockSize + 1000; i < out.size(); ++i)
    {
        finiteAfter = finiteAfter && guitarfx::IsFinite(out[i]);
    }

    Check(finiteAfter, "a NaN in the input is recovered from within a block");
}

void TestCost()
{
    std::cout << "\nCost" << std::endl;
    const double seconds = 5.0;
    const auto input = Sum(Hum(60.0, 0.01, 3000.0, seconds, kSampleRate), WhiteNoise(3.0e-4, seconds, kSampleRate));

    for (const double pickup : {0.0, 1.0})
    {
        auto effect = Make(kSampleRate, {{"pickup", pickup}, {"mains", 2.0}});
        const auto begin = std::chrono::steady_clock::now();
        const auto output = RunMono(*effect, input);
        const auto elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - begin).count();
        const double perBlockUs = elapsed * 1.0e6 / (seconds * kSampleRate / 64.0);
        std::cout << "  " << (pickup == 0.0 ? "Single Coil" : "Humbucker") << ": " << Fixed(perBlockUs, 2)
                  << " us per 64-sample block (" << effect->GetParam("harmonics") << " harmonics; build type sets this)"
                  << std::endl;
        Check(guitarfx::IsFinite(output.back()), "finite output");
    }
}
} // namespace

int main()
{
    guitarfx::RegisterNoiseReductionEffect();
    std::cout << "Noise Reduction tests" << std::endl;

    TestRegistration();
    TestHumRemoval();
    TestNoteOnHarmonicIsKept();
    TestHiss();
    TestRealGuitar();
    TestTransparencyAndListen();
    TestChannelsAndRecovery();
    TestCost();

    std::cout << "\n" << (gChecks - gFailures) << "/" << gChecks << " checks passed" << std::endl;
    return gFailures == 0 ? 0 : 1;
}

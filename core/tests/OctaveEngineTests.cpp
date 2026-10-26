/**
 * @file OctaveEngineTests.cpp
 * @brief Octave's two engines and the crossfades between its paths.
 *
 *   - the Engine parameter is registered and defaults to Low Latency
 *   - the Low Latency engine (two SpliceTransposers) puts each voice an octave from the input,
 *     within a few cents, at 44.1, 48 and 96 kHz
 *   - it reports 16 ms, and at Mix 0 plays the input delayed by exactly that
 *   - entering and leaving the bypass, and switching engine, never clicks
 *   - a re-engaged Low Latency engine does not replay audio from before its bypass
 *   - none of it allocates on the audio thread, including the engine switch and the splices
 */

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
#include <iostream>
#include <string>
#include <vector>

#include "dsp/EffectGuids.h"
#include "dsp/EffectRegistry.h"
#include "dsp/effects/BuiltinEffects.h"
#include "dsp/effects/OctaveEffect.h"
#include "helpers/AudioThreadAllocations.h"

namespace
{
constexpr double kPi = 3.14159265358979323846;
constexpr double kSampleRate = 48000.0;
constexpr int kBlockSize = 64;

using guitarfx::OctaveEffect;

int gFailures = 0;

void Check(bool condition, const std::string& what, const std::string& detail = "")
{
    gFailures += condition ? 0 : 1;
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

std::vector<float> Sine(double frequency, double seconds, double amplitude, double sampleRate = kSampleRate)
{
    std::vector<float> out(static_cast<size_t>(seconds * sampleRate));

    for (size_t i = 0; i < out.size(); ++i)
    {
        out[i] = static_cast<float>(amplitude * std::sin(2.0 * kPi * frequency * static_cast<double>(i) / sampleRate));
    }

    return out;
}

/// Six harmonics falling as 1/k: enough for a correlation splice to have a real waveform to match.
std::vector<float> Harmonic(double frequency, double seconds, double sampleRate)
{
    std::vector<float> out(static_cast<size_t>(seconds * sampleRate));

    for (size_t i = 0; i < out.size(); ++i)
    {
        double v = 0.0;

        for (int h = 1; h <= 6; ++h)
        {
            v += std::sin(2.0 * kPi * frequency * h * static_cast<double>(i) / sampleRate + h * 0.7) / h;
        }

        out[i] = static_cast<float>(0.2 * v);
    }

    return out;
}

/// Renders a mono signal through the effect as stereo, in blocks, calling `beforeBlock` with each
/// block's first sample index so a test can move parameters where MIDI would: between blocks.
std::vector<float> Render(OctaveEffect& effect, const std::vector<float>& input,
                          const std::function<void(size_t)>& beforeBlock = {})
{
    std::vector<float> out(input.size(), 0.0f);
    std::vector<float> inL(kBlockSize);
    std::vector<float> inR(kBlockSize);
    std::vector<float> outL(kBlockSize);
    std::vector<float> outR(kBlockSize);

    for (size_t pos = 0; pos + kBlockSize <= input.size(); pos += kBlockSize)
    {
        if (beforeBlock)
        {
            beforeBlock(pos);
        }

        std::copy_n(input.begin() + static_cast<std::ptrdiff_t>(pos), kBlockSize, inL.begin());
        std::copy_n(input.begin() + static_cast<std::ptrdiff_t>(pos), kBlockSize, inR.begin());
        float* inputs[2] = {inL.data(), inR.data()};
        float* outputs[2] = {outL.data(), outR.data()};
        effect.Process(inputs, outputs, kBlockSize);
        std::copy_n(outL.begin(), kBlockSize, out.begin() + static_cast<std::ptrdiff_t>(pos));
    }

    return out;
}

/// Octave on the given engine with one voice up full, the tone wide open and only wet heard.
void PrepareVoice(OctaveEffect& effect, double engine, double up, double down, double sampleRate = kSampleRate)
{
    effect.Prepare(sampleRate, kBlockSize);
    effect.SetParam("engine", engine);
    effect.SetParam("octaveUp", up);
    effect.SetParam("octaveDown", down);
    effect.SetParam("tone", 1.0);
    effect.SetParam("mix", 1.0);
    effect.Reset();
}

double Goertzel(const float* x, int n, double frequency, double sampleRate = kSampleRate)
{
    const double w = 2.0 * kPi * frequency / sampleRate;
    const double c = 2.0 * std::cos(w);
    double s1 = 0.0;
    double s2 = 0.0;

    for (int i = 0; i < n; ++i)
    {
        const double window = 0.5 - 0.5 * std::cos(2.0 * kPi * (i + 0.5) / n);
        const double s0 = x[i] * window + c * s1 - s2;
        s2 = s1;
        s1 = s0;
    }

    return s1 * s1 + s2 * s2 - c * s1 * s2;
}

/// The strongest frequency within +/-60 cents of `expected`, in cents from it.
double PitchErrorCents(const float* x, int n, double expected, double sampleRate)
{
    double best = 0.0;
    double bestCents = 0.0;

    for (double cents = -60.0; cents <= 60.0; cents += 0.25)
    {
        const double energy = Goertzel(x, n, expected * std::pow(2.0, cents / 1200.0), sampleRate);

        if (energy > best)
        {
            best = energy;
            bestCents = cents;
        }
    }

    return bestCents;
}

void TestRegistration()
{
    std::cout << "\n--- The Engine parameter ---\n";
    guitarfx::RegisterAllEffects();
    const auto info = guitarfx::EffectRegistry::Instance().GetTypeInfo(guitarfx::EffectGuids::kOctave);
    const guitarfx::ParameterDef* engine = nullptr;

    if (info)
    {
        for (const auto& param : info->parameters)
        {
            if (param.id == "engine")
            {
                engine = &param;
            }
        }
    }

    Check(engine != nullptr && engine->defaultValue == 1.0 && engine->labels.size() == 2,
          "Registered, defaulting to Low Latency (stored nodes without it: PitchShiftEngineTests)");

    OctaveEffect effect;
    Check(effect.GetParam("engine") == 1.0, "A fresh node is on Low Latency");
    effect.SetParam("engine", 0.0);
    Check(effect.GetParam("engine") == 0.0, "and reads back High Quality once set");
}

void TestPitchAccuracy()
{
    std::cout << "\n--- Low Latency puts each voice an octave away ---\n";

    for (const double sampleRate : {44100.0, 48000.0, 96000.0})
    {
        double worst = 0.0;
        std::string worstCase;

        for (const double note : {82.41, 196.0, 329.63})
        {
            for (const bool up : {true, false})
            {
                OctaveEffect effect;
                PrepareVoice(effect, 1.0, up ? 1.0 : 0.0, up ? 0.0 : 1.0, sampleRate);

                const auto out = Render(effect, Harmonic(note, 1.0, sampleRate));
                const int n = sampleRate > 60000.0 ? 32768 : 16384;
                const double expected = note * (up ? 2.0 : 0.5);
                const double error =
                    std::abs(PitchErrorCents(out.data() + out.size() - n - 512, n, expected, sampleRate));

                if (error > worst)
                {
                    worst = error;
                    worstCase = Num(note, 1) + " Hz " + (up ? "up" : "down");
                }
            }
        }

        Check(worst <= 3.0, "Within 3 cents at " + Num(sampleRate / 1000.0, 1) + " kHz",
              "worst " + Num(worst) + " cents, " + worstCase);
    }

    // Both voices at once: each octave carries its share, as on High Quality.
    OctaveEffect effect;
    PrepareVoice(effect, 1.0, 1.0, 1.0);
    const auto out = Render(effect, Sine(220.0, 1.0, 0.3));
    constexpr int kWindow = 16384;
    const float* tail = out.data() + out.size() - kWindow - 512;
    const double upEnergy = Goertzel(tail, kWindow, 440.0);
    const double downEnergy = Goertzel(tail, kWindow, 110.0);
    const double dryEnergy = Goertzel(tail, kWindow, 220.0);
    Check(upEnergy > 20.0 * dryEnergy && downEnergy > 20.0 * dryEnergy,
          "Both voices sound together, with the input itself 13 dB under each",
          "up/dry " + Num(10.0 * std::log10(upEnergy / std::max(dryEnergy, 1.0e-12)), 1) + " dB, down/dry " +
              Num(10.0 * std::log10(downEnergy / std::max(dryEnergy, 1.0e-12)), 1) + " dB");
}

void TestTransparencyAndAlignment()
{
    std::cout << "\n--- Latency, Mix 0 alignment and the bypass ---\n";

    {
        OctaveEffect effect;
        PrepareVoice(effect, 1.0, 1.0, 1.0);
        effect.SetParam("mix", 0.0);
        effect.SetParam("octaveUp", 0.0);
        effect.SetParam("octaveDown", 0.0);
        effect.Reset();
        Check(effect.GetLatencySamples() == 0, "Transparent reports no latency",
              std::to_string(effect.GetLatencySamples()));
    }

    {
        // Mix 0 is transparent, which reports 0; Mix just above it runs the engine and is
        // all dry to within a hair, so the dry tap's alignment shows.
        OctaveEffect effect;
        PrepareVoice(effect, 1.0, 1.0, 1.0);
        effect.SetParam("mix", 1.0e-6);
        effect.Reset();
        const int latency = effect.GetLatencySamples();
        const auto input = Harmonic(110.0, 0.5, kSampleRate);
        const auto out = Render(effect, input);
        double worst = 0.0;

        for (size_t i = static_cast<size_t>(latency) + 1000; i < out.size() - kBlockSize; ++i)
        {
            worst = std::max(worst, static_cast<double>(std::fabs(out[i] - input[i - static_cast<size_t>(latency)])));
        }

        Check(latency > 0 && worst < 1.0e-4, "Near Mix 0 the output is the input delayed by the reported latency",
              std::to_string(latency) + " samples, worst error " + Num(worst, 6));
        // The tap's mean delay over its window: (2 ms floor + 30 ms window) / 2.
        Check(latency == static_cast<int>(kSampleRate * 0.016), "Low Latency reports 16 ms",
              Num(latency * 1000.0 / kSampleRate, 1) + " ms");
        effect.SetParam("engine", 0.0);
        Check(effect.GetLatencySamples() >= static_cast<int>(kSampleRate * 0.08), "High Quality reports its 80 ms",
              std::to_string(effect.GetLatencySamples()));
    }
}

/// The largest sample-to-sample step in the output while mix and engine flip under a sine,
/// measured over the 30 ms after each change that leaves Mix above 0. Mix itself is not smoothed,
/// so turning it to 0 in one step cuts the wet at once, whatever the path fade does; that is the
/// knob's own behaviour, not a path change, and is left out.
double LargestStep(const std::vector<std::pair<double, double>>& schedule)
{
    OctaveEffect effect;
    PrepareVoice(effect, schedule.front().second, 0.0, 1.0);
    effect.SetParam("mix", schedule.front().first);
    effect.Reset();

    const auto input = Sine(220.0, 4.0, 0.5);
    const size_t every = static_cast<size_t>(kSampleRate * 0.11) / kBlockSize * kBlockSize;
    const size_t watch = static_cast<size_t>(kSampleRate * 0.03);
    size_t index = 0;
    std::vector<size_t> changes;
    const auto out = Render(effect, input, [&](size_t pos) {
        if (pos > 0 && pos % every == 0)
        {
            index = (index + 1) % schedule.size();
            effect.SetParam("mix", schedule[index].first);
            effect.SetParam("engine", schedule[index].second);

            if (schedule[index].first > 0.0)
            {
                changes.push_back(pos);
            }
        }
    });

    double largest = 0.0;

    for (const size_t change : changes)
    {
        // Past High Quality's first warm-up, which starts from an empty engine.
        if (change < static_cast<size_t>(kSampleRate * 0.2))
        {
            continue;
        }

        for (size_t i = change; i < std::min(out.size(), change + watch); ++i)
        {
            largest = std::max(largest, static_cast<double>(std::fabs(out[i] - out[i - 1])));
        }
    }

    return largest;
}

void TestNoClicks()
{
    std::cout << "\n--- Changing path crossfades ---\n";
    // A 0.5 sine at 220 Hz moves at most 0.014 a sample, its octave down half that; a hard switch
    // between two copies of it out of step moves up to 1.0.
    constexpr double kLimit = 0.04;
    const double bypass = LargestStep({{0.0, 1.0}, {1.0, 1.0}});
    Check(bypass < kLimit, "Low Latency out of the bypass", "largest step " + Num(bypass, 4));
    const double bypassHighQuality = LargestStep({{0.0, 0.0}, {1.0, 0.0}});
    Check(bypassHighQuality < kLimit, "High Quality out of the bypass", "largest step " + Num(bypassHighQuality, 4));
    const double engines = std::max(LargestStep({{1.0, 0.0}, {1.0, 1.0}}), LargestStep({{0.5, 0.0}, {0.5, 1.0}}));
    Check(engines < kLimit, "Switching engine, wet and half wet", "largest step " + Num(engines, 4));
    const double viaBypass = LargestStep({{1.0, 0.0}, {0.0, 0.0}, {1.0, 1.0}, {0.0, 1.0}});
    Check(viaBypass < kLimit, "Switching engine while in the bypass", "largest step " + Num(viaBypass, 4));
}

void TestReEngageIsClean()
{
    std::cout << "\n--- A re-engaged Low Latency engine plays nothing stale ---\n";
    OctaveEffect effect;
    PrepareVoice(effect, 1.0, 1.0, 1.0);

    const auto loud = Sine(220.0, 1.0, 0.5);
    const std::vector<float> silence(static_cast<size_t>(kSampleRate * 0.5), 0.0f);
    (void)Render(effect, loud);
    effect.SetParam("mix", 0.0);
    (void)Render(effect, silence);
    effect.SetParam("mix", 1.0);
    const auto out = Render(effect, silence);
    double peak = 0.0;

    for (float v : out)
    {
        peak = std::max(peak, static_cast<double>(std::fabs(v)));
    }

    Check(peak < 1.0e-3, "Silence after the bypass stays silent", "peak " + Num(peak, 6));
}

void TestNoAudioThreadAllocations()
{
    std::cout << "\n--- Nothing allocates on the audio thread ---\n";
    OctaveEffect effect;
    PrepareVoice(effect, 1.0, 0.6, 0.6);
    const auto input = Harmonic(110.0, 3.0, kSampleRate);
    // Warm both engines once, off the audio thread: the first High Quality engage is where a
    // lazily sized buffer would show.
    (void)Render(effect, input, [&](size_t pos) { effect.SetParam("engine", (pos / 4800) % 2 ? 0.0 : 1.0); });

    std::vector<float> inL(kBlockSize);
    std::vector<float> inR(kBlockSize);
    std::vector<float> outL(kBlockSize);
    std::vector<float> outR(kBlockSize);
    // Made here, as automation holds its bound parameter ids: a Debug std::string allocates an
    // iterator-debugging proxy when it is constructed, which would be the test's allocation.
    const std::string engineKey = "engine";
    const std::string mixKey = "mix";

    const auto allocations = audio_thread_allocations::CountOnAudioThread([&] {
        for (size_t pos = 0; pos + kBlockSize <= input.size(); pos += kBlockSize)
        {
            if (pos % 3200 == 0)
            {
                const auto step = pos / 3200;
                effect.SetParam(engineKey, step % 3 == 0 ? 0.0 : 1.0);
                effect.SetParam(mixKey, step % 5 == 0 ? 0.0 : 0.7);
            }

            std::copy_n(input.begin() + static_cast<std::ptrdiff_t>(pos), kBlockSize, inL.begin());
            std::copy_n(input.begin() + static_cast<std::ptrdiff_t>(pos), kBlockSize, inR.begin());
            float* inputs[2] = {inL.data(), inR.data()};
            float* outputs[2] = {outL.data(), outR.data()};
            effect.Process(inputs, outputs, kBlockSize);
        }
    });

    Check(allocations.count == 0, "Engine switches, bypasses and splices",
          audio_thread_allocations::Describe(allocations));
}
} // namespace

int main()
{
    std::cout << "Octave engine tests\n";
    TestRegistration();
    TestPitchAccuracy();
    TestTransparencyAndAlignment();
    TestNoClicks();
    TestReEngageIsClean();
    TestNoAudioThreadAllocations();

    std::cout << "\n" << (gFailures == 0 ? "All passed" : std::to_string(gFailures) + " failed") << std::endl;
    return gFailures == 0 ? 0 : 1;
}

/**
 * @file AutoArpEffectTests.cpp
 * @brief The Auto Arpeggiator's steps, its parameters and their safety on the audio thread.
 *
 *   - a shifted step after a 0 st step plays what is coming in now: not the tone that stopped
 *     before it, and not the chord played before a change
 *   - a step's pitch is heard on its first sample, where Signalsmith took 40-50 ms
 *   - held steps are in tune from -24 to +24 st, at 44.1, 48 and 96 kHz
 *   - legato steps never click, and a 100% gate never drops a sample at a step boundary
 *   - the output is the same in any block size: steps start on their own sample
 *   - SetParam takes any key and any value without throwing, and nothing it or Process does
 *     allocates on the audio thread
 *   - Steps declares one label per value, from its minimum, so a UI that indexes labels from the
 *     minimum reads 4 as "4"
 */

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <exception>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

#include "dsp/EffectGuids.h"
#include "dsp/EffectRegistry.h"
#include "dsp/FiniteCheck.h"
#include "dsp/effects/AutoArpEffect.h"
#include "helpers/AudioThreadAllocations.h"

namespace
{
constexpr double kPi = 3.14159265358979323846;

using guitarfx::AutoArpEffect;
using Params = std::vector<std::pair<std::string, double>>;

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

/// Six harmonics falling as 1/k, from `from` to `to` seconds of a `total`-second signal.
std::vector<float> Harmonic(double hz, double from, double to, double total, double sampleRate = 48000.0)
{
    std::vector<float> x(static_cast<size_t>(total * sampleRate), 0.0f);
    const auto end = std::min(x.size(), static_cast<size_t>(to * sampleRate));

    for (size_t n = static_cast<size_t>(from * sampleRate); n < end; ++n)
    {
        double v = 0.0;

        for (int k = 1; k <= 6; ++k)
        {
            v += std::sin(2.0 * kPi * k * hz * static_cast<double>(n) / sampleRate + 0.7 * k) / k;
        }

        x[n] = static_cast<float>(0.2 * v);
    }

    return x;
}

std::vector<float> Sine(double hz, double seconds, double sampleRate = 48000.0)
{
    std::vector<float> x(static_cast<size_t>(seconds * sampleRate));

    for (size_t n = 0; n < x.size(); ++n)
    {
        x[n] = static_cast<float>(0.3 * std::sin(2.0 * kPi * hz * static_cast<double>(n) / sampleRate));
    }

    return x;
}

/// Two steps of a Custom pattern with the gate fully open, so each step plays from end to end.
Params Legato(double first, double second, double stepRate)
{
    return {{"pattern", 4.0}, {"numSteps", 2.0}, {"step0", first}, {"step1", second}, {"stepRate", stepRate},
            {"gate", 1.0},    {"attack", 0.0},   {"release", 0.0}, {"mix", 1.0},      {"bpm", 120.0}};
}

/// Renders a mono signal through the arp as stereo, in blocks of `block`, and returns the left.
std::vector<float> Render(const std::vector<float>& input, const Params& params, int block = 64,
                          double sampleRate = 48000.0)
{
    AutoArpEffect effect;

    for (const auto& [key, value] : params)
    {
        effect.SetParam(key, value);
    }

    effect.Prepare(sampleRate, block);
    std::vector<float> out(input.size(), 0.0f);
    std::vector<float> in(static_cast<size_t>(block));
    std::vector<float> right(static_cast<size_t>(block));

    for (size_t pos = 0; pos + static_cast<size_t>(block) <= input.size(); pos += static_cast<size_t>(block))
    {
        std::copy_n(input.begin() + static_cast<std::ptrdiff_t>(pos), block, in.begin());
        float* inputs[2] = {in.data(), in.data()};
        float* outputs[2] = {out.data() + pos, right.data()};
        effect.Process(inputs, outputs, block);
    }

    return out;
}

double RmsDb(const std::vector<float>& y, double from, double to, double sampleRate = 48000.0)
{
    const auto start = static_cast<size_t>(from * sampleRate);
    const auto end = static_cast<size_t>(to * sampleRate);
    double energy = 0.0;

    for (size_t n = start; n < end; ++n)
    {
        energy += static_cast<double>(y[n]) * y[n];
    }

    return 10.0 * std::log10(std::max(energy / static_cast<double>(end - start), 1e-20));
}

double Goertzel(const float* x, int n, double hz, double sampleRate = 48000.0)
{
    const double c = 2.0 * std::cos(2.0 * kPi * hz / sampleRate);
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

/// Level in dB of a partial at `hz`, Hann-windowed over `n` samples.
double PartialDb(const float* x, int n, double hz)
{
    return 20.0 * std::log10(4.0 * std::sqrt(std::max(0.0, Goertzel(x, n, hz))) / n + 1e-12);
}

/// The strongest frequency within +/-60 cents of `expected`, in cents from it: 4-cent steps, then
/// quarter-cents around the best. Windowed once, as the search runs many Goertzels over it.
double PitchErrorCents(const float* x, int n, double expected, double sampleRate)
{
    std::vector<float> windowed(static_cast<size_t>(n));

    for (int i = 0; i < n; ++i)
    {
        windowed[static_cast<size_t>(i)] = x[i] * static_cast<float>(0.5 - 0.5 * std::cos(2.0 * kPi * (i + 0.5) / n));
    }

    double best = 0.0;
    double bestCents = 0.0;

    const auto search = [&](double from, double to, double step) {
        for (double cents = from; cents <= to; cents += step)
        {
            const double c = 2.0 * std::cos(2.0 * kPi * expected * std::pow(2.0, cents / 1200.0) / sampleRate);
            double s1 = 0.0;
            double s2 = 0.0;

            for (const float v : windowed)
            {
                const double s0 = v + c * s1 - s2;
                s2 = s1;
                s1 = s0;
            }

            const double energy = s1 * s1 + s2 * s2 - c * s1 * s2;

            if (energy > best)
            {
                best = energy;
                bestCents = cents;
            }
        }
    };

    search(-60.0, 60.0, 4.0);
    search(bestCents - 4.0, bestCents + 4.0, 0.25);
    return bestCents;
}

void TestNoStaleAudio()
{
    std::cout << "\n--- A shifted step after a 0 st step plays what is coming in now ---\n";

    // 500 ms steps (1/4 at 120). The tone stops at 1.2 s, in the 0 st step from 1.0 s; the +12 step
    // from 1.5 s used to replay the tone from before that step at about full level.
    for (const int block : {64, 512})
    {
        const auto tone = Harmonic(220.0, 0.0, 1.2, 2.5);
        const double afterRoot = RmsDb(Render(tone, Legato(0.0, 12.0, 0.0), block), 1.5, 1.6);
        const double control = RmsDb(Render(tone, Legato(5.0, 12.0, 0.0), block), 1.5, 1.6);
        Check(afterRoot < -90.0 && control < -90.0,
              "silence after the tone stops, block " + std::to_string(block) + " (control: no 0 st step)",
              Num(afterRoot, 1) + " / " + Num(control, 1) + " dB, the tone was " + Num(RmsDb(tone, 0.2, 1.0), 1));
    }

    // A chord change in the 0 st step: A3 to D4 at 1.0 s. The +12 step from 1.5 s is D5, not A4.
    auto change = Harmonic(220.0, 0.0, 1.0, 2.5);
    const auto next = Harmonic(293.66, 1.0, 2.5, 2.5);

    for (size_t n = 0; n < change.size(); ++n)
    {
        change[n] += next[n];
    }

    const auto out = Render(change, Legato(0.0, 12.0, 0.0));
    const float* window = out.data() + static_cast<size_t>(1.52 * 48000.0);
    const double newChord = PartialDb(window, 4096, 2.0 * 293.66);
    const double oldChord = PartialDb(window, 4096, 2.0 * 220.0);
    Check(newChord > -18.0 && oldChord < newChord - 50.0,
          "after a chord change the next shifted step plays the new one",
          "new " + Num(newChord, 1) + " dB, old " + Num(oldChord, 1) + " dB");
}

/// Mean ms from each step boundary until half the energy is at the new step's pitch, for steps
/// into `second` and back into `first`.
std::pair<double, double> StepLagMs(double first, double second, int block)
{
    const double base = 220.0;
    const auto out = Render(Sine(base, 6.0), Legato(first, second, 1.0), block); // 1/8 = 250 ms
    const auto step = static_cast<size_t>(0.25 * 48000.0);
    constexpr int kWindow = 256;
    double lag[2] = {0.0, 0.0};
    int count[2] = {0, 0};

    for (size_t k = 4; (k + 1) * step + 2048 < out.size(); ++k)
    {
        const int into = static_cast<int>(k % 2); // 1: into `second`
        const double newHz = base * std::pow(2.0, (into ? second : first) / 12.0);
        const double oldHz = base * std::pow(2.0, (into ? first : second) / 12.0);
        double ms = 1000.0;

        for (size_t centre = k * step - kWindow; centre < k * step + 9600; centre += 8)
        {
            const float* w = out.data() + centre - kWindow / 2;

            if (Goertzel(w, kWindow, newHz) >= Goertzel(w, kWindow, oldHz))
            {
                ms = (static_cast<double>(centre) - static_cast<double>(k * step)) * 1000.0 / 48000.0;
                break;
            }
        }

        lag[into] += ms;
        ++count[into];
    }

    return {lag[1] / count[1], lag[0] / count[0]};
}

void TestStepLag()
{
    std::cout << "\n--- A step's pitch is heard at once ---\n";

    for (const int block : {64, 100, 512})
    {
        // From one shifted step to the next the shifter only changes speed: Signalsmith took 40-50 ms.
        const auto [up, down] = StepLagMs(4.0, 7.0, block);
        // Into and out of a 0 st step it cross-fades over 5 ms.
        const auto [in, out] = StepLagMs(0.0, 12.0, block);
        Check(up < 1.0 && down < 1.0 && in < 5.0 && out < 5.0, "block " + std::to_string(block),
              "+4->+7 " + Num(up) + ", +7->+4 " + Num(down) + ", 0->+12 " + Num(in) + ", +12->0 " + Num(out) + " ms");
    }
}

void TestPitch()
{
    std::cout << "\n--- Held steps are in tune ---\n";

    for (const double sampleRate : {44100.0, 48000.0, 96000.0})
    {
        double worst = 0.0;
        std::string where;

        for (const double note : {82.41, 196.0, 329.63})
        {
            for (const double shift : {-24.0, -12.0, -5.0, 4.0, 7.0, 12.0, 24.0})
            {
                const double target = note * std::pow(2.0, shift / 12.0);

                // Low E two octaves down is 20.6 Hz: its period is longer than the shifter goes
                // between splices, so it has no clean fundamental to measure, and no rig plays it.
                if (target < 40.0)
                {
                    continue;
                }

                const auto out =
                    Render(Harmonic(note, 0.0, 2.0, 2.0, sampleRate), Legato(shift, shift, 0.0), 64, sampleRate);
                const auto n = static_cast<int>(0.5 * sampleRate);
                const double cents =
                    PitchErrorCents(out.data() + static_cast<size_t>(1.2 * sampleRate), n, target, sampleRate);

                if (std::abs(cents) >= std::abs(worst))
                {
                    worst = cents;
                    where = Num(note) + " Hz " + Num(shift, 0) + " st";
                }
            }
        }

        Check(std::abs(worst) <= 3.0, "within 3 cents at " + Num(sampleRate / 1000.0, 1) + " kHz",
              "worst " + Num(worst) + " cents, " + where);
    }
}

void TestNoClicks()
{
    std::cout << "\n--- Legato steps never click ---\n";
    const auto sine = Sine(220.0, 4.0);
    // The steepest the shifted sine itself moves in one sample.
    const double ownSlope = 0.3 * 2.0 * kPi * 220.0 * std::pow(2.0, 7.0 / 12.0) / 48000.0;

    for (const auto& [first, second] : {std::pair{0.0, 7.0}, std::pair{4.0, 7.0}, std::pair{-12.0, 7.0}})
    {
        const auto out = Render(sine, Legato(first, second, 1.0));
        double worst = 0.0;

        for (size_t n = 24000; n + 1 < out.size(); ++n)
        {
            worst = std::max(worst, static_cast<double>(std::abs(out[n + 1] - out[n])));
        }

        Check(worst < 1.3 * ownSlope, "{" + Num(first, 0) + ", " + Num(second, 0) + "}: no jump at a step boundary",
              Num(worst / ownSlope) + "x the tone's own steepest");
    }

    // With every step at 0 st and the gate fully open the arp is transparent, at any mix: it used
    // to drop the last sample of every step to silence.
    for (const double mix : {1.0, 0.5})
    {
        auto params = Legato(0.0, 0.0, 6.0); // 1/32T: a boundary every 8 ms at 300 bpm
        params.push_back({"bpm", 300.0});
        params.push_back({"mix", mix});
        const auto tone = Harmonic(196.0, 0.0, 2.0, 2.0);
        const auto out = Render(tone, params);
        size_t differ = 0;

        for (size_t n = 0; n < out.size(); ++n)
        {
            differ += std::abs(out[n] - tone[n]) > 1e-6f ? 1 : 0;
        }

        Check(differ == 0, "0 st steps at a 100% gate pass the input untouched, mix " + Num(mix, 1),
              std::to_string(differ) + " samples differ");
    }
}

void TestBlockSizeIndependence()
{
    std::cout << "\n--- Steps start on their own sample in any block size ---\n";
    // Default arp (Major Triad, 1/8, 80% gate) on a note, and a fast Up-Down custom arp.
    const auto tone = Harmonic(146.83, 0.0, 3.0, 3.0);
    const Params defaults = {{"bpm", 120.0}};
    const Params fast = {{"pattern", 4.0}, {"numSteps", 5.0}, {"direction", 2.0}, {"stepRate", 5.0}, {"bpm", 140.0},
                         {"step0", -12.0}, {"step1", 0.0},    {"step2", 7.0},     {"step3", 0.0},    {"step4", 19.0}};

    for (const auto& params : {defaults, fast})
    {
        const auto reference = Render(tone, params, 64);
        double worst = 0.0;

        for (const int block : {1, 37, 100, 512})
        {
            const auto out = Render(tone, params, block);
            // Render leaves a tail shorter than a block unprocessed.
            const size_t rendered = std::min(out.size() / static_cast<size_t>(block) * static_cast<size_t>(block),
                                             reference.size() / 64 * 64);

            for (size_t n = 0; n < rendered; ++n)
            {
                worst = std::max(worst, static_cast<double>(std::abs(out[n] - reference[n])));
            }
        }

        Check(worst < 1e-6, params.size() > 1 ? "fast Up-Down custom arp" : "default arp",
              "largest difference from 64-sample blocks " + Num(worst, 9));
    }
}

void TestParamSafety()
{
    std::cout << "\n--- SetParam takes any key and value ---\n";
    AutoArpEffect effect;
    effect.Prepare(48000.0, 64);
    const double before = effect.GetParam("step3");
    std::string problem;

    // None of these is declared, and "stepMode" used to throw std::invalid_argument from std::stoi.
    for (const char* key : {"stepMode", "step", "step8", "step-1", "step01", "stepX", "steps", "step 1"})
    {
        try
        {
            effect.SetParam(key, 5.0);

            if (effect.GetParam(key) != 0.0)
            {
                problem += std::string(problem.empty() ? "" : "; ") + key + " reads back";
            }
        }
        catch (const std::exception& e)
        {
            problem += std::string(problem.empty() ? "" : "; ") + key + " threw " + e.what();
        }
    }

    Check(problem.empty() && effect.GetParam("step3") == before, "undeclared step* keys are ignored, without throwing",
          problem);

    effect.SetParam("step7", 9.0);
    effect.SetParam("step0", -30.0);
    Check(effect.GetParam("step7") == 9.0 && effect.GetParam("step0") == -24.0, "step0..step7 still set and read back");

    // A NaN used to be cast to an int: a pattern, numSteps or step rate of INT_MIN, read as an index.
    const double nan = std::nan("");

    for (const char* key : {"pattern", "numSteps", "stepRate", "direction", "bpm", "gate", "mix", "step2"})
    {
        effect.SetParam(key, nan);
    }

    std::vector<float> in(64, 0.1f);
    std::vector<float> outL(64);
    std::vector<float> outR(64);
    float* inputs[2] = {in.data(), in.data()};
    float* outputs[2] = {outL.data(), outR.data()};
    bool finite = true;

    for (int b = 0; b < 400; ++b)
    {
        effect.Process(inputs, outputs, 64);
        finite = finite && std::all_of(outL.begin(), outL.end(), [](float v) { return guitarfx::IsFinite(v); });
    }

    Check(finite && effect.GetParam("pattern") == 0.0 && effect.GetParam("numSteps") == 4.0 &&
              effect.GetParam("stepRate") == 1.0,
          "a NaN changes nothing");
}

void TestNoAudioThreadAllocations()
{
    std::cout << "\n--- Nothing allocates on the audio thread ---\n";
    AutoArpEffect effect;
    effect.Prepare(48000.0, 64);
    const auto tone = Harmonic(196.0, 0.0, 4.0, 4.0);
    std::vector<float> outL(64);
    std::vector<float> outR(64);

    // The keys exist before the audio thread sees them, as an automation binding's do: a Debug
    // std::string allocates a container proxy when it is made from a literal.
    const std::string patternKey = "pattern";
    const std::string directionKey = "direction";
    const std::string numStepsKey = "numSteps";
    const std::string stepKey = "step5";
    const std::string stepRateKey = "stepRate";
    const std::string undeclaredKey = "stepMode";
    const std::string pitchModeKey = "pitchMode";

    bool threw = false;
    const auto allocations = audio_thread_allocations::CountOnAudioThread([&] {
        size_t pos = 0;

        auto play = [&](int blocks) {
            for (int b = 0; b < blocks && pos + 64 <= tone.size(); ++b, pos += 64)
            {
                float* inputs[2] = {const_cast<float*>(tone.data() + pos), const_cast<float*>(tone.data() + pos)};
                float* outputs[2] = {outL.data(), outR.data()};
                effect.Process(inputs, outputs, 64);
            }
        };

        play(200);

        // What MIDI and DAW automation can move: every pattern, direction and length, the custom
        // steps (rebuilding the list), undeclared keys, and the pitch trigger, between blocks.
        for (double pattern = 0.0; pattern <= 5.0; pattern += 1.0)
        {
            effect.SetParam(patternKey, pattern);

            for (double direction = 0.0; direction <= 2.0; direction += 1.0)
            {
                effect.SetParam(directionKey, direction);
                effect.SetParam(numStepsKey, 8.0);
                effect.SetParam(stepKey, -7.0);
                effect.SetParam(stepRateKey, 2.0 + direction);

                try
                {
                    effect.SetParam(undeclaredKey, 1.0);
                }
                catch (const std::exception&)
                {
                    threw = true;
                }

                play(40);
            }
        }

        effect.SetParam(pitchModeKey, 1.0);
        play(200);
        effect.SetParam(pitchModeKey, 0.0);
        play(200);
    });

    Check(allocations.count == 0 && !threw, "SetParam, step changes, splices and the pitch trigger",
          audio_thread_allocations::Describe(allocations) + (threw ? ", and \"stepMode\" threw" : ""));
}

void TestDeclarations()
{
    std::cout << "\n--- Declarations ---\n";
    guitarfx::RegisterAutoArpEffect();
    const auto info = guitarfx::EffectRegistry::Instance().GetTypeInfo(guitarfx::EffectGuids::kAutoArp);
    const guitarfx::ParameterDef* steps = nullptr;

    for (size_t i = 0; info && i < info->parameters.size(); ++i)
    {
        steps = info->parameters[i].id == "numSteps" ? &info->parameters[i] : steps;
    }

    // Stored presets hold 2..8, so the values stay; the labels are listed from the minimum, one per
    // step, as EffectParamSpec documents ("in value order").
    bool labelled =
        steps && steps->minValue == 2.0 && steps->maxValue == 8.0 && steps->step == 1.0 && steps->labels.size() == 7;

    for (size_t i = 0; labelled && i < steps->labels.size(); ++i)
    {
        labelled = steps->labels[i] == std::to_string(2 + i);
    }

    Check(labelled, "Steps keeps 2..8, with one label per value from its minimum");

    AutoArpEffect effect;
    effect.Prepare(48000.0, 64);
    Check(effect.GetLatencySamples() == 480, "reports the shifter's nominal 10 ms",
          std::to_string(effect.GetLatencySamples()) + " samples");
}
} // namespace

int main()
{
    std::cout << "=== AutoArpEffectTests ===" << std::endl;
    TestNoStaleAudio();
    TestStepLag();
    TestPitch();
    TestNoClicks();
    TestBlockSizeIndependence();
    TestNoAudioThreadAllocations();
    TestDeclarations();
    TestParamSafety();

    std::cout << "\n" << (gFailures == 0 ? "All passed" : std::to_string(gFailures) + " failed") << std::endl;
    return gFailures == 0 ? 0 : 1;
}

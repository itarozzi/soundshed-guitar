/**
 * @file TransposeEffectTests.cpp
 * @brief Transpose's two engines, the crossfades between its paths, and the global transpose.
 *
 *   - at 0 st the node passes its input through untouched and reports no latency, on either engine
 *   - shifting, it reports its engine's latency: 80 ms on High Quality, 16 ms on Low Latency
 *   - entering and leaving the 0 st bypass, and switching engine, never clicks or drops out
 *   - a shift that starts again after a bypass plays the input, not audio from before it
 *   - the Low Latency path's output does not depend on the block size
 *   - none of it allocates on the audio thread, and a NaN parameter changes nothing
 *   - the global transpose keeps its node running through 0 st on the Low Latency engine, so
 *     its knob fades in and out instead of jumping, and never replays stale audio
 */

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
#include <iostream>
#include <limits>
#include <string>
#include <vector>

#include "dsp/EffectGuids.h"
#include "dsp/GlobalChainEditor.h"
#include "dsp/SignalGraphExecutor.h"
#include "dsp/effects/BuiltinEffects.h"
#include "dsp/effects/TransposeEffect.h"
#include "helpers/AudioThreadAllocations.h"
#include "helpers/GuitarPhraseSynth.h"

namespace
{
constexpr double kPi = 3.14159265358979323846;
constexpr double kSampleRate = 48000.0;
constexpr int kBlockSize = 64;

using guitarfx::TransposeEffect;

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

std::vector<float> Sine(double frequency, double seconds, double amplitude = 0.3)
{
    std::vector<float> out(static_cast<size_t>(seconds * kSampleRate));

    for (size_t i = 0; i < out.size(); ++i)
    {
        out[i] = static_cast<float>(amplitude * std::sin(2.0 * kPi * frequency * static_cast<double>(i) / kSampleRate));
    }

    return out;
}

/// 1 kHz for `switchAt` seconds, then 300 Hz: which of the two a shifted output holds says
/// which moment of input it came from.
std::vector<float> ToneChange(double seconds, double switchAt)
{
    auto out = Sine(1000.0, seconds);
    const auto later = Sine(300.0, seconds);
    std::copy(later.begin() + static_cast<std::ptrdiff_t>(switchAt * kSampleRate), later.end(),
              out.begin() + static_cast<std::ptrdiff_t>(switchAt * kSampleRate));
    return out;
}

/// Power at `frequency` over x[from, to), by Goertzel.
double PowerAt(const std::vector<float>& x, size_t from, size_t to, double frequency)
{
    const double w = 2.0 * kPi * frequency / kSampleRate;
    const double c = 2.0 * std::cos(w);
    double s1 = 0.0;
    double s2 = 0.0;

    for (size_t i = from; i < to && i < x.size(); ++i)
    {
        const double s0 = x[i] + c * s1 - s2;
        s2 = s1;
        s1 = s0;
    }

    return (s1 * s1 + s2 * s2 - c * s1 * s2) / static_cast<double>(to - from);
}

double Rms(const std::vector<float>& x, size_t from, size_t to)
{
    double sum = 0.0;

    for (size_t i = from; i < to && i < x.size(); ++i)
    {
        sum += static_cast<double>(x[i]) * x[i];
    }

    return std::sqrt(sum / std::max<size_t>(1, to - from));
}

double MaxStep(const std::vector<float>& x, size_t from)
{
    double worst = 0.0;

    for (size_t i = std::max<size_t>(from, 1); i < x.size(); ++i)
    {
        worst = std::max(worst, static_cast<double>(std::abs(x[i] - x[i - 1])));
    }

    return worst;
}

double QuietestRms(const std::vector<float>& x, size_t from, size_t window)
{
    double quietest = 1.0e9;

    for (size_t start = from; start + window <= x.size(); start += window / 2)
    {
        quietest = std::min(quietest, Rms(x, start, start + window));
    }

    return quietest;
}

/// Runs mono `input` through `process` in blocks of `blockSize`, calling `atBlock` with each
/// block's start before it is processed. Returns the left output.
std::vector<float> Render(const std::vector<float>& input, int blockSize,
                          const std::function<void(float**, float**, int)>& process,
                          const std::function<void(size_t)>& atBlock = {})
{
    std::vector<float> out(input.size(), 0.0f);
    std::vector<float> inL(static_cast<size_t>(blockSize));
    std::vector<float> inR(static_cast<size_t>(blockSize));
    std::vector<float> outL(static_cast<size_t>(blockSize));
    std::vector<float> outR(static_cast<size_t>(blockSize));

    for (size_t pos = 0; pos < input.size(); pos += static_cast<size_t>(blockSize))
    {
        const int n = static_cast<int>(std::min<size_t>(static_cast<size_t>(blockSize), input.size() - pos));

        if (atBlock)
        {
            atBlock(pos);
        }

        std::copy_n(input.begin() + static_cast<std::ptrdiff_t>(pos), n, inL.begin());
        std::copy_n(input.begin() + static_cast<std::ptrdiff_t>(pos), n, inR.begin());
        float* inputs[2] = {inL.data(), inR.data()};
        float* outputs[2] = {outL.data(), outR.data()};
        process(inputs, outputs, n);
        std::copy_n(outL.begin(), n, out.begin() + static_cast<std::ptrdiff_t>(pos));
    }

    return out;
}

std::vector<float> RenderEffect(TransposeEffect& effect, const std::vector<float>& input,
                                const std::function<void(size_t)>& atBlock = {}, int blockSize = kBlockSize)
{
    return Render(input, blockSize, [&](float** in, float** out, int n) { effect.Process(in, out, n); }, atBlock);
}

void TestTransparentAtZero()
{
    std::cout << "\n--- Transparent at 0 st ---\n";

    for (const double engine : {0.0, 1.0})
    {
        TransposeEffect effect;
        effect.Prepare(kSampleRate, kBlockSize);
        effect.SetParam("engine", engine);
        const auto input = guitarfx::test::RenderPhrase({{0.05, 0.3, 110.0, 0.3}}, 0.5, kSampleRate, 2);
        const auto output = RenderEffect(effect, input);
        Check(output == input && effect.GetLatencySamples() == 0,
              std::string(engine > 0.5 ? "Low Latency" : "High Quality") + " passes the input through untouched",
              "latency " + std::to_string(effect.GetLatencySamples()));
    }
}

void TestLatencyByEngine()
{
    std::cout << "\n--- Latency by engine ---\n";
    TransposeEffect effect;
    effect.Prepare(kSampleRate, kBlockSize);
    effect.SetParam("semitones", -2.0);
    const int highQuality = effect.GetLatencySamples();
    effect.SetParam("engine", 1.0);
    const int lowLatency = effect.GetLatencySamples();
    Check(highQuality == 3840, "High Quality reports 80 ms", std::to_string(highQuality));
    Check(lowLatency == 768, "Low Latency reports 16 ms", std::to_string(lowLatency));
}

void TestSwitchesNeverClick()
{
    std::cout << "\n--- Path changes crossfade ---\n";
    TransposeEffect effect;
    effect.Prepare(kSampleRate, kBlockSize);
    effect.SetParam("engine", 1.0);
    const auto input = Sine(220.0, 2.0);
    const auto output = RenderEffect(effect, input, [&](size_t pos) {
        const double t = static_cast<double>(pos) / kSampleRate;
        effect.SetParam("semitones", t < 0.3 ? 0.0 : (t < 0.6 ? -2.0 : (t < 0.9 ? 0.0 : -3.0)));
        effect.SetParam("engine", (t >= 1.2 && t < 1.6) ? 0.0 : 1.0);
    });

    // A hard switch between paths whose latencies differ jumps by up to the tone's full swing.
    const double inputStep = MaxStep(input, 1);
    const double worstStep = MaxStep(output, 1);
    const double level = Rms(input, 0, input.size());
    // A crossfade between two pitches, or two latencies, beats for a few milliseconds; a path that
    // started on silence would leave 10 ms or more of it. 20 ms windows tell the two apart.
    const double quietest = QuietestRms(output, 960, 960);
    Check(worstStep < 1.5 * inputStep, "no step at any switch",
          Num(worstStep, 4) + " against the tone's own " + Num(inputStep, 4));
    Check(quietest > 0.25 * level, "no dropout at any switch", Num(20.0 * std::log10(quietest / level)) + " dB");
}

void TestNoStaleReplay()
{
    std::cout << "\n--- A restarted shift plays the input, not the past ---\n";

    for (const double engine : {0.0, 1.0})
    {
        TransposeEffect effect;
        effect.Prepare(kSampleRate, kBlockSize);
        effect.SetParam("engine", engine);
        const auto input = ToneChange(1.4, 0.4);
        // Shifted on 1 kHz, bypassed while the input moves to 300 Hz, then shifted again.
        const auto output = RenderEffect(effect, input, [&](size_t pos) {
            const double t = static_cast<double>(pos) / kSampleRate;
            effect.SetParam("semitones", (t < 0.3 || t >= 0.8) ? -2.0 : 0.0);
        });

        const double ratio = std::exp2(-2.0 / 12.0);
        const auto from = static_cast<size_t>(0.81 * kSampleRate);
        const auto to = static_cast<size_t>(0.95 * kSampleRate);
        const double stale = PowerAt(output, from, to, 1000.0 * ratio);
        const double current = PowerAt(output, from, to, 300.0 * ratio);
        const double db = 10.0 * std::log10((stale + 1.0e-20) / (current + 1.0e-20));
        Check(db < -40.0, std::string(engine > 0.5 ? "Low Latency" : "High Quality") + " resumes on the input",
              "stale tone " + Num(db, 1) + " dB under the current one");
    }
}

void TestBlockSizeIndependent()
{
    std::cout << "\n--- Low Latency output does not depend on the block size ---\n";
    const auto input = guitarfx::test::RenderPhrase(
        {{0.05, 0.3, 82.41, 0.3}, {0.4, 0.3, 110.0, 0.3}, {0.8, 0.4, 146.83, 0.3}}, 1.3, kSampleRate, 6);
    std::vector<std::vector<float>> outputs;

    for (const int blockSize : {64, 1, 37, 512})
    {
        TransposeEffect effect;
        effect.Prepare(kSampleRate, 512);
        effect.SetParam("engine", 1.0);
        effect.SetParam("semitones", -5.0);
        outputs.push_back(RenderEffect(effect, input, {}, blockSize));
    }

    const bool same = std::all_of(outputs.begin(), outputs.end(), [&](const auto& o) { return o == outputs.front(); });
    Check(same, "blocks of 64, 1, 37 and 512 give the same output, sample for sample");
}

void TestAllocationsAndNaN()
{
    std::cout << "\n--- Audio thread allocations, NaN parameters ---\n";
    TransposeEffect effect;
    effect.Prepare(kSampleRate, kBlockSize);
    effect.SetParam("semitones", 3.0);
    const auto input =
        guitarfx::test::RenderPhrase({{0.05, 0.6, 110.0, 0.3}, {0.7, 0.6, 146.83, 0.3}}, 2.0, kSampleRate, 8);
    // Warm both engines once, off the audio thread.
    (void)RenderEffect(effect, input, [&](size_t pos) { effect.SetParam("engine", (pos / 4800) % 2 ? 0.0 : 1.0); });

    const std::string engineKey = "engine";
    const std::string semitonesKey = "semitones";
    std::vector<float> inL(kBlockSize);
    std::vector<float> inR(kBlockSize);
    std::vector<float> outL(kBlockSize);
    std::vector<float> outR(kBlockSize);
    const auto allocations = audio_thread_allocations::CountOnAudioThread([&] {
        for (size_t pos = 0; pos + kBlockSize <= input.size(); pos += kBlockSize)
        {
            if (pos % 3200 == 0)
            {
                const auto step = pos / 3200;
                effect.SetParam(engineKey, step % 3 == 0 ? 0.0 : 1.0);
                effect.SetParam(semitonesKey, static_cast<double>(static_cast<int>(step % 7) * 4 - 12));
            }

            std::copy_n(input.begin() + static_cast<std::ptrdiff_t>(pos), kBlockSize, inL.begin());
            std::copy_n(input.begin() + static_cast<std::ptrdiff_t>(pos), kBlockSize, inR.begin());
            float* inputs[2] = {inL.data(), inR.data()};
            float* outputs[2] = {outL.data(), outR.data()};
            effect.Process(inputs, outputs, kBlockSize);
        }
    });

    Check(allocations.count == 0, "engine switches, shifts and splices allocate nothing",
          audio_thread_allocations::Describe(allocations));

    effect.SetParam("semitones", -4.0);
    effect.SetParam("engine", 1.0);
    effect.SetParam("semitones", std::numeric_limits<double>::quiet_NaN());
    effect.SetParam("engine", std::numeric_limits<double>::quiet_NaN());
    Check(effect.GetParam("semitones") == -4.0 && effect.GetParam("engine") == 1.0, "a NaN parameter changes nothing");
}

/// The global pre-chain, built and driven the way the mixer does: its executor from
/// LivePreChain(), its edits through GlobalChainEditor.
struct GlobalChain
{
    guitarfx::GlobalSignalChainConfig config;
    guitarfx::SignalGraphExecutor pre;
    guitarfx::SignalGraphExecutor post;
    guitarfx::GlobalChainEditor editor{config, pre, post};

    GlobalChain()
    {
        guitarfx::GlobalChainEditor::NormalizeConfig(config);
        pre.SetGraph(guitarfx::GlobalChainEditor::LivePreChain(config.preChainGraph));
        pre.Prepare(kSampleRate, kBlockSize);
    }

    [[nodiscard]] const guitarfx::GraphNode& ConfigNode() const
    {
        return *config.preChainGraph.FindNode("global_transpose");
    }

    [[nodiscard]] guitarfx::EffectProcessor& Running()
    {
        return *pre.GetNodeProcessor("global_transpose");
    }

    std::vector<float> Render(const std::vector<float>& input, const std::function<void(size_t)>& atBlock)
    {
        return ::Render(input, kBlockSize, [&](float** in, float** out, int n) { pre.Process(in, out, n); }, atBlock);
    }
};

void TestGlobalNodeKeepsRunning()
{
    std::cout << "\n--- The global transpose node keeps running ---\n";
    GlobalChain chain;
    auto& running = chain.Running();
    Check(running.IsEnabled() && running.GetParam("engine") == 1.0 && running.GetParam("semitones") == 0.0,
          "at rest it runs on the Low Latency engine at 0 st");

    chain.editor.SetTranspose(-2);
    Check(chain.ConfigNode().enabled && chain.ConfigNode().params.at("semitones") == -2.0 && running.IsEnabled() &&
              running.GetParam("semitones") == -2.0,
          "a shift is saved as an enabled node and played");

    chain.editor.SetTranspose(0);
    Check(!chain.ConfigNode().enabled && running.IsEnabled() && running.GetParam("semitones") == 0.0,
          "back at 0 st the saved node is off but the running one only goes transparent");

    chain.editor.SetTranspose(-4);
    chain.editor.SetTransposeEnabled(false);
    Check(chain.ConfigNode().params.at("semitones") == -4.0 && running.IsEnabled() &&
              running.GetParam("semitones") == 0.0,
          "switched off, it keeps the setting and plays 0 st");

    chain.editor.SetTransposeEnabled(true);
    Check(running.GetParam("semitones") == -4.0, "switched back on, it plays the setting again");

    auto stored = chain.config.preChainGraph;
    stored.FindNode("global_transpose")->enabled = false;
    stored.FindNode("global_transpose")->params["semitones"] = -3.0;
    const auto live = guitarfx::GlobalChainEditor::LivePreChain(stored);
    const auto* liveNode = live.FindNode("global_transpose");
    Check(liveNode->enabled && liveNode->params.at("semitones") == 0.0 && liveNode->params.at("engine") == 1.0,
          "a stored chain with the node off is rebuilt running at 0 st");
}

void TestGlobalSwitchingAudio()
{
    std::cout << "\n--- The global transpose knob fades ---\n";
    GlobalChain chain;
    const auto input = Sine(220.0, 1.6);
    std::vector<int> latencies;
    const auto output = chain.Render(input, [&](size_t pos) {
        const double t = static_cast<double>(pos) / kSampleRate;

        if (pos % static_cast<size_t>(0.3 * kSampleRate) < kBlockSize)
        {
            latencies.push_back(chain.pre.GetTotalLatencySamples());
            chain.editor.SetTranspose(t < 0.2 ? 0 : (t < 0.5 ? -2 : (t < 0.8 ? 0 : (t < 1.1 ? -5 : 0))));
        }
    });

    const double inputStep = MaxStep(input, 1);
    const double worstStep = MaxStep(output, 1);
    const double level = Rms(input, 0, input.size());
    const double quietest = QuietestRms(output, 960, 960);
    Check(worstStep < 1.5 * inputStep, "no step as the knob moves",
          Num(worstStep, 4) + " against the tone's own " + Num(inputStep, 4));
    Check(quietest > 0.25 * level, "no dropout as the knob moves", Num(20.0 * std::log10(quietest / level)) + " dB");
    // Sampled just before each change: 0 st at rest, then shifting, at rest, shifting, at rest.
    const bool reported = latencies.size() >= 6 && latencies[1] == 0 && latencies[2] == 768 && latencies[3] == 0 &&
                          latencies[4] == 768 && latencies[5] == 0;
    std::string detail;

    for (const int l : latencies)
    {
        detail += std::to_string(l) + " ";
    }

    Check(reported, "it reports 16 ms while shifting and nothing at rest", detail);
}

void TestGlobalNoStaleReplay()
{
    std::cout << "\n--- The global transpose never replays stale audio ---\n";
    GlobalChain chain;
    const auto input = ToneChange(1.4, 0.4);
    const auto output = chain.Render(input, [&](size_t pos) {
        const double t = static_cast<double>(pos) / kSampleRate;

        if (pos < kBlockSize || std::abs(t - 0.3) < 0.5 * kBlockSize / kSampleRate ||
            std::abs(t - 0.8) < 0.5 * kBlockSize / kSampleRate)
        {
            chain.editor.SetTranspose((t < 0.3 || t >= 0.8) ? -2 : 0);
        }
    });

    const double ratio = std::exp2(-2.0 / 12.0);
    const auto from = static_cast<size_t>(0.81 * kSampleRate);
    const auto to = static_cast<size_t>(0.95 * kSampleRate);
    const double stale = PowerAt(output, from, to, 1000.0 * ratio);
    const double current = PowerAt(output, from, to, 300.0 * ratio);
    const double db = 10.0 * std::log10((stale + 1.0e-20) / (current + 1.0e-20));
    Check(db < -40.0, "back on after a stretch at 0 st, it shifts the input playing now",
          "stale tone " + Num(db, 1) + " dB under the current one");
}
} // namespace

int main()
{
    guitarfx::RegisterAllEffects();
    std::cout << "=== Transpose ===\n";
    TestTransparentAtZero();
    TestLatencyByEngine();
    TestSwitchesNeverClick();
    TestNoStaleReplay();
    TestBlockSizeIndependent();
    TestAllocationsAndNaN();
    TestGlobalNodeKeepsRunning();
    TestGlobalSwitchingAudio();
    TestGlobalNoStaleReplay();
    std::cout << "\n" << (gFailures == 0 ? "All tests passed" : std::to_string(gFailures) + " failure(s)") << std::endl;
    return gFailures == 0 ? 0 : 1;
}

/**
 * @file ParamGlideTests.cpp
 * @brief Utility controls that are moved while playing must not click.
 *
 *   - Mixer (the merge node of every split): a level change glides; a delay change crossfades;
 *     an input unmuted after a mute plays what is arriving now, not what its delay line held
 *     from before the mute, which it replayed for the length of its delay
 *   - Gain: a change glides; a gain set before audio starts lands at once
 *   - Chorus, Flanger and Doubler: a Delay or Time change glides the read head
 *   - Octave: leaving its bypass (Mix up from 0) crossfades rather than jumping 80 ms in time
 *   - NAM amp: Input and Output glide; a model's output, being distorted, is judged against the
 *     same model at the new gain throughout, not against a sine's slope
 *
 * A 220 Hz sine moves by at most 2 pi 220 / 48000 = 0.029 of its peak per sample, so a
 * largest sample-to-sample step far above that is a click. The tone is 220.5 Hz so that a change
 * half a second in lands on a peak: at 220 Hz it lands on a zero crossing, where a step is silent.
 */

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <map>
#include <iostream>
#include <string>
#include <vector>

#include "dsp/EffectGuids.h"
#include "dsp/SignalGraphExecutor.h"
#include "dsp/effects/BuiltinEffects.h"
#include "dsp/effects/ChorusEffect.h"
#include "dsp/effects/DoublerEffect.h"
#include "dsp/effects/FlangerEffect.h"
#include "dsp/effects/GainEffect.h"
#include "dsp/effects/OctaveEffect.h"
#include "dsp/effects/OptimizedNAMAmpEffect.h"
#include "presets/PresetTypes.h"

namespace
{
using guitarfx::GraphEdge;
using guitarfx::GraphNode;
using guitarfx::SignalGraph;
using guitarfx::SignalGraphExecutor;

constexpr double kRate = 48000.0;
constexpr int kBlock = 64;
constexpr double kPi = 3.14159265358979323846;

int gChecks = 0;
int gFailures = 0;

void Check(bool ok, const std::string& what, const std::string& detail = "")
{
    ++gChecks;
    gFailures += ok ? 0 : 1;
    std::cout << (ok ? "  [PASS] " : "  [FAIL] ") << what;

    if (!detail.empty())
    {
        std::cout << " (" << detail << ")";
    }

    std::cout << std::endl;
}

GraphNode Node(const std::string& id, const std::string& type)
{
    GraphNode node;
    node.id = id;
    node.type = type;
    return node;
}

GraphEdge Edge(const std::string& from, const std::string& to, int fromPort = 0, int toPort = 0)
{
    GraphEdge edge;
    edge.from = from;
    edge.to = to;
    edge.fromPort = fromPort;
    edge.toPort = toPort;
    return edge;
}

/// input -> split -> two empty branches -> mix -> output, with the mixer's starting settings.
SignalGraph TwoBranchSplit(const std::map<std::string, double>& mixParams)
{
    SignalGraph graph;
    graph.nodes = {Node("__input__", "input"), Node("split", guitarfx::EffectGuids::kSplitter),
                   Node("mix", guitarfx::EffectGuids::kMixer), Node("__output__", "output")};
    graph.nodes[2].params = mixParams;
    graph.edges = {Edge("__input__", "split"), Edge("split", "mix", 0, 0), Edge("split", "mix", 1, 1),
                   Edge("mix", "__output__")};
    return graph;
}

std::vector<float> Sine(double hz, double seconds, float amplitude)
{
    std::vector<float> out(static_cast<std::size_t>(seconds * kRate));

    for (std::size_t i = 0; i < out.size(); ++i)
    {
        out[i] = amplitude * static_cast<float>(std::sin(2.0 * kPi * hz * static_cast<double>(i) / kRate));
    }

    return out;
}

/// Noise, so audio from one moment never matches audio from another.
std::vector<float> Noise(double seconds, float amplitude)
{
    std::vector<float> out(static_cast<std::size_t>(seconds * kRate));
    std::uint32_t state = 12345u;

    for (float& s : out)
    {
        state = state * 1664525u + 1013904223u;
        s = amplitude * (static_cast<float>(state >> 8) / 8388608.0f - 1.0f);
    }

    return out;
}

/// Runs `input` through `executor` in blocks, calling `between(start)` before each; left side.
template <typename Between>
std::vector<float> Run(SignalGraphExecutor& executor, const std::vector<float>& input, Between between)
{
    std::vector<float> inL = input;
    std::vector<float> inR = input;
    std::vector<float> outL(input.size());
    std::vector<float> outR(input.size());

    for (std::size_t start = 0; start + kBlock <= input.size(); start += kBlock)
    {
        between(start);
        float* ins[2] = {inL.data() + start, inR.data() + start};
        float* outs[2] = {outL.data() + start, outR.data() + start};
        executor.Process(ins, outs, kBlock);
    }

    return outL;
}

/// The largest sample-to-sample step of `x` from `from` on, over its peak there.
double WorstStepOverPeak(const std::vector<float>& x, std::size_t from)
{
    float peak = 0.0f;
    float worst = 0.0f;

    for (std::size_t i = std::max<std::size_t>(from, 1); i < x.size(); ++i)
    {
        peak = std::max(peak, std::fabs(x[i]));
        worst = std::max(worst, std::fabs(x[i] - x[i - 1]));
    }

    return worst / std::max(peak, 1.0e-9f);
}

std::size_t BlockAt(double seconds)
{
    return static_cast<std::size_t>(seconds * kRate) / kBlock * kBlock;
}

void TestMixerLevelAndDelay()
{
    std::cout << "\nMixer: level and delay changes" << std::endl;
    const auto tone = Sine(220.5, 1.5, 0.2f);
    const std::size_t changeAt = BlockAt(0.5);

    SignalGraphExecutor level;
    level.SetGraph(TwoBranchSplit({}));
    level.Prepare(kRate, kBlock);
    const auto leveled = Run(level, tone, [&](std::size_t start) {
        if (start == changeAt)
        {
            level.SetNodeParam("mix", "level_0", -30.0);
        }
    });
    const double levelStep = WorstStepOverPeak(leveled, changeAt - kBlock);
    Check(levelStep < 0.04, "a branch level cut while a note plays glides", std::to_string(levelStep) + " of the peak");

    SignalGraphExecutor delay;
    delay.SetGraph(TwoBranchSplit({}));
    delay.Prepare(kRate, kBlock);
    const auto delayed = Run(delay, tone, [&](std::size_t start) {
        if (start == changeAt)
        {
            delay.SetNodeParam("mix", "delay_1", 1.7);
        }
    });
    const double delayStep = WorstStepOverPeak(delayed, changeAt - kBlock);
    Check(delayStep < 0.04, "a branch delay change crossfades the read position",
          std::to_string(delayStep) + " of the peak");
}

void TestMixerUnmuteAfterMute()
{
    std::cout << "\nMixer: unmute after a mute" << std::endl;
    // Branch 1 runs 20 ms late. Muted for half a second, then unmuted: once its 10 ms glide is
    // over, the output is what a never-muted mixer plays. It used to replay the 20 ms its delay
    // line held from before the mute.
    const auto noise = Noise(1.5, 0.1f);
    const std::map<std::string, double> params = {{"delay_1", 20.0}};
    const std::size_t muteAt = BlockAt(0.4);
    const std::size_t unmuteAt = BlockAt(0.9);

    SignalGraphExecutor reference;
    reference.SetGraph(TwoBranchSplit(params));
    reference.Prepare(kRate, kBlock);
    const auto expected = Run(reference, noise, [](std::size_t) {});

    SignalGraphExecutor muted;
    muted.SetGraph(TwoBranchSplit(params));
    muted.Prepare(kRate, kBlock);
    const auto heard = Run(muted, noise, [&](std::size_t start) {
        if (start == muteAt)
        {
            muted.SetNodeParam("mix", "mute_1", 1.0);
        }
        else if (start == unmuteAt)
        {
            muted.SetNodeParam("mix", "mute_1", 0.0);
        }
    });

    double worst = 0.0;
    double scale = 0.0;

    for (std::size_t i = unmuteAt + static_cast<std::size_t>(0.012 * kRate); i < unmuteAt + 2 * 960; ++i)
    {
        worst = std::max(worst, static_cast<double>(std::fabs(heard[i] - expected[i])));
        scale = std::max(scale, static_cast<double>(std::fabs(expected[i])));
    }

    Check(worst < 1.0e-4 * std::max(1.0, scale * 10.0), "an unmuted branch plays what arrives now",
          "largest difference " + std::to_string(worst) + " against a peak of " + std::to_string(scale));
}

void TestGain()
{
    std::cout << "\nGain" << std::endl;
    const auto tone = Sine(220.5, 1.0, 0.2f);
    const std::size_t changeAt = BlockAt(0.5);
    guitarfx::GainEffect gain;
    gain.Prepare(kRate, kBlock);
    gain.SetParam("gainDb", -12.0);
    std::vector<float> inL = tone;
    std::vector<float> inR = tone;
    std::vector<float> outL(tone.size());
    std::vector<float> outR(tone.size());

    for (std::size_t start = 0; start + kBlock <= tone.size(); start += kBlock)
    {
        if (start == changeAt)
        {
            gain.SetParam("gainDb", 12.0);
        }

        float* ins[2] = {inL.data() + start, inR.data() + start};
        float* outs[2] = {outL.data() + start, outR.data() + start};
        gain.Process(ins, outs, kBlock);
    }

    const double firstRatio = outL[100] / tone[100];
    Check(std::fabs(firstRatio - std::pow(10.0, -12.0 / 20.0)) < 1.0e-5, "a gain set before audio lands at once",
          std::to_string(firstRatio));
    const double step = WorstStepOverPeak(outL, changeAt - kBlock);
    Check(step < 0.04, "a 24 dB gain change while a note plays glides", std::to_string(step) + " of the peak");
}

/// Runs a 220.5 Hz tone through `effect` with `settings`, sets `key` to `value` half a second in,
/// and returns the largest step from just before the change on, over the peak.
double StepWhenMoved(guitarfx::EffectProcessor& effect, const std::map<std::string, double>& settings,
                     const std::string& key, double value)
{
    effect.Prepare(kRate, kBlock);

    for (const auto& [name, setting] : settings)
    {
        effect.SetParam(name, setting);
    }

    const auto tone = Sine(220.5, 1.0, 0.2f);
    const std::size_t changeAt = BlockAt(0.5);
    std::vector<float> inL = tone;
    std::vector<float> inR = tone;
    std::vector<float> outL(tone.size());
    std::vector<float> outR(tone.size());

    for (std::size_t start = 0; start + kBlock <= tone.size(); start += kBlock)
    {
        if (start == changeAt)
        {
            effect.SetParam(key, value);
        }

        float* ins[2] = {inL.data() + start, inR.data() + start};
        float* outs[2] = {outL.data() + start, outR.data() + start};
        effect.Process(ins, outs, kBlock);
    }

    return WorstStepOverPeak(outL, changeAt - kBlock);
}

void TestModulationDelayGlides()
{
    std::cout << "\nChorus, Flanger and Doubler: delay changes" << std::endl;
    guitarfx::ChorusEffect chorus;
    const double chorusStep =
        StepWhenMoved(chorus, {{"depth", 0.0}, {"feedback", 0.0}, {"mix", 0.5}, {"delay", 15.0}}, "delay", 25.0);
    Check(chorusStep < 0.04, "Chorus Delay moved while a note plays glides",
          std::to_string(chorusStep) + " of the peak");

    guitarfx::FlangerEffect flanger;
    const double flangerStep =
        StepWhenMoved(flanger, {{"depth", 0.0}, {"feedback", 0.0}, {"mix", 0.5}, {"delay", 1.0}}, "delay", 4.0);
    Check(flangerStep < 0.04, "Flanger Delay moved while a note plays glides",
          std::to_string(flangerStep) + " of the peak");

    guitarfx::DoublerEffect doubler;
    const double doublerStep = StepWhenMoved(doubler, {{"mix", 0.3}, {"time", 6.0}}, "time", 16.0);
    Check(doublerStep < 0.04, "Doubler Time moved while a note plays glides",
          std::to_string(doublerStep) + " of the peak");

    guitarfx::OctaveEffect octave;
    const double octaveStep = StepWhenMoved(octave, {{"mix", 0.0}}, "mix", 0.01);
    Check(octaveStep < 0.04, "Octave leaving its bypass crossfades", std::to_string(octaveStep) + " of the peak");
}

/// The largest step of `x` over `[from, to)`.
float WorstStep(const std::vector<float>& x, std::size_t from, std::size_t to)
{
    float worst = 0.0f;

    for (std::size_t i = std::max<std::size_t>(from, 1); i < std::min(to, x.size()); ++i)
    {
        worst = std::max(worst, std::fabs(x[i] - x[i - 1]));
    }

    return worst;
}

void TestNamAmpGains()
{
    std::cout << "\nNAM amp: Input and Output" << std::endl;

    // No model: the amp passes its input through Input's gain, so a sine shows a step plainly.
    guitarfx::OptimizedNAMAmpEffect empty;
    const double inputStep = StepWhenMoved(empty, {{"inputGain", -12.0}}, "inputGain", 12.0);
    Check(inputStep < 0.04, "Input moved while a note plays glides", std::to_string(inputStep) + " of the peak");

    const auto model = std::filesystem::path(GUITARFX_TEST_RESOURCES_DIR) /
                       "assets/amps/Guitar/Emil Rohbe/VOX AC15 NAM PACK/VOX AC15 Capture 3 - Emil Rohbe.nam";
    const auto render = [&](double startDb, double endDb) {
        guitarfx::OptimizedNAMAmpEffect amp;

        if (!amp.LoadResource(model))
        {
            return std::vector<float>{};
        }

        amp.Prepare(kRate, kBlock);
        amp.SetParam("outputGain", startDb);
        const auto tone = Sine(220.5, 1.0, 0.2f);
        const std::size_t changeAt = BlockAt(0.5);
        std::vector<float> inL = tone;
        std::vector<float> inR = tone;
        std::vector<float> outL(tone.size());
        std::vector<float> outR(tone.size());

        for (std::size_t start = 0; start + kBlock <= tone.size(); start += kBlock)
        {
            if (start == changeAt)
            {
                amp.SetParam("outputGain", endDb);
            }

            float* ins[2] = {inL.data() + start, inR.data() + start};
            float* outs[2] = {outL.data() + start, outR.data() + start};
            amp.Process(ins, outs, kBlock);
        }

        return outL;
    };

    // Turned up 24 dB: no step may be larger than the loud amp's own largest one.
    const auto moved = render(-12.0, 12.0);
    const auto loud = render(12.0, 12.0);

    if (moved.empty() || loud.empty())
    {
        Check(false, "the test model loads", model.string());
        return;
    }

    const std::size_t changeAt = BlockAt(0.5);
    const float movedStep = WorstStep(moved, changeAt - kBlock, changeAt + 2 * kBlock);
    const float loudStep = WorstStep(loud, changeAt - kBlock, changeAt + 2 * kBlock);
    Check(movedStep <= 1.1f * loudStep, "Output moved while a note plays glides",
          std::to_string(movedStep) + " against the loud amp's " + std::to_string(loudStep));
}
} // namespace

int main()
{
    std::cout << "=== ParamGlideTests ===" << std::endl;
    guitarfx::RegisterAllEffects();
    TestMixerLevelAndDelay();
    TestMixerUnmuteAfterMute();
    TestGain();
    TestModulationDelayGlides();
    TestNamAmpGains();
    std::cout << "\n" << (gChecks - gFailures) << "/" << gChecks << " checks passed" << std::endl;
    return gFailures == 0 ? 0 : 1;
}

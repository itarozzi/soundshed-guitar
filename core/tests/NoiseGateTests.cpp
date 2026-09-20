/**
 * @file NoiseGateTests.cpp
 * @brief Tests for the noise gate, covering both what it stores and what it does.
 *
 * Parameter plumbing:
 *   - the registry ids are the ones the rest of the codebase writes, and the suffixed
 *     spellings shipped presets carry are declared as aliases of them
 *   - a node holding one parameter under two spellings keeps the canonical value, and a
 *     node holding only the legacy spelling keeps its value under the canonical id
 *   - a stored gate's threshold survives a graph rebuild, which is the regression: node
 *     params are applied in map order, so a duplicate key used to let "thresholdDb" sort
 *     after "threshold" and overwrite the user's setting with the registry default
 *   - out-of-range and non-finite values are clamped or dropped
 *
 * Gating behaviour:
 *   - a signal above the threshold passes at unity, one below it is attenuated to the floor
 *   - the gate closes as a ramp, not a step, with noise still present
 *   - hysteresis stops a level hovering at the threshold from chattering the gate
 *   - one detector drives both channels, so a quiet channel is not gated against a loud one
 *   - the mono fast path produces the same output as the stereo path
 *   - a non-finite input sample does not latch the gate shut
 */

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <iostream>
#include <limits>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "dsp/EffectGuids.h"
#include "dsp/EffectProcessor.h"
#include "dsp/EffectRegistry.h"
#include "dsp/GlobalChainEditor.h"
#include "dsp/SignalGraphExecutor.h"
#include "dsp/effects/NoiseGateEffect.h"
#include "presets/PresetTypes.h"

namespace
{
using namespace guitarfx;

constexpr double kSampleRate = 48000.0;
constexpr int kBlockSize = 128;
constexpr double kPi = 3.14159265358979323846;

using Params = std::vector<std::pair<std::string, double>>;

int gFailures = 0;
int gChecks = 0;

void Check(bool condition, const std::string& what, const std::string& detail = "")
{
    ++gChecks;

    if (condition)
    {
        std::cout << "  [PASS] " << what;
    }
    else
    {
        ++gFailures;
        std::cout << "  [FAIL] " << what;
    }

    if (!detail.empty())
    {
        std::cout << "  (" << detail << ")";
    }

    std::cout << std::endl;
}

std::string Num(double value, int precision = 4)
{
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), "%.*f", precision, value);
    return std::string(buffer);
}

double Db(double amplitude)
{
    return 20.0 * std::log10(std::max(amplitude, 1e-12));
}

double Amplitude(double dbfs)
{
    return std::pow(10.0, dbfs / 20.0);
}

std::unique_ptr<EffectProcessor> MakeGate(const Params& params)
{
    auto effect = EffectRegistry::Instance().Create(EffectGuids::kDynamicsGate);

    if (!effect)
    {
        return nullptr;
    }

    for (const auto& [key, value] : params)
    {
        effect->SetParam(key, value);
    }

    effect->Prepare(kSampleRate, kBlockSize);
    return effect;
}

/// A tone at 440 Hz -- above the sidechain high-pass, so the detector sees all of it.
std::vector<float> Tone(double amplitude, int samples, double frequency = 440.0, double startPhase = 0.0)
{
    std::vector<float> signal(static_cast<std::size_t>(samples));
    const double step = 2.0 * kPi * frequency / kSampleRate;

    for (std::size_t i = 0; i < signal.size(); ++i)
    {
        signal[i] = static_cast<float>(amplitude * std::sin(startPhase + step * static_cast<double>(i)));
    }

    return signal;
}

void Append(std::vector<float>& target, const std::vector<float>& tail)
{
    target.insert(target.end(), tail.begin(), tail.end());
}

struct StereoSignal
{
    std::vector<float> left;
    std::vector<float> right;
};

/// Runs the gate block by block, copying each block in and out so it never sees its own
/// output as input.
StereoSignal RunStereo(EffectProcessor& gate, const std::vector<float>& left, const std::vector<float>& right)
{
    StereoSignal out{std::vector<float>(left.size(), 0.0f), std::vector<float>(left.size(), 0.0f)};
    std::vector<float> inLeft(kBlockSize, 0.0f);
    std::vector<float> inRight(kBlockSize, 0.0f);
    std::vector<float> outLeft(kBlockSize, 0.0f);
    std::vector<float> outRight(kBlockSize, 0.0f);

    for (std::size_t offset = 0; offset < left.size(); offset += kBlockSize)
    {
        const std::size_t count = std::min<std::size_t>(kBlockSize, left.size() - offset);
        std::copy_n(left.begin() + static_cast<std::ptrdiff_t>(offset), count, inLeft.begin());
        std::copy_n(right.begin() + static_cast<std::ptrdiff_t>(offset), count, inRight.begin());

        float* inputs[2] = {inLeft.data(), inRight.data()};
        float* outputs[2] = {outLeft.data(), outRight.data()};
        gate.Process(inputs, outputs, static_cast<int>(count));

        std::copy_n(outLeft.begin(), count, out.left.begin() + static_cast<std::ptrdiff_t>(offset));
        std::copy_n(outRight.begin(), count, out.right.begin() + static_cast<std::ptrdiff_t>(offset));
    }

    return out;
}

std::vector<float> RunMono(EffectProcessor& gate, const std::vector<float>& input)
{
    std::vector<float> out(input.size(), 0.0f);
    std::vector<float> block(kBlockSize, 0.0f);
    std::vector<float> outBlock(kBlockSize, 0.0f);

    for (std::size_t offset = 0; offset < input.size(); offset += kBlockSize)
    {
        const std::size_t count = std::min<std::size_t>(kBlockSize, input.size() - offset);
        std::copy_n(input.begin() + static_cast<std::ptrdiff_t>(offset), count, block.begin());
        gate.ProcessMono(block.data(), outBlock.data(), static_cast<int>(count));
        std::copy_n(outBlock.begin(), count, out.begin() + static_cast<std::ptrdiff_t>(offset));
    }

    return out;
}

/// Peak magnitude over [first, last).
double Peak(const std::vector<float>& signal, std::size_t first, std::size_t last)
{
    double peak = 0.0;

    for (std::size_t i = first; i < std::min(last, signal.size()); ++i)
    {
        peak = std::max(peak, static_cast<double>(std::abs(signal[i])));
    }

    return peak;
}

const ParameterDef* FindParam(const EffectTypeInfo& info, const std::string& id)
{
    for (const auto& param : info.parameters)
    {
        if (param.id == id)
        {
            return &param;
        }
    }

    return nullptr;
}

void TestRegistration()
{
    std::cout << "\n-- registration --" << std::endl;

    const auto info = EffectRegistry::Instance().GetTypeInfo(EffectGuids::kDynamicsGate);
    Check(info.has_value(), "the gate is registered");

    if (!info.has_value())
    {
        return;
    }

    // These are the ids GlobalChainEditor::SetGate* and the UI knob write. A registry that
    // declares anything else puts a second spelling of the same parameter into stored nodes.
    for (const auto& [id, alias] : std::vector<std::pair<std::string, std::string>>{
             {"threshold", "thresholdDb"}, {"attack", "attackMs"}, {"hold", "holdMs"}, {"release", "releaseMs"}})
    {
        const auto* param = FindParam(*info, id);
        Check(param != nullptr, "the registry declares \"" + id + "\"");

        if (param != nullptr)
        {
            const bool declaresAlias =
                std::find(param->aliases.begin(), param->aliases.end(), alias) != param->aliases.end();
            Check(declaresAlias, "\"" + id + "\" declares \"" + alias + "\" as a legacy spelling");
        }
    }

    Check(FindParam(*info, "hysteresis") != nullptr, "the registry declares \"hysteresis\"");
    Check(FindParam(*info, "range") != nullptr, "the registry declares \"range\"");
}

void TestParamRoundTripAndClamping()
{
    std::cout << "\n-- parameter clamping --" << std::endl;

    auto gate = MakeGate({});
    Check(gate != nullptr, "the registry creates a gate");

    if (!gate)
    {
        return;
    }

    gate->SetParam("threshold", -42.0);
    Check(std::abs(gate->GetParam("threshold") - -42.0) < 1e-4, "threshold round-trips");
    Check(std::abs(gate->GetParam("thresholdDb") - -42.0) < 1e-4, "the legacy spelling reads the same parameter");

    gate->SetParam("threshold", 40.0);
    Check(std::abs(gate->GetParam("threshold") - NoiseGateEffect::kMaxThresholdDb) < 1e-4,
          "a threshold above the published range is clamped", Num(gate->GetParam("threshold")));

    gate->SetParam("hold", -5.0);
    Check(std::abs(gate->GetParam("hold") - NoiseGateEffect::kMinHoldMs) < 1e-4, "a negative hold is clamped");

    gate->SetParam("release", 1e9);
    Check(std::abs(gate->GetParam("release") - NoiseGateEffect::kMaxReleaseMs) < 1e-4,
          "a release beyond the published range is clamped");

    // A non-finite value must not reach the state: NaN compares false against every
    // threshold, which would leave the gate shut with no way back short of Reset().
    gate->SetParam("threshold", -30.0);
    gate->SetParam("threshold", std::nan(""));
    Check(std::abs(gate->GetParam("threshold") - -30.0) < 1e-4, "a NaN threshold is dropped, keeping the last good one",
          Num(gate->GetParam("threshold")));
}

void TestLegacyParamKeysFold()
{
    std::cout << "\n-- legacy parameter keys --" << std::endl;

    GraphNode legacyOnly;
    legacyOnly.id = "gate";
    legacyOnly.type = EffectGuids::kDynamicsGate;
    legacyOnly.params["thresholdDb"] = -55.0;
    legacyOnly.params["releaseMs"] = 80.0;
    CanonicalizeNodeParams(legacyOnly);

    Check(legacyOnly.params.count("thresholdDb") == 0 && legacyOnly.params.count("releaseMs") == 0,
          "a shipped preset's suffixed keys are folded away");
    Check(legacyOnly.params.count("threshold") == 1 && legacyOnly.params.at("threshold") == -55.0 &&
              legacyOnly.params.at("release") == 80.0,
          "and their values arrive under the canonical ids");

    // What an older build left behind: the editor wrote "threshold" while the registry had
    // already seeded its default under "thresholdDb".
    GraphNode both;
    both.id = "gate";
    both.type = EffectGuids::kDynamicsGate;
    both.params["threshold"] = -42.0;
    both.params["thresholdDb"] = -60.0;
    CanonicalizeNodeParams(both);

    Check(both.params.size() == 1 && both.params.at("threshold") == -42.0,
          "with both present the canonical key wins, because that is the one the user set");
}

void TestStoredThresholdSurvivesRebuild()
{
    std::cout << "\n-- a stored gate through a graph rebuild --" << std::endl;

    SignalGraph graph;
    graph.nodes.push_back({"in", kNodeTypeInput, "", "Input", true});

    GraphNode gate;
    gate.id = "gate";
    gate.type = EffectGuids::kDynamicsGate;
    gate.category = "dynamics";
    gate.label = "Gate";
    // Exactly what an older build persisted: the user's value under the id the editor
    // writes, the registry default under the id the registry used to declare.
    gate.params["threshold"] = -42.0;
    gate.params["thresholdDb"] = -60.0;
    gate.params["release"] = 120.0;
    gate.params["releaseMs"] = 50.0;
    graph.nodes.push_back(gate);

    graph.nodes.push_back({"out", kNodeTypeOutput, "", "Output", true});
    graph.edges.push_back({"in", "gate", 0, 0, 1.0});
    graph.edges.push_back({"gate", "out", 0, 0, 1.0});

    SignalGraphExecutor executor;
    executor.SetGraph(graph);
    executor.Prepare(kSampleRate, kBlockSize);

    const auto* processor = executor.GetNodeProcessor("gate");
    Check(processor != nullptr, "the rebuilt graph has a gate processor");

    if (processor == nullptr)
    {
        return;
    }

    Check(std::abs(processor->GetParam("threshold") - -42.0) < 1e-3, "the stored threshold reaches the processor",
          Num(processor->GetParam("threshold")) + " dB");
    Check(std::abs(processor->GetParam("release") - 120.0) < 1e-3, "and so does the stored release",
          Num(processor->GetParam("release")) + " ms");
}

void TestGlobalChainRoundTrip()
{
    std::cout << "\n-- the global gate, edited then rebuilt --" << std::endl;

    GlobalSignalChainConfig config;
    GlobalChainEditor::NormalizeConfig(config);

    SignalGraphExecutor pre;
    SignalGraphExecutor post;
    GlobalChainEditor editor(config, pre, post);
    editor.SetGateEnabled(true);
    editor.SetGateThreshold(-42.0);
    editor.SetGateHold(30.0);
    editor.SetGateRelease(120.0);

    // What GlobalChainEngine::Rebuild does after a restart, a device change or a pre-chain
    // edit: hand the stored graph to a fresh executor.
    SignalGraphExecutor rebuilt;
    rebuilt.SetGraph(config.BuildPreChainGraph());
    rebuilt.Prepare(kSampleRate, kBlockSize);

    const auto* processor = rebuilt.GetNodeProcessor("global_gate");
    Check(processor != nullptr, "the rebuilt pre-chain has a gate");

    if (processor == nullptr)
    {
        return;
    }

    Check(std::abs(processor->GetParam("threshold") - -42.0) < 1e-3, "the threshold the user set survives the rebuild",
          Num(processor->GetParam("threshold")) + " dB");
    Check(std::abs(processor->GetParam("hold") - 30.0) < 1e-3, "and so does hold",
          Num(processor->GetParam("hold")) + " ms");
    Check(std::abs(processor->GetParam("release") - 120.0) < 1e-3, "and release",
          Num(processor->GetParam("release")) + " ms");
}

void TestOpenAndClosedLevels()
{
    std::cout << "\n-- open and closed levels --" << std::endl;

    // Loud enough to open, run long enough for the gain ramp to settle.
    auto open = MakeGate({{"threshold", -40.0}, {"attack", 1.0}, {"hold", 50.0}, {"release", 50.0}});
    const auto loud = Tone(0.5, static_cast<int>(kSampleRate * 0.25));
    const auto opened = RunStereo(*open, loud, loud);
    const double openedPeak = Peak(opened.left, opened.left.size() / 2, opened.left.size());
    Check(std::abs(Db(openedPeak) - Db(0.5)) < 0.1, "a signal above the threshold passes at unity",
          Num(Db(openedPeak) - Db(0.5), 3) + " dB from unity");

    // Below the threshold, and given long enough for the gate to settle closed.
    auto closed = MakeGate({{"threshold", -40.0}, {"hold", 50.0}, {"release", 50.0}, {"range", -80.0}});
    const auto quiet = Tone(Amplitude(-60.0), static_cast<int>(kSampleRate * 0.5));
    const auto gated = RunStereo(*closed, quiet, quiet);
    const double gatedPeak = Peak(gated.left, gated.left.size() / 2, gated.left.size());
    Check(Db(gatedPeak) < -120.0, "a signal below the threshold is attenuated to the floor",
          Num(Db(gatedPeak), 1) + " dBFS");
}

void TestCloseIsARampNotAStep()
{
    std::cout << "\n-- the close is a ramp --" << std::endl;

    // A loud burst, then a steady low-level tone standing in for the noise floor the gate
    // is there to remove. The gate closes while that noise is still playing, which is where
    // a gate that steps its gain from 1 to 0 produces an audible click -- and this one sits
    // in front of the amp, so the step would be amplified along with everything else.
    const double noiseAmplitude = Amplitude(-60.0);
    const int burstSamples = static_cast<int>(kSampleRate * 0.2);
    const int tailSamples = static_cast<int>(kSampleRate * 0.6);

    std::vector<float> signal = Tone(0.5, burstSamples);
    Append(signal, Tone(noiseAmplitude, tailSamples, 440.0, 0.0));

    auto gate = MakeGate({{"threshold", -40.0}, {"attack", 1.0}, {"hold", 50.0}, {"release", 50.0}});
    const auto out = RunStereo(*gate, signal, signal);

    // Walk the output envelope forward from the end of the burst and find where it passes
    // 90% and 10% of the noise level. A step puts those one sample apart.
    constexpr std::size_t kWindow = 64;
    std::size_t at90 = 0;
    std::size_t at10 = 0;

    for (std::size_t i = static_cast<std::size_t>(burstSamples); i + kWindow < out.left.size(); i += kWindow)
    {
        const double peak = Peak(out.left, i, i + kWindow);

        if (at90 == 0 && peak < 0.9 * noiseAmplitude)
        {
            at90 = i;
        }

        if (at90 != 0 && at10 == 0 && peak < 0.1 * noiseAmplitude)
        {
            at10 = i;
            break;
        }
    }

    Check(at90 != 0 && at10 != 0, "the gate closes on the noise floor",
          "90% at " + std::to_string(at90) + ", 10% at " + std::to_string(at10));

    if (at90 == 0 || at10 == 0)
    {
        return;
    }

    // 50 ms of one-pole release takes roughly 2.2 time constants to go 90% -> 10%, about
    // 5000 samples at 48 kHz. Anything under a few hundred is a step with extra steps.
    const std::size_t rampSamples = at10 - at90;
    Check(rampSamples > 500, "and it does so as a ramp rather than a step",
          std::to_string(rampSamples) + " samples from 90% to 10%");

    // Nothing in the closing edge may exceed the level the gate was passing: a step would
    // show up as a discontinuity larger than the signal itself.
    double largestJump = 0.0;

    for (std::size_t i = at90 + 1; i < std::min(at10 + kWindow, out.left.size()); ++i)
    {
        largestJump = std::max(largestJump, static_cast<double>(std::abs(out.left[i] - out.left[i - 1])));
    }

    Check(largestJump < noiseAmplitude, "with no sample-to-sample jump bigger than the signal being gated",
          Num(Db(largestJump), 1) + " dBFS jump vs " + Num(Db(noiseAmplitude), 1) + " dBFS signal");
}

/// Counts how often the gate changes state while following a level that swings across the
/// threshold. Hold is off so this measures hysteresis alone.
int CountGateTransitions(double hysteresisDb)
{
    auto effect = EffectRegistry::Instance().Create(EffectGuids::kDynamicsGate);
    auto* gate = dynamic_cast<NoiseGateEffect*>(effect.get());

    if (gate == nullptr)
    {
        return -1;
    }

    gate->SetParam("threshold", -40.0);
    gate->SetParam("attack", 1.0);
    gate->SetParam("hold", 0.0);
    gate->SetParam("release", 10.0);
    gate->SetParam("hysteresis", hysteresisDb);
    gate->Prepare(kSampleRate, kBlockSize);

    // A 440 Hz tone whose level wanders +/- 3 dB around the threshold at 5 Hz: slow enough
    // for the detector to follow, which is exactly the decaying-note case that chatters.
    const int samples = static_cast<int>(kSampleRate * 2.0);
    std::vector<float> signal(static_cast<std::size_t>(samples));

    for (std::size_t i = 0; i < signal.size(); ++i)
    {
        const double t = static_cast<double>(i) / kSampleRate;
        const double levelDb = -40.0 + 3.0 * std::sin(2.0 * kPi * 5.0 * t);
        signal[i] = static_cast<float>(Amplitude(levelDb) * std::sin(2.0 * kPi * 440.0 * t));
    }

    int transitions = 0;
    bool wasOpen = gate->IsOpen();
    std::vector<float> block(kBlockSize, 0.0f);
    std::vector<float> outBlock(kBlockSize, 0.0f);

    for (std::size_t offset = 0; offset < signal.size(); offset += kBlockSize)
    {
        const std::size_t count = std::min<std::size_t>(kBlockSize, signal.size() - offset);
        std::copy_n(signal.begin() + static_cast<std::ptrdiff_t>(offset), count, block.begin());
        gate->ProcessMono(block.data(), outBlock.data(), static_cast<int>(count));

        if (gate->IsOpen() != wasOpen)
        {
            ++transitions;
            wasOpen = gate->IsOpen();
        }
    }

    return transitions;
}

void TestHysteresisStopsChatter()
{
    std::cout << "\n-- hysteresis --" << std::endl;

    const int withoutHysteresis = CountGateTransitions(0.0);
    const int withHysteresis = CountGateTransitions(6.0);

    Check(withoutHysteresis > 4, "a level swinging across the threshold chatters a gate with no hysteresis",
          std::to_string(withoutHysteresis) + " transitions");
    Check(withHysteresis <= 1, "and settles once the hysteresis exceeds the swing",
          std::to_string(withHysteresis) + " transitions");
}

void TestDetectorIsLinkedAcrossChannels()
{
    std::cout << "\n-- linked stereo detection --" << std::endl;

    // One loud channel and one quiet one. Gating each channel off its own envelope shuts
    // the quiet side while the loud side stays open, which wanders the stereo image.
    auto gate = MakeGate({{"threshold", -40.0}, {"attack", 1.0}, {"hold", 50.0}, {"release", 50.0}});
    const int samples = static_cast<int>(kSampleRate * 0.25);
    const auto loud = Tone(0.5, samples);
    const auto quiet = Tone(Amplitude(-60.0), samples);
    const auto out = RunStereo(*gate, loud, quiet);

    const std::size_t half = out.right.size() / 2;
    const double rightGainDb = Db(Peak(out.right, half, out.right.size())) - Db(Amplitude(-60.0));
    Check(std::abs(rightGainDb) < 0.1, "a quiet channel passes at unity while the other channel holds the gate open",
          Num(rightGainDb, 3) + " dB from unity");

    // The other mode, which StereoProcessingTests holds the gate to: each channel gates on
    // its own level, so the quiet one closes regardless of what the loud one is doing.
    auto independent =
        MakeGate({{"threshold", -40.0}, {"attack", 1.0}, {"hold", 50.0}, {"release", 50.0}, {"stereoLink", 0.0}});
    const auto unlinked = RunStereo(*independent, loud, quiet);
    const double unlinkedGainDb = Db(Peak(unlinked.right, half, unlinked.right.size())) - Db(Amplitude(-60.0));
    Check(unlinkedGainDb < -40.0, "and with the link off that same channel is gated on its own level",
          Num(unlinkedGainDb, 1) + " dB from unity");
}

void TestMonoMatchesStereo()
{
    std::cout << "\n-- the mono fast path --" << std::endl;

    auto stereoGate = MakeGate({{"threshold", -40.0}, {"hold", 20.0}, {"release", 40.0}});
    auto monoGate = MakeGate({{"threshold", -40.0}, {"hold", 20.0}, {"release", 40.0}});
    Check(stereoGate && stereoGate->SupportsMonoProcessing(), "the gate offers a mono path");

    if (!stereoGate || !monoGate)
    {
        return;
    }

    std::vector<float> signal = Tone(0.4, static_cast<int>(kSampleRate * 0.15));
    Append(signal, Tone(Amplitude(-70.0), static_cast<int>(kSampleRate * 0.25)));

    const auto stereoOut = RunStereo(*stereoGate, signal, signal);
    const auto monoOut = RunMono(*monoGate, signal);

    double largestDifference = 0.0;

    for (std::size_t i = 0; i < monoOut.size(); ++i)
    {
        largestDifference = std::max(largestDifference, static_cast<double>(std::abs(monoOut[i] - stereoOut.left[i])));
    }

    Check(largestDifference < 1e-6, "and it produces the same output as the stereo path",
          "largest difference " + Num(largestDifference, 9));
}

void TestNonFiniteInputDoesNotLatch()
{
    std::cout << "\n-- a non-finite input sample --" << std::endl;

    auto gate = MakeGate({{"threshold", -40.0}, {"hold", 10.0}, {"release", 20.0}});

    if (!gate)
    {
        return;
    }

    // One bad sample from upstream. NaN propagates through a one-pole detector and compares
    // false against every threshold, so without a guard the gate never opens again.
    std::vector<float> poisoned(kBlockSize, 0.0f);
    poisoned[10] = std::numeric_limits<float>::quiet_NaN();
    RunMono(*gate, poisoned);

    const auto loud = Tone(0.5, static_cast<int>(kSampleRate * 0.25));
    const auto recovered = RunMono(*gate, loud);
    const double peak = Peak(recovered, recovered.size() / 2, recovered.size());

    Check(std::isfinite(peak), "the output is finite again", Num(peak));
    Check(std::abs(Db(peak) - Db(0.5)) < 0.5, "and the gate opens on the next loud signal", Num(Db(peak), 2) + " dBFS");
}
} // namespace

int main()
{
    std::cout << "=== NoiseGateTests ===" << std::endl;
    guitarfx::RegisterNoiseGateEffect();

    TestRegistration();
    TestParamRoundTripAndClamping();
    TestLegacyParamKeysFold();
    TestStoredThresholdSurvivesRebuild();
    TestGlobalChainRoundTrip();
    TestOpenAndClosedLevels();
    TestCloseIsARampNotAStep();
    TestHysteresisStopsChatter();
    TestDetectorIsLinkedAcrossChannels();
    TestMonoMatchesStereo();
    TestNonFiniteInputDoesNotLatch();

    std::cout << "\n" << (gChecks - gFailures) << "/" << gChecks << " checks passed" << std::endl;
    return gFailures == 0 ? 0 : 1;
}

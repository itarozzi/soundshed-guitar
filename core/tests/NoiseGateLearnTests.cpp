/**
 * @file NoiseGateLearnTests.cpp
 * @brief The noise gate's Learn: what it measures and whether the threshold it suggests works.
 *
 *   - learning hiss suggests a threshold the same hiss never opens, a tone over it still
 *     opens, and the hiss cannot hold the gate open after the tone stops
 *   - a knock in the middle of a listen moves the noise floor by less than a dB
 *   - playing through a listen is reported as playing, not learned as noise
 *   - digital silence learns the bottom of Threshold's range
 *   - the mono path learns the same floor as the stereo one
 *   - learning leaves the audio untouched
 *   - a second Start replaces the first, Cancel goes idle, and progress climbs to 1
 *   - GateLearnService runs a listen on the live global gate through a sleeping mixer, which
 *     it holds awake, and reports a gate that is not in the chain
 */

#include <algorithm>
#include <cmath>
#include <iostream>
#include <memory>
#include <random>
#include <string>
#include <vector>

#include "MessageHandlerRegistry.h"
#include "controller/GateLearnService.h"
#include "dsp/MultiPresetMixer.h"
#include "dsp/effects/BuiltinEffects.h"
#include "dsp/effects/NoiseGateEffect.h"
#include "presets/PresetTypes.h"
#include "resources/ResourceLibrary.h"

#include <nlohmann/json.hpp>

namespace
{
using namespace guitarfx;

constexpr double kSampleRate = 48000.0;
constexpr int kBlockSize = 128;
constexpr double kPi = 3.14159265358979323846;
/// Long enough for the settle and every slice, with room to spare.
constexpr double kListenSeconds = 2.4;

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

std::vector<float> Hiss(double rmsDb, double seconds, unsigned seed)
{
    std::mt19937 rng(seed);
    std::normal_distribution<float> noise(0.0f, static_cast<float>(Amplitude(rmsDb)));
    std::vector<float> signal(static_cast<std::size_t>(kSampleRate * seconds));

    for (auto& sample : signal)
    {
        sample = noise(rng);
    }

    return signal;
}

std::vector<float> Tone(double peakDb, double seconds, double frequency = 440.0)
{
    std::vector<float> signal(static_cast<std::size_t>(kSampleRate * seconds));

    for (std::size_t i = 0; i < signal.size(); ++i)
    {
        signal[i] = static_cast<float>(Amplitude(peakDb) * std::sin(2.0 * kPi * frequency * static_cast<double>(i) / kSampleRate));
    }

    return signal;
}

std::unique_ptr<NoiseGateEffect> MakeGate(double thresholdDb = -60.0, double hysteresisDb = 4.0)
{
    auto gate = std::make_unique<NoiseGateEffect>();
    gate->SetParam("threshold", thresholdDb);
    gate->SetParam("hysteresis", hysteresisDb);
    gate->SetParam("hold", 20.0);
    gate->SetParam("release", 10.0);
    gate->Prepare(kSampleRate, kBlockSize);
    return gate;
}

/// Stereo, block by block. Returns the output's left channel; `openBlocks` counts the blocks
/// that ended with the gate open, from `countFrom` on.
std::vector<float> Run(NoiseGateEffect& gate, const std::vector<float>& input, int* openBlocks = nullptr,
                       std::size_t countFrom = 0, bool mono = false)
{
    std::vector<float> out(input.size(), 0.0f);
    std::vector<float> left(kBlockSize);
    std::vector<float> right(kBlockSize);
    std::vector<float> outLeft(kBlockSize);
    std::vector<float> outRight(kBlockSize);

    for (std::size_t offset = 0; offset < input.size(); offset += kBlockSize)
    {
        const auto count = static_cast<int>(std::min<std::size_t>(kBlockSize, input.size() - offset));
        std::copy_n(input.begin() + static_cast<std::ptrdiff_t>(offset), count, left.begin());
        std::copy_n(input.begin() + static_cast<std::ptrdiff_t>(offset), count, right.begin());

        if (mono)
        {
            gate.ProcessMono(left.data(), outLeft.data(), count);
        }
        else
        {
            float* inputs[2] = {left.data(), right.data()};
            float* outputs[2] = {outLeft.data(), outRight.data()};
            gate.Process(inputs, outputs, count);
        }

        std::copy_n(outLeft.begin(), count, out.begin() + static_cast<std::ptrdiff_t>(offset));

        if (openBlocks && offset >= countFrom && gate.IsOpen())
        {
            ++*openBlocks;
        }
    }

    return out;
}

GateNoiseLearner::Snapshot Learn(NoiseGateEffect& gate, const std::vector<float>& input, int session = 1,
                                 bool mono = false)
{
    gate.Learner().Start(session);
    Run(gate, input, nullptr, 0, mono);
    return gate.Learner().Read();
}

void TestLearnedThresholdGatesTheHiss()
{
    std::cout << "\n-- learning hiss --" << std::endl;

    auto learner = MakeGate();
    const auto heard = Learn(*learner, Hiss(-72.0, kListenSeconds, 1));
    const double threshold = learner->LearnedThresholdDb(heard.noiseFloorDb);

    Check(heard.state == GateNoiseLearner::State::Done && heard.session == 1, "the listen finishes");
    Check(GateNoiseLearner::HeardOnlyNoise(heard), "hiss is heard as noise", Num(heard.spreadDb) + " dB spread");
    Check(heard.noiseFloorDb > -85.0 && heard.noiseFloorDb < -60.0, "the floor is near the hiss",
          Num(heard.noiseFloorDb) + " dB");
    Check(std::abs(threshold - (heard.noiseFloorDb + 4.0 + GateNoiseLearner::kMarginDb)) < 1e-3,
          "Threshold goes the hysteresis and a margin above it", Num(threshold) + " dB");

    // Different hiss of the same level, through a gate set where Learn said.
    auto gate = MakeGate(threshold);
    int openBlocks = 0;
    Run(*gate, Hiss(-72.0, 3.0, 2), &openBlocks);
    Check(openBlocks == 0, "the same hiss never opens the gate", std::to_string(openBlocks) + " open blocks");

    // A note well over it opens the gate, and once it stops the hiss cannot hold it open.
    auto played = Tone(threshold + 20.0, 0.5);
    const std::size_t stopAt = played.size();
    const auto tail = Hiss(-72.0, 1.0, 3);
    played.insert(played.end(), tail.begin(), tail.end());
    int openAfter = 0;
    const auto passed = Run(*gate, played, &openAfter, stopAt + static_cast<std::size_t>(kSampleRate * 0.2));
    double notePeak = 0.0;

    for (std::size_t i = stopAt / 2; i < stopAt; ++i)
    {
        notePeak = std::max(notePeak, static_cast<double>(std::abs(passed[i])));
    }

    Check(notePeak > 0.9 * Amplitude(threshold + 20.0), "a note 20 dB over passes");
    Check(openAfter == 0, "the hiss lets the gate close after the note", std::to_string(openAfter) + " open blocks");
}

void TestAKnockDoesNotMoveTheFloor()
{
    std::cout << "\n-- a knock while listening --" << std::endl;

    auto clean = MakeGate();
    const auto quiet = Learn(*clean, Hiss(-72.0, kListenSeconds, 4));

    auto knocked = MakeGate();
    auto input = Hiss(-72.0, kListenSeconds, 4);
    const auto knock = Tone(-20.0, 0.005, 300.0);
    std::copy(knock.begin(), knock.end(), input.begin() + static_cast<std::ptrdiff_t>(kSampleRate * 1.2));
    const auto heard = Learn(*knocked, input);

    Check(GateNoiseLearner::HeardOnlyNoise(heard), "a knock is not taken for playing", Num(heard.spreadDb) + " dB spread");
    Check(std::abs(heard.noiseFloorDb - quiet.noiseFloorDb) < 1.0, "the floor moves by under 1 dB",
          Num(quiet.noiseFloorDb) + " -> " + Num(heard.noiseFloorDb) + " dB");
}

void TestPlayingIsNotLearnedAsNoise()
{
    std::cout << "\n-- playing while listening --" << std::endl;

    // Hiss with a phrase over its second half: the loud slices sit far above the typical one.
    auto input = Hiss(-72.0, kListenSeconds, 5);
    const auto phrase = Tone(-24.0, 1.0, 196.0);

    for (std::size_t i = 0; i < phrase.size(); ++i)
    {
        input[input.size() - phrase.size() + i] += phrase[i];
    }

    auto gate = MakeGate();
    const auto heard = Learn(*gate, input);
    Check(heard.state == GateNoiseLearner::State::Done, "the listen finishes");
    Check(!GateNoiseLearner::HeardOnlyNoise(heard), "playing is reported, not learned",
          Num(heard.spreadDb) + " dB spread, floor " + Num(heard.noiseFloorDb) + " dB");

    // A note held through the whole listen is steady, but far too loud to be a noise floor.
    auto held = MakeGate();
    const auto sustained = Learn(*held, Tone(-6.0, kListenSeconds));
    Check(!GateNoiseLearner::HeardOnlyNoise(sustained), "a note held throughout is not noise",
          "floor " + Num(sustained.noiseFloorDb) + " dB");
}

void TestSilenceLearnsTheBottomOfTheRange()
{
    std::cout << "\n-- digital silence --" << std::endl;

    auto gate = MakeGate();
    const auto heard = Learn(*gate, std::vector<float>(static_cast<std::size_t>(kSampleRate * kListenSeconds), 0.0f));
    Check(heard.state == GateNoiseLearner::State::Done && GateNoiseLearner::HeardOnlyNoise(heard),
          "silence is a quiet noise floor");
    Check(gate->LearnedThresholdDb(heard.noiseFloorDb) == NoiseGateEffect::kMinThresholdDb,
          "it suggests the lowest threshold");
}

void TestMonoLearnsTheSameFloor()
{
    std::cout << "\n-- mono --" << std::endl;

    const auto input = Hiss(-66.0, kListenSeconds, 6);
    auto stereo = MakeGate();
    auto mono = MakeGate();
    const auto stereoHeard = Learn(*stereo, input);
    const auto monoHeard = Learn(*mono, input, 1, true);
    Check(monoHeard.state == GateNoiseLearner::State::Done, "the mono path finishes a listen");
    Check(std::abs(monoHeard.noiseFloorDb - stereoHeard.noiseFloorDb) < 1e-3, "and hears the same floor",
          Num(stereoHeard.noiseFloorDb, 3) + " / " + Num(monoHeard.noiseFloorDb, 3) + " dB");
}

void TestLearningLeavesTheAudioAlone()
{
    std::cout << "\n-- audio while learning --" << std::endl;

    auto input = Hiss(-50.0, kListenSeconds, 7);
    const auto tone = Tone(-30.0, 0.5);
    std::copy(tone.begin(), tone.end(), input.begin() + static_cast<std::ptrdiff_t>(kSampleRate * 0.6));

    auto plain = MakeGate(-45.0);
    auto learning = MakeGate(-45.0);
    const auto expected = Run(*plain, input);
    learning->Learner().Start(1);
    const auto actual = Run(*learning, input);
    Check(expected == actual, "the output is bit-identical with and without a listen");
}

/// A listen that finishes part way through a long block used to keep counting slices for the rest
/// of it, writing past the end of its slice array (an assertion in Debug, a scribble in Release).
void TestAListenThatEndsMidBlock()
{
    std::cout << "\n-- a listen that ends mid-block --" << std::endl;

    GateNoiseLearner learner;
    learner.Prepare(kSampleRate);
    learner.Start(1);
    Check(learner.BeginBlock(), "the block that picks up Start listens");

    const int slice = static_cast<int>(GateNoiseLearner::kSliceMs * 0.001 * kSampleRate);
    const int settle = static_cast<int>(GateNoiseLearner::kSettleMs * 0.001 * kSampleRate);
    const int listen = settle + GateNoiseLearner::kSlices * slice;

    // One block holding the whole listen and three slices more, louder than the noise.
    for (int i = 0; i < listen + 3 * slice; ++i)
    {
        learner.Feed(i < listen ? 1.0e-3f : 0.5f);
    }

    learner.EndBlock();
    const auto heard = learner.Read();
    Check(heard.state == GateNoiseLearner::State::Done && heard.progress == 1.0f, "the listen finishes");
    Check(std::abs(heard.noiseFloorDb + 60.0f) < 0.01f, "only what it heard while listening counts",
          Num(heard.noiseFloorDb, 2) + " dB");
    Check(!learner.BeginBlock(), "and the next block no longer listens");
}

void TestSessions()
{
    std::cout << "\n-- sessions --" << std::endl;

    auto gate = MakeGate();
    const auto hiss = Hiss(-70.0, kListenSeconds, 8);
    const std::vector<float> half(hiss.begin(), hiss.begin() + static_cast<std::ptrdiff_t>(hiss.size() / 2));

    gate->Learner().Start(1);
    Run(*gate, half);
    const auto partway = gate->Learner().Read();
    Check(partway.state == GateNoiseLearner::State::Listening && partway.session == 1, "listening part way through");
    Check(partway.progress > 0.3f && partway.progress < 0.7f, "progress is part way", Num(partway.progress, 2));
    Check(partway.levelDb > -90.0f && partway.levelDb < -50.0f, "the live level is the detector's",
          Num(partway.levelDb) + " dB");

    // A second Start begins again rather than finishing the first.
    gate->Learner().Start(2);
    Run(*gate, half);
    Check(gate->Learner().Read().state == GateNoiseLearner::State::Listening, "a restart needs its own full listen");
    Run(*gate, hiss);
    const auto done = gate->Learner().Read();
    Check(done.state == GateNoiseLearner::State::Done && done.session == 2 && done.progress == 1.0f,
          "the restarted listen finishes as session 2");

    gate->Learner().Start(3);
    Run(*gate, half);
    gate->Learner().Cancel();
    Run(*gate, half);
    Check(gate->Learner().Read().state == GateNoiseLearner::State::Idle, "Cancel goes idle");
}
void TestServiceListensOnTheGlobalGate()
{
    std::cout << "\n-- the service, on the live global gate --" << std::endl;

    MultiPresetMixer mixer;
    ResourceLibrary library;
    mixer.SetResourceLibrary(&library);
    mixer.Prepare(kSampleRate, kBlockSize);

    Preset preset;
    preset.id = "p";
    preset.name = "p";
    preset.graph.nodes.push_back(GraphNode{"in", kNodeTypeInput, "", "Input", true});
    preset.graph.nodes.push_back(GraphNode{"out", kNodeTypeOutput, "", "Output", true});
    preset.graph.edges.push_back({"in", "out", 0, 0, 1.0});
    mixer.AddActivePreset(preset, preset.id, preset.name);
    mixer.SetGlobalGateEnabled(true);

    std::mutex dspMutex;
    std::vector<nlohmann::json> sent;
    GateLearnService service([&](const std::string& json) { sent.push_back(nlohmann::json::parse(json)); }, mixer,
                             dspMutex, [] { return std::string("p"); });
    MessageHandlerRegistry registry;
    service.RegisterMessageHandlers(registry);
    Check(registry.Types() == std::vector<std::string>{"gateLearn"}, "the service registers its one message");

    // Hiss far under the sleep threshold, long enough for the mixer to fall asleep on it.
    const auto hiss = Hiss(-100.0, 1.0, 9);
    std::vector<float> left(kBlockSize);
    std::vector<float> right(kBlockSize);
    std::vector<float> outLeft(kBlockSize);
    std::vector<float> outRight(kBlockSize);
    std::size_t position = 0;
    const auto runSeconds = [&](double seconds, bool idle) {
        const int blocks = static_cast<int>(seconds * kSampleRate / kBlockSize);

        for (int b = 0; b < blocks; ++b)
        {
            for (int i = 0; i < kBlockSize; ++i)
            {
                left[static_cast<std::size_t>(i)] = right[static_cast<std::size_t>(i)] = hiss[position];
                position = (position + 1) % hiss.size();
            }

            float* inputs[2] = {left.data(), right.data()};
            float* outputs[2] = {outLeft.data(), outRight.data()};
            {
                std::lock_guard<std::mutex> lock(dspMutex);
                mixer.Process(inputs, outputs, kBlockSize);
            }

            if (idle && b % 16 == 0)
            {
                service.OnIdle();
            }
        }
    };

    runSeconds(SilenceSleepGate::kRingOutHoldSeconds + 1.0, false);
    Check(mixer.IsSleepingOnSilence(), "the mixer is asleep on the hiss before the listen");

    registry.Dispatch("gateLearn", nlohmann::json{{"action", "start"}, {"scope", "global"}});
    Check(!sent.empty() && sent.front().value("state", "") == "listening" && service.IsListening(),
          "a start is acknowledged as listening");
    runSeconds(kListenSeconds, true);
    service.OnIdle();

    const auto& last = sent.back();
    Check(last.value("state", "") == "done" && last.value("scope", "") == "global" &&
              last.value("nodeId", "") == "global_gate",
          "the listen finishes on the global gate", last.dump());
    Check(last.value("thresholdDb", 0.0) == NoiseGateEffect::kMinThresholdDb,
          "and suggests the lowest threshold for hiss that quiet");
    Check(!service.IsListening(), "the service is done listening");

    runSeconds(SilenceSleepGate::kRingOutHoldSeconds + 1.0, false);
    Check(mixer.IsSleepingOnSilence(), "the mixer is released to sleep again");

    sent.clear();
    registry.Dispatch("gateLearn", nlohmann::json{{"action", "start"}, {"scope", "preset"}, {"nodeId", "missing"}});
    Check(sent.size() == 1 && sent.back().value("state", "") == "failed" &&
              sent.back().value("reason", "") == "notFound" && sent.back().value("presetId", "") == "p",
          "a gate not in the chain is reported, against the active preset");
}
} // namespace

int main()
{
    std::cout << "=== NoiseGateLearnTests ===" << std::endl;
    RegisterAllEffects();

    TestLearnedThresholdGatesTheHiss();
    TestAKnockDoesNotMoveTheFloor();
    TestPlayingIsNotLearnedAsNoise();
    TestSilenceLearnsTheBottomOfTheRange();
    TestMonoLearnsTheSameFloor();
    TestLearningLeavesTheAudioAlone();
    TestAListenThatEndsMidBlock();
    TestSessions();
    TestServiceListensOnTheGlobalGate();

    std::cout << "\n" << (gChecks - gFailures) << "/" << gChecks << " checks passed" << std::endl;
    return gFailures == 0 ? 0 : 1;
}

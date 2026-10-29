/**
 * @file MultibandGateEffectTests.cpp
 * @brief The Multi-band Gate: the six-band split, the per-band expander and its Learn.
 *
 *   - open, the six bands add back to flat: tones across the range come out at their own level
 *   - a band below its threshold is cut by Range; a loud tone in another band does not open it,
 *     nor is that tone touched
 *   - noise under every threshold comes out about Range quieter
 *   - a decaying note loses its air band while its body still plays
 *   - Offset moves all six thresholds
 *   - Learn: noise gives six floors, and the thresholds it suggests take that noise down while a
 *     note still passes; playing through a listen fails as playing
 *   - the mono path matches the stereo one; dual mono keeps one side's tone from opening the other
 *   - a non-finite sample does not latch
 *   - GateLearnService learns a Multi-band Gate node in a preset and reports all six bands
 */

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <iostream>
#include <limits>
#include <memory>
#include <random>
#include <string>
#include <vector>

#include "MessageHandlerRegistry.h"
#include "controller/GateLearnService.h"
#include "dsp/MultiPresetMixer.h"
#include "dsp/effects/BuiltinEffects.h"
#include "dsp/effects/MultibandGateEffect.h"
#include "presets/PresetTypes.h"
#include "resources/ResourceLibrary.h"

#include <nlohmann/json.hpp>

namespace
{
using namespace guitarfx;

constexpr double kSampleRate = 48000.0;
constexpr int kBlockSize = 128;
constexpr double kPi = 3.14159265358979323846;
constexpr int kBands = MultibandGateEffect::kBands;
/// Long enough for Learn's settle and every slice, with room to spare.
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

std::vector<float> Tone(double peakDb, double seconds, double frequency)
{
    std::vector<float> signal(static_cast<std::size_t>(kSampleRate * seconds));

    for (std::size_t i = 0; i < signal.size(); ++i)
    {
        signal[i] = static_cast<float>(Amplitude(peakDb) *
                                       std::sin(2.0 * kPi * frequency * static_cast<double>(i) / kSampleRate));
    }

    return signal;
}

std::vector<float> Mix(std::vector<float> a, const std::vector<float>& b)
{
    for (std::size_t i = 0; i < std::min(a.size(), b.size()); ++i)
    {
        a[i] += b[i];
    }

    return a;
}

/// RMS in dB of `signal` from `from` (seconds) to the end.
double RmsDb(const std::vector<float>& signal, double from = 0.0)
{
    const auto start = static_cast<std::size_t>(from * kSampleRate);
    double sum = 0.0;

    for (std::size_t i = start; i < signal.size(); ++i)
    {
        sum += static_cast<double>(signal[i]) * signal[i];
    }

    const double mean = sum / static_cast<double>(std::max<std::size_t>(1, signal.size() - start));
    return 10.0 * std::log10(std::max(mean, 1.0e-30));
}

/// Thresholds for every band at once.
void SetThresholds(MultibandGateEffect& gate, double db)
{
    for (int band = 1; band <= kBands; ++band)
    {
        gate.SetParam("threshold" + std::to_string(band), db);
    }
}

std::unique_ptr<MultibandGateEffect> MakeGate(double thresholdDb)
{
    auto gate = std::make_unique<MultibandGateEffect>();
    gate->Prepare(kSampleRate, kBlockSize);
    SetThresholds(*gate, thresholdDb);
    gate->SetParam("release", 50.0);
    return gate;
}

/// Block by block, the same signal on both sides (or `right` on the right). Returns the left
/// output, and the right in `rightOut` when asked; `perBlock` sees the gate after each block.
template <typename PerBlock>
std::vector<float> Run(MultibandGateEffect& gate, const std::vector<float>& input, PerBlock perBlock, bool mono = false,
                       const std::vector<float>* right = nullptr, std::vector<float>* rightOut = nullptr)
{
    std::vector<float> out(input.size(), 0.0f);
    std::vector<float> inL(kBlockSize);
    std::vector<float> inR(kBlockSize);
    std::vector<float> outL(kBlockSize);
    std::vector<float> outR(kBlockSize);

    if (rightOut)
    {
        rightOut->assign(input.size(), 0.0f);
    }

    for (std::size_t offset = 0; offset < input.size(); offset += kBlockSize)
    {
        const auto count = static_cast<int>(std::min<std::size_t>(kBlockSize, input.size() - offset));
        const auto at = static_cast<std::ptrdiff_t>(offset);
        std::copy_n(input.begin() + at, count, inL.begin());
        std::copy_n((right ? *right : input).begin() + at, count, inR.begin());

        if (mono)
        {
            gate.ProcessMono(inL.data(), outL.data(), count);
        }
        else
        {
            float* inputs[2] = {inL.data(), inR.data()};
            float* outputs[2] = {outL.data(), outR.data()};
            gate.Process(inputs, outputs, count);
        }

        std::copy_n(outL.begin(), count, out.begin() + at);

        if (rightOut)
        {
            std::copy_n(outR.begin(), count, rightOut->begin() + at);
        }

        perBlock(offset + static_cast<std::size_t>(count));
    }

    return out;
}

std::vector<float> Run(MultibandGateEffect& gate, const std::vector<float>& input, bool mono = false)
{
    return Run(gate, input, [](std::size_t) {}, mono);
}

void TestOpenIsFlat()
{
    std::cout << "\n-- open, the bands add back to flat --" << std::endl;

    double worst = 0.0;
    std::string where;

    for (const double hz :
         {40.0, 100.0, 150.0, 250.0, 400.0, 700.0, 1000.0, 1800.0, 2500.0, 4000.0, 6000.0, 10000.0, 16000.0})
    {
        auto gate = MakeGate(-96.0);
        const auto tone = Tone(-20.0, 0.5, hz);
        const double change = RmsDb(Run(*gate, tone), 0.25) - RmsDb(tone, 0.25);

        if (std::abs(change) > std::abs(worst))
        {
            worst = change;
            where = Num(hz, 0) + " Hz";
        }
    }

    Check(std::abs(worst) < 0.1, "tones from 40 Hz to 16 kHz keep their level",
          "worst " + Num(worst, 3) + " dB at " + where);
}

void TestABandBelowItsThreshold()
{
    std::cout << "\n-- one band below its threshold --" << std::endl;

    // Only the top two bands (above 2.5 kHz) have thresholds that matter. Both, because a 10 kHz
    // tone is near enough the 6 kHz crossover that the presence band carries it 19 dB down.
    auto gate = MakeGate(-96.0);
    gate->SetParam("threshold5", -20.0);
    gate->SetParam("threshold6", -20.0);
    gate->SetParam("range", -40.0);

    const auto air = Tone(-40.0, 0.6, 10000.0);
    const double airCut = RmsDb(Run(*gate, air), 0.4) - RmsDb(air, 0.4);
    Check(airCut < -38.0 && airCut > -42.0, "a quiet 10 kHz tone is cut by Range", Num(airCut) + " dB");

    gate->Reset();
    const auto body = Tone(-6.0, 0.6, 300.0);
    const double bodyChange = RmsDb(Run(*gate, body), 0.4) - RmsDb(body, 0.4);
    Check(std::abs(bodyChange) < 0.2, "a loud 300 Hz tone passes untouched", Num(bodyChange, 2) + " dB");
    Check(gate->BandGain(0, 5) < 0.02f, "and does not open the air band", Num(gate->BandGain(0, 5), 4));
}

void TestNoiseUnderEveryThreshold()
{
    std::cout << "\n-- noise under every threshold --" << std::endl;

    auto gate = MakeGate(-40.0);
    gate->SetParam("range", -40.0);
    const auto hiss = Hiss(-70.0, 1.0, 3);
    const double cut = RmsDb(Run(*gate, hiss), 0.5) - RmsDb(hiss, 0.5);
    Check(cut < -35.0, "white noise 30 dB under the thresholds comes out about Range quieter", Num(cut) + " dB");

    auto shallow = MakeGate(-40.0);
    shallow->SetParam("range", -12.0);
    const double shallowCut = RmsDb(Run(*shallow, hiss), 0.5) - RmsDb(hiss, 0.5);
    Check(shallowCut > -12.5 && shallowCut < -11.0, "and no deeper than a shallower Range", Num(shallowCut) + " dB");
}

void TestADecayingNoteLosesItsAirFirst()
{
    std::cout << "\n-- a decaying note --" << std::endl;

    // A 220 Hz sawtooth (harmonics falling 6 dB an octave) dying away 30 dB a second over hiss.
    auto gate = MakeGate(-60.0);
    const double seconds = 3.0;
    auto note = Hiss(-90.0, seconds, 5);

    for (std::size_t i = 0; i < note.size(); ++i)
    {
        const double t = static_cast<double>(i) / kSampleRate;
        const double phase = std::fmod(220.0 * t, 1.0);
        note[i] += static_cast<float>(Amplitude(-6.0 - 30.0 * t) * (2.0 * phase - 1.0));
    }

    bool airClosedWhileBodyOpen = false;
    double atSeconds = 0.0;
    Run(*gate, note, [&](std::size_t done) {
        if (!airClosedWhileBodyOpen && gate->BandGain(0, 5) < 0.1f && gate->BandGain(0, 1) > 0.95f)
        {
            airClosedWhileBodyOpen = true;
            atSeconds = static_cast<double>(done) / kSampleRate;
        }
    });

    Check(airClosedWhileBodyOpen, "the air band closes while the band with the fundamental is still open",
          "at " + Num(atSeconds, 2) + " s");
    Check(gate->BandGain(0, 1) < 0.1f, "and once the note is gone, that band closes too", Num(gate->BandGain(0, 1), 3));
}

void TestOffsetMovesEveryThreshold()
{
    std::cout << "\n-- Offset --" << std::endl;

    const auto hiss = Hiss(-60.0, 0.8, 7);
    auto gate = MakeGate(-80.0);
    const double open = RmsDb(Run(*gate, hiss), 0.4) - RmsDb(hiss, 0.4);
    auto raised = MakeGate(-80.0);
    raised->SetParam("offset", 24.0);
    const double raisedCut = RmsDb(Run(*raised, hiss), 0.4) - RmsDb(hiss, 0.4);

    Check(std::abs(open) < 0.5, "noise well over the thresholds passes", Num(open, 2) + " dB");
    Check(raisedCut < -10.0, "and is cut once Offset lifts every threshold above it", Num(raisedCut) + " dB");
}

/// Runs a listen to the end and returns the six snapshots.
std::vector<GateNoiseLearner::Snapshot> Learn(MultibandGateEffect& gate, const std::vector<float>& input, int session)
{
    for (int band = 0; band < kBands; ++band)
    {
        gate.NoiseLearner(band).Start(session);
    }

    Run(gate, input);
    std::vector<GateNoiseLearner::Snapshot> snapshots;

    for (int band = 0; band < kBands; ++band)
    {
        snapshots.push_back(gate.NoiseLearner(band).Read());
    }

    return snapshots;
}

void TestLearn()
{
    std::cout << "\n-- Learn --" << std::endl;

    auto gate = MakeGate(-65.0);
    const auto hiss = Hiss(-75.0, kListenSeconds, 11);
    const auto heard = Learn(*gate, hiss, 1);

    bool allDone = true;
    bool allNoise = true;
    std::string floors;

    for (const auto& snapshot : heard)
    {
        allDone = allDone && snapshot.state == GateNoiseLearner::State::Done && snapshot.session == 1;
        allNoise = allNoise && GateNoiseLearner::HeardOnlyNoise(snapshot);
        floors += Num(snapshot.noiseFloorDb) + " ";
    }

    Check(allDone, "all six bands finish the listen together");
    Check(allNoise, "and heard noise, not playing", floors);
    // White noise puts most of its power up top: the air band, 6-24 kHz, holds three quarters of it.
    Check(heard[5].noiseFloorDb > heard[0].noiseFloorDb + 10.0f,
          "the air band hears far more of white noise than the lows", floors);

    for (int band = 0; band < kBands; ++band)
    {
        gate->SetParam("threshold" + std::to_string(band + 1),
                       gate->LearnedThresholdDb(band, heard[static_cast<std::size_t>(band)].noiseFloorDb));
    }

    const auto more = Hiss(-75.0, 1.0, 12);
    const double cut = RmsDb(Run(*gate, more), 0.5) - RmsDb(more, 0.5);
    Check(cut < -15.0, "the learned thresholds take that noise down", Num(cut) + " dB");

    const auto note = Mix(Tone(-20.0, 1.0, 600.0), more);
    const double noteChange = RmsDb(Run(*gate, note), 0.5) - RmsDb(note, 0.5);
    Check(noteChange > -0.5, "while a note over it still plays", Num(noteChange, 2) + " dB");

    // A note played half way through the listen.
    auto played = Hiss(-75.0, kListenSeconds, 13);
    const auto chord = Tone(-12.0, kListenSeconds, 330.0);

    for (std::size_t i = played.size() / 2; i < played.size(); ++i)
    {
        played[i] += chord[i];
    }

    const auto heardPlaying = Learn(*gate, played, 2);
    const bool any = std::any_of(heardPlaying.begin(), heardPlaying.end(),
                                 [](const auto& snapshot) { return !GateNoiseLearner::HeardOnlyNoise(snapshot); });
    Check(any, "playing through a listen is heard as playing");
}

void TestMonoMatchesStereo()
{
    std::cout << "\n-- mono and stereo --" << std::endl;

    const auto signal = Mix(Tone(-30.0, 0.5, 900.0), Hiss(-60.0, 0.5, 17));
    auto stereo = MakeGate(-50.0);
    auto mono = MakeGate(-50.0);
    const auto a = Run(*stereo, signal);
    const auto b = Run(*mono, signal, true);
    float worst = 0.0f;

    for (std::size_t i = 0; i < a.size(); ++i)
    {
        worst = std::max(worst, std::abs(a[i] - b[i]));
    }

    Check(worst < 1.0e-6f, "the mono path matches the stereo one", Num(worst, 9));
}

void TestDualMono()
{
    std::cout << "\n-- linked and dual mono --" << std::endl;

    // Hiss alone on the left, a loud tone in the same band on the right.
    const auto left = Hiss(-70.0, 1.0, 19);
    const auto right = Tone(-10.0, 1.0, 9000.0);

    const auto leftCut = [&](bool dualMono) {
        auto gate = MakeGate(-50.0);
        gate->SetDualMono(dualMono);
        const auto out = Run(*gate, left, [](std::size_t) {}, false, &right);
        return RmsDb(out, 0.5) - RmsDb(left, 0.5);
    };

    const double linked = leftCut(false);
    const double separate = leftCut(true);
    Check(linked > separate + 6.0,
          "linked, the right side's tone holds the left's air band open; dual mono, it does not",
          "linked " + Num(linked) + " dB, dual mono " + Num(separate) + " dB");
}

void TestNonFiniteDoesNotLatch()
{
    std::cout << "\n-- a non-finite sample --" << std::endl;

    auto gate = MakeGate(-60.0);
    auto signal = Tone(-20.0, 0.5, 440.0);
    signal[1000] = std::numeric_limits<float>::quiet_NaN();
    const auto out = Run(*gate, signal);
    const auto tail = std::vector<float>(out.end() - 4800, out.end());
    bool finite = true;

    for (const float sample : tail)
    {
        finite = finite && IsFinite(sample);
    }

    Check(finite && RmsDb(tail) > -25.0, "the tone plays on after a NaN", Num(RmsDb(tail)) + " dB");
}

void TestServiceLearnsAPresetNode()
{
    std::cout << "\n-- the service, on a Multi-band Gate node --" << std::endl;

    MultiPresetMixer mixer;
    ResourceLibrary library;
    mixer.SetResourceLibrary(&library);
    mixer.Prepare(kSampleRate, kBlockSize);

    Preset preset;
    preset.id = "p";
    preset.name = "p";
    preset.graph.nodes.push_back(GraphNode{"in", kNodeTypeInput, "", "Input", true});
    preset.graph.nodes.push_back(GraphNode{"mbg", EffectGuids::kMultibandGate, "", "Multi-band Gate", true});
    preset.graph.nodes.push_back(GraphNode{"out", kNodeTypeOutput, "", "Output", true});
    preset.graph.edges.push_back({"in", "mbg", 0, 0, 1.0});
    preset.graph.edges.push_back({"mbg", "out", 0, 0, 1.0});
    mixer.AddActivePreset(preset, preset.id, preset.name);

    std::mutex dspMutex;
    std::vector<nlohmann::json> sent;
    GateLearnService service([&](const std::string& json) { sent.push_back(nlohmann::json::parse(json)); }, mixer,
                             dspMutex, [] { return std::string("p"); });
    MessageHandlerRegistry registry;
    service.RegisterMessageHandlers(registry);

    const auto hiss = Hiss(-70.0, 1.0, 23);
    std::vector<float> left(kBlockSize);
    std::vector<float> right(kBlockSize);
    std::vector<float> outLeft(kBlockSize);
    std::vector<float> outRight(kBlockSize);
    std::size_t position = 0;
    const int blocks = static_cast<int>((kListenSeconds + 0.5) * kSampleRate / kBlockSize);

    registry.Dispatch("gateLearn", nlohmann::json{{"action", "start"}, {"scope", "preset"}, {"nodeId", "mbg"}});
    Check(!sent.empty() && sent.front().value("state", "") == "listening", "a start is acknowledged as listening");

    bool sawBandLevels = false;

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

        if (b % 16 == 0)
        {
            service.OnIdle();
            const auto& last = sent.back();
            sawBandLevels = sawBandLevels || (last.value("state", "") == "listening" && last.contains("bands") &&
                                              last["bands"].size() == kBands);
        }
    }

    service.OnIdle();
    const auto& last = sent.back();
    const bool done =
        last.value("state", "") == "done" && last.value("nodeId", "") == "mbg" && last.value("presetId", "") == "p";
    Check(done, "the listen finishes on the node", last.dump());
    Check(last.contains("bands") && last["bands"].size() == kBands && !last.contains("thresholdDb"),
          "and reports six bands rather than one threshold");

    bool everyBandHasAThreshold = last.contains("bands");

    if (everyBandHasAThreshold)
    {
        for (const auto& band : last["bands"])
        {
            everyBandHasAThreshold = everyBandHasAThreshold && band.contains("thresholdDb") &&
                                     band.contains("noiseFloorDb") &&
                                     band.value("thresholdDb", 0.0) > band.value("noiseFloorDb", 0.0);
        }
    }

    Check(everyBandHasAThreshold, "each band suggests a threshold above the noise it heard");

    // Progress reports are throttled by the wall clock, so an offline loop may finish first.
    if (!sawBandLevels)
    {
        std::cout << "  [INFO] no progress report arrived before the listen finished" << std::endl;
    }
}
} // namespace

int main()
{
    std::cout << "=== MultibandGateEffectTests ===" << std::endl;
    RegisterAllEffects();

    TestOpenIsFlat();
    TestABandBelowItsThreshold();
    TestNoiseUnderEveryThreshold();
    TestADecayingNoteLosesItsAirFirst();
    TestOffsetMovesEveryThreshold();
    TestLearn();
    TestMonoMatchesStereo();
    TestDualMono();
    TestNonFiniteDoesNotLatch();
    TestServiceLearnsAPresetNode();

    std::cout << "\n" << (gChecks - gFailures) << "/" << gChecks << " checks passed" << std::endl;
    return gFailures == 0 ? 0 : 1;
}

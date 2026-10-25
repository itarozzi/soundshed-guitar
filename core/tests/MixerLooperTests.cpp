// The looper as a stage of MultiPresetMixer: where its two taps are, what survives a preset swap,
// and how it rides through the block's three exits (chains run, chains asleep, tuner muted).
// LooperEngineTests covers the engine itself.

#include "dsp/LevelTargets.h"
#include "dsp/MultiPresetMixer.h"
#include "dsp/TransportClock.h"
#include "dsp/effects/BuiltinEffects.h"
#include "presets/PresetTypes.h"
#include "resources/ResourceLibrary.h"

#include <cmath>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

using namespace guitarfx;
using namespace guitarfx::looper;

namespace
{
constexpr double kSampleRate = 48000.0;
constexpr int kBlock = 128;
// A rig at centre pan is 3 dB down (equal power); the tone peaks at 0.25.
constexpr float kCentrePan = 0.70710678f;
constexpr float kCalibration6dB = 1.99526231f;
constexpr float kHeard = 0.15f;

int gFailures = 0;

void Check(bool condition, const std::string& what)
{
    if (!condition)
    {
        std::cerr << "FAIL: " << what << std::endl;
        ++gFailures;
    }
}

Preset MakePassThroughPreset(const std::string& id)
{
    Preset preset;
    preset.id = id;
    preset.name = id;
    preset.graph.nodes.push_back({"in", kNodeTypeInput, "", "Input", true});
    preset.graph.nodes.push_back({"out", kNodeTypeOutput, "", "Output", true});
    preset.graph.edges.push_back({"in", "out", 0, 0, 1.0});
    return preset;
}

struct Audio
{
    std::vector<float> di0, di1, wet0, wet1;
    LayerAudio audio;

    Audio(std::int64_t capacity, int diChannels)
        : di0(static_cast<std::size_t>(capacity)), di1(diChannels > 1 ? static_cast<std::size_t>(capacity) : 0),
          wet0(static_cast<std::size_t>(capacity)), wet1(static_cast<std::size_t>(capacity))
    {
        audio.di[0] = di0.data();
        audio.di[1] = diChannels > 1 ? di1.data() : nullptr;
        audio.wet[0] = wet0.data();
        audio.wet[1] = wet1.data();
        audio.capacity = capacity;
    }
};

float Tone(std::int64_t t)
{
    return static_cast<float>(0.25 * std::sin(static_cast<double>(t) * 0.02));
}

struct Rig
{
    MultiPresetMixer mixer;
    ResourceLibrary library;
    TransportClock clock;
    Audio capture{static_cast<std::int64_t>(4 * kSampleRate), 2};
    Audio spare{static_cast<std::int64_t>(4 * kSampleRate), 2};
    std::int64_t now = 0;
    bool playing = false; // the input carries Tone while set, silence otherwise
    bool monoOutput = false;
    std::vector<float> inL = std::vector<float>(kBlock);
    std::vector<float> inR = std::vector<float>(kBlock);
    std::vector<float> outL = std::vector<float>(kBlock);
    std::vector<float> outR = std::vector<float>(kBlock);
    float lastPeak = 0.0f;

    Rig()
    {
        mixer.SetResourceLibrary(&library);
        mixer.Prepare(kSampleRate, kBlock);
        const Preset preset = MakePassThroughPreset("a");
        mixer.AddActivePreset(preset, preset.id, preset.name);
        mixer.Looper().SetTransportClock(&clock);
        mixer.Looper().Enqueue({LooperCommand::Type::SetCapture, LooperAction::RecPlayDub, -1, &capture.audio});
        mixer.Looper().Enqueue({LooperCommand::Type::SetSpare, LooperAction::RecPlayDub, -1, &spare.audio});
        // The buffers are the engine's from its next block, as they are once the service has set them up.
        Run(kBlock);
    }

    /// Runs whole blocks for at least `samples`; returns the loudest output sample.
    float Run(std::int64_t samples)
    {
        float peak = 0.0f;

        for (std::int64_t done = 0; done < samples; done += kBlock)
        {
            for (int i = 0; i < kBlock; ++i)
            {
                inL[static_cast<std::size_t>(i)] = playing ? Tone(now + i) : 0.0f;
                inR[static_cast<std::size_t>(i)] = inL[static_cast<std::size_t>(i)];
            }

            float* inputs[2] = {inL.data(), inR.data()};
            float* outputs[2] = {outL.data(), monoOutput ? nullptr : outR.data()};
            mixer.Process(inputs, outputs, kBlock);
            now += kBlock;

            for (int i = 0; i < kBlock; ++i)
            {
                peak = std::max(peak, std::abs(outL[static_cast<std::size_t>(i)]));
            }
        }

        lastPeak = peak;
        return peak;
    }

    LooperSnapshot Snap()
    {
        LooperSnapshot s;
        Check(mixer.Looper().ReadSnapshot(s), "a snapshot reads consistently");
        return s;
    }

    /// Records a 0.5 s loop of the tone and leaves it playing over silence.
    void RecordLoop()
    {
        playing = true;
        mixer.Looper().Press(LooperAction::RecPlayDub);
        Run(static_cast<std::int64_t>(0.5 * kSampleRate));
        mixer.Looper().Press(LooperAction::RecPlayDub);
        playing = false;
    }
};

void TestTheTapsAreTheDiBeforeCalibrationAndTheWetAfterTheChains()
{
    Rig rig;
    rig.mixer.SetUserInputCalibrationGainDb(6.0);
    rig.Run(kBlock * 4);
    const std::int64_t start = rig.now;
    rig.RecordLoop();
    rig.Run(kBlock);
    const auto snap = rig.Snap();
    const SlotInfo& layer = snap.slots[0];
    Check(layer.status == SlotStatus::Visible && layer.firstLayer, "the take is the loop's first layer");
    Check(layer.diChannels == 2, "a stereo input gives a two-channel DI");
    Check(std::abs(layer.calibrationGain - static_cast<float>(std::pow(10.0, 6.0 / 20.0))) < 1e-5f,
          "the layer stores the calibration it was recorded with");
    const std::int64_t p = 1000;
    const auto index = static_cast<std::size_t>((layer.head + p) % layer.audio->capacity);
    Check(layer.audio->di[0][index] == Tone(start + p), "the DI is the input before calibration");
    Check(std::abs(layer.audio->wet[0][index] - kCalibration6dB * kCentrePan * Tone(start + p)) < 1e-4f,
          "the wet sound is the chains' output, calibration and all");

    Rig mono;
    mono.mixer.SetMonoMode(true);
    mono.Run(kBlock);
    mono.RecordLoop();
    mono.Run(kBlock);
    Check(mono.Snap().slots[0].diChannels == 1, "Mono input mode gives a one-channel DI");
}

void TestTheLoopPlaysWhileTheChainsSleep()
{
    Rig rig;
    rig.RecordLoop();
    Check(rig.Run(static_cast<std::int64_t>(0.1 * kSampleRate)) > kHeard, "the loop plays");
    // The default global post-chain has a Doubler, a delay, so the gate waits out its long hold.
    rig.Run(static_cast<std::int64_t>((SilenceSleepGate::kRingOutHoldSeconds + 1.0) * kSampleRate));
    Check(rig.mixer.IsSleepingOnSilence(), "playback alone does not keep the chains awake");
    Check(rig.lastPeak > kHeard, "and the loop plays on through the sleep");
}

void TestArmedHoldsTheChainsAwake()
{
    Rig rig;
    rig.mixer.Looper().Settings().startArmed.store(true);
    rig.Run(static_cast<std::int64_t>((SilenceSleepGate::kRingOutHoldSeconds + 1.0) * kSampleRate));
    Check(rig.mixer.IsSleepingOnSilence(), "asleep with nothing to do");
    rig.mixer.Looper().Press(LooperAction::RecPlayDub);
    rig.Run(static_cast<std::int64_t>(3.0 * kSampleRate));
    Check(rig.mixer.Looper().State() == LooperState::Armed, "armed and waiting");
    Check(!rig.mixer.IsSleepingOnSilence(), "armed holds the chains awake for the first note");
}

void TestLayersSurviveAPresetSwapUntouched()
{
    Rig rig;
    rig.RecordLoop();
    rig.Run(static_cast<std::int64_t>(0.3 * kSampleRate));
    const std::vector<float> before = rig.capture.wet0;
    const Preset other = MakePassThroughPreset("b");
    rig.mixer.PreparePresetSwap(other, other.id, other.name);
    rig.mixer.CommitPresetSwap();
    rig.Run(static_cast<std::int64_t>(1.0 * kSampleRate));
    Check(rig.capture.wet0 == before, "a preset swap leaves every layer bit-identical");
    Check(rig.lastPeak > kHeard, "and the loop plays on");
}

void TestTheTunerMuteKeepsTimeSilently()
{
    Rig rig;
    rig.RecordLoop();
    rig.Run(kBlock * 10);
    const std::int64_t playhead = rig.Snap().playhead;
    rig.mixer.SetTunerEnabled(true);
    rig.mixer.SetLiveTunerMode(false);
    Check(rig.Run(kBlock * 100) == 0.0f, "the tuner's mute silences the loop");
    const std::int64_t length = rig.Snap().length;
    Check(rig.Snap().playhead == (playhead + kBlock * 100) % length, "but it keeps time");
    rig.mixer.SetTunerEnabled(false);
    Check(rig.Run(kBlock * 10) > kHeard, "and plays again when the tuner closes");
}

void TestTheOutputStageCoversTheLoop()
{
    Rig rig;
    rig.RecordLoop();
    rig.mixer.SetOutputMuted(true);
    Check(rig.Run(kBlock * 10) == 0.0f, "output mute silences the loop");
    rig.mixer.SetOutputMuted(false);
    rig.mixer.SetMasterGain(8.0);
    rig.mixer.SetLimiterEnabled(true);
    const auto ceiling = static_cast<float>(GetOutputProtectionCeilingLinear());
    const float peak = rig.Run(kBlock * 100);
    Check(peak > 0.5f * ceiling && peak <= ceiling + 1e-6f, "master gain and the limiter apply to the loop");

    Rig mono;
    mono.monoOutput = true;
    mono.RecordLoop();
    Check(mono.Run(kBlock * 10) > kHeard, "a mono output gets the loop folded in");
}
} // namespace

int main()
{
    RegisterAllEffects();
    TestTheTapsAreTheDiBeforeCalibrationAndTheWetAfterTheChains();
    TestTheLoopPlaysWhileTheChainsSleep();
    TestArmedHoldsTheChainsAwake();
    TestLayersSurviveAPresetSwapUntouched();
    TestTheTunerMuteKeepsTimeSilently();
    TestTheOutputStageCoversTheLoop();

    if (gFailures > 0)
    {
        std::cerr << gFailures << " check(s) failed" << std::endl;
        return 1;
    }

    std::cout << "MixerLooperTests passed" << std::endl;
    return 0;
}

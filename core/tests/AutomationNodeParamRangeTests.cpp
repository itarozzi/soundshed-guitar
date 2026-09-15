/**
 * AutomationNodeParamRangeTests.cpp — A node.* slot drives its parameter across the range
 * the effect declares.
 *
 * Slot values are 0..1 whether MIDI, the DAW or the keyboard wrote them, while an effect
 * takes its parameters in native units. This maps a CC to the gain effect's gainDb and
 * checks the node reads back the ends and middle of that dB range — not 0..1 dB, which is
 * what a node.* slot delivered before the value was mapped.
 */

#include <cmath>
#include <iostream>
#include <optional>
#include <string>

#include "automation/AutomationSlotTable.h"
#include "dsp/EffectRegistry.h"
#include "dsp/MultiPresetMixer.h"
#include "dsp/effects/BuiltinEffects.h"
#include "presets/PresetTypes.h"
#include "resources/ResourceLibrary.h"

using namespace guitarfx;

namespace
{
constexpr double kSampleRate = 48000.0;
constexpr int kBlockSize = 64;
constexpr double kToleranceDb = 1e-4;

bool Expect(bool condition, const std::string& message)
{
    if (!condition)
    {
        std::cerr << "FAIL: " << message << std::endl;
    }

    return condition;
}

Preset MakeGainPreset()
{
    Preset preset;
    preset.id = "rangePreset";
    preset.name = "RangePreset";

    GraphNode in{"in", kNodeTypeInput, "", "Input", true};
    GraphNode gain{"g1", "gain", "utility", "Gain", true};
    gain.params["gainDb"] = 0.0;
    GraphNode out{"out", kNodeTypeOutput, "", "Output", true};

    preset.graph.nodes = {in, gain, out};
    preset.graph.edges = {
        GraphEdge{"in", "g1", 0, 0, 1.0},
        GraphEdge{"g1", "out", 0, 0, 1.0},
    };
    return preset;
}

bool ExpectGainDb(const MultiPresetMixer& mixer, double expected, const std::string& message)
{
    const auto readouts = mixer.ReadNodeParamsForType("gain", {"gainDb"});

    if (readouts.empty() || readouts.front().values.empty())
    {
        return Expect(false, message + " (no gain node to read)");
    }

    const double actual = readouts.front().values.front();
    return Expect(std::abs(actual - expected) < kToleranceDb,
                  message + ": expected " + std::to_string(expected) + " dB, read " + std::to_string(actual) + " dB");
}
} // namespace

int main()
{
    RegisterAllEffects();

    bool allPassed = true;

    ResourceLibrary library;
    MultiPresetMixer mixer;
    mixer.SetResourceLibrary(&library);
    mixer.Prepare(kSampleRate, kBlockSize);

    const auto preset = MakeGainPreset();

    if (!mixer.AddActivePreset(preset, preset.id, preset.name))
    {
        std::cerr << "Failed to add gain preset" << std::endl;
        return 1;
    }

    const auto* gainDb = EffectRegistry::Instance().FindParameter("gain", "gainDb");

    if (!gainDb || gainDb->maxValue - gainDb->minValue <= 1.0)
    {
        std::cerr << "The gain effect must declare gainDb with a range wider than 0..1" << std::endl;
        return 1;
    }

    AutomationSlotTable table;
    table.InitializeRegistry(
        mixer, []() { return 0.0; }, [](int) {}, [](int) {}, [](int) {}, []() { return 0; }, []() { return 0; },
        [](int) {}, []() { return 0; }, [](int) {}, []() { return -1; });
    table.SetMixer(&mixer);
    table.SetEffectRegistry(&EffectRegistry::Instance());

    // The value handed on to the UI (signalPathNodeParamUpdated) must be native too.
    std::optional<double> notifiedValue;
    table.SetOnNodeParamApplied(
        [&notifiedValue](const std::string&, const std::string&, double value) { notifiedValue = value; });

    MidiControlMap midiMap;
    midiMap.eventType = MidiControlMap::EventType::CC;
    midiMap.channel = 0;
    midiMap.controller = 7;
    midiMap.mode = MidiControlMap::Mode::Absolute;

    const bool created = table.SetCustomSlot("custom.gain", std::optional<std::string>("Gain"),
                                             std::optional<std::string>("node.gain.gainDb"), std::nullopt,
                                             std::optional<MidiControlMap>(midiMap), std::nullopt);
    allPassed &= Expect(created, "Failed to create the gain slot");

    const auto nativeAt = [gainDb](double normalized) {
        return gainDb->minValue + normalized * (gainDb->maxValue - gainDb->minValue);
    };

    table.HandleMidi(MidiEvent{0xB0, 7, 0, 0});
    allPassed &= ExpectGainDb(mixer, gainDb->minValue, "CC 0 should reach the bottom of the range");

    table.HandleMidi(MidiEvent{0xB0, 7, 127, 0});
    allPassed &= ExpectGainDb(mixer, gainDb->maxValue, "CC 127 should reach the top of the range");
    allPassed &= Expect(notifiedValue.has_value() && std::abs(*notifiedValue - gainDb->maxValue) < kToleranceDb,
                        "The UI notification should carry the native value");

    table.HandleMidi(MidiEvent{0xB0, 7, 64, 0});
    allPassed &= ExpectGainDb(mixer, nativeAt(64.0 / 127.0), "CC 64 should land mid-range");

    // A DAW write takes the same path.
    table.ApplyAutomationLocked("custom.gain", 0.75f, AutomationSource::DAW);
    allPassed &= ExpectGainDb(mixer, nativeAt(0.75), "A DAW value of 0.75 should land three quarters up the range");

    if (allPassed)
    {
        std::cout << "AutomationNodeParamRange tests passed" << std::endl;
        return 0;
    }

    return 1;
}

/**
 * AutomationNodeParamRangeTests.cpp — A node.* slot drives its parameter across the range
 * the effect declares.
 *
 * Slot values are 0..1 whether MIDI, the DAW or the keyboard wrote them, while an effect
 * takes its parameters in native units. This maps a CC to the gain effect's gainDb and
 * checks the node reads back the ends and middle of that dB range — not 0..1 dB, which is
 * what a node.* slot delivered before the value was mapped.
 *
 * A node can narrow that range with its own settings: an expression pedal on a pitch shift
 * sweeps the node's Range Min..Range Max, in whole semitones only while it snaps.
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

constexpr double kPitchRangeMin = 0.0;
constexpr double kPitchRangeMax = 7.0;

Preset MakeRangePreset()
{
    Preset preset;
    preset.id = "rangePreset";
    preset.name = "RangePreset";

    GraphNode in{"in", kNodeTypeInput, "", "Input", true};
    GraphNode gain{"g1", "gain", "utility", "Gain", true};
    gain.params["gainDb"] = 0.0;
    GraphNode pitch{"p1", "pitch_shift", "pitch", "Pitch Shift", true};
    pitch.params["semitones"] = 0.0;
    pitch.params["minSemitones"] = kPitchRangeMin;
    pitch.params["maxSemitones"] = kPitchRangeMax;
    pitch.params["stepMode"] = 1.0;
    GraphNode out{"out", kNodeTypeOutput, "", "Output", true};

    preset.graph.nodes = {in, gain, pitch, out};
    preset.graph.edges = {
        GraphEdge{"in", "g1", 0, 0, 1.0},
        GraphEdge{"g1", "p1", 0, 0, 1.0},
        GraphEdge{"p1", "out", 0, 0, 1.0},
    };
    return preset;
}

bool ExpectParam(const MultiPresetMixer& mixer, const std::string& type, const std::string& paramId, double expected,
                 const std::string& message)
{
    const auto readouts = mixer.ReadNodeParamsForType(type, {paramId});

    if (readouts.empty() || readouts.front().values.empty())
    {
        return Expect(false, message + " (no " + type + " node to read)");
    }

    const double actual = readouts.front().values.front();
    return Expect(std::abs(actual - expected) < kToleranceDb,
                  message + ": expected " + std::to_string(expected) + ", read " + std::to_string(actual));
}

bool ExpectGainDb(const MultiPresetMixer& mixer, double expected, const std::string& message)
{
    return ExpectParam(mixer, "gain", "gainDb", expected, message);
}

bool ExpectSemitones(const MultiPresetMixer& mixer, double expected, const std::string& message)
{
    return ExpectParam(mixer, "pitch_shift", "semitones", expected, message);
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

    const auto preset = MakeRangePreset();

    if (!mixer.AddActivePreset(preset, preset.id, preset.name))
    {
        std::cerr << "Failed to add range preset" << std::endl;
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

    // An expression pedal on the pitch shift sweeps the node's own range, not the -12..12 st
    // the effect type declares.
    MidiControlMap pedalMap = midiMap;
    pedalMap.controller = 11;

    const bool pedalCreated = table.SetCustomSlot("custom.pitch", std::optional<std::string>("Whammy"),
                                                  std::optional<std::string>("node.pitch_shift.semitones"),
                                                  std::nullopt, std::optional<MidiControlMap>(pedalMap), std::nullopt);
    allPassed &= Expect(pedalCreated, "Failed to create the pitch slot");

    const double pedalMid = kPitchRangeMin + (64.0 / 127.0) * (kPitchRangeMax - kPitchRangeMin);

    table.HandleMidi(MidiEvent{0xB0, 11, 0, 0});
    allPassed &= ExpectSemitones(mixer, kPitchRangeMin, "Heel down should reach Range Min");

    table.HandleMidi(MidiEvent{0xB0, 11, 127, 0});
    allPassed &= ExpectSemitones(mixer, kPitchRangeMax, "Toe down should reach Range Max");
    allPassed &= Expect(notifiedValue.has_value() && std::abs(*notifiedValue - kPitchRangeMax) < kToleranceDb,
                        "The pitch notification should carry semitones");

    table.HandleMidi(MidiEvent{0xB0, 11, 64, 0});
    allPassed &= ExpectSemitones(mixer, std::round(pedalMid), "Snapped, mid-travel should land on a whole semitone");

    mixer.SetNodeParam(preset.id, "p1", "stepMode", 0.0);
    table.HandleMidi(MidiEvent{0xB0, 11, 64, 0});
    allPassed &= ExpectSemitones(mixer, pedalMid, "Free, mid-travel should glide between semitones");

    // Widening the range on the node widens the sweep with no change to the mapping.
    mixer.SetNodeParam(preset.id, "p1", "minSemitones", -12.0);
    mixer.SetNodeParam(preset.id, "p1", "maxSemitones", 12.0);
    table.HandleMidi(MidiEvent{0xB0, 11, 0, 0});
    allPassed &= ExpectSemitones(mixer, -12.0, "A widened range should reach its new min");

    if (allPassed)
    {
        std::cout << "AutomationNodeParamRange tests passed" << std::endl;
        return 0;
    }

    return 1;
}

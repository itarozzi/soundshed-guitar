/**
 * @file SetlistMidiOutTests.cpp
 * @brief A bank or setlist slot picked in the app is sent to the MIDI device as Bank Select and
 * Program Change, when the settings ask for it.
 *
 * SetlistMidiOut on its own:
 *  - each bank select mode splits the bank across CC0 and CC32 as documented, on the channel
 *    asked for, before the Program Change;
 *  - a channel, bank or program that cannot be sent sends nothing at all;
 *  - the newest pick replaces one not yet collected, and Take() leaves it queued when its
 *    buffer is too small.
 *
 * Through the controller, as the UI drives it:
 *  - nothing is sent until midi.setlistOut.enabled is on;
 *  - "setSetlistCursor" sends the active setlist's bank and the slot; "selectSetlist" sends the
 *    bank alone, and nothing for a setlist with no bank number;
 *  - an empty slot sends nothing;
 *  - a slot or bank picked by a MIDI footswitch is never echoed back.
 */

#include <array>
#include <cstdint>
#include <iostream>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "MessageThreadTestHost.h"
#include "PluginController.h"
#include "automation/AutomationTypes.h"
#include "controller/SetlistMidiOut.h"
#include "dsp/effects/BuiltinEffects.h"
#include "presets/PresetStorage.h"
#include "presets/PresetTypes.h"

namespace fs = std::filesystem;
using namespace guitarfx;

namespace
{
constexpr double kSampleRate = 48000.0;
constexpr int kBlock = 128;
constexpr const char* kPresetA = "setlist-midi-out-a";
constexpr const char* kPresetB = "setlist-midi-out-b";
constexpr int kSetlistTwoCc = 22;
constexpr int kBankUpCc = 23;

using Mode = SetlistMidiOut::BankSelect;
using Bytes = std::vector<std::vector<std::uint8_t>>;

bool Check(bool condition, const std::string& what)
{
    std::cout << (condition ? "[PASS] " : "[FAIL] ") << what << "\n";
    return condition;
}

Bytes ToBytes(std::span<const SetlistMidiOut::ShortMessage> messages)
{
    Bytes bytes;

    for (const auto& message : messages)
    {
        bytes.emplace_back(message.bytes.begin(), message.bytes.begin() + message.size);
    }

    return bytes;
}

std::string Describe(const Bytes& bytes)
{
    static constexpr char kHex[] = "0123456789ABCDEF";
    std::string text;

    for (const auto& message : bytes)
    {
        text += text.empty() ? "[" : " [";

        for (std::size_t i = 0; i < message.size(); ++i)
        {
            text += i == 0 ? "" : " ";
            text += kHex[message[i] >> 4];
            text += kHex[message[i] & 0x0F];
        }

        text += "]";
    }

    return text.empty() ? "nothing" : text;
}

Bytes Build(int channel, Mode mode, std::optional<int> bank, std::optional<int> program)
{
    SetlistMidiOut::Messages out{};
    const auto count = SetlistMidiOut::Build(channel, mode, bank, program, out);
    return ToBytes(std::span(out).first(count));
}

bool ExpectBuild(const Bytes& actual, const Bytes& expected, const std::string& what)
{
    return Check(actual == expected, what + ": " + Describe(actual));
}

bool RunBuildTests()
{
    bool passed = true;

    passed = ExpectBuild(Build(1, Mode::MsbLsb, 5, 2), {{0xB0, 0, 0}, {0xB0, 32, 5}, {0xC0, 2}},
                         "MSB + LSB: CC0 0, CC32 5, then PC 2 on channel 1") &&
             passed;
    passed = ExpectBuild(Build(16, Mode::MsbLsb, 200, 0), {{0xBF, 0, 1}, {0xBF, 32, 72}, {0xCF, 0}},
                         "MSB + LSB: bank 200 is 1 * 128 + 72, on channel 16") &&
             passed;
    passed = ExpectBuild(Build(3, Mode::Msb, 7, 4), {{0xB2, 0, 7}, {0xC2, 4}}, "MSB only: CC0 7, PC 4 on channel 3") &&
             passed;
    passed = ExpectBuild(Build(3, Mode::Lsb, 7, 4), {{0xB2, 32, 7}, {0xC2, 4}}, "LSB only: CC32 7, PC 4") && passed;
    passed = ExpectBuild(Build(1, Mode::None, 7, 4), {{0xC0, 4}}, "no bank select: PC alone") && passed;
    passed = ExpectBuild(Build(1, Mode::MsbLsb, std::nullopt, 4), {{0xC0, 4}}, "a setlist with no bank: PC alone") &&
             passed;
    passed = ExpectBuild(Build(1, Mode::Msb, 9, std::nullopt), {{0xB0, 0, 9}}, "a bank on its own: Bank Select alone") &&
             passed;

    passed = ExpectBuild(Build(1, Mode::Msb, 128, 0), {}, "MSB only cannot send bank 128: nothing") && passed;
    passed = ExpectBuild(Build(1, Mode::MsbLsb, 16384, 0), {}, "MSB + LSB cannot send bank 16384: nothing") && passed;
    passed = ExpectBuild(Build(1, Mode::MsbLsb, -1, 0), {}, "a negative bank: nothing") && passed;
    passed = ExpectBuild(Build(1, Mode::MsbLsb, 1, 128), {}, "slot 129 has no program number: nothing") && passed;
    passed = ExpectBuild(Build(0, Mode::MsbLsb, 1, 0), {}, "channel 0: nothing") && passed;
    passed = ExpectBuild(Build(17, Mode::MsbLsb, 1, 0), {}, "channel 17: nothing") && passed;

    passed = Check(SetlistMidiOut::ParseBankSelect("msb") == Mode::Msb &&
                       SetlistMidiOut::ParseBankSelect("lsb") == Mode::Lsb &&
                       SetlistMidiOut::ParseBankSelect("none") == Mode::None &&
                       SetlistMidiOut::ParseBankSelect("msbLsb") == Mode::MsbLsb &&
                       SetlistMidiOut::ParseBankSelect("") == Mode::MsbLsb &&
                       SetlistMidiOut::ParseBankSelect("junk") == Mode::MsbLsb,
                   "ParseBankSelect: each value, and the default for anything else") &&
             passed;

    SetlistMidiOut feed;
    std::array<SetlistMidiOut::ShortMessage, SetlistMidiOut::kMaxMessages> out{};
    passed = Check(feed.Take(out) == 0, "feed: nothing queued at first") && passed;
    passed = Check(!feed.Queue(1, Mode::Msb, 500, 0) && feed.Take(out) == 0,
                   "feed: a pick that cannot be sent queues nothing") &&
             passed;

    (void)feed.Queue(1, Mode::MsbLsb, 1, 0);
    (void)feed.Queue(2, Mode::None, 1, 3);
    std::array<SetlistMidiOut::ShortMessage, 0> tooSmall{};
    passed = Check(feed.Take(tooSmall) == 0, "feed: a buffer too small takes nothing") && passed;
    const auto count = feed.Take(out);
    passed = ExpectBuild(ToBytes(std::span(out).first(count)), {{0xC1, 3}},
                         "feed: the newest pick replaced the first, and was kept for the larger buffer") &&
             passed;
    passed = Check(feed.Take(out) == 0, "feed: taken once") && passed;

    return passed;
}

// ── Through the controller ──────────────────────────────────────────

Preset BuildPreset(const std::string& id)
{
    Preset preset;
    preset.id = id;
    preset.name = id;
    preset.version = 2;
    preset.category = "Test";

    GraphNode in;
    in.id = "in";
    in.type = kNodeTypeInput;

    GraphNode out;
    out.id = "out";
    out.type = kNodeTypeOutput;

    preset.graph.nodes = {in, out};
    preset.graph.edges = {{"in", "out", 0, 0, 1.0}};
    NormalizePresetScenes(preset);
    return preset;
}

nlohmann::json PresetJson(const Preset& preset)
{
    return nlohmann::json::parse(PresetStorage::SerializeToJson(preset));
}

void Send(PluginController& controller, const nlohmann::json& message)
{
    controller.HandleUIMessage(message.dump());
}

void SetSetting(PluginController& controller, const std::string& key, const nlohmann::json& value)
{
    Send(controller, {{"type", "setSetting"}, {"key", key}, {"value", value}});
}

void PickSlot(PluginController& controller, int index)
{
    Send(controller, {{"type", "setSetlistCursor"}, {"cursorIndex", index}});
}

void PickSetlist(PluginController& controller, const std::string& id)
{
    Send(controller, {{"type", "selectSetlist"}, {"setlistId", id}});
}

/// What the adapter's next processBlock would add to the MIDI output.
Bytes TakeSent(PluginController& controller)
{
    std::array<SetlistMidiOut::ShortMessage, SetlistMidiOut::kMaxMessages> out{};
    const auto count = controller.TakeSetlistMidiOut(out);
    return ToBytes(std::span(out).first(count));
}

void MapFootswitch(PluginController& controller, const std::string& slotId, int cc)
{
    MidiControlMap map;
    map.eventType = MidiControlMap::EventType::CC;
    map.channel = -1;
    map.controller = cc;
    (void)controller.GetAutomationSlots().SetDefaultSlotOverrides(slotId, std::nullopt, map, std::nullopt);
}

void PressFootswitch(PluginController& controller, int cc)
{
    const auto number = static_cast<std::uint8_t>(cc);
    controller.EnqueueMidi(MidiEvent{0xB0, number, 127, 0});
    controller.EnqueueMidi(MidiEvent{0xB0, number, 0, 0});
    controller.ProcessQueuedMidi();
    controller.DrainControlSurfaceRequests();
}

std::string ActivePresetId(const PluginController& controller)
{
    const auto& active = controller.GetActivePreset();
    return active ? active->id : std::string{"<none>"};
}

bool RunControllerTests(const fs::path& sandbox)
{
    test::PumpedTestHost host(sandbox, std::this_thread::get_id(), kSampleRate, kBlock);
    PluginController controller(host);
    controller.Initialize();
    controller.Prepare(kSampleRate, kBlock);
    host.Pump();

    for (const auto& preset : {BuildPreset(kPresetA), BuildPreset(kPresetB)})
    {
        Send(controller,
             {{"type", "savePreset"}, {"presetId", preset.id}, {"name", preset.name}, {"preset", PresetJson(preset)}});
    }

    const auto slots = nlohmann::json::array({{{"presetId", kPresetA}}, {{"presetId", kPresetB}}, {{"presetId", ""}}});
    Send(controller,
         {{"type", "setSetlists"},
          {"setlists", nlohmann::json::array({{{"id", "set-1"}, {"name", "Set 1"}, {"bank", 1}, {"slots", slots}},
                                              {{"id", "set-2"}, {"name", "Set 2"}, {"bank", 200}, {"slots", slots}},
                                              {{"id", "set-3"}, {"name", "Set 3"}, {"slots", slots}}})},
          {"activeSetlistId", "set-1"},
          {"cursorIndex", 0}});
    host.Pump();
    MapFootswitch(controller, "default.setlistPreset2", kSetlistTwoCc);
    MapFootswitch(controller, "default.bankUp", kBankUpCc);
    (void)TakeSent(controller);

    bool passed = true;

    // Off until asked for.
    PickSlot(controller, 1);
    passed = Check(ActivePresetId(controller) == kPresetB, "setting off: the pick still loads the slot") && passed;
    passed = ExpectBuild(TakeSent(controller), {}, "setting off: nothing is sent") && passed;

    SetSetting(controller, "midi.setlistOut.enabled", true);
    SetSetting(controller, "midi.setlistOut.channel", 3);

    PickSlot(controller, 0);
    passed = Check(ActivePresetId(controller) == kPresetA, "slot pick: loads slot 1") && passed;
    passed = ExpectBuild(TakeSent(controller), {{0xB2, 0, 0}, {0xB2, 32, 1}, {0xC2, 0}},
                         "slot pick: bank 1, then PC 0, on channel 3") &&
             passed;

    PickSlot(controller, 0);
    passed = ExpectBuild(TakeSent(controller), {{0xB2, 0, 0}, {0xB2, 32, 1}, {0xC2, 0}},
                         "slot pick of the preset already playing: still sent, the cursor moved to it") &&
             passed;

    PickSlot(controller, 2);
    passed = ExpectBuild(TakeSent(controller), {}, "empty slot: nothing is sent") && passed;
    PickSlot(controller, 9);
    passed = ExpectBuild(TakeSent(controller), {}, "slot past the end: nothing is sent") && passed;

    PickSetlist(controller, "set-2");
    passed = Check(controller.GetSetlistBankNumber() == 200, "setlist pick: set 2 is active") && passed;
    passed = ExpectBuild(TakeSent(controller), {{0xB2, 0, 1}, {0xB2, 32, 72}},
                         "setlist pick: its bank 200 alone, as CC0 1 + CC32 72") &&
             passed;

    PickSlot(controller, 1);
    passed = ExpectBuild(TakeSent(controller), {{0xB2, 0, 1}, {0xB2, 32, 72}, {0xC2, 1}},
                         "slot pick in set 2: bank 200, then PC 1") &&
             passed;

    SetSetting(controller, "midi.setlistOut.bankSelect", "msb");
    PickSlot(controller, 0);
    passed = ExpectBuild(TakeSent(controller), {}, "CC0 only cannot send bank 200: nothing, not the wrong bank") &&
             passed;

    PickSetlist(controller, "set-1");
    passed = ExpectBuild(TakeSent(controller), {{0xB2, 0, 1}}, "CC0 only: set 1's bank as CC0 1") && passed;

    PickSetlist(controller, "set-3");
    passed = ExpectBuild(TakeSent(controller), {}, "setlist pick with no bank number: nothing") && passed;
    PickSlot(controller, 1);
    passed = ExpectBuild(TakeSent(controller), {{0xC2, 1}}, "slot pick in a setlist with no bank number: PC alone") &&
             passed;

    SetSetting(controller, "midi.setlistOut.bankSelect", "none");
    PickSetlist(controller, "set-1");
    passed = ExpectBuild(TakeSent(controller), {}, "no bank select: a setlist pick sends nothing") && passed;

    // From the device itself: never echoed back.
    PressFootswitch(controller, kSetlistTwoCc);
    passed = Check(ActivePresetId(controller) == kPresetB && controller.GetSetlistCursorIndex() == 1,
                   "MIDI footswitch: loads slot 2") &&
             passed;
    passed = ExpectBuild(TakeSent(controller), {}, "MIDI footswitch: nothing is echoed back") && passed;
    PressFootswitch(controller, kBankUpCc);
    passed = Check(controller.GetSetlistBankNumber() == 200, "MIDI bank up: set 2 is active") && passed;
    passed = ExpectBuild(TakeSent(controller), {}, "MIDI bank up: nothing is echoed back") && passed;

    SetSetting(controller, "midi.setlistOut.enabled", false);
    PickSlot(controller, 0);
    passed = ExpectBuild(TakeSent(controller), {}, "switched back off: nothing is sent") && passed;

    return passed;
}
} // namespace

int main()
{
    std::cout << std::unitbuf;

    const fs::path sandbox = fs::temp_directory_path() / "guitarfx-setlist-midi-out-tests";
    std::error_code ec;
    fs::remove_all(sandbox, ec);
    fs::create_directories(sandbox, ec);
    test::SetSettingsEnvRoot(sandbox);
    RegisterAllEffects();

    bool passed = RunBuildTests();
    passed = RunControllerTests(sandbox) && passed;
    std::cout << (passed ? "SetlistMidiOutTests PASSED\n" : "SetlistMidiOutTests FAILED\n");
    return passed ? 0 : 1;
}

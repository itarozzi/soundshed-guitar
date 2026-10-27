#pragma once

/**
 * Automation - MIDI mapping for the native UIs, as the web UI's midiMapping.ts, midiLearnMenu.ts
 * and automationPanel.ts do it: which address a control drives, which slot already drives it,
 * what a control's right-click "MIDI Learn" menu offers and does, how a mapping reads on screen,
 * and what the MIDI & Automation panel's target picker lists.
 *
 * The slots themselves, the learns and the keyboard mappings are the engine's
 * (ClientState::automation, UiCommands). Keys are run by the engine (automationKey); a UI only
 * passes on the keys it does not use, and KeyIsMapped says which to hold back.
 */

#include "uiclient/ClientState.h"

#include <nlohmann/json.hpp>

#include <optional>
#include <string>
#include <vector>

namespace guitarfx::uiclient
{
class UiCommands;

/// The slots as the engine sends them (automation.slots, and the full state's "automation").
[[nodiscard]] std::vector<AutomationSlotInfo> ParseAutomationSlots(const nlohmann::json& slots);
[[nodiscard]] std::vector<AutomationRegistryEntry> ParseAutomationRegistry(const nlohmann::json& registry);

// ── What a control drives ────────────────────────────────────────────────────────

/// An automation address a control drives, and a label for it.
struct MidiLearnTarget
{
    std::string address;
    std::string label;
};

/// An address from the engine's registry (global.*, setlist.*), with its label; nothing when the
/// registry does not offer it.
[[nodiscard]] std::optional<MidiLearnTarget> RegistryLearnTarget(const AutomationState& automation,
                                                                 const std::string& address);

/// The global input and output level knobs: "input" or "output".
[[nodiscard]] std::optional<MidiLearnTarget> GlobalKnobLearnTarget(const AutomationState& automation,
                                                                   const std::string& knob);

/// An effect parameter: node.<type>.<key>, labelled "Effect: Param". Nothing for a parameter the
/// type does not declare, and for the plugin and Custom Effect hosts, whose parameters belong to
/// what a node has loaded rather than to its type.
[[nodiscard]] std::optional<MidiLearnTarget> NodeParamLearnTarget(const EffectTypeInfo* type,
                                                                  const std::string& paramKey);

/// A setlist pad: setlist.preset<N>, the Nth slot of whichever setlist is active (0-based index).
[[nodiscard]] std::optional<MidiLearnTarget> SetlistPadLearnTarget(const AutomationState& automation, int slotIndex);

// ── Slots ────────────────────────────────────────────────────────────────────────

/// The slot that drives `address` in every preset: a default slot wins over a custom one.
[[nodiscard]] const AutomationSlotInfo* FindSlotForAddress(const std::vector<AutomationSlotInfo>& slots,
                                                           const std::string& address);
/// The mapping `presetId` holds for `address` itself, live only while that preset is active.
[[nodiscard]] const AutomationSlotInfo* FindPresetSlotForAddress(const std::vector<AutomationSlotInfo>& slots,
                                                                 const std::string& presetId,
                                                                 const std::string& address);
/// Whether a control can have a mapping of its own in each preset: effect parameters and the
/// input and output levels; not the setlist and bank controls, which pick the preset.
[[nodiscard]] bool AllowsPresetMapping(const std::string& address);
[[nodiscard]] int CountCustomSlots(const std::vector<AutomationSlotInfo>& slots);
[[nodiscard]] int CountPresetSlots(const std::vector<AutomationSlotInfo>& slots, const std::string& presetId);
/// A "custom.N" / "preset.N" id no slot uses yet.
[[nodiscard]] std::string NextCustomSlotId(const std::vector<AutomationSlotInfo>& slots);
[[nodiscard]] std::string NextPresetSlotId(const std::vector<AutomationSlotInfo>& slots);

// ── How mappings read ────────────────────────────────────────────────────────────

/// Channels are 0-15 on the wire and -1 for any; musicians count 1-16.
[[nodiscard]] std::string FormatMidiChannel(int channel);
/// "CC 7 ch1 Abs"; without the mode when `withMode` is false (a learn capture has none).
[[nodiscard]] std::string DescribeMidiMap(int eventType, int channel, int controller, std::optional<int> mode);
[[nodiscard]] std::string DescribeMidiMap(const AutomationMidiMap& map);
/// A slot's keys: "z(trig), Space(=0.50)"; "—" when it has none.
[[nodiscard]] std::string DescribeKeyMaps(const AutomationSlotInfo& slot);
/// "(0..1 dB)": the registry entry's range, or "" for an address it does not hold.
[[nodiscard]] std::string DescribeRange(const AutomationRegistryEntry* entry);

/// Whether the engine maps this key (DOM KeyboardEvent.key) to any slot, compared as it compares
/// them: one character without regard to case. A UI holds such a key back from its own shortcuts.
[[nodiscard]] bool KeyIsMapped(const AutomationState& automation, const std::string& key);

// ── The right-click menu ─────────────────────────────────────────────────────────

enum class MidiLearnMenuAction
{
    Learn,
    LearnPreset,
    Cancel,
    Clear
};

struct MidiLearnMenuItem
{
    MidiLearnMenuAction action;
    std::string label;
    std::string hint;
    bool disabled = false;
};

/// What the menu offers for `target` (midiMapping.ts buildMidiLearnMenuItems): MIDI Learn, and
/// MIDI Learn for this preset on a signal chain control (Cancel instead while either of the
/// control's slots is listening), and Clear Mapping when there is a mapping to clear, the active
/// preset's own first.
[[nodiscard]] std::vector<MidiLearnMenuItem> BuildMidiLearnMenuItems(const AutomationState& automation,
                                                                     const MidiLearnTarget& target,
                                                                     const std::string& activePresetId);

/// The menu's title: the slot's label when one drives the control, else the target's.
[[nodiscard]] std::string MidiLearnMenuTitle(const AutomationState& automation, const MidiLearnTarget& target,
                                             const std::string& activePresetId);

/// Runs a menu action (midiLearnMenu.ts runMenuAction). A learn goes into the slot that already
/// drives the control, or one made for it (with `presetId`, that preset's own). Returns a line
/// to show the user ("Cleared MIDI mapping for Gain"), or why it could not ("all 16 custom slots
/// are in use"); empty for a learn that started.
std::string RunMidiLearnMenuAction(UiCommands& commands, const ClientState& state, MidiLearnMenuAction action,
                                   const MidiLearnTarget& target);

// ── The panel ─────────────────────────────────────────────────────────────────────

/// A node address's label from the catalog ("Delay: Mix", "Delay: Bypass"), a registry
/// address's own, or "Custom".
[[nodiscard]] std::string DeriveSlotLabel(const ClientState& state, const std::string& address);
[[nodiscard]] bool IsBypassAddress(const std::string& address);
/// Whether the panel offers Test on a slot: a trigger, or any custom slot.
[[nodiscard]] bool ShowsTestButton(const AutomationState& automation, const AutomationSlotInfo& slot);
/// What Test sends: a trigger fires (1), a bypass or a toggle flips, anything else goes to the
/// top and back.
[[nodiscard]] double TestValue(const AutomationState& automation, const AutomationSlotInfo& slot);

/// An address the target picker offers, and the group it sits in.
struct AutomationTargetOption
{
    std::string address;
    std::string label;
    std::string group;
};

/// Every address a custom slot can target: the registry's (Global, Setlist), the chain's Input
/// and Output gains, and each offered effect's Bypass and parameters, grouped by category.
[[nodiscard]] std::vector<AutomationTargetOption> BuildTargetOptions(const ClientState& state);
/// `options` with the slot's own address among them (under "Current" when the catalog does not
/// offer it), so picking from them never silently repoints the slot.
[[nodiscard]] std::vector<AutomationTargetOption> WithCurrentTargetOption(std::vector<AutomationTargetOption> options,
                                                                          const std::string& address,
                                                                          const std::string& label);
} // namespace guitarfx::uiclient

/**
 * AutomationSlotTable.cpp — Manages all automation slots and the apply dispatch.
 */

#include "automation/AutomationSlotTable.h"
#include "dsp/MultiPresetMixer.h"
#include "dsp/EffectRegistry.h"
#include "dsp/FiniteCheck.h"

#include <algorithm>
#include <cmath>

namespace guitarfx
{
namespace
{
bool IsBypassNodeAddress(const std::string& address)
{
    std::string effectType;
    std::string paramId;

    if (!ParamRegistry::ParseNodeAddress(address, effectType, paramId))
    {
        return false;
    }

    return paramId == "bypassed" || paramId == "bypass" || paramId == "enabled";
}

/// Maps a slot's 0..1 value onto the range an effect declares for a parameter, snapped to
/// the parameter's step, or to a whole index for an enum.
double DenormalizeNodeParam(const ParameterDef& def, double normalized)
{
    double native = def.minValue + normalized * (def.maxValue - def.minValue);

    if (def.step > 0.0)
    {
        native = def.minValue + std::round((native - def.minValue) / def.step) * def.step;
    }
    else if (!def.labels.empty())
    {
        native = std::round(native);
    }

    return std::clamp(native, std::min(def.minValue, def.maxValue), std::max(def.minValue, def.maxValue));
}

nlohmann::json MidiMapToJson(const MidiControlMap& map)
{
    nlohmann::json json = nlohmann::json::object();
    json["eventType"] = static_cast<int>(map.eventType);
    json["channel"] = map.channel;
    json["controller"] = map.controller;
    json["mode"] = static_cast<int>(map.mode);
    json["sensitivity"] = map.sensitivity;
    json["pickupRange"] = map.pickupRange;
    return json;
}

MidiControlMap MidiMapFromJson(const nlohmann::json& json)
{
    MidiControlMap map;
    map.eventType = static_cast<MidiControlMap::EventType>(json.value("eventType", 0));
    map.channel = json.value("channel", 0);
    map.controller = json.value("controller", 0);
    map.mode = static_cast<MidiControlMap::Mode>(json.value("mode", 0));
    map.sensitivity = json.value("sensitivity", 0.1f);
    map.pickupRange = json.value("pickupRange", 0.1f);
    return map;
}

/// Whether a mapping listens to this MIDI control. A NoteOn mapping also hears its key's
/// release, which arrives as NoteOff.
bool ListensTo(const MidiControlMap& map, MidiControlMap::EventType eventType, int channel, int controller)
{
    const bool typeMatches = map.eventType == eventType || (map.eventType == MidiControlMap::EventType::NoteOn &&
                                                            eventType == MidiControlMap::EventType::NoteOff);
    return typeMatches && map.controller == controller && (map.channel == -1 || map.channel == channel);
}
} // namespace

// ── AutomationSlot copy helpers ──────────────────────────────────────────

AutomationSlot::AutomationSlot(const AutomationSlot& other)
    : slotId(other.slotId), label(other.label), address(other.address), nodeSelector(other.nodeSelector),
      isDefault(other.isDefault), presetId(other.presetId), midiMap(other.midiMap), keyMaps(other.keyMaps),
      value(other.value.load()), lastSource(other.lastSource.load()), lastNormalized(other.lastNormalized.load()),
      lastToggleGate(other.lastToggleGate.load()), pendingApply(other.pendingApply.load())
{
}

AutomationSlot& AutomationSlot::operator=(const AutomationSlot& other)
{
    if (this != &other)
    {
        slotId = other.slotId;
        label = other.label;
        address = other.address;
        nodeSelector = other.nodeSelector;
        isDefault = other.isDefault;
        presetId = other.presetId;
        midiMap = other.midiMap;
        keyMaps = other.keyMaps;
        value.store(other.value.load());
        lastSource.store(other.lastSource.load());
        lastNormalized.store(other.lastNormalized.load());
        lastToggleGate.store(other.lastToggleGate.load());
        pendingApply.store(other.pendingApply.load());
    }

    return *this;
}

void AutomationSlot::SetValue(float normalized, AutomationSource src)
{
    value.store(normalized);
    lastSource.store(static_cast<int>(src));
    pendingApply.store(true);
}

// ── AutomationSlotTable ───────────────────────────────────────────────────

AutomationSlotTable::AutomationSlotTable()
{
    InitializeDefaultSlots();
}

AutomationSlotTable::~AutomationSlotTable() = default;

void AutomationSlotTable::InitializeDefaultSlots()
{
    mSlots.clear();

    for (const auto& def : kDefaultSlots)
    {
        AutomationSlot slot;
        slot.slotId = def.slotId;
        slot.address = def.address;
        slot.label = def.label;
        slot.isDefault = true;
        mSlots.push_back(std::move(slot));
    }
}

void AutomationSlotTable::InitializeRegistry(
    MultiPresetMixer& mixer, const std::function<double()>& getSetlistCursor,
    const std::function<void(int)>& applySetlistPresetByIndex, const std::function<void(int)>& bankUp,
    const std::function<void(int)>& bankDown, const std::function<int()>& getSetlistLength,
    const std::function<int()>& getSetlistBankBase, const std::function<void(int)>& selectSetlistBank,
    const std::function<int()>& getSetlistBankNumber, const std::function<void(int)>& selectSceneByIndex,
    const std::function<int()>& getActiveSceneIndex)
{
    mMixer = &mixer;
    mGetSetlistCursor = getSetlistCursor;
    mApplySetlistPresetByIndex = applySetlistPresetByIndex;
    mBankUp = bankUp;
    mBankDown = bankDown;
    mGetSetlistLength = getSetlistLength;
    mGetSetlistBankBase = getSetlistBankBase;
    mSelectSetlistBank = selectSetlistBank;
    mGetSetlistBankNumber = getSetlistBankNumber;
    mSelectSceneByIndex = selectSceneByIndex;
    mGetActiveSceneIndex = getActiveSceneIndex;

    // global.inputTrim
    {
        ParamRegistryEntry e;
        e.address = "global.inputTrim";
        e.label = "Input Trim";
        e.unit = "dB";
        e.minValue = -40.0;
        e.maxValue = 20.0;
        e.get = [this]() { return mMixer ? mMixer->GetGlobalChainConfig().inputGain : 0.0; };
        e.apply = [this](double v, bool) {
            if (mMixer)
            {
                mMixer->SetGlobalInputGain(v);
            }
        };
        mRegistry.Register(e);
    }

    // global.outputTrim
    {
        ParamRegistryEntry e;
        e.address = "global.outputTrim";
        e.label = "Output Trim";
        e.unit = "dB";
        e.minValue = -40.0;
        e.maxValue = 20.0;
        e.get = [this]() { return mMixer ? mMixer->GetGlobalChainConfig().outputGain : 0.0; };
        e.apply = [this](double v, bool) {
            if (mMixer)
            {
                mMixer->SetGlobalOutputGain(v);
            }
        };
        mRegistry.Register(e);
    }

    // setlist.preset1..N — one trigger parameter per preset slot in the current bank
    for (int i = 1; i <= kSetlistPresetsPerBank; ++i)
    {
        ParamRegistryEntry e;
        e.address = "setlist.preset" + std::to_string(i);
        e.label = "Setlist Preset " + std::to_string(i);
        e.unit = "";
        e.minValue = 0.0;
        e.maxValue = 1.0;
        e.isTrigger = true;
        const int slotOffset = i; // captured by value
        e.get = [this, slotOffset]() -> double {
            if (!mGetSetlistBankBase || !mGetSetlistCursor)
            {
                return 0.0;
            }

            const int base = mGetSetlistBankBase();
            const int cursor = static_cast<int>(std::round(mGetSetlistCursor()));
            return (cursor == base + (slotOffset - 1)) ? 1.0 : 0.0;
        };
        e.apply = [this, slotOffset](double, bool) {
            if (!mGetSetlistBankBase || !mApplySetlistPresetByIndex)
            {
                return;
            }

            const int base = mGetSetlistBankBase();
            mApplySetlistPresetByIndex(base + (slotOffset - 1));
        };
        mRegistry.Register(e);
    }

    // setlist.bankUp
    {
        ParamRegistryEntry e;
        e.address = "setlist.bankUp";
        e.label = "Bank Up";
        e.unit = "";
        e.minValue = 0.0;
        e.maxValue = 1.0;
        e.isTrigger = true;
        e.get = []() { return 0.0; };
        e.apply = [this](double, bool) {
            if (mBankUp)
            {
                mBankUp(1);
            }
        };
        mRegistry.Register(e);
    }

    // setlist.bankDown
    {
        ParamRegistryEntry e;
        e.address = "setlist.bankDown";
        e.label = "Bank Down";
        e.unit = "";
        e.minValue = 0.0;
        e.maxValue = 1.0;
        e.isTrigger = true;
        e.get = []() { return 0.0; };
        e.apply = [this](double, bool) {
            if (mBankDown)
            {
                mBankDown(1);
            }
        };
        mRegistry.Register(e);
    }

    // setlist.bankSelect — select a specific bank number (0..127) directly.
    // The value maps onto the bank number, selecting the setlist whose `bank`
    // field matches. A MIDI CC (0..127) therefore picks the bank directly.
    {
        ParamRegistryEntry e;
        e.address = "setlist.bankSelect";
        e.label = "Select Bank";
        e.unit = "";
        e.minValue = 0.0;
        e.maxValue = 127.0;
        e.isStepped = true;
        e.get = [this]() -> double {
            return mGetSetlistBankNumber ? static_cast<double>(mGetSetlistBankNumber()) : 0.0;
        };
        e.apply = [this](double v, bool) {
            if (mSelectSetlistBank)
            {
                mSelectSetlistBank(static_cast<int>(std::round(v)));
            }
        };
        mRegistry.Register(e);
    }

    // scene.select1..N — one trigger parameter per scene of the *active* preset.
    // Selection is handled entirely in the controller so a footswitch still
    // switches scenes when the plugin editor is closed and no UI exists.
    for (int i = 1; i <= kSceneSelectSlots; ++i)
    {
        ParamRegistryEntry e;
        e.address = "scene.select" + std::to_string(i);
        e.label = "Scene " + std::to_string(i);
        e.unit = "";
        e.minValue = 0.0;
        e.maxValue = 1.0;
        e.isTrigger = true;
        const int sceneOffset = i; // captured by value
        e.get = [this, sceneOffset]() -> double {
            if (!mGetActiveSceneIndex)
            {
                return 0.0;
            }

            return mGetActiveSceneIndex() == (sceneOffset - 1) ? 1.0 : 0.0;
        };
        e.apply = [this, sceneOffset](double, bool) {
            if (mSelectSceneByIndex)
            {
                mSelectSceneByIndex(sceneOffset - 1);
            }
        };
        mRegistry.Register(e);
    }
}

// ── Serialization ─────────────────────────────────────────────────────────

nlohmann::json AutomationSlotTable::SaveToJson() const
{
    nlohmann::json j = nlohmann::json::object();
    j["schemaVersion"] = 1;

    // Default slot overrides (midiMap, keyMaps, label)
    nlohmann::json overrides = nlohmann::json::object();

    for (const auto& slot : mSlots)
    {
        if (!slot.isDefault)
        {
            continue;
        }

        if (!slot.midiMap && slot.keyMaps.empty() && slot.label.empty())
        {
            continue;
        }

        nlohmann::json o = nlohmann::json::object();

        // Only store label if it differs from the default
        for (const auto& def : kDefaultSlots)
        {
            if (def.slotId == slot.slotId && def.label != slot.label)
            {
                o["label"] = slot.label;
                break;
            }
        }

        if (slot.midiMap)
        {
            o["midiMap"] = MidiMapToJson(*slot.midiMap);
        }

        if (!slot.keyMaps.empty())
        {
            nlohmann::json km = nlohmann::json::array();

            for (const auto& k : slot.keyMaps)
            {
                km.push_back({{"key", k.key}, {"mode", static_cast<int>(k.mode)}, {"value", k.value}});
            }

            o["keyMap"] = std::move(km);
        }

        overrides[slot.slotId] = std::move(o);
    }

    j["defaultSlotOverrides"] = std::move(overrides);

    // Custom slots
    nlohmann::json customs = nlohmann::json::array();

    for (const auto& slot : mSlots)
    {
        if (slot.isDefault || !slot.presetId.empty())
        {
            continue;
        }

        nlohmann::json s = nlohmann::json::object();
        s["slotId"] = slot.slotId;
        s["label"] = slot.label;
        s["address"] = slot.address;

        if (!slot.nodeSelector.empty())
        {
            s["nodeSelector"] = slot.nodeSelector;
        }

        if (slot.midiMap)
        {
            s["midiMap"] = MidiMapToJson(*slot.midiMap);
        }

        if (!slot.keyMaps.empty())
        {
            nlohmann::json km = nlohmann::json::array();

            for (const auto& k : slot.keyMaps)
            {
                km.push_back({{"key", k.key}, {"mode", static_cast<int>(k.mode)}, {"value", k.value}});
            }

            s["keyMap"] = std::move(km);
        }

        customs.push_back(std::move(s));
    }

    j["customSlots"] = std::move(customs);

    // Per-preset MIDI mappings. Written only when there are some, so a document from before
    // they existed saves back unchanged.
    nlohmann::json presetSlots = nlohmann::json::array();

    for (const auto& slot : mSlots)
    {
        if (slot.presetId.empty())
        {
            continue;
        }

        nlohmann::json s = nlohmann::json::object();
        s["slotId"] = slot.slotId;
        s["presetId"] = slot.presetId;
        s["label"] = slot.label;
        s["address"] = slot.address;

        if (slot.midiMap)
        {
            s["midiMap"] = MidiMapToJson(*slot.midiMap);
        }

        presetSlots.push_back(std::move(s));
    }

    if (!presetSlots.empty())
    {
        j["presetSlots"] = std::move(presetSlots);
    }

    return j;
}

void AutomationSlotTable::LoadFromJson(const nlohmann::json& j)
{
    InitializeDefaultSlots();

    // Load default slot overrides
    if (j.contains("defaultSlotOverrides") && j["defaultSlotOverrides"].is_object())
    {
        for (auto it = j["defaultSlotOverrides"].begin(); it != j["defaultSlotOverrides"].end(); ++it)
        {
            const auto& slotId = it.key();
            const auto& o = it.value();
            auto* slot = FindSlot(slotId);

            if (!slot || !slot->isDefault)
            {
                continue;
            }

            if (o.contains("label") && o["label"].is_string())
            {
                slot->label = o["label"].get<std::string>();
            }

            if (o.contains("midiMap") && o["midiMap"].is_object())
            {
                slot->midiMap = MidiMapFromJson(o["midiMap"]);
            }

            if (o.contains("keyMap") && o["keyMap"].is_array())
            {
                slot->keyMaps.clear();

                for (const auto& k : o["keyMap"])
                {
                    KeyboardMap km;
                    km.key = k.value("key", "");
                    km.mode = static_cast<KeyboardMap::Mode>(k.value("mode", 0));
                    km.value = k.value("value", 0.0f);
                    slot->keyMaps.push_back(std::move(km));
                }
            }
        }
    }

    // Load custom slots
    if (j.contains("customSlots") && j["customSlots"].is_array())
    {
        for (const auto& cs : j["customSlots"])
        {
            AutomationSlot slot;
            slot.slotId = cs.value("slotId", "");
            slot.label = cs.value("label", "");
            slot.address = cs.value("address", "");
            slot.nodeSelector = cs.value("nodeSelector", "");
            slot.isDefault = false;

            if (cs.contains("midiMap") && cs["midiMap"].is_object())
            {
                slot.midiMap = MidiMapFromJson(cs["midiMap"]);
            }

            if (cs.contains("keyMap") && cs["keyMap"].is_array())
            {
                for (const auto& k : cs["keyMap"])
                {
                    KeyboardMap km;
                    km.key = k.value("key", "");
                    km.mode = static_cast<KeyboardMap::Mode>(k.value("mode", 0));
                    km.value = k.value("value", 0.0f);
                    slot.keyMaps.push_back(std::move(km));
                }
            }

            if (!slot.slotId.empty())
            {
                mSlots.push_back(std::move(slot));
            }
        }
    }

    // Per-preset MIDI mappings
    if (j.contains("presetSlots") && j["presetSlots"].is_array())
    {
        for (const auto& ps : j["presetSlots"])
        {
            if (!ps.is_object())
            {
                continue;
            }

            AutomationSlot slot;
            slot.slotId = ps.value("slotId", "");
            slot.presetId = ps.value("presetId", "");
            slot.label = ps.value("label", "");
            slot.address = ps.value("address", "");

            if (ps.contains("midiMap") && ps["midiMap"].is_object())
            {
                slot.midiMap = MidiMapFromJson(ps["midiMap"]);
            }

            if (!slot.slotId.empty() && !slot.presetId.empty() && !FindSlot(slot.slotId))
            {
                mSlots.push_back(std::move(slot));
            }
        }
    }
}

// ── Host state: slot values ──────────────────────────────────────────────

nlohmann::json AutomationSlotTable::SaveValuesToJson() const
{
    // A json object is an ordered map, so the same values always dump to the same bytes.
    // Hosts, and clap-validator's state tests, compare saved states byte for byte.
    nlohmann::json values = nlohmann::json::object();

    for (const auto& slot : mSlots)
    {
        values[slot.slotId] = slot.value.load();
    }

    return values;
}

void AutomationSlotTable::LoadValuesFromJson(const nlohmann::json& json)
{
    for (auto& slot : mSlots)
    {
        float value = 0.0f;

        if (const auto it = json.find(slot.slotId); it != json.end() && it->is_number())
        {
            const auto stored = it->get<double>();

            // std::clamp passes a NaN straight through, so a non-finite value is rejected
            // before clamping rather than after.
            if (IsFinite(stored))
            {
                value = static_cast<float>(std::clamp(stored, 0.0, 1.0));
            }
        }

        slot.value.store(value);
        // As if this value had been applied: a host re-sending it is then not a rising
        // edge, and does not fire the trigger.
        slot.lastNormalized.store(value);
        slot.pendingApply.store(false);
    }
}

// ── Slot queries ─────────────────────────────────────────────────────────

AutomationSlot* AutomationSlotTable::FindSlot(const std::string& slotId)
{
    for (auto& s : mSlots)
    {
        if (s.slotId == slotId)
        {
            return &s;
        }
    }

    return nullptr;
}

const AutomationSlot* AutomationSlotTable::FindSlot(const std::string& slotId) const
{
    for (const auto& s : mSlots)
    {
        if (s.slotId == slotId)
        {
            return &s;
        }
    }

    return nullptr;
}

std::vector<std::string> AutomationSlotTable::GetSlotIds() const
{
    std::vector<std::string> ids;
    ids.reserve(mSlots.size());

    for (const auto& s : mSlots)
    {
        if (s.presetId.empty())
        {
            ids.push_back(s.slotId);
        }
    }

    return ids;
}

nlohmann::json AutomationSlotTable::GetSlotsJson() const
{
    nlohmann::json slots = nlohmann::json::array();

    for (const auto& s : mSlots)
    {
        nlohmann::json sj = nlohmann::json::object();
        sj["slotId"] = s.slotId;
        sj["label"] = s.label;
        sj["address"] = s.address;

        if (!s.nodeSelector.empty())
        {
            sj["nodeSelector"] = s.nodeSelector;
        }

        sj["isDefault"] = s.isDefault;

        if (!s.presetId.empty())
        {
            sj["presetId"] = s.presetId;
        }

        sj["value"] = s.value.load();

        if (s.midiMap)
        {
            sj["midiMap"] = MidiMapToJson(*s.midiMap);
        }

        if (!s.keyMaps.empty())
        {
            nlohmann::json km = nlohmann::json::array();

            for (const auto& k : s.keyMaps)
            {
                km.push_back({{"key", k.key}, {"mode", static_cast<int>(k.mode)}, {"value", k.value}});
            }

            sj["keyMap"] = std::move(km);
        }

        slots.push_back(std::move(sj));
    }

    return slots;
}

std::vector<ParamRegistryInfo> AutomationSlotTable::GetRegistryInfo() const
{
    return mRegistry.GetAllInfo();
}

// ── Slot management ──────────────────────────────────────────────────────

bool AutomationSlotTable::SetCustomSlot(const std::string& slotId, const std::optional<std::string>& label,
                                        const std::optional<std::string>& address,
                                        const std::optional<std::string>& nodeSelector,
                                        const std::optional<MidiControlMap>& midiMap,
                                        const std::optional<std::vector<KeyboardMap>>& keyMaps)
{
    auto* slot = FindSlot(slotId);

    if (slot && (slot->isDefault || !slot->presetId.empty()))
    {
        return false; // Defaults keep their address; per-preset slots go through SetPresetSlot
    }

    if (!slot)
    {
        // Check max custom slots
        int customCount = 0;

        for (const auto& s : mSlots)
        {
            if (!s.isDefault && s.presetId.empty())
            {
                ++customCount;
            }
        }

        if (customCount >= kMaxCustomSlots)
        {
            return false;
        }

        AutomationSlot newSlot;
        newSlot.slotId = slotId;
        newSlot.isDefault = false;

        if (label)
        {
            newSlot.label = *label;
        }

        if (address)
        {
            newSlot.address = *address;
        }

        if (nodeSelector)
        {
            newSlot.nodeSelector = *nodeSelector;
        }

        if (midiMap)
        {
            newSlot.midiMap = *midiMap;
        }

        if (keyMaps)
        {
            newSlot.keyMaps = *keyMaps;
        }

        mSlots.push_back(std::move(newSlot));
        return true;
    }

    if (label)
    {
        slot->label = *label;
    }

    if (address)
    {
        slot->address = *address;
    }

    if (nodeSelector)
    {
        slot->nodeSelector = *nodeSelector;
    }

    if (midiMap)
    {
        slot->midiMap = *midiMap;
    }

    if (keyMaps)
    {
        slot->keyMaps = *keyMaps;
    }

    return true;
}

bool AutomationSlotTable::SetDefaultSlotOverrides(const std::string& slotId, const std::optional<std::string>& label,
                                                  const std::optional<MidiControlMap>& midiMap,
                                                  const std::optional<std::vector<KeyboardMap>>& keyMaps)
{
    auto* slot = FindSlot(slotId);

    if (!slot || !slot->isDefault)
    {
        return false;
    }

    if (label)
    {
        slot->label = *label;
    }

    if (midiMap)
    {
        slot->midiMap = *midiMap;
    }

    if (keyMaps)
    {
        slot->keyMaps = *keyMaps;
    }

    return true;
}

bool AutomationSlotTable::RemoveCustomSlot(const std::string& slotId)
{
    for (auto it = mSlots.begin(); it != mSlots.end(); ++it)
    {
        if (it->slotId == slotId && !it->isDefault)
        {
            mSlots.erase(it);
            return true;
        }
    }

    return false;
}

bool AutomationSlotTable::SetPresetSlot(const std::string& slotId, const std::string& presetId,
                                        const std::optional<std::string>& label,
                                        const std::optional<std::string>& address,
                                        const std::optional<MidiControlMap>& midiMap)
{
    auto* slot = FindSlot(slotId);

    if (!slot)
    {
        if (slotId.empty() || presetId.empty() || CountPresetSlots(presetId) >= kMaxPresetSlotsPerPreset)
        {
            return false;
        }

        AutomationSlot newSlot;
        newSlot.slotId = slotId;
        newSlot.presetId = presetId;
        mSlots.push_back(std::move(newSlot));
        slot = &mSlots.back();
    }
    else if (slot->presetId.empty())
    {
        return false; // A default or custom slot does not become a per-preset one
    }

    if (label)
    {
        slot->label = *label;
    }

    if (address)
    {
        slot->address = *address;
    }

    if (midiMap)
    {
        slot->midiMap = *midiMap;
    }

    return true;
}

int AutomationSlotTable::RemovePresetSlots(const std::string& presetId)
{
    if (presetId.empty())
    {
        return 0;
    }

    return static_cast<int>(
        std::erase_if(mSlots, [&presetId](const AutomationSlot& slot) { return slot.presetId == presetId; }));
}

int AutomationSlotTable::CountPresetSlots(const std::string& presetId) const
{
    return static_cast<int>(std::count_if(mSlots.begin(), mSlots.end(), [&presetId](const AutomationSlot& slot) {
        return !presetId.empty() && slot.presetId == presetId;
    }));
}

// ── Apply path ────────────────────────────────────────────────────────────

bool AutomationSlotTable::ApplyAutomationLocked(const std::string& slotId, float normalized, AutomationSource src)
{
    auto* slot = FindSlot(slotId);

    if (!slot)
    {
        return false;
    }

    slot->SetValue(normalized, src);
    return ApplySlotLocked(*slot);
}

bool AutomationSlotTable::ApplySlotLocked(AutomationSlot& slot)
{
    if (slot.address.empty())
    {
        return false; // Unmapped slot
    }

    const auto* entry = mRegistry.Find(slot.address);

    // Node.* address — lazy resolution via the single prefix handler
    if (ParamRegistry::IsNodeAddress(slot.address))
    {
        std::string effectType, paramId;

        if (!ParamRegistry::ParseNodeAddress(slot.address, effectType, paramId))
        {
            return false;
        }

        // Resolve alias to canonical UUID via EffectRegistry
        if (mEffectRegistry)
        {
            effectType = mEffectRegistry->Resolve(effectType);
        }

        if (mMixer)
        {
            if (IsBypassNodeAddress(slot.address))
            {
                const bool enabled = (paramId == "enabled") ? (slot.value.load() >= 0.5f) : (slot.value.load() < 0.5f);
                const bool ok = mMixer->SetNodeEnabledByType(effectType, enabled);

                if (ok && mOnNodeBypassApplied)
                {
                    mOnNodeBypassApplied(effectType, enabled);
                }

                return ok;
            }

            // Every source hands a slot a 0..1 value, but an effect takes its parameters in
            // native units. A parameter the effect does not declare has no range to map onto,
            // so it keeps the 0..1 value.
            double native = static_cast<double>(slot.value.load());

            if (const auto* def = mEffectRegistry ? mEffectRegistry->FindParameter(effectType, paramId) : nullptr)
            {
                native = DenormalizeNodeParam(*def, native);
            }

            const bool ok = mMixer->SetNodeParamByType(effectType, paramId, native);

            if (ok && mOnNodeParamApplied)
            {
                mOnNodeParamApplied(effectType, paramId, native);
            }

            return ok;
        }

        return false;
    }

    // Static registry entry (global.* / setlist.*)
    if (!entry)
    {
        return false;
    }

    const float normalized = slot.value.load();

    const double maxVal = entry->maxValue;

    const double native = entry->isStepped ? std::round(entry->minValue + normalized * (maxVal - entry->minValue))
                                           : entry->minValue + normalized * (maxVal - entry->minValue);

    // Trigger edge detection
    if (entry->isTrigger)
    {
        const float prev = slot.lastNormalized.load();
        slot.lastNormalized.store(normalized);
        const bool fire = (prev < 0.5f) && (normalized >= 0.5f);

        if (!fire)
        {
            return false;
        }

        // Reset both value and lastNormalized so the trigger can fire again
        // on the next rising edge. Without this, a sustained/high MIDI value
        // or repeated Test button presses would prevent retriggering.
        slot.value.store(0.0f);
        slot.lastNormalized.store(0.0f);
    }
    else
    {
        slot.lastNormalized.store(normalized);
    }

    if (entry->apply)
    {
        entry->apply(native, true);
        return true;
    }

    return false;
}

std::string AutomationSlotTable::ResolveNodeAddress(const std::string& address, const std::string& nodeSelector) const
{
    if (!mMixer)
    {
        return {};
    }

    std::string effectType, paramId;

    if (!ParamRegistry::ParseNodeAddress(address, effectType, paramId))
    {
        return {};
    }

    if (mEffectRegistry)
    {
        effectType = mEffectRegistry->Resolve(effectType);
    }

    const auto found = mMixer->FindFirstEnabledNodeOfType(effectType);

    if (!found)
    {
        return {};
    }

    // v1: nodeSelector is ignored — always returns first. Future: interpret selector.
    return found->second;
}

// ── MIDI handling ─────────────────────────────────────────────────────────

void AutomationSlotTable::HandleMidi(const MidiEvent& ev)
{
    const int channel = ev.status & 0x0F;
    const int statusType = (ev.status >> 4) & 0x0F;

    MidiControlMap::EventType eventType;
    int controller = 0;
    int dataValue = 0;

    switch (statusType)
    {
    case 0x0B: // CC
        eventType = MidiControlMap::EventType::CC;
        controller = ev.data1;
        dataValue = ev.data2;
        break;
    case 0x0C: // Program Change
        eventType = MidiControlMap::EventType::ProgramChange;
        controller = ev.data1;
        dataValue = ev.data2; // typically 0 for PC
        break;
    case 0x09: // Note On
        // MIDI spec: Note On with velocity 0 is equivalent to Note Off.
        eventType = (ev.data2 == 0) ? MidiControlMap::EventType::NoteOff : MidiControlMap::EventType::NoteOn;
        controller = ev.data1;
        dataValue = ev.data2; // velocity
        break;
    case 0x08: // Note Off
        eventType = MidiControlMap::EventType::NoteOff;
        controller = ev.data1;
        dataValue = ev.data2;
        break;
    case 0x0E: // Pitch Bend
        eventType = MidiControlMap::EventType::PitchBend;
        controller = 0;
        dataValue = (ev.data2 << 7) | ev.data1;
        break;
    default:
        return;
    }

    // MIDI learn: capture the next incoming event for the armed slot
    if (mMidiLearnSlotId.has_value() && !mMidiLearnCapture.has_value())
    {
        // Only capture CC, ProgramChange, NoteOn, and PitchBend for learning
        // (NoteOff is not useful as a controller)
        if (eventType == MidiControlMap::EventType::NoteOff)
        {
            return;
        }

        MidiControlMap captured;
        captured.eventType = eventType;
        captured.channel = channel;
        captured.controller = controller;
        captured.mode = MidiControlMap::Mode::Absolute;
        captured.sensitivity = 0.1f;
        captured.pickupRange = 0.1f;
        mMidiLearnCapture = captured;
        return;
    }

    // A mapping made for the active preset takes its MIDI control over from any global mapping
    // on the same control, so one pedal can do a different job in each preset.
    const auto isLivePresetSlot = [this](const AutomationSlot& slot) {
        return !slot.presetId.empty() && slot.presetId == mActivePresetId;
    };
    const bool presetOwnsControl = std::any_of(mSlots.begin(), mSlots.end(), [&](const AutomationSlot& slot) {
        return isLivePresetSlot(slot) && slot.midiMap && ListensTo(*slot.midiMap, eventType, channel, controller);
    });

    // Match against all slot MIDI maps
    for (auto& slot : mSlots)
    {
        if (!slot.midiMap.has_value())
        {
            continue;
        }

        const auto& mm = slot.midiMap.value();

        if (slot.presetId.empty() ? presetOwnsControl && ListensTo(mm, eventType, channel, controller)
                                  : !isLivePresetSlot(slot))
        {
            continue;
        }

        if (mm.channel != -1 && mm.channel != channel)
        {
            continue;
        }

        if (mm.controller != controller)
        {
            continue;
        }

        const bool isNoteOnToggleRelease = (mm.mode == MidiControlMap::Mode::Toggle) &&
                                           (mm.eventType == MidiControlMap::EventType::NoteOn) &&
                                           (eventType == MidiControlMap::EventType::NoteOff);

        if (isNoteOnToggleRelease)
        {
            // NoteOn mappings receive releases as NoteOff/NoteOn(vel=0):
            // reset the gate so the next key press can fire, but do not toggle on release.
            slot.lastToggleGate.store(false);
            continue;
        }

        if (mm.eventType != eventType)
        {
            continue;
        }

        float normalized = 0.0f;
        const bool isBypassAddress = IsBypassNodeAddress(slot.address);

        switch (mm.mode)
        {
        case MidiControlMap::Mode::Absolute:

            if (isBypassAddress && (eventType == MidiControlMap::EventType::NoteOn ||
                                    eventType == MidiControlMap::EventType::ProgramChange))
            {
                // Discrete events commonly carry only "on" values. For bypass
                // targets, flip state so both directions work from a single press.
                normalized = (slot.value.load() < 0.5f) ? 1.0f : 0.0f;
            }
            else if (eventType == MidiControlMap::EventType::PitchBend)
            {
                normalized = static_cast<float>(dataValue) / 16383.0f;
            }
            else if (eventType == MidiControlMap::EventType::ProgramChange)
            {
                // Program Change carries the program number in data1 (controller).
                normalized = static_cast<float>(controller) / 127.0f;
            }
            else
            {
                normalized = static_cast<float>(dataValue) / 127.0f;
            }

            break;
        case MidiControlMap::Mode::Relative: {
            const float current = slot.value.load();
            const float delta = (static_cast<float>(dataValue) - 64.0f) / 64.0f * mm.sensitivity;
            normalized = std::clamp(current + delta, 0.0f, 1.0f);
            break;
        }
        case MidiControlMap::Mode::Toggle: {
            bool gateHigh = false;

            if (eventType == MidiControlMap::EventType::ProgramChange)
            {
                // Program change events are already discrete, so every match toggles once.
                gateHigh = true;
            }
            else if (eventType == MidiControlMap::EventType::NoteOff)
            {
                // Releasing a key must not fire a second toggle.
                slot.lastToggleGate.store(false);
                continue;
            }
            else if (eventType == MidiControlMap::EventType::NoteOn)
            {
                // For NoteOn mappings, treat each NoteOn press as a discrete toggle
                // so re-activation works even when release events are not delivered.
                gateHigh = dataValue > 0;

                if (!gateHigh)
                {
                    slot.lastToggleGate.store(false);
                    continue;
                }

                slot.lastToggleGate.store(true);
                normalized = (slot.value.load() < 0.5f) ? 1.0f : 0.0f;
                break;
            }
            else if (eventType == MidiControlMap::EventType::PitchBend)
            {
                gateHigh = dataValue >= 8192;
            }
            else
            {
                gateHigh = dataValue >= 64;
            }

            const bool fire = (eventType == MidiControlMap::EventType::ProgramChange)
                                  ? true
                                  : (!slot.lastToggleGate.load() && gateHigh);
            slot.lastToggleGate.store(gateHigh);

            if (!fire)
            {
                continue;
            }

            normalized = (slot.value.load() < 0.5f) ? 1.0f : 0.0f;
            break;
        }
        case MidiControlMap::Mode::Pickup: {
            const float target = static_cast<float>(dataValue) / 127.0f;

            if (std::abs(slot.value.load() - target) > mm.pickupRange)
            {
                continue; // Too far away, don't jump
            }

            normalized = target;
            break;
        }
        }

        slot.SetValue(normalized, AutomationSource::MIDI);
        ApplySlotLocked(slot);
    }
}

std::optional<MidiControlMap> AutomationSlotTable::PollMidiLearnCapture()
{
    if (!mMidiLearnCapture.has_value())
    {
        return std::nullopt;
    }

    auto result = std::move(mMidiLearnCapture);
    mMidiLearnCapture.reset();
    mMidiLearnSlotId.reset();
    return result;
}
} // namespace guitarfx

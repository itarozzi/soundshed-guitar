/**
 * UiClientAutomationHandlers.cpp - The automation slots and registry, MIDI learn's capture and
 * the MIDI Log (Topic::Automation).
 */

#include "uiclient/Automation.h"
#include "uiclient/UiClient.h"

#include <chrono>
#include <cstdio>
#include <ctime>

namespace guitarfx::uiclient
{
namespace
{
/// The time of day now, "HH:MM:SS.mmm", as the web UI's MIDI Log stamps an event.
std::string WallClockText()
{
    const auto now = std::chrono::system_clock::now();
    const auto seconds = std::chrono::system_clock::to_time_t(now);
    const auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count() % 1000;
    std::tm local{};
#ifdef _WIN32
    localtime_s(&local, &seconds);
#else
    localtime_r(&seconds, &local);
#endif
    char text[16];
    std::snprintf(text, sizeof(text), "%02d:%02d:%02d.%03d", local.tm_hour, local.tm_min, local.tm_sec,
                  static_cast<int>(millis));
    return text;
}
} // namespace

void UiClient::RegisterAutomationHandlers()
{
    // The slot table and the addresses a slot can drive: an answer to getAutomation, and after
    // every slot edit, learn and key learn.
    On("automation", [this](const nlohmann::json& m) {
        auto& automation = mState.automation;
        automation.loaded = true;
        automation.slots = ParseAutomationSlots(m.value("slots", nlohmann::json::array()));

        if (m.contains("registry") && m["registry"].is_array())
        {
            automation.registry = ParseAutomationRegistry(m["registry"]);
        }

        automation.maxCustomSlots = m.value("maxCustomSlots", automation.maxCustomSlots);
        automation.maxPresetSlots = m.value("maxPresetSlots", automation.maxPresetSlots);

        if (m.contains("keyLearnSlotId") && m["keyLearnSlotId"].is_string())
        {
            automation.keyLearnSlotId = m["keyLearnSlotId"].get<std::string>();
        }

        // A learn armed on a slot since removed has nothing left to listen for.
        if (!automation.midiLearnSlotId.empty() && automation.FindSlot(automation.midiLearnSlotId) == nullptr)
        {
            automation.midiLearnSlotId.clear();
        }

        Notify(Topic::Automation);
    });

    On("midiLearnCapture", [this](const nlohmann::json& m) {
        if (!m.contains("slotId") || !m["slotId"].is_string())
        {
            return;
        }

        auto& automation = mState.automation;
        MidiLearnCaptureInfo capture;
        capture.slotId = m["slotId"].get<std::string>();
        capture.eventType = m.value("eventType", 0);
        capture.channel = m.value("channel", 0);
        capture.controller = m.value("controller", 0);
        automation.capture = capture;

        if (automation.midiLearnSlotId == capture.slotId)
        {
            automation.midiLearnSlotId.clear();
        }

        Notify(Topic::Automation);
    });

    On("midiLog", [this](const nlohmann::json& m) {
        auto& automation = mState.automation;

        // One that was on its way when the log was turned off is dropped, as the web UI drops it.
        if (!automation.midiLogEnabled)
        {
            return;
        }

        MidiLogEntry entry;
        entry.receivedAt = mNowSeconds;
        entry.clock = WallClockText();
        entry.type = m.value("midiType", std::string("Unknown"));
        entry.channel = m.value("channel", 0);
        entry.data1 = m.value("data1", 0);
        entry.data2 = m.value("data2", 0);
        automation.midiLog.push_back(std::move(entry));

        if (automation.midiLog.size() > AutomationState::kMidiLogLimit)
        {
            automation.midiLog.erase(
                automation.midiLog.begin(),
                automation.midiLog.begin() +
                    static_cast<std::ptrdiff_t>(automation.midiLog.size() - AutomationState::kMidiLogLimit));
        }

        Notify(Topic::Automation);
    });
}
} // namespace guitarfx::uiclient

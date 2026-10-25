// LooperService: the looper.* settings, the UI's messages, and the state the UI is sent.
// See LooperService.h; the message contract is in docs/user-interface.md.

#include "controller/LooperService.h"

#include "IPluginHost.h"
#include "MessageHandlerRegistry.h"
#include "controller/MetronomeService.h"

#include <algorithm>
#include <cmath>

namespace guitarfx
{
using looper::LooperAction;
using looper::LooperCommand;
using looper::LooperState;
using looper::SlotStatus;

namespace
{
constexpr const char* kSettingsKey = "looper";

const char* StateName(LooperState state)
{
    switch (state)
    {
    case LooperState::Empty:
        return "empty";
    case LooperState::CountIn:
        return "countIn";
    case LooperState::Armed:
        return "armed";
    case LooperState::Recording:
        return "recording";
    case LooperState::Playing:
        return "playing";
    case LooperState::OverdubArmed:
        return "overdubArmed";
    case LooperState::Overdubbing:
        return "overdubbing";
    case LooperState::Stopped:
        return "stopped";
    }

    return "empty";
}

bool ParseAction(const std::string& command, LooperAction& action)
{
    static const std::pair<const char*, LooperAction> kActions[] = {
        {"recPlayDub", LooperAction::RecPlayDub}, {"record", LooperAction::Record}, {"play", LooperAction::Play},
        {"overdub", LooperAction::Overdub},       {"stop", LooperAction::Stop},     {"undo", LooperAction::Undo},
        {"redo", LooperAction::Redo},             {"clear", LooperAction::Clear},
    };

    for (const auto& [name, value] : kActions)
    {
        if (command == name)
        {
            action = value;
            return true;
        }
    }

    return false;
}

template <typename T> void ReadNumber(const nlohmann::json& source, const char* key, T& target)
{
    const auto it = source.find(key);

    if (it != source.end() && it->is_number())
    {
        target = it->get<T>();
    }
}

void ReadBool(const nlohmann::json& source, const char* key, bool& target)
{
    const auto it = source.find(key);

    if (it != source.end() && it->is_boolean())
    {
        target = it->get<bool>();
    }
}

/// Reads `source` (app settings or a setLooper payload) into `settings`, normalising every field.
void ReadSettings(const nlohmann::json& source, LooperService::Settings& settings)
{
    if (!source.is_object())
    {
        return;
    }

    ReadBool(source, "enabled", settings.enabled);
    ReadNumber(source, "maxSeconds", settings.maxSeconds);
    ReadNumber(source, "memoryBudgetMb", settings.memoryBudgetMb);
    ReadNumber(source, "level", settings.level);
    ReadNumber(source, "bars", settings.bars);
    ReadNumber(source, "countInBars", settings.countInBars);
    ReadBool(source, "clickWhileRecording", settings.clickWhileRecording);
    ReadNumber(source, "armThresholdDb", settings.armThresholdDb);
    ReadNumber(source, "alignOffsetMs", settings.alignOffsetMs);

    if (const auto it = source.find("start"); it != source.end() && it->is_string())
    {
        settings.startArmed = it->get<std::string>() == "armed";
    }

    if (const auto it = source.find("length"); it != source.end() && it->is_string())
    {
        settings.lengthBars = it->get<std::string>() == "bars";
    }

    if (const auto it = source.find("afterRecord"); it != source.end() && it->is_string())
    {
        settings.afterRecordOverdub = it->get<std::string>() == "overdub";
    }

    // 30, 60, 120 or 300 seconds: the nearest.
    static constexpr int kMaxSeconds[] = {30, 60, 120, 300};
    int nearest = 60;

    for (const int choice : kMaxSeconds)
    {
        if (std::abs(choice - settings.maxSeconds) < std::abs(nearest - settings.maxSeconds))
        {
            nearest = choice;
        }
    }

    settings.maxSeconds = nearest;
    settings.memoryBudgetMb = std::clamp(settings.memoryBudgetMb, 64, 4096);
    settings.level = std::isfinite(settings.level) ? std::clamp(settings.level, 0.0, 2.0) : 1.0;
    settings.bars = std::clamp(settings.bars, 1, 64);
    settings.countInBars = std::clamp(settings.countInBars, 0, 2);
    settings.armThresholdDb =
        std::isfinite(settings.armThresholdDb) ? std::clamp(settings.armThresholdDb, -80.0, -20.0) : -50.0;
    settings.alignOffsetMs =
        std::isfinite(settings.alignOffsetMs) ? std::clamp(settings.alignOffsetMs, -50.0, 50.0) : 0.0;
}

nlohmann::json SettingsToJson(const LooperService::Settings& settings)
{
    return {{"enabled", settings.enabled},
            {"maxSeconds", settings.maxSeconds},
            {"memoryBudgetMb", settings.memoryBudgetMb},
            {"level", settings.level},
            {"start", settings.startArmed ? "armed" : "press"},
            {"length", settings.lengthBars ? "bars" : "free"},
            {"bars", settings.bars},
            {"countInBars", settings.countInBars},
            {"clickWhileRecording", settings.clickWhileRecording},
            {"afterRecord", settings.afterRecordOverdub ? "overdub" : "play"},
            {"armThresholdDb", settings.armThresholdDb},
            {"alignOffsetMs", settings.alignOffsetMs}};
}
} // namespace

void LooperService::ApplySettingsFromAppSettings()
{
    Settings settings;

    if (mAppSettings.contains(kSettingsKey))
    {
        ReadSettings(mAppSettings[kSettingsKey], settings);
    }

    mSettings = settings;
    StoreSettings();
    ApplySettingsToEngine();
}

void LooperService::StoreSettings()
{
    mAppSettings[kSettingsKey] = SettingsToJson(mSettings);
}

void LooperService::ApplySettingsToEngine()
{
    auto& engine = mEngine.Settings();
    engine.startArmed.store(mSettings.startArmed);
    engine.lengthBars.store(mSettings.lengthBars);
    engine.bars.store(mSettings.bars);
    engine.countInBars.store(mSettings.countInBars);
    engine.countInEveryTake.store(false);
    engine.afterRecordOverdub.store(mSettings.afterRecordOverdub);
    engine.armThreshold.store(static_cast<float>(std::pow(10.0, mSettings.armThresholdDb / 20.0)));
    engine.level.store(static_cast<float>(mSettings.level));
    engine.playbackEnabled.store(true);
    mClickWhileRecording.store(mSettings.clickWhileRecording, std::memory_order_relaxed);
}

void LooperService::ApplySettingsRequest(const nlohmann::json& payload)
{
    Settings settings = mSettings;
    ReadSettings(payload, settings);
    mSettings = settings;
    StoreSettings();
    ApplySettingsToEngine();

    if (mSaveSettings)
    {
        mSaveSettings();
    }

    mLastState.clear(); // the next idle sends the state with the new settings
}

void LooperService::HandleCommand(const std::string& command)
{
    LooperAction action = LooperAction::RecPlayDub;

    if (!ParseAction(command, action))
    {
        mPendingNotice = "unknownCommand";
        return;
    }

    if (!Active())
    {
        mPendingNotice = "disabled";
        mLastState.clear();
        return;
    }

    // A Bars take started on a press counts in. With the metronome off, the count-in is the
    // guidance click riff capture uses, at the metronome's tempo and meter; the engine restarts its
    // bar on the press's sample, so it starts on beat 1. It stops when the take does (OnIdle).
    const bool startsTake = action == LooperAction::RecPlayDub || action == LooperAction::Record;
    const bool empty = !mHaveSnapshot || mSnapshot.state == LooperState::Empty;

    if (startsTake && empty && mSettings.lengthBars && !mSettings.startArmed && mSettings.countInBars > 0 &&
        !mMetronome.IsEnabled() && !mGuidanceForCountIn)
    {
        MetronomeService::GuidanceConfig config;
        config.tempoBpm = mMetronome.EffectiveTempoBpm();
        config.timeSigNum = mMetronome.TimeSigNum();
        config.timeSigDen = mMetronome.TimeSigDen();
        config.beatPattern = mMetronome.BeatPattern();
        std::lock_guard<std::mutex> lock(mDSPMutex);
        mMetronome.ActivateGuidance(config, true, false);
        mGuidanceForCountIn = true;
    }

    mEngine.Enqueue({LooperCommand::Type::Action, action, -1, nullptr});
}

void LooperService::HandleSetLayer(const nlohmann::json& payload)
{
    const int slot = FindSlot(payload.value("layerId", std::string{}));

    if (slot < 0)
    {
        return;
    }

    auto& mix = mEngine.Mix(slot);

    if (const auto it = payload.find("level"); it != payload.end() && it->is_number())
    {
        const double level = it->get<double>();
        mix.level.store(std::isfinite(level) ? static_cast<float>(std::clamp(level, 0.0, 2.0)) : 1.0f);
    }

    if (const auto it = payload.find("balance"); it != payload.end() && it->is_number())
    {
        const double balance = it->get<double>();
        mix.balance.store(std::isfinite(balance) ? static_cast<float>(std::clamp(balance, -1.0, 1.0)) : 0.0f);
    }

    if (const auto it = payload.find("muted"); it != payload.end() && it->is_boolean())
    {
        mix.muted.store(it->get<bool>());
    }

    if (const auto it = payload.find("solo"); it != payload.end() && it->is_boolean())
    {
        mix.solo.store(it->get<bool>());
    }

    if (const auto it = payload.find("title"); it != payload.end() && it->is_string())
    {
        const auto serial = mSnapshot.slots[static_cast<std::size_t>(slot)].serial;

        if (auto meta = mLayers.find(serial); meta != mLayers.end())
        {
            meta->second.title = it->get<std::string>().substr(0, 80);
        }
    }

    mLastLayers.clear();
}

void LooperService::HandleRemoveLayer(const std::string& layerId)
{
    const int slot = FindSlot(layerId);

    if (slot >= 0)
    {
        mEngine.Enqueue({LooperCommand::Type::RemoveLayer, LooperAction::RecPlayDub, slot, nullptr});
    }
}

nlohmann::json LooperService::BuildState() const
{
    const double rate = std::max(1.0, mSampleRate.load(std::memory_order_relaxed));
    const auto& clock = mMetronome.Clock();
    const double samplesPerBar = clock.samplesPerBar.load(std::memory_order_relaxed);
    const double samplesPerBeat = clock.samplesPerBeat.load(std::memory_order_relaxed);
    const auto& s = mSnapshot;

    nlohmann::json state;
    state["available"] = mHost.IsStandalone();
    state["enabled"] = Active();
    state["state"] = StateName(s.state);
    state["playing"] = s.playing;
    state["positionSec"] = static_cast<double>(s.playhead) / rate;
    state["lengthSec"] = static_cast<double>(s.length) / rate;
    state["canUndo"] = s.canUndo;
    state["canRedo"] = s.canRedo;
    state["layerCount"] = s.visibleLayers;
    state["memoryMb"] = static_cast<double>(HeldBytes()) / (1024.0 * 1024.0);
    state["tempoLocked"] = mTempoLocked;
    state["settings"] = SettingsToJson(mSettings);

    if (s.barsLoop && samplesPerBar > 1.0)
    {
        state["bars"] = static_cast<int>(std::lround(static_cast<double>(s.length) / samplesPerBar));
        state["barIndex"] = static_cast<int>(static_cast<double>(s.playhead) / samplesPerBar) + 1;
        state["tempoBpm"] = mMetronome.EffectiveTempoBpm();
    }

    if (s.state == LooperState::CountIn && samplesPerBeat > 1.0)
    {
        state["countInBeatsLeft"] =
            static_cast<int>(std::ceil(static_cast<double>(mEngine.CountInSamplesLeft()) / samplesPerBeat));
    }

    if (s.state == LooperState::Recording || s.state == LooperState::Overdubbing)
    {
        state["recordedSec"] = static_cast<double>(mEngine.LiveRecordedSamples()) / rate;
    }

    if (s.state == LooperState::Armed || s.state == LooperState::OverdubArmed)
    {
        state["armWaitingSec"] = std::round(static_cast<double>(mEngine.ArmWaitingSamples()) / rate * 10.0) / 10.0;
    }

    if (!mPendingNotice.empty())
    {
        state["notice"] = mPendingNotice;
    }

    return state;
}

nlohmann::json LooperService::BuildLayers() const
{
    const double rate = std::max(1.0, mSampleRate.load(std::memory_order_relaxed));
    std::vector<int> order;

    for (int s = 0; s < looper::kMaxLayers; ++s)
    {
        const auto status = mSnapshot.slots[static_cast<std::size_t>(s)].status;

        if (status == SlotStatus::Visible || status == SlotStatus::Recording)
        {
            order.push_back(s);
        }
    }

    // Newest first, as the layers panel lists them.
    std::sort(order.begin(), order.end(), [this](int a, int b) {
        return mSnapshot.slots[static_cast<std::size_t>(a)].serial > mSnapshot.slots[static_cast<std::size_t>(b)].serial;
    });

    nlohmann::json layers = nlohmann::json::array();

    for (const int s : order)
    {
        const auto& slot = mSnapshot.slots[static_cast<std::size_t>(s)];
        const auto& mix = mEngine.Mix(s);
        const auto meta = mLayers.find(slot.serial);
        const Tone tone = meta != mLayers.end() ? meta->second.tone : Tone{};
        layers.push_back(
            {{"id", LayerId(slot.serial)},
             {"title", meta != mLayers.end() ? meta->second.title : std::string{}},
             {"order", slot.serial},
             {"recording", slot.status == SlotStatus::Recording},
             {"firstLayer", slot.firstLayer},
             {"span",
              {{"startSec", static_cast<double>(slot.spanStart) / rate},
               {"lengthSec", static_cast<double>(slot.spanLength) / rate}}},
             {"level", mix.level.load()},
             {"balance", mix.balance.load()},
             {"muted", mix.muted.load()},
             {"solo", mix.solo.load()},
             {"diChannels", slot.diChannels},
             {"tone", {{"presetId", tone.presetId}, {"presetName", tone.presetName}, {"sceneId", tone.sceneId}}},
             {"toneChanged", meta != mLayers.end() && meta->second.toneChanged},
             {"wetSource", "live"}});
    }

    return layers;
}

void LooperService::RegisterMessageHandlers(MessageHandlerRegistry& registry)
{
    registry.Register("looperCommand",
                      [this](const nlohmann::json& payload) { HandleCommand(payload.value("command", std::string{})); });
    registry.Register("setLooper", [this](const nlohmann::json& payload) { ApplySettingsRequest(payload); });
    registry.Register("setLooperLayer", [this](const nlohmann::json& payload) { HandleSetLayer(payload); });
    registry.Register("removeLooperLayer", [this](const nlohmann::json& payload) {
        HandleRemoveLayer(payload.value("layerId", std::string{}));
    });
    registry.Register("getLooperState", [this](const nlohmann::json&) {
        // Everything again on the next idle tick, waveforms included.
        mLastState.clear();
        mLastLayers.clear();

        for (auto& [serial, meta] : mLayers)
        {
            meta.peaksSent = false;
        }
    });
}
} // namespace guitarfx

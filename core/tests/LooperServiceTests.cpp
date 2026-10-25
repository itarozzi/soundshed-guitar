// LooperService through the controller, as the UI drives it: settings, the buffers it hands the
// mixer's looper and takes back, the state and layer feeds, layer edits, the metronome link (the
// count-in click with the metronome off, the tempo lock), and switching it off.

#include "PluginController.h"
#include "dsp/effects/BuiltinEffects.h"

#include "MessageThreadTestHost.h"

#include <nlohmann/json.hpp>

#include <cmath>
#include <filesystem>
#include <iostream>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;
using namespace guitarfx;

namespace
{
constexpr double kSampleRate = 48000.0;
constexpr int kBlock = 128;

int gFailures = 0;

void Check(bool condition, const std::string& what)
{
    if (!condition)
    {
        std::cerr << "FAIL: " << what << std::endl;
        ++gFailures;
    }
}

/// A standalone host that keeps the looper messages it is sent.
class LooperHost : public test::PumpedTestHost
{
  public:
    using test::PumpedTestHost::PumpedTestHost;

    [[nodiscard]] bool IsStandalone() const override
    {
        return true;
    }

    void SendMessageToUI(const std::string& message) override
    {
        const auto parsed = nlohmann::json::parse(message, nullptr, false);

        if (!parsed.is_object())
        {
            return;
        }

        const std::lock_guard<std::mutex> lock(mMutex);
        const std::string type = parsed.value("type", std::string());

        if (type.rfind("looper", 0) == 0)
        {
            mLast[type] = parsed;
            ++mCounts[type];
        }

        if (type == "state" || type == "metronomeState" || parsed.contains("metronome"))
        {
            mLastBroadcast = parsed;
        }
    }

    nlohmann::json Last(const std::string& type)
    {
        const std::lock_guard<std::mutex> lock(mMutex);
        return mLast.count(type) ? mLast[type] : nlohmann::json{};
    }

    int Count(const std::string& type)
    {
        const std::lock_guard<std::mutex> lock(mMutex);
        return mCounts[type];
    }

  private:
    std::mutex mMutex;
    std::map<std::string, nlohmann::json> mLast;
    std::map<std::string, int> mCounts;
    nlohmann::json mLastBroadcast;
};

struct Session
{
    LooperHost& host;
    PluginController& controller;
    bool playing = false;
    std::size_t phase = 0;

    void Send(const nlohmann::json& message)
    {
        controller.HandleUIMessage(message.dump());
    }

    /// `seconds` of blocks, an idle tick every four.
    void Run(double seconds)
    {
        std::vector<float> l(kBlock);
        std::vector<float> r(kBlock);
        std::vector<float> ol(kBlock);
        std::vector<float> orr(kBlock);
        const int blocks = static_cast<int>(std::ceil(seconds * kSampleRate / kBlock));

        for (int b = 0; b < blocks; ++b)
        {
            for (int i = 0; i < kBlock; ++i, ++phase)
            {
                l[static_cast<std::size_t>(i)] =
                    playing ? static_cast<float>(0.3 * std::sin(static_cast<double>(phase) * 0.03)) : 0.0f;
                r[static_cast<std::size_t>(i)] = l[static_cast<std::size_t>(i)];
            }

            float* inputs[2] = {l.data(), r.data()};
            float* outputs[2] = {ol.data(), orr.data()};
            controller.ProcessAudio(inputs, outputs, kBlock);

            if (b % 4 == 3)
            {
                controller.OnIdle();
                host.Pump();
            }
        }

        controller.OnIdle();
        host.Pump();
    }

    nlohmann::json State()
    {
        return host.Last("looperState");
    }

    void Command(const std::string& command)
    {
        Send({{"type", "looperCommand"}, {"command", command}});
    }
};

void TestEnableAllocatesAndReports(Session& s)
{
    s.Run(0.05);
    Check(!s.State().value("enabled", true), "off by default");
    s.Send({{"type", "setLooper"}, {"enabled", true}, {"maxSeconds", 30}});
    s.Run(0.05);
    const auto state = s.State();
    Check(state.value("enabled", false), "setLooper enables it");
    Check(state.value("state", std::string()) == "empty", "empty");
    // A 30 s capture (two DI channels and the wet pair) and a spare the same size.
    Check(state.value("memoryMb", 0.0) > 40.0, "the capture buffer and a spare are held: " + state.dump());
    Check(state["settings"].value("maxSeconds", 0) == 30, "the state carries the settings");
}

void TestRecordAndStackLayers(Session& s)
{
    s.playing = true;
    s.Command("recPlayDub");
    s.Run(0.5);
    Check(s.State().value("state", std::string()) == "recording", "recording");
    s.Command("recPlayDub");
    s.Run(0.1);
    auto state = s.State();
    Check(state.value("state", std::string()) == "playing", "the second press plays it");
    Check(std::abs(state.value("lengthSec", 0.0) - 0.5) < 0.02, "half a second long: " + state.dump());

    // Held in Overdub over three wraps, with the service handing over a spare for each pass.
    s.Command("overdub");
    s.Run(1.6);
    s.Command("play");
    s.Run(0.6);
    state = s.State();
    const auto layers = s.host.Last("looperLayers")["layers"];
    Check(state.value("layerCount", 0) >= 4, "a layer per pass: " + state.dump());
    Check(layers.is_array() && layers.size() == static_cast<std::size_t>(state.value("layerCount", 0)),
          "looperLayers lists them");
    Check(layers.is_array() && !layers.empty() && layers[0].value("title", std::string()).rfind("Layer ", 0) == 0,
          "named in order");
    Check(s.host.Count("looperWaveforms") > 0, "waveforms are sent as layers finish");
    Check(state.value("canUndo", false), "undoable");
}

void TestLayerEditsAndUndo(Session& s)
{
    const auto layers = s.host.Last("looperLayers")["layers"];

    if (!layers.is_array() || layers.empty())
    {
        Check(false, "layers to edit");
        return;
    }

    const std::string newest = layers[0].value("id", std::string());
    s.Send({{"type", "setLooperLayer"}, {"layerId", newest}, {"muted", true}, {"balance", -0.5}, {"title", "Riff"}});
    s.Run(0.05);
    const auto edited = s.host.Last("looperLayers")["layers"][0];
    Check(edited.value("muted", false) && edited.value("balance", 0.0) < -0.4 &&
              edited.value("title", std::string()) == "Riff",
          "a layer's mix and title are set: " + edited.dump());

    const int before = s.State().value("layerCount", 0);
    s.Send({{"type", "removeLooperLayer"}, {"layerId", newest}});
    s.Run(0.05);
    Check(s.State().value("layerCount", 0) == before - 1, "a layer is removed");
    s.Command("undo");
    s.Run(0.05);
    Check(s.State().value("layerCount", 0) == before, "undo brings it back");
}

void TestClearAndSwitchOff(Session& s)
{
    s.playing = false;
    s.Command("clear");
    s.Run(0.1);
    Check(s.State().value("state", std::string()) == "empty", "cleared");
    s.Send({{"type", "setLooper"}, {"enabled", false}});
    s.Run(0.1);
    const auto state = s.State();
    Check(!state.value("enabled", true), "switched off");
    Check(state.value("memoryMb", 1.0) == 0.0, "and every buffer let go: " + state.dump());
}

void TestBarsCountInAndTempoLock(Session& s)
{
    s.Send({{"type", "setMetronome"}, {"enabled", false}, {"bpm", 240.0}});
    s.Send({{"type", "setLooper"}, {"enabled", true}, {"length", "bars"}, {"bars", 1}, {"countInBars", 1}});
    s.Run(0.05);
    s.playing = true;
    s.Command("recPlayDub");
    s.Run(0.3);
    auto state = s.State();
    Check(state.value("state", std::string()) == "countIn", "a Bars take counts in: " + state.dump());
    Check(state.value("tempoLocked", false), "and locks the tempo");
    // One bar at 240 BPM is a second; count in, then record it.
    s.Run(2.2);
    state = s.State();
    Check(state.value("state", std::string()) == "playing" && std::abs(state.value("lengthSec", 0.0) - 1.0) < 0.001,
          "one bar exactly, playing: " + state.dump());
    Check(state.value("bars", 0) == 1 && state.value("barIndex", 0) >= 1, "bar count and position");

    // The tempo cannot move under a Bars loop.
    s.Send({{"type", "setMetronome"}, {"bpm", 100.0}});
    s.Run(0.05);
    Check(std::abs(s.State().value("tempoBpm", 0.0) - 240.0) < 1e-9, "the tempo is locked");
    s.Command("clear");
    s.Run(0.2);
    Check(!s.State().value("tempoLocked", true), "clear unlocks it");
    s.Send({{"type", "setLooper"}, {"length", "free"}});
}

bool Run(const fs::path& sandbox)
{
    LooperHost host(sandbox, std::this_thread::get_id(), kSampleRate, kBlock);
    PluginController controller(host);
    controller.Initialize();
    controller.Prepare(kSampleRate, kBlock);
    host.Pump();
    Session session{host, controller};

    TestEnableAllocatesAndReports(session);
    TestRecordAndStackLayers(session);
    TestLayerEditsAndUndo(session);
    TestClearAndSwitchOff(session);
    TestBarsCountInAndTempoLock(session);
    return gFailures == 0;
}

/// The settings outlast the session.
void TestSettingsPersist(const fs::path& sandbox)
{
    LooperHost host(sandbox, std::this_thread::get_id(), kSampleRate, kBlock);
    PluginController controller(host);
    controller.Initialize();
    controller.Prepare(kSampleRate, kBlock);
    host.Pump();
    Session session{host, controller};
    session.Run(0.05);
    const auto settings = session.State()["settings"];
    Check(settings.value("enabled", false) && settings.value("bars", 0) == 1 && settings.value("maxSeconds", 0) == 30,
          "the looper settings are saved with the app settings: " + settings.dump());
}
} // namespace

int main()
{
    const fs::path sandbox = fs::temp_directory_path() / "guitarfx-looper-service-tests";
    std::error_code ec;
    fs::remove_all(sandbox, ec);
    fs::create_directories(sandbox, ec);
    test::SetSettingsEnvRoot(sandbox);
    RegisterAllEffects();

    Run(sandbox);
    TestSettingsPersist(sandbox);
    fs::remove_all(sandbox, ec);

    if (gFailures > 0)
    {
        std::cerr << gFailures << " check(s) failed" << std::endl;
        return 1;
    }

    std::cout << "LooperServiceTests passed" << std::endl;
    return 0;
}

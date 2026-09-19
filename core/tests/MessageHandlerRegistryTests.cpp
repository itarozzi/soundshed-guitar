/**
 * MessageHandlerRegistryTests.cpp — services that answer their own UI messages.
 *
 * The registry's own rules (one owner per message type), and the Practice Tool's and
 * the tuner's registrations: every message they used to reach through a
 * PluginController::Handle* method is registered, and those methods' payload checks and
 * replies still happen.
 * That registered handlers run inside the dispatcher's JSON-error guard is covered by
 * MessageDispatchTypeErrorTests.
 */

#include "IPluginHost.h"
#include "MessageHandlerRegistry.h"
#include "controller/PracticeToolService.h"
#include "controller/TunerService.h"
#include "dsp/MultiPresetMixer.h"

#include <filesystem>
#include <functional>
#include <iostream>
#include <mutex>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace fs = std::filesystem;
using guitarfx::MessageHandlerRegistry;

namespace
{
int gFailures = 0;

void Check(bool condition, const char* what)
{
    if (!condition)
    {
        std::cerr << "FAIL: " << what << "\n";
        ++gFailures;
    }
}

class TestHost final : public guitarfx::IPluginHost
{
  public:
    void SendMessageToUI(const std::string&) override
    {
    }

    void BrowseFileAsync(guitarfx::BrowseFileType, const std::string&,
                         std::function<void(const guitarfx::BrowseFileResult&)> callback) override
    {
        ++browseCount;
        callback(guitarfx::BrowseFileResult{}); // the user cancelled
    }

    void SaveFileAsync(guitarfx::BrowseFileType, const std::string&, const std::string&,
                       std::function<void(const guitarfx::BrowseFileResult&)> callback) override
    {
        callback(guitarfx::BrowseFileResult{});
    }

    void RunOnMainThread(std::function<void()> fn) override
    {
        fn();
    }

    [[nodiscard]] fs::path GetUserDataPath() const override
    {
        return fs::temp_directory_path();
    }

    [[nodiscard]] fs::path GetBundledAssetsPath() const override
    {
        return fs::temp_directory_path();
    }

    [[nodiscard]] double GetSampleRate() const override
    {
        return 48000.0;
    }

    [[nodiscard]] int GetBlockSize() const override
    {
        return 512;
    }

    int browseCount = 0;
};

void TestRegistryRules()
{
    MessageHandlerRegistry registry;
    int first = 0;
    int second = 0;

    Check(registry.Register("ping", [&](const nlohmann::json&) { ++first; }), "a new type registers");
    Check(!registry.Register("ping", [&](const nlohmann::json&) { ++second; }), "a second owner is refused");
    Check(!registry.Register("", [](const nlohmann::json&) {}), "an empty type is refused");
    Check(!registry.Register("empty", MessageHandlerRegistry::Handler{}), "an empty handler is refused");

    Check(registry.Dispatch("ping", nlohmann::json::object()) && first == 1 && second == 0,
          "the first owner handles the message");
    Check(!registry.Dispatch("unknown", nlohmann::json::object()), "an unregistered type is left to the caller");
    Check(registry.Handles("ping") && !registry.Handles("unknown"), "Handles reports registrations");
    Check(registry.Types() == std::vector<std::string>{"ping"}, "Types lists what is registered");
}

void TestPracticeToolRegistrations()
{
    TestHost host;
    std::mutex dspMutex;
    std::vector<std::string> errors;
    guitarfx::PracticeToolService service(
        host, dspMutex,
        [&](const std::string& message, const std::string& detail) { errors.push_back(message + ": " + detail); },
        [](const std::string&) {});

    MessageHandlerRegistry registry;
    service.RegisterMessageHandlers(registry);

    const std::vector<std::string> expected{
        "browsePracticeToolFile", "loadPracticeToolFile", "loadPracticeToolFileData", "seekPracticeToolFile",
        "setPracticeToolBalance", "setPracticeToolEq",    "setPracticeToolGain",      "setPracticeToolLoopRegion",
        "setPracticeToolLooping", "setPracticeToolPitch", "setPracticeToolSpeed",     "setPracticeToolTransport",
    };
    Check(registry.Types() == expected, "the Practice Tool registers every one of its messages");

    registry.Dispatch("loadPracticeToolFile", nlohmann::json{{"type", "loadPracticeToolFile"}});
    Check(errors.size() == 1 && errors.back().find("No file path provided") != std::string::npos,
          "a load without a path is reported, not attempted");

    registry.Dispatch("loadPracticeToolFileData", nlohmann::json{{"type", "loadPracticeToolFileData"}, {"data", ""}});
    Check(errors.size() == 2 && errors.back().find("did not include data") != std::string::npos,
          "a dropped file without data is reported");

    registry.Dispatch("browsePracticeToolFile", nlohmann::json::object());
    Check(host.browseCount == 1 && errors.size() == 2, "a cancelled browse opens the picker and loads nothing");

    // Nothing is loaded, so these only have to be accepted: wrong-typed fader values fall
    // back rather than throwing, and a loop region without bounds clears it.
    registry.Dispatch("setPracticeToolSpeed", nlohmann::json{{"ratio", "fast"}});
    registry.Dispatch("setPracticeToolGain", nlohmann::json{{"value", 0.5}});
    registry.Dispatch("setPracticeToolLoopRegion", nlohmann::json::object());
    registry.Dispatch("setPracticeToolEq", nlohmann::json{{"enabled", true}, {"params", {{"lowGain", 3.0}}}});
    registry.Dispatch("setPracticeToolTransport", nlohmann::json{{"action", "stop"}});
    Check(errors.size() == 2, "well-formed control messages report nothing");

    service.Shutdown();
}

void TestTunerRegistrations()
{
    guitarfx::MultiPresetMixer mixer;
    std::mutex dspMutex;
    std::vector<nlohmann::json> sent;
    guitarfx::TunerService tuner([&](const std::string& json) { sent.push_back(nlohmann::json::parse(json)); }, mixer,
                                 dspMutex);

    MessageHandlerRegistry registry;
    tuner.RegisterMessageHandlers(registry);
    Check(registry.Types() == std::vector<std::string>{"setTunerEnabled", "setTunerReference", "tuner"},
          "the tuner registers its three messages");

    registry.Dispatch("tuner", nlohmann::json{{"action", "start"}, {"liveMode", false}, {"referenceFrequency", 442.0}});
    Check(tuner.IsActive() && mixer.IsTunerEnabled() && !mixer.IsLiveTunerMode(),
          "starting turns the mixer's tuner on");
    Check(sent.size() == 1 && sent.back().value("type", "") == "tunerStarted" &&
              sent.back().value("referenceFrequency", 0.0) == mixer.GetTunerReferenceFrequency() &&
              sent.back().value("liveMode", true) == false,
          "a start is acknowledged with the reference pitch and monitoring mode");

    registry.Dispatch("tuner", nlohmann::json{{"action", "stop"}});
    Check(!tuner.IsActive() && !mixer.IsTunerEnabled(), "stopping turns it off");
    Check(sent.size() == 2 && sent.back() == nlohmann::json{{"type", "tunerStopped"}}, "a stop carries no fields");

    registry.Dispatch("setTunerEnabled", nlohmann::json{{"enabled", true}});
    Check(tuner.IsActive() && mixer.IsTunerEnabled() && sent.size() == 2, "setTunerEnabled switches without a reply");
}
} // namespace

int main()
{
    TestRegistryRules();
    TestPracticeToolRegistrations();
    TestTunerRegistrations();

    if (gFailures > 0)
    {
        std::cerr << gFailures << " check(s) failed\n";
        return 1;
    }

    std::cout << "MessageHandlerRegistryTests passed\n";
    return 0;
}

/**
 * AudioDeviceRequestTests.cpp — The standalone app's audio device settings reach the host.
 *
 * The devices belong to the host framework (juce/source/StandaloneAudioSettings), so the
 * core's part is small: an "audioDevice" UI message goes to IPluginHost::HandleAudioDeviceRequest
 * whole, and the full state tells the UI whether the host can take such requests at all, which
 * is what decides whether Settings shows its own device controls.
 */

#include <cstdlib>
#include <filesystem>
#include <functional>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "IPluginHost.h"
#include "PluginController.h"

namespace fs = std::filesystem;

namespace
{
class TestHost final : public guitarfx::IPluginHost
{
  public:
    TestHost(fs::path root, bool supportsAudioDeviceSettings)
        : mRoot(std::move(root)), mSupportsAudioDeviceSettings(supportsAudioDeviceSettings)
    {
    }

    void SendMessageToUI(const std::string& jsonMessage) override
    {
        sentMessages.push_back(jsonMessage);
    }

    void BrowseFileAsync(guitarfx::BrowseFileType, const std::string&,
                         std::function<void(const guitarfx::BrowseFileResult&)> callback) override
    {
        callback(guitarfx::BrowseFileResult{});
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
        return mRoot;
    }

    [[nodiscard]] fs::path GetBundledAssetsPath() const override
    {
        return mRoot;
    }

    [[nodiscard]] double GetSampleRate() const override
    {
        return 48000.0;
    }

    [[nodiscard]] int GetBlockSize() const override
    {
        return 512;
    }

    [[nodiscard]] bool IsStandalone() const override
    {
        return mSupportsAudioDeviceSettings;
    }

    [[nodiscard]] bool SupportsAudioDeviceSettings() const override
    {
        return mSupportsAudioDeviceSettings;
    }

    void HandleAudioDeviceRequest(const std::string& requestJson) override
    {
        audioDeviceRequests.push_back(requestJson);
    }

    std::vector<std::string> sentMessages;
    std::vector<std::string> audioDeviceRequests;

  private:
    fs::path mRoot;
    bool mSupportsAudioDeviceSettings;
};

bool Expect(bool condition, const std::string& message)
{
    if (!condition)
    {
        std::cerr << "FAIL: " << message << std::endl;
    }

    return condition;
}

void SetSettingsEnvRoot(const fs::path& root)
{
#ifdef _WIN32
    _putenv_s("APPDATA", root.string().c_str());
#else
    setenv("HOME", root.string().c_str(), 1);
#endif
}

std::optional<nlohmann::json> LatestOfType(const std::vector<std::string>& messages, const std::string& type)
{
    for (auto it = messages.rbegin(); it != messages.rend(); ++it)
    {
        auto parsed = nlohmann::json::parse(*it, nullptr, false);

        if (parsed.is_object() && parsed.value("type", std::string{}) == type)
        {
            return parsed;
        }
    }

    return std::nullopt;
}

/// What a full state broadcast says about the host's device settings, if it says anything.
std::optional<bool> ReportedAudioDeviceSettings(TestHost& host, guitarfx::PluginController& controller)
{
    // Nothing is broadcast until the page has loaded, which is also what queues the full state.
    host.sentMessages.clear();
    controller.OnWebContentLoaded();
    controller.OnIdle();

    const auto state = LatestOfType(host.sentMessages, "state");

    if (!state || !state->contains("environment") || !(*state)["environment"].contains("audioDeviceSettings"))
    {
        return std::nullopt;
    }

    return (*state)["environment"]["audioDeviceSettings"].get<bool>();
}
} // namespace

int main()
{
    const fs::path sandbox = fs::temp_directory_path() / "guitarfx-audio-device-request-tests";
    std::error_code ec;
    fs::remove_all(sandbox, ec);
    fs::create_directories(sandbox, ec);
    SetSettingsEnvRoot(sandbox);

    bool allPassed = true;

    {
        TestHost host(sandbox / "standalone", true);
        guitarfx::PluginController controller(host);
        controller.Initialize();

        controller.HandleUIMessage(R"({"type":"audioDevice","action":"setDevice","kind":"input","name":"USB In"})");
        controller.HandleUIMessage(R"({"type":"audioDevice","action":"setBufferSize","bufferSize":128})");

        allPassed &= Expect(host.audioDeviceRequests.size() == 2, "Each audioDevice message should reach the host once");

        if (host.audioDeviceRequests.size() == 2)
        {
            const auto device = nlohmann::json::parse(host.audioDeviceRequests[0]);
            allPassed &= Expect(device.value("action", std::string{}) == "setDevice"
                                    && device.value("kind", std::string{}) == "input"
                                    && device.value("name", std::string{}) == "USB In",
                                "The host should receive the request's action and arguments unchanged");

            const auto buffer = nlohmann::json::parse(host.audioDeviceRequests[1]);
            allPassed &= Expect(buffer.value("bufferSize", 0) == 128, "Numeric arguments should arrive as numbers");
        }

        allPassed &= Expect(ReportedAudioDeviceSettings(host, controller) == std::optional<bool>(true),
                            "The standalone app's full state should offer the device settings");
    }

    {
        TestHost host(sandbox / "plugin", false);
        guitarfx::PluginController controller(host);
        controller.Initialize();

        allPassed &= Expect(ReportedAudioDeviceSettings(host, controller) == std::optional<bool>(false),
                            "A plugin format's full state should not offer the device settings");
    }

    if (allPassed)
    {
        std::cout << "Audio device request tests passed" << std::endl;
        return 0;
    }

    return 1;
}

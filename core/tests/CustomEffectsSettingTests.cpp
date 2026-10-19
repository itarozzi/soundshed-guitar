// The Custom Effects setting is the engine's gate on WASM code (WasmEffect::SetHostingEnabled):
// off by default, on with the feature flag or, failing that, the legacy advanced-options switch,
// and never switched on by a DAW project, which is a file people send each other.

#include <filesystem>
#include <iostream>
#include <string>
#include <thread>

#include <nlohmann/json.hpp>

#include "MessageThreadTestHost.h"
#include "PluginController.h"
#include "dsp/effects/WasmEffect.h"

namespace fs = std::filesystem;
using namespace guitarfx;

namespace
{
constexpr double kSampleRate = 48000.0;
constexpr int kBlock = 256;
constexpr const char* kFeatureKey = "features.customEffects.enabled";
constexpr const char* kLegacyKey = "ui.advancedOptionsEnabled";

int gFailures = 0;

void Check(bool condition, const std::string& label)
{
    std::cout << "  " << label << ": " << (condition ? "PASS" : "FAIL") << "\n";

    if (!condition)
    {
        ++gFailures;
    }
}

class TestHost final : public test::PumpedTestHost
{
  public:
    explicit TestHost(fs::path userDataPath)
        : PumpedTestHost(std::move(userDataPath), std::this_thread::get_id(), kSampleRate, kBlock)
    {
    }
};

void SetSetting(PluginController& controller, TestHost& host, const char* key, const nlohmann::json& value)
{
    controller.HandleUIMessage(nlohmann::json{{"type", "setSetting"}, {"key", key}, {"value", value}}.dump());
    host.Pump();
}

void TestSettingDrivesTheGate(const fs::path& root)
{
    std::cout << "\n--- The setting drives the gate ---\n";
    TestHost host(root);
    PluginController controller(host);
    controller.Initialize();
    host.Pump();
    Check(!WasmEffect::IsHostingEnabled(), "Custom Effects are off by default");

    SetSetting(controller, host, kFeatureKey, true);
    Check(WasmEffect::IsHostingEnabled(), "turning the feature on opens the gate");

    SetSetting(controller, host, kFeatureKey, false);
    Check(!WasmEffect::IsHostingEnabled(), "turning it off closes it");

    SetSetting(controller, host, kFeatureKey, nullptr);
    SetSetting(controller, host, kLegacyKey, true);
    Check(WasmEffect::IsHostingEnabled(), "with the feature unset, the legacy advanced-options switch opens it");

    SetSetting(controller, host, kFeatureKey, false);
    Check(!WasmEffect::IsHostingEnabled(), "the feature setting outranks the legacy switch");
    SetSetting(controller, host, kLegacyKey, false);
}

void TestProjectCannotOpenTheGate(const fs::path& root)
{
    std::cout << "\n--- A DAW project cannot switch Custom Effects on ---\n";
    TestHost host(root);
    PluginController controller(host);
    controller.Initialize();
    host.Pump();
    Check(!WasmEffect::IsHostingEnabled(), "Custom Effects start off");

    auto state = nlohmann::json::parse(controller.SerializeState(), nullptr, false);
    Check(state.is_object(), "the controller serialises its state");

    if (!state.is_object())
    {
        return;
    }

    state["appSettings"][kFeatureKey] = true;
    state["appSettings"][kLegacyKey] = true;
    controller.DeserializeState(state.dump());
    host.Pump();
    Check(!WasmEffect::IsHostingEnabled(), "restoring a project that turns them on leaves them off");
}
} // namespace

int main()
{
    std::cout << std::unitbuf << "\n--- Custom Effects setting ---\n";

    const fs::path sandbox = fs::temp_directory_path() / "guitarfx-custom-effects-setting-tests";
    std::error_code ec;
    fs::remove_all(sandbox, ec);
    fs::create_directories(sandbox, ec);
    test::SetSettingsEnvRoot(sandbox);

    TestSettingDrivesTheGate(sandbox);
    TestProjectCannotOpenTheGate(sandbox);

    fs::remove_all(sandbox, ec);

    if (gFailures > 0)
    {
        std::cerr << "Custom Effects setting tests failed: " << gFailures << "\n";
        return 1;
    }

    std::cout << "All Custom Effects setting tests passed.\n";
    return 0;
}

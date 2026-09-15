/**
 * MessageDispatchTypeErrorTests.cpp — A parseable UI message with a wrong-typed field is
 * dropped rather than taking the app down.
 *
 * nlohmann's json::value(key, default) throws type_error.302 when the key holds an
 * incompatible type, and value() on anything but an object throws type_error.306. Those
 * used to escape HandleUIMessage and terminate the Standalone app (seen live with
 * setAutomationSlot and slotId: null). Without the guard in MessageDispatcher::Dispatch
 * every malformed message below aborts this process. The test also checks that automation
 * state is untouched, that each drop reaches the session log, and that a well-formed
 * message still applies afterwards.
 */

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <string>

#include <nlohmann/json.hpp>

#include "IPluginHost.h"
#include "PluginController.h"
#include "util/FileSystem.h"

namespace fs = std::filesystem;

namespace
{
constexpr const char* kDroppedLogPrefix = "Dropped UI message";

class TestHost final : public guitarfx::IPluginHost
{
  public:
    explicit TestHost(fs::path root) : mRoot(std::move(root))
    {
    }

    void SendMessageToUI(const std::string&) override
    {
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

  private:
    fs::path mRoot;
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

size_t CountOccurrences(const std::string& text, const std::string& needle)
{
    size_t count = 0;

    for (auto pos = text.find(needle); pos != std::string::npos; pos = text.find(needle, pos + needle.size()))
    {
        ++count;
    }

    return count;
}
} // namespace

int main()
{
    const fs::path sandbox = fs::temp_directory_path() / "guitarfx-message-dispatch-type-error-tests";
    std::error_code ec;
    fs::remove_all(sandbox, ec);
    fs::create_directories(sandbox, ec);
    SetSettingsEnvRoot(sandbox);

    TestHost host(sandbox);
    guitarfx::PluginController controller(host);

    bool allPassed = true;
    const auto slotsBefore = controller.GetAutomationSlots().SaveToJson();

    const char* const malformed[] = {
        // The payload seen live: payload.value("slotId", "") throws on null.
        R"({"type":"setAutomationSlot","slotId":null,"midiMap":{"eventType":1,"channel":0,"controller":60,"mode":1}})",
        // The dispatcher's own msg.value("type", "") throws for a non-string type...
        R"({"type":null})",
        R"({"type":42})",
        // ...and for a top-level value that is not an object.
        R"([1,2,3])",
        // A throw deep in a handler: past its slotId check, before it touches the table.
        R"({"type":"setAutomationSlot","slotId":"custom.typeError","midiMap":{"channel":"one"}})",
    };

    for (const char* message : malformed)
    {
        controller.HandleUIMessage(message);
    }

    allPassed &= Expect(controller.GetAutomationSlots().SaveToJson() == slotsBefore,
                        "Malformed messages must leave the automation slots unchanged");

    std::ifstream logFile(guitarfx::FileSystem{}.ResolveSettingsDirectory() / "logs" / "session-log.txt");
    const std::string log{std::istreambuf_iterator<char>(logFile), std::istreambuf_iterator<char>()};

    allPassed &= Expect(CountOccurrences(log, kDroppedLogPrefix) == std::size(malformed),
                        "Each malformed message should add one line to the session log");
    allPassed &= Expect(CountOccurrences(log, std::string{kDroppedLogPrefix} + " type=setAutomationSlot") == 2,
                        "The session log should name the type of a dropped setAutomationSlot message");
    allPassed &=
        Expect(log.find("type_error.302") != std::string::npos && log.find("type_error.306") != std::string::npos,
               "The session log should carry the JSON exception text");

    controller.HandleUIMessage(
        R"({"type":"setAutomationSlot","slotId":"custom.afterTypeError","label":"After","address":"global.inputTrim"})");
    const auto* applied = controller.GetAutomationSlots().FindSlot("custom.afterTypeError");
    allPassed &= Expect(applied != nullptr && applied->address == "global.inputTrim",
                        "A well-formed message after the dropped ones must still apply");

    if (allPassed)
    {
        std::cout << "MessageDispatch type error tests passed" << std::endl;
        return 0;
    }

    return 1;
}

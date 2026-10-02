// Folders and profiles named outside plain ASCII, end to end through the controller.
//
// On Windows the narrow side of std::filesystem::path is the ANSI code page, not UTF-8.
// Anything that crossed from a path to a std::string with path::string(), or back with
// path(std::string), garbled a name like "Müzik" and threw for one the code page cannot
// spell. A user's "Müzik" captures folder stayed on "No folder selected", and a Windows
// user name like "Şule" put the profile in a folder that does not exist.
//
// The JUCE side of the same crossing (the file dialog's result, the profile folder) is
// covered by juce/tests/JucePathConversionTests.cpp.

#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "IPluginHost.h"
#include "PluginController.h"
#include "util/FileSystem.h"
#include "util/PathEncoding.h"

namespace fs = std::filesystem;

namespace
{
class TestHost final : public guitarfx::IPluginHost
{
  public:
    explicit TestHost(fs::path root) : mRoot(std::move(root))
    {
    }

    void SendMessageToUI(const std::string& jsonMessage) override
    {
        std::lock_guard<std::mutex> lock(mMessageMutex);
        mMessages.push_back(jsonMessage);
        mMessageCv.notify_all();
    }

    void BrowseFileAsync(guitarfx::BrowseFileType, const std::string&,
                         std::function<void(const guitarfx::BrowseFileResult&)> callback) override
    {
        guitarfx::BrowseFileResult result;

        if (browseResult)
        {
            result.success = true;
            result.path = *browseResult;
        }

        callback(result);
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

    /// What the file dialog hands back; unset means the user cancelled it.
    std::optional<fs::path> browseResult;

    /// The latest message of this type, waiting up to `timeout` for one to arrive.
    std::optional<nlohmann::json> WaitForMessage(const std::string& type,
                                                 std::chrono::milliseconds timeout = std::chrono::seconds(5))
    {
        std::unique_lock<std::mutex> lock(mMessageMutex);
        std::optional<nlohmann::json> found;

        mMessageCv.wait_for(lock, timeout, [&]() {
            for (auto it = mMessages.rbegin(); it != mMessages.rend(); ++it)
            {
                // Every message must parse: one that does not is a path that left in the ANSI code page.
                auto parsed = nlohmann::json::parse(*it);

                if (parsed.value("type", "") == type)
                {
                    found = std::move(parsed);
                    return true;
                }
            }

            return false;
        });

        return found;
    }

  private:
    fs::path mRoot;
    std::vector<std::string> mMessages;
    std::condition_variable mMessageCv;
    std::mutex mMessageMutex;
};

/// Points the profile root (%APPDATA%, or $HOME elsewhere) at `root` for as long as it lives.
/// On Windows through the wide environment: the only one that can hold every name used here.
class ScopedProfileRoot
{
  public:
    explicit ScopedProfileRoot(const fs::path& root)
    {
#ifdef _WIN32
        wchar_t* value = nullptr;
        std::size_t length = 0;

        if (_wdupenv_s(&value, &length, L"APPDATA") == 0 && value != nullptr)
        {
            mPrevious = value;
            std::free(value);
        }

        _wputenv_s(L"APPDATA", root.c_str());
#else
        if (const char* value = std::getenv("HOME"))
        {
            mPrevious = value;
        }

        setenv("HOME", root.c_str(), 1);
#endif
    }

    ~ScopedProfileRoot()
    {
#ifdef _WIN32
        _wputenv_s(L"APPDATA", mPrevious.c_str());
#else
        setenv("HOME", mPrevious.c_str(), 1);
#endif
    }

    ScopedProfileRoot(const ScopedProfileRoot&) = delete;
    ScopedProfileRoot& operator=(const ScopedProfileRoot&) = delete;

  private:
    fs::path::string_type mPrevious;
};

fs::path FreshSandbox(const fs::path& leaf)
{
    const fs::path sandbox = fs::temp_directory_path() / "guitarfx-non-ascii-path-tests" / leaf;
    std::error_code ec;
    fs::remove_all(sandbox, ec);
    fs::create_directories(sandbox, ec);
    return sandbox;
}

/// A folder picked under a name outside plain ASCII is added, listed, and its models already in
/// the library are marked as such. The scan matched library entries through path(std::string),
/// so even once listed, no model in such a folder showed as already in the library.
bool TestFolderPickUnderNonAsciiName()
{
    const fs::path sandbox = FreshSandbox("folder-pick");
    const ScopedProfileRoot profileRoot(sandbox);

    // "Müzik" is the reported folder; "şarkılar" has letters the Western code page cannot spell.
    const std::string leafName = "\xC5\x9F"
                                 "ark\xC4\xB1lar";
    const fs::path folder =
        sandbox / guitarfx::util::PathFromUtf8("M\xC3\xBCzik") / guitarfx::util::PathFromUtf8(leafName);
    const fs::path model = folder / "A2.nam";
    const fs::path source = fs::path(GUITARFX_TEST_RESOURCES_DIR) / "assets" / "amps" / "Guitar" / "A2" /
                            guitarfx::util::PathFromUtf8("A2 -wth space an \xE2\x80\x94 _emdash.nam");
    std::error_code ec;
    fs::create_directories(folder, ec);

    if (!fs::copy_file(source, model, fs::copy_options::overwrite_existing, ec))
    {
        std::cerr << "Could not stage the test model: " << ec.message() << "\n";
        return false;
    }

    TestHost host(sandbox);
    guitarfx::PluginController controller(host);

    controller.HandleUIMessage(nlohmann::json{
        {"type", "saveLocalLibraryResource"}, {"resourceType", "nam"}, {"filePath", guitarfx::util::PathToUtf8(model)}}
                                   .dump());

    std::string libraryId;

    for (const auto& resource : controller.GetResourceLibrary().GetResourcesByType("nam"))
    {
        if (resource.filePath == model)
        {
            libraryId = resource.id;
        }
    }

    host.browseResult = folder;
    controller.HandleUIMessage(R"({"type":"browseResourceFolder"})");
    const auto picked = host.WaitForMessage("resourceFolderPicked");

    if (libraryId.empty() || !picked || !picked->value("success", false) ||
        picked->value("path", "") != guitarfx::util::PathToUtf8(folder) || picked->value("name", "") != leafName)
    {
        std::cerr << "Unexpected pick: " << (picked ? picked->dump() : std::string("none")) << "\n";
        return false;
    }

    controller.HandleUIMessage(
        nlohmann::json{{"type", "listResourceFolder"}, {"path", picked->value("path", "")}}.dump());
    const auto listing = host.WaitForMessage("resourceFolderListing");

    if (!listing)
    {
        std::cerr << "The picked folder was not listed\n";
        return false;
    }

    for (const auto& file : listing->value("files", nlohmann::json::array()))
    {
        if (file.value("name", "") == "A2.nam")
        {
            return file.value("libraryId", "") == libraryId;
        }
    }

    std::cerr << "The model was missing from the listing: " << listing->dump() << "\n";
    return false;
}

/// A profile under a Windows user name outside plain ASCII. The core read %APPDATA% through the
/// narrow environment, where the Western code page spells "Şule" as "Sule", and paths it reported
/// went through path::string(): the ANSI code page, which JSON rejects, or a throw.
bool TestProfileUnderNonAsciiUserName()
{
#ifdef _WIN32
    const fs::path sandbox = FreshSandbox(guitarfx::util::PathFromUtf8("\xC5\x9Eule G\xC3\xB6khan"));
    const fs::path profile = sandbox / "Soundshed Guitar";
    const ScopedProfileRoot profileRoot(sandbox);

    if (const auto resolved = guitarfx::FileSystem{}.ResolvePlatformRootDirectory(); resolved != profile)
    {
        std::cerr << "The profile resolved to " << guitarfx::util::PathToUtf8(resolved) << "\n";
        return false;
    }

    TestHost host(profile);
    guitarfx::PluginController controller(host);
    controller.Initialize();
    controller.HandleUIMessage(R"({"type":"debugReportUiState","source":"test"})");

    const auto written = host.WaitForMessage("debugSnapshotWritten");
    const std::string reportedPath = written ? written->value("path", "") : "";
    const std::string profilePrefix = guitarfx::util::PathToUtf8(profile) + "/";

    if (reportedPath.rfind(profilePrefix, 0) != 0 || !fs::exists(guitarfx::util::PathFromUtf8(reportedPath)) ||
        !fs::exists(profile / "data" / "v1" / "soundshed.db"))
    {
        std::cerr << "Snapshot path '" << reportedPath << "' is not a file under '" << profilePrefix
                  << "', or the document store is missing\n";
        return false;
    }
#endif

    return true;
}
} // namespace

int main()
{
    int failed = 0;

    const auto run = [&](const std::string& name, bool (*test)()) {
        bool ok = false;

        try
        {
            ok = test();
        }
        catch (const std::exception& e)
        {
            std::cerr << name << " threw: " << e.what() << "\n";
        }

        std::cout << (ok ? "[PASS] " : "[FAIL] ") << name << "\n";
        failed += ok ? 0 : 1;
    };

    run("Folder pick under a non-ASCII name", TestFolderPickUnderNonAsciiName);
    run("Profile under a non-ASCII user name", TestProfileUnderNonAsciiUserName);

    std::cout << "\nNon-ASCII path workflow tests: " << (failed == 0 ? "all passed" : "FAILED") << "\n";
    return failed == 0 ? 0 : 1;
}

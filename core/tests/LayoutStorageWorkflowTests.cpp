// Custom effect layouts on disk, end to end through the controller.
//
// An imported layout archive names its own images, and the engine wrote each one to
// imagesDir / fileName as given: "..\..\Startup\x.bat" or "C:/x.dll" went where they pointed.
// Images were also copied into a layout's own folder only on its first save, so a layout that
// picked another layout's image lost it when that layout was deleted. These drive the real
// message handlers against a sandboxed profile.

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "IPluginHost.h"
#include "PluginController.h"
#include "util/Base64.h"
#include "util/FileSystem.h"
#include "util/PathEncoding.h"

#ifdef _WIN32
    #include <process.h>
    #define GUITARFX_TEST_PID _getpid()
#else
    #include <unistd.h>
    #define GUITARFX_TEST_PID getpid()
#endif

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

    /// The latest message of this type, waiting up to `timeout` for one to arrive.
    std::optional<nlohmann::json> WaitForMessage(const std::string& type,
                                                 std::chrono::milliseconds timeout = std::chrono::seconds(5))
    {
        std::unique_lock<std::mutex> lock(mMessageMutex);
        std::optional<nlohmann::json> found;

        mMessageCv.wait_for(lock, timeout, [&]() {
            for (auto it = mMessages.rbegin(); it != mMessages.rend(); ++it)
            {
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

    [[nodiscard]] std::size_t CountMessages(const std::string& type)
    {
        std::lock_guard<std::mutex> lock(mMessageMutex);
        std::size_t count = 0;

        for (const auto& message : mMessages)
        {
            if (nlohmann::json::parse(message).value("type", "") == type)
            {
                ++count;
            }
        }

        return count;
    }

  private:
    fs::path mRoot;
    std::vector<std::string> mMessages;
    std::condition_variable mMessageCv;
    std::mutex mMessageMutex;
};

/// Points every FileSystem at `root` for as long as it lives.
class ScopedPlatformRoot
{
  public:
    explicit ScopedPlatformRoot(const fs::path& root)
    {
        guitarfx::FileSystem::SetPlatformRootOverride(root);
    }

    ~ScopedPlatformRoot()
    {
        guitarfx::FileSystem::SetPlatformRootOverride({});
    }

    ScopedPlatformRoot(const ScopedPlatformRoot&) = delete;
    ScopedPlatformRoot& operator=(const ScopedPlatformRoot&) = delete;
};

/// A folder of this process's own under %TEMP%: other test runs clear theirs whenever they start.
fs::path FreshSandbox(const std::string& leaf)
{
    const fs::path sandbox =
        fs::temp_directory_path() / ("guitarfx-layout-storage-tests-" + std::to_string(GUITARFX_TEST_PID)) / leaf;
    std::error_code ec;
    fs::remove_all(sandbox, ec);
    fs::create_directories(sandbox, ec);
    return sandbox;
}

fs::path ContentDir(const fs::path& sandbox)
{
    return sandbox / "data" / "v1" / "layouts" / "content";
}

std::vector<std::uint8_t> PngBytes()
{
    return {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A, 0x00, 0x00, 0x00, 0x0D, 'I', 'H', 'D', 'R'};
}

std::vector<std::uint8_t> JpegBytes()
{
    return {0xFF, 0xD8, 0xFF, 0xE0, 0x00, 0x10, 'J', 'F', 'I', 'F', 0x00};
}

void WriteBytes(const fs::path& path, const std::vector<std::uint8_t>& bytes)
{
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

void SaveImage(guitarfx::PluginController& controller, const std::string& layoutId, const std::string& imageId,
               const std::string& fileName, const std::vector<std::uint8_t>& bytes)
{
    controller.HandleUIMessage(nlohmann::json{
        {"type", "saveLayoutImage"},
        {"imageId", imageId},
        {"fileName", fileName},
        {"data", guitarfx::util::EncodeBase64(bytes)},
        {"layoutId", layoutId}}.dump());
}

/// Saves a layout whose background layers draw on `imageIds`, one layer each.
void SaveLayout(guitarfx::PluginController& controller, const std::string& layoutId, const std::string& effectType,
                const std::vector<std::string>& imageIds, const std::string& blendId = {})
{
    nlohmann::json backgrounds = nlohmann::json::array();

    for (std::size_t i = 0; i < imageIds.size(); ++i)
    {
        backgrounds.push_back({{"layerIndex", i}, {"type", "image"}, {"value", imageIds[i]}});
    }

    const nlohmann::json layout{{"effectType", effectType},
                                {"layoutId", layoutId},
                                {"version", 1},
                                {"dimensions", {{"width", 400}, {"height", 280}}},
                                {"backgrounds", backgrounds},
                                {"controls", nlohmann::json::array()},
                                {"textLabels", nlohmann::json::array()}};

    controller.HandleUIMessage(nlohmann::json{
        {"type", "saveEffectLayout"},
        {"effectType", effectType},
        {"blendId", blendId},
        {"layoutId", layoutId},
        {"layout", layout}}.dump());
}

/// Every name an archive could use to reach outside the layout's folder is refused and reported,
/// and nothing is written anywhere. A plain name still lands in the layout's own folder.
bool TestImportedImageNamesStayInTheLayoutFolder()
{
    const fs::path sandbox = FreshSandbox("unsafe-names");
    const ScopedPlatformRoot root(sandbox);
    const fs::path outside = sandbox / "outside";
    fs::create_directories(outside);

    TestHost host(sandbox);
    guitarfx::PluginController controller(host);
    const std::string layoutId = "layout-import";
    const std::size_t errorsBefore = host.CountMessages("error");

    struct Case
    {
        std::string imageId;
        std::string fileName;
        std::vector<std::uint8_t> bytes;
    };

    const std::vector<Case> refused = {
        {"escape", "..\\..\\..\\..\\escape.png", PngBytes()},
        {"escape", "../../../../escape.png", PngBytes()},
        {"absolute", guitarfx::util::PathToUtf8(outside / "absolute.png"), PngBytes()},
        {"drive", "C:drive.png", PngBytes()},
        {"script", "script.bat", PngBytes()},
        {"CON", "CON.png", PngBytes()},
        {"mismatch", "other.png", PngBytes()},
        {"fake", "fake.png", std::vector<std::uint8_t>{'M', 'Z', 0x90, 0x00}},
    };

    for (const auto& c : refused)
    {
        SaveImage(controller, layoutId, c.imageId, c.fileName, c.bytes);
    }

    const std::set<std::string> refusedNames = {"escape.png", "absolute.png", "drive.png", "script.bat",
                                                "CON.png",    "other.png",    "fake.png"};

    for (const auto& entry : fs::recursive_directory_iterator(sandbox))
    {
        if (entry.is_regular_file() && refusedNames.contains(guitarfx::util::PathToUtf8(entry.path().filename())))
        {
            std::cerr << "A refused image was written: " << guitarfx::util::PathToUtf8(entry.path()) << "\n";
            return false;
        }
    }

    if (const auto reported = host.CountMessages("error") - errorsBefore; reported != refused.size())
    {
        std::cerr << "Expected " << refused.size() << " refusals reported, got " << reported << "\n";
        return false;
    }

    SaveImage(controller, layoutId, "face_1", "face_1.png", PngBytes());

    if (!fs::is_regular_file(ContentDir(sandbox) / layoutId / "images" / "face_1.png"))
    {
        std::cerr << "A plain image name was not written to the layout's folder\n";
        return false;
    }

    return true;
}

/// A layout that picks another layout's image keeps it when that layout is deleted: a save takes
/// a copy at once, and a delete gives the copy to a layout that borrowed it before saves did.
bool TestBorrowedImagesSurviveDeletingTheirLayout()
{
    const fs::path sandbox = FreshSandbox("borrowed-images");
    const ScopedPlatformRoot root(sandbox);
    const fs::path content = ContentDir(sandbox);

    TestHost host(sandbox);
    guitarfx::PluginController controller(host);

    SaveImage(controller, "A", "shared_1", "shared_1.png", PngBytes());
    SaveLayout(controller, "A", "fx_a", {"shared_1"});
    SaveLayout(controller, "B", "fx_b", {"shared_1"});

    if (!fs::is_regular_file(content / "B" / "images" / "shared_1.png"))
    {
        std::cerr << "Saving B did not copy the image it picked from A\n";
        return false;
    }

    // C refers to an image that only A holds and was never copied, as a layout saved before
    // every save copied its images does.
    SaveLayout(controller, "C", "fx_c", {"late_2"});
    SaveImage(controller, "A", "late_2", "late_2.png", PngBytes());
    SaveLayout(controller, "A", "fx_a", {"shared_1", "late_2"});

    if (fs::exists(content / "C" / "images" / "late_2.png"))
    {
        std::cerr << "Test setup: C already holds its own copy\n";
        return false;
    }

    controller.HandleUIMessage(
        nlohmann::json{{"type", "deleteLayout"}, {"effectType", "fx_a"}, {"blendId", ""}, {"layoutId", "A"}}.dump());

    if (fs::exists(content / "A"))
    {
        std::cerr << "The deleted layout's folder is still there\n";
        return false;
    }

    if (!fs::is_regular_file(content / "B" / "images" / "shared_1.png") ||
        !fs::is_regular_file(content / "C" / "images" / "late_2.png"))
    {
        std::cerr << "Deleting A took an image another layout draws on\n";
        return false;
    }

    controller.HandleUIMessage(R"({"type":"requestLayoutImages"})");
    const auto loaded = host.WaitForMessage("layoutImagesLoaded");
    std::set<std::string> ids;

    for (const auto& image : loaded ? loaded->value("images", nlohmann::json::array()) : nlohmann::json::array())
    {
        ids.insert(image.value("imageId", ""));
    }

    if (!ids.contains("shared_1") || !ids.contains("late_2"))
    {
        std::cerr << "The surviving images were not listed: " << (loaded ? loaded->dump() : std::string("none"))
                  << "\n";
        return false;
    }

    return true;
}

/// Images an older build left behind still load: one with an upper-case extension (the picker
/// kept the file's own case) and one in the shared folder an id-less layout wrote to.
bool TestLoaderListsLegacyAndUpperCaseImages()
{
    const fs::path sandbox = FreshSandbox("legacy-images");
    const ScopedPlatformRoot root(sandbox);
    const fs::path content = ContentDir(sandbox);
    WriteBytes(content / "L" / "images" / "SHOUT_1.JPG", JpegBytes());
    WriteBytes(content / "images" / "legacy_1.png", PngBytes());

    TestHost host(sandbox);
    guitarfx::PluginController controller(host);
    SaveLayout(controller, "L", "fx", {"SHOUT_1", "legacy_1"});

    if (!fs::is_regular_file(content / "L" / "images" / "legacy_1.png"))
    {
        std::cerr << "Saving did not copy the image from the legacy folder\n";
        return false;
    }

    controller.HandleUIMessage(R"({"type":"requestLayoutImages"})");
    const auto loaded = host.WaitForMessage("layoutImagesLoaded");
    bool jpeg = false;
    bool png = false;

    for (const auto& image : loaded ? loaded->value("images", nlohmann::json::array()) : nlohmann::json::array())
    {
        const std::string dataUrl = image.value("dataUrl", "");
        jpeg = jpeg || (image.value("imageId", "") == "SHOUT_1" && dataUrl.rfind("data:image/jpeg;base64,", 0) == 0);
        png = png || (image.value("imageId", "") == "legacy_1" && dataUrl.rfind("data:image/png;base64,", 0) == 0);
    }

    if (!jpeg || !png)
    {
        std::cerr << "Expected SHOUT_1 as JPEG and legacy_1 as PNG: " << (loaded ? loaded->dump() : std::string("none"))
                  << "\n";
        return false;
    }

    return true;
}

/// A save the engine cannot complete says so, naming the layout, rather than claiming success.
bool TestFailedSaveIsReported()
{
    const fs::path sandbox = FreshSandbox("failed-save");
    const ScopedPlatformRoot root(sandbox);

    TestHost host(sandbox);
    guitarfx::PluginController controller(host);
    controller.HandleUIMessage(R"({"type":"saveEffectLayout","layoutId":"X","layout":{"version":1}})");

    const auto failed = host.WaitForMessage("layoutSaveFailed", std::chrono::milliseconds(500));

    if (!failed || failed->value("layoutId", "") != "X" || host.CountMessages("layoutSaved") != 0)
    {
        std::cerr << "A save with no effect type was not reported as failed\n";
        return false;
    }

    return true;
}

/// A layout listed under an effect type and a blend of it is gone from both once deleted.
bool TestDeleteRemovesTheLayoutFromEveryKey()
{
    const fs::path sandbox = FreshSandbox("delete-every-key");
    const ScopedPlatformRoot root(sandbox);

    TestHost host(sandbox);
    guitarfx::PluginController controller(host);
    SaveLayout(controller, "L", "fx", {});
    SaveLayout(controller, "L", "fx", {}, "blend1");
    controller.HandleUIMessage(
        nlohmann::json{{"type", "deleteLayout"}, {"effectType", "fx"}, {"blendId", ""}, {"layoutId", "L"}}.dump());

    std::ifstream input(sandbox / "data" / "v1" / "layouts" / "indexes" / "effect-layouts.json");
    const auto index = nlohmann::json::parse(input, nullptr, false);
    const auto associations =
        index.is_object() ? index.value("associations", nlohmann::json::object()) : nlohmann::json::object();

    if (associations.contains("fx") || associations.contains("fx::blend1"))
    {
        std::cerr << "The deleted layout is still indexed: " << index.dump() << "\n";
        return false;
    }

    const auto library = host.WaitForMessage("layoutLibraryLoaded");
    const auto byEffectType =
        library
            ? library->value("layoutLibrary", nlohmann::json::object()).value("byEffectType", nlohmann::json::object())
            : nlohmann::json::object();

    if (!library || byEffectType.contains("fx") || byEffectType.contains("fx::blend1"))
    {
        std::cerr << "The library still lists the deleted layout\n";
        return false;
    }

    return true;
}
} // namespace

int main()
{
    struct Test
    {
        const char* name;
        bool (*run)();
    };

    const Test tests[] = {
        {"Imported image names stay in the layout folder", TestImportedImageNamesStayInTheLayoutFolder},
        {"Borrowed images survive deleting their layout", TestBorrowedImagesSurviveDeletingTheirLayout},
        {"Loader lists legacy and upper-case images", TestLoaderListsLegacyAndUpperCaseImages},
        {"A failed save is reported", TestFailedSaveIsReported},
        {"Delete removes the layout from every key", TestDeleteRemovesTheLayoutFromEveryKey},
    };

    int failed = 0;

    for (const auto& test : tests)
    {
        bool ok = false;

        try
        {
            ok = test.run();
        }
        catch (const std::exception& e)
        {
            std::cerr << "Exception: " << e.what() << "\n";
        }

        std::cout << (ok ? "[PASS] " : "[FAIL] ") << test.name << "\n";
        failed += ok ? 0 : 1;
    }

    std::error_code ec;
    fs::remove_all(fs::temp_directory_path() / ("guitarfx-layout-storage-tests-" + std::to_string(GUITARFX_TEST_PID)),
                   ec);
    return failed == 0 ? 0 : 1;
}

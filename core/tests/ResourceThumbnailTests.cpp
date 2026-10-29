/**
 * ResourceThumbnailTests.cpp - The resource browser's thumbnails of Tone3000 tones.
 *
 * getResourceThumbnails says which picture each library resource shows (cached, to fetch, or a
 * tone to look up); saveResourceThumbnail keeps what a UI fetched or looked up. These hold the
 * answer's shape, that every resource of a tone shares one picture, that a lookup's result
 * (a picture, or none) is remembered on disk, that only real pictures of a sane size are stored,
 * and that the editor's route serves only cache files.
 */

#include <filesystem>
#include <functional>
#include <iostream>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "MessageHandlerRegistry.h"
#include "MessageThreadTestHost.h"
#include "PluginController.h"
#include "controller/ResourceThumbnailService.h"
#include "resources/ResourceLibrary.h"
#include "util/Base64.h"

namespace fs = std::filesystem;

namespace
{
using Service = guitarfx::ResourceThumbnailService;

const std::string kPicture = "https://api.tone3000.com/storage/v1/object/public/images/abc123.jpg";
const std::string kOtherPicture = "https://api.tone3000.com/storage/v1/object/public/images/def456.png";

class RecordingHost final : public guitarfx::test::PumpedTestHost
{
  public:
    explicit RecordingHost(const fs::path& root) : PumpedTestHost(root, std::this_thread::get_id(), 48000.0, 512)
    {
    }

    void SendMessageToUI(const std::string& message) override
    {
        const std::lock_guard<std::mutex> lock(mMutex);
        mMessages.push_back(message);
    }

    void RunOnMainThread(std::function<void()> fn) override
    {
        fn();
    }

    [[nodiscard]] std::optional<nlohmann::json> LastMessageOfType(const std::string& type)
    {
        const std::lock_guard<std::mutex> lock(mMutex);

        for (auto it = mMessages.rbegin(); it != mMessages.rend(); ++it)
        {
            const auto parsed = nlohmann::json::parse(*it, nullptr, false);

            if (parsed.is_object() && parsed.value("type", "") == type)
            {
                return parsed;
            }
        }

        return std::nullopt;
    }

  private:
    std::mutex mMutex;
    std::vector<std::string> mMessages;
};

fs::path FreshSandbox(const std::string& name)
{
    const fs::path sandbox = fs::temp_directory_path() / "guitarfx-resource-thumbnail-tests" / name;
    std::error_code ec;
    fs::remove_all(sandbox, ec);
    fs::create_directories(sandbox, ec);
    guitarfx::test::SetSettingsEnvRoot(sandbox);
    return sandbox;
}

void AddResource(guitarfx::PluginController& controller, const std::string& id,
                 std::map<std::string, std::string> metadata, const std::string& type = "nam")
{
    guitarfx::LibraryResource resource;
    resource.type = type;
    resource.id = id;
    resource.name = id;
    resource.metadata = std::move(metadata);
    controller.GetResourceLibrary().AddResource(resource);
}

nlohmann::json Ask(guitarfx::PluginController& controller, RecordingHost& host, const std::string& type = "nam",
                   const std::optional<std::vector<std::string>>& ids = std::nullopt)
{
    nlohmann::json message{{"type", "getResourceThumbnails"}, {"resourceType", type}, {"requestId", "r1"}};

    if (ids)
    {
        message["resourceIds"] = *ids;
    }

    controller.HandleUIMessage(message.dump());
    return host.LastMessageOfType("resourceThumbnails").value_or(nlohmann::json::object());
}

std::string Jpeg()
{
    const std::vector<std::uint8_t> bytes{0xFF, 0xD8, 0xFF, 0xE0, 0, 16, 'J', 'F', 'I', 'F', 0, 1, 2, 3};
    return guitarfx::util::EncodeBase64(bytes);
}

/// answer[group][key][field] as text, or empty when any of it is missing.
std::string Field(const nlohmann::json& answer, const char* group, const std::string& key, const char* field)
{
    const auto g = answer.find(group);

    if (g == answer.end() || !g->is_object() || !g->contains(key) || !(*g)[key].is_object())
    {
        return {};
    }

    return (*g)[key].value(field, std::string());
}

bool Check(bool condition, const std::string& what)
{
    if (!condition)
    {
        std::cerr << "  " << what << "\n";
    }

    return condition;
}

bool TestRecordedPictureIsFetchedThenServedFromTheCache()
{
    const auto sandbox = FreshSandbox("recorded");
    RecordingHost host(sandbox);
    guitarfx::PluginController controller(host);
    AddResource(controller, "tone3000:1", {{"provider", "tone3000"}, {"toneId", "10"}, {"imageUrl", kPicture}});
    AddResource(controller, "tone3000:2", {{"provider", "tone3000"}, {"toneId", "10"}}); // same tone, no URL
    AddResource(controller, "local-model", {{"gearMake", "Fender"}});                    // not from Tone3000

    const auto key = Service::KeyFor(kPicture);
    auto answer = Ask(controller, host);
    bool ok = Check(answer.value("requestId", "") == "r1", "the request id is not echoed");
    ok &= Check(answer["thumbnails"].value("tone3000:1", "") == key, "the recorded picture is not the thumbnail");
    ok &= Check(answer["thumbnails"].value("tone3000:2", "") == key, "the tone's other model does not share it");
    ok &= Check(!answer["thumbnails"].contains("local-model"), "a resource not from Tone3000 has a thumbnail");
    ok &= Check(Field(answer, "fetch", key, "url") ==
                    "https://api.tone3000.com/storage/v1/render/image/public/images/abc123.jpg?width=96&height=96&"
                    "resize=cover",
                "the fetch URL is not the sized copy: " + answer["fetch"].dump());
    ok &= Check(answer["files"].empty() && answer["lookup"].empty(), "nothing is cached or looked up yet");

    controller.HandleUIMessage(
        nlohmann::json{{"type", "saveResourceThumbnail"}, {"imageUrl", kPicture}, {"data", Jpeg()}}.dump());
    answer = Ask(controller, host, "nam", std::vector<std::string>{"tone3000:2"});
    const auto file = Field(answer, "files", key, "file");
    ok &= Check(file == key + ".jpg", "the saved thumbnail is not listed as a file: " + answer.dump());
    ok &= Check(answer["fetch"].empty(), "a cached thumbnail is still to be fetched");
    ok &= Check(!answer["thumbnails"].contains("tone3000:1"), "a resource not asked about is answered");
    ok &= Check(fs::exists(Service::CacheDirectory() / file), "the file is not in the cache folder");

    const auto served = Service::ResolveUiResource("resource-thumbnails/" + file);
    ok &= Check(served && served->second == "image/jpeg", "the editor's route does not serve the file");
    return ok;
}

bool TestLookupIsListedAndItsResultRemembered()
{
    const auto sandbox = FreshSandbox("lookup");
    RecordingHost host(sandbox);
    guitarfx::PluginController controller(host);
    // Imported before the picture's URL was recorded: a tone id and title, no imageUrl key.
    AddResource(controller, "tone3000:3", {{"provider", "tone3000"}, {"toneId", "20"}, {"toneTitle", "Twin"}});
    AddResource(controller, "tone3000:4", {{"provider", "tone3000"}, {"toneId", "30"}, {"toneTitle", "Plexi"}});
    // Imported when the tone had no picture: nothing to look up.
    AddResource(controller, "tone3000:5", {{"provider", "tone3000"}, {"toneId", "40"}, {"imageUrl", ""}});
    // No title to search by: nothing to look up either.
    AddResource(controller, "tone3000:6", {{"provider", "tone3000"}, {"toneId", "50"}});

    auto answer = Ask(controller, host);
    bool ok = Check(answer["thumbnails"].empty(), "a thumbnail without a picture: " + answer.dump());
    ok &= Check(answer["lookup"].size() == 2, "not exactly the two titled tones to look up: " + answer.dump());
    ok &= Check(answer["lookup"].size() == 2 && answer["lookup"][0].value("toneId", "") == "20" &&
                    answer["lookup"][0].value("title", "") == "Twin" &&
                    answer["lookup"][0]["resourceIds"] == nlohmann::json::array({"tone3000:3"}),
                "the lookup does not carry the tone, title and resources");

    controller.HandleUIMessage(
        nlohmann::json{{"type", "saveResourceThumbnail"}, {"toneId", "20"}, {"imageUrl", kOtherPicture}}.dump());
    controller.HandleUIMessage(
        nlohmann::json{{"type", "saveResourceThumbnail"}, {"toneId", "30"}, {"imageUrl", ""}}.dump());
    answer = Ask(controller, host);
    ok &= Check(answer["lookup"].empty(), "a looked-up tone is listed again: " + answer.dump());
    ok &= Check(answer["thumbnails"].value("tone3000:3", "") == Service::KeyFor(kOtherPicture),
                "the looked-up picture is not used");
    ok &= Check(!answer["thumbnails"].contains("tone3000:4"), "a tone found to have no picture has a thumbnail");

    // The next session (a service of its own) reads what the lookups found from the cache.
    guitarfx::ResourceLibrary library;
    library.AddResource(controller.GetResourceLibrary().LookupResource("nam", "tone3000:3").value());
    library.AddResource(controller.GetResourceLibrary().LookupResource("nam", "tone3000:4").value());
    std::string sent;
    Service next([&sent](const std::string& message) { sent = message; }, library);
    guitarfx::MessageHandlerRegistry registry;
    next.RegisterMessageHandlers(registry);
    registry.Dispatch("getResourceThumbnails", {{"type", "getResourceThumbnails"}, {"resourceType", "nam"}});
    answer = nlohmann::json::parse(sent, nullptr, false);
    ok &= Check(answer.is_object() && answer["lookup"].empty() &&
                    answer["thumbnails"].value("tone3000:3", "") == Service::KeyFor(kOtherPicture),
                "the next session does not remember the lookups: " + sent);
    return ok;
}

bool TestOnlyPicturesAreStored()
{
    const auto sandbox = FreshSandbox("refusals");
    RecordingHost host(sandbox);
    guitarfx::PluginController controller(host);
    const auto key = Service::KeyFor(kPicture);
    const auto save = [&](const std::string& imageUrl, const std::string& data) {
        controller.HandleUIMessage(
            nlohmann::json{{"type", "saveResourceThumbnail"}, {"imageUrl", imageUrl}, {"data", data}}.dump());
    };

    save(kPicture, guitarfx::util::EncodeBase64(std::vector<std::uint8_t>{'<', 'h', 't', 'm', 'l', '>'}));
    save(kPicture, guitarfx::util::EncodeBase64(std::vector<std::uint8_t>(Service::kMaxBytes + 1, 0xFF)));
    save("file:///C:/Windows/win.ini", Jpeg());
    // Malformed payloads are ignored, not thrown on.
    controller.HandleUIMessage(R"({"type":"saveResourceThumbnail","imageUrl":7,"data":[1,2]})");
    controller.HandleUIMessage(R"({"type":"getResourceThumbnails","resourceType":"nam","resourceIds":"x"})");

    bool ok = Check(!fs::exists(Service::CacheDirectory() / (key + ".jpg")), "a page or an oversized file was stored");
    ok &= Check(!Service::ResolveUiResource("resource-thumbnails/../tones.json"), "the route serves outside names");
    ok &= Check(!Service::ResolveUiResource("resource-thumbnails/tones.json"), "the route serves the index");
    ok &= Check(!Service::ResolveUiResource("layout-images/" + key + ".jpg"), "the route serves another prefix");
    ok &= Check(Service::FetchUrlFor("https://example.com/a.jpg") == "https://example.com/a.jpg",
                "a picture elsewhere is not fetched as it is");
    return ok;
}
} // namespace

int main()
{
    int passed = 0;
    int failed = 0;

    const auto run = [&](const char* name, bool ok) {
        std::cout << (ok ? "[PASS] " : "[FAIL] ") << name << "\n";
        (ok ? passed : failed)++;
    };

    run("A recorded picture is fetched once, then served from the cache",
        TestRecordedPictureIsFetchedThenServedFromTheCache());
    run("A tone with no recorded picture is looked up once", TestLookupIsListedAndItsResultRemembered());
    run("Only pictures of a sane size are stored", TestOnlyPicturesAreStored());

    std::cout << "\nResource thumbnail tests: " << passed << " passed, " << failed << " failed\n";
    return failed == 0 ? 0 : 1;
}

/**
 * ResourceThumbnailClientTests.cpp - The native UIs' half of the resource browser's thumbnails
 * (uiclient/ResourceThumbnails.h), end to end against a headless engine: a recorded picture is
 * downloaded at the engine's sized URL and stored, a tone with no recorded picture is found on
 * the relay by its title and then stored too, with no credentials on any request, and a second
 * session finds both in the cache without asking the network again.
 */

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <iostream>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "IPluginHost.h"
#include "PluginController.h"
#include "controller/ResourceThumbnailService.h"
#include "net/Http.h"
#include "resources/ResourceLibrary.h"
#include "uiclient/ResourceThumbnails.h"
#include "uiclient/ToneService.h"
#include "uiclient/UiClient.h"
#include "uiclient/UiCommands.h"

namespace fs = std::filesystem;
using namespace guitarfx;
using namespace guitarfx::uiclient;

namespace
{
int gFailures = 0;

void Expect(bool condition, const std::string& what)
{
    if (!condition)
    {
        std::cerr << "FAIL: " << what << std::endl;
        ++gFailures;
    }
}

const std::string kPicture = "https://api.tone3000.com/storage/v1/object/public/images/abc123.jpg";
const std::string kFoundPicture = "https://api.tone3000.com/storage/v1/object/public/images/found.png";

class LoopbackHost final : public IPluginHost
{
  public:
    explicit LoopbackHost(fs::path root) : mRoot(std::move(root))
    {
    }

    void SendMessageToUI(const std::string& json) override
    {
        if (client != nullptr)
        {
            client->Enqueue(json);
        }
    }

    void BrowseFileAsync(BrowseFileType, const std::string&, std::function<void(const BrowseFileResult&)> cb) override
    {
        cb(BrowseFileResult{});
    }

    void SaveFileAsync(BrowseFileType, const std::string&, const std::string&,
                       std::function<void(const BrowseFileResult&)> cb) override
    {
        cb(BrowseFileResult{});
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
        return 256;
    }

    [[nodiscard]] bool IsStandalone() const override
    {
        return true;
    }

    UiClient* client = nullptr;

  private:
    fs::path mRoot;
};

/// Answers from a table by URL (404 otherwise), and remembers every request.
struct FakeNetwork final : net::HttpTransport
{
    std::mutex mutex;
    std::map<std::string, net::HttpResponse> answers;
    std::vector<net::HttpRequest> requests;

    net::HttpResponse Perform(const net::HttpRequest& request, net::CancelToken&) override
    {
        const std::lock_guard<std::mutex> lock(mutex);
        requests.push_back(request);
        const auto it = answers.find(request.url);
        return it != answers.end() ? it->second : net::HttpResponse{404, "", {}, "", false};
    }

    std::vector<std::string> Urls()
    {
        const std::lock_guard<std::mutex> lock(mutex);
        std::vector<std::string> urls;

        for (const auto& request : requests)
        {
            urls.push_back(request.url);
        }

        return urls;
    }
};

fs::path Sandbox()
{
    const auto root = fs::temp_directory_path() / "guitarfx-resource-thumbnail-client-tests";
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root, ec);
#ifdef _WIN32
    _putenv_s("APPDATA", root.string().c_str());
#else
    setenv("HOME", root.string().c_str(), 1);
#endif
    return root;
}

std::string Bytes(std::initializer_list<unsigned char> bytes)
{
    return std::string(bytes.begin(), bytes.end());
}

/// One UI session against `controller`: a client, its commands and a tone service on `network`.
struct Session
{
    Session(LoopbackHost& host, PluginController& controller, std::shared_ptr<FakeNetwork> network)
        : client([&controller](const std::string& json) { controller.HandleUIMessage(json); }), commands(client),
          tones(client, commands,
                ToneService::Host{[this](std::function<void()> fn) {
                                      const std::lock_guard<std::mutex> lock(postedMutex);
                                      posted.push_back(std::move(fn));
                                  },
                                  [](const std::string&, const std::string&, bool) {}},
                network),
          fetcher(client, tones)
    {
        host.client = &client;
        subscription = client.Subscribe(Topic::Thumbnails, [this] { fetcher.Update(); });
        listener = tones.AddListener([this] { fetcher.Update(); });
    }

    /// Runs what the workers posted and what the engine sent until `done`, or five seconds.
    bool PumpUntil(const std::function<bool()>& done)
    {
        const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(5);

        while (std::chrono::steady_clock::now() < until)
        {
            std::vector<std::function<void()>> work;
            {
                const std::lock_guard<std::mutex> lock(postedMutex);
                work.swap(posted);
            }

            for (auto& fn : work)
            {
                fn();
            }

            client.DrainPending();

            if (done())
            {
                return true;
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }

        return false;
    }

    std::mutex postedMutex;
    std::vector<std::function<void()>> posted;
    UiClient client;
    UiCommands commands;
    ToneService tones;
    ResourceThumbnailFetcher fetcher;
    Subscription subscription;
    int listener = 0;
};

void AddResource(PluginController& controller, const std::string& id, std::map<std::string, std::string> metadata)
{
    guitarfx::LibraryResource resource;
    resource.type = "nam";
    resource.id = id;
    resource.name = id;
    resource.metadata = std::move(metadata);
    controller.GetResourceLibrary().AddResource(resource);
}

void TestNativeClientFillsTheCache()
{
    const auto root = Sandbox();
    LoopbackHost host(root);
    PluginController controller(host);
    AddResource(controller, "tone3000:1", {{"provider", "tone3000"}, {"toneId", "10"}, {"imageUrl", kPicture}});
    AddResource(controller, "tone3000:2", {{"provider", "tone3000"}, {"toneId", "20"}, {"toneTitle", "Twin Pack"}});

    const auto jpeg = Bytes({0xFF, 0xD8, 0xFF, 0xE0, 0, 16, 'J', 'F', 'I', 'F'});
    const auto png = Bytes({0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A, 0, 0, 0, 13});
    const auto search = tone3000::Url(tone3000::TonesSearchPath("Twin Pack", "", "", "", 1));
    auto network = std::make_shared<FakeNetwork>();
    network->answers[ResourceThumbnailService::FetchUrlFor(kPicture)] = {200, jpeg, {}, "", false};
    network->answers[ResourceThumbnailService::FetchUrlFor(kFoundPicture)] = {200, png, {}, "", false};
    network->answers[search] = {
        200,
        nlohmann::json{
            {"data", nlohmann::json::array({{{"id", 21}, {"title", "Twin Pack II"}},
                                            {{"id", 20}, {"title", "Twin Pack"}, {"images", {kFoundPicture}}}})},
            {"page", 1},
            {"total_pages", 1}}
            .dump(),
        {},
        "",
        false};

    const auto cached = [](const std::string& url, const char* extension) {
        return fs::exists(ResourceThumbnailService::CacheDirectory() /
                          (ResourceThumbnailService::KeyFor(url) + extension));
    };

    {
        Session session(host, controller, network);
        session.fetcher.Request("nam", {"tone3000:1", "tone3000:2"});
        Expect(session.PumpUntil([&] { return cached(kPicture, ".jpg") && cached(kFoundPicture, ".png"); }),
               "the recorded and the looked-up pictures are both stored in the cache");

        const auto* found = session.fetcher.Find("nam", "tone3000:2");
        Expect(found != nullptr && found->imageUrl == kFoundPicture, "the looked-up tone shows the picture found");

        bool credentials = false;

        for (const auto& request : network->requests)
        {
            for (const auto& [name, value] : request.headers)
            {
                credentials |= name == "Authorization" || name == "x-api-key" || name == "x-session-id";
            }
        }

        Expect(!credentials, "no request carries credentials");
        host.client = nullptr;
    }

    // The next session: everything is on disk, so nothing is fetched or looked up.
    const auto asked = network->Urls().size();
    Session next(host, controller, network);
    next.fetcher.Request("nam", {"tone3000:1", "tone3000:2"});
    Expect(next.PumpUntil([&] {
        const auto* a = next.fetcher.Find("nam", "tone3000:1");
        const auto* b = next.fetcher.Find("nam", "tone3000:2");
        return a != nullptr && b != nullptr && !a->path.empty() && !b->path.empty();
    }),
           "the next session finds both thumbnails as cached files");
    Expect(network->Urls().size() == asked, "the next session asks the network for nothing");
    host.client = nullptr;
}
} // namespace

int main()
{
    TestNativeClientFillsTheCache();
    std::cout << (gFailures == 0 ? "Resource thumbnail client tests passed" : "Resource thumbnail client tests FAILED")
              << std::endl;
    return gFailures == 0 ? 0 : 1;
}

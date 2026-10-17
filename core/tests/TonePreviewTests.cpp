/**
 * TonePreviewTests.cpp - Auditioning a shared tone without installing it
 * (controller/PluginControllerTonePreview.cpp, "previewPreset").
 *
 * The files a preview sends must play, in every scene, from a folder of the preview's own, and
 * never reach the library; any other path the shared preset carries is dropped; and the next
 * preview takes the last one's files away.
 */

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "IPluginHost.h"
#include "PluginController.h"
#include "presets/PresetStorage.h"
#include "resources/ResourceLibrary.h"
#include "util/Base64.h"

namespace fs = std::filesystem;
using guitarfx::GraphNode;
using guitarfx::Preset;

namespace
{
class TestHost final : public guitarfx::IPluginHost
{
  public:
    explicit TestHost(fs::path root) : mRoot(std::move(root))
    {
    }

    void SendMessageToUI(const std::string& json) override
    {
        sent.push_back(json);
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
        return 256;
    }

    std::vector<std::string> sent;

  private:
    fs::path mRoot;
};

int gFailures = 0;

void Expect(bool condition, const std::string& what)
{
    if (!condition)
    {
        std::cerr << "FAIL: " << what << std::endl;
        ++gFailures;
    }
}

/// A folder of this run's own: a fixed one collides with another checkout running the suite.
fs::path Sandbox(const std::string& name)
{
    static const auto run = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count() % 100000000);
    const auto root = fs::temp_directory_path() / ("guitarfx-tone-preview-tests-" + run) / name;
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

GraphNode Node(const std::string& id, const std::string& type)
{
    GraphNode node;
    node.id = id;
    node.type = type;
    return node;
}

/// Input, an amp naming a Tone3000 model, a cab carrying the sharer's own path, output; and a
/// second scene with the same amp, since a scene past the first can name a model too.
Preset SharedPreset()
{
    Preset preset;
    preset.id = "shared-1";
    preset.name = "Shared Lead";
    preset.version = 2;

    auto amp = Node("amp_1", "utility");
    amp.resources.push_back({"nam", "tone3000:42"});
    auto cab = Node("cab_1", "utility");
    guitarfx::ResourceRef sharerPath;
    sharerPath.resourceType = "ir";
    sharerPath.resourceId = "sharers-cab";
    sharerPath.filePath = fs::path(R"(\\attacker.example\share\cab.wav)");
    cab.resources.push_back(sharerPath);

    preset.graph.nodes = {Node("__input__", guitarfx::kNodeTypeInput), amp, cab,
                          Node("__output__", guitarfx::kNodeTypeOutput)};
    preset.graph.edges = {
        {"__input__", "amp_1", 0, 0, 1.0}, {"amp_1", "cab_1", 0, 0, 1.0}, {"cab_1", "__output__", 0, 0, 1.0}};
    preset.scenes = {{"scene-a", "A", preset.graph}, {"scene-b", "B", preset.graph}};
    return preset;
}

nlohmann::json PreviewMessage(const Preset& preset, const std::string& name, const std::string& data)
{
    return {
        {"type", "previewPreset"},
        {"preset", nlohmann::json::parse(guitarfx::PresetStorage::SerializeToJson(preset))},
        {"presetId", "preview-item-1"},
        {"resources", {{{"resourceType", "nam"}, {"resourceId", "tone3000:42"}, {"name", name}, {"data", data}}}},
    };
}

const guitarfx::ResourceRef* FindRef(const guitarfx::SignalGraph& graph, const std::string& nodeId)
{
    const auto* node = graph.FindNode(nodeId);
    return node && !node->resources.empty() ? &node->resources.front() : nullptr;
}

bool SentType(const TestHost& host, const std::string& type)
{
    for (const auto& message : host.sent)
    {
        const auto parsed = nlohmann::json::parse(message, nullptr, false);

        if (parsed.is_object() && parsed.value("type", std::string{}) == type)
        {
            return true;
        }
    }

    return false;
}

std::string ReadText(const fs::path& path)
{
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

void TestPreviewPlaysStagedFilesOutsideTheLibrary()
{
    TestHost host(Sandbox("staged"));
    guitarfx::PluginController controller(host);
    controller.Initialize();
    const auto librarySize = controller.GetResourceLibrary().GetAllResources().size();

    const std::string model = R"({"version":"0.5.0","architecture":"Linear"})";
    controller.HandleUIMessage(
        PreviewMessage(SharedPreset(), "Plexi Crunch", guitarfx::util::EncodeBase64({model.begin(), model.end()}))
            .dump());

    const auto& active = controller.GetActivePreset();
    Expect(active.has_value() && active->name == "Shared Lead", "the preview is the preset playing");
    Expect(SentType(host, "presetLoaded"), "the load is answered with presetLoaded");

    if (!active)
    {
        return;
    }

    const auto* amp = FindRef(active->graph, "amp_1");
    Expect(amp && amp->resourceId == "tone3000:42", "the amp keeps naming the model by its id");
    Expect(amp && amp->filePath.filename() == "Plexi_Crunch.nam", "the staged file carries the model's name");
    Expect(amp && amp->filePath.parent_path().parent_path().filename() == "tone-preview",
           "the file is in a folder of the preview's own under temp/tone-preview");
    Expect(amp && ReadText(amp->filePath) == model, "the file holds the bytes the UI sent");

    for (const auto& scene : active->scenes)
    {
        const auto* sceneAmp = FindRef(scene.graph, "amp_1");
        Expect(sceneAmp && amp && sceneAmp->filePath == amp->filePath,
               "scene " + scene.id + " plays the staged file too");
    }

    const auto* cab = FindRef(active->graph, "cab_1");
    Expect(cab && cab->filePath.empty(), "the sharer's own path is dropped");
    Expect(!controller.GetResourceLibrary().HasResource("nam", "tone3000:42"), "the model never joins the library");
    Expect(controller.GetResourceLibrary().GetAllResources().size() == librarySize, "the library is unchanged");
}

void TestNextPreviewReplacesTheFiles()
{
    TestHost host(Sandbox("replace"));
    guitarfx::PluginController controller(host);
    controller.Initialize();

    controller.HandleUIMessage(PreviewMessage(SharedPreset(), "First", "AQID").dump());
    const auto* first = controller.GetActivePreset() ? FindRef(controller.GetActivePreset()->graph, "amp_1") : nullptr;
    const auto firstFolder = first ? first->filePath.parent_path() : fs::path{};
    Expect(!firstFolder.empty() && fs::exists(firstFolder), "the first preview stages its file");

    controller.HandleUIMessage(PreviewMessage(SharedPreset(), "Second", "BAUG").dump());
    const auto* second = controller.GetActivePreset() ? FindRef(controller.GetActivePreset()->graph, "amp_1") : nullptr;
    Expect(second && fs::exists(second->filePath), "the second preview stages its own");
    Expect(!firstFolder.empty() && !fs::exists(firstFolder), "the first preview's folder is gone");
}
} // namespace

int main()
{
    TestPreviewPlaysStagedFilesOutsideTheLibrary();
    TestNextPreviewReplacesTheFiles();

    if (gFailures > 0)
    {
        std::cerr << gFailures << " check(s) failed" << std::endl;
        return 1;
    }

    std::cout << "TonePreviewTests passed" << std::endl;
    return 0;
}

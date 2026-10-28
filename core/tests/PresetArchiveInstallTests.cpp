/**
 * PresetArchiveInstallTests.cpp - Installing tones downloaded from tone sharing, and taking
 * them out again (controller/PluginControllerArchiveInstall.cpp).
 *
 * Each test builds real zips in the web UI's archive format (preset.json or presets.json plus
 * resources/<file>), sends them to a headless controller as Soundshed Nano Q does, and
 * checks the library, the presets, the folders and the toneSharing.installedPacks entry the
 * web UI reads. The same archives are previewed too (previewPresetArchive,
 * controller/PluginControllerTonePreview.cpp), which must write none of those.
 */

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

#include <miniz.h>
#include <nlohmann/json.hpp>

#include "IPluginHost.h"
#include "PluginController.h"
#include "controller/internal/PresetArchiveSupport.h"
#include "controller/internal/RemoteResourceRefs.h"
#include "presets/PresetStorage.h"
#include "resources/ResourceLibrary.h"
#include "util/Base64.h"

namespace fs = std::filesystem;
using Bytes = std::vector<std::uint8_t>;
using guitarfx::GraphEdge;
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

    [[nodiscard]] bool IsStandalone() const override
    {
        return true;
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

std::optional<nlohmann::json> Latest(const TestHost& host, const std::string& type)
{
    for (auto it = host.sent.rbegin(); it != host.sent.rend(); ++it)
    {
        auto parsed = nlohmann::json::parse(*it, nullptr, false);

        if (parsed.is_object() && parsed.value("type", std::string{}) == type)
        {
            return parsed;
        }
    }

    return std::nullopt;
}

void Send(guitarfx::PluginController& controller, const std::string& type, nlohmann::json payload = {})
{
    if (!payload.is_object())
    {
        payload = nlohmann::json::object();
    }

    payload["type"] = type;
    controller.HandleUIMessage(payload.dump());
}

fs::path Sandbox(const std::string& name)
{
    static const auto run = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count() % 100000000);
    const auto root = fs::temp_directory_path() / ("guitarfx-archive-install-tests-" + run) / name;
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

Bytes BuildZip(const std::vector<std::pair<std::string, Bytes>>& entries)
{
    mz_zip_archive archive{};

    if (!mz_zip_writer_init_heap(&archive, 0, 0))
    {
        return {};
    }

    for (const auto& [name, data] : entries)
    {
        mz_zip_writer_add_mem(&archive, name.c_str(), data.empty() ? nullptr : data.data(), data.size(),
                              MZ_DEFAULT_COMPRESSION);
    }

    void* buffer = nullptr;
    std::size_t size = 0;
    Bytes zip;

    if (mz_zip_writer_finalize_heap_archive(&archive, &buffer, &size))
    {
        zip.assign(static_cast<std::uint8_t*>(buffer), static_cast<std::uint8_t*>(buffer) + size);
    }

    mz_zip_writer_end(&archive);
    return zip;
}

Bytes ToBytes(const std::string& text)
{
    return Bytes(text.begin(), text.end());
}

/// Input, one NAM node playing `resourceId` (with a blend id if given), output.
Preset BuildPreset(const std::string& id, const std::string& name, const std::string& resourceId,
                   const std::string& blendId = {})
{
    Preset preset;
    preset.id = id;
    preset.name = name;
    preset.category = "Shared";
    preset.version = 2;

    GraphNode input;
    input.id = "__input__";
    input.type = guitarfx::kNodeTypeInput;
    GraphNode amp;
    amp.id = "amp_1";
    amp.type = "amp_nam_optimized";
    amp.resources.push_back({"nam", resourceId});

    if (!blendId.empty())
    {
        amp.config["blendId"] = blendId;
    }

    GraphNode output;
    output.id = "__output__";
    output.type = guitarfx::kNodeTypeOutput;
    preset.graph.nodes = {input, amp, output};
    preset.graph.edges = {GraphEdge{"__input__", "amp_1", 0, 0, 1.0}, GraphEdge{"amp_1", "__output__", 0, 0, 1.0}};
    preset.globalSignalChain = guitarfx::GlobalSignalChainConfig{};
    return preset;
}

nlohmann::json PresetJson(const Preset& preset)
{
    return nlohmann::json::parse(guitarfx::PresetStorage::SerializeToJson(preset));
}

/// A one-preset archive, as the web UI shares one: the model under its hash-prefixed name.
Bytes SinglePresetArchive(const Preset& preset, const std::string& resourceId, const Bytes& model,
                          const nlohmann::json& extra = nlohmann::json::object())
{
    nlohmann::json root = {
        {"formatVersion", 1},
        {"preset", PresetJson(preset)},
        {"resources",
         {{{"id", resourceId},
           {"name", "Shared Model"},
           {"category", "amp"},
           {"type", "nam"},
           {"fileName", resourceId + "-model.nam"},
           {"hash", resourceId}}}},
        {"blends", nlohmann::json::array()},
    };

    for (const auto& [key, value] : extra.items())
    {
        root[key] = value;
    }

    return BuildZip({{"preset.json", ToBytes(root.dump())}, {"resources/" + resourceId + "-model.nam", model}});
}

nlohmann::json Archive(const Bytes& zip, const std::string& title)
{
    return {{"data", guitarfx::util::EncodeBase64(zip)}, {"title", title}};
}

std::optional<Preset> StoredPreset(TestHost& host, guitarfx::PluginController& controller, const std::string& id)
{
    const auto before = host.sent.size();
    Send(controller, "getPresetById", {{"presetId", id}});
    std::optional<nlohmann::json> reply;

    for (auto i = before; i < host.sent.size(); ++i)
    {
        auto parsed = nlohmann::json::parse(host.sent[i], nullptr, false);

        if (parsed.is_object() && parsed.value("type", "") == "presetData" &&
            parsed.value("requestedPresetId", "") == id)
        {
            reply = parsed;
        }
    }

    if (!reply || !reply->contains("preset"))
    {
        return std::nullopt;
    }

    return guitarfx::PresetStorage::DeserializeFromJson((*reply)["preset"].dump());
}

nlohmann::json InstalledEntries(const guitarfx::PluginController& controller)
{
    return controller.GetAppSettings().value("toneSharing.installedPacks", nlohmann::json::array());
}

std::string ActivePresetId(const guitarfx::PluginController& controller)
{
    return controller.GetActivePreset() ? controller.GetActivePreset()->id : std::string{};
}

std::size_t LibraryCount(guitarfx::PluginController& controller, const std::string& type)
{
    std::size_t count = 0;

    for (const auto& resource : controller.GetResourceLibrary().GetAllResources())
    {
        count += resource.type == type ? 1 : 0;
    }

    return count;
}

const Bytes kModelA = ToBytes(
    R"({"version":"0.5.0","architecture":"Linear","config":{"receptive_field":2,"bias":false},"weights":[1.0,0.0]})");
const Bytes kModelB = ToBytes(
    R"({"version":"0.5.0","architecture":"Linear","config":{"receptive_field":2,"bias":false},"weights":[0.5,0.0]})");

void TestInstallOnePreset()
{
    TestHost host(Sandbox("one"));
    guitarfx::PluginController controller(host);
    controller.Initialize();
    const auto activeBefore = ActivePresetId(controller);
    const auto modelsBefore = LibraryCount(controller, "nam");

    const auto zip = SinglePresetArchive(BuildPreset("shared-1", "Original Name", "sha-a"), "sha-a", kModelA);
    host.sent.clear();
    Send(controller, "installPresetArchives",
         {{"requestId", "r1"},
          {"entry", {{"id", "tone-sharing-api:item:item-1"}, {"title", "Shared Crunch"}, {"source", "toneSharingApi"}}},
          {"archives", {Archive(zip, "Shared Crunch")}}});

    const auto reply = Latest(host, "presetArchivesInstalled");
    Expect(reply.has_value(), "an install is answered with presetArchivesInstalled");

    if (!reply)
    {
        return;
    }

    Expect(reply->value("requestId", "") == "r1", "the reply carries the request id");
    const auto presetIds = reply->value("presetIds", nlohmann::json::array());
    Expect(presetIds.size() == 1, "one preset installed");
    Expect(LibraryCount(controller, "nam") == modelsBefore + 1, "its model joins the library");
    Expect(ActivePresetId(controller) == activeBefore, "the running preset is left alone");
    Expect(Latest(host, "presetList").has_value(), "the UI gets the new preset list");

    const auto id = presetIds.empty() ? std::string{} : presetIds[0].get<std::string>();
    Expect(id != "shared-1" && id.rfind("user-", 0) == 0, "the preset gets a new user id");

    if (const auto stored = StoredPreset(host, controller, id))
    {
        Expect(stored->name == "Shared Crunch", "a one-preset archive takes the shared item's title");
        Expect(!stored->globalSignalChain.has_value(), "the sharer's global chain is not installed");

        const auto& amp = stored->graph.nodes[1];
        const auto resourceId = amp.resources.empty() ? std::string{} : amp.resources[0].resourceId;
        const auto resource = controller.GetResourceLibrary().LookupResource("nam", resourceId);
        Expect(resourceId != "sha-a" && resource.has_value(), "the preset plays the library's copy of the model");

        if (resource)
        {
            Expect(fs::exists(resource->filePath), "whose file was written");
        }
    }
    else
    {
        Expect(false, "the installed preset can be read back");
    }

    const auto entries = InstalledEntries(controller);
    Expect(entries.size() == 1 && entries[0].value("id", "") == "tone-sharing-api:item:item-1",
           "the install is recorded in toneSharing.installedPacks");

    if (!entries.empty())
    {
        Expect(entries[0].value("presetIds", nlohmann::json::array()) == presetIds, "with its preset ids");
        Expect(entries[0].value("resources", nlohmann::json::array()).size() == 1, "and the model it added");
        Expect(entries[0].value("source", "") == "toneSharingApi" && entries[0].contains("importedAt"),
               "in the web UI's schema");
    }

    const auto changed = Latest(host, "appSettingChanged");
    Expect(changed && changed->value("key", "") == "toneSharing.installedPacks",
           "and the UI is told of the new setting");
}

// The web UI writes node ids into markup, and a resource path would have the engine open any file
// the sharer names, a network share included.
void TestHostileNodeIdsAndPathsAreCleaned()
{
    TestHost host(Sandbox("hostile"));
    guitarfx::PluginController controller(host);
    controller.Initialize();

    const std::string hostileId = "amp\"><img src=x onerror=alert(1)>";
    auto preset = BuildPreset("shared-hostile", "Hostile", "sha-h");
    preset.graph.nodes[1].id = hostileId;
    preset.graph.nodes[1].resources.push_back({"ir", "", R"(\\attacker.example\share\cab.wav)"});
    preset.graph.edges = {GraphEdge{"__input__", hostileId, 0, 0, 1.0}, GraphEdge{hostileId, "__output__", 0, 0, 1.0},
                          GraphEdge{"__input__", "no such <node>", 0, 0, 1.0}};
    preset.scenes.push_back({"scene-b", "B", preset.graph});

    Send(controller, "installPresetArchives",
         {{"requestId", "r-hostile"},
          {"entry", {{"id", "tone-sharing-api:item:hostile"}, {"title", "Hostile"}, {"source", "toneSharingApi"}}},
          {"archives", {Archive(SinglePresetArchive(preset, "sha-h", kModelA), "Hostile")}}});

    const auto reply = Latest(host, "presetArchivesInstalled");
    const auto presetIds = reply ? reply->value("presetIds", nlohmann::json::array()) : nlohmann::json::array();
    Expect(presetIds.size() == 1, "the preset with a hostile node id still installs");

    if (presetIds.size() != 1)
    {
        return;
    }

    const auto stored = StoredPreset(host, controller, presetIds[0].get<std::string>());
    Expect(stored.has_value(), "the cleaned preset can be read back");

    if (!stored)
    {
        return;
    }

    // Found by its model: the stored type is the canonical effect id, not the alias it was saved under.
    const auto isAmp = [](const GraphNode& node) {
        return std::any_of(node.resources.begin(), node.resources.end(),
                           [](const guitarfx::ResourceRef& resource) { return resource.resourceType == "nam"; });
    };
    const auto amp = std::find_if(stored->graph.nodes.begin(), stored->graph.nodes.end(), isAmp);
    Expect(amp != stored->graph.nodes.end(), "the amp node survives");

    if (amp == stored->graph.nodes.end())
    {
        return;
    }

    const std::string newId = amp->id;
    Expect(guitarfx::controller_detail::IsSafeImportedNodeId(newId) && newId != hostileId, "the node gets a safe id");
    Expect(std::count_if(stored->graph.edges.begin(), stored->graph.edges.end(),
                         [&](const GraphEdge& edge) { return edge.from == newId || edge.to == newId; }) == 2,
           "both of its edges follow it");

    for (const auto& edge : stored->graph.edges)
    {
        Expect(edge.to != "no such <node>", "an edge to no node is dropped");
    }

    for (const auto& resource : amp->resources)
    {
        Expect(resource.filePath.empty(), "no resource keeps the archive's file path");
    }

    const auto scene = std::find_if(stored->scenes.begin(), stored->scenes.end(),
                                    [](const guitarfx::PresetScene& s) { return s.id == "scene-b"; });
    Expect(scene != stored->scenes.end() && std::any_of(scene->graph.nodes.begin(), scene->graph.nodes.end(),
                                                        [&](const GraphNode& node) { return node.id == newId; }),
           "the scene's copy of the node gets the same new id");
}

void TestSecondInstallReusesTheModel()
{
    TestHost host(Sandbox("reuse"));
    guitarfx::PluginController controller(host);
    controller.Initialize();

    const auto zip = SinglePresetArchive(BuildPreset("shared-1", "A", "sha-a"), "sha-a", kModelA);
    Send(controller, "installPresetArchives",
         {{"entry", {{"id", "tone-sharing-api:item:one"}, {"title", "One"}}}, {"archives", {Archive(zip, "One")}}});
    const auto models = LibraryCount(controller, "nam");

    host.sent.clear();
    Send(controller, "installPresetArchives",
         {{"entry", {{"id", "tone-sharing-api:item:two"}, {"title", "Two"}}}, {"archives", {Archive(zip, "Two")}}});

    Expect(LibraryCount(controller, "nam") == models, "the same model is not copied into the library twice");
    const auto entries = InstalledEntries(controller);
    Expect(entries.size() == 2 && entries[0].value("id", "") == "tone-sharing-api:item:two",
           "the newest install is listed first");

    if (!entries.empty())
    {
        Expect(entries[0].value("resources", nlohmann::json::array()).empty(),
               "a model the library already had is not counted as this install's");
    }
}

void TestReusesAnOlderImportOfTheSameFile()
{
    TestHost host(Sandbox("older-import"));
    guitarfx::PluginController controller(host);
    controller.Initialize();

    // An earlier import of the same file, recorded with a hash of another kind (the browser's
    // SHA-256, as older web UI imports stored it).
    const auto folder = fs::path(host.GetUserDataPath()) / "older";
    fs::create_directories(folder);
    const auto path = folder / "sha-a-model.nam";
    {
        std::ofstream out(path, std::ios::binary);
        out.write(reinterpret_cast<const char*>(kModelA.data()), static_cast<std::streamsize>(kModelA.size()));
    }

    guitarfx::LibraryResource older;
    older.type = "nam";
    older.id = "older-model";
    older.name = "Older";
    older.filePath = path;
    older.hash = "sha-a";
    controller.GetResourceLibrary().AddResource(older);
    const auto models = LibraryCount(controller, "nam");

    const auto zip = SinglePresetArchive(BuildPreset("p", "Again", "sha-a"), "sha-a", kModelA);
    host.sent.clear();
    Send(controller, "installPresetArchives",
         {{"entry", {{"id", "tone-sharing-api:item:again"}}}, {"archives", {Archive(zip, "Again")}}});

    Expect(LibraryCount(controller, "nam") == models,
           "a file the library has under its archive name, same bytes, is reused");
    const auto reply = Latest(host, "presetArchivesInstalled");
    const auto ids = reply ? reply->value("presetIds", nlohmann::json::array()) : nlohmann::json::array();
    const auto preset = ids.empty() ? std::nullopt : StoredPreset(host, controller, ids[0].get<std::string>());
    Expect(preset && preset->graph.nodes[1].resources[0].resourceId == "older-model", "and the preset plays it");
}

void TestPackGetsAFolder()
{
    TestHost host(Sandbox("pack"));
    guitarfx::PluginController controller(host);
    controller.Initialize();

    const auto first = SinglePresetArchive(BuildPreset("p1", "First", "sha-a"), "sha-a", kModelA);
    const auto second = SinglePresetArchive(BuildPreset("p2", "Second", "sha-b"), "sha-b", kModelB);
    host.sent.clear();
    Send(controller, "installPresetArchives",
         {{"entry", {{"id", "tone-sharing-api:pack-9"}, {"title", "Metal Pack"}, {"packId", "pack-9"}}},
          {"folder", "Metal Pack"},
          {"archives", {Archive(first, "Chug"), Archive(second, "Lead")}}});

    const auto reply = Latest(host, "presetArchivesInstalled");
    const auto presetIds = reply ? reply->value("presetIds", nlohmann::json::array()) : nlohmann::json::array();
    Expect(presetIds.size() == 2, "a pack installs every preset");

    const auto folders = Latest(host, "presetFolders");
    Expect(folders.has_value(), "the UI gets the new folders");

    bool found = false;

    for (const auto& folder : folders ? folders->value("folders", nlohmann::json::array()) : nlohmann::json::array())
    {
        if (folder.value("id", "") == "tone-sharing::pack-9")
        {
            found = true;
            Expect(folder.value("name", "") == "Metal Pack", "the folder is named after the pack");
            Expect(folder.value("presetIds", nlohmann::json::array()) == presetIds, "and holds its presets");
        }
    }

    Expect(found, "the pack gets a folder of its own");

    if (presetIds.size() == 2)
    {
        const auto chug = StoredPreset(host, controller, presetIds[0].get<std::string>());
        Expect(chug && chug->name == "Chug", "each preset takes its item's title");
    }

    const auto entries = InstalledEntries(controller);
    Expect(!entries.empty() && entries[0].value("packId", "") == "pack-9", "the entry records the pack id");
}

void TestMissingTone3000ModelsInstallNothing()
{
    TestHost host(Sandbox("tone3000"));
    guitarfx::PluginController controller(host);
    controller.Initialize();
    const auto models = LibraryCount(controller, "nam");

    const nlohmann::json extra = {
        {"tone3000Resources",
         {{{"id", "t3k-1"}, {"name", "Borrowed Amp"}, {"type", "nam"}, {"toneId", "123"}, {"modelId", "456"}}}}};
    const auto zip = SinglePresetArchive(BuildPreset("p", "Needs T3K", "sha-a"), "sha-a", kModelA, extra);
    host.sent.clear();
    Send(controller, "installPresetArchives",
         {{"requestId", "r2"},
          {"entry", {{"id", "tone-sharing-api:item:t3k"}}},
          {"archives", {Archive(zip, "Needs T3K")}}});

    const auto failed = Latest(host, "presetArchivesInstallFailed");
    Expect(failed.has_value() && failed->value("requestId", "") == "r2", "the install fails, answering the request");

    if (failed)
    {
        Expect(failed->value("detail", "").find("Borrowed Amp") != std::string::npos, "naming the model it needs");
    }

    Expect(!Latest(host, "presetArchivesInstalled").has_value(), "nothing reports success");
    Expect(LibraryCount(controller, "nam") == models, "no model was written");
    Expect(InstalledEntries(controller).empty(), "and nothing is recorded");
}

/// A remote reference from any provider resolves to the library's copy by provider and provider
/// key, even when the archive used another library id for it, and the preset is pointed there.
void TestRemoteResourceResolvesByProviderKey()
{
    TestHost host(Sandbox("remote-key"));
    guitarfx::PluginController controller(host);
    controller.Initialize();

    Send(controller, "importRemoteResource",
         {{"provider", "soundshed"},
          {"resourceType", "nam"},
          {"resourceId", "soundshed:open-amp-1"},
          {"name", "Open Amp"},
          {"fileName", "open-amp.nam"},
          {"metadata", {{"provider", "soundshed"}, {"providerKey", "open-amp-1"}, {"licence", "cc-by"}}},
          {"data", guitarfx::util::EncodeBase64(kModelB)}});
    Expect(controller.GetResourceLibrary().LookupResource("nam", "soundshed:open-amp-1").has_value(),
           "the remote model is in the library");

    const nlohmann::json extra = {{"remoteResources",
                                   {{{"id", "soundshed:renamed-on-the-sharers-machine"},
                                     {"type", "nam"},
                                     {"name", "Open Amp"},
                                     {"provider", "soundshed"},
                                     {"providerKey", "open-amp-1"},
                                     {"licence", "cc-by"}}}}};
    const auto zip = SinglePresetArchive(BuildPreset("p", "Open Tone", "soundshed:renamed-on-the-sharers-machine"),
                                         "sha-a", kModelA, extra);
    host.sent.clear();
    Send(controller, "installPresetArchives",
         {{"requestId", "r3"},
          {"entry", {{"id", "tone-sharing-api:item:open"}}},
          {"archives", {Archive(zip, "Open Tone")}}});

    const auto reply = Latest(host, "presetArchivesInstalled");
    Expect(reply.has_value(), "the install succeeds");
    const auto presetIds = reply ? reply->value("presetIds", nlohmann::json::array()) : nlohmann::json::array();

    if (presetIds.size() == 1)
    {
        if (const auto stored = StoredPreset(host, controller, presetIds[0].get<std::string>()))
        {
            const auto& amp = stored->graph.nodes[1];
            Expect(!amp.resources.empty() && amp.resources[0].resourceId == "soundshed:open-amp-1",
                   "the preset plays the library's copy");
        }
    }
}

void TestMissingRemoteModelsFromOtherProvidersInstallNothing()
{
    TestHost host(Sandbox("remote-missing"));
    guitarfx::PluginController controller(host);
    controller.Initialize();

    const nlohmann::json extra = {{"remoteResources",
                                   {{{"id", "soundshed:nowhere"},
                                     {"type", "nam"},
                                     {"name", "Open Amp"},
                                     {"provider", "soundshed"},
                                     {"providerKey", "nowhere"}}}}};
    const auto zip = SinglePresetArchive(BuildPreset("p", "Open Tone", "soundshed:nowhere"), "sha-a", kModelA, extra);
    host.sent.clear();
    Send(controller, "installPresetArchives",
         {{"requestId", "r4"},
          {"entry", {{"id", "tone-sharing-api:item:missing"}}},
          {"archives", {Archive(zip, "Open Tone")}}});

    const auto failed = Latest(host, "presetArchivesInstallFailed");
    Expect(failed.has_value(), "the install fails");

    if (failed)
    {
        const auto detail = failed->value("detail", "");
        Expect(detail.find("Open Amp") != std::string::npos && detail.find("soundshed") != std::string::npos,
               "naming the model and where it comes from");
    }

    Expect(InstalledEntries(controller).empty(), "and nothing is recorded");
}

/// An archive listing a Tone3000 model in both lists (as the web UI now writes them) needs it once.
void TestBothListsNameTheModelOnce()
{
    const nlohmann::json root = {
        {"remoteResources",
         {{{"id", "tone3000:456"}, {"type", "nam"}, {"provider", "tone3000"}, {"providerKey", "456"}}}},
        {"tone3000Resources",
         {{{"id", "tone3000:456"}, {"type", "nam"}, {"toneId", "123"}, {"modelId", "456"}},
          {{"id", "tone3000:789"}, {"type", "nam"}, {"toneId", "123"}, {"modelId", "789"}, {"name", "Legacy Only"}}}},
    };
    const auto refs = guitarfx::controller_detail::ReadArchiveRemoteResources(root);
    Expect(refs.size() == 2, "one entry per library id");

    if (refs.size() == 2)
    {
        Expect(refs[1].value("provider", "") == "tone3000" && refs[1].value("providerKey", "") == "789",
               "a legacy entry becomes a tone3000 reference");
        Expect(refs[1]["providerData"].value("toneId", "") == "123", "keeping its tone id");
    }
}

void TestUnreadableDownloadIsRefused()
{
    TestHost host(Sandbox("unreadable"));
    guitarfx::PluginController controller(host);
    controller.Initialize();
    host.sent.clear();
    Send(controller, "installPresetArchives",
         {{"entry", {{"id", "tone-sharing-api:item:bad"}}}, {"archives", {Archive(ToBytes("not a zip"), "Bad")}}});

    Expect(Latest(host, "presetArchivesInstallFailed").has_value(), "a download that is not an archive is refused");
    Expect(InstalledEntries(controller).empty(), "and not recorded");
}

void TestDeleteTakesOutWhatItAdded()
{
    TestHost host(Sandbox("delete"));
    guitarfx::PluginController controller(host);
    controller.Initialize();

    // The user already has model B; the pack's second preset reuses it.
    const auto own = SinglePresetArchive(BuildPreset("mine", "Mine", "sha-b"), "sha-b", kModelB);
    Send(controller, "installPresetArchives",
         {{"entry", {{"id", "zipImport:mine"}, {"title", "Mine"}}}, {"archives", {Archive(own, "Mine")}}});

    const auto first = SinglePresetArchive(BuildPreset("p1", "First", "sha-a"), "sha-a", kModelA);
    const auto second = SinglePresetArchive(BuildPreset("p2", "Second", "sha-b"), "sha-b", kModelB);
    host.sent.clear();
    Send(controller, "installPresetArchives",
         {{"entry", {{"id", "tone-sharing-api:pack-1"}, {"title", "Pack"}, {"packId", "pack-1"}}},
          {"folder", "Pack"},
          {"archives", {Archive(first, "One"), Archive(second, "Two")}}});

    const auto installed = Latest(host, "presetArchivesInstalled");
    const auto packPresetIds =
        installed ? installed->value("presetIds", nlohmann::json::array()) : nlohmann::json::array();
    const auto entries = InstalledEntries(controller);
    const auto addedModels =
        entries.empty() ? nlohmann::json::array() : entries[0].value("resources", nlohmann::json::array());
    Expect(addedModels.size() == 1, "the pack added only model A");

    const auto addedId = addedModels.empty() ? std::string{} : addedModels[0].value("id", "");
    const auto addedPath =
        controller.GetResourceLibrary().LookupResource("nam", addedId).value_or(guitarfx::LibraryResource{}).filePath;
    const auto modelsBefore = LibraryCount(controller, "nam");

    Send(controller, "setPresetFavorite",
         {{"presetId", packPresetIds.empty() ? "" : packPresetIds[0].get<std::string>()}, {"favorite", true}});

    host.sent.clear();
    Send(controller, "deleteInstalledPresetArchive", {{"id", "tone-sharing-api:pack-1"}});

    const auto deleted = Latest(host, "installedPresetArchiveDeleted");
    Expect(deleted.has_value() && deleted->value("presetIds", nlohmann::json::array()).size() == 2,
           "removing the pack deletes both its presets");

    for (const auto& id : packPresetIds)
    {
        Expect(!StoredPreset(host, controller, id.get<std::string>()).has_value(),
               "a removed preset is gone from the store");
    }

    Expect(LibraryCount(controller, "nam") == modelsBefore - 1, "the model only the pack used is removed");
    Expect(addedPath.empty() || !fs::exists(addedPath), "with its file");

    const auto remaining = InstalledEntries(controller);
    Expect(remaining.size() == 1 && remaining[0].value("id", "") == "zipImport:mine",
           "the entry is gone; the other stays");

    const auto folders = Latest(host, "presetFolders");
    bool folderLeft = false;

    for (const auto& folder : folders ? folders->value("folders", nlohmann::json::array()) : nlohmann::json::array())
    {
        folderLeft = folderLeft || folder.value("id", "") == "tone-sharing::pack-1";
    }

    Expect(folders.has_value() && !folderLeft, "and so is the pack's folder");

    const auto favorites = Latest(host, "presetFavorites");
    Expect(favorites && favorites->value("favorites", nlohmann::json::array()).empty(), "and its favourite");
}

/// The web UI installs even when a model it tried to fetch could not be had (its creator removed
/// or hid it): it says so, and the engine leaves that slot empty and names the model back.
void TestMissingRemoteModelsCanBeAllowed()
{
    TestHost host(Sandbox("remote-allowed"));
    guitarfx::PluginController controller(host);
    controller.Initialize();

    const nlohmann::json extra = {
        {"tone3000Resources",
         {{{"id", "t3k-gone"}, {"name", "Removed Amp"}, {"type", "nam"}, {"toneId", "1"}, {"modelId", "2"}}}}};
    const auto zip = SinglePresetArchive(BuildPreset("p", "Partly There", "sha-a"), "sha-a", kModelA, extra);
    host.sent.clear();
    Send(controller, "installPresetArchives",
         {{"requestId", "r-allow"},
          {"allowMissingRemote", true},
          {"entry",
           {{"id", "tone-sharing-api:item:partly"}, {"title", "Partly There"}, {"archiveFileName", "partly.zip"}}},
          {"archives", {Archive(zip, "Partly There")}}});

    const auto reply = Latest(host, "presetArchivesInstalled");
    Expect(reply.has_value() && !Latest(host, "presetArchivesInstallFailed").has_value(),
           "with allowMissingRemote the install goes ahead");

    if (reply)
    {
        const auto missing = reply->value("missingRemote", nlohmann::json::array());
        Expect(missing.size() == 1 && missing[0] == "Removed Amp", "and the reply names the model it lacks");
        Expect(reply->value("presetIds", nlohmann::json::array()).size() == 1, "the preset is installed");
    }

    const auto entries = InstalledEntries(controller);
    Expect(!entries.empty() && entries[0].value("archiveFileName", "") == "partly.zip",
           "the entry keeps the archive's file name for the Installed list");
}

/// Models the UI fetched for an install's remote references are recorded with it, so removing the
/// install takes them out again; a resource the library does not hold is not recorded.
void TestFetchedResourcesLeaveWithTheInstall()
{
    TestHost host(Sandbox("fetched"));
    guitarfx::PluginController controller(host);
    controller.Initialize();

    Send(controller, "importRemoteResource",
         {{"provider", "tone3000"},
          {"resourceType", "nam"},
          {"resourceId", "tone3000:456"},
          {"name", "Fetched Amp"},
          {"fileName", "fetched.nam"},
          {"metadata", {{"provider", "tone3000"}, {"providerKey", "456"}, {"toneId", "123"}, {"modelId", "456"}}},
          {"data", guitarfx::util::EncodeBase64(kModelB)}});

    const nlohmann::json extra = {{"remoteResources",
                                   {{{"id", "tone3000:456"},
                                     {"type", "nam"},
                                     {"name", "Fetched Amp"},
                                     {"provider", "tone3000"},
                                     {"providerKey", "456"},
                                     {"providerData", {{"toneId", "123"}, {"modelId", "456"}}}}}}};
    auto preset = BuildPreset("p", "Fetched", "sha-a");
    preset.graph.nodes[1].resources = {{"nam", "tone3000:456"}};
    const auto zip = SinglePresetArchive(preset, "sha-a", kModelA, extra);
    host.sent.clear();
    Send(
        controller, "installPresetArchives",
        {{"requestId", "r-fetched"},
         {"entry", {{"id", "tone-sharing-api:item:fetched"}, {"title", "Fetched"}}},
         {"fetchedResources", {{{"type", "nam"}, {"id", "tone3000:456"}}, {{"type", "nam"}, {"id", "not-in-library"}}}},
         {"archives", {Archive(zip, "Fetched")}}});

    Expect(Latest(host, "presetArchivesInstalled").has_value(), "the install succeeds");
    const auto entries = InstalledEntries(controller);
    const auto resources =
        entries.empty() ? nlohmann::json::array() : entries[0].value("resources", nlohmann::json::array());
    const auto recorded = [&](const std::string& id) {
        return std::any_of(resources.begin(), resources.end(),
                           [&](const nlohmann::json& r) { return r.value("id", "") == id; });
    };
    Expect(recorded("tone3000:456"), "the fetched model is recorded with the install");
    Expect(!recorded("not-in-library"), "a resource the library lacks is not");
    Expect(resources.size() == 2, "beside the archive's own model");

    Send(controller, "deleteInstalledPresetArchive", {{"id", "tone-sharing-api:item:fetched"}});
    Expect(!controller.GetResourceLibrary().LookupResource("nam", "tone3000:456").has_value(),
           "removing the install removes the model fetched for it");
}

/// A pack's own archive may lay its presets out in folders: they are rebuilt under the pack's folder,
/// and removing the pack takes the whole tree.
void TestPackArchiveFoldersAreKept()
{
    TestHost host(Sandbox("pack-folders"));
    guitarfx::PluginController controller(host);
    controller.Initialize();

    const nlohmann::json root = {
        {"formatVersion", 1},
        {"presets",
         {PresetJson(BuildPreset("p1", "Chug", "sha-a")), PresetJson(BuildPreset("p2", "Gallop", "sha-a")),
          PresetJson(BuildPreset("p3", "Solo", "sha-a"))}},
        {"resources",
         {{{"id", "sha-a"}, {"name", "Model"}, {"type", "nam"}, {"fileName", "sha-a-model.nam"}, {"hash", "sha-a"}}}},
        {"blends", nlohmann::json::array()},
        {"presetFolders",
         {{{"name", "Rhythm"}, {"presetIds", {"p1", "p2"}}}, {{"name", "Lead"}, {"presetIds", {"p3", "unknown"}}}}},
    };
    const auto zip = BuildZip({{"presets.json", ToBytes(root.dump())}, {"resources/sha-a-model.nam", kModelA}});
    host.sent.clear();
    Send(controller, "installPresetArchives",
         {{"entry", {{"id", "tone-sharing-api:pack-f"}, {"title", "Foldered"}, {"packId", "pack-f"}}},
          {"folder", "Foldered"},
          {"archives", {Archive(zip, "Foldered")}}});

    const auto reply = Latest(host, "presetArchivesInstalled");
    const auto presetIds = reply ? reply->value("presetIds", nlohmann::json::array()) : nlohmann::json::array();
    Expect(presetIds.size() == 3, "the pack's three presets install");

    const auto folders = Latest(host, "presetFolders");
    std::optional<nlohmann::json> pack;

    for (const auto& folder : folders ? folders->value("folders", nlohmann::json::array()) : nlohmann::json::array())
    {
        if (folder.value("id", "") == "tone-sharing::pack-f")
        {
            pack = folder;
        }
    }

    Expect(pack.has_value(), "the pack gets its folder");

    if (pack && presetIds.size() == 3)
    {
        const auto children = pack->value("children", nlohmann::json::array());
        Expect(pack->value("presetIds", nlohmann::json::array()).empty(),
               "every preset sits in one of the pack's subfolders");
        Expect(children.size() == 2 && children[0].value("name", "") == "Rhythm" &&
                   children[1].value("name", "") == "Lead",
               "the archive's folders are rebuilt under it");

        if (children.size() == 2)
        {
            Expect(children[0].value("presetIds", nlohmann::json::array()) ==
                       nlohmann::json::array({presetIds[0], presetIds[1]}),
                   "holding the installed presets by their new ids");
            Expect(children[1].value("presetIds", nlohmann::json::array()) == nlohmann::json::array({presetIds[2]}),
                   "and naming no preset the archive lacks");
        }
    }

    host.sent.clear();
    Send(controller, "deleteInstalledPresetArchive", {{"id", "tone-sharing-api:pack-f"}});
    const auto after = Latest(host, "presetFolders");
    bool left = false;

    for (const auto& folder : after ? after->value("folders", nlohmann::json::array()) : nlohmann::json::array())
    {
        left = left || folder.value("id", "") == "tone-sharing::pack-f";
    }

    Expect(after.has_value() && !left, "removing the pack drops its folder, emptied through its subfolders");
}

void TestDeletingAnUnknownInstallIsAnswered()
{
    TestHost host(Sandbox("delete-unknown"));
    guitarfx::PluginController controller(host);
    controller.Initialize();
    host.sent.clear();
    Send(controller, "deleteInstalledPresetArchive", {{"id", "tone-sharing-api:item:never"}});

    const auto deleted = Latest(host, "installedPresetArchiveDeleted");
    Expect(deleted && deleted->value("id", "") == "tone-sharing-api:item:never" &&
               deleted->value("presetIds", nlohmann::json::array()).empty(),
           "removing an install the engine does not know is still answered, removing nothing");
}
} // namespace

// A Custom Effect module is code. One in a shared archive must not reach the library, and the
// preset's Custom Effect node must come in with no module and no descriptor of the archive's making.
void TestCustomEffectModulesAreNotInstalled()
{
    TestHost host(Sandbox("custom-effect"));
    guitarfx::PluginController controller(host);
    controller.Initialize();

    auto preset = BuildPreset("shared-custom", "Custom", "sha-c");
    GraphNode fx;
    fx.id = "fx_1";
    fx.type = "wasm_host";
    fx.resources.push_back({"wasm", "their-module"});
    fx.config["wasmGuestDescriptor"] = R"([{"key":"effect.category","value":"<iframe src=x>"}])";
    fx.config["customEffectId"] = "theirs";
    preset.graph.nodes.insert(preset.graph.nodes.begin() + 2, fx);

    const nlohmann::json resources = nlohmann::json::array({
        {{"id", "sha-c"}, {"name", "Model"}, {"type", "nam"}, {"fileName", "sha-c-model.nam"}, {"hash", "sha-c"}},
        {{"id", "their-module"}, {"name", "Fx"}, {"type", "wasm"}, {"fileName", "fx.wasm"}, {"hash", "fx"}},
    });
    const Bytes zip = BuildZip({{"preset.json", ToBytes(nlohmann::json{{"formatVersion", 1},
                                                                       {"preset", PresetJson(preset)},
                                                                       {"resources", resources},
                                                                       {"blends", nlohmann::json::array()}}
                                                            .dump())},
                                {"resources/sha-c-model.nam", kModelA},
                                {"resources/fx.wasm", Bytes{0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00}}});

    Send(controller, "installPresetArchives",
         {{"requestId", "r-custom"},
          {"entry", {{"id", "tone-sharing-api:item:custom"}, {"title", "Custom"}, {"source", "toneSharingApi"}}},
          {"archives", {Archive(zip, "Custom")}}});

    Expect(LibraryCount(controller, "nam") == 1, "the archive's model still installs");
    Expect(LibraryCount(controller, "wasm") == 0, "the archive's Custom Effect module does not");

    const auto reply = Latest(host, "presetArchivesInstalled");
    const auto presetIds = reply ? reply->value("presetIds", nlohmann::json::array()) : nlohmann::json::array();
    const auto stored =
        presetIds.size() == 1 ? StoredPreset(host, controller, presetIds[0].get<std::string>()) : std::nullopt;
    Expect(stored.has_value(), "the preset with a Custom Effect still installs");

    if (stored)
    {
        const auto node = std::find_if(stored->graph.nodes.begin(), stored->graph.nodes.end(),
                                       [](const GraphNode& n) { return n.id == "fx_1"; });
        Expect(node != stored->graph.nodes.end(), "the Custom Effect node keeps its place");

        if (node != stored->graph.nodes.end())
        {
            Expect(std::none_of(node->resources.begin(), node->resources.end(),
                                [](const guitarfx::ResourceRef& r) { return !r.resourceId.empty(); }),
                   "the Custom Effect node names no module");
            Expect(!node->config.contains("wasmGuestDescriptor") && !node->config.contains("customEffectId"),
                   "the Custom Effect node carries no descriptor from the archive");
        }
    }

    // The web UI's own import path ends in importRemoteResource.
    Send(controller, "importRemoteResource",
         {{"requestId", "r-remote"},
          {"provider", "presetArchive"},
          {"resourceType", "wasm"},
          {"resourceId", "remote-module"},
          {"data", guitarfx::util::EncodeBase64(Bytes{0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00})}});

    Expect(LibraryCount(controller, "wasm") == 0, "a remote import of a Custom Effect module is refused");
    Expect(Latest(host, "resourceImportFailed").has_value(), "the refused import is reported as failed");
}

// ── Previewing a downloaded archive (previewPresetArchive) ──────────────────────

/// The node "amp_1"'s first resource in the running preset.
const guitarfx::ResourceRef* ActiveAmp(const guitarfx::PluginController& controller)
{
    const auto& active = controller.GetActivePreset();

    if (!active)
    {
        return nullptr;
    }

    for (const auto& node : active->graph.nodes)
    {
        if (node.id == "amp_1" && !node.resources.empty())
        {
            return &node.resources.front();
        }
    }

    return nullptr;
}

/// A preview of an archive the library knows nothing of: its model is staged in temp, the preset
/// plays under the preview's id, and the library, the presets and the installs are untouched.
void TestArchivePreviewWritesNothing()
{
    TestHost host(Sandbox("preview-archive"));
    guitarfx::PluginController controller(host);
    controller.Initialize();
    const auto models = LibraryCount(controller, "nam");
    const auto zip = SinglePresetArchive(BuildPreset("shared-p", "Original", "sha-p"), "sha-p", kModelA);

    host.sent.clear();
    Send(controller, "previewPresetArchive",
         {{"requestId", "p1"},
          {"itemId", "item-9"},
          {"title", "Shared Lead"},
          {"data", guitarfx::util::EncodeBase64(zip)}});

    const auto reply = Latest(host, "presetArchivePreviewed");
    Expect(reply.has_value() && reply->value("requestId", "") == "p1",
           "a preview is answered with presetArchivePreviewed");

    if (reply)
    {
        Expect(reply->value("presetId", "") == "preview-item-9", "it plays under the preview's id");
        Expect(reply->value("stagedCount", 0) == 1, "the archive's model is staged");
        Expect(reply->value("unavailable", nlohmann::json::array()).empty(), "nothing is missing");
        Expect(reply->value("title", "") == "Shared Lead", "a one-preset archive's preset takes the title");
    }

    Expect(ActivePresetId(controller) == "preview-item-9", "the preview is the running preset");
    const auto* amp = ActiveAmp(controller);
    Expect(amp && fs::exists(amp->filePath) && amp->filePath.parent_path().parent_path().filename() == "tone-preview",
           "its model plays from a staged file");
    Expect(LibraryCount(controller, "nam") == models, "no model joins the library");
    Expect(InstalledEntries(controller).empty(), "nothing is recorded as installed");
    Expect(!StoredPreset(host, controller, "preview-item-9").has_value(), "no preset is saved");
}

/// An archive whose model the library already holds plays the library's copy: nothing is staged.
void TestArchivePreviewPlaysTheLibrarysCopy()
{
    TestHost host(Sandbox("preview-library"));
    guitarfx::PluginController controller(host);
    controller.Initialize();
    const auto zip = SinglePresetArchive(BuildPreset("shared-q", "Q", "sha-q"), "sha-q", kModelA);
    Send(controller, "installPresetArchives",
         {{"requestId", "i1"},
          {"entry", {{"id", "tone-sharing-api:item:q"}, {"title", "Q"}}},
          {"archives", {Archive(zip, "Q")}}});
    const auto models = LibraryCount(controller, "nam");

    host.sent.clear();
    Send(controller, "previewPresetArchive", {{"itemId", "q"}, {"data", guitarfx::util::EncodeBase64(zip)}});

    const auto reply = Latest(host, "presetArchivePreviewed");
    Expect(reply.has_value() && reply->value("stagedCount", -1) == 0, "nothing is staged for a model the library has");
    const auto* amp = ActiveAmp(controller);
    Expect(amp && amp->resourceId != "sha-q" &&
               controller.GetResourceLibrary().LookupResource("nam", amp->resourceId).has_value(),
           "the node names the library's model");
    Expect(LibraryCount(controller, "nam") == models, "and the library is unchanged");
}

/// A Tone3000 model the archive names is staged from what the UI fetched; without it, the
/// preview still plays and the reply names what it lacks.
void TestArchivePreviewStagesFetchedRemoteModels()
{
    TestHost host(Sandbox("preview-remote"));
    guitarfx::PluginController controller(host);
    controller.Initialize();
    const auto models = LibraryCount(controller, "nam");

    Preset preset = BuildPreset("shared-r", "Remote", "t3k-r");
    const nlohmann::json root = {
        {"formatVersion", 1},
        {"preset", PresetJson(preset)},
        {"resources", nlohmann::json::array()},
        {"blends", nlohmann::json::array()},
        {"remoteResources",
         {{{"id", "t3k-r"},
           {"type", "nam"},
           {"name", "Borrowed Amp"},
           {"provider", "tone3000"},
           {"providerKey", "123:456"},
           {"providerData", {{"toneId", "123"}, {"modelId", "456"}}}}}},
    };
    const auto zip = BuildZip({{"preset.json", ToBytes(root.dump())}});

    host.sent.clear();
    Send(controller, "previewPresetArchive", {{"itemId", "r"}, {"data", guitarfx::util::EncodeBase64(zip)}});
    auto reply = Latest(host, "presetArchivePreviewed");
    Expect(reply.has_value(), "a preview without the remote model still plays");

    if (reply)
    {
        const auto unavailable = reply->value("unavailable", nlohmann::json::array());
        Expect(unavailable.size() == 1 && unavailable[0] == "Borrowed Amp", "and names the model it lacks");
        Expect(reply->value("stagedCount", -1) == 0, "nothing staged for it");
    }

    host.sent.clear();
    Send(controller, "previewPresetArchive",
         {{"itemId", "r"},
          {"data", guitarfx::util::EncodeBase64(zip)},
          {"fetchedResources",
           {{{"resourceType", "nam"},
             {"resourceId", "t3k-r"},
             {"name", "Borrowed Amp"},
             {"data", guitarfx::util::EncodeBase64(kModelB)}}}}});
    reply = Latest(host, "presetArchivePreviewed");
    Expect(reply.has_value() && reply->value("stagedCount", -1) == 1 &&
               reply->value("unavailable", nlohmann::json::array()).empty(),
           "the fetched model is staged");
    const auto* amp = ActiveAmp(controller);
    Expect(amp && amp->resourceId == "t3k-r" && fs::exists(amp->filePath), "the node plays the staged model");
    Expect(LibraryCount(controller, "nam") == models, "the fetched model does not join the library");
}

void TestUnreadableArchivePreviewIsRefused()
{
    TestHost host(Sandbox("preview-bad"));
    guitarfx::PluginController controller(host);
    controller.Initialize();
    const auto activeBefore = ActivePresetId(controller);

    host.sent.clear();
    Send(controller, "previewPresetArchive",
         {{"requestId", "bad"}, {"title", "Junk"}, {"data", guitarfx::util::EncodeBase64(ToBytes("not a zip"))}});

    const auto failed = Latest(host, "presetArchivePreviewFailed");
    Expect(failed.has_value() && failed->value("requestId", "") == "bad", "an unreadable download is refused");
    Expect(!Latest(host, "presetArchivePreviewed").has_value(), "and nothing reports a preview");
    Expect(ActivePresetId(controller) == activeBefore, "the running preset stays");
}

/// An install records which item each preset came from, when the UI says, so a favourite or
/// rating given to an installed pack preset can be sent to the service.
void TestInstallRecordsEachPresetsItem()
{
    TestHost host(Sandbox("preset-items"));
    guitarfx::PluginController controller(host);
    controller.Initialize();
    const auto first = SinglePresetArchive(BuildPreset("a", "A", "sha-a"), "sha-a", kModelA);
    const auto second = SinglePresetArchive(BuildPreset("b", "B", "sha-b"), "sha-b", kModelB);
    auto archiveA = Archive(first, "A");
    archiveA["itemId"] = "item-a";
    auto archiveB = Archive(second, "B");
    archiveB["itemId"] = "item-b";

    host.sent.clear();
    Send(controller, "installPresetArchives",
         {{"requestId", "pk"},
          {"entry", {{"id", "tone-sharing-api:pack:pk"}, {"title", "Pack"}, {"packId", "pk"}}},
          {"folder", "Pack"},
          {"archives", {archiveA, archiveB}}});

    const auto reply = Latest(host, "presetArchivesInstalled");
    const auto entries = InstalledEntries(controller);
    Expect(reply.has_value() && entries.size() == 1, "the pack installs");

    if (reply && entries.size() == 1)
    {
        const auto ids = reply->value("presetIds", nlohmann::json::array());
        const auto items = entries[0].value("presetItems", nlohmann::json::object());
        Expect(ids.size() == 2 && items.value(ids[0].get<std::string>(), "") == "item-a" &&
                   items.value(ids[1].get<std::string>(), "") == "item-b",
               "each preset's item is recorded");
    }
}

int main()
{
    TestInstallOnePreset();
    TestCustomEffectModulesAreNotInstalled();
    TestHostileNodeIdsAndPathsAreCleaned();
    TestSecondInstallReusesTheModel();
    TestReusesAnOlderImportOfTheSameFile();
    TestPackGetsAFolder();
    TestMissingTone3000ModelsInstallNothing();
    TestRemoteResourceResolvesByProviderKey();
    TestMissingRemoteModelsFromOtherProvidersInstallNothing();
    TestBothListsNameTheModelOnce();
    TestUnreadableDownloadIsRefused();
    TestDeleteTakesOutWhatItAdded();
    TestMissingRemoteModelsCanBeAllowed();
    TestFetchedResourcesLeaveWithTheInstall();
    TestPackArchiveFoldersAreKept();
    TestDeletingAnUnknownInstallIsAnswered();
    TestInstallRecordsEachPresetsItem();
    TestArchivePreviewWritesNothing();
    TestArchivePreviewPlaysTheLibrarysCopy();
    TestArchivePreviewStagesFetchedRemoteModels();
    TestUnreadableArchivePreviewIsRefused();

    if (gFailures > 0)
    {
        std::cerr << gFailures << " check(s) failed" << std::endl;
        return 1;
    }

    std::cout << "PresetArchiveInstallTests passed" << std::endl;
    return 0;
}

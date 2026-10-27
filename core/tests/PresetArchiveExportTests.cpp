/**
 * PresetArchiveExportTests.cpp - Exporting presets as a preset archive
 * (controller/PluginControllerArchiveExport.cpp, "exportPresetArchive").
 *
 * A headless controller is seeded by installing a pack (two presets, two models, a blend and a
 * folder), exports one and then both presets, and the zips the test host's save dialog was
 * pointed at are read back: the web UI's JSON shapes, models filed under their SHA-256 and
 * every reference moved onto it. Each archive is then installed into a fresh profile with
 * installPresetArchives, which must bring the presets, the models and the blend back.
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
#include "dsp/EffectGuids.h"
#include "presets/PresetStorage.h"
#include "resources/ResourceLibrary.h"
#include "util/Base64.h"
#include "util/FileIO.h"
#include "util/PathEncoding.h"
#include "util/Sha256.h"

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

    /// The save dialog: answers with `savePath`, or as cancelled when it is empty.
    void SaveFileAsync(guitarfx::BrowseFileType, const std::string&, const std::string& defaultName,
                       std::function<void(const guitarfx::BrowseFileResult&)> callback) override
    {
        lastDefaultName = defaultName;
        guitarfx::BrowseFileResult result;
        result.success = !savePath.empty();
        result.path = savePath;
        callback(result);
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
    fs::path savePath;
    std::string lastDefaultName;

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

fs::path RunRoot()
{
    static const auto run = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count() % 100000000);
    return fs::temp_directory_path() / ("guitarfx-archive-export-tests-" + run);
}

fs::path Sandbox(const std::string& name)
{
    const auto root = RunRoot() / name;
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

/// Where the save dialog puts the exports: outside every profile.
fs::path ExportDir()
{
    const auto dir = RunRoot() / "exports";
    std::error_code ec;
    fs::create_directories(dir, ec);
    return dir;
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
        mz_free(buffer);
    }

    mz_zip_writer_end(&archive);
    return zip;
}

Bytes ToBytes(const std::string& text)
{
    return Bytes(text.begin(), text.end());
}

std::vector<std::string> ZipEntryNames(const Bytes& zip)
{
    std::vector<std::string> names;
    mz_zip_archive archive{};

    if (!mz_zip_reader_init_mem(&archive, zip.data(), zip.size(), 0))
    {
        return names;
    }

    for (mz_uint i = 0; i < mz_zip_reader_get_num_files(&archive); ++i)
    {
        char name[512] = {};
        mz_zip_reader_get_filename(&archive, i, name, sizeof(name));
        names.emplace_back(name);
    }

    mz_zip_reader_end(&archive);
    return names;
}

std::optional<nlohmann::json> ZipJson(const Bytes& zip, const std::string& entry)
{
    const auto bytes = guitarfx::controller_detail::ExtractZipEntry(zip, entry);

    if (!bytes)
    {
        return std::nullopt;
    }

    auto parsed = nlohmann::json::parse(std::string(bytes->begin(), bytes->end()), nullptr, false);
    return parsed.is_discarded() ? std::nullopt : std::optional<nlohmann::json>(parsed);
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
    return preset;
}

nlohmann::json PresetJson(const Preset& preset)
{
    return nlohmann::json::parse(guitarfx::PresetStorage::SerializeToJson(preset));
}

nlohmann::json Archive(const Bytes& zip, const std::string& title)
{
    return {{"data", guitarfx::util::EncodeBase64(zip)}, {"title", title}};
}

std::optional<Preset> StoredPreset(TestHost& host, guitarfx::PluginController& controller, const std::string& id)
{
    const auto before = host.sent.size();
    Send(controller, "getPresetById", {{"presetId", id}});

    for (auto i = before; i < host.sent.size(); ++i)
    {
        auto parsed = nlohmann::json::parse(host.sent[i], nullptr, false);

        if (parsed.is_object() && parsed.value("type", "") == "presetData" && parsed.value("requestedPresetId", "") == id)
        {
            return guitarfx::PresetStorage::DeserializeFromJson(parsed["preset"].dump());
        }
    }

    return std::nullopt;
}

const Bytes kModelA = ToBytes(R"({"version":"0.5.0","architecture":"Linear","config":{"receptive_field":2,"bias":false},"weights":[1.0,0.0]})");
const Bytes kModelB = ToBytes(R"({"version":"0.5.0","architecture":"Linear","config":{"receptive_field":2,"bias":false},"weights":[0.5,0.0]})");

/// A pack as tone sharing serves one: "Chug" plays model A, "Lead" plays a blend of A and B.
Bytes SeedPack()
{
    const nlohmann::json blend = {{"id", "blend-1"},
                                  {"name", "Sweep"},
                                  {"models", {"sha-a", "sha-b"}},
                                  {"modelMappings", {{{"id", "sha-a"}, {"parameterValue", 0.0}},
                                                     {{"id", "sha-b"}, {"parameterValue", 1.0}}}}};
    const nlohmann::json root = {
        {"formatVersion", 1},
        {"presets", {PresetJson(BuildPreset("p1", "Chug", "sha-a")), PresetJson(BuildPreset("p2", "Lead", "sha-b", "blend-1"))}},
        {"resources",
         {{{"id", "sha-a"}, {"name", "Model A"}, {"category", "amp"}, {"type", "nam"}, {"fileName", "sha-a-Model A.nam"}, {"hash", "sha-a"}},
          {{"id", "sha-b"}, {"name", "Model B"}, {"category", "amp"}, {"type", "nam"}, {"fileName", "sha-b-model-b.nam"}, {"hash", "sha-b"}}}},
        {"blends", {blend}},
    };

    return BuildZip({{"presets.json", ToBytes(root.dump())},
                     {"resources/sha-a-Model A.nam", kModelA},
                     {"resources/sha-b-model-b.nam", kModelB}});
}

/// `json[key]`, or null where it is missing: the const operator[] asserts.
nlohmann::json At(const nlohmann::json& json, const std::string& key)
{
    return json.is_object() && json.contains(key) ? json[key] : nlohmann::json();
}

nlohmann::json At(const nlohmann::json& json, std::size_t index)
{
    return json.is_array() && index < json.size() ? json[index] : nlohmann::json();
}

/// The node with `nodeId` in the preset's graph or, as the engine writes a preset with scenes,
/// its first scene's.
const nlohmann::json* NodeById(const nlohmann::json& preset, const std::string& nodeId)
{
    const nlohmann::json* graph = nullptr;

    if (preset.contains("graph") && preset["graph"].is_object())
    {
        graph = &preset["graph"];
    }
    else if (preset.contains("scenes") && preset["scenes"].is_array() && !preset["scenes"].empty() &&
             preset["scenes"][0].contains("graph"))
    {
        graph = &preset["scenes"][0]["graph"];
    }

    if (!graph || !graph->contains("nodes") || !(*graph)["nodes"].is_array())
    {
        return nullptr;
    }

    for (const auto& node : (*graph)["nodes"])
    {
        if (node.value("id", "") == nodeId)
        {
            return &node;
        }
    }

    return nullptr;
}

/// The blend library a UI opening now is sent.
nlohmann::json BlendLibrary(TestHost& host, guitarfx::PluginController& controller)
{
    host.sent.clear();
    controller.HandleUIMessage(nlohmann::json{{"type", "uiReady"}}.dump());
    controller.OnIdle();
    nlohmann::json blends = nlohmann::json::array();

    for (const auto& message : host.sent)
    {
        const auto parsed = nlohmann::json::parse(message, nullptr, false);

        if (parsed.is_object() && parsed.contains("blendLibrary") && parsed["blendLibrary"].is_array())
        {
            blends = parsed["blendLibrary"];
        }
    }

    return blends;
}

struct Exports
{
    Bytes single;
    Bytes collection;
};

void TestSha256()
{
    Expect(guitarfx::util::Sha256Hex(ToBytes("abc")) ==
               "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
           "SHA-256 of \"abc\"");
    Expect(guitarfx::util::Sha256Hex(Bytes{}) == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
           "SHA-256 of nothing");
    // 56 bytes: the length no longer fits the first padded block.
    Expect(guitarfx::util::Sha256Hex(ToBytes("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq")) ==
               "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1",
           "SHA-256 of the two-block test vector");
}

Exports TestExport()
{
    Exports exports;
    TestHost host(Sandbox("export"));
    guitarfx::PluginController controller(host);
    controller.Initialize();

    Send(controller, "installPresetArchives",
         {{"requestId", "seed"},
          {"entry", {{"id", "tone-sharing-api:pack-seed"}, {"title", "Seed Pack"}, {"packId", "seed"}}},
          {"folder", "Seed Pack"},
          {"archives", {Archive(SeedPack(), "Seed Pack")}}});

    const auto installed = Latest(host, "presetArchivesInstalled");
    const auto presetIds = installed ? installed->value("presetIds", nlohmann::json::array()) : nlohmann::json::array();
    Expect(presetIds.size() == 2, "the seed pack installs two presets");

    if (presetIds.size() != 2)
    {
        return exports;
    }

    const std::string chugId = presetIds[0].get<std::string>();
    const std::string leadId = presetIds[1].get<std::string>();
    const std::string hashA = guitarfx::util::Sha256Hex(kModelA);
    const std::string hashB = guitarfx::util::Sha256Hex(kModelB);

    // ── Refused before any dialog ───────────────────────────────────────────────
    host.savePath = ExportDir() / "unused.soundshed.preset";
    host.lastDefaultName.clear();
    Send(controller, "exportPresetArchive", {{"presetIds", nlohmann::json::array()}, {"name", "x"}});
    auto failed = Latest(host, "presetExportFailed");
    Expect(failed && failed->value("message", "") == "No presets to export", "no ids is refused");
    Send(controller, "exportPresetArchive", {{"presetIds", {"no-such-preset"}}, {"name", "x"}});
    failed = Latest(host, "presetExportFailed");
    Expect(failed && failed->value("message", "") == "Preset not found", "an unknown preset is refused");
    Expect(host.lastDefaultName.empty(), "and no save dialog is shown for either");

    // ── Cancelled ───────────────────────────────────────────────────────────────
    host.sent.clear();
    host.savePath.clear();
    Send(controller, "exportPresetArchive", {{"presetIds", {chugId}}, {"name", "Chug"}});
    failed = Latest(host, "presetExportFailed");
    Expect(failed && failed->value("message", "") == "Save cancelled", "a cancelled dialog is answered as savePresetArchive's is");
    Expect(!Latest(host, "presetExportSaved").has_value(), "and nothing is saved");

    // ── One preset ──────────────────────────────────────────────────────────────
    host.sent.clear();
    host.savePath = ExportDir() / "chug-export";
    Send(controller, "exportPresetArchive", {{"presetIds", {chugId}}, {"name", "My Chug!.soundshed.preset"}});

    Expect(host.lastDefaultName == "My-Chug-.soundshed.preset",
           "the dialog suggests the web UI's file name (got " + host.lastDefaultName + ")");
    auto saved = Latest(host, "presetExportSaved");
    Expect(saved.has_value(), "a one-preset export is answered with presetExportSaved");

    if (saved)
    {
        const fs::path path = guitarfx::util::PathFromUtf8(saved->value("path", ""));
        Expect(path.filename() == "chug-export.soundshed.preset",
               "the saved path gets the archive extension (got " + saved->value("path", "") + ")");
        Expect(saved->value("presetCount", 0) == 1 && saved->value("resourceCount", 0) == 1, "with what it carries");
        Expect(!saved->contains("warnings"), "and no warnings");
        exports.single = guitarfx::util::ReadFileBytes(path);
    }

    Expect(!exports.single.empty(), "the one-preset archive is written");
    const auto names = ZipEntryNames(exports.single);
    Expect(names.size() == 3 && names[0] == "resources/" && names[2] == "preset.json",
           "laid out as JSZip lays it out: resources/, its files, then preset.json");
    Expect(!ZipJson(exports.single, "presets.json").has_value(), "a one-preset archive has no presets.json");

    if (const auto manifest = ZipJson(exports.single, "preset.json"))
    {
        Expect(manifest->value("formatVersion", 0) == 1, "formatVersion 1");
        Expect(!manifest->contains("createdAt") && !manifest->contains("presetFolders") &&
                   !manifest->contains("effectPresets"),
               "the single format carries no createdAt, folders or effect presets");
        Expect(manifest->value("blends", nlohmann::json::object()) == nlohmann::json::array(), "and no blends it does not use");

        const auto resources = manifest->value("resources", nlohmann::json::array());
        Expect(resources.size() == 1, "the model it plays is carried");

        if (resources.size() == 1)
        {
            const auto& resource = resources[0];
            const std::string fileName = resource.value("fileName", "");
            Expect(resource.value("id", "") == hashA && resource.value("hash", "") == hashA,
                   "identified by the SHA-256 of its bytes");
            Expect(resource.value("type", "") == "nam" && resource.value("name", "") == "Model A" &&
                       resource.value("category", "") == "amp",
                   "with its library type, name and category");
            Expect(fileName.rfind(hashA + "-", 0) == 0 && fileName.size() > 4 &&
                       fileName.compare(fileName.size() - 4, 4, ".nam") == 0 &&
                       fileName.find(' ') == std::string::npos,
                   "filed as <hash>-<name>.nam (got " + fileName + ")");
            Expect(resource.contains("metadata") && resource["metadata"].is_object(),
                   "and the library entry's metadata");
            const auto bytes = guitarfx::controller_detail::ExtractZipEntry(exports.single, "resources/" + fileName);
            Expect(bytes && *bytes == kModelA, "the file holds the model's bytes");
        }

        const auto preset = manifest->value("preset", nlohmann::json::object());
        Expect(preset.value("id", "") == chugId && preset.value("name", "") == "Chug", "the preset is the one asked for");
        Expect(!preset.contains("globalSignalChain") && !preset.contains("globals"), "without the rig's globals");

        if (const auto* amp = NodeById(preset, "amp_1"))
        {
            const auto res = At(At(*amp, "resources"), 0);
            Expect(res.is_object() && res.value("resourceId", "") == hashA && res.value("id", "") == hashA,
                   "its node plays the archive's copy, under resourceId and id");
            Expect(res.is_object() && !res.contains("filePath"), "and names no file on this machine");
        }
        else
        {
            Expect(false, "the exported preset keeps its amp node");
        }
    }
    else
    {
        Expect(false, "preset.json can be read back");
    }

    // ── One preset, asked for as a collection (a folder holding one) ────────────
    host.sent.clear();
    host.savePath = ExportDir() / "lead-folder";
    Send(controller, "exportPresetArchive",
         {{"presetIds", {leadId}}, {"name", "Lead Folder"}, {"folderId", "__all__"}, {"collection", true}});

    Expect(host.lastDefaultName == "Lead-Folder.soundshed.presets",
           "a one-preset collection is suggested as .soundshed.presets (got " + host.lastDefaultName + ")");
    saved = Latest(host, "presetExportSaved");
    Expect(saved.has_value(), "a one-preset collection is answered with presetExportSaved");

    if (saved)
    {
        const fs::path path = guitarfx::util::PathFromUtf8(saved->value("path", ""));
        Expect(path.filename() == "lead-folder.soundshed.presets",
               "a bare saved name gets the collection's extension (got " + saved->value("path", "") + ")");
        const auto zip = guitarfx::util::ReadFileBytes(path);
        Expect(!ZipJson(zip, "preset.json").has_value(), "a one-preset collection has no preset.json");

        if (const auto oneCollection = ZipJson(zip, "presets.json"))
        {
            const auto presets = oneCollection->value("presets", nlohmann::json::array());
            Expect(presets.size() == 1 && presets[0].value("id", "") == leadId, "it holds the one preset");
            Expect(oneCollection->contains("createdAt"), "in the collection format");
            Expect(oneCollection->value("blends", nlohmann::json::array()).size() == 1 &&
                       oneCollection->value("resources", nlohmann::json::array()).size() == 2,
                   "with the blend it plays and both of the blend's models");
            const auto folders = oneCollection->value("presetFolders", nlohmann::json::array());
            Expect(folders.size() == 1 && folders[0].value("name", "") == "Seed Pack" &&
                       folders[0].value("presetIds", nlohmann::json::array()) == nlohmann::json::array({leadId}),
                   "and the folder it sits in");
        }
        else
        {
            Expect(false, "the one-preset collection's presets.json can be read back");
        }
    }

    // ── Both, with their folder ─────────────────────────────────────────────────
    host.sent.clear();
    host.savePath = ExportDir() / "seed-export.soundshed.presets";
    Send(controller, "exportPresetArchive",
         {{"presetIds", {chugId, leadId, "gone-preset"}}, {"name", "Seed Pack"}, {"folderId", "__all__"}});

    Expect(host.lastDefaultName == "Seed-Pack.soundshed.presets", "a collection is suggested as .soundshed.presets");
    saved = Latest(host, "presetExportSaved");
    Expect(saved.has_value(), "a collection export is answered with presetExportSaved");

    if (saved)
    {
        exports.collection = guitarfx::util::ReadFileBytes(guitarfx::util::PathFromUtf8(saved->value("path", "")));
        const auto warnings = saved->value("warnings", nlohmann::json::array());
        Expect(warnings.size() == 1 && warnings[0].get<std::string>() == "1 preset could not be found",
               "a preset that cannot be found is left out with a warning");
        Expect(saved->value("presetCount", 0) == 2 && saved->value("resourceCount", 0) == 2, "with what it carries");
    }

    const auto collection = ZipJson(exports.collection, "presets.json");
    Expect(collection.has_value(), "presets.json can be read back");

    if (collection)
    {
        Expect(collection->value("formatVersion", 0) == 1, "formatVersion 1");
        const std::string createdAt = collection->value("createdAt", "");
        Expect(createdAt.size() == 24 && createdAt[10] == 'T' && createdAt[19] == '.' && createdAt.back() == 'Z',
               "createdAt as toISOString writes it (got " + createdAt + ")");

        const auto presets = collection->value("presets", nlohmann::json::array());
        Expect(presets.size() == 2 && presets[0].value("id", "") == chugId && presets[1].value("id", "") == leadId,
               "both presets, in the order asked for");

        const auto resources = collection->value("resources", nlohmann::json::array());
        Expect(resources.size() == 2 && resources[0].value("id", "") == hashA && resources[1].value("id", "") == hashB,
               "each model once, in the order the presets reach them");

        const auto blends = collection->value("blends", nlohmann::json::array());
        Expect(blends.size() == 1, "the blend the second preset plays is carried");

        if (blends.size() == 1)
        {
            Expect(blends[0].value("models", nlohmann::json::array()) == nlohmann::json({hashA, hashB}),
                   "its models moved onto the archive's hashes");
            const auto mapping = At(At(blends[0], "modelMappings"), 1);
            Expect(mapping.is_object() && mapping.value("id", "") == hashB && mapping.value("parameterValue", 0.0) == 1.0,
                   "and its mappings too, keeping their places");

            if (presets.size() == 2)
            {
                const auto* amp = NodeById(presets[1], "amp_1");
                Expect(amp && amp->value("config", nlohmann::json::object()).value("blendId", "") == blends[0].value("id", "x"),
                       "the preset names the blend it carries");
            }
        }

        const auto folders = collection->value("presetFolders", nlohmann::json::array());
        Expect(folders.size() == 1 && folders[0].value("name", "") == "Seed Pack" &&
                   folders[0].value("presetIds", nlohmann::json::array()) == nlohmann::json({chugId, leadId}) &&
                   !folders[0].contains("id"),
               "the folder they sit in, as buildArchivePresetFoldersForExport writes it");
        Expect(!collection->contains("effectPresets"), "no effect presets while the setting is off");
    }

    // ── Effect presets, for factory archive authoring ───────────────────────────
    std::string libraryA;

    for (const auto& resource : controller.GetResourceLibrary().GetResourcesByType("nam"))
    {
        if (guitarfx::util::Sha256Hex(guitarfx::util::ReadFileBytes(resource.filePath)) == hashA)
        {
            libraryA = resource.id;
        }
    }

    Expect(!libraryA.empty(), "model A is in the library");

    Preset fxChain = BuildPreset("p-fx", "FX", libraryA);
    fxChain.graph.nodes[1].type = guitarfx::EffectGuids::kAmpNamOptimized;
    Send(controller, "loadPreset", {{"preset", PresetJson(fxChain)}, {"presetId", "p-fx"}});
    Send(controller, "saveEffectPreset",
         {{"effectType", guitarfx::EffectGuids::kAmpNamOptimized}, {"nodeId", "amp_1"}, {"name", "Chugs"}});
    Send(controller, "saveEffectPreset",
         {{"effectType", guitarfx::EffectGuids::kGain}, {"name", "Boost"}, {"parameters", {{"gain", 6.0}}}});
    Send(controller, "setSetting", {{"key", "features.factoryPresetArchives.enabled"}, {"value", true}});
    Send(controller, "setSetting", {{"key", "factoryPresets.exportIncludesEffectPresets"}, {"value", true}});

    host.sent.clear();
    host.savePath = ExportDir() / "with-effects.soundshed.presets";
    Send(controller, "exportPresetArchive", {{"presetIds", {leadId, chugId}}, {"name", "With Effects"}});
    saved = Latest(host, "presetExportSaved");
    const auto withEffects = saved ? ZipJson(guitarfx::util::ReadFileBytes(guitarfx::util::PathFromUtf8(saved->value("path", ""))),
                                             "presets.json")
                                   : std::nullopt;
    Expect(withEffects.has_value(), "the export with effect presets is saved");

    if (withEffects)
    {
        Expect(!withEffects->contains("presetFolders"), "no folderId, no folders");
        const auto effectPresets = withEffects->value("effectPresets", nlohmann::json::object());
        const auto amps = effectPresets.value(guitarfx::EffectGuids::kAmpNamOptimized, nlohmann::json::array());
        const auto gains = effectPresets.value(guitarfx::EffectGuids::kGain, nlohmann::json::array());
        Expect(amps.size() == 1 && amps[0].value("name", "") == "Chugs", "the NAM effect preset rides along");
        Expect(gains.size() == 1 && At(gains[0], "parameters").is_object() && At(gains[0], "parameters").value("gain", 0.0) == 6.0, "and the parameters-only one");

        if (amps.size() == 1)
        {
            const auto ref = At(At(amps[0], "resources"), 0);
            Expect(ref.is_object() && ref.value("resourceType", "") == "nam" && ref.value("resourceId", "") == hashA,
                   "its model moved onto the archive's hash");
            Expect(ref.is_object() && !ref.contains("filePath") && !ref.contains("id") && !ref.contains("type"),
                   "without a path or the old id fields");
        }
    }

    return exports;
}

void TestRoundTrip(const Exports& exports)
{
    TestHost host(Sandbox("round-trip"));
    guitarfx::PluginController controller(host);
    controller.Initialize();

    Send(controller, "installPresetArchives",
         {{"requestId", "rt"},
          {"entry", {{"id", "zip-import:round-trip"}, {"title", "Round Trip"}}},
          {"archives", {Archive(exports.collection, "Seed Pack"), Archive(exports.single, "Single Chug")}}});

    const auto reply = Latest(host, "presetArchivesInstalled");
    Expect(reply.has_value(), "the exported archives install");

    if (!reply)
    {
        if (const auto failed = Latest(host, "presetArchivesInstallFailed"))
        {
            std::cerr << "  install failed: " << failed->dump() << std::endl;
        }

        return;
    }

    const auto presetIds = reply->value("presetIds", nlohmann::json::array());
    Expect(presetIds.size() == 3, "all three presets come back");
    Expect(reply->value("resourceCount", 0) == 2, "with both models, the single archive's reusing the first copy");

    if (presetIds.size() != 3)
    {
        return;
    }

    const auto modelBytes = [&](const Preset& preset) {
        const auto* amp = preset.graph.FindNode("amp_1");
        const auto resource = amp && !amp->resources.empty()
                                  ? controller.GetResourceLibrary().LookupResource("nam", amp->resources[0].resourceId)
                                  : std::nullopt;
        return resource ? guitarfx::util::ReadFileBytes(resource->filePath) : Bytes{};
    };

    const auto chug = StoredPreset(host, controller, presetIds[0].get<std::string>());
    const auto lead = StoredPreset(host, controller, presetIds[1].get<std::string>());
    const auto single = StoredPreset(host, controller, presetIds[2].get<std::string>());

    Expect(chug && chug->name == "Chug" && modelBytes(*chug) == kModelA, "Chug plays model A again");
    Expect(single && single->name == "Single Chug" && modelBytes(*single) == kModelA,
           "the one-preset archive's preset plays model A too");

    if (lead)
    {
        const auto* amp = lead->graph.FindNode("amp_1");
        const auto blendIt = amp ? amp->config.find("blendId") : std::map<std::string, std::string>::const_iterator{};
        Expect(amp && blendIt != amp->config.end() && !blendIt->second.empty(), "Lead still plays a blend");
        Expect(modelBytes(*lead) == kModelB, "and its own model is model B");

        bool blendFound = false;

        if (amp && blendIt != amp->config.end())
        {
            for (const auto& blend : BlendLibrary(host, controller))
            {
                if (blend.value("id", "") != blendIt->second)
                {
                    continue;
                }

                blendFound = true;
                const auto models = blend.value("models", nlohmann::json::array());
                Expect(models.size() == 2, "the blend keeps both models");

                for (const auto& model : models)
                {
                    Expect(controller.GetResourceLibrary().HasResource("nam", model.get<std::string>()),
                           "each blend model is a library model");
                }
            }
        }

        Expect(blendFound, "the blend comes back into the library");
    }
    else
    {
        Expect(false, "Lead comes back");
    }
}
} // namespace

int main()
{
    TestSha256();
    const auto exports = TestExport();

    if (!exports.single.empty() && !exports.collection.empty())
    {
        TestRoundTrip(exports);
    }
    else
    {
        Expect(false, "both archives were exported, so they can be installed again");
    }

    if (gFailures > 0)
    {
        std::cerr << gFailures << " check(s) failed" << std::endl;
        return 1;
    }

    std::cout << "PresetArchiveExportTests passed" << std::endl;
    return 0;
}

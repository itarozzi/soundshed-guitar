/**
 * PluginControllerArchiveExport.cpp - Exporting presets as a preset archive
 * ("exportPresetArchive"), and the library as a library archive ("exportLibraryArchive",
 * at the end of the file).
 *
 * One preset becomes a `.soundshed.preset` zip (preset.json); several, or any number with
 * `collection`, become a `.soundshed.presets` zip (presets.json) that also carries their folder
 * layout and, for
 * factory archive authoring, the user's effect presets. Either way the models and IRs the
 * presets play travel under resources/, named and identified by the SHA-256 of their bytes,
 * with the blends they use. This is the format the web UI has written since it defined it
 * (core/ui/ts/presets/archive.ts: buildPresetArchiveBlob, exportPresetCollectionArchive) and
 * that every importer reads (PresetArchiveSupport.cpp, PluginControllerArchiveInstall.cpp,
 * the web UI's importPresetArchive), built here so a UI without JSZip can export too.
 *
 * Where it differs from the TypeScript, on purpose:
 * - The preset being edited is exported from its working copy, as the web UI exports what its
 *   cache holds; any other preset is read from where it is stored. A preset that cannot be found
 *   is left out with a warning (the web UI wrote what little its list held).
 * - Resource file paths, which only name files on this machine and which every importer drops,
 *   are not written.
 * - Only models and IRs are carried (IsShareableResourceType): importers skip anything else.
 * - The same file reached by two ids is carried once.
 * - Entries are deflated rather than stored, and JSON keys come out in sorted order.
 */

#include "PluginController.h"

#include "controller/internal/BlendSupport.h"
#include "controller/internal/HostedPluginSupport.h"
#include "controller/internal/PresetArchiveSupport.h"
#include "dsp/EffectGuids.h"
#include "resources/ResourceLibrary.h"
#include "util/FileIO.h"
#include "util/PathEncoding.h"
#include "util/Sha256.h"

#include <miniz.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

using namespace guitarfx::controller_detail;

namespace guitarfx
{
namespace
{
constexpr const char* kAllPresetsFolderId = "__all__";
constexpr const char* kFavoritesFolderId = "__favorites__";
constexpr const char* kRecentsFolderId = "__recents__";
constexpr const char* kEffectPresetsDocument = "effect-presets.json";
constexpr const char* kFactoryArchiveToolsSettingKey = "features.factoryPresetArchives.enabled";
constexpr const char* kLegacyAdvancedOptionsSettingKey = "ui.advancedOptionsEnabled";
constexpr const char* kExportEffectPresetsSettingKey = "factoryPresets.exportIncludesEffectPresets";
constexpr std::size_t kMaxArchiveFileNameLength = 120;

using IdMap = std::unordered_map<std::string, std::string>;

struct ExportRef
{
    std::string type;
    std::string id;
};

std::string Trim(const std::string& value)
{
    const auto isSpace = [](unsigned char ch) { return std::isspace(ch) != 0; };
    const auto begin = std::find_if_not(value.begin(), value.end(), isSpace);
    const auto end = std::find_if_not(value.rbegin(), value.rend(), isSpace).base();
    return begin < end ? std::string(begin, end) : std::string{};
}

std::string ToLowerAscii(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return value;
}

/// archiveUtils.ts sanitizeFilename: a run of anything but ASCII letters, digits, '-', '_' and
/// '.' becomes one '-'. Not util::SanitizeFilename, whose output differs.
std::string SanitizeArchiveName(const std::string& raw, const std::string& fallback = "file")
{
    std::string trimmed = Trim(raw);

    if (trimmed.empty())
    {
        trimmed = fallback;
    }

    std::string result;
    bool inRun = false;

    for (const char ch : trimmed)
    {
        const auto byte = static_cast<unsigned char>(ch);
        const bool allowed = byte < 0x80 && (std::isalnum(byte) || ch == '-' || ch == '_' || ch == '.');

        if (allowed)
        {
            result.push_back(ch);
            inRun = false;
        }
        else if (!inRun)
        {
            result.push_back('-');
            inRun = true;
        }
    }

    return result;
}

/// The part of an id after its last "__": a factory or session id's library id.
std::string IdSuffix(const std::string& id)
{
    const auto pos = id.rfind("__");
    return pos == std::string::npos ? id : id.substr(pos + 2);
}

const std::string* MappedId(const IdMap& idMap, const std::string& id)
{
    if (const auto it = idMap.find(id); it != idMap.end())
    {
        return &it->second;
    }

    if (const auto it = idMap.find(IdSuffix(id)); it != idMap.end())
    {
        return &it->second;
    }

    return nullptr;
}

std::string StringField(const nlohmann::json& object, const char* key)
{
    const auto it = object.find(key);
    return it != object.end() && it->is_string() ? it->get<std::string>() : std::string{};
}

/// `primary` when the object has it as a string (even empty), else `fallback`: TS `a ?? b`.
std::string StringFieldOr(const nlohmann::json& object, const char* primary, const char* fallback)
{
    const auto it = object.find(primary);
    return it != object.end() && it->is_string() ? it->get<std::string>() : StringField(object, fallback);
}

/// utils.ts findResourceById: the exact id, else a library entry named by the id's suffix.
std::optional<LibraryResource> FindLibraryResource(const ResourceLibrary& library, const std::string& type,
                                                   const std::string& id)
{
    if (type.empty() || id.empty())
    {
        return std::nullopt;
    }

    if (auto exact = library.LookupResource(type, id))
    {
        return exact;
    }

    if (id.find("__") == std::string::npos)
    {
        return std::nullopt;
    }

    const std::string suffix = IdSuffix(id);

    if (suffix.empty())
    {
        return std::nullopt;
    }

    for (const auto& resource : library.GetResourcesByType(type))
    {
        if (resource.id == suffix ||
            (resource.id.size() > suffix.size() + 2 &&
             resource.id.compare(resource.id.size() - suffix.size() - 2, std::string::npos, "__" + suffix) == 0))
        {
            return resource;
        }
    }

    return std::nullopt;
}

std::string DefaultExtension(const std::string& type)
{
    return type == "ir" ? ".wav" : type == "nam" ? ".nam" : ".bin";
}

/// archiveUtils.ts buildArchiveFileNameWithHash: "<hash>-<stem><ext>", the stem from the
/// library file's name, at most 120 characters in all.
std::string BuildArchiveFileNameWithHash(const LibraryResource& resource, const std::string& type,
                                         const std::string& hash)
{
    std::string baseName = resource.filePath.empty() ? std::string{} : util::PathToUtf8(resource.filePath.filename());

    if (baseName.empty())
    {
        baseName = SanitizeArchiveName(!resource.id.empty()     ? resource.id
                                       : !resource.name.empty() ? resource.name
                                                                : "resource") +
                   DefaultExtension(type);
    }

    std::string stem = Trim(baseName);
    std::string extension;

    if (const auto dot = stem.rfind('.'); dot != std::string::npos && dot > 0 && dot + 1 < stem.size())
    {
        extension = ToLowerAscii(stem.substr(dot));
        stem.resize(dot);
    }

    const std::string hashPrefix = SanitizeArchiveName(ToLowerAscii(Trim(hash)), "resource");

    if (extension.empty())
    {
        extension = DefaultExtension(type);
    }

    std::string sanitizedStem = SanitizeArchiveName(stem, SanitizeArchiveName(!resource.name.empty() ? resource.name
                                                                              : !resource.id.empty() ? resource.id
                                                                                                     : "resource",
                                                                              "resource"));

    const std::string marker = hashPrefix + "-";

    while (ToLowerAscii(sanitizedStem).rfind(marker, 0) == 0)
    {
        sanitizedStem.erase(0, marker.size());
    }

    if (sanitizedStem.empty())
    {
        sanitizedStem = "resource";
    }

    const std::size_t reserved = hashPrefix.size() + extension.size() + 1;
    const std::size_t maxStemLength =
        std::max<std::size_t>(16, kMaxArchiveFileNameLength > reserved ? kMaxArchiveFileNameLength - reserved : 0);

    std::string trimmedStem = sanitizedStem.substr(0, maxStemLength);

    while (!trimmedStem.empty() &&
           (trimmedStem.back() == '-' || trimmedStem.back() == '_' || trimmedStem.back() == '.'))
    {
        trimmedStem.pop_back();
    }

    if (trimmedStem.empty())
    {
        trimmedStem = "resource";
    }

    return hashPrefix + "-" + trimmedStem + extension;
}

/// archive.ts normalizeArchiveExportBaseName: sanitized, without archive suffixes or trailing dots.
std::string NormalizeExportBaseName(const std::string& raw, const std::string& fallback)
{
    std::string normalized = SanitizeArchiveName(raw, fallback);

    for (bool trimmed = true; trimmed;)
    {
        trimmed = false;
        const std::string lower = ToLowerAscii(normalized);

        for (const std::string suffix : {".soundshed.presets", ".soundshed.preset", ".zip"})
        {
            if (lower.size() >= suffix.size() &&
                lower.compare(lower.size() - suffix.size(), suffix.size(), suffix) == 0)
            {
                normalized.resize(normalized.size() - suffix.size());
                trimmed = true;
                break;
            }
        }
    }

    while (!normalized.empty() && normalized.back() == '.')
    {
        normalized.pop_back();
    }

    return normalized.empty() ? fallback : normalized;
}

/// Date.prototype.toISOString: "2026-10-27T09:41:05.123Z".
std::string IsoTimestampWithMilliseconds()
{
    const auto now = std::chrono::system_clock::now();
    const auto tt = std::chrono::system_clock::to_time_t(now);
    const auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count() % 1000;
    std::tm utcTime{};
#ifdef _WIN32
    gmtime_s(&utcTime, &tt);
#else
    gmtime_r(&tt, &utcTime);
#endif
    std::ostringstream oss;
    oss << std::put_time(&utcTime, "%Y-%m-%dT%H:%M:%S") << '.' << std::setw(3) << std::setfill('0') << millis << 'Z';
    return oss.str();
}

/// The preset's graph and each scene's.
std::vector<nlohmann::json*> PresetGraphs(nlohmann::json& preset)
{
    std::vector<nlohmann::json*> graphs;

    if (auto it = preset.find("graph"); it != preset.end() && it->is_object())
    {
        graphs.push_back(&*it);
    }

    if (auto scenes = preset.find("scenes"); scenes != preset.end() && scenes->is_array())
    {
        for (auto& scene : *scenes)
        {
            if (scene.is_object())
            {
                if (auto it = scene.find("graph"); it != scene.end() && it->is_object())
                {
                    graphs.push_back(&*it);
                }
            }
        }
    }

    return graphs;
}

template <typename Fn> void ForEachNode(nlohmann::json& preset, Fn&& fn)
{
    for (auto* graph : PresetGraphs(preset))
    {
        if (auto nodes = graph->find("nodes"); nodes != graph->end() && nodes->is_array())
        {
            for (auto& node : *nodes)
            {
                if (node.is_object())
                {
                    fn(node);
                }
            }
        }
    }
}

/// archive.ts collectPresetBlendIds, appended to `ids` in first-seen order.
void CollectPresetBlendIds(nlohmann::json preset, std::vector<std::string>& ids, std::unordered_set<std::string>& seen)
{
    ForEachNode(preset, [&](nlohmann::json& node) {
        const auto config = node.find("config");
        const std::string blendId = config != node.end() && config->is_object() ? StringField(*config, "blendId") : "";

        if (!blendId.empty() && seen.insert(blendId).second)
        {
            ids.push_back(blendId);
        }
    });
}

class RefCollector
{
  public:
    void Add(const std::string& type, const std::string& id)
    {
        if (!type.empty() && !id.empty() && mSeen.insert(type + ":" + id).second)
        {
            mRefs.push_back({type, id});
        }
    }

    /// archive.ts collectPresetResourceRefs: node resources, the legacy preset-level model, IR
    /// and attachments, then every model of the exported blends.
    void AddPreset(nlohmann::json preset, const nlohmann::json& blends)
    {
        ForEachNode(preset, [&](nlohmann::json& node) {
            if (auto resources = node.find("resources"); resources != node.end() && resources->is_array())
            {
                for (const auto& res : *resources)
                {
                    if (res.is_object())
                    {
                        Add(StringFieldOr(res, "resourceType", "type"), StringFieldOr(res, "resourceId", "id"));
                    }
                }
            }
        });

        Add("nam", StringField(preset, "audioFxModelId"));
        Add("ir", StringField(preset, "irId"));

        if (const auto attachments = preset.find("attachments"); attachments != preset.end() && attachments->is_array())
        {
            for (const auto& attachment : *attachments)
            {
                if (!attachment.is_object())
                {
                    continue;
                }

                const std::string type = StringField(attachment, "type");
                Add(type == "audiofx" ? "nam" : type, StringField(attachment, "id"));
            }
        }

        for (const auto& blend : blends)
        {
            if (const auto models = blend.find("models"); models != blend.end() && models->is_array())
            {
                for (const auto& model : *models)
                {
                    if (model.is_string())
                    {
                        Add("nam", model.get<std::string>());
                    }
                }
            }

            if (const auto mappings = blend.find("modelMappings"); mappings != blend.end() && mappings->is_array())
            {
                for (const auto& mapping : *mappings)
                {
                    if (mapping.is_object())
                    {
                        Add("nam", StringField(mapping, "id"));
                    }
                }
            }
        }
    }

    [[nodiscard]] const std::vector<ExportRef>& Refs() const
    {
        return mRefs;
    }

  private:
    std::vector<ExportRef> mRefs;
    std::unordered_set<std::string> mSeen;
};

/// sanitize.ts sanitizePresetForArchive, plus the file paths every importer drops.
void SanitizePresetForArchive(nlohmann::json& preset)
{
    preset.erase("globals");
    preset.erase("global");
    preset.erase("globalSignalChain");
    preset.erase("customModelPath");
    preset.erase("customIrPath");

    ForEachNode(preset, [](nlohmann::json& node) {
        if (auto resources = node.find("resources"); resources != node.end() && resources->is_array())
        {
            for (auto& res : *resources)
            {
                if (res.is_object())
                {
                    res.erase("filePath");
                }
            }
        }

        if (auto legacy = node.find("resource"); legacy != node.end() && legacy->is_object())
        {
            legacy->erase("filePath");
        }
    });

    if (auto attachments = preset.find("attachments"); attachments != preset.end() && attachments->is_array())
    {
        for (auto& attachment : *attachments)
        {
            if (attachment.is_object())
            {
                for (const char* key : {"filePath", "path", "customModelPath", "customIrPath"})
                {
                    attachment.erase(key);
                }
            }
        }
    }

    if (auto embedded = preset.find("embeddedResources"); embedded != preset.end() && embedded->is_array())
    {
        for (auto& resource : *embedded)
        {
            if (resource.is_object())
            {
                resource.erase("originalPath");
            }
        }
    }
}

/// archive.ts remapPresetResourceReferences: each model and IR onto the hash the archive files it
/// under. A node resource gets both `resourceId` and `id`, as the web UI writes them.
void RemapPresetResourceReferences(nlohmann::json& preset, const IdMap& idMap)
{
    ForEachNode(preset, [&](nlohmann::json& node) {
        if (auto resources = node.find("resources"); resources != node.end() && resources->is_array())
        {
            for (auto& res : *resources)
            {
                if (!res.is_object())
                {
                    continue;
                }

                const std::string resourceId = StringFieldOr(res, "resourceId", "id");

                if (const auto* mapped = resourceId.empty() ? nullptr : MappedId(idMap, resourceId))
                {
                    res["resourceId"] = *mapped;
                    res["id"] = *mapped;
                }
            }
        }
    });

    for (const char* key : {"audioFxModelId", "irId"})
    {
        const std::string id = StringField(preset, key);

        if (const auto* mapped = id.empty() ? nullptr : MappedId(idMap, id))
        {
            preset[key] = *mapped;
        }
    }

    if (auto attachments = preset.find("attachments"); attachments != preset.end() && attachments->is_array())
    {
        for (auto& attachment : *attachments)
        {
            const std::string id = attachment.is_object() ? StringField(attachment, "id") : std::string{};

            if (const auto* mapped = id.empty() ? nullptr : MappedId(idMap, id))
            {
                attachment["id"] = *mapped;
            }
        }
    }
}

/// archive.ts remapBlendResourceReferences.
void RemapBlendResourceReferences(nlohmann::json& blends, const IdMap& idMap)
{
    const auto remap = [&](nlohmann::json& id) {
        if (id.is_string())
        {
            if (const auto* mapped = MappedId(idMap, id.get<std::string>()))
            {
                id = *mapped;
            }
        }
    };

    for (auto& blend : blends)
    {
        if (auto models = blend.find("models"); models != blend.end() && models->is_array())
        {
            for (auto& model : *models)
            {
                remap(model);
            }
        }

        if (auto mappings = blend.find("modelMappings"); mappings != blend.end() && mappings->is_array())
        {
            for (auto& mapping : *mappings)
            {
                if (mapping.is_object() && mapping.contains("id"))
                {
                    remap(mapping["id"]);
                }
            }
        }
    }
}

/// folderArchive.ts buildArchivePresetFolder: the folder with only the exported presets, or null
/// when nothing exported is in it or below it.
nlohmann::json BuildArchivePresetFolder(const nlohmann::json& folder, const std::unordered_set<std::string>& allowed)
{
    nlohmann::json presetIds = nlohmann::json::array();

    if (const auto ids = folder.find("presetIds"); ids != folder.end() && ids->is_array())
    {
        for (const auto& id : *ids)
        {
            if (id.is_string() && allowed.contains(id.get<std::string>()))
            {
                presetIds.push_back(id);
            }
        }
    }

    nlohmann::json children = nlohmann::json::array();

    if (const auto kids = folder.find("children"); kids != folder.end() && kids->is_array())
    {
        for (const auto& child : *kids)
        {
            if (child.is_object())
            {
                if (auto built = BuildArchivePresetFolder(child, allowed); !built.is_null())
                {
                    children.push_back(std::move(built));
                }
            }
        }
    }

    if (presetIds.empty() && children.empty())
    {
        return nullptr;
    }

    nlohmann::json result = {{"name", folder.contains("name") ? folder["name"] : nlohmann::json("")}};

    if (!presetIds.empty())
    {
        result["presetIds"] = std::move(presetIds);
    }

    if (!children.empty())
    {
        result["children"] = std::move(children);
    }

    return result;
}

const nlohmann::json* FindFolderById(const nlohmann::json& folders, const std::string& folderId)
{
    if (!folders.is_array())
    {
        return nullptr;
    }

    for (const auto& folder : folders)
    {
        if (!folder.is_object())
        {
            continue;
        }

        if (StringField(folder, "id") == folderId)
        {
            return &folder;
        }

        if (const auto children = folder.find("children"); children != folder.end())
        {
            if (const auto* match = FindFolderById(*children, folderId))
            {
                return match;
            }
        }
    }

    return nullptr;
}

/// folderArchive.ts buildArchivePresetFoldersForExport.
nlohmann::json BuildArchivePresetFoldersForExport(const std::string& folderId, const nlohmann::json& folders,
                                                  const std::unordered_set<std::string>& allowed)
{
    nlohmann::json result = nlohmann::json::array();

    if (folderId == kFavoritesFolderId || folderId == kRecentsFolderId || allowed.empty() || !folders.is_array())
    {
        return result;
    }

    if (folderId != kAllPresetsFolderId)
    {
        if (const auto* folder = FindFolderById(folders, folderId))
        {
            if (auto built = BuildArchivePresetFolder(*folder, allowed); !built.is_null())
            {
                result.push_back(std::move(built));
            }
        }

        return result;
    }

    for (const auto& folder : folders)
    {
        if (folder.is_object())
        {
            if (auto built = BuildArchivePresetFolder(folder, allowed); !built.is_null())
            {
                result.push_back(std::move(built));
            }
        }
    }

    return result;
}

struct EffectPresetExport
{
    nlohmann::json byEffectType = nlohmann::json::object();
    std::vector<std::string> blendIds;
    std::vector<ExportRef> refs;
};

/// effectPresetExport.ts finishEffectPresetsForArchive: each model and IR moved onto its hash; an
/// entry naming something the archive does not carry is left out and counted.
nlohmann::json FinishEffectPresetsForArchive(const EffectPresetExport& collected, const IdMap& idMap,
                                             const nlohmann::json& blends, std::size_t& leftOut)
{
    std::unordered_set<std::string> carriedBlendIds;

    for (const auto& blend : blends)
    {
        carriedBlendIds.insert(StringField(blend, "id"));
    }

    nlohmann::json effectPresets = nlohmann::json::object();

    for (const auto& [effectType, entries] : collected.byEffectType.items())
    {
        nlohmann::json kept = nlohmann::json::array();

        for (const auto& entry : entries)
        {
            if (!entry.is_object())
            {
                continue;
            }

            bool carried = true;
            nlohmann::json resources = nlohmann::json::array();
            const auto entryResources = entry.find("resources");

            if (entryResources != entry.end() && entryResources->is_array())
            {
                for (const auto& ref : *entryResources)
                {
                    const std::string type = ref.is_object() ? StringFieldOr(ref, "resourceType", "type") : "";
                    const std::string id = ref.is_object() ? StringFieldOr(ref, "resourceId", "id") : "";
                    const auto mapped = idMap.find(id);

                    if (type.empty() || mapped == idMap.end())
                    {
                        carried = false;
                        break;
                    }

                    // The rest is a blend model's place on the sweep; a path or embedded id names a
                    // file on this machine.
                    nlohmann::json moved = ref;

                    for (const char* key : {"type", "id", "filePath", "embeddedId"})
                    {
                        moved.erase(key);
                    }

                    moved["resourceType"] = type;
                    moved["resourceId"] = mapped->second;
                    resources.push_back(std::move(moved));
                }
            }

            const auto config = entry.find("config");
            const std::string blendId =
                config != entry.end() && config->is_object() ? StringField(*config, "blendId") : std::string{};

            if (!carried || (!blendId.empty() && !carriedBlendIds.contains(blendId)))
            {
                ++leftOut;
                continue;
            }

            nlohmann::json out = nlohmann::json::object();

            for (const char* key : {"id", "name"})
            {
                if (entry.contains(key))
                {
                    out[key] = entry[key];
                }
            }

            const auto parameters = entry.find("parameters");
            out["parameters"] =
                parameters != entry.end() && parameters->is_object() ? *parameters : nlohmann::json::object();

            if (entryResources != entry.end() && !entryResources->is_null())
            {
                out["resources"] = std::move(resources);
            }

            if (config != entry.end() && !config->is_null())
            {
                out["config"] = *config;
            }

            kept.push_back(std::move(out));
        }

        if (!kept.empty())
        {
            effectPresets[effectType] = std::move(kept);
        }
    }

    return effectPresets;
}

std::vector<std::uint8_t> ToBytes(const std::string& text)
{
    return {text.begin(), text.end()};
}

std::string DumpArchiveJson(const nlohmann::json& json)
{
    // JSON.stringify(archive, null, 2). Text that is not UTF-8 is replaced rather than thrown on.
    return json.dump(2, ' ', false, nlohmann::json::error_handler_t::replace);
}

/// The zip as JSZip lays it out: the resources/ folder, its files, then the manifest.
std::optional<std::vector<std::uint8_t>> BuildArchiveZip(
    const std::vector<std::pair<std::string, std::vector<std::uint8_t>>>& resourceFiles,
    const std::string& manifestName, const std::string& manifestJson)
{
    mz_zip_archive zip{};

    if (!mz_zip_writer_init_heap(&zip, 0, 0))
    {
        return std::nullopt;
    }

    bool ok = mz_zip_writer_add_mem(&zip, "resources/", nullptr, 0, MZ_NO_COMPRESSION) != 0;

    for (const auto& [fileName, bytes] : resourceFiles)
    {
        const std::string entryName = "resources/" + fileName;
        ok = ok && mz_zip_writer_add_mem(&zip, entryName.c_str(), bytes.empty() ? nullptr : bytes.data(), bytes.size(),
                                         MZ_DEFAULT_COMPRESSION) != 0;
    }

    ok = ok && mz_zip_writer_add_mem(&zip, manifestName.c_str(), manifestJson.data(), manifestJson.size(),
                                     MZ_DEFAULT_COMPRESSION) != 0;

    void* buffer = nullptr;
    std::size_t size = 0;
    ok = ok && mz_zip_writer_finalize_heap_archive(&zip, &buffer, &size) != 0;

    std::optional<std::vector<std::uint8_t>> result;

    if (ok && buffer)
    {
        result.emplace(static_cast<std::uint8_t*>(buffer), static_cast<std::uint8_t*>(buffer) + size);
    }

    // finalize_heap_archive hands the buffer over, and writer_end then leaves it alone.
    if (buffer)
    {
        mz_free(buffer);
    }

    mz_zip_writer_end(&zip);
    return result;
}

std::string Plural(std::size_t count, const std::string& one, const std::string& many)
{
    return std::to_string(count) + " " + (count == 1 ? one : many);
}

/// archiveUtils.ts buildArchiveFileName: the library file's own name, or the id (else the name)
/// sanitized with the type's extension. A name already used in the same folder gets "-2", "-3"
/// before its extension, where JSZip would have written one file over the other.
std::string LibraryArchiveFileName(const LibraryResource& resource, const std::string& type,
                                   std::unordered_set<std::string>& used)
{
    std::string name = resource.filePath.empty() ? std::string{} : util::PathToUtf8(resource.filePath.filename());

    if (name.empty())
    {
        name = SanitizeArchiveName(!resource.id.empty()     ? resource.id
                                   : !resource.name.empty() ? resource.name
                                                            : "resource",
                                   "resource") +
               DefaultExtension(type);
    }

    if (used.insert(type + "/" + ToLowerAscii(name)).second)
    {
        return name;
    }

    const auto dot = name.rfind('.');
    const std::string stem = dot != std::string::npos && dot > 0 ? name.substr(0, dot) : name;
    const std::string extension = dot != std::string::npos && dot > 0 ? name.substr(dot) : std::string{};

    for (int n = 2;; ++n)
    {
        std::string candidate = stem + "-" + std::to_string(n) + extension;

        if (used.insert(type + "/" + ToLowerAscii(candidate)).second)
        {
            return candidate;
        }
    }
}
} // namespace

void PluginController::HandleExportPresetArchiveRequest(const nlohmann::json& payload)
{
    const auto fail = [this](const std::string& message) {
        SendMessageToUI(nlohmann::json{{"type", "presetExportFailed"}, {"message", message}}.dump());
    };

    std::vector<std::string> requestedIds;

    if (const auto ids = payload.find("presetIds"); ids != payload.end() && ids->is_array())
    {
        for (const auto& id : *ids)
        {
            if (id.is_string() && !id.get<std::string>().empty() &&
                std::find(requestedIds.begin(), requestedIds.end(), id.get<std::string>()) == requestedIds.end())
            {
                requestedIds.push_back(id.get<std::string>());
            }
        }
    }

    if (requestedIds.empty())
    {
        fail("No presets to export");
        return;
    }

    // One id is a one-preset archive unless the caller asks for a collection: a folder or the
    // whole library exported as a collection keeps its folders, however few presets it holds.
    const bool single =
        requestedIds.size() == 1 &&
        !(payload.contains("collection") && payload["collection"].is_boolean() && payload["collection"].get<bool>());
    const std::string archiveName = payload.value("name", std::string{});
    const std::string folderId = payload.value("folderId", std::string{});
    std::vector<std::string> warnings;

    // ── The presets, as the UI is sent them ─────────────────────────────────────
    std::vector<nlohmann::json> sourcePresets;
    std::size_t notFound = 0;

    for (const auto& id : requestedIds)
    {
        // The preset being edited: its working copy, which the web UI's cache holds too.
        const std::optional<Preset> preset =
            ResolveActivePresetSlotId(id) && mActivePreset ? mActivePreset : LoadPresetById(id);

        if (!preset)
        {
            ++notFound;
            continue;
        }

        sourcePresets.push_back(SerializePresetForUi(*preset));
    }

    if (sourcePresets.empty())
    {
        fail(requestedIds.size() == 1 ? "Preset not found" : "No presets to export");
        return;
    }

    if (notFound > 0)
    {
        warnings.push_back(Plural(notFound, "preset", "presets") + " could not be found");
    }

    // ── Effect presets, for factory archive authoring (collections only) ────────
    EffectPresetExport effectPresetExport;

    const auto settingIsTrue = [this](const char* key) {
        const auto it = mAppSettings.find(key);
        return it != mAppSettings.end() && it->is_boolean() && it->get<bool>();
    };
    const auto settingIsBool = [this](const char* key) {
        const auto it = mAppSettings.find(key);
        return it != mAppSettings.end() && it->is_boolean();
    };
    const bool archiveToolsOn = settingIsBool(kFactoryArchiveToolsSettingKey)
                                    ? settingIsTrue(kFactoryArchiveToolsSettingKey)
                                    : settingIsTrue(kLegacyAdvancedOptionsSettingKey);

    if (!single && archiveToolsOn && settingIsTrue(kExportEffectPresetsSettingKey))
    {
        const auto document = LoadUiStorageJson(kEffectPresetsDocument, nlohmann::json::object());
        const auto byEffectType =
            document.is_object() ? document.value("byEffectType", nlohmann::json::object()) : nlohmann::json::object();

        for (const auto& [effectType, entries] : byEffectType.items())
        {
            // Hosted plugins are left out: the machine the archive is installed on may not have one.
            if (effectType == EffectGuids::kPluginHost || !entries.is_array() || entries.empty())
            {
                continue;
            }

            effectPresetExport.byEffectType[effectType] = entries;

            for (const auto& entry : entries)
            {
                if (!entry.is_object())
                {
                    continue;
                }

                if (const auto resources = entry.find("resources"); resources != entry.end() && resources->is_array())
                {
                    for (const auto& ref : *resources)
                    {
                        if (ref.is_object())
                        {
                            effectPresetExport.refs.push_back(
                                {StringFieldOr(ref, "resourceType", "type"), StringFieldOr(ref, "resourceId", "id")});
                        }
                    }
                }

                if (const auto config = entry.find("config"); config != entry.end() && config->is_object())
                {
                    if (const std::string blendId = StringField(*config, "blendId"); !blendId.empty())
                    {
                        effectPresetExport.blendIds.push_back(blendId);
                    }
                }
            }
        }
    }

    // ── Blends they play, and the models and IRs of it all ──────────────────────
    std::vector<std::string> blendIds;
    std::unordered_set<std::string> seenBlendIds;

    for (const auto& preset : sourcePresets)
    {
        CollectPresetBlendIds(preset, blendIds, seenBlendIds);
    }

    for (const auto& blendId : effectPresetExport.blendIds)
    {
        seenBlendIds.insert(blendId);
    }

    nlohmann::json blendDefs = nlohmann::json::array();

    if (mBlendLibrary.is_array())
    {
        for (const auto& blend : mBlendLibrary)
        {
            if (blend.is_object() && seenBlendIds.contains(StringField(blend, "id")))
            {
                blendDefs.push_back(blend);
            }
        }
    }

    RefCollector refs;

    for (const auto& preset : sourcePresets)
    {
        refs.AddPreset(preset, blendDefs);
    }

    for (const auto& ref : effectPresetExport.refs)
    {
        refs.Add(ref.type, ref.id);
    }

    IdMap idMap;
    nlohmann::json exportResources = nlohmann::json::array();
    std::vector<std::pair<std::string, std::vector<std::uint8_t>>> resourceFiles;
    std::unordered_set<std::string> carriedFiles;
    std::size_t missing = 0;

    for (const auto& ref : refs.Refs())
    {
        if (!IsShareableResourceType(ref.type))
        {
            continue;
        }

        const auto resource = FindLibraryResource(mResourceLibrary, ref.type, ref.id);

        if (!resource)
        {
            ++missing;
            continue;
        }

        // As requestResourceData reads it for the web UI.
        ResourceRef lookup;
        lookup.resourceType = ref.type;
        lookup.resourceId = ref.id;
        const auto path = ResolveResourceRef(lookup);
        auto bytes = path && !path->empty() ? util::ReadFileBytes(*path) : std::vector<std::uint8_t>{};

        if (bytes.empty())
        {
            ++missing;
            continue;
        }

        const std::string hash = util::Sha256Hex(bytes);
        idMap[ref.id] = hash;

        if (ref.id.find("__") != std::string::npos)
        {
            idMap[IdSuffix(ref.id)] = hash;
        }

        if (!carriedFiles.insert(ref.type + ":" + hash).second)
        {
            continue;
        }

        const std::string fileName = BuildArchiveFileNameWithHash(*resource, ref.type, hash);
        nlohmann::json entry = {{"id", hash},       {"name", resource->name}, {"category", resource->category},
                                {"type", ref.type}, {"fileName", fileName},   {"hash", hash}};

        if (!resource->metadata.empty())
        {
            entry["metadata"] = resource->metadata;
        }

        exportResources.push_back(std::move(entry));
        resourceFiles.emplace_back(fileName, std::move(bytes));
    }

    if (missing > 0)
    {
        warnings.push_back(Plural(missing, "resource", "resources") + " could not be read");
    }

    // ── The manifest ────────────────────────────────────────────────────────────
    nlohmann::json exportPresets = nlohmann::json::array();
    std::unordered_set<std::string> exportedIds;

    for (auto preset : sourcePresets)
    {
        SanitizePresetForArchive(preset);
        RemapPresetResourceReferences(preset, idMap);
        exportedIds.insert(StringField(preset, "id"));
        exportPresets.push_back(std::move(preset));
    }

    nlohmann::json clonedBlends = blendDefs;
    RemapBlendResourceReferences(clonedBlends, idMap);

    nlohmann::json archive;
    std::string manifestName;
    std::string fileName;

    if (single)
    {
        archive = {{"formatVersion", 1},
                   {"preset", exportPresets[0]},
                   {"resources", exportResources},
                   {"blends", clonedBlends}};
        manifestName = "preset.json";

        const auto& preset = exportPresets[0];
        const std::string presetName = StringField(preset, "name");
        const std::string fallbackName = !presetName.empty()                  ? presetName
                                         : !StringField(preset, "id").empty() ? StringField(preset, "id")
                                                                              : std::string("preset");
        fileName = NormalizeExportBaseName(Trim(archiveName).empty() ? fallbackName : archiveName, "preset") +
                   ".soundshed.preset";
    }
    else
    {
        archive = {{"formatVersion", 1},
                   {"createdAt", IsoTimestampWithMilliseconds()},
                   {"presets", exportPresets},
                   {"resources", exportResources},
                   {"blends", clonedBlends}};

        if (!folderId.empty())
        {
            auto folders = LoadPresetFoldersDocument().value("folders", nlohmann::json::array());

            // The tree the UI is shown (HandleGetPresetFoldersRequest).
            if (!IsPresetArchiveSessionActive() && !IsFactoryPresetArchiveLoadingEnabled() && folders.is_array())
            {
                folders.erase(std::remove_if(folders.begin(), folders.end(),
                                             [](const nlohmann::json& folder) {
                                                 return folder.is_object() &&
                                                        IsFactoryArchiveFolderId(StringField(folder, "id"));
                                             }),
                              folders.end());
            }

            if (auto presetFolders = BuildArchivePresetFoldersForExport(folderId, folders, exportedIds);
                !presetFolders.empty())
            {
                archive["presetFolders"] = std::move(presetFolders);
            }
        }

        std::size_t leftOut = 0;

        if (auto effectPresets = FinishEffectPresetsForArchive(effectPresetExport, idMap, clonedBlends, leftOut);
            !effectPresets.empty())
        {
            archive["effectPresets"] = std::move(effectPresets);
        }

        if (leftOut > 0)
        {
            warnings.push_back(Plural(leftOut, "effect preset was", "effect presets were") +
                               " left out: each uses a model, IR or blend the archive could not carry");
        }

        manifestName = "presets.json";
        fileName = NormalizeExportBaseName(archiveName, "presets") + ".soundshed.presets";
    }

    auto zipBytes = BuildArchiveZip(resourceFiles, manifestName, DumpArchiveJson(archive));

    if (!zipBytes)
    {
        fail("Unable to create archive");
        return;
    }

    const std::size_t presetCount = exportPresets.size();
    const std::size_t resourceCount = exportResources.size();
    const std::string suffix = single ? ".soundshed.preset" : ".soundshed.presets";

    // As savePresetArchive saves the web UI's bytes, and answered the same way.
    mHost.SaveFileAsync(
        BrowseFileType::ArchiveFile, "Save Preset Archive", fileName,
        [this, bytes = std::move(*zipBytes), warnings, presetCount, resourceCount,
         suffix](const BrowseFileResult& result) {
            if (!result.success)
            {
                SendMessageToUI(nlohmann::json{{"type", "presetExportFailed"}, {"message", "Save cancelled"}}.dump());
                return;
            }

            // A dialog that hands back a bare name gets the archive's extension, so the file opens
            // as an archive again; one already named .soundshed.preset(s) or .zip is kept.
            auto chosenPath = result.path;
            const std::string chosenName = ToLowerAscii(util::PathToUtf8(chosenPath.filename()));
            const auto endsWith = [&chosenName](const std::string& tail) {
                return chosenName.size() >= tail.size() &&
                       chosenName.compare(chosenName.size() - tail.size(), tail.size(), tail) == 0;
            };

            if (!endsWith(".soundshed.preset") && !endsWith(".soundshed.presets") && !endsWith(".zip"))
            {
                chosenPath += util::PathFromUtf8(suffix);
            }

            const auto normalizedPath = NormalizePresetArchiveSavePath(chosenPath);

            if (!WriteFile(normalizedPath, bytes))
            {
                SendMessageToUI(
                    nlohmann::json{{"type", "presetExportFailed"}, {"message", "Failed to save file"}}.dump());
                return;
            }

            nlohmann::json reply = {{"type", "presetExportSaved"},
                                    {"path", util::PathToUtf8(normalizedPath)},
                                    {"presetCount", presetCount},
                                    {"resourceCount", resourceCount}};

            if (!warnings.empty())
            {
                reply["warnings"] = warnings;
            }

            SendMessageToUI(reply.dump());
            AppendSessionLog("Preset export saved: " + util::PathToUtf8(normalizedPath) + " (" +
                             std::to_string(presetCount) + " preset(s), " + std::to_string(resourceCount) +
                             " resource(s))");
        });
}

/// The library as one archive, `.soundshed-library.zip`: the user's presets, every blend, and the
/// models and IRs they use (resources "used", the default) or the whole library ("all"), in the
/// layout the web UI's libraryExport.ts wrote with JSZip: library.json, {formatVersion: 1,
/// createdAt, resourceMode, presets, blends, resources}, with each file under resources/nam/ or
/// resources/ir/ by its own name and listed as {id, name, category, type, fileName, hash, tags}.
/// Unlike a preset archive it keeps every id as the library has it: it is the library backed up,
/// not a pack to share. Presets are as the UI is sent them, the one being edited from its working
/// copy; factory presets kept in the user's store (mTrackedFactoryArchivePresetIds) are left out.
void PluginController::HandleExportLibraryArchiveRequest(const nlohmann::json& payload)
{
    const auto fail = [this](const std::string& message) {
        SendMessageToUI(nlohmann::json{{"type", "libraryExportFailed"}, {"message", message}}.dump());
    };

    const std::string resourceMode = payload.value("resources", std::string{}) == "all" ? "all" : "used";
    std::vector<std::string> warnings;

    std::vector<nlohmann::json> presets;

    for (const auto& stored : LoadAllUserPresets())
    {
        if (mTrackedFactoryArchivePresetIds.contains(stored.id))
        {
            continue;
        }

        const bool editing = ResolveActivePresetSlotId(stored.id) && mActivePreset;
        presets.push_back(SerializePresetForUi(editing ? *mActivePreset : stored));
    }

    const nlohmann::json blends = mBlendLibrary.is_array() ? mBlendLibrary : nlohmann::json::array();

    if (presets.empty() && blends.empty())
    {
        fail("No presets or blends available");
        return;
    }

    // ── Which models and IRs ────────────────────────────────────────────────────
    std::vector<std::pair<std::string, LibraryResource>> entries;

    if (resourceMode == "all")
    {
        for (const char* type : {"nam", "ir"})
        {
            for (auto& resource : mResourceLibrary.GetResourcesByType(type))
            {
                entries.emplace_back(type, std::move(resource));
            }
        }
    }
    else
    {
        // The blends the presets play, and through them their models (resourceUsage.ts).
        std::vector<std::string> blendIds;
        std::unordered_set<std::string> seenBlendIds;

        for (const auto& preset : presets)
        {
            CollectPresetBlendIds(preset, blendIds, seenBlendIds);
        }

        nlohmann::json playedBlends = nlohmann::json::array();

        for (const auto& blend : blends)
        {
            if (blend.is_object() && seenBlendIds.contains(StringField(blend, "id")))
            {
                playedBlends.push_back(blend);
            }
        }

        RefCollector refs;

        for (const auto& preset : presets)
        {
            refs.AddPreset(preset, playedBlends);
        }

        for (const auto& ref : refs.Refs())
        {
            if (ref.type != "nam" && ref.type != "ir")
            {
                continue;
            }

            if (auto resource = FindLibraryResource(mResourceLibrary, ref.type, ref.id))
            {
                entries.emplace_back(ref.type, std::move(*resource));
            }
        }
    }

    // ── The files ───────────────────────────────────────────────────────────────
    nlohmann::json exportResources = nlohmann::json::array();
    std::vector<std::pair<std::string, std::vector<std::uint8_t>>> files{{"nam/", {}}, {"ir/", {}}};
    std::unordered_set<std::string> usedNames;
    std::size_t missing = 0;

    for (const auto& [type, resource] : entries)
    {
        ResourceRef lookup;
        lookup.resourceType = type;
        lookup.resourceId = resource.id;
        const auto path = ResolveResourceRef(lookup);
        auto bytes = path && !path->empty() ? util::ReadFileBytes(*path) : std::vector<std::uint8_t>{};

        if (bytes.empty())
        {
            ++missing;
            continue;
        }

        const std::string fileName = LibraryArchiveFileName(resource, type, usedNames);
        exportResources.push_back({{"id", resource.id},
                                   {"name", resource.name},
                                   {"category", resource.category},
                                   {"type", type},
                                   {"fileName", fileName},
                                   {"hash", util::Sha256Hex(bytes)},
                                   {"tags", resource.tags}});
        files.emplace_back(type + "/" + fileName, std::move(bytes));
    }

    if (missing > 0)
    {
        warnings.push_back(Plural(missing, "resource", "resources") + " could not be read");
    }

    const nlohmann::json archive = {{"formatVersion", 1},
                                    {"createdAt", IsoTimestampWithMilliseconds()},
                                    {"resourceMode", resourceMode},
                                    {"presets", presets},
                                    {"blends", blends},
                                    {"resources", exportResources}};

    auto zipBytes = BuildArchiveZip(files, "library.json", DumpArchiveJson(archive));

    if (!zipBytes)
    {
        fail("Unable to create archive");
        return;
    }

    const std::size_t presetCount = presets.size();
    const std::size_t resourceCount = exportResources.size();

    // As saveLibraryArchive saves the web UI's bytes, and answered the same way.
    mHost.SaveFileAsync(
        BrowseFileType::ArchiveFile, "Save Library Export", "Soundshed-Library.soundshed-library.zip",
        [this, bytes = std::move(*zipBytes), warnings, presetCount, resourceCount](const BrowseFileResult& result) {
            if (!result.success)
            {
                SendMessageToUI(nlohmann::json{{"type", "libraryExportFailed"}, {"message", "Save cancelled"}}.dump());
                return;
            }

            // A bare name chosen in the dialog gets the archive's extension.
            auto chosenPath = result.path;
            const std::string chosenName = ToLowerAscii(util::PathToUtf8(chosenPath.filename()));

            if (chosenName.size() < 4 || chosenName.compare(chosenName.size() - 4, 4, ".zip") != 0)
            {
                chosenPath += util::PathFromUtf8(".soundshed-library.zip");
            }

            if (!WriteFile(chosenPath, bytes))
            {
                SendMessageToUI(
                    nlohmann::json{{"type", "libraryExportFailed"}, {"message", "Failed to save file"}}.dump());
                return;
            }

            nlohmann::json reply = {{"type", "libraryExportSaved"},
                                    {"path", util::PathToUtf8(chosenPath)},
                                    {"presetCount", presetCount},
                                    {"resourceCount", resourceCount}};

            if (!warnings.empty())
            {
                reply["warnings"] = warnings;
            }

            SendMessageToUI(reply.dump());
            AppendSessionLog("Library export saved: " + util::PathToUtf8(chosenPath) + " (" +
                             std::to_string(presetCount) + " preset(s), " + std::to_string(resourceCount) +
                             " resource(s))");
        });
}
} // namespace guitarfx

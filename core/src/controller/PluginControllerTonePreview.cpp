/**
 * PluginControllerTonePreview.cpp - Auditioning a shared tone without installing it.
 *
 * A tone sharing preview must not write to the user's library: no preset, blend or resource is
 * saved until the tone is installed. Its models and IRs still have to play, though, or the
 * preview sounds nothing like the tone (an amp model missing leaves the guitar clean). So the UI
 * sends the files the library lacks along with the preset ("previewPreset"): the archive's own,
 * and the Tone3000 ones it names, fetched by their ids. They are written to a folder of this
 * preview's own under the settings folder's temp/, and the preset's nodes are pointed at them by
 * file path. The library is never told, so nothing appears in it, and a later install fetches
 * and imports the files as it always did.
 *
 * The files stay while the preview can still play them: a scene switch or a rebuild reads them
 * again. The next preview replaces the folder, and one left by a crashed or closed instance is
 * removed by the next preview after a day.
 */

#include "PluginController.h"

#include "controller/internal/PresetArchiveSupport.h"
#include "presets/PresetStorage.h"
#include "util/Base64.h"
#include "util/PathEncoding.h"
#include "util/PathSanitizer.h"

#include <chrono>
#include <filesystem>
#include <map>
#include <string>
#include <unordered_set>
#include <utility>

using namespace guitarfx::controller_detail;

namespace guitarfx
{
namespace
{
constexpr const char* kTonePreviewFolder = "tone-preview";
constexpr std::size_t kMaxPreviewFileName = 72;
constexpr auto kStalePreviewAge = std::chrono::hours(24);

using StagedFiles = std::map<std::pair<std::string, std::string>, std::filesystem::path>;

/// Removes the preview folders other instances left behind more than a day ago. A younger one may
/// still be playing in another instance sharing this settings folder.
void RemoveStalePreviewFolders(const std::filesystem::path& root)
{
    std::error_code ec;
    const auto now = std::filesystem::file_time_type::clock::now();

    for (std::filesystem::directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec))
    {
        std::error_code entryEc;
        const auto written = it->last_write_time(entryEc);

        if (!entryEc && it->is_directory(entryEc) && now - written > kStalePreviewAge)
        {
            std::filesystem::remove_all(it->path(), entryEc);
        }
    }
}

/// A file name for a staged resource, unique within its folder: the resource's own name, so the
/// node shows what it plays.
std::string PreviewFileName(const std::string& name, const std::string& resourceType,
                            std::unordered_set<std::string>& taken)
{
    const std::string extension = resourceType == "ir" ? ".wav" : ".nam";
    const std::string stem = util::SanitizeFilename(name);
    std::string candidate = util::LimitFilenameLength(stem + extension, kMaxPreviewFileName);

    for (int suffix = 2; taken.contains(candidate); ++suffix)
    {
        candidate = util::LimitFilenameLength(stem + "-" + std::to_string(suffix) + extension, kMaxPreviewFileName);
    }

    taken.insert(candidate);
    return candidate;
}

/// Points every node naming a staged resource at its file. The id stays, so a library that holds
/// the resource after all (the tone was installed meanwhile) still wins.
void PointPresetAtStagedFiles(Preset& preset, const StagedFiles& staged)
{
    auto pointGraph = [&](SignalGraph& graph) {
        for (auto& node : graph.nodes)
        {
            for (auto& ref : node.resources)
            {
                const auto it = staged.find({ref.resourceType, ref.resourceId});

                if (it != staged.end())
                {
                    ref.filePath = it->second;
                }
            }
        }
    };

    pointGraph(preset.graph);

    for (auto& scene : preset.scenes)
    {
        pointGraph(scene.graph);
    }
}
} // namespace

void PluginController::HandlePreviewPresetRequest(const nlohmann::json& payload)
{
    if (!payload.contains("preset") || !payload["preset"].is_object())
    {
        ReportErrorToUI("Preview failed", "No preset to preview");
        return;
    }

    auto presetOpt = PresetStorage::DeserializeFromJson(payload["preset"].dump());

    if (!presetOpt)
    {
        ReportErrorToUI("Preview failed", "The preset could not be read");
        return;
    }

    Preset preset = std::move(*presetOpt);
    // Any path the shared preset carries is the sharer's; only the files staged below play.
    SanitizeImportedPreset(preset);

    const auto previewRoot = mFileSystem.ResolveSettingsDirectory() / "temp" / kTonePreviewFolder;
    std::error_code ec;

    if (!mTonePreviewDir.empty())
    {
        std::filesystem::remove_all(mTonePreviewDir, ec);
        mTonePreviewDir.clear();
    }

    RemoveStalePreviewFolders(previewRoot);

    StagedFiles staged;
    const auto resources = payload.value("resources", nlohmann::json::array());

    if (resources.is_array() && !resources.empty())
    {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        const auto previewDir = previewRoot / std::to_string(stamp);
        [[maybe_unused]] const auto ensuredDir = mFileSystem.EnsureDirectory(previewDir);
        std::unordered_set<std::string> takenNames;

        for (const auto& resource : resources)
        {
            if (!resource.is_object())
            {
                continue;
            }

            const std::string resourceType = resource.value("resourceType", "");
            const std::string resourceId = resource.value("resourceId", "");

            if ((resourceType != "nam" && resourceType != "ir") || resourceId.empty() ||
                staged.contains({resourceType, resourceId}))
            {
                continue;
            }

            const auto bytes = util::DecodeBase64(resource.value("data", ""));

            if (bytes.empty())
            {
                continue;
            }

            const auto path =
                previewDir / PreviewFileName(resource.value("name", resourceId), resourceType, takenNames);

            if (!WriteFile(path, bytes))
            {
                AppendSessionLog("Tone preview: could not write " + util::PathToUtf8(path));
                continue;
            }

            staged[{resourceType, resourceId}] = path;
        }

        mTonePreviewDir = previewDir;
    }

    PointPresetAtStagedFiles(preset, staged);
    AppendSessionLog("Tone preview: " + std::to_string(staged.size()) + " file(s) staged for " + preset.name);

    nlohmann::json load;
    load["preset"] = nlohmann::json::parse(PresetStorage::SerializeToJson(preset));
    load["presetId"] = payload.value("presetId", preset.id);
    HandlePresetLoadRequest(load);
}
} // namespace guitarfx

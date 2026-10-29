/**
 * PluginControllerArchiveImport.cpp - Importing a preset archive file the user picked or dropped
 * (importPresetArchiveFile): read here and handed to the tone sharing install, as the web UI's
 * Import Preset and a dropped .soundshed.preset(s) do (presets/archive.ts
 * importPackWithConfirmation). The archive's name titles a folder of its own, and the install is
 * recorded as a zip import, so it can be removed again like any other.
 */

#include "PluginController.h"

#include "util/Base64.h"
#include "util/FileIO.h"
#include "util/PathEncoding.h"

#include <chrono>

namespace guitarfx
{
namespace
{
/// "Metal Pack.soundshed.presets" reads "Metal Pack".
std::string ArchiveTitle(const std::string& fileName)
{
    for (const char* suffix : {".soundshed.presets", ".soundshed.preset", ".zip"})
    {
        const std::string tail = suffix;

        if (fileName.size() > tail.size() && fileName.compare(fileName.size() - tail.size(), tail.size(), tail) == 0)
        {
            return fileName.substr(0, fileName.size() - tail.size());
        }
    }

    return fileName;
}
} // namespace

void PluginController::HandleImportPresetArchiveFileRequest(const nlohmann::json& payload)
{
    const std::string requestId = payload.value("requestId", "");
    const std::string path = payload.value("path", "");

    if (!path.empty())
    {
        ImportPresetArchiveFile(util::PathFromUtf8(path), requestId);
        return;
    }

    mHost.BrowseFileAsync(BrowseFileType::ArchiveFile, "Import Preset",
                          [this, requestId](const BrowseFileResult& result) {
                              if (result.success)
                              {
                                  ImportPresetArchiveFile(result.path, requestId);
                              }
                          });
}

void PluginController::ImportPresetArchiveFile(const std::filesystem::path& path, std::string requestId)
{
    const std::string fileName = util::PathToUtf8(path.filename());
    const auto bytes = util::ReadFileBytes(path);

    if (requestId.empty())
    {
        const auto now =
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
                .count();
        requestId = "zip-" + std::to_string(now);
    }

    if (bytes.empty())
    {
        ReportErrorToUI("Import failed", "Could not read " + fileName);
        SendMessageToUI(nlohmann::json{{"type", "presetArchivesInstallFailed"},
                                       {"requestId", requestId},
                                       {"entryId", requestId},
                                       {"message", "Import failed"},
                                       {"detail", "Could not read " + fileName}}
                            .dump());
        return;
    }

    const std::string title = ArchiveTitle(fileName);
    HandleInstallPresetArchivesRequest(
        {{"requestId", requestId},
         {"entry", {{"id", requestId}, {"title", title}, {"source", "zipImport"}, {"archiveFileName", fileName}}},
         {"folder", title},
         {"archives", nlohmann::json::array({{{"title", title}, {"data", util::EncodeBase64(bytes)}}})}});
}
} // namespace guitarfx

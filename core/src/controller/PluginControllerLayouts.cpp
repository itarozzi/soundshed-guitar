/**
 * PluginControllerLayouts.cpp - Custom effect layouts and their images.
 *
 * A layout is an index entry in one shared settings file plus a content
 * directory holding its images. Every save copies each image the layout draws
 * on into that directory, so a layout never depends on another's folder, and a
 * delete first lets every other layout take its own copy of what it borrowed.
 * Images arriving from an imported archive are named by the archive, so their
 * names are checked before anything is written.
 */

#include "PluginController.h"

#include "controller/internal/ControllerUtils.h"
#include "controller/internal/LayoutSupport.h"
#include "util/Base64.h"
#include "util/PathEncoding.h"

#include <algorithm>
#include <cctype>
#include <fstream>

using namespace guitarfx::controller_detail;

namespace guitarfx
{
void PluginController::HandleDeleteLayoutRequest(const nlohmann::json& payload)
{
    const std::string effectType = payload.value("effectType", "");
    const std::string layoutId = payload.value("layoutId", "");

    if (effectType.empty())
    {
        ReportErrorToUI("Delete layout failed", "Missing effect type");
        return;
    }

    if (layoutId.empty())
    {
        ReportErrorToUI("Delete layout failed", "Missing layoutId");
        return;
    }

    nlohmann::json settings = LoadEffectLayoutsSettings(mFileSystem);
    const auto log = [this](const std::string& line) { AppendSessionLog(line); };

    // Any image picker can choose an image from another layout's folder, and this one's folder
    // goes below. Every other layout takes its own copy of what it draws on first.
    if (settings.contains("associations") && settings["associations"].is_object())
    {
        const auto factoryDir = ResolveFactoryLayoutsDir(mHost.GetBundledAssetsPath());

        for (const auto& [key, entry] : settings["associations"].items())
        {
            if (!entry.is_object() || !entry.contains("layoutIds") || !entry["layoutIds"].is_array())
            {
                continue;
            }

            for (const auto& id : entry["layoutIds"])
            {
                if (!id.is_string() || id.get<std::string>() == layoutId)
                {
                    continue;
                }

                if (const auto other = Store().Get(storage::ItemType::kLayout, id.get<std::string>()))
                {
                    CopyMissingLayoutImages(mFileSystem, factoryDir, id.get<std::string>(),
                                            CollectLayoutImageIds(*other), log);
                }
            }
        }
    }

    if (!Store().Remove(storage::ItemType::kLayout, layoutId))
    {
        ReportErrorToUI("Delete layout failed", "The layout could not be removed from the library");
        LoadLayoutLibrary(); // the UI dropped it already; put it back
        return;
    }

    AppendSessionLog("Layout deleted: " + layoutId);

    // Out of every association, not only the one the UI named: the same layout can be listed
    // under an effect type and a blend of it, and a listing left behind points at nothing.
    if (settings.contains("associations") && settings["associations"].is_object())
    {
        auto& assoc = settings["associations"];
        std::vector<std::string> emptied;

        for (auto& [key, entry] : assoc.items())
        {
            if (!entry.is_object())
            {
                continue;
            }

            const auto ids = entry.contains("layoutIds") && entry["layoutIds"].is_array() ? entry["layoutIds"]
                                                                                          : nlohmann::json::array();
            nlohmann::json updated = nlohmann::json::array();

            for (const auto& id : ids)
            {
                if (!(id.is_string() && id.get<std::string>() == layoutId))
                {
                    updated.push_back(id);
                }
            }

            entry["layoutIds"] = updated;

            if (entry.contains("defaultLayoutId") && entry["defaultLayoutId"].is_string() &&
                entry["defaultLayoutId"].get<std::string>() == layoutId)
            {
                entry["defaultLayoutId"] =
                    !updated.empty() && updated[0].is_string() ? updated[0].get<std::string>() : std::string{};
            }

            if (updated.empty())
            {
                emptied.push_back(key);
            }
        }

        for (const auto& key : emptied)
        {
            assoc.erase(key);
        }
    }

    if (!SaveEffectLayoutsSettings(mFileSystem, settings))
    {
        AppendSessionLog("Delete layout: the layout index could not be written");
    }

    // The image folder last, and only as tidying: the layout is gone from the library either
    // way, and a folder an antivirus scan holds open must not leave it half deleted.
    const auto layoutDir = ResolveLayoutDir(mFileSystem, layoutId);
    std::error_code ec;
    std::filesystem::remove_all(layoutDir, ec);

    if (ec)
    {
        AppendSessionLog("Delete layout: could not remove " + util::PathToUtf8(layoutDir) + ": " + ec.message());
    }

    LoadLayoutLibrary();
}

void PluginController::HandleSaveEffectLayoutRequest(const nlohmann::json& payload)
{
    const std::string effectType = payload.value("effectType", "");
    const std::string blendId = payload.value("blendId", "");
    std::string layoutId = payload.value("layoutId", "");
    const auto layoutIt = payload.find("layout");
    const auto reportFailure = [this, &layoutId](const std::string& reason) {
        AppendSessionLog("Save layout failed (" + layoutId + "): " + reason);
        SendMessageToUI(
            nlohmann::json{{"type", "layoutSaveFailed"}, {"layoutId", layoutId}, {"message", reason}}.dump());
    };

    if (effectType.empty() || layoutIt == payload.end() || !layoutIt->is_object())
    {
        reportFailure("Missing effect type or layout data");
        return;
    }

    const std::string lookupKey = blendId.empty() ? effectType : (effectType + "::" + blendId);

    if (layoutId.empty())
    {
        layoutId = GenerateGuidV4String();
    }

    nlohmann::json layoutJson = *layoutIt;
    layoutJson["layoutId"] = layoutId;

    if (!SaveLayoutToFile(layoutId, layoutJson))
    {
        reportFailure("The layout could not be written to the library");
        return;
    }

    // Every image the layout draws on gets a copy in its own folder, on every save rather than
    // only the first: one picked from another layout's folder would otherwise go with that layout
    // when it is deleted, and a factory layout's fork would depend on the bundled folder.
    for (const auto& imageId : CopyMissingLayoutImages(
             mFileSystem, ResolveFactoryLayoutsDir(mHost.GetBundledAssetsPath()), layoutId,
             CollectLayoutImageIds(layoutJson), [this](const std::string& line) { AppendSessionLog(line); }))
    {
        AppendSessionLog("Layout " + layoutId + " refers to an image that is not in the library: " + imageId);
    }

    // Update association mapping
    nlohmann::json settings = LoadEffectLayoutsSettings(mFileSystem);

    if (!settings.contains("associations") || !settings["associations"].is_object())
    {
        settings["associations"] = nlohmann::json::object();
    }

    if (!settings["associations"].contains(lookupKey) || !settings["associations"][lookupKey].is_object())
    {
        settings["associations"][lookupKey] =
            nlohmann::json::object({{"defaultLayoutId", layoutId}, {"layoutIds", nlohmann::json::array()}});
    }

    auto& assocEntry = settings["associations"][lookupKey];
    auto ids = assocEntry.value("layoutIds", nlohmann::json::array());

    if (!ids.is_array())
    {
        ids = nlohmann::json::array();
    }

    bool found = false;

    for (const auto& id : ids)
    {
        if (id.is_string() && id.get<std::string>() == layoutId)
        {
            found = true;
            break;
        }
    }

    if (!found)
    {
        ids.push_back(layoutId);
    }

    assocEntry["layoutIds"] = ids;
    assocEntry["defaultLayoutId"] = layoutId;

    if (!SaveEffectLayoutsSettings(mFileSystem, settings))
    {
        reportFailure("The layout index could not be written");
        LoadLayoutLibrary();
        return;
    }

    SendMessageToUI(nlohmann::json{
        {"type", "layoutSaved"},
        {"effectType", effectType},
        {"blendId", blendId},
        {"lookupKey", lookupKey},
        {"layoutId", layoutId},
        {"layout",
         layoutJson}}.dump());

    AppendSessionLog("Effect layout saved: " + lookupKey + " -> " + layoutId);

    // Broadcast updated library so UI can select/apply immediately.
    LoadLayoutLibrary();
}

void PluginController::HandleExportEffectLayoutRequest(const nlohmann::json& payload)
{
    const std::string dataEncoded = payload.value("data", "");
    const std::string suggestedName = payload.value("fileName", "layout.sgfxlayout.zip");

    if (dataEncoded.empty())
    {
        SendMessageToUI(nlohmann::json{{"type", "layoutExportFailed"}, {"message", "Missing export data"}}.dump());
        return;
    }

    mHost.SaveFileAsync(
        BrowseFileType::ArchiveFile, "Export Effect Layout", suggestedName,
        [this, dataEncoded](const BrowseFileResult& result) {
            if (!result.success)
            {
                SendMessageToUI(nlohmann::json{{"type", "layoutExportFailed"}, {"message", "Export cancelled"}}.dump());
                return;
            }

            const auto decodedBytes = util::DecodeBase64(dataEncoded);

            if (decodedBytes.empty())
            {
                SendMessageToUI(
                    nlohmann::json{{"type", "layoutExportFailed"}, {"message", "Invalid export data"}}.dump());
                return;
            }

            if (!WriteFile(result.path, decodedBytes))
            {
                SendMessageToUI(
                    nlohmann::json{{"type", "layoutExportFailed"}, {"message", "Failed to write file"}}.dump());
                return;
            }

            SendMessageToUI(
                nlohmann::json{{"type", "layoutExportSaved"}, {"path", util::PathToUtf8(result.path)}}.dump());
            AppendSessionLog("Layout exported: " + util::PathToUtf8(result.path));
        });
}

void PluginController::HandleBrowseLayoutImageRequest(const nlohmann::json& payload)
{
    const std::string purpose = payload.value("purpose", "");
    const int layerIndex = payload.value("layerIndex", 0);
    const std::string paramKey = payload.value("paramKey", "");
    const std::string layoutId = payload.value("layoutId", "");

    // An image goes in the folder of the layout it is for. Without an id it would land in the
    // legacy shared folder, which the loader only reads for old layouts' sake.
    if (layoutId.empty())
    {
        ReportErrorToUI("Image import failed", "Missing layoutId");
        return;
    }

    mHost.BrowseFileAsync(
        BrowseFileType::ImageFile, "Select Image",
        [this, purpose, layerIndex, paramKey, layoutId](const BrowseFileResult& result) {
            if (!result.success)
            {
                return;
            }

            const auto selectedPath = result.path;
            const std::string mimeType = LayoutImageMimeType(selectedPath.extension());

            if (mimeType.empty())
            {
                ReportErrorToUI("Image import failed", "Layouts take PNG, JPEG, GIF, WebP or SVG images");
                return;
            }

            std::error_code sizeEc;
            const auto size = std::filesystem::file_size(selectedPath, sizeEc);

            if (!sizeEc && size > kMaxLayoutImageBytes)
            {
                ReportErrorToUI("Image import failed", "The image is larger than 16 MB");
                return;
            }

            const auto imagesDir = ResolveLayoutDir(mFileSystem, layoutId) / "images";
            [[maybe_unused]] const auto ensuredImagesDir = mFileSystem.EnsureDirectory(imagesDir);

            // The extension in lower case, so "IMG_0042.JPG" is read back like any other JPEG.
            std::string extension = util::PathToUtf8(selectedPath.extension());
            std::transform(extension.begin(), extension.end(), extension.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            const auto timestamp = std::chrono::system_clock::now().time_since_epoch().count();
            const std::string imageId = util::PathToUtf8(selectedPath.stem()) + "_" + std::to_string(timestamp);
            const std::string destFilename = imageId + extension;
            const auto destPath = imagesDir / util::PathFromUtf8(destFilename);

            try
            {
                std::filesystem::copy_file(selectedPath, destPath, std::filesystem::copy_options::overwrite_existing);

                std::ifstream imageFile(destPath, std::ios::binary);

                if (!imageFile)
                {
                    ReportErrorToUI("Image import failed", "Failed to read copied image file");
                    return;
                }

                std::vector<std::uint8_t> imageData((std::istreambuf_iterator<char>(imageFile)),
                                                    std::istreambuf_iterator<char>());
                imageFile.close();

                const std::string dataUrl = "data:" + mimeType + ";base64," + util::EncodeBase64(imageData);

                SendMessageToUI(nlohmann::json{
                    {"type", "layoutImageSelected"},
                    {"purpose", purpose},
                    {"imageId", imageId},
                    {"fileName", destFilename},
                    {"dataUrl", dataUrl},
                    {"layerIndex", layerIndex},
                    {"paramKey",
                     paramKey}}.dump());
            }
            catch (const std::exception& e)
            {
                AppendSessionLog("Failed to copy layout image: " + std::string(e.what()));
                ReportErrorToUI("Image import failed", "Failed to copy image file");
            }
        });
}

void PluginController::HandleSaveLayoutImageRequest(const nlohmann::json& payload)
{
    const std::string imageId = payload.value("imageId", "");
    const std::string fileName = payload.value("fileName", "");
    const std::string dataEncoded = payload.value("data", "");
    const std::string layoutId = payload.value("layoutId", "");

    if (imageId.empty() || fileName.empty() || dataEncoded.empty() || layoutId.empty())
    {
        AppendSessionLog("SaveLayoutImage: missing required fields");
        ReportErrorToUI("Image import failed", "The layout's image was incomplete");
        return;
    }

    // The name comes from an imported archive's manifest, so the archive chooses it. A name
    // with a folder or a drive in it would be written wherever it points.
    if (!IsSafeLayoutImageFileName(fileName, imageId))
    {
        AppendSessionLog("SaveLayoutImage: refused the file name " + fileName);
        ReportErrorToUI("Image import failed", "The layout's image has a file name that cannot be used");
        return;
    }

    const auto decodedBytes = util::DecodeBase64(dataEncoded);

    if (decodedBytes.empty() || decodedBytes.size() > kMaxLayoutImageBytes ||
        !LooksLikeLayoutImage(decodedBytes, util::PathFromUtf8(fileName).extension()))
    {
        AppendSessionLog("SaveLayoutImage: refused " + fileName + " (" + std::to_string(decodedBytes.size()) +
                         " bytes): not an image of its type, or too large");
        ReportErrorToUI("Image import failed", "The layout's image is not a supported image, or is larger than 16 MB");
        return;
    }

    const auto imagesDir = ResolveLayoutDir(mFileSystem, layoutId) / "images";
    [[maybe_unused]] const auto ensuredImagesDir = mFileSystem.EnsureDirectory(imagesDir);
    const auto destPath = imagesDir / util::PathFromUtf8(fileName);

    if (WriteFile(destPath, decodedBytes))
    {
        AppendSessionLog("Layout image saved from import: " + util::PathToUtf8(destPath));
    }
    else
    {
        AppendSessionLog("SaveLayoutImage: failed to write " + util::PathToUtf8(destPath));
        ReportErrorToUI("Image import failed", "The layout's image could not be written");
    }
}

void PluginController::LoadLayoutLibrary()
{
    nlohmann::json library;
    library["byEffectType"] = nlohmann::json::object();
    library["defaults"] = nlohmann::json::object();
    // Layout images (base64-encoded backgrounds) are intentionally omitted here to keep
    // app startup lightweight; they are loaded on demand via BuildLayoutImages() when the
    // layout designer/manager requests them. Per-layout thumbnailDataUrl values remain
    // embedded in each layout entry below since they are used for signal-path node avatars.
    library["images"] = nlohmann::json::array();

    // Build user layout library from the associations index.
    // Each layout lives in its own subfolder: layouts/content/<layoutId>/layout.json
    // Images live in: layouts/content/<layoutId>/images/
    nlohmann::json settings = LoadEffectLayoutsSettings(mFileSystem);

    if (settings.contains("associations") && settings["associations"].is_object())
    {
        for (auto it = settings["associations"].begin(); it != settings["associations"].end(); ++it)
        {
            const std::string lookupKey = it.key();
            const auto& assocEntry = it.value();

            if (!assocEntry.is_object())
            {
                continue;
            }

            const std::string defaultLayoutId = assocEntry.value("defaultLayoutId", "");
            const auto ids = assocEntry.value("layoutIds", nlohmann::json::array());

            if (!ids.is_array())
            {
                continue;
            }

            nlohmann::json entries = nlohmann::json::array();

            for (const auto& id : ids)
            {
                if (!id.is_string())
                {
                    continue;
                }

                const std::string layoutId = id.get<std::string>();
                auto layoutJson = Store().Get(storage::ItemType::kLayout, layoutId);

                if (!layoutJson || !layoutJson->is_object())
                {
                    continue;
                }

                // Ensure layoutId is embedded for UI round-trip.
                (*layoutJson)["layoutId"] = layoutId;

                nlohmann::json layoutEntry;
                layoutEntry["layout"] = std::move(*layoutJson);
                layoutEntry["isDefault"] = (layoutId == defaultLayoutId);
                layoutEntry["layoutId"] = layoutId;
                // Sidecar images still live on disk next to where the layout
                // document used to be; the designer resolves them from here.
                layoutEntry["filePath"] = util::PathToUtf8(ResolveLayoutFilePath(mFileSystem, layoutId));
                entries.push_back(layoutEntry);
            }

            if (!entries.empty())
            {
                library["byEffectType"][lookupKey] = entries;

                if (!defaultLayoutId.empty())
                {
                    library["defaults"][lookupKey] = defaultLayoutId;
                }
            }
        }
    }

    // Load factory layouts from the bundled assets directory.
    // Structure: ui/assets/layouts/<folder-name>/layout.json  (+ images/ subfolder)
    // Each folder represents one exported layout. Factory layouts are read-only and are
    // added as defaults only when no user-defined default already exists for that key.
    {
        const auto bundledRoot = mHost.GetBundledAssetsPath();
        const auto factoryLayoutsDir = ResolveFactoryLayoutsDir(bundledRoot);

        if (std::filesystem::exists(factoryLayoutsDir))
        {
            for (const auto& layoutFolder : std::filesystem::directory_iterator(factoryLayoutsDir))
            {
                if (!layoutFolder.is_directory())
                {
                    continue;
                }

                const auto layoutJsonPath = layoutFolder.path() / "layout.json";

                if (!std::filesystem::exists(layoutJsonPath))
                {
                    continue;
                }

                try
                {
                    std::ifstream input(layoutJsonPath);

                    if (!input)
                    {
                        continue;
                    }

                    nlohmann::json archive;
                    input >> archive;

                    if (!archive.is_object() || !archive.contains("layout") || !archive["layout"].is_object())
                    {
                        continue;
                    }

                    nlohmann::json layoutJson = archive["layout"];

                    const std::string effectType = layoutJson.value("effectType", "");

                    if (effectType.empty())
                    {
                        continue;
                    }

                    const std::string blendId = layoutJson.value("blendId", "");
                    const std::string lookupKey = blendId.empty() ? effectType : (effectType + "::" + blendId);

                    // Use embedded layoutId or derive a stable one from the folder name.
                    std::string layoutId = layoutJson.value("layoutId", "");

                    if (layoutId.empty())
                    {
                        layoutId = "factory::" + util::PathToUtf8(layoutFolder.path().filename());
                        layoutJson["layoutId"] = layoutId;
                    }

                    // Load images referenced in the manifest and add them to the library image list.
                    const auto imagesDir = layoutFolder.path() / "images";
                    (void)imagesDir; // Factory images are loaded on demand via BuildLayoutImages().

                    // Build the library entry and prepend it so user layouts (added below)
                    // can override/supplement without losing the factory entry.
                    nlohmann::json factoryEntry;
                    factoryEntry["layout"] = layoutJson;
                    factoryEntry["isDefault"] = false; // resolved after user entries are built
                    factoryEntry["layoutId"] = layoutId;
                    factoryEntry["isFactory"] = true;
                    factoryEntry["filePath"] = util::PathToUtf8(layoutJsonPath);

                    if (!library["byEffectType"].contains(lookupKey))
                    {
                        library["byEffectType"][lookupKey] = nlohmann::json::array();
                    }

                    // Prepend so factory entries appear first; user entries appended later.
                    library["byEffectType"][lookupKey].insert(library["byEffectType"][lookupKey].begin(), factoryEntry);
                }
                catch (const std::exception& e)
                {
                    AppendSessionLog("Failed to load factory layout from " + util::PathToUtf8(layoutFolder.path()) +
                                     ": " + e.what());
                }
            }
        }
    }

    // Resolve defaults: for each key without a user-defined default, use the first
    // factory layout found (if any).
    for (auto& [key, entries] : library["byEffectType"].items())
    {
        if (!library["defaults"].contains(key) || library["defaults"][key].get<std::string>().empty())
        {
            for (const auto& entry : entries)
            {
                if (entry.value("isFactory", false))
                {
                    const std::string fid = entry.value("layoutId", "");

                    if (!fid.empty())
                    {
                        library["defaults"][key] = fid;
                        break;
                    }
                }
            }
        }

        // Stamp isDefault on each entry.
        const std::string defaultId = library["defaults"].value(key, "");

        for (auto& entry : entries)
        {
            entry["isDefault"] = (!defaultId.empty() && entry.value("layoutId", "") == defaultId);
        }
    }

    SendMessageToUI(nlohmann::json{{"type", "layoutLibraryLoaded"}, {"layoutLibrary", library}}.dump());
}

nlohmann::json PluginController::BuildLayoutImages()
{
    nlohmann::json images = nlohmann::json::array();

    // Helper: load all images from a directory into the image list.
    const auto appendImagesFromDir = [&images](const std::filesystem::path& imagesDir) {
        if (!std::filesystem::exists(imagesDir))
        {
            return;
        }

        for (const auto& entry : std::filesystem::directory_iterator(imagesDir))
        {
            if (!entry.is_regular_file())
            {
                continue;
            }

            const std::string mimeType = LayoutImageMimeType(entry.path().extension());

            if (mimeType.empty())
            {
                continue;
            }

            std::ifstream imageFile(entry.path(), std::ios::binary);

            if (!imageFile)
            {
                continue;
            }

            std::vector<std::uint8_t> imageData((std::istreambuf_iterator<char>(imageFile)),
                                                std::istreambuf_iterator<char>());
            imageFile.close();

            const std::string dataUrl = "data:" + mimeType + ";base64," + util::EncodeBase64(imageData);
            const std::string imageId = util::PathToUtf8(entry.path().stem());
            bool replaced = false;

            for (auto& existing : images)
            {
                if (existing.is_object() && existing.value("imageId", std::string{}) == imageId)
                {
                    existing["fileName"] = util::PathToUtf8(entry.path().filename());
                    existing["dataUrl"] = dataUrl;
                    replaced = true;
                    break;
                }
            }

            if (!replaced)
            {
                nlohmann::json imageRef;
                imageRef["imageId"] = imageId;
                imageRef["fileName"] = util::PathToUtf8(entry.path().filename());
                imageRef["dataUrl"] = dataUrl;
                images.push_back(imageRef);
            }
        }
    };

    // Images saved before every layout had an id went to one shared folder. Listed first, so a
    // layout's own copy of the same image wins.
    appendImagesFromDir(ResolveLegacyLayoutImagesDir(mFileSystem));

    // User layout images: one images/ folder per associated layoutId.
    nlohmann::json settings = LoadEffectLayoutsSettings(mFileSystem);

    if (settings.contains("associations") && settings["associations"].is_object())
    {
        for (auto it = settings["associations"].begin(); it != settings["associations"].end(); ++it)
        {
            const auto& assocEntry = it.value();

            if (!assocEntry.is_object())
            {
                continue;
            }

            const auto ids = assocEntry.value("layoutIds", nlohmann::json::array());

            if (!ids.is_array())
            {
                continue;
            }

            for (const auto& id : ids)
            {
                if (!id.is_string())
                {
                    continue;
                }

                const std::string layoutId = id.get<std::string>();
                appendImagesFromDir(ResolveLayoutDir(mFileSystem, layoutId) / "images");
            }
        }
    }

    // Factory layout images: referenced via each layout's manifest.
    {
        const auto bundledRoot = mHost.GetBundledAssetsPath();
        const auto factoryLayoutsDir = ResolveFactoryLayoutsDir(bundledRoot);

        if (std::filesystem::exists(factoryLayoutsDir))
        {
            for (const auto& layoutFolder : std::filesystem::directory_iterator(factoryLayoutsDir))
            {
                if (!layoutFolder.is_directory())
                {
                    continue;
                }

                const auto layoutJsonPath = layoutFolder.path() / "layout.json";

                if (!std::filesystem::exists(layoutJsonPath))
                {
                    continue;
                }

                try
                {
                    std::ifstream input(layoutJsonPath);

                    if (!input)
                    {
                        continue;
                    }

                    nlohmann::json archive;
                    input >> archive;

                    if (!archive.is_object() || !archive.contains("images") || !archive["images"].is_array())
                    {
                        continue;
                    }

                    const auto imagesDir = layoutFolder.path() / "images";

                    if (!std::filesystem::exists(imagesDir))
                    {
                        continue;
                    }

                    for (const auto& imgRef : archive["images"])
                    {
                        if (!imgRef.is_object())
                        {
                            continue;
                        }

                        const std::string imageId = imgRef.value("imageId", "");
                        const std::string fileName = imgRef.value("fileName", "");

                        if (imageId.empty() || fileName.empty())
                        {
                            continue;
                        }

                        const auto imgPath = imagesDir / util::PathFromUtf8(fileName);

                        if (!std::filesystem::exists(imgPath))
                        {
                            continue;
                        }

                        std::ifstream imgFile(imgPath, std::ios::binary);

                        if (!imgFile)
                        {
                            continue;
                        }

                        std::vector<std::uint8_t> imgData((std::istreambuf_iterator<char>(imgFile)),
                                                          std::istreambuf_iterator<char>());
                        imgFile.close();

                        const std::string mimeType = LayoutImageMimeType(imgPath.extension());

                        if (mimeType.empty())
                        {
                            continue;
                        }

                        const std::string dataUrl = "data:" + mimeType + ";base64," + util::EncodeBase64(imgData);

                        bool replaced = false;

                        for (auto& existing : images)
                        {
                            if (existing.is_object() && existing.value("imageId", std::string{}) == imageId)
                            {
                                existing["fileName"] = fileName;
                                existing["dataUrl"] = dataUrl;
                                replaced = true;
                                break;
                            }
                        }

                        if (!replaced)
                        {
                            nlohmann::json imageRef;
                            imageRef["imageId"] = imageId;
                            imageRef["fileName"] = fileName;
                            imageRef["dataUrl"] = dataUrl;
                            images.push_back(imageRef);
                        }
                    }
                }
                catch (const std::exception& e)
                {
                    AppendSessionLog("Failed to load factory layout images from " +
                                     util::PathToUtf8(layoutFolder.path()) + ": " + e.what());
                }
            }
        }
    }

    return images;
}

void PluginController::HandleRequestLayoutImagesRequest()
{
    SendMessageToUI(nlohmann::json{{"type", "layoutImagesLoaded"}, {"images", BuildLayoutImages()}}.dump());
}

bool PluginController::SaveLayoutToFile(const std::string& layoutId, const nlohmann::json& layoutJson)
{
    // The layout document goes to the store; its image sidecars stay in
    // layouts/content/<id>/images/, so the directory is still created.
    const auto layoutDir = ResolveLayoutDir(mFileSystem, layoutId);
    [[maybe_unused]] const auto ensuredDir = mFileSystem.EnsureDirectory(layoutDir);

    if (Store().Put(storage::ItemType::kLayout, layoutId, layoutJson))
    {
        AppendSessionLog("Layout saved: " + layoutId);
        return true;
    }

    AppendSessionLog("Failed to save layout: " + layoutId);
    return false;
}
} // namespace guitarfx

#include "controller/internal/LayoutSupport.h"

#include "util/FileIO.h"
#include "util/FileSystem.h"
#include "util/PathEncoding.h"
#include "util/PathSanitizer.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <fstream>
#include <map>
#include <optional>
#include <string_view>
#include <system_error>

namespace guitarfx::controller_detail
{
std::filesystem::path ResolveEffectLayoutsSettingsPath(const FileSystem& fileSystem)
{
    return fileSystem.ResolveSettingsDirectory() / "layouts" / "indexes" / "effect-layouts.json";
}

nlohmann::json LoadEffectLayoutsSettings(const FileSystem& fileSystem)
{
    const auto path = ResolveEffectLayoutsSettingsPath(fileSystem);
    nlohmann::json root = nlohmann::json::object();
    root["version"] = 1;
    root["associations"] = nlohmann::json::object();

    try
    {
        if (path.empty() || !std::filesystem::exists(path))
        {
            return root;
        }

        std::ifstream input(path);

        if (!input)
        {
            return root;
        }

        nlohmann::json parsed;
        input >> parsed;

        if (!parsed.is_object())
        {
            return root;
        }

        if (!parsed.contains("associations") || !parsed["associations"].is_object())
        {
            parsed["associations"] = nlohmann::json::object();
        }

        if (!parsed.contains("version") || !parsed["version"].is_number())
        {
            parsed["version"] = 1;
        }

        return parsed;
    }
    catch (...)
    {
        return root;
    }
}

bool SaveEffectLayoutsSettings(const FileSystem& fileSystem, const nlohmann::json& root)
{
    const auto path = ResolveEffectLayoutsSettingsPath(fileSystem);

    if (path.empty())
    {
        return false;
    }

    try
    {
        const auto dir = path.parent_path();
        [[maybe_unused]] const auto ensured = fileSystem.EnsureDirectory(dir);
        return util::WriteTextFileAtomic(path, root.dump(2));
    }
    catch (...)
    {
        return false;
    }
}

std::filesystem::path ResolveLayoutDir(const FileSystem& fileSystem, const std::string& layoutId)
{
    const auto settingsDir = fileSystem.ResolveSettingsDirectory();
    const std::string safeStem = util::SanitizeFilename(layoutId);
    return settingsDir / "layouts" / "content" / safeStem;
}

std::filesystem::path ResolveLayoutFilePath(const FileSystem& fileSystem, const std::string& layoutId)
{
    return ResolveLayoutDir(fileSystem, layoutId) / "layout.json";
}

std::filesystem::path ResolveLegacyLayoutImagesDir(const FileSystem& fileSystem)
{
    return fileSystem.ResolveSettingsDirectory() / "layouts" / "content" / "images";
}

std::filesystem::path ResolveFactoryLayoutsDir(const std::filesystem::path& bundledAssetsRoot)
{
    return bundledAssetsRoot / "ui" / "assets" / "layouts";
}

namespace
{
using ImageSources = std::map<std::string, std::filesystem::path>;

std::string LowerAscii(std::string value)
{
    for (char& c : value)
    {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }

    return value;
}

std::string LowerExtension(const std::filesystem::path& extension)
{
    return LowerAscii(util::PathToUtf8(extension));
}

std::string StringField(const nlohmann::json& object, const char* key)
{
    const auto it = object.find(key);
    return it != object.end() && it->is_string() ? it->get<std::string>() : std::string{};
}

bool StartsWith(const std::vector<std::uint8_t>& bytes, std::size_t offset, std::string_view prefix)
{
    if (bytes.size() < offset + prefix.size())
    {
        return false;
    }

    return std::equal(prefix.begin(), prefix.end(), bytes.begin() + static_cast<std::ptrdiff_t>(offset),
                      [](char expected, std::uint8_t actual) { return static_cast<std::uint8_t>(expected) == actual; });
}

/// CON, NUL, COM1 and the rest name devices on Windows whatever the extension.
bool IsWindowsDeviceName(const std::string& stem)
{
    static constexpr std::array<std::string_view, 22> kDevices = {
        "con",  "prn",  "aux",  "nul",  "com1", "com2", "com3", "com4", "com5", "com6", "com7",
        "com8", "com9", "lpt1", "lpt2", "lpt3", "lpt4", "lpt5", "lpt6", "lpt7", "lpt8", "lpt9"};
    const std::string lower = LowerAscii(stem.substr(0, stem.find('.')));
    return std::find(kDevices.begin(), kDevices.end(), lower) != kDevices.end();
}

/// A file name with nothing in it that leads elsewhere: no separator of either kind, no drive
/// or stream colon, no control character, and not "." or "..".
bool IsBareFileName(const std::string& name)
{
    if (name.empty() || name == "." || name == ".." || name.find_first_of("/\\:") != std::string::npos)
    {
        return false;
    }

    if (std::any_of(name.begin(), name.end(), [](char c) { return static_cast<unsigned char>(c) < 0x20; }))
    {
        return false;
    }

    const auto path = util::PathFromUtf8(name);
    return !path.has_root_path() && !path.has_parent_path() && path.filename() == path;
}

/// Image files in `imagesDir`, by stem: how user layouts and the legacy folder name them.
void AddImagesByStem(ImageSources& sources, const std::filesystem::path& imagesDir)
{
    std::error_code ec;

    if (!std::filesystem::is_directory(imagesDir, ec))
    {
        return;
    }

    for (const auto& entry : std::filesystem::directory_iterator(imagesDir, ec))
    {
        std::error_code entryEc;

        if (entry.is_regular_file(entryEc) && !LayoutImageMimeType(entry.path().extension()).empty())
        {
            sources[util::PathToUtf8(entry.path().stem())] = entry.path();
        }
    }
}

/// Factory images, by the id each factory layout's manifest gives them.
void AddFactoryImages(ImageSources& sources, const std::filesystem::path& factoryLayoutsDir)
{
    std::error_code ec;

    if (!std::filesystem::is_directory(factoryLayoutsDir, ec))
    {
        return;
    }

    for (const auto& folder : std::filesystem::directory_iterator(factoryLayoutsDir, ec))
    {
        std::error_code folderEc;

        if (!folder.is_directory(folderEc))
        {
            continue;
        }

        std::ifstream input(folder.path() / "layout.json");
        const auto archive = nlohmann::json::parse(input, nullptr, false);

        if (!archive.is_object() || !archive.contains("images") || !archive["images"].is_array())
        {
            continue;
        }

        for (const auto& image : archive["images"])
        {
            const std::string imageId = image.is_object() ? StringField(image, "imageId") : std::string{};
            const std::string fileName = image.is_object() ? StringField(image, "fileName") : std::string{};

            if (imageId.empty() || !IsBareFileName(fileName))
            {
                continue;
            }

            const auto path = folder.path() / "images" / util::PathFromUtf8(fileName);
            std::error_code fileEc;

            if (std::filesystem::is_regular_file(path, fileEc) && !LayoutImageMimeType(path.extension()).empty())
            {
                sources[imageId] = path;
            }
        }
    }
}

/// Every image a layout could have picked, except those already in `excludeImagesDir`.
/// Lowest priority first, so for one id a user layout's file wins over the legacy folder's,
/// and that over a factory one, as in BuildLayoutImages.
ImageSources CollectImageSources(const FileSystem& fileSystem, const std::filesystem::path& factoryLayoutsDir,
                                 const std::filesystem::path& excludeImagesDir)
{
    ImageSources sources;
    AddFactoryImages(sources, factoryLayoutsDir);
    AddImagesByStem(sources, ResolveLegacyLayoutImagesDir(fileSystem));

    const auto contentDir = fileSystem.ResolveSettingsDirectory() / "layouts" / "content";
    std::error_code ec;

    if (!std::filesystem::is_directory(contentDir, ec))
    {
        return sources;
    }

    for (const auto& layoutDir : std::filesystem::directory_iterator(contentDir, ec))
    {
        std::error_code dirEc;
        const auto imagesDir = layoutDir.path() / "images";

        if (layoutDir.is_directory(dirEc) && imagesDir != excludeImagesDir)
        {
            AddImagesByStem(sources, imagesDir);
        }
    }

    return sources;
}
} // namespace

std::string LayoutImageMimeType(const std::filesystem::path& extension)
{
    const std::string ext = LowerExtension(extension);

    if (ext == ".png")
    {
        return "image/png";
    }

    if (ext == ".jpg" || ext == ".jpeg")
    {
        return "image/jpeg";
    }

    if (ext == ".gif")
    {
        return "image/gif";
    }

    if (ext == ".webp")
    {
        return "image/webp";
    }

    if (ext == ".svg")
    {
        return "image/svg+xml";
    }

    return {};
}

bool IsSafeLayoutImageFileName(const std::string& fileName, const std::string& imageId)
{
    try
    {
        if (imageId.empty() || !IsBareFileName(fileName))
        {
            return false;
        }

        const auto path = util::PathFromUtf8(fileName);
        const std::string stem = util::PathToUtf8(path.stem());
        return stem == imageId && !IsWindowsDeviceName(stem) && !LayoutImageMimeType(path.extension()).empty();
    }
    catch (...)
    {
        return false;
    }
}

bool LooksLikeLayoutImage(const std::vector<std::uint8_t>& bytes, const std::filesystem::path& extension)
{
    const std::string ext = LowerExtension(extension);

    if (ext == ".png")
    {
        return StartsWith(bytes, 0, "\x89PNG\r\n\x1a\n");
    }

    if (ext == ".jpg" || ext == ".jpeg")
    {
        return StartsWith(bytes, 0, "\xFF\xD8\xFF");
    }

    if (ext == ".gif")
    {
        return StartsWith(bytes, 0, "GIF87a") || StartsWith(bytes, 0, "GIF89a");
    }

    if (ext == ".webp")
    {
        return StartsWith(bytes, 0, "RIFF") && StartsWith(bytes, 8, "WEBP");
    }

    if (ext == ".svg")
    {
        // Markup, optionally after a byte-order mark and whitespace, with an <svg> element early on.
        std::size_t start = StartsWith(bytes, 0, "\xEF\xBB\xBF") ? 3 : 0;

        while (start < bytes.size() && std::isspace(bytes[start]) != 0)
        {
            ++start;
        }

        if (start >= bytes.size() || bytes[start] != '<')
        {
            return false;
        }

        const std::size_t end = std::min(bytes.size(), start + 4096);
        const std::string head(bytes.begin() + static_cast<std::ptrdiff_t>(start),
                               bytes.begin() + static_cast<std::ptrdiff_t>(end));
        return LowerAscii(head).find("<svg") != std::string::npos;
    }

    return false;
}

std::vector<std::string> CollectLayoutImageIds(const nlohmann::json& layout)
{
    std::vector<std::string> ids;
    const auto add = [&ids](std::string id) {
        if (!id.empty() && std::find(ids.begin(), ids.end(), id) == ids.end())
        {
            ids.push_back(std::move(id));
        }
    };

    if (!layout.is_object())
    {
        return ids;
    }

    if (const auto backgrounds = layout.find("backgrounds"); backgrounds != layout.end() && backgrounds->is_array())
    {
        for (const auto& background : *backgrounds)
        {
            if (background.is_object() && StringField(background, "type") == "image")
            {
                add(StringField(background, "value"));
            }
        }
    }

    if (const auto controls = layout.find("controls"); controls != layout.end() && controls->is_array())
    {
        for (const auto& control : *controls)
        {
            const auto style = control.is_object() ? control.find("style") : control.end();

            if (control.is_object() && style != control.end() && style->is_object())
            {
                add(StringField(*style, "knobImageId"));
            }
        }
    }

    return ids;
}

std::vector<std::string> CopyMissingLayoutImages(const FileSystem& fileSystem,
                                                 const std::filesystem::path& factoryLayoutsDir,
                                                 const std::string& layoutId, const std::vector<std::string>& imageIds,
                                                 const std::function<void(const std::string&)>& log)
{
    std::vector<std::string> missing;

    if (layoutId.empty())
    {
        return imageIds;
    }

    const auto destDir = ResolveLayoutDir(fileSystem, layoutId) / "images";
    ImageSources own;
    std::optional<ImageSources> elsewhere;

    try
    {
        AddImagesByStem(own, destDir);
    }
    catch (const std::exception& e)
    {
        log("Could not list layout images in " + util::PathToUtf8(destDir) + ": " + e.what());
    }

    for (const auto& imageId : imageIds)
    {
        if (imageId.empty() || own.contains(imageId))
        {
            continue;
        }

        try
        {
            if (!elsewhere)
            {
                elsewhere = CollectImageSources(fileSystem, factoryLayoutsDir, destDir);
            }

            const auto found = elsewhere->find(imageId);

            if (found == elsewhere->end())
            {
                missing.push_back(imageId);
                continue;
            }

            const std::string destName = imageId + LowerExtension(found->second.extension());

            if (!IsSafeLayoutImageFileName(destName, imageId))
            {
                log("Layout image id is not usable as a file name: " + imageId);
                missing.push_back(imageId);
                continue;
            }

            [[maybe_unused]] const auto ensured = fileSystem.EnsureDirectory(destDir);
            std::error_code ec;
            std::filesystem::copy_file(found->second, destDir / util::PathFromUtf8(destName),
                                       std::filesystem::copy_options::overwrite_existing, ec);

            if (ec)
            {
                log("Failed to copy layout image " + util::PathToUtf8(found->second) + ": " + ec.message());
                missing.push_back(imageId);
            }
        }
        catch (const std::exception& e)
        {
            log("Failed to copy layout image " + imageId + ": " + e.what());
            missing.push_back(imageId);
        }
    }

    return missing;
}
} // namespace guitarfx::controller_detail

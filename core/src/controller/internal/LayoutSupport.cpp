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
#include <initializer_list>
#include <map>
#include <optional>
#include <sstream>
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
using ImageSources = std::vector<LayoutImageSource>;

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

/// Points `imageId` at `path`: a later folder replaces an earlier one's file but keeps the id's
/// place in the list, as the library always has.
void PutImage(ImageSources& sources, const std::string& imageId, const std::filesystem::path& path)
{
    const auto existing = std::find_if(sources.begin(), sources.end(), [&imageId](const LayoutImageSource& source) {
        return source.imageId == imageId;
    });

    if (existing != sources.end())
    {
        existing->path = path;
    }
    else
    {
        sources.push_back({imageId, path});
    }
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
            PutImage(sources, util::PathToUtf8(entry.path().stem()), entry.path());
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
                PutImage(sources, imageId, path);
            }
        }
    }
}

/// The images folder of every layout the index lists, in index order: each association's
/// layoutIds in turn. A layout listed twice (under an effect type and a blend of it) appears twice.
std::vector<std::filesystem::path> IndexedLayoutImageDirs(const FileSystem& fileSystem)
{
    std::vector<std::filesystem::path> dirs;
    const auto settings = LoadEffectLayoutsSettings(fileSystem);
    const auto associations = settings.find("associations");

    if (associations == settings.end() || !associations->is_object())
    {
        return dirs;
    }

    for (const auto& entry : *associations)
    {
        const auto ids = entry.is_object() ? entry.find("layoutIds") : entry.end();

        if (!entry.is_object() || ids == entry.end() || !ids->is_array())
        {
            continue;
        }

        for (const auto& id : *ids)
        {
            if (id.is_string())
            {
                dirs.push_back(ResolveLayoutDir(fileSystem, id.get<std::string>()) / "images");
            }
        }
    }

    return dirs;
}
} // namespace

std::vector<LayoutImageSource> CollectLayoutImageSources(const FileSystem& fileSystem,
                                                         const std::filesystem::path& factoryLayoutsDir,
                                                         bool includeUnindexed,
                                                         const std::filesystem::path& excludeImagesDir)
{
    ImageSources sources;
    const auto indexedDirs = IndexedLayoutImageDirs(fileSystem);

    if (includeUnindexed)
    {
        const auto contentDir = fileSystem.ResolveSettingsDirectory() / "layouts" / "content";
        std::error_code ec;

        if (std::filesystem::is_directory(contentDir, ec))
        {
            for (const auto& layoutDir : std::filesystem::directory_iterator(contentDir, ec))
            {
                std::error_code dirEc;
                const auto imagesDir = layoutDir.path() / "images";

                if (layoutDir.is_directory(dirEc) && imagesDir != excludeImagesDir &&
                    std::find(indexedDirs.begin(), indexedDirs.end(), imagesDir) == indexedDirs.end())
                {
                    AddImagesByStem(sources, imagesDir);
                }
            }
        }
    }

    if (ResolveLegacyLayoutImagesDir(fileSystem) != excludeImagesDir)
    {
        AddImagesByStem(sources, ResolveLegacyLayoutImagesDir(fileSystem));
    }

    for (const auto& imagesDir : indexedDirs)
    {
        if (imagesDir != excludeImagesDir)
        {
            AddImagesByStem(sources, imagesDir);
        }
    }

    AddFactoryImages(sources, factoryLayoutsDir);
    return sources;
}

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

namespace
{
/// `value` with everything but A-Z a-z 0-9 - _ . ~ percent-encoded, for one URL path segment.
std::string PercentEncode(const std::string& value)
{
    static constexpr char kHex[] = "0123456789ABCDEF";
    std::string encoded;

    for (const char c : value)
    {
        const auto byte = static_cast<unsigned char>(c);

        if (std::isalnum(byte) != 0 || c == '-' || c == '_' || c == '.' || c == '~')
        {
            encoded += c;
        }
        else
        {
            encoded += '%';
            encoded += kHex[byte >> 4];
            encoded += kHex[byte & 0x0F];
        }
    }

    return encoded;
}

/// One percent-encoded path segment decoded into an image id, or empty when it holds a separator,
/// a bad escape, or decodes to something that could name another folder.
std::string DecodeImageIdSegment(std::string_view segment)
{
    const auto hexValue = [](char c) -> int {
        if (c >= '0' && c <= '9')
        {
            return c - '0';
        }

        if (c >= 'a' && c <= 'f')
        {
            return c - 'a' + 10;
        }

        if (c >= 'A' && c <= 'F')
        {
            return c - 'A' + 10;
        }

        return -1;
    };

    std::string id;

    for (std::size_t i = 0; i < segment.size(); ++i)
    {
        const char c = segment[i];

        if (c == '/' || c == '\\' || c == '?' || c == '#')
        {
            return {};
        }

        if (c != '%')
        {
            id += c;
            continue;
        }

        const int high = i + 2 < segment.size() ? hexValue(segment[i + 1]) : -1;
        const int low = i + 2 < segment.size() ? hexValue(segment[i + 2]) : -1;

        if (high < 0 || low < 0)
        {
            return {};
        }

        id += static_cast<char>((high << 4) | low);
        i += 2;
    }

    // An id is a file stem: nothing that could name another folder once decoded.
    return id.find_first_of("/\\") == std::string::npos && id != "." && id != ".." ? id : std::string{};
}

constexpr std::string_view kImageRoutePrefix = "layout-images/id/";
constexpr std::string_view kRenditionRoutePrefix = "layout-images/rendition/";
} // namespace

std::string LayoutImageUrl(const std::string& imageId, std::uint64_t version)
{
    return std::string(kImageRoutePrefix) + PercentEncode(imageId) + "?v=" + std::to_string(version);
}

std::string LayoutImageIdFromResourcePath(const std::string& path)
{
    if (path.size() <= kImageRoutePrefix.size() || path.compare(0, kImageRoutePrefix.size(), kImageRoutePrefix) != 0)
    {
        return {};
    }

    return DecodeImageIdSegment(std::string_view(path).substr(kImageRoutePrefix.size()));
}

std::uint64_t LayoutImageVersion(const std::filesystem::path& file)
{
    std::error_code ec;
    const auto modified = std::filesystem::last_write_time(file, ec);
    return ec ? 0 : static_cast<std::uint64_t>(modified.time_since_epoch().count());
}

bool IsLayoutRenditionEdge(int edge)
{
    return edge == 1024 || edge == 2048 || edge == 4096;
}

std::filesystem::path ResolveLayoutRenditionsDir(const FileSystem& fileSystem)
{
    return fileSystem.ResolveSettingsDirectory() / "layouts" / "renditions";
}

std::string LayoutRenditionFileName(const std::string& imageId, int edge, std::uint64_t version)
{
    // FNV-1a of the original id: SanitizeFilename can map two ids to one name.
    std::uint32_t hash = 2166136261u;

    for (const char c : imageId)
    {
        hash = (hash ^ static_cast<unsigned char>(c)) * 16777619u;
    }

    static constexpr char kHex[] = "0123456789abcdef";
    std::string hashText;

    for (int shift = 28; shift >= 0; shift -= 4)
    {
        hashText += kHex[(hash >> shift) & 0x0F];
    }

    return util::SanitizeFilename(imageId) + "-" + hashText + "@" + std::to_string(edge) + "-" +
           std::to_string(version) + ".webp";
}

std::string LayoutRenditionUrl(const std::string& imageId, int edge, std::uint64_t version)
{
    return std::string(kRenditionRoutePrefix) + PercentEncode(imageId) + "/" + std::to_string(edge) +
           "?v=" + std::to_string(version);
}

std::optional<std::pair<std::string, int>> LayoutRenditionFromResourcePath(const std::string& path)
{
    if (path.size() <= kRenditionRoutePrefix.size() ||
        path.compare(0, kRenditionRoutePrefix.size(), kRenditionRoutePrefix) != 0)
    {
        return std::nullopt;
    }

    const std::string_view rest = std::string_view(path).substr(kRenditionRoutePrefix.size());
    const auto slash = rest.rfind('/');

    if (slash == std::string_view::npos || slash == 0 || slash + 1 >= rest.size())
    {
        return std::nullopt;
    }

    const std::string_view edgeText = rest.substr(slash + 1);

    if (edgeText.size() > 4 ||
        !std::all_of(edgeText.begin(), edgeText.end(), [](char c) { return c >= '0' && c <= '9'; }))
    {
        return std::nullopt;
    }

    const int edge = std::stoi(std::string(edgeText));
    std::string id = DecodeImageIdSegment(rest.substr(0, slash));

    if (id.empty() || !IsLayoutRenditionEdge(edge))
    {
        return std::nullopt;
    }

    return std::make_pair(std::move(id), edge);
}

namespace
{
std::uint32_t BigEndian(const std::vector<std::uint8_t>& b, std::size_t at, std::size_t count)
{
    std::uint32_t value = 0;

    for (std::size_t i = 0; i < count; ++i)
    {
        value = (value << 8) | b[at + i];
    }

    return value;
}

std::uint32_t LittleEndian(const std::vector<std::uint8_t>& b, std::size_t at, std::size_t count)
{
    std::uint32_t value = 0;

    for (std::size_t i = count; i > 0; --i)
    {
        value = (value << 8) | b[at + i - 1];
    }

    return value;
}

/// A JPEG's frame size, walking its markers from the file so a large EXIF block is skipped
/// rather than read.
std::optional<LayoutImageSize> ReadJpegSize(std::ifstream& input)
{
    input.seekg(2);

    while (input)
    {
        int byte = input.get();

        if (byte != 0xFF)
        {
            return std::nullopt;
        }

        while (byte == 0xFF)
        {
            byte = input.get();
        }

        if (byte < 0)
        {
            return std::nullopt;
        }

        const auto marker = static_cast<std::uint8_t>(byte);

        if (marker == 0x01 || (marker >= 0xD0 && marker <= 0xD8))
        {
            continue; // Markers with no length.
        }

        if (marker == 0xD9 || marker == 0xDA)
        {
            return std::nullopt; // End of image, or scan data with no frame header before it.
        }

        std::array<char, 2> lengthBytes{};

        if (!input.read(lengthBytes.data(), 2))
        {
            return std::nullopt;
        }

        const auto length = static_cast<std::uint16_t>((static_cast<std::uint8_t>(lengthBytes[0]) << 8) |
                                                       static_cast<std::uint8_t>(lengthBytes[1]));
        const bool isFrame = marker >= 0xC0 && marker <= 0xCF && marker != 0xC4 && marker != 0xC8 && marker != 0xCC;

        if (isFrame)
        {
            std::array<char, 5> frame{};

            if (!input.read(frame.data(), 5))
            {
                return std::nullopt;
            }

            const auto height = static_cast<std::uint32_t>((static_cast<std::uint8_t>(frame[1]) << 8) |
                                                           static_cast<std::uint8_t>(frame[2]));
            const auto width = static_cast<std::uint32_t>((static_cast<std::uint8_t>(frame[3]) << 8) |
                                                          static_cast<std::uint8_t>(frame[4]));
            return width > 0 && height > 0 ? std::optional<LayoutImageSize>({width, height}) : std::nullopt;
        }

        if (length < 2)
        {
            return std::nullopt;
        }

        input.seekg(length - 2, std::ios::cur);
    }

    return std::nullopt;
}

/// The number at the start of an SVG length ("640", "640px", "48.5"), or 0.
std::uint32_t SvgLength(const std::string& value)
{
    std::size_t used = 0;

    try
    {
        const double number = std::stod(value, &used);
        return used > 0 && number > 0 && number < 1e6 ? static_cast<std::uint32_t>(number + 0.5) : 0;
    }
    catch (...)
    {
        return 0;
    }
}

/// The value of attribute `name` in an SVG start tag, or empty.
std::string SvgAttribute(const std::string& tag, const std::string& name)
{
    std::size_t at = 0;

    while ((at = tag.find(name, at)) != std::string::npos)
    {
        const bool startsWord = at == 0 || std::isspace(static_cast<unsigned char>(tag[at - 1])) != 0;
        std::size_t next = at + name.size();

        while (next < tag.size() && std::isspace(static_cast<unsigned char>(tag[next])) != 0)
        {
            ++next;
        }

        if (startsWord && next < tag.size() && tag[next] == '=')
        {
            ++next;

            while (next < tag.size() && std::isspace(static_cast<unsigned char>(tag[next])) != 0)
            {
                ++next;
            }

            if (next < tag.size() && (tag[next] == '"' || tag[next] == '\''))
            {
                const auto end = tag.find(tag[next], next + 1);
                return end == std::string::npos ? std::string{} : tag.substr(next + 1, end - next - 1);
            }
        }

        at += name.size();
    }

    return {};
}

std::optional<LayoutImageSize> ReadSvgSize(const std::vector<std::uint8_t>& head)
{
    const std::string text(head.begin(), head.end());
    const auto open = LowerAscii(text).find("<svg");

    if (open == std::string::npos)
    {
        return std::nullopt;
    }

    const auto close = text.find('>', open);
    const std::string tag = text.substr(open, close == std::string::npos ? std::string::npos : close - open);
    const std::uint32_t width = SvgLength(SvgAttribute(tag, "width"));
    const std::uint32_t height = SvgLength(SvgAttribute(tag, "height"));

    if (width > 0 && height > 0)
    {
        return LayoutImageSize{width, height};
    }

    std::istringstream viewBox(SvgAttribute(tag, "viewBox"));
    double minX = 0;
    double minY = 0;
    double boxWidth = 0;
    double boxHeight = 0;

    if (viewBox >> minX >> minY >> boxWidth >> boxHeight && boxWidth > 0 && boxHeight > 0)
    {
        return LayoutImageSize{static_cast<std::uint32_t>(boxWidth + 0.5), static_cast<std::uint32_t>(boxHeight + 0.5)};
    }

    return std::nullopt;
}
} // namespace

std::optional<LayoutImageSize> ReadLayoutImageSize(const std::filesystem::path& path)
{
    try
    {
        std::ifstream input(path, std::ios::binary);

        if (!input)
        {
            return std::nullopt;
        }

        std::vector<std::uint8_t> head(4096);
        input.read(reinterpret_cast<char*>(head.data()), static_cast<std::streamsize>(head.size()));
        head.resize(static_cast<std::size_t>(input.gcount()));
        input.clear();

        const auto sized = [](std::uint32_t width, std::uint32_t height) -> std::optional<LayoutImageSize> {
            return width > 0 && height > 0 ? std::optional<LayoutImageSize>({width, height}) : std::nullopt;
        };

        if (StartsWith(head, 0, "\x89PNG\r\n\x1a\n") && head.size() >= 24 && StartsWith(head, 12, "IHDR"))
        {
            return sized(BigEndian(head, 16, 4), BigEndian(head, 20, 4));
        }

        if ((StartsWith(head, 0, "GIF87a") || StartsWith(head, 0, "GIF89a")) && head.size() >= 10)
        {
            return sized(LittleEndian(head, 6, 2), LittleEndian(head, 8, 2));
        }

        if (StartsWith(head, 0, "RIFF") && StartsWith(head, 8, "WEBP") && head.size() >= 30)
        {
            if (StartsWith(head, 12, "VP8 "))
            {
                return sized(LittleEndian(head, 26, 2) & 0x3FFF, LittleEndian(head, 28, 2) & 0x3FFF);
            }

            if (StartsWith(head, 12, "VP8L"))
            {
                const std::uint32_t bits = LittleEndian(head, 21, 4);
                return sized((bits & 0x3FFF) + 1, ((bits >> 14) & 0x3FFF) + 1);
            }

            if (StartsWith(head, 12, "VP8X"))
            {
                return sized(LittleEndian(head, 24, 3) + 1, LittleEndian(head, 27, 3) + 1);
            }

            return std::nullopt;
        }

        if (StartsWith(head, 0, "\xFF\xD8\xFF"))
        {
            return ReadJpegSize(input);
        }

        if (LowerExtension(path.extension()) == ".svg")
        {
            return ReadSvgSize(head);
        }
    }
    catch (...)
    {
    }

    return std::nullopt;
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

                // Image knobs, toggles and sliders; the UI's layoutImageIds lists the same places.
                for (const auto& [group, keys] :
                     std::initializer_list<std::pair<const char*, std::initializer_list<const char*>>>{
                         {"knobImage", {"imageId", "baseImageId", "glareImageId"}},
                         {"toggleImage", {"onImageId", "offImageId"}},
                         {"sliderImage", {"trackImageId", "thumbImageId"}}})
                {
                    const auto object = style->find(group);

                    if (object != style->end() && object->is_object())
                    {
                        for (const char* key : keys)
                        {
                            add(StringField(*object, key));
                        }
                    }
                }
            }
        }
    }

    if (const auto overlays = layout.find("overlays"); overlays != layout.end() && overlays->is_array())
    {
        for (const auto& overlay : *overlays)
        {
            const auto style = overlay.is_object() ? overlay.find("style") : overlay.end();

            if (overlay.is_object() && style != overlay.end() && style->is_object())
            {
                add(StringField(*style, "imageId"));
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
    std::map<std::string, std::filesystem::path> own;
    std::optional<std::map<std::string, std::filesystem::path>> elsewhere;

    try
    {
        ImageSources listed;
        AddImagesByStem(listed, destDir);

        for (auto& source : listed)
        {
            own.emplace(std::move(source.imageId), std::move(source.path));
        }
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
                // Unindexed folders too: an image picked from a layout that was never saved still
                // has to be copied before that folder is tidied away.
                elsewhere.emplace();

                for (auto& source : CollectLayoutImageSources(fileSystem, factoryLayoutsDir, true, destDir))
                {
                    elsewhere->emplace(std::move(source.imageId), std::move(source.path));
                }
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

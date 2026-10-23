#pragma once

/**
 * LayoutSupport.h — On-disk locations for custom effect layouts.
 *
 * A layout is an index entry in one shared settings file plus a content
 * directory of its own. Both the layout handlers and the startup loader need
 * to agree on where those live, so the path rules are stated once here.
 */

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace guitarfx
{
class FileSystem;
}

namespace guitarfx::controller_detail
{
/// Index file listing every known effect layout.
[[nodiscard]] std::filesystem::path ResolveEffectLayoutsSettingsPath(const FileSystem& fileSystem);

/// Reads the layout index, returning an empty object when it is missing or unreadable.
[[nodiscard]] nlohmann::json LoadEffectLayoutsSettings(const FileSystem& fileSystem);

/// Writes the layout index atomically; false when it could not be written.
bool SaveEffectLayoutsSettings(const FileSystem& fileSystem, const nlohmann::json& root);

/// Content directory for one layout (images and layout.json).
[[nodiscard]] std::filesystem::path ResolveLayoutDir(const FileSystem& fileSystem, const std::string& layoutId);

[[nodiscard]] std::filesystem::path ResolveLayoutFilePath(const FileSystem& fileSystem, const std::string& layoutId);

/// Where images went when a layout had no id yet. Nothing writes here any more, but layouts
/// saved before that still refer to images in it.
[[nodiscard]] std::filesystem::path ResolveLegacyLayoutImagesDir(const FileSystem& fileSystem);

/// The factory layouts shipped with the app: one folder each, holding layout.json and images/.
[[nodiscard]] std::filesystem::path ResolveFactoryLayoutsDir(const std::filesystem::path& bundledAssetsRoot);

/// Largest image a layout takes, from the file picker or an imported archive. The UI loads
/// images by URL (LayoutImageUrl), so this bounds only the one base64 message an archive's
/// image arrives in when a layout is imported.
inline constexpr std::size_t kMaxLayoutImageBytes = 64u * 1024u * 1024u;

/// The URL the UI draws image `imageId` from: `layout-images/id/<id>` with the id
/// percent-encoded (everything but A-Z a-z 0-9 - _ . ~), and `?v=<version>` so a file that
/// changes under the same id is fetched again. The editor's resource provider serves it.
[[nodiscard]] std::string LayoutImageUrl(const std::string& imageId, std::uint64_t version);

/// The image id in a `layout-images/id/<id>` resource path (no query), percent-decoded, or
/// empty when the path is not one.
[[nodiscard]] std::string LayoutImageIdFromResourcePath(const std::string& path);

/// Pixel size an image file declares, read from its header without decoding it.
struct LayoutImageSize
{
    std::uint32_t width = 0;
    std::uint32_t height = 0;
};

/// The pixel size in a PNG, JPEG, GIF or WebP header, or an SVG's width and height attributes
/// (else its viewBox); nullopt when the file cannot be read or declares none.
[[nodiscard]] std::optional<LayoutImageSize> ReadLayoutImageSize(const std::filesystem::path& path);

/// MIME type of an image a layout can use, from its extension in any case (".JPG" as ".jpg"),
/// or empty for a type layouts do not take.
[[nodiscard]] std::string LayoutImageMimeType(const std::filesystem::path& extension);

/// True when `fileName` can be written inside a layout's images folder as `imageId`: a bare
/// name with no folder, drive or root in it, an image extension layouts take, and `imageId`
/// as its stem, which is how the loader reads the id back. An imported archive names its own
/// images, and "..\..\x.bat" or "C:/x.dll" would otherwise be written where they point.
[[nodiscard]] bool IsSafeLayoutImageFileName(const std::string& fileName, const std::string& imageId);

/// True when `bytes` begin the way an image with that extension does, so an archive cannot
/// write something else under an image's name.
[[nodiscard]] bool LooksLikeLayoutImage(const std::vector<std::uint8_t>& bytes, const std::filesystem::path& extension);

/// One image a layout can draw on: its id and the file that holds it.
struct LayoutImageSource
{
    std::string imageId;
    std::filesystem::path path;
};

/// Every image a layout can draw on, each id once, in the order the library first lists it.
/// Ids are shared by every layout, and when two folders hold the same id the later one wins, in
/// this order: the legacy shared folder, then each indexed layout's folder in index order, then
/// the factory layouts. That is the file the UI has always shown for an id, so everything that
/// reads an image by id resolves it here. With `includeUnindexed`, layout folders the index does
/// not list (a layout never saved, or one a delete left behind) come first, so they only supply an
/// id found nowhere else. `excludeImagesDir` is left out entirely.
[[nodiscard]] std::vector<LayoutImageSource>
CollectLayoutImageSources(const FileSystem& fileSystem, const std::filesystem::path& factoryLayoutsDir,
                          bool includeUnindexed = false, const std::filesystem::path& excludeImagesDir = {});

/// The image ids a layout document draws on: image background layers and custom knob faces,
/// each once, in the order they appear.
[[nodiscard]] std::vector<std::string> CollectLayoutImageIds(const nlohmann::json& layout);

/// Copies each of `imageIds` that `layoutId`'s own images folder lacks into it, from the file
/// CollectLayoutImageSources resolves the id to, which is the one the user saw. A layout can pick
/// any image in the library, and without its own copy it loses that image when the layout it came
/// from is deleted. Returns the ids found nowhere.
std::vector<std::string> CopyMissingLayoutImages(const FileSystem& fileSystem,
                                                 const std::filesystem::path& factoryLayoutsDir,
                                                 const std::string& layoutId, const std::vector<std::string>& imageIds,
                                                 const std::function<void(const std::string&)>& log);
} // namespace guitarfx::controller_detail

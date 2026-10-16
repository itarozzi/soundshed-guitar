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

/// Largest image a layout takes, from the file picker or an imported archive. Every image is
/// sent to the UI base64-encoded, so this also bounds that message.
inline constexpr std::size_t kMaxLayoutImageBytes = 16u * 1024u * 1024u;

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

/// The image ids a layout document draws on: image background layers and custom knob faces,
/// each once, in the order they appear.
[[nodiscard]] std::vector<std::string> CollectLayoutImageIds(const nlohmann::json& layout);

/// Copies each of `imageIds` that `layoutId`'s own images folder lacks into it, from wherever
/// it is found: another layout's folder, the legacy shared folder, or a factory layout. A layout
/// can pick any image in the library, and without its own copy it loses that image when the
/// layout it came from is deleted. Returns the ids found nowhere.
std::vector<std::string> CopyMissingLayoutImages(const FileSystem& fileSystem,
                                                 const std::filesystem::path& factoryLayoutsDir,
                                                 const std::string& layoutId, const std::vector<std::string>& imageIds,
                                                 const std::function<void(const std::string&)>& log);
} // namespace guitarfx::controller_detail

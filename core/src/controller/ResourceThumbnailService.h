#pragma once

// ResourceThumbnailService — the small pictures the resource browser's local lists (the Library
// and the Folder tab's files that are in the library) show for a model or IR that came from a
// Tone3000 tone: the tone's picture, as a thumbnail kept in the profile's cache.
//
// The engine makes no network requests, so the UIs fetch and this keeps what they fetched, once:
//
// - "getResourceThumbnails" says, for library resources of one type, which picture each shows:
//   a file already in the cache, a URL to download a thumbnail-sized copy from, or nothing. The
//   picture is the one the import recorded (metadata "imageUrl"), else another resource's of the
//   same tone, else one a lookup found. A tone whose picture was never recorded (an import
//   from before the URL was kept) and has a title is listed for the UI to look up: Soundshed's
//   Tone3000 relay has no tone-by-id path, so the UI searches by the title and matches the id.
// - "saveResourceThumbnail" stores a thumbnail the UI downloaded, and what a lookup found (a
//   picture, or that the tone has none), so neither is asked for again.
//
// The cache is <profile>/data/v1/cache/resource-thumbnails: one file per picture, named by a
// hash of the picture's URL (so every resource of a tone shares it), and tones.json, the
// lookups' results. The web UI loads a file through the editor's `resource-thumbnails/<file>`
// route (ResolveUiResource); a native UI loads it from its path. Nothing in the resource library
// is changed, so none of this broadcasts the library or bumps shared sync.
//
// Message thread only, but for the static functions, which touch no state.

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <utility>

#include <nlohmann/json.hpp>

namespace guitarfx
{
class MessageHandlerRegistry;
class ResourceLibrary;

class ResourceThumbnailService
{
  public:
    using SendMessageFn = std::function<void(const std::string&)>;

    /// The edge, in pixels, of the square a thumbnail is fetched at: the lists draw 36 px
    /// squares, and this covers them at twice that and a little more.
    static constexpr int kEdge = 96;
    /// A stored thumbnail is at most this big. The sized copy is a few kilobytes; a picture on a
    /// host that cannot size it is taken whole only while it stays under this.
    static constexpr std::size_t kMaxBytes = 512 * 1024;
    /// A lookup that found no picture is tried again after this long.
    static constexpr std::int64_t kNoPictureRetrySeconds = 30LL * 24 * 60 * 60;

    ResourceThumbnailService(SendMessageFn sendMessage, const ResourceLibrary& resourceLibrary);

    ResourceThumbnailService(const ResourceThumbnailService&) = delete;
    ResourceThumbnailService& operator=(const ResourceThumbnailService&) = delete;

    /// Registers "getResourceThumbnails" and "saveResourceThumbnail" with the dispatcher.
    void RegisterMessageHandlers(MessageHandlerRegistry& registry);

    /// The cache folder: <profile>/data/v1/cache/resource-thumbnails.
    [[nodiscard]] static std::filesystem::path CacheDirectory();

    /// The name a picture's thumbnail is kept under, without the extension: 16 hex digits of the
    /// FNV-1a hash of its URL.
    [[nodiscard]] static std::string KeyFor(const std::string& imageUrl);

    /// Where to download a thumbnail of `imageUrl` from: Tone3000's picture storage serves a sized
    /// copy (/storage/v1/render/image/public/... at kEdge, cropped to fill), anything else is
    /// fetched as it is.
    [[nodiscard]] static std::string FetchUrlFor(const std::string& imageUrl);

    /// The file and MIME type for a UI resource path `resource-thumbnails/<key>.<ext>` (no
    /// query), or nullopt when the path is not one or the file is not in the cache. Any thread.
    [[nodiscard]] static std::optional<std::pair<std::filesystem::path, std::string>> ResolveUiResource(
        const std::string& resourcePath);

  private:
    struct ToneEntry
    {
        std::string imageUrl; // empty: the tone has no picture
        std::int64_t checked = 0;
    };

    void HandleGet(const nlohmann::json& payload);
    void HandleSave(const nlohmann::json& payload);

    void LoadIndex();
    void SaveIndex();
    /// The cached file's name for `key`, or empty when it is not on disk.
    std::string CachedFile(const std::string& key);

    SendMessageFn mSendMessage;
    const ResourceLibrary& mResourceLibrary;

    bool mIndexLoaded = false;
    std::map<std::string, ToneEntry> mTones;   // tone id -> what a lookup found
    std::map<std::string, std::string> mFiles; // key -> file name, as found on disk
};
} // namespace guitarfx

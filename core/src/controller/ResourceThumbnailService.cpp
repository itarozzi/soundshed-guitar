/**
 * ResourceThumbnailService.cpp — the resource browser's thumbnails of Tone3000 tones, kept in
 * the profile's cache. See ResourceThumbnailService.h for what each message does and where the
 * files live.
 */

#include "controller/ResourceThumbnailService.h"

#include "MessageHandlerRegistry.h"
#include "resources/ResourceLibrary.h"
#include "util/Base64.h"
#include "util/FileIO.h"
#include "util/FileSystem.h"
#include "util/LongPath.h"
#include "util/PathEncoding.h"
#include "util/SessionLog.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <set>
#include <string_view>
#include <system_error>
#include <vector>

namespace guitarfx
{
namespace
{
constexpr std::string_view kRoutePrefix = "resource-thumbnails/";
// Tone3000 keeps its pictures in Supabase storage, which serves a resized copy of any public
// object under /render/image/ (the same ETag as the original, a few kilobytes at 96 px).
constexpr std::string_view kStorageObject = "https://api.tone3000.com/storage/v1/object/public/";
constexpr std::string_view kStorageRender = "https://api.tone3000.com/storage/v1/render/image/public/";
constexpr const char* kIndexFileName = "tones.json";
constexpr std::array<std::string_view, 4> kExtensions = {".jpg", ".png", ".webp", ".gif"};
/// How many resources one request may name; more than any library lists in one view.
constexpr std::size_t kMaxRequestedIds = 5000;

/// A field as text whether it is a string or a whole number (ids come as both); else empty.
std::string Text(const nlohmann::json& object, const char* key)
{
    if (!object.is_object())
    {
        return {};
    }

    const auto it = object.find(key);

    if (it == object.end())
    {
        return {};
    }

    if (it->is_string())
    {
        return it->get<std::string>();
    }

    if (it->is_number_integer())
    {
        return std::to_string(it->get<std::int64_t>());
    }

    return {};
}

std::string Meta(const LibraryResource& resource, const char* key)
{
    const auto it = resource.metadata.find(key);
    return it != resource.metadata.end() ? it->second : std::string();
}

bool StartsWith(std::string_view text, std::string_view prefix)
{
    return text.size() >= prefix.size() && text.compare(0, prefix.size(), prefix) == 0;
}

/// An absolute web address the UIs may fetch: http(s), not absurdly long, nothing that could
/// break out of a quoted attribute or a URL.
bool IsWebUrl(std::string_view url)
{
    if (!(StartsWith(url, "https://") || StartsWith(url, "http://")) || url.size() > 2048)
    {
        return false;
    }

    return std::none_of(url.begin(), url.end(), [](char c) {
        const auto ch = static_cast<unsigned char>(c);
        return ch <= 0x20 || ch == 0x7f || c == '"' || c == '\'' || c == '<' || c == '>' || c == '\\';
    });
}

/// A Tone3000 tone id as the API gives one (digits), or an id another provider might use.
bool IsToneId(std::string_view id)
{
    return !id.empty() && id.size() <= 64 && std::all_of(id.begin(), id.end(), [](char c) {
        return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '-' || c == '_';
    });
}

bool IsKey(std::string_view key)
{
    return key.size() == 16 &&
           std::all_of(key.begin(), key.end(), [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
}

/// A resource that came from Tone3000: imported from it, or named after one of its models.
bool IsTone3000Resource(const LibraryResource& resource)
{
    return Meta(resource, "provider") == "tone3000" || StartsWith(resource.id, "tone3000:") ||
           !Meta(resource, "toneId").empty();
}

std::string ToneIdOf(const LibraryResource& resource)
{
    auto id = Meta(resource, "toneId");

    if (id.empty() && Meta(resource, "provider") == "tone3000")
    {
        id = Meta(resource, "groupId");
    }

    return IsToneId(id) ? id : std::string();
}

std::string ToneTitleOf(const LibraryResource& resource)
{
    auto title = Meta(resource, "toneTitle");
    return title.empty() ? Meta(resource, "groupName") : title;
}

/// The extension a picture's bytes say it is (the formats both UIs decode), or empty.
std::string_view SniffExtension(const std::vector<std::uint8_t>& bytes)
{
    const auto starts = [&bytes](std::initializer_list<std::uint8_t> magic, std::size_t at = 0) {
        return bytes.size() >= at + magic.size() && std::equal(magic.begin(), magic.end(), bytes.begin() + at);
    };

    if (starts({0xFF, 0xD8, 0xFF}))
    {
        return ".jpg";
    }

    if (starts({0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A}))
    {
        return ".png";
    }

    if (starts({'R', 'I', 'F', 'F'}) && starts({'W', 'E', 'B', 'P'}, 8))
    {
        return ".webp";
    }

    if (starts({'G', 'I', 'F', '8'}))
    {
        return ".gif";
    }

    return {};
}

std::string MimeFor(std::string_view extension)
{
    if (extension == ".png")
    {
        return "image/png";
    }

    if (extension == ".webp")
    {
        return "image/webp";
    }

    if (extension == ".gif")
    {
        return "image/gif";
    }

    return "image/jpeg";
}

std::int64_t NowSeconds()
{
    return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch())
        .count();
}

void Log(const std::string& message)
{
    util::AppendSessionLog("[ResourceThumbnails] " + message);
}
} // namespace

ResourceThumbnailService::ResourceThumbnailService(SendMessageFn sendMessage, const ResourceLibrary& resourceLibrary)
    : mSendMessage(std::move(sendMessage)), mResourceLibrary(resourceLibrary)
{
}

void ResourceThumbnailService::RegisterMessageHandlers(MessageHandlerRegistry& registry)
{
    registry.Register("getResourceThumbnails", [this](const nlohmann::json& payload) { HandleGet(payload); });
    registry.Register("saveResourceThumbnail", [this](const nlohmann::json& payload) { HandleSave(payload); });
}

std::filesystem::path ResourceThumbnailService::CacheDirectory()
{
    return FileSystem{}.ResolveCacheDirectory() / "resource-thumbnails";
}

std::string ResourceThumbnailService::KeyFor(const std::string& imageUrl)
{
    std::uint64_t hash = 14695981039346656037ULL;

    for (const char c : imageUrl)
    {
        hash ^= static_cast<std::uint8_t>(c);
        hash *= 1099511628211ULL;
    }

    char text[17];
    std::snprintf(text, sizeof(text), "%016llx", static_cast<unsigned long long>(hash));
    return text;
}

std::string ResourceThumbnailService::FetchUrlFor(const std::string& imageUrl)
{
    if (!StartsWith(imageUrl, kStorageObject))
    {
        return imageUrl;
    }

    const auto rest = imageUrl.substr(kStorageObject.size());
    const auto edge = std::to_string(kEdge);
    return std::string(kStorageRender) + rest + (rest.find('?') == std::string::npos ? "?" : "&") + "width=" + edge +
           "&height=" + edge + "&resize=cover";
}

std::optional<std::pair<std::filesystem::path, std::string>> ResourceThumbnailService::ResolveUiResource(
    const std::string& resourcePath)
{
    if (!StartsWith(resourcePath, kRoutePrefix))
    {
        return std::nullopt;
    }

    const std::string_view name = std::string_view(resourcePath).substr(kRoutePrefix.size());
    const auto dot = name.find('.');

    if (dot == std::string_view::npos || !IsKey(name.substr(0, dot)) ||
        std::find(kExtensions.begin(), kExtensions.end(), name.substr(dot)) == kExtensions.end())
    {
        return std::nullopt;
    }

    const auto file = CacheDirectory() / util::PathFromUtf8(std::string(name));

    if (!util::PathExists(file))
    {
        return std::nullopt;
    }

    return std::make_pair(file, MimeFor(name.substr(dot)));
}

void ResourceThumbnailService::HandleGet(const nlohmann::json& payload)
{
    const auto resourceType = Text(payload, "resourceType");

    if (resourceType != "nam" && resourceType != "ir")
    {
        return;
    }

    // Only the resources asked about, when the request names them (the rows a list shows).
    std::set<std::string> wanted;
    const auto ids = payload.find("resourceIds");
    const bool named = ids != payload.end() && ids->is_array();

    if (named)
    {
        for (const auto& id : *ids)
        {
            if (id.is_string() && wanted.size() < kMaxRequestedIds)
            {
                wanted.insert(id.get<std::string>());
            }
        }
    }

    const auto resources = mResourceLibrary.GetResourcesByType(resourceType);

    // What each tone's resources say between them: the picture one of them recorded, that an
    // import found none, and the tone's title for a lookup.
    struct ToneFacts
    {
        std::string imageUrl;
        bool recordedNone = false;
        std::string title;
    };

    std::map<std::string, ToneFacts> tones;

    for (const auto& resource : resources)
    {
        const auto toneId = ToneIdOf(resource);

        if (toneId.empty() || !IsTone3000Resource(resource))
        {
            continue;
        }

        auto& facts = tones[toneId];
        const auto imageUrl = Meta(resource, "imageUrl");

        if (IsWebUrl(imageUrl) && facts.imageUrl.empty())
        {
            facts.imageUrl = imageUrl;
        }
        else if (imageUrl.empty() && resource.metadata.count("imageUrl") > 0)
        {
            facts.recordedNone = true;
        }

        if (facts.title.empty())
        {
            facts.title = ToneTitleOf(resource);
        }
    }

    LoadIndex();
    const auto now = NowSeconds();

    nlohmann::json thumbnails = nlohmann::json::object();
    nlohmann::json files = nlohmann::json::object();
    nlohmann::json fetch = nlohmann::json::object();
    std::map<std::string, nlohmann::json> lookups; // tone id -> {toneId, title, resourceIds}

    for (const auto& resource : resources)
    {
        if ((named && wanted.count(resource.id) == 0) || !IsTone3000Resource(resource))
        {
            continue;
        }

        const auto toneId = ToneIdOf(resource);
        const auto* facts = toneId.empty() ? nullptr : &tones[toneId];
        auto imageUrl = Meta(resource, "imageUrl");

        if (!IsWebUrl(imageUrl))
        {
            imageUrl = facts != nullptr ? facts->imageUrl : std::string();
        }

        if (imageUrl.empty() && facts != nullptr)
        {
            const auto looked = mTones.find(toneId);
            const bool known = looked != mTones.end() && (!looked->second.imageUrl.empty() ||
                                                          now - looked->second.checked < kNoPictureRetrySeconds);

            if (known)
            {
                imageUrl = looked->second.imageUrl;
            }
            else if (!facts->recordedNone && !facts->title.empty())
            {
                auto& lookup = lookups[toneId];

                if (lookup.is_null())
                {
                    lookup = {{"toneId", toneId}, {"title", facts->title}, {"resourceIds", nlohmann::json::array()}};
                }

                lookup["resourceIds"].push_back(resource.id);
            }
        }

        if (!IsWebUrl(imageUrl))
        {
            continue; // no picture, or none known yet
        }

        const auto key = KeyFor(imageUrl);
        thumbnails[resource.id] = key;

        if (files.contains(key) || fetch.contains(key))
        {
            continue;
        }

        if (const auto file = CachedFile(key); !file.empty())
        {
            files[key] = {{"file", file}, {"path", util::PathToUtf8(CacheDirectory() / util::PathFromUtf8(file))}};
        }
        else
        {
            fetch[key] = {{"url", FetchUrlFor(imageUrl)}, {"imageUrl", imageUrl}};
        }
    }

    nlohmann::json lookupList = nlohmann::json::array();

    for (auto& [toneId, lookup] : lookups)
    {
        lookupList.push_back(std::move(lookup));
    }

    nlohmann::json answer{{"type", "resourceThumbnails"},        {"resourceType", resourceType},
                          {"thumbnails", std::move(thumbnails)}, {"files", std::move(files)},
                          {"fetch", std::move(fetch)},           {"lookup", std::move(lookupList)}};

    if (const auto requestId = Text(payload, "requestId"); !requestId.empty())
    {
        answer["requestId"] = requestId;
    }

    mSendMessage(answer.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace));
}

void ResourceThumbnailService::HandleSave(const nlohmann::json& payload)
{
    const auto imageUrl = Text(payload, "imageUrl");
    const auto toneId = Text(payload, "toneId");

    // What a lookup found for a tone: its picture, or that it has none.
    if (!toneId.empty())
    {
        if (!IsToneId(toneId) || (!imageUrl.empty() && !IsWebUrl(imageUrl)))
        {
            Log("refused a lookup result for tone '" + toneId.substr(0, 64) + "'");
            return;
        }

        LoadIndex();
        mTones[toneId] = {imageUrl, NowSeconds()};
        SaveIndex();
    }

    const auto data = Text(payload, "data");

    if (data.empty())
    {
        return;
    }

    if (!IsWebUrl(imageUrl) || data.size() > (kMaxBytes / 3 + 1) * 4)
    {
        Log("refused a thumbnail: no picture URL, or larger than " + std::to_string(kMaxBytes) + " bytes");
        return;
    }

    const auto bytes = util::DecodeBase64(data);
    const auto extension = SniffExtension(bytes);

    if (bytes.empty() || bytes.size() > kMaxBytes || extension.empty())
    {
        Log("refused a thumbnail that is not a JPEG, PNG, WebP or GIF picture");
        return;
    }

    const auto directory = CacheDirectory();

    if (!FileSystem{}.EnsureDirectory(directory))
    {
        Log("could not create " + util::PathToUtf8(directory));
        return;
    }

    const auto key = KeyFor(imageUrl);
    const auto name = key + std::string(extension);
    std::string error;

    if (!util::WriteFileAtomic(directory / util::PathFromUtf8(name), bytes, &error))
    {
        Log("could not write " + name + ": " + error);
        return;
    }

    // One file per picture: a copy in another format (the server's choice can change) goes.
    for (const auto other : kExtensions)
    {
        if (other != extension)
        {
            std::error_code ec;
            std::filesystem::remove(util::LongPathSafe(directory / util::PathFromUtf8(key + std::string(other))), ec);
        }
    }

    mFiles[key] = name;
}

void ResourceThumbnailService::LoadIndex()
{
    if (mIndexLoaded)
    {
        return;
    }

    mIndexLoaded = true;
    std::ifstream file(util::LongPathSafe(CacheDirectory() / kIndexFileName), std::ios::binary);

    if (!file)
    {
        return;
    }

    const std::string text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    const auto index = nlohmann::json::parse(text, nullptr, false);
    const auto list = index.is_object() ? index.find("tones") : index.end();

    if (!index.is_object() || list == index.end() || !list->is_object())
    {
        return;
    }

    for (const auto& [toneId, entry] : list->items())
    {
        const auto imageUrl = Text(entry, "imageUrl");
        const auto checked = entry.is_object() && entry.contains("checked") && entry["checked"].is_number_integer()
                                 ? entry["checked"].get<std::int64_t>()
                                 : 0;

        if (IsToneId(toneId) && (imageUrl.empty() || IsWebUrl(imageUrl)))
        {
            mTones[toneId] = {imageUrl, checked};
        }
    }
}

void ResourceThumbnailService::SaveIndex()
{
    // Another instance (a plugin beside the standalone) may have written lookups since this one
    // read the file: keep theirs, the newer of two answers for one tone winning.
    auto mine = std::move(mTones);
    mTones.clear();
    mIndexLoaded = false;
    LoadIndex();

    for (auto& [toneId, entry] : mine)
    {
        if (const auto it = mTones.find(toneId); it == mTones.end() || it->second.checked <= entry.checked)
        {
            mTones[toneId] = std::move(entry);
        }
    }

    nlohmann::json list = nlohmann::json::object();

    for (const auto& [toneId, entry] : mTones)
    {
        list[toneId] = {{"imageUrl", entry.imageUrl}, {"checked", entry.checked}};
    }

    const auto directory = CacheDirectory();
    std::string error;

    if (!FileSystem{}.EnsureDirectory(directory) ||
        !util::WriteTextFileAtomic(directory / kIndexFileName, nlohmann::json{{"version", 1}, {"tones", list}}.dump(2),
                                   &error))
    {
        Log("could not save " + std::string(kIndexFileName) + ": " + error);
    }
}

std::string ResourceThumbnailService::CachedFile(const std::string& key)
{
    const auto directory = CacheDirectory();

    // Remembered while it is still there (the cache folder may have been emptied since).
    if (const auto it = mFiles.find(key); it != mFiles.end())
    {
        if (util::PathExists(directory / util::PathFromUtf8(it->second)))
        {
            return it->second;
        }

        mFiles.erase(it);
    }

    // Another instance may have stored it, so look on disk.
    for (const auto extension : kExtensions)
    {
        auto name = key + std::string(extension);

        if (util::PathExists(directory / util::PathFromUtf8(name)))
        {
            mFiles[key] = name;
            return name;
        }
    }

    return {};
}
} // namespace guitarfx

#pragma once

/**
 * Jam.h - The Jam tab's backing tracks as a native UI shows them: the YouTube search, its one
 * cached answer, and the favourites. Everything is kept in the same app settings the web UI's
 * jam/state.ts uses (jam.favorites, jam.searchCache), so a favourite starred in either product
 * shows in both. The search itself is ToneService::SearchBackingTracks; the key is the engine's
 * bundled jam.youtubeApiKey setting.
 *
 * Playing a track is web content (YouTube's embedded player, through the Soundshed API's wrapper
 * page, as jam/player.ts embeds it), so a UI shows EmbedUrl in a web view of its own.
 */

#include <nlohmann/json.hpp>

#include <optional>
#include <string>
#include <vector>

namespace guitarfx::uiclient::jam
{
inline constexpr const char* kApiKeySetting = "jam.youtubeApiKey";
inline constexpr const char* kFavoritesSetting = "jam.favorites";
inline constexpr const char* kSearchCacheSetting = "jam.searchCache";
inline constexpr const char* kDefaultQuery = "backing track";
inline constexpr int kSearchMaxResults = 12;

struct Video
{
    std::string videoId;
    std::string title;
    std::string channelTitle;
    std::string thumbnailUrl;
};

/// The query as sent: spaces collapsed, and "backing track" added unless it is there already.
/// Empty for an empty query.
[[nodiscard]] std::string NormalizeQuery(const std::string& rawQuery);

/// YouTube's search answers with titles HTML-escaped ("80&#39;s Rock"): the text as written.
[[nodiscard]] std::string DecodeYouTubeText(const std::string& value);

/// The search request for a normalised query.
[[nodiscard]] std::string SearchUrl(const std::string& normalizedQuery, const std::string& apiKey);

/// The videos in a search answer; entries without a video id are left out.
[[nodiscard]] std::vector<Video> ParseSearchResults(const nlohmann::json& payload);

/// The API's own message from a failed answer, or "Request failed.".
[[nodiscard]] std::string DescribeError(const nlohmann::json& payload);

/// The bundled key, or empty when this build has none.
[[nodiscard]] std::string ApiKey(const nlohmann::json& appSettings);

[[nodiscard]] std::vector<Video> Favorites(const nlohmann::json& appSettings);
[[nodiscard]] nlohmann::json VideosJson(const std::vector<Video>& videos);
[[nodiscard]] bool IsFavorite(const std::vector<Video>& favorites, const std::string& videoId);
/// Removes `video` when it is a favourite, else puts it first.
[[nodiscard]] std::vector<Video> ToggleFavorite(std::vector<Video> favorites, const Video& video);

struct SearchCache
{
    std::string query;           // as typed
    std::string normalizedQuery; // as sent
    std::vector<Video> results;
};

/// The one search kept (the last), or nothing.
[[nodiscard]] std::optional<SearchCache> ReadSearchCache(const nlohmann::json& appSettings);
/// The kept search when it answers `query`, or nothing.
[[nodiscard]] std::optional<SearchCache> CachedSearch(const nlohmann::json& appSettings, const std::string& query);
/// What kSearchCacheSetting stores for a search; null for an empty query.
[[nodiscard]] nlohmann::json SearchCacheJson(const std::string& query, const std::vector<Video>& results);

/// The player page for a video: the Soundshed API's wrapper around YouTube's embedded player,
/// which gives it the https parent origin YouTube wants.
[[nodiscard]] std::string EmbedUrl(const std::string& videoId);
/// The video on YouTube itself, for a UI with no web view.
[[nodiscard]] std::string WatchUrl(const std::string& videoId);
/// Whether a page a web view is asked to load is one the player may show in place; anything else
/// (a click through to YouTube) goes to the system browser.
[[nodiscard]] bool StaysInWebView(const std::string& url);
} // namespace guitarfx::uiclient::jam

#pragma once

/**
 * ResourceThumbnails.h - The resource browser's tone pictures as the native UIs keep them.
 *
 * A library model or IR that came from a Tone3000 tone shows the tone's picture in the browser's
 * local lists. The engine keeps those pictures as small files (controller/ResourceThumbnailService.h)
 * but makes no requests, so a UI fills its cache:
 *
 * - ResourceThumbnails is the engine's answers to getResourceThumbnails, for the session: which
 *   picture each resource shows, and whether it is a cached file or still to be fetched. UiClient
 *   applies them (Topic::Thumbnails).
 * - ResourceThumbnailFetcher does the fetching: a thumbnail downloaded through ToneService (the
 *   sized URL the engine gave), then handed to the engine to keep (saveResourceThumbnail); a tone
 *   whose picture was never recorded looked up on Tone3000 through Soundshed's relay, by its
 *   title, and what was found handed to the engine too. Never more than kMaxLookups lookups a
 *   session, one at a time, so the relay's shared key is not run down.
 *
 * The web UI's equivalent is core/ui/ts/resourceBrowser/libraryThumbnails.ts. UI thread only.
 */

#include <nlohmann/json.hpp>

#include <cstddef>
#include <deque>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace guitarfx::uiclient
{
class ToneService;
class UiClient;

class ResourceThumbnails
{
  public:
    /// Lookups of tones with no recorded picture a session may make, all UIs counted apart.
    static constexpr int kMaxLookups = 12;

    struct Picture
    {
        std::string key;
        std::string path;     // the cached file, once the engine has it
        std::string fetchUrl; // else where to download a thumbnail from
        std::string imageUrl; // the picture the thumbnail is of
    };

    struct Lookup
    {
        std::string resourceType;
        std::string toneId;
        std::string title;
        std::vector<std::string> resourceIds;
    };

    /// The picture a library resource shows; null when it has none, or the engine has not said.
    [[nodiscard]] const Picture* Find(const std::string& resourceType, const std::string& resourceId) const;

    /// Of `resourceIds`, those the engine has not been asked about this session, now marked asked.
    [[nodiscard]] std::vector<std::string> TakeUnasked(const std::string& resourceType,
                                                       const std::vector<std::string>& resourceIds);

    /// Asks about these again on the next TakeUnasked (after a lookup found their picture).
    void Forget(const std::string& resourceType, const std::vector<std::string>& resourceIds);

    /// Applies a "resourceThumbnails" answer.
    void Apply(const nlohmann::json& message);

    /// The pictures still to be fetched, by key.
    [[nodiscard]] std::vector<Picture> ToFetch() const;

    /// Records that the thumbnail for `key` was handed to the engine (or could not be fetched),
    /// so it is not sent again.
    void MarkSaved(const std::string& key);

    /// The next tone to look up, or false when there is none left or the session's lookups are
    /// spent. A tone is looked up at most once a session.
    bool NextLookup(Lookup& lookup);

  private:
    static std::string Id(const std::string& resourceType, const std::string& resourceId)
    {
        return resourceType + '\n' + resourceId;
    }

    std::map<std::string, std::string> mKeys; // type + id -> picture key
    std::map<std::string, Picture> mPictures; // key -> picture
    std::set<std::string> mAsked;             // type + id
    std::set<std::string> mSaved;             // keys handed to the engine
    std::deque<Lookup> mLookups;
    std::set<std::string> mLookedUp; // tone ids, queued or done
    int mLookupsStarted = 0;
};

class ResourceThumbnailFetcher
{
  public:
    ResourceThumbnailFetcher(UiClient& client, ToneService& tones);
    ~ResourceThumbnailFetcher();

    ResourceThumbnailFetcher(const ResourceThumbnailFetcher&) = delete;
    ResourceThumbnailFetcher& operator=(const ResourceThumbnailFetcher&) = delete;

    /// Asks the engine which pictures these library resources show (those not asked yet).
    void Request(const std::string& resourceType, const std::vector<std::string>& resourceIds);

    /// The picture a resource shows, as Find; a native UI draws `path` when it is set, else the
    /// bytes ToneService::Image(fetchUrl) gives.
    [[nodiscard]] const ResourceThumbnails::Picture* Find(const std::string& resourceType,
                                                          const std::string& resourceId) const;

    /// Call when the engine has answered (Topic::Thumbnails) and when ToneService has news
    /// (a download arrived): starts the downloads and lookups the answers ask for, and hands what
    /// arrived to the engine. Does nothing while the Tone3000 feature is switched off (cached
    /// pictures still show).
    void Update();

  private:
    void StartNextLookup();

    UiClient& mClient;
    ToneService& mTones;
    std::shared_ptr<bool> mAlive = std::make_shared<bool>(true);
    bool mLookingUp = false;
};
} // namespace guitarfx::uiclient

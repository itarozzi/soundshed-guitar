#include "uiclient/ResourceThumbnails.h"

#include "uiclient/FeatureFlags.h"
#include "uiclient/ToneService.h"
#include "uiclient/UiClient.h"
#include "util/Base64.h"

#include <cstdint>
#include <utility>

namespace guitarfx::uiclient
{
namespace
{
/// The engine keeps no thumbnail bigger than this (ResourceThumbnailService::kMaxBytes).
constexpr std::size_t kMaxThumbnailBytes = 512 * 1024;

std::string Str(const nlohmann::json& object, const char* key)
{
    if (!object.is_object())
    {
        return {};
    }

    const auto it = object.find(key);
    return it != object.end() && it->is_string() ? it->get<std::string>() : std::string();
}
} // namespace

// ── ResourceThumbnails ──────────────────────────────────────────────────────────

const ResourceThumbnails::Picture* ResourceThumbnails::Find(const std::string& resourceType,
                                                            const std::string& resourceId) const
{
    const auto key = mKeys.find(Id(resourceType, resourceId));

    if (key == mKeys.end())
    {
        return nullptr;
    }

    const auto picture = mPictures.find(key->second);
    return picture != mPictures.end() ? &picture->second : nullptr;
}

std::vector<std::string> ResourceThumbnails::TakeUnasked(const std::string& resourceType,
                                                         const std::vector<std::string>& resourceIds)
{
    std::vector<std::string> unasked;

    for (const auto& id : resourceIds)
    {
        if (mAsked.insert(Id(resourceType, id)).second)
        {
            unasked.push_back(id);
        }
    }

    return unasked;
}

void ResourceThumbnails::Forget(const std::string& resourceType, const std::vector<std::string>& resourceIds)
{
    for (const auto& id : resourceIds)
    {
        mAsked.erase(Id(resourceType, id));
    }
}

void ResourceThumbnails::Apply(const nlohmann::json& message)
{
    const auto resourceType = Str(message, "resourceType");

    if (const auto it = message.find("thumbnails"); it != message.end() && it->is_object())
    {
        for (const auto& [id, key] : it->items())
        {
            if (key.is_string())
            {
                mKeys[Id(resourceType, id)] = key.get<std::string>();
            }
        }
    }

    if (const auto it = message.find("files"); it != message.end() && it->is_object())
    {
        for (const auto& [key, file] : it->items())
        {
            if (const auto path = Str(file, "path"); !path.empty())
            {
                auto& picture = mPictures[key];
                picture.key = key;
                picture.path = path;
                picture.fetchUrl.clear();
            }
        }
    }

    if (const auto it = message.find("fetch"); it != message.end() && it->is_object())
    {
        for (const auto& [key, fetch] : it->items())
        {
            auto& picture = mPictures[key];
            picture.key = key;
            picture.imageUrl = Str(fetch, "imageUrl");

            if (picture.path.empty())
            {
                picture.fetchUrl = Str(fetch, "url");
            }
        }
    }

    if (const auto it = message.find("lookup"); it != message.end() && it->is_array())
    {
        for (const auto& entry : *it)
        {
            Lookup lookup;
            lookup.resourceType = resourceType;
            lookup.toneId = Str(entry, "toneId");
            lookup.title = Str(entry, "title");

            if (const auto ids = entry.find("resourceIds"); ids != entry.end() && ids->is_array())
            {
                for (const auto& id : *ids)
                {
                    if (id.is_string())
                    {
                        lookup.resourceIds.push_back(id.get<std::string>());
                    }
                }
            }

            if (!lookup.toneId.empty() && !lookup.title.empty() && mLookedUp.insert(lookup.toneId).second)
            {
                mLookups.push_back(std::move(lookup));
            }
        }
    }
}

std::vector<ResourceThumbnails::Picture> ResourceThumbnails::ToFetch() const
{
    std::vector<Picture> pictures;

    for (const auto& [key, picture] : mPictures)
    {
        if (picture.path.empty() && !picture.fetchUrl.empty() && mSaved.count(key) == 0)
        {
            pictures.push_back(picture);
        }
    }

    return pictures;
}

void ResourceThumbnails::MarkSaved(const std::string& key)
{
    mSaved.insert(key);
}

bool ResourceThumbnails::NextLookup(Lookup& lookup)
{
    if (mLookups.empty() || mLookupsStarted >= kMaxLookups)
    {
        return false;
    }

    lookup = std::move(mLookups.front());
    mLookups.pop_front();
    ++mLookupsStarted;
    return true;
}

// ── ResourceThumbnailFetcher ────────────────────────────────────────────────────

ResourceThumbnailFetcher::ResourceThumbnailFetcher(UiClient& client, ToneService& tones)
    : mClient(client), mTones(tones)
{
}

ResourceThumbnailFetcher::~ResourceThumbnailFetcher()
{
    *mAlive = false;
}

void ResourceThumbnailFetcher::Request(const std::string& resourceType, const std::vector<std::string>& resourceIds)
{
    auto ids = mClient.Thumbnails().TakeUnasked(resourceType, resourceIds);

    if (!ids.empty())
    {
        mClient.Send("getResourceThumbnails", {{"resourceType", resourceType}, {"resourceIds", std::move(ids)}});
    }
}

const ResourceThumbnails::Picture* ResourceThumbnailFetcher::Find(const std::string& resourceType,
                                                                  const std::string& resourceId) const
{
    return mClient.Thumbnails().Find(resourceType, resourceId);
}

void ResourceThumbnailFetcher::Update()
{
    // Cached pictures show regardless; the network is only asked while Tone3000 is switched on.
    if (!IsFeatureEnabled(mClient.State(), Feature::Tone3000))
    {
        return;
    }

    auto& thumbnails = mClient.Thumbnails();

    for (const auto& picture : thumbnails.ToFetch())
    {
        // The first call starts the download; ToneService says when it has arrived.
        const auto* bytes = mTones.Image(picture.fetchUrl);

        if (bytes == nullptr)
        {
            continue;
        }

        if (!bytes->empty() && bytes->size() <= kMaxThumbnailBytes && !picture.imageUrl.empty())
        {
            const std::vector<std::uint8_t> data(bytes->begin(), bytes->end());
            mClient.Send("saveResourceThumbnail", {{"imageUrl", picture.imageUrl}, {"data", util::EncodeBase64(data)}});
        }

        thumbnails.MarkSaved(picture.key); // a failed download is not retried, as ToneService does not
    }

    StartNextLookup();
}

void ResourceThumbnailFetcher::StartNextLookup()
{
    ResourceThumbnails::Lookup lookup;

    if (mLookingUp || !mClient.Thumbnails().NextLookup(lookup))
    {
        return;
    }

    mLookingUp = true;
    // The relay has no tone-by-id path: search by the title and take the tone with this id.
    mTones.SearchTone3000(
        lookup.title, {}, {}, {}, 1,
        [this, alive = mAlive, lookup](const tone3000::Page<tone3000::Tone>& page, const std::string& error) {
            if (!*alive)
            {
                return;
            }

            mLookingUp = false;

            if (error.empty())
            {
                // Not among the results counts as no picture; the engine tries again in a month.
                std::string imageUrl;

                for (const auto& tone : page.items)
                {
                    if (tone.id == lookup.toneId)
                    {
                        imageUrl = tone.imageUrl;
                        break;
                    }
                }

                mClient.Send("saveResourceThumbnail", {{"toneId", lookup.toneId}, {"imageUrl", imageUrl}});

                if (!imageUrl.empty())
                {
                    mClient.Thumbnails().Forget(lookup.resourceType, lookup.resourceIds);
                    Request(lookup.resourceType, lookup.resourceIds);
                }
            }

            StartNextLookup();
        });
}
} // namespace guitarfx::uiclient

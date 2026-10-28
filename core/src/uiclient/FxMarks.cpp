#include "uiclient/FxMarks.h"

#include "uiclient/UiCommands.h"

#include <algorithm>
#include <limits>

namespace guitarfx::uiclient
{
namespace
{
/// Strings only, each once, in order (normalizeFxKeys).
std::vector<std::string> Keys(const ClientState& state, const char* name, std::size_t limit)
{
    std::vector<std::string> keys;
    const auto it = state.uiSettings.find(name);

    if (it == state.uiSettings.end() || !it->is_array())
    {
        return keys;
    }

    for (const auto& entry : *it)
    {
        if (!entry.is_string())
        {
            continue;
        }

        auto key = entry.get<std::string>();

        if (key.empty() || std::find(keys.begin(), keys.end(), key) != keys.end())
        {
            continue;
        }

        keys.push_back(std::move(key));

        if (keys.size() >= limit)
        {
            break;
        }
    }

    return keys;
}
} // namespace

std::string FxItemKey(const std::string& effectType, const std::string& blendId, const std::string& customEffectId)
{
    return effectType + "|" + blendId + "|" + customEffectId;
}

std::vector<std::string> FxFavoriteKeys(const ClientState& state)
{
    return Keys(state, "fxFavorites", std::numeric_limits<std::size_t>::max());
}

std::vector<std::string> FxRecentKeys(const ClientState& state)
{
    return Keys(state, "fxRecents", kFxRecentsLimit);
}

bool IsFxFavorite(const ClientState& state, const std::string& key)
{
    const auto keys = FxFavoriteKeys(state);
    return std::find(keys.begin(), keys.end(), key) != keys.end();
}

bool ToggleFxFavorite(UiCommands& commands, const ClientState& state, const std::string& key)
{
    auto keys = FxFavoriteKeys(state);
    const auto it = std::find(keys.begin(), keys.end(), key);
    const bool favorite = it == keys.end();

    if (favorite)
    {
        keys.push_back(key);
    }
    else
    {
        keys.erase(it);
    }

    commands.PatchUiSettings({{"fxFavorites", keys}});
    return favorite;
}

void RecordFxRecent(UiCommands& commands, const ClientState& state, const std::string& key)
{
    auto keys = FxRecentKeys(state);

    if (!keys.empty() && keys.front() == key)
    {
        return;
    }

    keys.erase(std::remove(keys.begin(), keys.end(), key), keys.end());
    keys.insert(keys.begin(), key);

    if (keys.size() > kFxRecentsLimit)
    {
        keys.resize(kFxRecentsLimit);
    }

    commands.PatchUiSettings({{"fxRecents", keys}});
}
} // namespace guitarfx::uiclient

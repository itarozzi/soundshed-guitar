#pragma once

/**
 * FxMarks.h - The effect chooser's favourites and recently used effects.
 *
 * The web UI's lists (core/ui/ts/fxLibrary/favorites.ts): item keys ("type|blendId|customId")
 * in the shared UI settings, "fxFavorites" in the order they were starred and "fxRecents" most
 * recent first, at most 12. Both UIs read and write the same lists, so an effect starred in
 * one is starred in the other.
 */

#include "uiclient/ClientState.h"

#include <cstddef>
#include <string>
#include <vector>

namespace guitarfx::uiclient
{
class UiCommands;

inline constexpr std::size_t kFxRecentsLimit = 12;

[[nodiscard]] std::string FxItemKey(const std::string& effectType, const std::string& blendId = {},
                                    const std::string& customEffectId = {});

[[nodiscard]] std::vector<std::string> FxFavoriteKeys(const ClientState& state);
/// Most recent first.
[[nodiscard]] std::vector<std::string> FxRecentKeys(const ClientState& state);
[[nodiscard]] bool IsFxFavorite(const ClientState& state, const std::string& key);

/// Stars or unstars an item; a new favourite goes last. Returns whether it is now a favourite.
bool ToggleFxFavorite(UiCommands& commands, const ClientState& state, const std::string& key);
/// An item put in the chain: it goes first, and the list keeps kFxRecentsLimit.
void RecordFxRecent(UiCommands& commands, const ClientState& state, const std::string& key);
} // namespace guitarfx::uiclient

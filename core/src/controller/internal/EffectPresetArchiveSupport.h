#pragma once

/**
 * EffectPresetArchiveSupport.h — Effect presets carried by a preset archive.
 *
 * An archive's manifest may hold an `effectPresets` section in the shape the user's own
 * effect preset store has: effect type -> [{id, name, parameters, resources, config}]. A
 * factory archive's become read-only factory presets for those effects, and since the
 * models, IRs and blends they choose ship in the same archive, their references are moved
 * onto the archive-scoped ids those were registered under, as its presets' are.
 */

#include <string>
#include <unordered_map>
#include <vector>

#include <nlohmann/json.hpp>

namespace guitarfx::controller_detail
{
/// The well-formed part of an archive's `effectPresets` section, by effect type as the
/// archive names it. An entry needs an id and a name; it keeps its numeric parameters, its
/// string config and its resource references, without file paths or embedded ids, which
/// name files on the machine it was exported from.
[[nodiscard]] nlohmann::json ReadArchiveEffectPresets(const nlohmann::json& section);

/// A factory archive's effect presets as they are served: keyed by canonical effect type,
/// ids scoped by `archiveKey`, models and IRs moved through `resourceIdMap` and a blend
/// node's blendId through `blendIdMap`. An entry that names a resource or blend the archive
/// did not deliver is left out, rather than offered pointing at nothing, and its name is
/// added to `skipped`.
[[nodiscard]] nlohmann::json ScopeFactoryArchiveEffectPresets(
    const nlohmann::json& byEffectType, const std::string& archiveKey,
    const std::unordered_map<std::string, std::string>& resourceIdMap,
    const std::unordered_map<std::string, std::string>& blendIdMap, std::vector<std::string>& skipped);

/// Appends `from`'s entries to `into`'s, effect type by effect type.
void AppendEffectPresetsByType(nlohmann::json& into, const nlohmann::json& from);
} // namespace guitarfx::controller_detail

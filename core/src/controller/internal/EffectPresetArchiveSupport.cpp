#include "controller/internal/EffectPresetArchiveSupport.h"

#include "controller/internal/BlendSupport.h"
#include "controller/internal/PresetArchiveSupport.h"
#include "dsp/EffectRegistry.h"
#include "presets/PresetTypesJson.h"

#include <iterator>
#include <unordered_set>

namespace guitarfx::controller_detail
{
namespace
{
/// One entry as the archive should have written it, or null when it cannot be offered.
nlohmann::json ReadArchiveEffectPreset(const nlohmann::json& entry)
{
    if (!entry.is_object())
    {
        return nullptr;
    }

    const auto id = entry.find("id");
    const auto name = entry.find("name");

    if (id == entry.end() || !id->is_string() || id->get_ref<const std::string&>().empty() || name == entry.end() ||
        !name->is_string() || name->get_ref<const std::string&>().empty())
    {
        return nullptr;
    }

    nlohmann::json read;
    read["id"] = *id;
    read["name"] = *name;
    read["parameters"] = nlohmann::json::object();

    if (const auto parameters = entry.find("parameters"); parameters != entry.end() && parameters->is_object())
    {
        for (const auto& [key, value] : parameters->items())
        {
            if (value.is_number())
            {
                read["parameters"][key] = value;
            }
        }
    }

    // Kept only when present: an entry without them leaves the node's own in place.
    if (const auto resources = entry.find("resources"); resources != entry.end() && resources->is_array())
    {
        read["resources"] = nlohmann::json::array();

        for (const auto& resource : *resources)
        {
            if (!resource.is_object())
            {
                continue;
            }

            auto ref = DeserializeResourceRef(resource);
            ref.filePath.clear();
            ref.embeddedId.clear();
            read["resources"].push_back(SerializeResourceRef(ref));
        }
    }

    if (const auto config = entry.find("config"); config != entry.end() && config->is_object())
    {
        read["config"] = nlohmann::json::object();

        for (const auto& [key, value] : config->items())
        {
            if (value.is_string())
            {
                read["config"][key] = value;
            }
        }
    }

    return read;
}

/// Moves an entry's references onto the archive's scoped ids. False when one names something
/// the archive did not deliver.
bool ScopeEntryReferences(nlohmann::json& entry, const std::unordered_map<std::string, std::string>& resourceIdMap,
                          const std::unordered_map<std::string, std::string>& blendIdMap)
{
    if (entry.contains("resources"))
    {
        for (auto& resource : entry["resources"])
        {
            auto ref = DeserializeResourceRef(resource);
            const auto mapped = resourceIdMap.find(ref.resourceId);

            if (!ref.IsLibraryRef() || mapped == resourceIdMap.end())
            {
                return false;
            }

            ref.resourceId = mapped->second;
            resource = SerializeResourceRef(ref);
        }
    }

    if (entry.contains("config") && entry["config"].contains(kBlendIdConfigKey))
    {
        auto& blendId = entry["config"][kBlendIdConfigKey];
        const auto mapped = blendIdMap.find(blendId.get<std::string>());

        if (mapped == blendIdMap.end())
        {
            return false;
        }

        blendId = mapped->second;
    }

    return true;
}
} // namespace

nlohmann::json ReadArchiveEffectPresets(const nlohmann::json& section)
{
    nlohmann::json byEffectType = nlohmann::json::object();

    if (!section.is_object())
    {
        return byEffectType;
    }

    for (const auto& [effectType, entries] : section.items())
    {
        if (effectType.empty() || !entries.is_array())
        {
            continue;
        }

        nlohmann::json read = nlohmann::json::array();

        for (const auto& entry : entries)
        {
            if (auto preset = ReadArchiveEffectPreset(entry); !preset.is_null())
            {
                read.push_back(std::move(preset));
            }
        }

        if (!read.empty())
        {
            byEffectType[effectType] = std::move(read);
        }
    }

    return byEffectType;
}

nlohmann::json ScopeFactoryArchiveEffectPresets(const nlohmann::json& byEffectType, const std::string& archiveKey,
                                                const std::unordered_map<std::string, std::string>& resourceIdMap,
                                                const std::unordered_map<std::string, std::string>& blendIdMap,
                                                std::vector<std::string>& skipped)
{
    nlohmann::json scoped = nlohmann::json::object();
    const auto& registry = EffectRegistry::Instance();
    std::unordered_set<std::string> seenIds;

    for (const auto& [effectType, entries] : byEffectType.items())
    {
        // Under the key the UIs look effect presets up by, whichever alias the archive used.
        auto& target = scoped[registry.Resolve(effectType)];

        if (!target.is_array())
        {
            target = nlohmann::json::array();
        }

        for (auto entry : entries)
        {
            entry["id"] = BuildScopedFactoryArchiveId(archiveKey, entry["id"].get<std::string>());

            if (!seenIds.insert(entry["id"].get<std::string>()).second)
            {
                continue;
            }

            if (!ScopeEntryReferences(entry, resourceIdMap, blendIdMap))
            {
                skipped.push_back(entry["name"].get<std::string>());
                continue;
            }

            target.push_back(std::move(entry));
        }
    }

    for (auto it = scoped.begin(); it != scoped.end();)
    {
        it = it->empty() ? scoped.erase(it) : std::next(it);
    }

    return scoped;
}

void AppendEffectPresetsByType(nlohmann::json& into, const nlohmann::json& from)
{
    for (const auto& [effectType, entries] : from.items())
    {
        auto& target = into[effectType];

        if (!target.is_array())
        {
            target = nlohmann::json::array();
        }

        for (const auto& entry : entries)
        {
            target.push_back(entry);
        }
    }
}
} // namespace guitarfx::controller_detail

#include "uiclient/PresetInspect.h"

#include "dsp/FiniteCheck.h"
#include "presets/PresetStorage.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <set>
#include <string_view>

namespace guitarfx::uiclient
{
namespace
{
const std::set<std::string, std::less<>> kAllowedKeys = {"id",
                                                         "name",
                                                         "category",
                                                         "description",
                                                         "attachments",
                                                         "fxChain",
                                                         "audioFxModelId",
                                                         "irId",
                                                         "customModelPath",
                                                         "customIrPath",
                                                         "formatVersion",
                                                         "graph",
                                                         "scenes",
                                                         "globalSignalChain",
                                                         "embeddedResources",
                                                         "version",
                                                         "author",
                                                         "tags",
                                                         "createdAt",
                                                         "modifiedAt"};

constexpr std::array<std::string_view, 5> kOptionalStrings = {"category", "description", "customModelPath",
                                                              "customIrPath", "author"};
constexpr std::array<std::string_view, 5> kOptionalArrays = {"attachments", "fxChain", "embeddedResources", "scenes",
                                                             "tags"};

// The global EQ's defaults (DEFAULT_GLOBAL_SIGNAL_CHAIN's eq_parametric, and validate.ts's
// fallback, which agree).
constexpr std::array<std::pair<std::string_view, double>, 10> kEqDefaults = {{{"lowGain", 0.0},
                                                                              {"lowFreq", 100.0},
                                                                              {"lowMidGain", 0.0},
                                                                              {"lowMidFreq", 400.0},
                                                                              {"lowMidQ", 1.0},
                                                                              {"highMidGain", 0.0},
                                                                              {"highMidFreq", 2000.0},
                                                                              {"highMidQ", 1.0},
                                                                              {"highGain", 0.0},
                                                                              {"highFreq", 8000.0}}};

bool IsBlankString(const nlohmann::json& value)
{
    if (!value.is_string())
    {
        return true;
    }

    const auto& text = value.get_ref<const std::string&>();
    return text.find_first_not_of(" \t\r\n") == std::string::npos;
}

std::string StringOr(const nlohmann::json& object, const char* key)
{
    const auto it = object.find(key);
    return it != object.end() && it->is_string() ? it->get<std::string>() : std::string();
}

bool IsDefaultGlobalEq(const nlohmann::json& node)
{
    const auto id = StringOr(node, "id");
    const auto type = StringOr(node, "type");

    if (id != "global_eq" && StringOr(node, "label") != "Global EQ")
    {
        return false;
    }

    if (!type.empty() && type != "eq_parametric")
    {
        return false;
    }

    bool enabled = true;

    if (const auto it = node.find("enabled"); it != node.end() && it->is_boolean())
    {
        enabled = it->get<bool>();
    }
    else if (const auto bypassed = node.find("bypassed"); bypassed != node.end() && bypassed->is_boolean())
    {
        enabled = !bypassed->get<bool>();
    }

    if (enabled)
    {
        return false;
    }

    const auto params = node.find("params");

    if (params == node.end() || !params->is_object())
    {
        return false;
    }

    return std::all_of(kEqDefaults.begin(), kEqDefaults.end(), [&](const auto& entry) {
        const auto it = params->find(std::string(entry.first));
        return it != params->end() && it->is_number() && it->template get<double>() == entry.second;
    });
}

bool HasEdge(const nlohmann::json& edges, const std::string& from, const std::string& to)
{
    return std::any_of(edges.begin(), edges.end(),
                       [&](const auto& e) { return StringOr(e, "from") == from && StringOr(e, "to") == to; });
}

bool HasNode(const nlohmann::json& nodes, const std::string& id)
{
    return std::any_of(nodes.begin(), nodes.end(), [&](const auto& n) { return StringOr(n, "id") == id; });
}
} // namespace

nlohmann::json InspectablePresetJson(const Preset& preset)
{
    auto json = nlohmann::json::parse(PresetStorage::SerializeToJson(preset), nullptr, false);

    if (json.is_discarded())
    {
        return nlohmann::json::object();
    }

    json.erase("globalSignalChain");
    return json;
}

std::vector<std::string> ValidatePresetJson(const nlohmann::json& preset)
{
    if (!preset.is_object() || preset.empty())
    {
        return {"No preset loaded."};
    }

    std::vector<std::string> issues;
    std::vector<std::string> unknown;

    for (const auto& [key, value] : preset.items())
    {
        if (!kAllowedKeys.contains(key))
        {
            unknown.push_back(key);
        }
    }

    if (!unknown.empty())
    {
        std::sort(unknown.begin(), unknown.end());
        std::string line = "Unknown fields: ";

        for (std::size_t i = 0; i < unknown.size(); ++i)
        {
            line += (i > 0 ? ", " : "") + unknown[i];
        }

        issues.push_back(line);
    }

    if (IsBlankString(preset.value("id", nlohmann::json())))
    {
        issues.push_back("Missing or invalid preset id.");
    }

    if (IsBlankString(preset.value("name", nlohmann::json())))
    {
        issues.push_back("Missing or invalid preset name.");
    }

    if (const auto graph = preset.find("graph"); graph != preset.end() && graph->is_object())
    {
        const auto nodes = graph->find("nodes");
        const auto edges = graph->find("edges");

        if (nodes == graph->end() || !nodes->is_array())
        {
            issues.push_back("Preset graph.nodes must be an array.");
        }
        else
        {
            for (std::size_t i = 0; i < nodes->size(); ++i)
            {
                const auto& node = (*nodes)[i];
                const auto number = std::to_string(i + 1);

                if (!node.is_object() || IsBlankString(node.value("id", nlohmann::json())))
                {
                    issues.push_back("Graph node #" + number + " is missing a valid id.");
                }

                if (!node.is_object() || IsBlankString(node.value("type", nlohmann::json())))
                {
                    issues.push_back("Graph node #" + number + " is missing a valid type.");
                }
            }
        }

        if (edges == graph->end() || !edges->is_array())
        {
            issues.push_back("Preset graph.edges must be an array.");
        }
        else
        {
            for (std::size_t i = 0; i < edges->size(); ++i)
            {
                const auto& edge = (*edges)[i];

                if (!edge.is_object() || !edge.value("from", nlohmann::json()).is_string() ||
                    !edge.value("to", nlohmann::json()).is_string())
                {
                    issues.push_back("Graph edge #" + std::to_string(i + 1) + " is missing valid endpoints.");
                }
            }
        }
    }

    if (const auto scenes = preset.find("scenes"); scenes != preset.end() && !scenes->is_null() && !scenes->is_array())
    {
        issues.push_back("Preset scenes must be an array when present.");
    }

    return issues;
}

PresetCleanup CleanupPresetJson(const nlohmann::json& preset)
{
    PresetCleanup result;
    result.cleaned = preset.is_object() ? preset : nlohmann::json::object();
    auto& cleaned = result.cleaned;

    const auto removed = [&](const std::string& key) {
        if (std::find(result.removedKeys.begin(), result.removedKeys.end(), key) == result.removedKeys.end())
        {
            result.removedKeys.push_back(key);
        }
    };

    for (auto it = cleaned.begin(); it != cleaned.end();)
    {
        if (!kAllowedKeys.contains(it.key()))
        {
            removed(it.key());
            it = cleaned.erase(it);
        }
        else
        {
            ++it;
        }
    }

    for (const char* key : {"globals", "global", "globalSignalChain"})
    {
        if (cleaned.contains(key))
        {
            cleaned.erase(key);
            removed(key);
        }
    }

    for (const auto key : kOptionalStrings)
    {
        const std::string name(key);

        if (const auto it = cleaned.find(name); it != cleaned.end() && it->is_string() && IsBlankString(*it))
        {
            cleaned.erase(it);
            removed(name);
        }
    }

    for (const auto key : kOptionalArrays)
    {
        const std::string name(key);

        if (const auto it = cleaned.find(name); it != cleaned.end() && it->is_array() && it->empty())
        {
            cleaned.erase(it);
            removed(name);
        }
    }

    // Resource refs: the legacy id and type folded into resourceId and resourceType.
    if (auto graph = cleaned.find("graph"); graph != cleaned.end() && graph->is_object())
    {
        if (auto nodes = graph->find("nodes"); nodes != graph->end() && nodes->is_array())
        {
            for (auto& node : *nodes)
            {
                auto resources = node.find("resources");

                if (resources == node.end() || !resources->is_array())
                {
                    continue;
                }

                for (auto& ref : *resources)
                {
                    if (!ref.is_object())
                    {
                        continue;
                    }

                    const auto legacyId = StringOr(ref, "id");
                    const auto legacyType = StringOr(ref, "type");
                    const auto resourceId = StringOr(ref, "resourceId");
                    const auto resourceType = StringOr(ref, "resourceType");
                    const auto finalId = resourceId.empty() ? legacyId : resourceId;
                    const auto finalType = resourceType.empty() ? legacyType : resourceType;

                    if (!finalId.empty())
                    {
                        ref["resourceId"] = finalId;
                    }

                    if (!finalType.empty())
                    {
                        ref["resourceType"] = finalType;
                    }

                    if (!legacyId.empty() && legacyId == finalId)
                    {
                        ref.erase("id");
                        ++result.normalizedAliases;
                    }

                    if (!legacyType.empty() && legacyType == finalType)
                    {
                        ref.erase("type");
                        ++result.normalizedAliases;
                    }
                }
            }
        }

        // A default, disabled global EQ left in the preset's own graph: taken out, and what it
        // joined linked straight through.
        auto nodes = graph->find("nodes");
        auto edges = graph->find("edges");

        if (nodes != graph->end() && edges != graph->end() && nodes->is_array() && edges->is_array() &&
            !nodes->empty() && !edges->empty())
        {
            const auto eq = std::find_if(nodes->begin(), nodes->end(), IsDefaultGlobalEq);

            if (eq != nodes->end())
            {
                const auto eqId = StringOr(*eq, "id");
                const bool inputToEq = HasEdge(*edges, "__input__", eqId);
                const bool eqToDoubler = HasEdge(*edges, eqId, "global_doubler");
                const bool eqToOutput = HasEdge(*edges, eqId, "__output__");
                const bool hasDoubler = HasNode(*nodes, "global_doubler");
                const bool hasOutput = HasNode(*nodes, "__output__");

                nodes->erase(std::remove_if(nodes->begin(), nodes->end(),
                                            [&](const auto& n) { return StringOr(n, "id") == eqId; }),
                             nodes->end());
                edges->erase(std::remove_if(edges->begin(), edges->end(),
                                            [&](const auto& e) {
                                                return StringOr(e, "from") == eqId || StringOr(e, "to") == eqId;
                                            }),
                             edges->end());

                const auto link = [&](const char* to) {
                    if (!HasEdge(*edges, "__input__", to))
                    {
                        edges->push_back(
                            {{"from", "__input__"}, {"to", to}, {"fromPort", 0}, {"toPort", 0}, {"gain", 1}});
                    }
                };

                if (inputToEq && eqToDoubler && hasDoubler)
                {
                    link("global_doubler");
                }
                else if (inputToEq && eqToOutput && hasOutput)
                {
                    link("__output__");
                }

                result.removedGlobalEq = true;
            }
        }
    }

    for (const char* key : {"audioFxModelId", "irId", "customModelPath", "customIrPath"})
    {
        const auto it = cleaned.find(key);

        if (it == cleaned.end() || it->is_null() || (it->is_string() && it->get_ref<const std::string&>().empty()))
        {
            if (it != cleaned.end())
            {
                cleaned.erase(it);
            }

            removed(key);
        }
    }

    return result;
}

std::vector<std::string> CleanupReport(const PresetCleanup& cleanup)
{
    std::vector<std::string> lines;

    if (!cleanup.removedKeys.empty())
    {
        auto keys = cleanup.removedKeys;
        std::sort(keys.begin(), keys.end());
        std::string line = "Removed fields: ";

        for (std::size_t i = 0; i < keys.size(); ++i)
        {
            line += (i > 0 ? ", " : "") + keys[i];
        }

        lines.push_back(line);
    }

    if (cleanup.normalizedAliases > 0)
    {
        lines.push_back("Normalized resource ref aliases: " + std::to_string(cleanup.normalizedAliases));
    }

    if (cleanup.removedGlobalEq)
    {
        lines.push_back("Removed default global EQ node.");
    }

    if (lines.empty())
    {
        lines.push_back("No unused fields removed.");
    }

    return lines;
}

void PeakHold::Push(double peakDb, double nowSeconds)
{
    if (!guitarfx::IsFinite(peakDb))
    {
        return;
    }

    if (!mHas || nowSeconds - mWindowStart > kWindowSeconds)
    {
        mPeak = peakDb;
        mWindowStart = nowSeconds;
        mHas = true;
    }
    else if (peakDb >= mPeak)
    {
        mPeak = peakDb;
    }
}

std::optional<double> PeakHold::Peak() const
{
    return mHas ? std::optional<double>(mPeak) : std::nullopt;
}

std::string FormatPeakDb(std::optional<double> peakDb)
{
    if (!peakDb || !guitarfx::IsFinite(*peakDb))
    {
        return "\xE2\x80\x94";
    }

    char text[32];
    std::snprintf(text, sizeof(text), "%.1f dBFS", *peakDb);
    return text;
}
} // namespace guitarfx::uiclient

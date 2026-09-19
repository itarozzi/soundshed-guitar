/**
 * EffectAliasParityTests.cpp — the engine's effect aliases against the shared list.
 *
 * core/protocol/effect-aliases.json lists every registered effect type with the legacy
 * ids that resolve to it. The UI's EFFECT_ALIAS_MAP is held to the same list by
 * core/ui/tests/effectAliases.test.ts, so a preset resolves to the same effect whether
 * the engine loads it (a DAW restore, a program change) or the UI does. Aliases are read
 * from the registry after registration, not from the source, because some are built at
 * run time (the reverb modes take theirs from ReverbEffect::ModeType).
 */

#include "dsp/EffectRegistry.h"
#include "dsp/effects/BuiltinEffects.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <string>

#include <nlohmann/json.hpp>

namespace fs = std::filesystem;
using guitarfx::EffectRegistry;

namespace
{
int gFailures = 0;

void Fail(const std::string& what)
{
    std::cerr << "FAIL: " << what << "\n";
    ++gFailures;
}

nlohmann::json LoadManifest()
{
    const fs::path path = fs::path(GUITARFX_TEST_RESOURCES_DIR) / ".." / ".." / "protocol" / "effect-aliases.json";
    std::ifstream input(path, std::ios::binary);

    if (!input)
    {
        Fail("cannot open " + path.string());
        return {};
    }

    return nlohmann::json::parse(input);
}

std::string Describe(const std::set<std::string>& ids)
{
    std::string text = "{";

    for (const auto& id : ids)
    {
        text += (text.size() > 1 ? ", " : "") + id;
    }

    return text + "}";
}

bool ExpectedInThisBuild(const nlohmann::json& entry)
{
    if (entry.value("registeredIn", "core") != "core")
    {
        return false;
    }

    if (entry.value("build", "") == "wasm")
    {
#if defined(GUITARFX_ENABLE_WASM_EFFECTS)
        return true;
#else
        return false;
#endif
    }

    return true;
}

void TestRegistryMatchesManifest(const nlohmann::json& manifest)
{
    auto& registry = EffectRegistry::Instance();
    const auto& effects = manifest.at("effects");
    std::set<std::string> listedGuids;

    for (const auto& [name, entry] : effects.items())
    {
        const std::string guid = entry.at("guid").get<std::string>();
        listedGuids.insert(guid);

        std::set<std::string> expected;

        for (const auto& alias : entry.at("aliases"))
        {
            expected.insert(alias.get<std::string>());
        }

        // A retired type id runs as this effect, so the engine lists it among the aliases.
        const auto retiredTypes = entry.value("retiredTypes", nlohmann::json::object());

        for (const auto& retired : retiredTypes.items())
        {
            expected.insert(retired.value().get<std::string>());
        }

        const auto info = registry.GetTypeInfo(guid);

        if (!ExpectedInThisBuild(entry))
        {
            if (entry.value("registeredIn", "core") != "core" && info.has_value())
            {
                Fail(name + " is listed as registered outside the core, but the core registers it");
            }

            continue;
        }

        if (!info.has_value() || info->type != guid)
        {
            Fail(name + " (" + guid + ") is listed but not registered");
            continue;
        }

        const std::set<std::string> actual(info->aliases.begin(), info->aliases.end());

        if (actual != expected)
        {
            Fail(name + " aliases: engine " + Describe(actual) + ", list " + Describe(expected));
        }

        for (const auto& alias : expected)
        {
            if (registry.Resolve(alias) != guid)
            {
                Fail(alias + " resolves to " + registry.Resolve(alias) + ", not " + name);
            }
        }
    }

    for (const auto& info : registry.GetAllTypes())
    {
        if (listedGuids.count(info.type) == 0)
        {
            Fail("registered type " + info.type + " (" + info.displayName + ") is missing from effect-aliases.json");
        }
    }
}
} // namespace

int main()
{
    guitarfx::RegisterAllEffects();

    const auto manifest = LoadManifest();

    if (!manifest.is_null())
    {
        TestRegistryMatchesManifest(manifest);
    }

    if (gFailures > 0)
    {
        std::cerr << gFailures << " check(s) failed\n";
        return 1;
    }

    std::cout << "EffectAliasParityTests passed\n";
    return 0;
}

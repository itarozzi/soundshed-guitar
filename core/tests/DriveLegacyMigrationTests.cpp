/**
 * @file DriveLegacyMigrationTests.cpp
 * @brief Presets saved before the drive pedals' Model switch keep their loudness.
 *
 * The old Overdrive, Distortion and Fuzz (helpers/LegacyDrivePedals.h, kept verbatim) came out
 * 13-25 dB above bypass; the new ones match bypass. DriveLegacyMigration.h gives an old node
 * the first model and the Level that keeps it as loud. These check:
 *   - the rules: which nodes migrate, the defaults for missing keys, clamping, idempotence
 *   - that every way a stored node is read (presets, composites, the global chain) migrates it
 *   - the factory composite and the factory preset pack's settings come out as loud as before
 *   - off the table's grid points, interpolation still lands within a dB
 *
 * `DriveLegacyMigrationTests --calibrate` re-derives the tables, for when a new pedal's voicing
 * changes: it prints each table's initialiser, found by bisection on the new Level.
 */

#include <algorithm>
#include <cmath>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <random>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "DriveTestSupport.h"
#include "LegacyDrivePedals.h"
#include "dsp/EffectGuids.h"
#include "dsp/effects/DriveLegacyMigration.h"
#include "presets/PresetStorage.h"
#include "presets/PresetTypes.h"
#include "presets/PresetTypesJson.h"

namespace
{
using namespace drive_test;
namespace legacy = guitarfx::drive_legacy;

struct Family
{
    const char* name;
    const char* type;
    std::size_t pedalIndex; ///< into drive_test::Pedals()
    std::function<std::unique_ptr<guitarfx::EffectProcessor>()> makeOld;
};

const std::vector<Family>& Families()
{
    static const std::vector<Family> families = {
        {"Overdrive", guitarfx::EffectGuids::kOverdrive, 0,
         [] { return std::make_unique<legacy_drive::LegacyOverdriveEffect>(); }},
        {"Distortion", guitarfx::EffectGuids::kDistortion, 1,
         [] { return std::make_unique<legacy_drive::LegacyDistortionEffect>(); }},
        {"Fuzz", guitarfx::EffectGuids::kFuzz, 2, [] { return std::make_unique<legacy_drive::LegacyFuzzEffect>(); }},
    };
    return families;
}

using ParamMap = std::map<std::string, double>;

/// The old pedal with a stored node's params, fed the guitar phrase.
double OldLoudness(const Family& family, const ParamMap& params, const std::vector<float>& phrase)
{
    auto effect = family.makeOld();

    for (const auto& [key, value] : params)
    {
        effect->SetParam(key, value);
    }

    effect->Prepare(kSampleRate, kBlockSize);
    return KWeightedDb(Render(*effect, phrase));
}

/// The new pedal with (migrated) params.
double NewLoudness(const Family& family, const ParamMap& params, const std::vector<float>& phrase)
{
    Params list(params.begin(), params.end());
    auto effect = Make(Pedals()[family.pedalIndex], static_cast<std::size_t>(params.at("model")), list);
    return KWeightedDb(Render(*effect, phrase));
}

void TestRules()
{
    std::cout << "\nMigration rules" << std::endl;

    ParamMap od = {{"drive", 0.5}, {"tone", 0.5}, {"level", 0.0}, {"mix", 1.0}};
    const bool migrated = legacy::MigrateParams(guitarfx::EffectGuids::kOverdrive, od);
    const double expected = legacy::MatchedLevelDb(legacy::kOverdriveLevels, 0.5, 0.5, 0.0);
    Check(migrated && od.at("model") == 0.0 && od.at("level") == expected && od.at("mix") == 1.0,
          "an old Overdrive node gets the TS-808 and a matching Level", "Level " + Num(od.at("level"), 1) + " dB");

    const ParamMap once = od;
    Check(!legacy::MigrateParams(guitarfx::EffectGuids::kOverdrive, od) && od == once,
          "a migrated node is left alone the next time it is read");

    ParamMap current = {{"model", 2.0}, {"drive", 0.5}, {"level", 0.0}};
    const ParamMap currentBefore = current;
    Check(!legacy::MigrateParams(guitarfx::EffectGuids::kFuzz, current) && current == currentBefore,
          "a node saved with a model is not touched");

    ParamMap gain = {{"level", 0.0}};
    Check(!legacy::MigrateParams(guitarfx::EffectGuids::kGain, gain) && gain.size() == 1, "nor is any other effect");

    ParamMap empty;
    Check(legacy::MigrateParams("fuzz", empty) && empty.at("drive") == 0.7 && empty.at("tone") == 0.5 &&
              empty.at("level") == legacy::MatchedLevelDb(legacy::kFuzzLevels, 0.7, 0.5, 0.0),
          "the legacy alias migrates, and a node that stored nothing gets the old defaults");

    ParamMap hot = {{"drive", 1.0}, {"tone", 0.5}, {"level", 30.0}};
    ParamMap clamped = {{"drive", 1.0}, {"tone", 0.5}, {"level", 12.0}};
    legacy::MigrateParams("distortion", hot);
    legacy::MigrateParams("distortion", clamped);
    Check(hot.at("level") == clamped.at("level") && hot.at("level") <= legacy::kNewLevelMaxDb,
          "an out-of-range old Level counts as the old pedal's clamp, and the result is in range");
}

nlohmann::json LegacyNodeJson(const std::string& id, const std::string& type)
{
    return {{"id", id}, {"type", type}, {"params", {{"drive", 0.5}, {"tone", 0.5}, {"level", 0.0}, {"mix", 1.0}}}};
}

void TestReadPaths()
{
    std::cout << "\nEvery read path migrates" << std::endl;
    const double expected = legacy::MatchedLevelDb(legacy::kOverdriveLevels, 0.5, 0.5, 0.0);

    nlohmann::json current = LegacyNodeJson("od2", "overdrive");
    current["params"]["model"] = 3.0;
    const nlohmann::json presetJson = {
        {"id", "legacy-preset"},
        {"name", "Legacy"},
        {"graph", {{"nodes", {LegacyNodeJson("od", "overdrive"), current}}, {"edges", nlohmann::json::array()}}}};
    const auto preset = guitarfx::PresetStorage::DeserializeFromJson(presetJson.dump());
    bool presetMigrated = false;
    bool currentUntouched = false;

    if (preset)
    {
        for (const auto* graph : {&preset->graph})
        {
            for (const auto& node : graph->nodes)
            {
                presetMigrated = presetMigrated || (node.id == "od" && node.params.count("model") != 0 &&
                                                    node.params.at("level") == expected);
                currentUntouched = currentUntouched || (node.id == "od2" && node.params.at("model") == 3.0 &&
                                                        node.params.at("level") == 0.0);
            }
        }

        for (const auto& scene : preset->scenes)
        {
            for (const auto& node : scene.graph.nodes)
            {
                presetMigrated = presetMigrated || (node.id == "od" && node.params.count("model") != 0 &&
                                                    node.params.at("level") == expected);
            }
        }
    }

    Check(preset.has_value() && presetMigrated, "a preset's old drive node is migrated as it loads");
    Check(currentUntouched, "and a node saved since keeps its values");

    const auto graph = guitarfx::DeserializeSignalGraph(
        {{"nodes", {LegacyNodeJson("pre", guitarfx::EffectGuids::kOverdrive)}}, {"edges", nlohmann::json::array()}});
    Check(!graph.nodes.empty() && graph.nodes[0].params.count("model") != 0 &&
              graph.nodes[0].params.at("level") == expected,
          "so is one in a composite's inner graph or the global chain");

    // A migrated node written out and read back is not migrated again.
    const nlohmann::json written = guitarfx::SerializeNode(graph.nodes[0]);
    const auto reread = guitarfx::DeserializeNode(written);
    Check(reread.params == graph.nodes[0].params, "and saving and reloading it changes nothing");
}

void TestFactoryContent()
{
    std::cout << "\nFactory content keeps its loudness" << std::endl;
    const auto phrase = GuitarPhrase();

    // The factory "Supercharged Neural Amp" composite's preamp, and the drive settings in the
    // factory preset pack (Preset-Pack-Vol.-1): Gravity Storm, Heavy American, Space, Stranger Thing.
    const std::vector<std::pair<std::size_t, ParamMap>> settings = {
        {0, {{"drive", 0.35}, {"tone", 0.5}, {"level", 0.0}, {"mix", 1.0}}},
        {0, {{"drive", 0.5}, {"tone", 0.5}, {"level", -0.24}, {"mix", 1.0}}},
        {1, {{"drive", 1.0}, {"tone", 0.03}, {"level", 3.72}, {"mix", 0.55}}},
        {2, {{"drive", 0.145}, {"tone", 0.5}, {"level", 0.0}, {"mix", 1.0}}},
        {1, {{"drive", 0.6}, {"tone", 0.3307}, {"level", -5.21}, {"mix", 1.0}}},
        {0, {{"drive", 0.115}, {"tone", 0.095}, {"level", -5.76}, {"mix", 1.0}}},
        {1, {{"drive", 0.54}, {"tone", 0.035}, {"level", 0.6}, {"mix", 1.0}}},
        {1, {{"drive", 0.54}, {"tone", 0.0}, {"level", -6.84}, {"mix", 1.0}}},
    };

    for (const auto& [familyIndex, stored] : settings)
    {
        const Family& family = Families()[familyIndex];
        ParamMap migrated = stored;
        legacy::MigrateParams(family.type, migrated);
        const double before = OldLoudness(family, stored, phrase);
        const double after = NewLoudness(family, migrated, phrase);
        Check(std::fabs(after - before) < 1.0,
              std::string(family.name) + " at drive " + Num(stored.at("drive"), 2) + ", tone " +
                  Num(stored.at("tone"), 2) + ", level " + Num(stored.at("level"), 1) + ", mix " +
                  Num(stored.at("mix"), 2) + " is as loud as it was",
              Num(after - before, 2) + " dB, Level " + Num(stored.at("level"), 1) + " -> " +
                  Num(migrated.at("level"), 1));
    }
}

void TestBetweenGridPoints()
{
    std::cout << "\nBetween the table's points" << std::endl;
    const auto phrase = GuitarPhrase();
    std::mt19937 rng(11);
    std::uniform_real_distribution<double> unit(0.0, 1.0);

    for (const auto& family : Families())
    {
        // Within 1 dB, or 2 dB where the match needed the whole +24 dB: there the old pedal sat
        // at full scale in its output limiter, which the new one reaches but cannot pass.
        double worst = 0.0;
        double worstAtCeiling = 0.0;
        std::string worstAt;

        for (int trial = 0; trial < 10; ++trial)
        {
            const ParamMap stored = {{"drive", unit(rng)}, {"tone", unit(rng)}, {"level", -12.0 + 24.0 * unit(rng)}};
            ParamMap migrated = stored;
            legacy::MigrateParams(family.type, migrated);
            const double error = std::fabs(NewLoudness(family, migrated, phrase) - OldLoudness(family, stored, phrase));
            const bool atCeiling = migrated.at("level") >= legacy::kNewLevelMaxDb - 0.5;
            double& bucket = atCeiling ? worstAtCeiling : worst;

            if (error > bucket && !atCeiling)
            {
                worstAt = " (drive " + Num(stored.at("drive"), 2) + ", tone " + Num(stored.at("tone"), 2) + ", level " +
                          Num(stored.at("level"), 1) + ")";
            }

            bucket = std::max(bucket, error);
        }

        Check(worst < 1.0 && worstAtCeiling < 2.0,
              std::string(family.name) + ": ten random old settings keep their loudness",
              "worst " + Num(worst, 2) + " dB" + worstAt + ", " + Num(worstAtCeiling, 2) + " dB at the +24 dB ceiling");
    }
}

/// Prints each table's initialiser: for every grid point, the new Level (bisected to 0.01 dB)
/// at which the new pedal's first model matches the old pedal's loudness.
void PrintTables()
{
    const auto phrase = GuitarPhrase();

    for (const auto& family : Families())
    {
        std::cout << "// " << family.name << std::endl;

        for (std::size_t d = 0; d < legacy::kDriveSteps; ++d)
        {
            std::cout << "    {{";

            for (std::size_t t = 0; t < legacy::kToneSteps; ++t)
            {
                std::cout << (t ? "}, {" : "{");

                for (std::size_t l = 0; l < legacy::kLevelSteps; ++l)
                {
                    const double drive = legacy::DriveAtRow(d);
                    const double tone = static_cast<double>(t) / (legacy::kToneSteps - 1);
                    const double level = legacy::kOldLevelMinDb + (legacy::kOldLevelMaxDb - legacy::kOldLevelMinDb) *
                                                                      static_cast<double>(l) /
                                                                      (legacy::kLevelSteps - 1);
                    const double target =
                        OldLoudness(family, {{"drive", drive}, {"tone", tone}, {"level", level}}, phrase);
                    const auto loudnessAt = [&](double newLevel) {
                        return NewLoudness(
                            family, {{"model", 0.0}, {"drive", drive}, {"tone", tone}, {"level", newLevel}}, phrase);
                    };
                    double low = legacy::kNewLevelMinDb;
                    double high = legacy::kNewLevelMaxDb;

                    if (loudnessAt(high) < target)
                    {
                        low = high;
                    }
                    else if (loudnessAt(low) > target)
                    {
                        high = low;
                    }
                    else
                    {
                        for (int step = 0; step < 12; ++step)
                        {
                            const double mid = 0.5 * (low + high);
                            (loudnessAt(mid) < target ? low : high) = mid;
                        }
                    }

                    std::cout << (l ? ", " : "") << Num(0.5 * (low + high), 1);
                }
            }

            std::cout << "}}}, // drive " << Num(legacy::DriveAtRow(d), 3) << std::endl;
        }
    }
}
} // namespace

int main(int argc, char** argv)
{
    std::cout << "=== DriveLegacyMigrationTests ===" << std::endl;
    guitarfx::RegisterOverdriveEffect();
    guitarfx::RegisterDistortionEffect();
    guitarfx::RegisterFuzzEffect();

    if (argc > 1 && std::string(argv[1]) == "--calibrate")
    {
        PrintTables();
        return 0;
    }

    TestRules();
    TestReadPaths();
    TestFactoryContent();
    TestBetweenGridPoints();

    std::cout << "\n" << (gChecks - gFailures) << "/" << gChecks << " checks passed" << std::endl;
    return gFailures == 0 ? 0 : 1;
}

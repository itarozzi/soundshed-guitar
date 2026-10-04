/**
 * @file BuiltinAmpLegacyMigrationTests.cpp
 * @brief Presets saved before the Heavy American was rebuilt keep their loudness.
 *
 * The 1.5.0 amp (helpers/LegacyBuiltinAmp.h, kept verbatim) had no level makeup and came out
 * 12-18 dB above a guitar at the nominal level; the new one holds that level at Output 0 dB.
 * BuiltinAmpLegacyMigration.h gives an old node the Output that keeps it as loud. These check:
 *   - the rules: which nodes migrate, the old defaults for missing keys, the legacy stage-gain
 *     keys, clamping, idempotence
 *   - that every way a stored node is read (presets, composites, the global chain) migrates it
 *   - the factory preset pack's amps come out as loud as they were
 *   - random old settings, off the table's points, stay close
 *
 * `BuiltinAmpLegacyMigrationTests --calibrate` re-derives the tables and the old power stage's
 * fit, for when the new amp's voicing or makeup changes, and prints them ready to paste.
 */

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <iostream>
#include <map>
#include <random>
#include <sstream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "BuiltinAmpTestSupport.h"
#include "LegacyBuiltinAmp.h"
#include "dsp/EffectGuids.h"
#include "dsp/effects/BuiltinAmpEffect.h"
#include "dsp/effects/BuiltinAmpLegacyMigration.h"
#include "presets/PresetStorage.h"
#include "presets/PresetTypes.h"
#include "presets/PresetTypesJson.h"

namespace
{
namespace legacy = guitarfx::amp_legacy;
using ParamMap = std::map<std::string, double>;

int gFailures = 0;
int gChecks = 0;

void Check(bool condition, const std::string& what, const std::string& detail = "")
{
    ++gChecks;
    gFailures += condition ? 0 : 1;
    std::cout << (condition ? "  [PASS] " : "  [FAIL] ") << what << (detail.empty() ? "" : "  (" + detail + ")")
              << std::endl;
}

std::string Num(double value, int precision = 2)
{
    std::ostringstream text;
    text << std::fixed << std::setprecision(precision) << value;
    return text.str();
}

/// How much louder the old amp, with a stored node's params, makes `guitar`, as heard.
double OldHeardDb(const ParamMap& params, const std::vector<float>& guitar)
{
    legacy_amp::LegacyBuiltinAmpEffect amp;

    for (const auto& [key, value] : params)
    {
        amp.SetParam(key, value);
    }

    amp.Prepare(48000.0, 256);
    return builtin_amp_test::HeardGainDb(guitar, builtin_amp_test::RenderGuitar(amp, guitar));
}

/// The same for the new amp, with (migrated) params.
double NewHeardDb(const ParamMap& params, const std::vector<float>& guitar)
{
    guitarfx::BuiltinAmpEffect amp;

    for (const auto& [key, value] : params)
    {
        amp.SetParam(key, value);
    }

    amp.Prepare(48000.0, 256);
    return builtin_amp_test::HeardGainDb(guitar, builtin_amp_test::RenderGuitar(amp, guitar));
}

void TestRules()
{
    std::cout << "\nMigration rules" << std::endl;

    ParamMap amp = {{"voice", 1.0}, {"gain", 0.7}, {"stageCount", 3.0}, {"output", -12.0}};
    const bool migrated = legacy::MigrateParams(guitarfx::EffectGuids::kAmpBuiltin, amp);
    const double expected = -12.0 + legacy::OutputChangeDb({1.0, 0.7, 3});
    Check(migrated && amp.at("character") == legacy::kMigratedCharacter && amp.at("output") == expected &&
              amp.at("gain") == 0.7,
          "an old amp node gets Character's default and the Output that matches it",
          "Output -12.0 -> " + Num(amp.at("output"), 1) + " dB");

    const ParamMap once = amp;
    Check(!legacy::MigrateParams(guitarfx::EffectGuids::kAmpBuiltin, amp) && amp == once,
          "a migrated node is left alone the next time it is read");

    ParamMap current = {{"character", 0.3}, {"gain", 0.7}, {"output", -12.0}};
    const ParamMap currentBefore = current;
    Check(!legacy::MigrateParams(guitarfx::EffectGuids::kAmpBuiltin, current) && current == currentBefore,
          "a node saved with a Character is not touched");

    ParamMap drive = {{"drive", 0.5}, {"level", 0.0}};
    Check(!legacy::MigrateParams(guitarfx::EffectGuids::kOverdrive, drive) && drive.size() == 2,
          "nor is any other effect");

    ParamMap empty;
    Check(legacy::MigrateParams("amp_builtin", empty) && empty.at("output") == legacy::OutputChangeDb({}),
          "the legacy alias migrates, and a node that stored nothing gets the old defaults");

    // The old presets carry stage1Gain..stage6Gain beside stageGain; applied in the map's order,
    // the last one present is the Input Trim the node played at.
    ParamMap staged = {{"stage1Gain", 12.0}, {"stage6Gain", -6.0}};
    ParamMap withTrim = {{"stage1Gain", 12.0}, {"stage6Gain", -6.0}, {"stageGain", 3.0}};
    Check(legacy::StoredTrimDb(staged) == -6.0 && legacy::StoredTrimDb(withTrim) == 3.0,
          "the Input Trim is the last stage-gain key in the map's order, as both amps apply them");

    ParamMap hot = {{"gain", 1.0}, {"voice", 1.0}, {"output", 24.0}, {"powerDrive", 1.0}};
    ParamMap beyond = {{"gain", 1.0}, {"voice", 1.0}, {"output", 40.0}, {"powerDrive", 1.0}};
    legacy::MigrateParams(guitarfx::EffectGuids::kAmpBuiltin, hot);
    legacy::MigrateParams(guitarfx::EffectGuids::kAmpBuiltin, beyond);
    Check(hot.at("output") == legacy::kOutputMaxDb && beyond.at("output") == hot.at("output"),
          "an out-of-range old Output counts as the old clamp, and the result stays in range");
}

nlohmann::json OldAmpJson(const std::string& id)
{
    return {{"id", id},
            {"type", guitarfx::EffectGuids::kAmpBuiltin},
            {"params", {{"voice", 1.0}, {"gain", 0.6}, {"stageCount", 2.0}, {"output", -15.0}}}};
}

void TestReadPaths()
{
    std::cout << "\nEvery read path migrates" << std::endl;
    ParamMap reference = {{"voice", 1.0}, {"gain", 0.6}, {"stageCount", 2.0}, {"output", -15.0}};
    legacy::MigrateParams(guitarfx::EffectGuids::kAmpBuiltin, reference);
    const double expected = reference.at("output");

    nlohmann::json current = OldAmpJson("amp2");
    current["params"]["character"] = 0.5;
    const nlohmann::json presetJson = {
        {"id", "legacy-amp-preset"},
        {"name", "Legacy amp"},
        {"graph", {{"nodes", {OldAmpJson("amp"), current}}, {"edges", nlohmann::json::array()}}}};
    const auto preset = guitarfx::PresetStorage::DeserializeFromJson(presetJson.dump());
    bool presetMigrated = false;
    bool currentUntouched = false;

    if (preset)
    {
        std::vector<const guitarfx::SignalGraph*> graphs = {&preset->graph};

        for (const auto& scene : preset->scenes)
        {
            graphs.push_back(&scene.graph);
        }

        for (const auto* graph : graphs)
        {
            for (const auto& node : graph->nodes)
            {
                presetMigrated = presetMigrated || (node.id == "amp" && node.params.count("character") != 0 &&
                                                    node.params.at("output") == expected);
                currentUntouched = currentUntouched || (node.id == "amp2" && node.params.at("output") == -15.0);
            }
        }
    }

    Check(preset.has_value() && presetMigrated, "a preset's old amp node is migrated as it loads",
          "Output -15.0 -> " + Num(expected, 1) + " dB");
    Check(currentUntouched, "and a node saved since keeps its values");

    const auto graph =
        guitarfx::DeserializeSignalGraph({{"nodes", {OldAmpJson("pre")}}, {"edges", nlohmann::json::array()}});
    Check(!graph.nodes.empty() && graph.nodes[0].params.count("character") != 0 &&
              graph.nodes[0].params.at("output") == expected,
          "so is one in a composite's inner graph or the global chain");

    const nlohmann::json written = guitarfx::SerializeNode(graph.nodes[0]);
    const auto reread = guitarfx::DeserializeNode(written);
    Check(reread.params == graph.nodes[0].params, "and saving and reloading it changes nothing");
}

void TestFactoryContent(const std::vector<float>& guitar)
{
    std::cout << "\nThe factory pack's amps keep their loudness" << std::endl;

    // The Heavy American nodes of the factory preset pack (Preset-Pack-Vol.-1), as stored.
    const std::vector<std::pair<std::string, ParamMap>> settings = {
        {"Convoluted Acoustic Sim",
         {{"bass", 0.0},
          {"bias", 0.0},
          {"bright", 1.0},
          {"contour", 0.2},
          {"damping", 0.5},
          {"depth", 0.4},
          {"gain", 0.37857},
          {"middle", 0.71429},
          {"output", 0.0},
          {"powerDrive", 0.0},
          {"preEmphasis", 0.0},
          {"presence", 0.94571},
          {"resonance", 0.4},
          {"sag", 0.42857},
          {"stageCount", 2.0},
          {"stageGain", 0.0},
          {"treble", 0.37714},
          {"voice", 0.0}}},
        {"Heavy American",
         {{"bass", 0.64857},
          {"bias", 0.0},
          {"bright", 0.0},
          {"contour", 0.20286},
          {"damping", 0.125},
          {"depth", 0.39714},
          {"gain", 0.44429},
          {"middle", 0.12214},
          {"output", -24.0},
          {"powerDrive", 0.22},
          {"preEmphasis", 0.0},
          {"presence", 0.8},
          {"resonance", 0.3},
          {"sag", 0.085},
          {"stageCount", 1.0},
          {"stageGain", -2.88},
          {"treble", 0.78857},
          {"voice", 0.0}}},
        {"Space and Stranger Thing",
         {{"bass", 0.5},       {"bias", 0.53},      {"bright", 1.0},     {"contour", 0.605},  {"depth", 0.175},
          {"gain", 0.275},     {"middle", 0.0},     {"output", -15.6},   {"powerDrive", 0.2}, {"preEmphasis", 0.65},
          {"presence", 0.64},  {"resonance", 0.0},  {"stage1Gain", 0.0}, {"stage2Gain", 0.0}, {"stage3Gain", 0.0},
          {"stage4Gain", 0.0}, {"stage5Gain", 0.0}, {"stage6Gain", 0.0}, {"stageCount", 3.0}, {"stageGain", -2.16},
          {"treble", 0.5},     {"voice", 1.0}}},
    };

    for (const auto& [name, stored] : settings)
    {
        ParamMap migrated = stored;
        legacy::MigrateParams(guitarfx::EffectGuids::kAmpBuiltin, migrated);
        const double before = OldHeardDb(stored, guitar);
        const double after = NewHeardDb(migrated, guitar);
        Check(std::fabs(after - before) < 1.0, name + " is as loud as it was",
              Num(after - before) + " dB, Output " + Num(stored.at("output"), 1) + " -> " +
                  Num(migrated.at("output"), 1));
    }
}

void TestBetweenGridPoints(const std::vector<float>& guitar)
{
    std::cout << "\nOld settings off the table's points" << std::endl;
    std::mt19937 rng(23);
    std::uniform_real_distribution<double> unit(0.0, 1.0);
    double worst = 0.0, squares = 0.0;
    std::string worstAt;
    constexpr int kTrials = 12;

    for (int trial = 0; trial < kTrials; ++trial)
    {
        // The tone stack within a quarter of centre either way, as presets mostly sit.
        const ParamMap stored = {{"voice", trial % 2 == 0 ? 0.0 : 1.0},
                                 {"gain", unit(rng)},
                                 {"stageCount", static_cast<double>(1 + trial % 4)},
                                 {"stageGain", -12.0 + 24.0 * unit(rng)},
                                 {"powerDrive", unit(rng)},
                                 {"sag", unit(rng)},
                                 {"bass", 0.25 + 0.5 * unit(rng)},
                                 {"middle", 0.25 + 0.5 * unit(rng)},
                                 {"treble", 0.25 + 0.5 * unit(rng)},
                                 {"contour", 0.5 * unit(rng)},
                                 // At Output 0 the old amp already ran near full scale, so presets
                                 // turned it down; one turned up can need more than the new +24 dB.
                                 {"output", -24.0 * unit(rng)}};
        ParamMap migrated = stored;
        legacy::MigrateParams(guitarfx::EffectGuids::kAmpBuiltin, migrated);
        const double error = NewHeardDb(migrated, guitar) - OldHeardDb(stored, guitar);
        squares += error * error;

        if (std::fabs(error) > std::fabs(worst))
        {
            worst = error;
            worstAt = " (voice " + Num(stored.at("voice"), 0) + ", " + Num(stored.at("stageCount"), 0) +
                      " stages, gain " + Num(stored.at("gain")) + ", trim " + Num(stored.at("stageGain"), 1) +
                      " dB, Power Drive " + Num(stored.at("powerDrive")) + ", Sag " + Num(stored.at("sag")) + ")";
        }
    }

    const double rms = std::sqrt(squares / kTrials);
    Check(rms < 1.0 && std::fabs(worst) < 2.0, "random old settings keep their loudness",
          Num(rms) + " dB RMS, worst " + Num(worst) + " dB" + worstAt);
}

/// Prints one table's initialiser: a row per Gain step, Input Trim -24 to +24 dB across.
void PrintGrid(const char* name, const std::vector<double>& cells)
{
    std::cout << "inline constexpr LevelGrid " << name << " = {\n" << std::fixed;
    std::size_t cell = 0;

    for (int voice = 0; voice < 2; ++voice)
    {
        std::cout << "    { // " << (voice == 0 ? "Clean" : "Drive") << "\n";

        for (int stages = 1; stages <= legacy::kStageCounts; ++stages)
        {
            std::cout << "        { // " << stages << (stages == 1 ? " stage" : " stages") << "\n";

            for (std::size_t row = 0; row < legacy::kGainSteps; ++row)
            {
                std::cout << "            {";

                for (std::size_t column = 0; column < legacy::kTrimSteps; ++column)
                {
                    const double level = cells[cell++];
                    std::cout << (column ? ", " : "") << std::setprecision(2)
                              << (std::fabs(level) < 0.005 ? 0.0 : level) << "f";
                }

                std::cout << "}" << (row + 1 < legacy::kGainSteps ? "," : "") << " // Gain " << std::setprecision(3)
                          << legacy::GainAtRow(row) << "\n";
            }

            std::cout << "        }" << (stages < legacy::kStageCounts ? "," : "") << "\n";
        }

        std::cout << "    }" << (voice == 0 ? "," : "") << "\n";
    }

    std::cout << "};\n";
}

/// Measures the old amp over the table's grid on the measuring guitar (the new one untrimmed, for the tone
/// shares), and fits the old power stage.
void Calibrate()
{
    const auto guitar = builtin_amp_test::MeasuringGuitar();
    const auto both = [&](const ParamMap& params, bool old) {
        double sum = 0.0;

        for (const auto& take : guitar)
        {
            sum += old ? OldHeardDb(params, take) : NewHeardDb(params, take);
        }

        return sum / static_cast<double>(guitar.size());
    };

    constexpr int kCells = 2 * legacy::kStageCounts * static_cast<int>(legacy::kGainSteps * legacy::kTrimSteps);
    std::vector<double> oldCells(kCells), newCells(kCells);
    builtin_amp_test::ParallelFor(kCells, [&](int cell) {
        const auto perStages = static_cast<int>(legacy::kGainSteps * legacy::kTrimSteps);
        const int voice = cell / (legacy::kStageCounts * perStages);
        const int stages = cell / perStages % legacy::kStageCounts + 1;
        const auto row = static_cast<std::size_t>(cell % perStages) / legacy::kTrimSteps;
        const auto column = static_cast<std::size_t>(cell % perStages) % legacy::kTrimSteps;
        const ParamMap params = {{"voice", static_cast<double>(voice)},
                                 {"stageCount", static_cast<double>(stages)},
                                 {"gain", legacy::GainAtRow(row)},
                                 {"stageGain", legacy::kTrimMinDb + 6.0 * static_cast<double>(column)},
                                 {"character", legacy::kMigratedCharacter}};
        oldCells[cell] = both(params, true);
        // The new amp is only needed untrimmed, for the tone shares; its trim table is its own.
        newCells[cell] = column == legacy::kTrimSteps / 2 ? both(params, false) : 0.0;
    });

    std::cout << "// clang-format off\n// *INDENT-OFF*\n";
    PrintGrid("kOldLevelDb", oldCells);
    std::cout << "// *INDENT-ON*\n// clang-format on\n";

    // The tone stack's share, against the Input Trim 0 column of the grids just measured.
    constexpr int kRows = 2 * legacy::kStageCounts * static_cast<int>(legacy::kGainSteps);
    std::vector<double> cutShares(kRows), boostShares(kRows);
    builtin_amp_test::ParallelFor(kRows, [&](int index) {
        const int voice = index / (legacy::kStageCounts * static_cast<int>(legacy::kGainSteps));
        const int stages = index / static_cast<int>(legacy::kGainSteps) % legacy::kStageCounts + 1;
        const auto row = static_cast<std::size_t>(index) % legacy::kGainSteps;
        const auto cell = static_cast<std::size_t>(index) * legacy::kTrimSteps + legacy::kTrimSteps / 2;
        const double differenceAtRest = newCells[cell] - oldCells[cell];
        const auto share = [&](double middle, double contour) {
            const ParamMap params = {{"voice", static_cast<double>(voice)},
                                     {"stageCount", static_cast<double>(stages)},
                                     {"gain", legacy::GainAtRow(row)},
                                     {"middle", middle},
                                     {"contour", contour},
                                     {"character", legacy::kMigratedCharacter}};
            legacy::Settings tone;
            tone.middle = middle;
            tone.contour = contour;
            return (both(params, false) - both(params, true) - differenceAtRest) / legacy::ToneStackDb(tone);
        };
        cutShares[index] = share(0.0, 0.6);
        boostShares[index] = share(1.0, legacy::Settings{}.contour);
    });

    const auto printShares = [](const char* name, const std::vector<double>& shares) {
        std::cout << "inline constexpr ShareGrid " << name << " = {\n" << std::fixed << std::setprecision(2);
        std::size_t index = 0;

        for (int voice = 0; voice < 2; ++voice)
        {
            std::cout << "    { // " << (voice == 0 ? "Clean" : "Drive") << ": a row per stage count, Gain across\n";

            for (int stages = 1; stages <= legacy::kStageCounts; ++stages)
            {
                std::cout << "        {";

                for (std::size_t row = 0; row < legacy::kGainSteps; ++row)
                {
                    const double share = shares[index++];
                    std::cout << (row ? ", " : "") << (std::fabs(share) < 0.005 ? 0.0 : share) << "f";
                }

                std::cout << "}" << (stages < legacy::kStageCounts ? "," : "") << "\n";
            }

            std::cout << "    }" << (voice == 0 ? "," : "") << "\n";
        }

        std::cout << "};\n";
    };
    std::cout << "// clang-format off\n// *INDENT-OFF*\n";
    printShares("kToneCutShare", cutShares);
    printShares("kToneBoostShare", boostShares);
    std::cout << "// *INDENT-ON*\n// clang-format on\n";

    // The old power stage: what Power Drive and Sag added to the old amp's level without them.
    struct Point
    {
        double oldDb, drive, sag, addedDb;
    };

    std::vector<ParamMap> settings;

    for (const double voice : {0.0, 1.0})
    {
        for (const double stages : {1.0, 2.0, 4.0})
        {
            for (const double gain : {0.0, 0.0625, 0.125, 0.25, 0.5, 1.0})
            {
                for (const double trim : {-12.0, 0.0, 12.0})
                {
                    for (const double drive : {0.0, 0.25, 0.5, 0.75, 1.0})
                    {
                        for (const double sag : {0.0, 0.5, 1.0})
                        {
                            settings.push_back({{"voice", voice},
                                                {"stageCount", stages},
                                                {"gain", gain},
                                                {"stageGain", trim},
                                                {"powerDrive", drive},
                                                {"sag", sag}});
                        }
                    }
                }
            }
        }
    }

    std::vector<double> levels(settings.size());
    builtin_amp_test::ParallelFor(static_cast<int>(settings.size()),
                                  [&](int i) { levels[i] = both(settings[i], true); });
    std::vector<Point> points;
    double undriven = 0.0;

    for (std::size_t i = 0; i < settings.size(); ++i)
    {
        const double drive = settings[i].at("powerDrive"), sag = settings[i].at("sag");

        if (drive == 0.0 && sag == 0.0)
        {
            undriven = levels[i];
            continue;
        }

        points.push_back({undriven, drive, sag, levels[i] - undriven});
    }

    const auto fit = [&](double feed, double slope, double& worst) {
        double squares = 0.0;
        worst = 0.0;

        for (const Point& point : points)
        {
            const double error =
                legacy::OldPowerStageDb(point.oldDb, point.drive, point.sag, feed, slope) - point.addedDb;
            squares += error * error;
            worst = std::max(worst, std::fabs(error));
        }

        return std::sqrt(squares / static_cast<double>(points.size()));
    };

    double bestFeed = 0.0, bestSlope = 0.0, bestRms = 1.0e9, bestWorst = 0.0;

    for (int feed = -300; feed <= 0; ++feed)
    {
        for (int slope = 50; slope <= 130; ++slope)
        {
            double worst = 0.0;
            const double rms = fit(0.1 * feed, 0.01 * slope, worst);

            if (rms < bestRms)
            {
                bestRms = rms;
                bestWorst = worst;
                bestFeed = 0.1 * feed;
                bestSlope = 0.01 * slope;
            }
        }
    }

    std::cout << std::setprecision(2) << "inline constexpr double kOldFeedDb = " << bestFeed
              << ";\ninline constexpr double kOldFeedSlope = " << bestSlope << "; // " << bestRms << " dB RMS, "
              << bestWorst << " dB at worst\n";
}
} // namespace

int main(int argc, char** argv)
{
    std::cout << "=== BuiltinAmpLegacyMigrationTests ===" << std::endl;
    guitarfx::RegisterBuiltinAmpEffect();

    if (argc > 1 && std::string(argv[1]) == "--calibrate")
    {
        Calibrate();
        return 0;
    }

    TestRules();
    TestReadPaths();

    const auto guitar = builtin_amp_test::NominalGuitar("guitar-riff-01.wav");
    Check(!guitar.empty(), "the demo riff loads");

    if (!guitar.empty())
    {
        TestFactoryContent(guitar);
        TestBetweenGridPoints(guitar);
    }

    std::cout << "\n" << (gChecks - gFailures) << "/" << gChecks << " checks passed" << std::endl;
    return gFailures == 0 ? 0 : 1;
}

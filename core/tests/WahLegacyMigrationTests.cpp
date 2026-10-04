/**
 * @file WahLegacyMigrationTests.cpp
 * @brief Auto-wah nodes saved before the effect was folded into the Wah keep their sound.
 *
 * The old Auto-Wah (helpers/LegacyAutoWah.h, kept verbatim) runs as the Wah's Auto Wah control
 * now. WahLegacyMigration.h gives a stored auto-wah node the wah's parameters, mapped to what the
 * old filter did rather than what its knobs said. These check:
 *   - the rules: which nodes migrate, the defaults for missing keys, clamping, idempotence
 *   - that every way a stored node is read (presets, composites, the global chain) migrates it
 *   - against the old effect itself: at rest and fully open, the migrated node sits where the old
 *     filter peaked, as loud, and pinned open on noise the two come out as loud
 *   - the sweep between agrees at mid-travel and within 10% elsewhere
 */

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "LegacyAutoWah.h"
#include "WahTestSupport.h"
#include "dsp/EffectGuids.h"
#include "dsp/EffectProcessor.h"
#include "dsp/EffectRegistry.h"
#include "dsp/effects/WahEffect.h"
#include "dsp/effects/WahLegacyMigration.h"
#include "presets/PresetStorage.h"
#include "presets/PresetTypes.h"
#include "presets/PresetTypesJson.h"

namespace
{
namespace legacy = guitarfx::wah_legacy;
using ParamMap = std::map<std::string, double>;

using namespace wah_test;
const std::string kAutoWah = guitarfx::EffectGuids::kAutoWah;

/// A node as a preset from before the merge stored it.
nlohmann::json LegacyNodeJson(const std::string& id, const std::string& type, const ParamMap& params)
{
    nlohmann::json node = {{"id", id}, {"type", type}, {"params", nlohmann::json::object()}};

    for (const auto& [key, value] : params)
    {
        node["params"][key] = value;
    }

    return node;
}

/// The old effect with a stored node's params.
std::unique_ptr<guitarfx::EffectProcessor> OldAutoWah(const ParamMap& params)
{
    auto effect = std::make_unique<legacy_wah::LegacyAutoWahEffect>();

    for (const auto& [key, value] : params)
    {
        effect->SetParam(key, value);
    }

    effect->Prepare(kSampleRate, kBlockSize);
    return effect;
}

/// The same node, migrated, as the registry runs it: created by its retired type.
std::unique_ptr<guitarfx::EffectProcessor> MigratedWah(const ParamMap& stored)
{
    ParamMap params = stored;
    legacy::MigrateParams(kAutoWah, params);
    auto effect = guitarfx::EffectRegistry::Instance().Create(kAutoWah);

    for (const auto& [key, value] : params)
    {
        effect->SetParam(key, value);
    }

    effect->Prepare(kSampleRate, kBlockSize);
    return effect;
}

/// The filter's response with the detector where `level` leaves it: a stretch of that level to
/// settle the detector, then an impulse too small to move it, on the same steady level.
Peak ResponseAt(guitarfx::EffectProcessor& effect, double level)
{
    constexpr double kImpulse = 1.0e-3;
    Render(effect, std::vector<float>(static_cast<std::size_t>(0.5 * kSampleRate), static_cast<float>(level)));

    // The steady level's own output has died away by now (the filter blocks DC), so what follows
    // the impulse is its response alone.
    std::vector<float> input(16384, static_cast<float>(level));
    input[0] += static_cast<float>(kImpulse);
    auto ir = Render(effect, input);

    for (auto& sample : ir)
    {
        sample /= static_cast<float>(kImpulse);
    }

    return FindPeak(ir);
}

void TestRules()
{
    std::cout << "\nThe rules" << std::endl;

    for (const std::string type : {kAutoWah, std::string("auto_wah")})
    {
        ParamMap params;
        const bool migrated = legacy::MigrateParams(type, params);
        const double heelQ = legacy::OldEffectiveQ(2.5, 300.0);
        const double toeQ = legacy::OldEffectiveQ(2.5, 2800.0);
        Check(migrated && params.at("control") == 1.0 && params.at("heelFreq") == 150.0 &&
                  params.at("toeFreq") == 1400.0 &&
                  Near(params.at("sensitivity"), legacy::MatchedSensitivity(0.6), 1.0e-9) &&
                  params.at("attack") == 5.0 && params.at("release") == 80.0 && params.at("position") == 0.0 &&
                  params.at("autoEngage") == 0.0 && params.at("mix") == 1.0 && params.at("lowEnd") == 0.0 &&
                  params.at("treble") == 0.0 && params.at("saturation") == 0.0,
              "a bare " + type + " node gets the old defaults on the Auto Wah control");
        Check(Near(params.at("q"), heelQ, 1.0e-9) && Near(params.at("toeQScale"), toeQ / heelQ, 1.0e-9) &&
                  Near(params.at("level"), legacy::MatchedLevelDb(heelQ), 1.0e-9) &&
                  Near(params.at("toeGain"), legacy::MatchedLevelDb(toeQ) - legacy::MatchedLevelDb(heelQ), 1.0e-9) &&
                  Near(params.at("taper"), legacy::LinearSweepTaper(150.0, 1400.0), 1.0e-9),
              "with the old filter's Q and peak at each end of the sweep, and a taper for its linear sweep",
              "q " + Num(params.at("q"), 2) + ", toe x" + Num(params.at("toeQScale"), 2) + ", level " +
                  Num(params.at("level"), 2) + " dB, toe gain " + Num(params.at("toeGain"), 2) + " dB, taper " +
                  Num(params.at("taper"), 2));
        Check(params.size() == guitarfx::wah::kParamCount, "and every wah parameter, nothing else");
    }

    {
        ParamMap params = {
            {"sensitivity", 0.3}, {"minFreq", 500.0}, {"maxFreq", 4000.0}, {"resonance", 6.0}, {"mix", 0.7}};
        legacy::MigrateParams(kAutoWah, params);
        Check(params.at("heelFreq") == 250.0 && params.at("toeFreq") == 2000.0 &&
                  Near(params.at("q"), legacy::OldEffectiveQ(6.0, 500.0), 1.0e-9) &&
                  Near(params.at("sensitivity"), legacy::MatchedSensitivity(0.3), 1.0e-9) && params.at("mix") == 0.7,
              "stored settings carry over, the sweep at half its setting");
        Check(params.count("minFreq") == 0 && params.count("maxFreq") == 0 && params.count("resonance") == 0,
              "and the old keys are gone");
    }

    {
        ParamMap params = {{"minFreq", 50.0}, {"maxFreq", 9000.0}, {"resonance", 50.0}, {"sensitivity", 3.0}};
        legacy::MigrateParams(kAutoWah, params);
        Check(params.at("heelFreq") == 100.0 && params.at("toeFreq") == 2500.0 &&
                  Near(params.at("q"), legacy::OldEffectiveQ(10.0, 200.0), 1.0e-9) &&
                  Near(params.at("sensitivity"), legacy::MatchedSensitivity(1.0), 1.0e-9),
              "out-of-range values migrate as the old effect clamped them");
    }

    {
        ParamMap params = {{"minFreq", 1000.0}, {"maxFreq", 800.0}};
        legacy::MigrateParams(kAutoWah, params);
        Check(params.at("heelFreq") == 500.0 && params.at("toeFreq") == 400.0 && params.at("taper") == 0.0,
              "a sweep set upside down keeps its ends, with an even taper");
    }

    {
        ParamMap params;
        legacy::MigrateParams(kAutoWah, params);
        const ParamMap once = params;
        Check(!legacy::MigrateParams(kAutoWah, params) && params == once, "a migrated node is not migrated again");

        ParamMap current = {{"control", 0.0}, {"minFreq", 500.0}};
        Check(!legacy::MigrateParams(kAutoWah, current) && current.size() == 2,
              "a node with a Control key is left alone");

        ParamMap wahNode = {{"heelFreq", 300.0}};
        Check(!legacy::MigrateParams(guitarfx::EffectGuids::kWah, wahNode) && wahNode.size() == 1,
              "and so is a wah node");

        // A loader may resolve the type to the wah's first; the old keys still give the node away.
        ParamMap resolved = {{"minFreq", 500.0}, {"resonance", 4.0}};
        Check(legacy::MigrateParams(guitarfx::EffectGuids::kWah, resolved) && resolved.at("control") == 1.0 &&
                  resolved.at("heelFreq") == 250.0 && resolved.count("minFreq") == 0,
              "an auto-wah node whose type was already resolved to the wah still migrates, by its old keys");

        ParamMap other = {{"minFreq", 500.0}};
        Check(!legacy::MigrateParams(guitarfx::EffectGuids::kPhaser, other) && other.size() == 1,
              "and a node of any other effect");
    }
}

void TestReadPaths()
{
    std::cout << "\nEvery read path migrates" << std::endl;

    const nlohmann::json presetJson = {{"id", "legacy-preset"},
                                       {"name", "Legacy"},
                                       {"graph",
                                        {{"nodes",
                                          {LegacyNodeJson("aw", kAutoWah, {{"minFreq", 400.0}}),
                                           LegacyNodeJson("aw2", "auto_wah", {{"control", 0.0}, {"heelFreq", 300.0}})}},
                                         {"edges", nlohmann::json::array()}}}};
    const auto preset = guitarfx::PresetStorage::DeserializeFromJson(presetJson.dump());
    bool presetMigrated = false;
    bool typeKept = false;
    bool currentUntouched = false;

    if (preset)
    {
        for (const auto& node : preset->graph.nodes)
        {
            if (node.id == "aw")
            {
                presetMigrated = node.params.count("control") != 0 && node.params.at("control") == 1.0 &&
                                 node.params.at("heelFreq") == 200.0 && node.params.count("minFreq") == 0;
                typeKept = guitarfx::EffectRegistry::Instance().Resolve(node.type) == guitarfx::EffectGuids::kWah;
            }

            if (node.id == "aw2")
            {
                currentUntouched =
                    node.params.size() == 2 && node.params.at("control") == 0.0 && node.params.at("heelFreq") == 300.0;
            }
        }
    }

    Check(preset.has_value() && presetMigrated, "a preset's old auto-wah node is migrated as it loads");
    Check(typeKept, "and its type runs as the wah");
    Check(currentUntouched, "while a node saved since keeps its values");

    const auto graph = guitarfx::DeserializeSignalGraph(
        {{"nodes", {LegacyNodeJson("pre", "auto_wah", {})}}, {"edges", nlohmann::json::array()}});
    Check(!graph.nodes.empty() && graph.nodes[0].params.count("control") != 0 &&
              graph.nodes[0].params.at("heelFreq") == 150.0,
          "so is one in a composite's inner graph or the global chain");

    // A migrated node written out and read back is not migrated again.
    const nlohmann::json written = guitarfx::SerializeNode(graph.nodes[0]);
    const auto reread = guitarfx::DeserializeNode(written);
    Check(reread.params == graph.nodes[0].params, "and saving and reloading it changes nothing");
}

struct OldSetting
{
    const char* label;
    ParamMap params;
};

const std::vector<OldSetting>& OldSettings()
{
    static const std::vector<OldSetting> settings = {
        {"defaults", {}},
        {"low Q", {{"resonance", 0.5}}},
        {"high Q, wide", {{"resonance", 10.0}, {"minFreq", 200.0}, {"maxFreq", 5000.0}}},
        {"narrow, high", {{"resonance", 4.0}, {"minFreq", 800.0}, {"maxFreq", 1600.0}, {"sensitivity", 0.2}}},
    };
    return settings;
}

/// Pink noise at twice full scale, which pins either detector at its toe whatever the sensitivity.
std::vector<float> LoudPinkNoise(double seconds)
{
    std::uint32_t state = 0x2545F491u;
    float b0 = 0.0f;
    float b1 = 0.0f;
    float b2 = 0.0f;
    std::vector<float> out(static_cast<std::size_t>(seconds * kSampleRate));

    for (auto& sample : out)
    {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        const float white = static_cast<float>(state) / 2147483648.0f - 1.0f;
        b0 = 0.99765f * b0 + white * 0.0990460f;
        b1 = 0.96300f * b1 + white * 0.2965164f;
        b2 = 0.57000f * b2 + white * 1.0526913f;
        sample = 0.5f * (b0 + b1 + b2 + white * 0.1848f);
    }

    return out;
}

/// The old filter is measured; the migrated node is read, since WahEffectTests holds the wah to its
/// sweep and gain laws. Measuring the migrated wah's resting response would mean an impulse through
/// its detector, and on the steep taper a wide sweep gets, even a tiny opening moves its filter.
void TestMatchesOldEffect()
{
    std::cout << "\nAgainst the old effect" << std::endl;
    std::cout << "  centre Hz and peak dB, old measured / migrated node, at rest then fully open:" << std::endl;
    bool restMatches = true;
    bool openMatches = true;
    bool loudnessMatches = true;
    std::string problem;

    for (const auto& setting : OldSettings())
    {
        ParamMap migrated = setting.params;
        legacy::MigrateParams(kAutoWah, migrated);
        const double heelQ = migrated.at("q");
        const double toeQ = heelQ * migrated.at("toeQScale");
        const double heelGain =
            guitarfx::wah::kPeakGainScale * std::sqrt(heelQ) * std::pow(10.0, migrated.at("level") / 20.0);
        const double toeGain = guitarfx::wah::kPeakGainScale * std::sqrt(toeQ) *
                               std::pow(10.0, (migrated.at("level") + migrated.at("toeGain")) / 20.0);

        // At rest: the detector at zero. Fully open: a level the sensitivity reads as 1 and more,
        // so the old filter sits at its toe. A very broad peak has no sharp centre, so below Q 1
        // the frequency is held loosely.
        for (const double level : {0.0, 1.0})
        {
            auto old = OldAutoWah(setting.params);
            const Peak before = ResponseAt(*old, level);
            const double expectedHz = migrated.at(level == 0.0 ? "heelFreq" : "toeFreq");
            const double expectedGain = level == 0.0 ? heelGain : toeGain;
            const double q = level == 0.0 ? heelQ : toeQ;
            const double gainDifferenceDb = 20.0 * std::log10(expectedGain / before.gain);
            const bool matches = Near(expectedHz, before.hz, q < 1.0 ? 0.12 : 0.03) && std::abs(gainDifferenceDb) < 0.5;

            if (!matches)
            {
                (level == 0.0 ? restMatches : openMatches) = false;
                problem = std::string(setting.label) + (level == 0.0 ? " at rest" : " open");
            }

            std::cout << "    " << setting.label << (level == 0.0 ? ", rest: " : ", open: ") << Num(before.hz, 1)
                      << " / " << Num(expectedHz, 1) << " Hz, " << Num(20.0 * std::log10(before.gain), 2) << " / "
                      << Num(20.0 * std::log10(expectedGain), 2) << " dB" << std::endl;
        }

        // End to end, both pinned at the toe by loud pink noise. The two filters differ in shape
        // away from the peak (the old one has no zero at Nyquist), so the match is loose.
        const auto pink = LoudPinkNoise(1.0);
        const std::size_t analysisStart = pink.size() / 4;
        auto old = OldAutoWah(setting.params);
        auto wah = MigratedWah(setting.params);
        const double oldDb = 20.0 * std::log10(Rms(Render(*old, pink), analysisStart));
        const double newDb = 20.0 * std::log10(Rms(Render(*wah, pink), analysisStart));
        std::cout << "    " << setting.label << ", pinned open on pink noise: " << Num(oldDb, 2) << " / "
                  << Num(newDb, 2) << " dB" << std::endl;

        if (std::abs(newDb - oldDb) > 1.5)
        {
            loudnessMatches = false;
            problem = std::string(setting.label) + " on pink noise";
        }
    }

    Check(restMatches, "at rest the migrated node sits where the old filter peaked, within 3% and 0.5 dB", problem);
    Check(openMatches, "and fully open it sits where the old filter peaked", problem);
    Check(loudnessMatches,
          "pinned open on loud pink noise, the migrated wah is as loud as the old effect within 1.5 dB", problem);
}

void TestSweepAgreement()
{
    std::cout << "\nThe sweep between" << std::endl;
    bool agrees = true;
    std::string problem;

    for (const auto& setting : OldSettings())
    {
        ParamMap params = setting.params;
        legacy::MigrateParams(kAutoWah, params);
        const double sensitivity = params.at("sensitivity");
        const auto stored = [&setting](const char* key, double fallback) {
            const auto it = setting.params.find(key);
            return it != setting.params.end() ? it->second : fallback;
        };
        const double minFreq = stored("minFreq", legacy::kOldDefaultMinFreq);
        const double maxFreq = stored("maxFreq", legacy::kOldDefaultMaxFreq);

        for (const double opening : {0.25, 0.5, 0.75})
        {
            // The old sweep was linear in its setting, and sat at half of it.
            const double oldHz = (minFreq + (maxFreq - minFreq) * opening) * legacy::kOldCentreScale;
            auto migrated = MigratedWah(setting.params);
            const double level = opening / guitarfx::wah::SensitivityGain(sensitivity);
            Render(*migrated,
                   std::vector<float>(static_cast<std::size_t>(0.5 * kSampleRate), static_cast<float>(level)));
            const double newHz = migrated->GetParam("currentFrequency");
            // The taper is clamped at 1 for the widest sweeps, which leaves mid-travel about 1% out.
            const double tolerance = opening == 0.5 ? 0.02 : 0.10;

            if (!Near(newHz, oldHz, tolerance))
            {
                agrees = false;
                problem = std::string(setting.label) + " at " + Num(opening, 2) + ": " + Num(oldHz, 1) + " vs " +
                          Num(newHz, 1) + " Hz";
            }
        }
    }

    Check(agrees, "part-way open, the two sweeps agree within 2% at mid-travel and 10% at a quarter and three quarters",
          problem);
}
} // namespace

int main()
{
    std::cout << "=== WahLegacyMigrationTests ===" << std::endl;
    guitarfx::RegisterWahEffect();

    TestRules();
    TestReadPaths();
    TestMatchesOldEffect();
    TestSweepAgreement();

    std::cout << "\n" << (gChecks - gFailures) << "/" << gChecks << " checks passed" << std::endl;
    return gFailures == 0 ? 0 : 1;
}

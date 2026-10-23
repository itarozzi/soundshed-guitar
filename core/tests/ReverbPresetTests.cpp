/**
 * @file ReverbPresetTests.cpp
 * @brief The algorithmic reverbs' factory presets (core/src/dsp/effects/ReverbPresets.h).
 *
 * For Room, Chamber, Advanced, Spring and Ambient Reverb, and Verbal:
 *   - each ships factory presets, each with a unique id and a name
 *   - exactly one is the default, it comes first, and it is the parameter defaults, so a node
 *     added fresh and one set to the default preset agree
 *   - every preset sets every control its reverb declares except Output, in range, on an enum's
 *     steps, and nothing else, so choosing one never leaves another's settings behind
 *   - every preset runs finite, dies away, and sits within reach of the default's level
 */

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "dsp/EffectGuids.h"
#include "dsp/EffectProcessor.h"
#include "dsp/EffectRegistry.h"
#include "dsp/FiniteCheck.h"
#include "dsp/effects/AmbientReverbEffect.h"
#include "dsp/effects/ReverbEffect.h"
#include "dsp/effects/SpringReverbEffect.h"
#include "dsp/reverb/VerbalEffect.h"

namespace
{
constexpr double kSampleRate = 48000.0;
constexpr int kBlockSize = 256;

int gFailures = 0;
int gChecks = 0;

void Check(bool condition, const std::string& what, const std::string& detail = "")
{
    ++gChecks;

    if (condition)
    {
        std::cout << "  [PASS] " << what;
    }
    else
    {
        ++gFailures;
        std::cout << "  [FAIL] " << what;
    }

    if (!detail.empty())
    {
        std::cout << "  (" << detail << ")";
    }

    std::cout << std::endl;
}

struct Reverb
{
    const char* name;
    const char* type;
};

constexpr Reverb kReverbs[] = {
    {"Room", guitarfx::EffectGuids::kReverbRoom},         {"Chamber", guitarfx::EffectGuids::kReverbChamber},
    {"Advanced", guitarfx::EffectGuids::kReverbAdvanced}, {"Spring", guitarfx::EffectGuids::kReverbSpring},
    {"Ambient", guitarfx::EffectGuids::kReverbAmbient},   {"Verbal", guitarfx::EffectGuids::kReverbVerbal}};

// Output is the player's level, so presets leave it alone.
bool PresetSets(const std::string& id)
{
    return id != "outputGain";
}

void TestWellFormed(const Reverb& reverb)
{
    std::cout << "\n" << reverb.name << " Reverb: the preset list" << std::endl;
    const auto info = guitarfx::EffectRegistry::Instance().GetTypeInfo(reverb.type);

    if (!info)
    {
        Check(false, "registered");
        return;
    }

    Check(info->presets.size() >= 4, "ships factory presets", std::to_string(info->presets.size()));

    std::set<std::string> ids;
    bool unique = true;
    int defaults = 0;

    for (const auto& preset : info->presets)
    {
        unique = unique && !preset.id.empty() && !preset.displayName.empty() && preset.isFactory &&
                 ids.insert(preset.id).second;
        defaults += preset.isDefault ? 1 : 0;
    }

    Check(unique, "each is a named factory preset with its own id");
    Check(defaults == 1 && !info->presets.empty() && info->presets.front().isDefault,
          "exactly one starts new nodes, and it comes first", std::to_string(defaults) + " defaults");

    bool defaultIsDefaults = !info->presets.empty();
    std::string problem;

    for (const auto& def : info->parameters)
    {
        if (!PresetSets(def.id) || info->presets.empty())
        {
            continue;
        }

        const auto& values = info->presets.front().parameters;
        const auto found = values.find(def.id);

        if (found == values.end() || found->second != def.defaultValue)
        {
            defaultIsDefaults = false;
            problem = def.id;
        }
    }

    Check(defaultIsDefaults, "the default preset is the parameter defaults, so a fresh node sounds as before", problem);

    bool complete = true;
    problem.clear();

    for (const auto& preset : info->presets)
    {
        std::size_t expected = 0;

        for (const auto& def : info->parameters)
        {
            if (!PresetSets(def.id))
            {
                complete = complete && preset.parameters.count(def.id) == 0;
                problem = complete ? problem : preset.id + " sets " + def.id;
                continue;
            }

            ++expected;
            const auto found = preset.parameters.find(def.id);

            if (found == preset.parameters.end())
            {
                complete = false;
                problem = preset.id + " omits " + def.id;
                continue;
            }

            const double value = found->second;
            const double offset = value - def.minValue;
            const bool onStep = def.step <= 0.0 || std::abs(std::round(offset / def.step) * def.step - offset) < 1e-9;

            if (!guitarfx::IsFinite(value) || value < def.minValue || value > def.maxValue || !onStep)
            {
                complete = false;
                problem = preset.id + " puts " + def.id + " out of range";
            }
        }

        if (preset.parameters.size() != expected)
        {
            complete = false;
            problem = preset.id + " sets a key the reverb does not declare";
        }
    }

    Check(complete, "every preset sets every control but Output, in range, and nothing else", problem);
}

struct Rendered
{
    bool finite = true;
    double peak = 0.0;
    double energy = 0.0;
    double early = 0.0; // tail energy 0.15-0.35 s after the burst ends
    double late = 0.0;  // and 0.2 s from lateFromS after (0.95 s unless asked)
};

// A 50 ms burst of noise at -12 dBFS rms, then 1.2 s of tail.
Rendered Render(const std::string& type, const std::map<std::string, double>& params, double lateFromS = 0.95)
{
    auto& registry = guitarfx::EffectRegistry::Instance();
    auto effect = registry.Create(type);
    const auto info = registry.GetTypeInfo(type);

    for (const auto& def : info->parameters)
    {
        effect->SetParam(def.id, def.defaultValue);
    }

    for (const auto& [key, value] : params)
    {
        effect->SetParam(key, value);
    }

    effect->Prepare(kSampleRate, kBlockSize);

    const auto burst = static_cast<std::size_t>(kSampleRate * 0.05);
    const auto total = burst + static_cast<std::size_t>(kSampleRate * std::max(1.2, lateFromS + 0.25));
    std::vector<float> inL(kBlockSize), inR(kBlockSize), outL(kBlockSize), outR(kBlockSize);
    float* inputs[2] = {inL.data(), inR.data()};
    float* outputs[2] = {outL.data(), outR.data()};
    std::uint32_t seed = 12345u;
    Rendered r;

    for (std::size_t start = 0; start < total; start += kBlockSize)
    {
        for (int i = 0; i < kBlockSize; ++i)
        {
            float v = 0.0f;

            if (start + i < burst)
            {
                seed = seed * 1664525u + 1013904223u;
                v = 0.25f * 1.7320508f * (static_cast<float>(seed >> 8) / 8388608.0f - 1.0f);
            }

            inL[i] = v;
            inR[i] = v;
        }

        effect->Process(inputs, outputs, kBlockSize);

        for (int i = 0; i < kBlockSize; ++i)
        {
            const std::size_t n = start + static_cast<std::size_t>(i);
            const double e = 0.5 * (static_cast<double>(outL[i]) * outL[i] + static_cast<double>(outR[i]) * outR[i]);
            r.finite = r.finite && guitarfx::IsFinite(outL[i]) && guitarfx::IsFinite(outR[i]);
            r.peak = std::max({r.peak, static_cast<double>(std::abs(outL[i])), static_cast<double>(std::abs(outR[i]))});
            r.energy += e;

            if (n < burst)
            {
                continue;
            }

            const double t = static_cast<double>(n - burst) / kSampleRate;

            if (t >= 0.15 && t < 0.35)
            {
                r.early += e;
            }
            else if (t >= lateFromS && t < lateFromS + 0.2)
            {
                r.late += e;
            }
        }
    }

    return r;
}

void TestPresetsRun(const Reverb& reverb)
{
    std::cout << "\n" << reverb.name << " Reverb: every preset runs" << std::endl;
    const auto info = guitarfx::EffectRegistry::Instance().GetTypeInfo(reverb.type);

    if (!info || info->presets.empty())
    {
        Check(false, "has presets to run");
        return;
    }

    // Verbal runs to long, slow-building tails (the Cloud swells for half a second before it
    // decays), so its tail is read three seconds on rather than one.
    const bool verbal = std::string(reverb.type) == guitarfx::EffectGuids::kReverbVerbal;
    const double lateFromS = verbal ? 2.95 : 0.95;
    const auto reference = Render(reverb.type, info->presets.front().parameters, lateFromS);
    bool sound = true;
    std::string problem;

    for (const auto& preset : info->presets)
    {
        const auto r = Render(reverb.type, preset.parameters, lateFromS);
        const double levelDb = 10.0 * std::log10(std::max(r.energy, 1e-30) / std::max(reference.energy, 1e-30));
        const double decayDb = 10.0 * std::log10(std::max(r.late, 1e-30) / std::max(r.early, 1e-30));
        std::cout << "    " << preset.displayName << ": level " << std::round(levelDb * 10.0) / 10.0
                  << " dB against the default, tail " << std::round(decayDb * 10.0) / 10.0 << " dB over "
                  << (lateFromS - 0.15) << " s" << std::endl;

        // The bounds catch a gross mistake, not a voicing: a preset jumping far from the default's
        // level, a tail that holds or grows (the longest decays about 3 dB over this span), or a
        // burst at -12 dBFS rms coming out near full scale.
        // Verbal's Swell in Dry mode swells the dry too, and a 50 ms burst is over before it has
        // risen: its level is not comparable, so only the rest is held to it.
        const auto machine = preset.parameters.find("machine");
        const bool swell = verbal && machine != preset.parameters.end() && machine->second == 4.0;
        const bool levelOk = swell || (levelDb >= -9.0 && levelDb <= 9.0);

        if (!r.finite || r.peak > 1.0 || !levelOk || decayDb > -1.0)
        {
            sound = false;
            problem = preset.id;
        }
    }

    Check(sound, "each stays finite, dies away, and stays within 9 dB of the default's level", problem);
}

std::unique_ptr<guitarfx::EffectProcessor> MakeAtDefaults(const std::string& type)
{
    auto& registry = guitarfx::EffectRegistry::Instance();
    auto effect = registry.Create(type);

    for (const auto& def : registry.GetTypeInfo(type)->parameters)
    {
        effect->SetParam(def.id, def.defaultValue);
    }

    effect->Prepare(kSampleRate, kBlockSize);
    return effect;
}

/// Runs `seconds` of input, a noise burst at -12 dBFS rms for the first `burstSeconds` and silence
/// after, and returns the output's energy.
double RunEnergy(guitarfx::EffectProcessor& effect, double seconds, double burstSeconds = 0.0)
{
    const auto total = static_cast<std::size_t>(kSampleRate * seconds);
    const auto burst = static_cast<std::size_t>(kSampleRate * burstSeconds);
    std::vector<float> inL(kBlockSize), inR(kBlockSize), outL(kBlockSize), outR(kBlockSize);
    float* inputs[2] = {inL.data(), inR.data()};
    float* outputs[2] = {outL.data(), outR.data()};
    std::uint32_t seed = 777u;
    double energy = 0.0;

    for (std::size_t start = 0; start < total; start += kBlockSize)
    {
        for (int i = 0; i < kBlockSize; ++i)
        {
            float v = 0.0f;

            if (start + i < burst)
            {
                seed = seed * 1664525u + 1013904223u;
                v = 0.25f * 1.7320508f * (static_cast<float>(seed >> 8) / 8388608.0f - 1.0f);
            }

            inL[i] = v;
            inR[i] = v;
        }

        effect.Process(inputs, outputs, kBlockSize);

        for (int i = 0; i < kBlockSize; ++i)
        {
            energy += static_cast<double>(outL[i]) * outL[i] + static_cast<double>(outR[i]) * outR[i];
        }
    }

    return energy;
}

/// Setting Advanced Reverb's Character to the one it already has (a CC landing on the same step,
/// an effect preset that keeps it) used to clear the tank, chopping the tail every time.
void TestSameCharacterKeepsTheTail()
{
    std::cout << "\nAdvanced Reverb: the same Character again keeps the tail" << std::endl;
    auto effect = MakeAtDefaults(guitarfx::EffectGuids::kReverbAdvanced);
    RunEnergy(*effect, 0.25, 0.05);
    const double before = RunEnergy(*effect, 0.1);
    effect->SetParam("character", effect->GetParam("character"));
    const double after = RunEnergy(*effect, 0.1);
    Check(before > 1e-6 && after > before * 0.05, "the tail rings on",
          "energy " + std::to_string(before) + " before, " + std::to_string(after) + " after");
}

/// At Mix 0 the reverb stops running its tank. It used to keep what the tank held, so raising Mix
/// seconds later played the old tail, and the old pre-delay as if it were new input.
void TestMixUpFromZeroStartsClean(const Reverb& reverb)
{
    std::cout << "\n" << reverb.name << " Reverb: Mix up from zero starts clean" << std::endl;
    auto effect = MakeAtDefaults(reverb.type);
    effect->SetParam("mix", 1.0);
    RunEnergy(*effect, 0.25, 0.05);
    effect->SetParam("mix", 0.0);
    RunEnergy(*effect, 1.0);
    effect->SetParam("mix", 1.0);
    const double replayed = RunEnergy(*effect, 0.5);
    Check(replayed < 1e-12, "silence in, silence out", "energy " + std::to_string(replayed));
}
} // namespace

int main()
{
    std::cout << "=== ReverbPresetTests ===" << std::endl;
    guitarfx::RegisterReverbEffect();
    guitarfx::RegisterSpringReverbEffect();
    guitarfx::RegisterAmbientReverbEffect();
    guitarfx::RegisterVerbalEffect();

    for (const auto& reverb : kReverbs)
    {
        TestWellFormed(reverb);
        TestPresetsRun(reverb);
    }

    TestSameCharacterKeepsTheTail();

    for (const auto& reverb : kReverbs)
    {
        // Spring, Ambient and Verbal are their own effects, with no Mix-zero shortcut.
        if (std::string(reverb.type) != guitarfx::EffectGuids::kReverbSpring &&
            std::string(reverb.type) != guitarfx::EffectGuids::kReverbAmbient &&
            std::string(reverb.type) != guitarfx::EffectGuids::kReverbVerbal)
        {
            TestMixUpFromZeroStartsClean(reverb);
        }
    }

    std::cout << "\n" << (gChecks - gFailures) << "/" << gChecks << " checks passed" << std::endl;
    return gFailures == 0 ? 0 : 1;
}

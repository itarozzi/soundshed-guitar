/**
 * @file AudioSnapshot.cpp
 * @brief Renders what every effect, and a set of whole signal chains, sound like at their
 *        defaults, so two builds of the app can be compared.
 *
 * Not a ctest test. tools/audio-ab/audio-ab.mjs builds this against each revision it compares,
 * copying this file and its two headers into older checkouts so both sides run the same
 * harness, then compares the renders. See tools/audio-ab/README.md.
 *
 * Two passes:
 *  - effects: every registered effect type on its own in a SignalGraphExecutor, with the
 *    parameters a node the user has just added gets (registry defaults, then the effect's
 *    nominated factory preset) and, for the NAM types, the interface calibration level that
 *    PluginController injects at default settings. This isolates each effect's DSP.
 *  - chains: the chains in the definition file, each loaded with a `loadPreset` message into a
 *    real PluginController running a fresh profile -- default app settings, standalone -- so
 *    the input stage, NAM calibration, global chain, mixer and master are all the app's own.
 *
 * Every stimulus is rendered through a fresh engine. The `di` stimulus is rendered twice and the
 * two compared, which marks cases that are not deterministic (an effect seeding a random
 * generator from the clock), so the comparison can judge those by level and tone alone.
 *
 * Usage:
 *   AudioSnapshot --out DIR --definition FILE --assets DIR --di FILE
 *                 [--rate 48000] [--block 64] [--only effects|chains] [--filter TEXT]
 *                 [--signals di,sweep,impulse,silence] [--no-repeat-check]
 */

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "AudioSnapshotChains.h"
#include "AudioSnapshotSupport.h"
#include "dsp/EffectRegistry.h"
#include "dsp/SignalGraphExecutor.h"
#include "dsp/effects/BuiltinEffects.h"
#include "presets/PresetTypes.h"

namespace fs = std::filesystem;
using nlohmann::json;
using namespace audio_snapshot;

namespace
{
constexpr int kHarnessVersion = 1;

// What PluginController applies at default settings (SettingsKeys.h), mirrored for the effects
// pass, which runs the executor without a controller. The chains pass gets the real thing.
constexpr double kDefaultNamInterfaceCalibrationDbu = 12.0;

struct Options
{
    fs::path out;
    fs::path definition;
    fs::path assets;
    fs::path di;
    double sampleRate = 48000.0;
    int block = 64;
    std::string only;
    std::string filter;
    std::vector<std::string> signals;
    bool repeatCheck = true;
};

std::vector<std::string> SplitCsv(const std::string& s)
{
    std::vector<std::string> out;
    std::stringstream ss(s);

    for (std::string item; std::getline(ss, item, ',');)
    {
        if (!item.empty())
        {
            out.push_back(item);
        }
    }

    return out;
}

bool ParseArgs(int argc, char** argv, Options& o)
{
    for (int i = 1; i < argc; ++i)
    {
        const std::string a = argv[i];
        const auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : std::string{}; };

        if (a == "--out")
        {
            o.out = next();
        }
        else if (a == "--definition")
        {
            o.definition = next();
        }
        else if (a == "--assets")
        {
            o.assets = next();
        }
        else if (a == "--di")
        {
            o.di = next();
        }
        else if (a == "--rate")
        {
            o.sampleRate = std::atof(next().c_str());
        }
        else if (a == "--block")
        {
            o.block = std::atoi(next().c_str());
        }
        else if (a == "--only")
        {
            o.only = next();
        }
        else if (a == "--filter")
        {
            o.filter = next();
        }
        else if (a == "--signals")
        {
            o.signals = SplitCsv(next());
        }
        else if (a == "--no-repeat-check")
        {
            o.repeatCheck = false;
        }
        else
        {
            std::cerr << "unknown argument: " << a << "\n";
            return false;
        }
    }

    return !o.out.empty() && !o.definition.empty() && !o.assets.empty() && o.sampleRate > 0.0 && o.block > 0;
}

json LoadJson(const fs::path& file)
{
    std::ifstream f(file, std::ios::binary);

    if (!f)
    {
        throw std::runtime_error("cannot open " + file.string());
    }

    return json::parse(f);
}

bool Matches(const Options& o, std::initializer_list<std::string> names)
{
    if (o.filter.empty())
    {
        return true;
    }

    for (const auto& n : names)
    {
        if (n.find(o.filter) != std::string::npos)
        {
            return true;
        }
    }

    return false;
}

bool IsNamCalibratable(const std::string& canonicalType)
{
    // controller_detail::IsNamCalibratableEffectType, by alias so older revisions resolve it too.
    auto& registry = guitarfx::EffectRegistry::Instance();

    for (const char* alias : {"amp_nam", "amp_nam_optimized", "amp_nam_blend", "fx_nam"})
    {
        if (registry.Resolve(alias) == canonicalType)
        {
            return true;
        }
    }

    return false;
}

// ---------------------------------------------------------------- output

struct CaseWriter
{
    const Options& options;
    json cases = json::array();

    /// Writes the render and records it. `extra` carries the case's own fields.
    void Add(const std::string& kind, const std::string& subject, const std::string& slug, const Stimulus& stim,
             const Render& render, std::optional<bool> deterministic, json extra)
    {
        const bool mono = IsDualMono(render);
        const std::string rel = kind + "s/" + slug + "__" + stim.id + ".wav";

        if (!WriteWav(options.out / rel, render, options.sampleRate, mono))
        {
            std::cerr << "  could not write " << rel << "\n";
        }

        json c = {{"id", kind + "/" + subject + "/" + stim.id},
                  {"kind", kind},
                  {"subject", subject},
                  {"slug", slug},
                  {"signal", stim.id},
                  {"file", rel},
                  {"frames", render.left.size()},
                  {"playFrames", stim.playFrames},
                  {"channels", mono ? 1 : 2},
                  {"processFailures", render.processFailures},
                  {"stats", MeasureRender(render)}};

        if (deterministic.has_value())
        {
            c["deterministic"] = *deterministic;
        }

        c.update(extra);
        const auto& st = c["stats"];
        std::printf("  %-44s peak %8.4f  rms %8.5f%s%s\n", (slug + "__" + stim.id).c_str(), st["peak"].get<double>(),
                    st["rms"].get<double>(), st["nonFinite"].get<long>() > 0 ? "  NON-FINITE" : "",
                    deterministic.has_value() && !*deterministic ? "  (not deterministic)" : "");
        cases.push_back(std::move(c));
    }
};

// ---------------------------------------------------------------- effects pass

json DescribeRegistry()
{
    json types = json::array();

    for (const auto& t : guitarfx::EffectRegistry::Instance().GetAllTypes())
    {
        json params = json::array();

        for (const auto& p : t.parameters)
        {
            params.push_back(DescribeParam(p));
        }

        json presets = json::array();

        for (const auto& p : t.presets)
        {
            presets.push_back(p.id);
        }

        types.push_back({{"type", t.type},
                         {"slug", EffectSlug(t)},
                         {"name", t.displayName},
                         {"category", t.category},
                         {"aliases", t.aliases},
                         {"requiresResource", t.requiresResource},
                         {"resourceType", t.resourceType},
                         {"parameters", params},
                         {"presets", presets},
                         {"freshPreset", FreshNodePresetId(t)},
                         {"freshParams", ParamsJson(FreshNodeParams(t))}});
    }

    return types;
}

Render RenderExecutor(const guitarfx::SignalGraph& graph, const Stimulus& stim, const Options& o, int* latencyOut)
{
    auto dsp = std::make_unique<guitarfx::SignalGraphExecutor>();
    dsp->SetInputTrim(0.0);
    dsp->SetOutputTrim(0.0);
    dsp->SetTempo(kTempoBpm);
    dsp->SetGraph(graph);
    dsp->Prepare(o.sampleRate, o.block);

    if (latencyOut != nullptr)
    {
        *latencyOut = dsp->GetTotalLatencySamples();
    }

    return RenderBlocks(stim.samples, o.block, [&](float** in, float** out, int n) {
        dsp->Process(in, out, n);
        return true;
    });
}

void RunEffects(const Options& o, const Resources& resources, const std::vector<Stimulus>& stimuli, CaseWriter& writer)
{
    const auto types = guitarfx::EffectRegistry::Instance().GetAllTypes();
    std::cout << "effects: " << types.size() << " registered types\n";

    for (const auto& t : types)
    {
        const std::string slug = EffectSlug(t);

        if (!Matches(o, {slug, t.type, t.displayName}))
        {
            continue;
        }

        guitarfx::GraphNode fx;
        fx.id = "fx";
        fx.type = t.type;
        fx.category = t.category;
        fx.enabled = true;
        fx.params = FreshNodeParams(t);
        std::vector<std::string> notes;

        if (IsNamCalibratable(t.type))
        {
            fx.params["calibrationInputLevel"] = kDefaultNamInterfaceCalibrationDbu;
            fx.params["calibrationInputLevelEnabled"] = 1.0;
            notes.push_back("NAM interface calibration injected at the default 12 dBu");
        }

        std::string resourceKey;

        if (t.requiresResource)
        {
            const auto keyIt = resources.forEffectType.find(t.resourceType);
            const auto file = keyIt == resources.forEffectType.end() ? std::nullopt : resources.Find(keyIt->second);

            if (file)
            {
                resourceKey = keyIt->second;
                fx.resources.push_back(FileRef(t.resourceType, *file));
            }
            else
            {
                notes.push_back("no test resource for resource type '" + t.resourceType + "'");
            }
        }

        guitarfx::SignalGraph graph;
        graph.nodes = {BoundaryNode("input", guitarfx::kNodeTypeInput), fx,
                       BoundaryNode("output", guitarfx::kNodeTypeOutput)};
        graph.edges = {Edge("input", "fx"), Edge("fx", "output")};

        for (const auto& stim : stimuli)
        {
            int latency = 0;
            const Render render = RenderExecutor(graph, stim, o, &latency);
            std::optional<bool> deterministic;

            if (o.repeatCheck && &stim == &stimuli.front())
            {
                deterministic = SameSamples(render, RenderExecutor(graph, stim, o, nullptr));
            }

            writer.Add("effect", t.type, slug, stim, render, deterministic,
                       {{"name", t.displayName},
                        {"category", t.category},
                        {"latencySamples", latency},
                        {"resource", resourceKey},
                        {"params", ParamsJson(fx.params)},
                        {"notes", notes}});
        }
    }
}

// ---------------------------------------------------------------- chains pass

void RunChains(const Options& o, const json& def, const Resources& resources, const std::vector<Stimulus>& stimuli,
               CaseWriter& writer, json& skipped)
{
    const auto chains = def.value("chains", json::array());
    std::cout << "chains: " << chains.size() << " defined\n";
    const fs::path profiles = o.out / "_profiles";

    for (const auto& spec : chains)
    {
        const std::string id = spec.at("id").get<std::string>();

        if (!Matches(o, {id}))
        {
            continue;
        }

        const ChainBuild chain = BuildChain(spec, resources);

        if (!chain.error.empty())
        {
            std::cout << "  " << id << ": skipped, " << chain.error << "\n";
            skipped.push_back({{"kind", "chain"}, {"subject", id}, {"reason", chain.error}});
            continue;
        }

        for (const auto& stim : stimuli)
        {
            const fs::path profile = profiles / (id + "__" + stim.id);
            const auto first = RenderController(chain, spec, stim, o.sampleRate, o.block, profile);
            std::optional<bool> deterministic;

            if (o.repeatCheck && &stim == &stimuli.front())
            {
                deterministic = SameSamples(first.render,
                                            RenderController(chain, spec, stim, o.sampleRate, o.block, profile).render);
            }

            json notes = json::array();

            if (first.activePreset != chain.preset.id)
            {
                notes.push_back("controller's active preset is '" + first.activePreset + "', not the chain");
            }

            writer.Add("chain", id, id, stim, first.render, deterministic,
                       {{"name", spec.value("name", id)},
                        {"description", spec.value("description", "")},
                        {"nodes", chain.nodes},
                        {"globalChain", first.globalChain},
                        {"notes", notes}});
        }
    }

    std::error_code ec;
    fs::remove_all(profiles, ec);
}
} // namespace

int main(int argc, char** argv)
{
    try
    {
        Options o;

        if (!ParseArgs(argc, argv, o))
        {
            std::cerr << "usage: AudioSnapshot --out DIR --definition FILE --assets DIR --di FILE [--rate 48000]\n"
                         "                     [--block 64] [--only effects|chains] [--filter TEXT]\n"
                         "                     [--signals di,sweep,impulse,silence] [--no-repeat-check]\n";
            return 2;
        }

        const json def = LoadJson(o.definition);
        const Resources resources = LoadResources(def, o.assets);

        for (const auto& [key, file] : resources.byKey)
        {
            if (!fs::exists(file))
            {
                std::cerr << "missing resource '" << key << "': " << file.string() << "\n";
                return 1;
            }
        }

        if (o.signals.empty())
        {
            o.signals = def.value("signals", std::vector<std::string>{"di", "sweep", "impulse", "silence"});
        }

        double diRate = 0.0;
        const auto diRaw = o.di.empty() ? std::vector<double>{} : ReadWavMono(o.di, diRate);
        const auto di = Resample(diRaw, diRate, o.sampleRate);
        std::vector<Stimulus> stimuli;

        for (const auto& id : o.signals)
        {
            Stimulus s;

            if (!MakeStimulus(id, o.sampleRate, di, s))
            {
                std::cerr << "cannot make stimulus '" << id << "'" << (di.empty() ? " (no DI audio loaded)" : "")
                          << "\n";
                return 1;
            }

            stimuli.push_back(std::move(s));
        }

        fs::create_directories(o.out);
        guitarfx::RegisterAllEffects();

        const auto started = std::chrono::steady_clock::now();
        CaseWriter writer{o};
        json skipped = json::array();

        if (o.only.empty() || o.only == "effects")
        {
            RunEffects(o, resources, stimuli, writer);
        }

        if (o.only.empty() || o.only == "chains")
        {
            RunChains(o, def, resources, stimuli, writer, skipped);
        }

        {
            std::ofstream f(o.out / "effects.json", std::ios::binary);
            f << DescribeRegistry().dump(1) << "\n";
        }

        json signals = json::array();

        for (const auto& s : stimuli)
        {
            signals.push_back({{"id", s.id}, {"frames", s.samples.size()}, {"playFrames", s.playFrames}});
        }

        const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
        json manifest = {{"harnessVersion", kHarnessVersion},
                         {"sampleRate", o.sampleRate},
                         {"block", o.block},
                         {"signals", signals},
                         {"renderSeconds", seconds},
                         {"cases", writer.cases},
                         {"skipped", skipped}};
        std::ofstream f(o.out / "manifest.json", std::ios::binary);
        f << manifest.dump(1) << "\n";
        std::cout << "done: " << writer.cases.size() << " renders in " << seconds << " s -> " << o.out.string() << "\n";
        return 0;
    }
    catch (const std::exception& e)
    {
        std::cerr << "ERROR: " << e.what() << "\n";
        return 1;
    }
}

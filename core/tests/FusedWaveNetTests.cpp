// The fused WaveNet pass (dsp/nam/FusedWaveNet.h) against the NAM core it stands in for.
//
// - Every model in the test assets, and synthetic models for the shapes the assets lack (lite,
//   feather, four arrays, odd channel counts, kernel-1 layers, a multi-tap head with bias), at
//   least 100 dB from the NAM core, with the same levels, sample rate and prewarm length.
// - Bit-identical output across host block patterns, past the prepared size included.
// - The one-pass prewarm leaves the state a real prewarm does, bit for bit.
// - Time scales 2 and 4 against the NAM core's.
// - The detector refusing gated, FiLM, head1x1, grouped, condition-DSP and slimmable layers,
//   post-stack heads, other activations and wrong weight counts.
// - NamModelCache building fused models that share weights, and the NAM core when switched off.
//
// docs/nam-dsp-performance-review-2026-10-16.md has the measurements behind it.

#include "dsp/NamModelCache.h"
#include "dsp/nam/FusedWaveNet.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "NAM/activations.h"
#include "NAM/dsp.h"
#include "NAM/get_dsp.h"
#include "NAM/slimmable.h"

namespace fs = std::filesystem;
namespace fwn = guitarfx::fusedwavenet;
namespace cache = guitarfx::nammodelcache;
using nlohmann::json;

namespace nam::factory
{
void ForceFactoryRegistration();
}

namespace
{
bool gAllPassed = true;

void Check(bool condition, const std::string& what)
{
    if (!condition)
    {
        std::cerr << "FAIL: " << what << std::endl;
        gAllPassed = false;
    }
    else
    {
        std::cout << "  ok: " << what << std::endl;
    }
}

constexpr double kSampleRate = 48000.0;
constexpr double kMinSnrDb = 100.0;

/// nam::DSP keeps PrewarmSamples() protected; a member pointer through a derived type reads it.
struct PrewarmReader : ::nam::DSP
{
    static int Read(::nam::DSP& model)
    {
        return (model.*(&PrewarmReader::PrewarmSamples))();
    }
};

/// A guitar-like signal: decaying notes plus a little noise.
std::vector<double> MakeInput(int frames)
{
    std::vector<double> x(static_cast<size_t>(frames));
    unsigned seed = 999;

    for (int i = 0; i < frames; ++i)
    {
        const double t = i / kSampleRate;
        const double env = std::exp(-std::fmod(t, 0.5) * 5.0);
        seed = seed * 1664525u + 1013904223u;
        x[static_cast<size_t>(i)] = 0.3 * env * std::sin(2 * 3.14159265358979 * 110.0 * t) +
                                    0.25 * env * std::sin(2 * 3.14159265358979 * 331.0 * t) +
                                    (double(seed >> 8) / double(1 << 24) - 0.5) * 0.01;
    }

    return x;
}

/// Renders from the model's current state in host blocks cycling through `pattern`.
std::vector<double> Run(::nam::DSP& model, const std::vector<double>& input, const std::vector<int>& pattern)
{
    std::vector<double> in = input;
    std::vector<double> out(input.size(), 0.0);
    size_t pos = 0;
    size_t step = 0;

    while (pos < in.size())
    {
        const int n =
            static_cast<int>(std::min<size_t>(static_cast<size_t>(pattern[step++ % pattern.size()]), in.size() - pos));
        NAM_SAMPLE* inPtr[1] = {in.data() + pos};
        NAM_SAMPLE* outPtr[1] = {out.data() + pos};
        model.process(inPtr, outPtr, n);
        pos += static_cast<size_t>(n);
    }

    return out;
}

std::vector<double> Render(::nam::DSP& model, const std::vector<double>& input, const std::vector<int>& pattern,
                           int maxBuffer = 256)
{
    model.Reset(kSampleRate, maxBuffer);
    return Run(model, input, pattern);
}

double SnrDb(const std::vector<double>& reference, const std::vector<double>& x, size_t count = 0)
{
    const size_t n = count == 0 ? reference.size() : std::min(count, reference.size());
    double error = 0.0;
    double signal = 0.0;

    for (size_t i = 0; i < n; ++i)
    {
        error += (x[i] - reference[i]) * (x[i] - reference[i]);
        signal += reference[i] * reference[i];
    }

    return error == 0.0 ? 999.0 : 10.0 * std::log10(signal / error);
}

bool SameBits(const std::vector<double>& a, const std::vector<double>& b)
{
    return a.size() == b.size() && std::memcmp(a.data(), b.data(), a.size() * sizeof(double)) == 0;
}

std::string Fixed(double value)
{
    char text[32];
    std::snprintf(text, sizeof(text), "%.1f", value);
    return text;
}

// ---------------------------------------------------------------- model sources
/// A model to compare: how to build the NAM core's and the fused one, at one slimmable size.
struct Subject
{
    std::string name;
    std::string shape; // the config without its weights or head scale, plus the slimmable size
    std::function<std::unique_ptr<::nam::DSP>()> core;
    std::function<std::unique_ptr<::nam::DSP>()> fused;
};

json WithoutWeights(const json& value)
{
    if (value.is_object())
    {
        json out = json::object();

        for (const auto& [key, item] : value.items())
        {
            if (key != "weights" && key != "metadata" && key != "head_scale")
            {
                out[key] = WithoutWeights(item);
            }
        }

        return out;
    }

    if (value.is_array())
    {
        json out = json::array();

        for (const auto& item : value)
        {
            out.push_back(WithoutWeights(item));
        }

        return out;
    }

    return value;
}

std::string ShapeOf(const json& file, double size)
{
    return file.value("architecture", std::string()) + WithoutWeights(file.at("config")).dump() + Fixed(size);
}

void ApplySize(::nam::DSP& model, double size)
{
    if (auto* slimmable = dynamic_cast<::nam::SlimmableModel*>(&model))
    {
        slimmable->SetSlimmableSize(size);
    }
}

::nam::dspData DataFromJson(const json& file)
{
    ::nam::dspData data;
    data.version = file.at("version").get<std::string>();
    data.architecture = file.at("architecture").get<std::string>();
    data.config = file.at("config");
    data.metadata = file.value("metadata", json());
    data.weights = file.at("weights").get<std::vector<float>>();
    data.expected_sample_rate = ::nam::get_sample_rate_from_nam_file(file);
    return data;
}

std::unique_ptr<::nam::DSP> BuildFused(const json& file)
{
    const auto packed = fwn::Pack(DataFromJson(file));
    return packed ? fwn::Build(*packed) : nullptr;
}

// ---------------------------------------------------------------- synthetic models
json Layer(int inputSize, int channels, int headSize, json kernel, std::vector<int> dilations, json activation,
           int headKernel = 1, bool headBias = false)
{
    json layer = {{"input_size", inputSize}, {"condition_size", 1},      {"channels", channels},
                  {"dilations", dilations},  {"activation", activation}, {"gated", false}};
    layer[kernel.is_array() ? "kernel_sizes" : "kernel_size"] = kernel;

    if (headKernel == 1)
    {
        layer["head_size"] = headSize;
        layer["head_bias"] = headBias;
    }
    else
    {
        layer["head"] = {{"out_channels", headSize}, {"kernel_size", headKernel}, {"bias", headBias}};
    }

    return layer;
}

size_t CountWeights(const json& layers)
{
    size_t count = 1; // head scale

    for (const auto& l : layers)
    {
        const size_t c = l.at("channels").get<size_t>();
        const size_t dilations = l.at("dilations").size();
        count += l.at("input_size").get<size_t>() * c;

        for (size_t i = 0; i < dilations; ++i)
        {
            const size_t k =
                l.contains("kernel_sizes") ? l["kernel_sizes"][i].get<size_t>() : l["kernel_size"].get<size_t>();
            count += c * c * k + c + c + c * c + c;
        }

        const bool nested = l.contains("head");
        const size_t h = nested ? l["head"]["out_channels"].get<size_t>() : l["head_size"].get<size_t>();
        const size_t hk = nested ? l["head"]["kernel_size"].get<size_t>() : 1;
        const bool bias = nested ? l["head"]["bias"].get<bool>() : l["head_bias"].get<bool>();
        count += h * c * hk + (bias ? h : 0);
    }

    return count;
}

json WaveNetFile(const json& layers, unsigned seed, double sampleRate = kSampleRate)
{
    std::vector<float> weights(CountWeights(layers));

    for (auto& w : weights)
    {
        seed = seed * 1664525u + 1013904223u;
        w = static_cast<float>((double(seed >> 8) / double(1 << 24) - 0.5) * 0.4);
    }

    weights.back() = 0.05f; // head scale
    return {{"version", "0.5.4"},
            {"architecture", "WaveNet"},
            {"config", {{"layers", layers}, {"head", nullptr}, {"head_scale", 0.05}}},
            {"weights", weights},
            {"sample_rate", sampleRate},
            {"metadata", {{"loudness", -18.5}, {"input_level_dbu", 12.0}, {"output_level_dbu", 6.0}}}};
}

std::vector<int> Dilations(int count, int start = 1)
{
    std::vector<int> d;

    for (int i = 0; i < count; ++i)
    {
        d.push_back(start << (i % 10));
    }

    return d;
}

std::vector<std::pair<std::string, json>> SyntheticModels()
{
    const json tanh = "Tanh";
    const json leaky = {{"type", "LeakyReLU"}, {"negative_slope", 0.05}};
    std::vector<std::pair<std::string, json>> models;
    models.emplace_back("lite 12/6", WaveNetFile(json::array({Layer(1, 12, 6, 3, Dilations(10), tanh),
                                                              Layer(12, 6, 1, 3, Dilations(10), tanh, 1, true)}),
                                                 11));
    models.emplace_back("feather 8/4", WaveNetFile(json::array({Layer(1, 8, 4, 3, Dilations(10), tanh),
                                                                Layer(8, 4, 1, 3, Dilations(10), tanh, 1, true)}),
                                                   12));
    models.emplace_back(
        "four 8-channel arrays, kernels 6 and 15",
        WaveNetFile(
            json::array({Layer(1, 8, 8, 6, Dilations(6), tanh), Layer(8, 8, 8, 15, Dilations(4), tanh),
                         Layer(8, 8, 8, 6, Dilations(5, 2), tanh), Layer(8, 8, 1, 15, Dilations(3), tanh, 1, true)}),
            13));
    // Odd channels (tile tails), kernel-1 layers (no lookback), per-layer kernels and activations,
    // and a multi-tap head with bias between arrays.
    models.emplace_back("odd: 5 and 3 channels, kernel 1, mixed activations, 4-tap head",
                        WaveNetFile(json::array({Layer(1, 5, 3, json::array({1, 4, 2, 1}), {1, 3, 9, 27},
                                                       json::array({tanh, leaky, tanh, leaky}), 4, true),
                                                 Layer(5, 3, 1, 2, Dilations(7), leaky, 3, false)}),
                                    14));
    json nano = WaveNetFile(json::array({Layer(1, 3, 1, json::array({6, 15, 6}), {1, 2, 4}, leaky, 16, true)}), 15);
    json full = WaveNetFile(json::array({Layer(1, 8, 1, json::array({6, 15, 6}), {1, 2, 4}, leaky, 16, true)}), 16);
    models.emplace_back("container: 3- and 8-channel submodels",
                        json{{"version", "0.5.4"},
                             {"architecture", "SlimmableContainer"},
                             {"config",
                              {{"submodels", json::array({{{"max_value", 0.5}, {"model", nano}},
                                                          {{"max_value", 1.0}, {"model", full}}})}}},
                             {"weights", json::array()},
                             {"sample_rate", kSampleRate},
                             {"metadata", {{"loudness", -20.0}}}});
    return models;
}

std::vector<Subject> Subjects()
{
    std::vector<Subject> subjects;
    const fs::path root = fs::path(GUITARFX_TEST_RESOURCES_DIR) / "assets" / "amps";
    std::vector<fs::path> files;

    for (const auto& entry : fs::recursive_directory_iterator(root))
    {
        if (entry.is_regular_file() && entry.path().extension() == ".nam")
        {
            files.push_back(entry.path());
        }
    }

    std::sort(files.begin(), files.end());
    std::set<size_t> seen; // some assets are copies under awkward names, for path tests

    for (const auto& path : files)
    {
        std::ifstream stream(path, std::ios::binary);
        const std::string text((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());

        if (!seen.insert(std::hash<std::string>{}(text)).second)
        {
            continue;
        }

        const json file = json::parse(text);
        const bool container = file.value("architecture", std::string()) == "SlimmableContainer";
        // Parsed once: JSON parsing dominates a Debug run otherwise.
        const auto data = std::make_shared<const ::nam::dspData>(DataFromJson(file));

        for (const double size : container ? std::vector<double>{0.0, 1.0} : std::vector<double>{1.0})
        {
            const auto label = path.filename().u8string();
            subjects.push_back({std::string(label.begin(), label.end()) + (container ? " @" + Fixed(size) : ""),
                                ShapeOf(file, size),
                                [data, size] {
                                    ::nam::dspData copy = *data;
                                    auto model = ::nam::get_dsp(copy);
                                    ApplySize(*model, size);
                                    return model;
                                },
                                [path, size] {
                                    auto model = cache::GetModel(path);
                                    ApplySize(*model, size);
                                    return model;
                                }});
        }
    }

    for (const auto& [name, file] : SyntheticModels())
    {
        for (const double size :
             file["architecture"] == "SlimmableContainer" ? std::vector<double>{0.0, 1.0} : std::vector<double>{1.0})
        {
            subjects.push_back({name + (size < 1.0 ? " @0.0" : ""), ShapeOf(file, size),
                                [file, size] {
                                    auto model = ::nam::get_dsp(file);
                                    ApplySize(*model, size);
                                    return model;
                                },
                                [file, size] {
                                    auto model = BuildFused(file);
                                    ApplySize(*model, size);
                                    return model;
                                }});
        }
    }

    return subjects;
}

bool IsFused(const ::nam::DSP& model)
{
    return dynamic_cast<const fwn::FusedWaveNet*>(&model) != nullptr ||
           dynamic_cast<const fwn::FusedContainer*>(&model) != nullptr;
}

// ---------------------------------------------------------------- tests
void TestMatchesNamCore(const Subject& subject, const std::vector<double>& input)
{
    auto core = subject.core();
    auto fused = subject.fused();
    Check(fused && IsFused(*fused), subject.name + ": builds on the fused pass");

    if (!core || !fused || !IsFused(*fused))
    {
        return;
    }

    const auto reference = Render(*core, input, {64});
    const auto output = Render(*fused, input, {64});
    const double snr = SnrDb(reference, output);
    const double firstBlock = SnrDb(reference, output, 64);
    Check(snr >= kMinSnrDb && firstBlock >= kMinSnrDb,
          subject.name + ": " + Fixed(snr) + " dB from the NAM core (first block " + Fixed(firstBlock) + " dB)");

    Check(fused->GetExpectedSampleRate() == core->GetExpectedSampleRate() &&
              fused->HasLoudness() == core->HasLoudness() &&
              (!core->HasLoudness() || fused->GetLoudness() == core->GetLoudness()) &&
              fused->HasInputLevel() == core->HasInputLevel() && fused->GetInputLevel() == core->GetInputLevel() &&
              fused->HasOutputLevel() == core->HasOutputLevel() && fused->GetOutputLevel() == core->GetOutputLevel(),
          subject.name + ": same sample rate, loudness and levels as the NAM core's");

    // The NAM core's A2 fast path counts its prewarm without the WaveNet's extra sample.
    const int corePrewarm = PrewarmReader::Read(*core);
    const int fusedPrewarm = PrewarmReader::Read(*fused);
    Check(fusedPrewarm == corePrewarm || fusedPrewarm == corePrewarm + 1,
          subject.name + ": prewarm length " + std::to_string(fusedPrewarm) + " (NAM core " +
              std::to_string(corePrewarm) + ")");
}

void TestBlockInvariance(const Subject& subject, const std::vector<double>& input, int timeScale)
{
    auto fused = subject.fused();

    if (!fused)
    {
        return;
    }

    fused->SetTimeScale(timeScale);
    const std::vector<std::vector<int>> patterns = {{47}, {17, 256, 3, 100}, {512}, {13, 7}};
    const auto reference = Render(*fused, input, {64});
    bool same = true;

    for (const auto& pattern : patterns)
    {
        same = same && SameBits(reference, Render(*fused, input, pattern));
    }

    // One frame a call is slow, and a prefix shows any difference as well as the whole.
    const std::vector<double> prefix(input.begin(), input.begin() + static_cast<long>(input.size() / 8));
    const auto single = Render(*fused, prefix, {1});
    same = same && std::memcmp(single.data(), reference.data(), single.size() * sizeof(double)) == 0;

    Check(same, subject.name + ": bit-identical for blocks of 64, 47, 1, mixed and 512 (over the prepared 256)" +
                    (timeScale > 1 ? ", time scale " + std::to_string(timeScale) : std::string()));
}

fwn::FusedWaveNet* ActiveWaveNet(::nam::DSP& model)
{
    if (auto* container = dynamic_cast<fwn::FusedContainer*>(&model))
    {
        return &container->SubmodelAt(container->ActiveIndex());
    }

    return dynamic_cast<fwn::FusedWaveNet*>(&model);
}

void TestOnePassPrewarm(const Subject& subject, const std::vector<double>& input)
{
    for (const int scale : {1, 2})
    {
        auto onePass = subject.fused();
        auto running = subject.fused();

        if (!onePass || !running)
        {
            return;
        }

        onePass->SetTimeScale(scale);
        running->SetTimeScale(scale);
        onePass->Reset(kSampleRate, 64); // prewarms in one pass
        running->Reset(kSampleRate, 64);
        ActiveWaveNet(*running)->PrewarmByRunning();
        Check(SameBits(Run(*onePass, input, {64}), Run(*running, input, {64})),
              subject.name + ": one-pass prewarm leaves a real prewarm's state, time scale " + std::to_string(scale));
    }
}

void TestTimeScales(const Subject& subject, const std::vector<double>& input)
{
    for (const int scale : {2, 4})
    {
        auto core = subject.core();
        auto fused = subject.fused();

        if (!core || !fused)
        {
            return;
        }

        core->SetTimeScale(scale);
        fused->SetTimeScale(scale);
        const double snr = SnrDb(Render(*core, input, {64}), Render(*fused, input, {64}));
        Check(snr >= kMinSnrDb,
              subject.name + ": time scale " + std::to_string(scale) + ", " + Fixed(snr) + " dB from the NAM core");
    }
}

void TestDetectorRejects()
{
    std::cout << "\n[shape detector]\n";
    const json tanh = "Tanh";
    const json base =
        WaveNetFile(json::array({Layer(1, 4, 4, 3, {1, 2}, tanh), Layer(4, 4, 1, 3, {1, 2}, tanh, 1, true)}), 21);
    Check(fwn::Pack(DataFromJson(base)) != nullptr, "the unmodified model packs");

    auto reject = [&](const std::string& what, const std::function<void(json&)>& mutate) {
        json file = base;
        mutate(file);
        Check(fwn::Pack(DataFromJson(file)) == nullptr, "refuses " + what);
    };

    auto first = [](json& f) -> json& { return f["config"]["layers"][0]; };
    reject("a gated layer", [&](json& f) { first(f)["gated"] = true; });
    reject("a gating_mode other than none", [&](json& f) {
        first(f).erase("gated");
        first(f)["gating_mode"] = json::array({"none", "gated"});
    });
    reject("an active FiLM", [&](json& f) { first(f)["conv_pre_film"] = {{"active", true}}; });
    reject("a FiLM given as true", [&](json& f) { first(f)["activation_post_film"] = true; });
    reject("a null FiLM, which the NAM core refuses", [&](json& f) { first(f)["layer1x1_post_film"] = nullptr; });
    reject("an active head1x1",
           [&](json& f) { first(f)["head1x1"] = {{"active", true}, {"out_channels", 4}, {"groups", 1}}; });
    reject("grouped input convs", [&](json& f) { first(f)["groups_input"] = 2; });
    reject("a grouped input mixin", [&](json& f) { first(f)["groups_input_mixin"] = 2; });
    reject("a grouped layer1x1", [&](json& f) { first(f)["layer1x1"] = {{"active", true}, {"groups", 2}}; });
    reject("an inactive layer1x1", [&](json& f) { first(f)["layer1x1"] = {{"active", false}, {"groups", 1}}; });
    reject("a bottleneck", [&](json& f) { first(f)["bottleneck"] = 2; });
    reject("a condition DSP", [&](json& f) { f["config"]["condition_dsp"] = WaveNetFile(f["config"]["layers"], 1); });
    reject("a post-stack head", [&](json& f) {
        f["config"]["head"] = {{"channels", 4}, {"out_channels", 1}, {"kernel_sizes", {1}}, {"activation", "Tanh"}};
    });
    reject("a slimmable layer array", [&](json& f) { first(f)["slimmable"] = {{"method", "slice_channels_uniform"}}; });
    reject("another activation", [&](json& f) { first(f)["activation"] = "Sigmoid"; });
    reject("a LeakyReLU slope of 1 or more",
           [&](json& f) { first(f)["activation"] = {{"type", "LeakyReLU"}, {"negative_slope", 1.5}}; });
    reject("a zero dilation", [&](json& f) { first(f)["dilations"] = {0, 2}; });
    reject("both kernel_size and kernel_sizes", [&](json& f) { first(f)["kernel_sizes"] = {3, 3}; });
    reject("a condition size other than 1", [&](json& f) { first(f)["condition_size"] = 2; });
    reject("an input size that does not chain", [&](json& f) { f["config"]["layers"][1]["input_size"] = 3; });
    reject("a last head wider than one channel", [&](json& f) { f["config"]["layers"][1]["head_size"] = 2; });
    reject("one weight too few", [&](json& f) { f["weights"].erase(f["weights"].size() - 1); });
    reject("one weight too many", [&](json& f) { f["weights"].push_back(0.5f); });
    reject("an unsupported version", [&](json& f) { f["version"] = "9.0.0"; });
    reject("another architecture", [&](json& f) { f["architecture"] = "LSTM"; });

    // Containers: every submodel must fuse, in order, ending at or past 1.0.
    const json container = SyntheticModels().back().second;
    Check(fwn::Pack(DataFromJson(container)) != nullptr, "the synthetic container packs");
    auto rejectContainer = [&](const std::string& what, const std::function<void(json&)>& mutate) {
        json file = container;
        mutate(file);
        Check(fwn::Pack(DataFromJson(file)) == nullptr, "refuses a container with " + what);
    };
    rejectContainer("a submodel that does not fuse",
                    [](json& f) { f["config"]["submodels"][0]["model"]["config"]["layers"][0]["gated"] = true; });
    rejectContainer("unordered max values", [](json& f) { f["config"]["submodels"][0]["max_value"] = 1.0; });
    rejectContainer("a last max value under 1", [](json& f) { f["config"]["submodels"][1]["max_value"] = 0.9; });
    rejectContainer("a submodel at another sample rate",
                    [](json& f) { f["config"]["submodels"][0]["model"]["sample_rate"] = 44100.0; });
}

void TestCacheIntegration(const fs::path& modelPath, const std::vector<double>& input)
{
    std::cout << "\n[model cache]\n";
    cache::SetFusedEngineEnabled(true);
    cache::Clear();
    cache::ResetStats();

    auto first = cache::GetModel(modelPath);
    const auto start = std::chrono::steady_clock::now();
    auto second = cache::GetModel(modelPath);
    const double hitMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    std::cout << "  (a cache hit built a fused model in " << hitMs << " ms)" << std::endl;

    auto* a = first ? ActiveWaveNet(*first) : nullptr;
    auto* b = second ? ActiveWaveNet(*second) : nullptr;
    Check(a != nullptr && b != nullptr, "the cache builds fused models");

    if (a == nullptr || b == nullptr)
    {
        return;
    }

    Check(a->SharedWeights() == b->SharedWeights(), "two models from one file share their packed weights");
    Check(cache::GetStats().fusedBuilds == 2 && cache::GetStats().coreBuilds == 0, "both counted as fused builds");
    Check(SameBits(Render(*first, input, {64}), Render(*second, input, {64})),
          "a cache hit renders bit-identically to the first load");

    cache::SetFusedEngineEnabled(false);
    auto core = cache::GetModel(modelPath);
    auto direct = ::nam::get_dsp(modelPath);
    Check(core && !IsFused(*core), "switched off, the cache builds the NAM core's model");
    Check(cache::GetStats().coreBuilds == 1, "and counts it as a NAM core build");

    if (core && direct)
    {
        Check(SameBits(Render(*core, input, {256}), Render(*direct, input, {256})),
              "which matches an uncached get_dsp(path) model bit for bit");
    }

    cache::SetFusedEngineEnabled(true);
    auto again = cache::GetModel(modelPath);
    Check(again && IsFused(*again), "switched back on, the next load is fused again");
}
} // namespace

int main()
{
    nam::factory::ForceFactoryRegistration();
    // The app's NAM core runs fast tanh (NamModelCache switches it on), and so does the fused pass.
    ::nam::activations::Activation::enable_fast_tanh();
    std::cout << "Fused WaveNet kernel: " << fwn::KernelName() << std::endl;

    if (!fwn::IsAvailable())
    {
        std::cout << "FusedWaveNetTests skipped: no fused kernel in this build" << std::endl;
        return 0;
    }

    cache::SetFusedEngineEnabled(true);
    const auto input = MakeInput(static_cast<int>(kSampleRate)); // 1 s
    const std::vector<double> shortInput(input.begin(), input.begin() + static_cast<long>(kSampleRate / 2));
    const auto subjects = Subjects();
    std::set<std::string> shapes;

    for (const auto& subject : subjects)
    {
        std::cout << "\n[" << subject.name << "]\n";
        TestMatchesNamCore(subject, input);

        // The rest depends on the shape, not the weights: once per shape.
        if (shapes.insert(subject.shape).second)
        {
            TestBlockInvariance(subject, shortInput, 1);
            TestOnePassPrewarm(subject, shortInput);
            TestTimeScales(subject, shortInput);
        }
    }

    std::cout << "\n" << subjects.size() << " models, " << shapes.size() << " shapes" << std::endl;

    // Oversampled block invariance, on one standard model and one container.
    std::cout << "\n[time-scaled block invariance]\n";
    TestBlockInvariance(subjects.front(), shortInput, 2);
    TestBlockInvariance(subjects.back(), shortInput, 4);

    TestDetectorRejects();
    TestCacheIntegration(fs::path(GUITARFX_TEST_RESOURCES_DIR) / "assets" / "amps" / "Guitar" / "A2" /
                             "Boosted-6505+-A2-Chugs-FullRig.nam",
                         shortInput);

    std::cout << (gAllPassed ? "\nFusedWaveNetTests passed" : "\nFusedWaveNetTests FAILED") << std::endl;
    return gAllPassed ? 0 : 1;
}

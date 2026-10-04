/**
 * @file TransposeExternalRenderTool.cpp
 * @brief Offline renderer that automates external plugins' transpose parameter
 *        for the transpose benchmark.
 *
 * For each plugin configured with a "transposeParameter" block in
 * tools/transpose-benchmark/external-plugins.json this tool:
 *   - hosts the plugin via JuceHostedPluginEffect (same code path as the app),
 *   - optionally applies a base plugin state (stateBase64),
 *   - optionally applies fixed parameterOverrides (e.g. disable Archetype
 *     amp/cab/FX sections so only Transpose remains for a fair comparison),
 *   - sets the plugin's transpose parameter for each requested semitone value
 *     (mapping: "text" via getValueForText, "linear" over a declared semitone
 *     range, or "explicit" normalized values per semitone),
 *   - renders the same trimmed demo audio the TransposeBenchmark uses,
 *     compensated by the plugin's reported latency,
 *   - writes WAVs under <snapshotDir>/external/ and a render manifest
 *     <snapshotDir>/external-renders-<pluginId>.json compatible with
 *     tools/transpose-benchmark/build_external_passes.py.
 *
 * Usage:
 *   TransposeExternalRender [--list-params] [--all-demo-audio]
 *                           [--plugin <pluginId>] [--collection <path>]
 *                           [snapshotDir]
 *     --list-params    print each configured plugin's parameters (index, name,
 *                      current value/text) and exit; use this to author the
 *                      transposeParameter config.
 *     --all-demo-audio render all demo inputs (default: first riff only, to
 *                      match the TransposeBenchmark default).
 *     --plugin <id>    only process the plugin with this pluginId.
 *     --collection     path to external-plugins.json (default: found by
 *                      walking up from cwd to tools/transpose-benchmark/).
 *     snapshotDir      an existing TransposeBenchmark snapshot directory
 *                      (required unless --list-params).
 *
 * Typical workflow:
 *   1. TransposeBenchmark.exe testing\transpose-benchmark <label>
 *   2. TransposeExternalRender.exe testing\transpose-benchmark\<label>
 *   3. python tools/transpose-benchmark/build_external_passes.py
 *        testing/transpose-benchmark/<label>
 *        testing/transpose-benchmark/<label>/external-renders-<pluginId>.json
 *   4. python tools/transpose-benchmark/generate_report.py testing/transpose-benchmark
 */

#include "JuceHostedPluginEffect.h"

#include "util/AudioDecoder.h"

#include <juce_events/juce_events.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#ifndef GUITARFX_DEMO_AUDIO_DIR
#error "GUITARFX_DEMO_AUDIO_DIR must be defined"
#endif

namespace fs = std::filesystem;

namespace
{
constexpr int kBlockSize = 512;
constexpr double kMaxRenderSeconds = 12.0;
constexpr int kFlushPadFrames = 8192;

// Keep in sync with core/tests/TransposeBenchmark.cpp so external renders line
// up with the dry references and built-in variant passes.
const std::vector<int> kDefaultSemitones = {-12, -7, -5, -3, -2, -1, 0, 2, 5, 7, 12};

// The demo clips (core/ui/demo): WAVs and, for the long DI take, an MP3; LoadDemoSample decodes either.
const std::vector<std::string> kDemoSamples = {
    "guitar-riff-01.wav",
    "guitar-riff-02.wav",
    "DI_Guitar_L.mp3",
};

const fs::path kCollectionRelPath = fs::path("tools") / "transpose-benchmark" / "external-plugins.json";

struct ParameterAutomation
{
    std::string name;                       // case-insensitive parameter name match
    int index = -1;                         // explicit parameter index (overrides name)
    std::string mapping = "text";           // "text" | "linear" | "explicit"
    double minSemitones = -12.0;            // for "linear"
    double maxSemitones = 12.0;             // for "linear"
    std::map<int, double> valueBySemitone;  // for "explicit" (normalized 0..1)
};

// Fixed parameter values applied once after load (e.g. turn off amp/cab/FX).
struct ParameterOverride
{
    std::string name;                       // case-insensitive parameter name match
    int index = -1;                         // explicit parameter index (overrides name)
    bool hasValue = false;
    float value = 0.0f;                     // normalized 0..1
    std::string text;                       // optional: resolve via getValueForText when no value
};

struct ExternalPluginJob
{
    std::string pluginId;
    std::string label;
    std::string pluginPath;
    std::string stateBase64;
    std::vector<int> semitones = kDefaultSemitones;
    std::optional<ParameterAutomation> transposeParam;
    std::vector<ParameterOverride> parameterOverrides;
};

struct StereoAudio
{
    double sampleRate = 0.0;
    std::vector<float> left;
    std::vector<float> right;

    [[nodiscard]] size_t frames() const { return left.size(); }
};

std::vector<std::uint8_t> ReadBinaryFile(const fs::path& path)
{
    std::ifstream input(path, std::ios::binary);
    if (!input)
        return {};
    return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(input),
                                     std::istreambuf_iterator<char>());
}

std::optional<StereoAudio> LoadDemoSample(const fs::path& path)
{
    const auto bytes = ReadBinaryFile(path);
    if (bytes.empty())
        return std::nullopt;

    const auto decoded = guitarfx::util::DecodeAudioBytes(bytes);
    if (!decoded || decoded->channelSamples.empty() || decoded->sampleRate <= 0.0)
        return std::nullopt;

    StereoAudio audio;
    audio.sampleRate = decoded->sampleRate;

    const size_t maxFrames = static_cast<size_t>(kMaxRenderSeconds * decoded->sampleRate);
    const auto& srcL = decoded->channelSamples[0];
    const auto& srcR = decoded->channelSamples.size() > 1 ? decoded->channelSamples[1]
                                                          : decoded->channelSamples[0];
    const size_t frames = std::min(std::min(srcL.size(), srcR.size()), maxFrames);

    audio.left.resize(frames);
    audio.right.resize(frames);
    for (size_t i = 0; i < frames; ++i)
    {
        audio.left[i] = static_cast<float>(srcL[i]);
        audio.right[i] = static_cast<float>(srcR[i]);
    }
    return audio;
}

bool WriteWav16(const fs::path& path, const StereoAudio& audio)
{
    std::ofstream out(path, std::ios::binary);
    if (!out)
        return false;

    const std::uint32_t frames = static_cast<std::uint32_t>(audio.frames());
    const std::uint16_t channels = 2;
    const std::uint16_t bitsPerSample = 16;
    const std::uint32_t sampleRate = static_cast<std::uint32_t>(std::lround(audio.sampleRate));
    const std::uint32_t byteRate = sampleRate * channels * (bitsPerSample / 8);
    const std::uint16_t blockAlign = channels * (bitsPerSample / 8);
    const std::uint32_t dataSize = frames * blockAlign;
    const std::uint32_t riffSize = 36 + dataSize;

    auto write32 = [&](std::uint32_t v) { out.write(reinterpret_cast<const char*>(&v), 4); };
    auto write16 = [&](std::uint16_t v) { out.write(reinterpret_cast<const char*>(&v), 2); };

    out.write("RIFF", 4);
    write32(riffSize);
    out.write("WAVE", 4);
    out.write("fmt ", 4);
    write32(16);
    write16(1); // PCM
    write16(channels);
    write32(sampleRate);
    write32(byteRate);
    write16(blockAlign);
    write16(bitsPerSample);
    out.write("data", 4);
    write32(dataSize);

    for (std::uint32_t i = 0; i < frames; ++i)
    {
        const auto clampToInt16 = [](float v)
        {
            const float clamped = std::clamp(v, -1.0f, 1.0f);
            return static_cast<std::int16_t>(std::lround(clamped * 32767.0f));
        };
        const std::int16_t l = clampToInt16(audio.left[i]);
        const std::int16_t r = clampToInt16(audio.right[i]);
        out.write(reinterpret_cast<const char*>(&l), 2);
        out.write(reinterpret_cast<const char*>(&r), 2);
    }
    return static_cast<bool>(out);
}

std::string SanitizeForFilename(std::string value)
{
    for (char& c : value)
    {
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '-' && c != '_')
            c = '_';
    }
    return value;
}

std::optional<fs::path> FindCollectionPath()
{
    fs::path current = fs::current_path();
    for (int i = 0; i < 6; ++i)
    {
        const fs::path candidate = current / kCollectionRelPath;
        if (fs::exists(candidate))
            return candidate;

        const fs::path parent = current.parent_path();
        if (parent == current)
            break;
        current = parent;
    }
    return std::nullopt;
}

std::vector<ExternalPluginJob> ParseCollection(const fs::path& path)
{
    std::vector<ExternalPluginJob> jobs;

    std::ifstream input(path);
    if (!input)
    {
        std::cerr << "ERROR: cannot open plugin collection: " << path << '\n';
        return jobs;
    }

    nlohmann::json parsed;
    try
    {
        input >> parsed;
    }
    catch (const std::exception& e)
    {
        std::cerr << "ERROR: cannot parse plugin collection " << path << ": " << e.what() << '\n';
        return jobs;
    }

    const auto pluginsIt = parsed.find("plugins");
    if (pluginsIt == parsed.end() || !pluginsIt->is_array())
    {
        std::cerr << "ERROR: plugin collection has no 'plugins' array: " << path << '\n';
        return jobs;
    }

    for (const auto& row : *pluginsIt)
    {
        if (!row.is_object())
            continue;

        ExternalPluginJob job;
        job.pluginId = row.value("pluginId", std::string{});
        job.label = row.value("pluginLabel", job.pluginId);
        job.pluginPath = row.value("pluginPath", std::string{});
        if (job.pluginId.empty() || job.pluginPath.empty())
        {
            std::cerr << "WARN: skipping plugin entry without pluginId/pluginPath\n";
            continue;
        }

        job.stateBase64 = row.value("stateBase64", std::string{});

        if (const auto semitonesIt = row.find("semitones");
            semitonesIt != row.end() && semitonesIt->is_array())
        {
            job.semitones.clear();
            for (const auto& v : *semitonesIt)
            {
                if (v.is_number_integer())
                    job.semitones.push_back(v.get<int>());
            }
        }

        if (const auto paramIt = row.find("transposeParameter");
            paramIt != row.end() && paramIt->is_object())
        {
            ParameterAutomation automation;
            automation.name = paramIt->value("name", std::string{});
            automation.index = paramIt->value("index", -1);
            automation.mapping = paramIt->value("mapping", std::string{"text"});
            automation.minSemitones = paramIt->value("minSemitones", -12.0);
            automation.maxSemitones = paramIt->value("maxSemitones", 12.0);

            if (const auto mapIt = paramIt->find("valueBySemitone");
                mapIt != paramIt->end() && mapIt->is_object())
            {
                for (auto it = mapIt->begin(); it != mapIt->end(); ++it)
                {
                    if (!it.value().is_number())
                        continue;
                    try
                    {
                        automation.valueBySemitone[std::stoi(it.key())] = it.value().get<double>();
                    }
                    catch (...)
                    {
                    }
                }
            }

            if (automation.mapping != "text" && automation.mapping != "linear"
                && automation.mapping != "explicit")
            {
                std::cerr << "WARN: plugin '" << job.pluginId << "' has unknown transposeParameter.mapping '"
                          << automation.mapping << "'; expected text|linear|explicit. Skipping automation.\n";
            }
            else if (automation.index < 0 && automation.name.empty())
            {
                std::cerr << "WARN: plugin '" << job.pluginId
                          << "' transposeParameter needs a 'name' or 'index'. Skipping automation.\n";
            }
            else
            {
                job.transposeParam = std::move(automation);
            }
        }

        if (const auto overridesIt = row.find("parameterOverrides");
            overridesIt != row.end() && overridesIt->is_array())
        {
            for (const auto& item : *overridesIt)
            {
                if (!item.is_object())
                    continue;

                ParameterOverride override;
                override.name = item.value("name", std::string{});
                override.index = item.value("index", -1);
                override.text = item.value("text", std::string{});

                if (const auto valueIt = item.find("value");
                    valueIt != item.end() && valueIt->is_number())
                {
                    override.hasValue = true;
                    override.value = static_cast<float>(valueIt->get<double>());
                }

                if (override.index < 0 && override.name.empty())
                {
                    std::cerr << "WARN: plugin '" << job.pluginId
                              << "' parameterOverride needs a 'name' or 'index'; skipping entry\n";
                    continue;
                }
                if (!override.hasValue && override.text.empty())
                {
                    std::cerr << "WARN: plugin '" << job.pluginId
                              << "' parameterOverride needs a 'value' or 'text'; skipping entry\n";
                    continue;
                }

                job.parameterOverrides.push_back(std::move(override));
            }
        }

        jobs.push_back(std::move(job));
    }

    return jobs;
}

juce::AudioProcessorParameter* ResolveParameterByNameOrIndex(juce::AudioPluginInstance& plugin,
                                                             int index,
                                                             const std::string& name,
                                                             std::string& outName,
                                                             const char* contextLabel)
{
    const auto& parameters = plugin.getParameters();

    if (index >= 0)
    {
        if (index >= static_cast<int>(parameters.size()))
        {
            std::cerr << "  ERROR: " << contextLabel << ".index " << index << " out of range (plugin has "
                      << parameters.size() << " parameters)\n";
            return nullptr;
        }
        auto* parameter = parameters[static_cast<size_t>(index)];
        if (parameter != nullptr)
            outName = parameter->getName(512).toStdString();
        return parameter;
    }

    const juce::String needle(name);

    // Prefer an exact (case-insensitive) name match, then fall back to substring.
    for (auto* parameter : parameters)
    {
        if (parameter != nullptr && parameter->getName(512).equalsIgnoreCase(needle))
        {
            outName = parameter->getName(512).toStdString();
            return parameter;
        }
    }
    for (auto* parameter : parameters)
    {
        if (parameter != nullptr && parameter->getName(512).containsIgnoreCase(needle))
        {
            outName = parameter->getName(512).toStdString();
            return parameter;
        }
    }

    std::cerr << "  ERROR: no parameter matching '" << name
              << "' found; run with --list-params to inspect the plugin\n";
    return nullptr;
}

juce::AudioProcessorParameter* ResolveParameter(juce::AudioPluginInstance& plugin,
                                                const ParameterAutomation& cfg,
                                                std::string& outName)
{
    return ResolveParameterByNameOrIndex(plugin, cfg.index, cfg.name, outName, "transposeParameter");
}

int ApplyParameterOverrides(juce::AudioPluginInstance& plugin,
                            const std::vector<ParameterOverride>& overrides)
{
    int applied = 0;
    for (const auto& override : overrides)
    {
        std::string resolvedName;
        auto* parameter = ResolveParameterByNameOrIndex(plugin, override.index, override.name,
                                                        resolvedName, "parameterOverride");
        if (parameter == nullptr)
            continue;

        float normalized = 0.0f;
        if (override.hasValue)
        {
            normalized = std::clamp(override.value, 0.0f, 1.0f);
        }
        else
        {
            normalized = parameter->getValueForText(juce::String(override.text));
        }

        parameter->setValueNotifyingHost(normalized);
        const std::string resultingText =
            parameter->getText(parameter->getValue(), 128).toStdString();
        std::cout << "  override: [" << parameter->getParameterIndex() << "] \"" << resolvedName
                  << "\" = " << normalized << " (\"" << resultingText << "\")\n";
        ++applied;
    }
    return applied;
}

std::optional<float> NormalizedValueForSemitones(juce::AudioProcessorParameter& parameter,
                                                 const ParameterAutomation& cfg,
                                                 int semitones)
{
    if (cfg.mapping == "explicit")
    {
        const auto it = cfg.valueBySemitone.find(semitones);
        if (it == cfg.valueBySemitone.end())
        {
            std::cerr << "  WARN: no explicit value for " << semitones << " st; skipping\n";
            return std::nullopt;
        }
        return static_cast<float>(std::clamp(it->second, 0.0, 1.0));
    }

    if (cfg.mapping == "linear")
    {
        if (semitones < cfg.minSemitones || semitones > cfg.maxSemitones)
        {
            std::cerr << "  WARN: " << semitones << " st outside declared range ["
                      << cfg.minSemitones << ", " << cfg.maxSemitones << "]; skipping\n";
            return std::nullopt;
        }
        const double span = cfg.maxSemitones - cfg.minSemitones;
        if (span <= 0.0)
        {
            std::cerr << "  ERROR: invalid linear range (min >= max); skipping\n";
            return std::nullopt;
        }
        return static_cast<float>(std::clamp((semitones - cfg.minSemitones) / span, 0.0, 1.0));
    }

    // "text": ask the plugin to convert display text to a normalized value.
    return parameter.getValueForText(juce::String(semitones));
}

struct RenderStats
{
    int reportedLatencySamples = 0;
    double processMs = 0.0;
    double audioMs = 0.0;
    double realtimeFactor = 0.0;
    double avgBlockUs = 0.0;
    double maxBlockUs = 0.0;
};

// Commercial plugins commonly finish initialization (and licensing checks)
// via timers/async updaters on the message thread; without a running dispatch
// loop they can output silence. Pump the loop the way a real host would.
void PumpMessageLoop(int milliseconds)
{
    juce::MessageManager::getInstance()->runDispatchLoopUntil(milliseconds);
}

std::optional<RenderStats> RenderPass(guitarfx::JuceHostedPluginEffect& effect,
                                      const StereoAudio& input,
                                      StereoAudio& outRender)
{
    const int reportedLatency = effect.GetLatencySamples();
    const size_t inputFrames = input.frames();
    const size_t flushFrames = static_cast<size_t>(std::max(reportedLatency, 0)) + kFlushPadFrames;
    const size_t totalFrames = inputFrames + flushFrames;

    StereoAudio raw;
    raw.sampleRate = input.sampleRate;
    raw.left.assign(totalFrames, 0.0f);
    raw.right.assign(totalFrames, 0.0f);

    std::vector<float> inL(kBlockSize, 0.0f);
    std::vector<float> inR(kBlockSize, 0.0f);
    std::vector<float> outL(kBlockSize, 0.0f);
    std::vector<float> outR(kBlockSize, 0.0f);
    float* inputs[2] = {inL.data(), inR.data()};
    float* outputs[2] = {outL.data(), outR.data()};

    using Clock = std::chrono::steady_clock;
    std::chrono::nanoseconds totalNs{0};
    std::chrono::nanoseconds maxBlockNs{0};
    size_t timedBlocks = 0;

    for (size_t pos = 0; pos < totalFrames; pos += kBlockSize)
    {
        // Let plugin message-thread work (timers, async init) run between blocks.
        if (timedBlocks % 32 == 0)
            PumpMessageLoop(1);

        const int blockFrames = static_cast<int>(std::min<size_t>(kBlockSize, totalFrames - pos));
        for (int i = 0; i < blockFrames; ++i)
        {
            const size_t idx = pos + static_cast<size_t>(i);
            inL[static_cast<size_t>(i)] = idx < inputFrames ? input.left[idx] : 0.0f;
            inR[static_cast<size_t>(i)] = idx < inputFrames ? input.right[idx] : 0.0f;
        }

        const auto start = Clock::now();
        effect.Process(inputs, outputs, blockFrames);
        const auto elapsed = Clock::now() - start;

        if (timedBlocks > 0)
            maxBlockNs = std::max(maxBlockNs, std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed));
        totalNs += std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed);
        ++timedBlocks;

        for (int i = 0; i < blockFrames; ++i)
        {
            raw.left[pos + static_cast<size_t>(i)] = outL[static_cast<size_t>(i)];
            raw.right[pos + static_cast<size_t>(i)] = outR[static_cast<size_t>(i)];
        }
    }

    // Compensate by the reported latency, mirroring TransposeBenchmark, so any
    // PDC misreport by the plugin remains measurable/audible in the report.
    outRender.sampleRate = input.sampleRate;
    outRender.left.assign(inputFrames, 0.0f);
    outRender.right.assign(inputFrames, 0.0f);
    const size_t offset = static_cast<size_t>(std::max(reportedLatency, 0));
    for (size_t i = 0; i < inputFrames && (i + offset) < totalFrames; ++i)
    {
        outRender.left[i] = raw.left[i + offset];
        outRender.right[i] = raw.right[i + offset];
    }

    // Guard against silent renders (plugin bypassed, wrong bus layout, ...).
    double peak = 0.0;
    for (size_t i = 0; i < inputFrames; ++i)
        peak = std::max({peak,
                         std::abs(static_cast<double>(outRender.left[i])),
                         std::abs(static_cast<double>(outRender.right[i]))});
    if (peak < 1.0e-6)
    {
        std::cerr << "  ERROR: render is silent (peak < -120 dB); check plugin state/bus configuration\n";
        return std::nullopt;
    }

    RenderStats stats;
    stats.reportedLatencySamples = reportedLatency;
    stats.audioMs = 1000.0 * static_cast<double>(totalFrames) / input.sampleRate;
    stats.processMs = std::chrono::duration<double, std::milli>(totalNs).count();
    stats.realtimeFactor = stats.processMs > 0.0 ? stats.audioMs / stats.processMs : 0.0;
    stats.avgBlockUs = timedBlocks > 0
        ? std::chrono::duration<double, std::micro>(totalNs).count() / static_cast<double>(timedBlocks)
        : 0.0;
    stats.maxBlockUs = std::chrono::duration<double, std::micro>(maxBlockNs).count();
    return stats;
}

void PrintPluginParameters(const ExternalPluginJob& job, juce::AudioPluginInstance& plugin)
{
    std::cout << "\nPlugin: " << job.label << " (" << job.pluginId << ")\n"
              << "  path: " << job.pluginPath << '\n'
              << "  parameters (" << plugin.getParameters().size() << ", automatable only):\n";

    const auto& parameters = plugin.getParameters();
    int skipped = 0;
    for (int index = 0; index < static_cast<int>(parameters.size()); ++index)
    {
        auto* parameter = parameters[static_cast<size_t>(index)];
        if (parameter == nullptr)
            continue;

        if (!parameter->isAutomatable())
        {
            ++skipped;
            continue;
        }

        std::cout << "    [" << index << "] \"" << parameter->getName(512).toStdString() << "\""
                  << " value=" << parameter->getValue()
                  << " text=\"" << parameter->getText(parameter->getValue(), 128).toStdString() << "\""
                  << '\n';
    }

    if (skipped > 0)
        std::cout << "  (" << skipped << " non-automatable parameter(s) hidden)\n";
}

void PrintUsage(const char* argv0)
{
    std::cout << "Usage: " << argv0
              << " [--list-params] [--all-demo-audio] [--plugin <pluginId>] [--collection <path>] [snapshotDir]\n"
              << "  --list-params     list each configured plugin's parameters and exit\n"
              << "  --all-demo-audio  render all demo inputs (default: first riff only)\n"
              << "  --plugin <id>     only process the plugin with this pluginId\n"
              << "  --collection      path to external-plugins.json\n"
              << "  snapshotDir       TransposeBenchmark snapshot directory (required unless --list-params)\n";
}
} // namespace

int main(int argc, char** argv)
{
    bool listParams = false;
    bool renderAllDemoAudio = false;
    std::string pluginFilter;
    fs::path collectionPath;
    std::vector<std::string> positionalArgs;

    for (int i = 1; i < argc; ++i)
    {
        const std::string arg = argv[i];
        if (arg == "--list-params")
        {
            listParams = true;
        }
        else if (arg == "--all-demo-audio")
        {
            renderAllDemoAudio = true;
        }
        else if (arg == "--plugin")
        {
            if (i + 1 >= argc)
            {
                std::cerr << "ERROR: --plugin requires a pluginId argument\n";
                return 1;
            }
            pluginFilter = argv[++i];
        }
        else if (arg == "--collection")
        {
            if (i + 1 >= argc)
            {
                std::cerr << "ERROR: --collection requires a path argument\n";
                return 1;
            }
            collectionPath = fs::path(argv[++i]);
        }
        else if (arg == "-h" || arg == "--help")
        {
            PrintUsage(argv[0]);
            return 0;
        }
        else if (!arg.empty() && arg[0] == '-')
        {
            std::cerr << "ERROR: unknown option: " << arg << '\n';
            PrintUsage(argv[0]);
            return 1;
        }
        else
        {
            positionalArgs.push_back(arg);
        }
    }

    if (positionalArgs.size() > 1)
    {
        std::cerr << "ERROR: too many positional arguments\n";
        return 1;
    }

    fs::path snapshotDir;
    if (!listParams)
    {
        if (positionalArgs.empty())
        {
            std::cerr << "ERROR: snapshotDir is required (or use --list-params)\n";
            PrintUsage(argv[0]);
            return 1;
        }
        snapshotDir = fs::path(positionalArgs[0]);
        if (!fs::is_directory(snapshotDir))
        {
            std::cerr << "ERROR: snapshot directory does not exist: " << snapshotDir << '\n';
            return 1;
        }
        if (!fs::exists(snapshotDir / "results.json"))
        {
            std::cerr << "WARN: " << (snapshotDir / "results.json")
                      << " not found; build_external_passes.py needs a TransposeBenchmark snapshot with dry references\n";
        }
    }

    if (collectionPath.empty())
    {
        if (const auto found = FindCollectionPath())
        {
            collectionPath = *found;
        }
        else
        {
            std::cerr << "ERROR: cannot find " << kCollectionRelPath.generic_string()
                      << " from the current directory; use --collection\n";
            return 1;
        }
    }

    std::vector<ExternalPluginJob> jobs = ParseCollection(collectionPath);
    if (!pluginFilter.empty())
    {
        std::erase_if(jobs, [&](const ExternalPluginJob& job) { return job.pluginId != pluginFilter; });
        if (jobs.empty())
        {
            std::cerr << "ERROR: no plugin with pluginId '" << pluginFilter << "' in " << collectionPath << '\n';
            return 1;
        }
    }
    if (jobs.empty())
    {
        std::cerr << "ERROR: no usable plugin entries in " << collectionPath << '\n';
        return 1;
    }

    juce::ScopedJuceInitialiser_GUI juceInitialiser;

    const fs::path demoDir(GUITARFX_DEMO_AUDIO_DIR);
    std::vector<std::string> demoSamplesToRender;
    if (renderAllDemoAudio)
        demoSamplesToRender = kDemoSamples;
    else
        demoSamplesToRender.push_back(kDemoSamples.front());

    int failures = 0;
    int rendered = 0;
    std::vector<fs::path> manifestsWritten;

    for (const auto& job : jobs)
    {
        std::cout << "\n=== " << job.label << " (" << job.pluginId << ") ===\n";

        if (!fs::exists(fs::path(job.pluginPath)))
        {
            std::cerr << "WARN: plugin not installed, skipping: " << job.pluginPath << '\n';
            continue;
        }

        guitarfx::JuceHostedPluginEffect effect;
        effect.SetParam("mix", 1.0);
        effect.Prepare(48000.0, kBlockSize);
        if (!effect.LoadResource(fs::path(job.pluginPath)))
        {
            std::cerr << "ERROR: failed to load plugin: " << effect.GetConfig("lastError") << '\n';
            ++failures;
            continue;
        }

        if (!job.stateBase64.empty())
            effect.SetConfig("pluginStateBase64", job.stateBase64);

        // Give the plugin time to complete message-thread initialization.
        PumpMessageLoop(1500);

        auto* plugin = effect.GetHostedPluginForTesting();
        if (plugin == nullptr)
        {
            std::cerr << "ERROR: plugin instance unavailable after load\n";
            ++failures;
            continue;
        }

        if (!job.parameterOverrides.empty())
        {
            std::cout << "  applying " << job.parameterOverrides.size() << " parameter override(s)...\n";
            const int applied = ApplyParameterOverrides(*plugin, job.parameterOverrides);
            PumpMessageLoop(100);
            std::cout << "  applied " << applied << "/" << job.parameterOverrides.size()
                      << " parameter override(s)\n";
        }

        if (listParams)
        {
            PrintPluginParameters(job, *plugin);
            continue;
        }

        if (!job.transposeParam.has_value())
        {
            std::cerr << "WARN: plugin '" << job.pluginId
                      << "' has no transposeParameter config; skipping (add one, using --list-params to find the parameter)\n";
            continue;
        }

        std::string parameterName;
        auto* parameter = ResolveParameter(*plugin, *job.transposeParam, parameterName);
        if (parameter == nullptr)
        {
            ++failures;
            continue;
        }
        std::cout << "  automating parameter [" << parameter->getParameterIndex() << "] \""
                  << parameterName << "\" (mapping: " << job.transposeParam->mapping << ")\n";

        nlohmann::json manifest;
        manifest["pluginId"] = job.pluginId;
        manifest["pluginLabel"] = job.label;
        manifest["pluginPath"] = job.pluginPath;
        manifest["automatedParameter"] = parameterName;
        manifest["entries"] = nlohmann::json::array();

        const fs::path externalDir = snapshotDir / "external";
        std::error_code ec;
        fs::create_directories(externalDir, ec);
        if (ec)
        {
            std::cerr << "ERROR: cannot create " << externalDir << ": " << ec.message() << '\n';
            ++failures;
            continue;
        }

        for (const auto& sampleName : demoSamplesToRender)
        {
            const auto audio = LoadDemoSample(demoDir / sampleName);
            if (!audio)
            {
                std::cerr << "WARN: skipping demo sample (missing or undecodable): "
                          << (demoDir / sampleName) << '\n';
                continue;
            }

            effect.Prepare(audio->sampleRate, kBlockSize);
            PumpMessageLoop(200);

            // Warm the plugin up with real audio so async DSP setup completes
            // before the first measured render.
            {
                std::vector<float> wl(kBlockSize), wr(kBlockSize), ol(kBlockSize), or_(kBlockSize);
                float* wIn[2] = {wl.data(), wr.data()};
                float* wOut[2] = {ol.data(), or_.data()};
                const size_t warmupFrames = std::min<size_t>(audio->frames(),
                    static_cast<size_t>(audio->sampleRate));
                for (size_t pos = 0; pos + kBlockSize <= warmupFrames; pos += kBlockSize)
                {
                    for (int i = 0; i < kBlockSize; ++i)
                    {
                        wl[static_cast<size_t>(i)] = audio->left[pos + static_cast<size_t>(i)];
                        wr[static_cast<size_t>(i)] = audio->right[pos + static_cast<size_t>(i)];
                    }
                    effect.Process(wIn, wOut, kBlockSize);
                    if ((pos / kBlockSize) % 16 == 0)
                        PumpMessageLoop(1);
                }
            }

            const std::string sampleStem = SanitizeForFilename(fs::path(sampleName).stem().string());

            for (const int semitones : job.semitones)
            {
                std::cout << "  " << sampleName << " @ " << semitones << " st ... " << std::flush;

                const auto normalized = NormalizedValueForSemitones(*parameter, *job.transposeParam, semitones);
                if (!normalized.has_value())
                    continue;

                parameter->setValueNotifyingHost(*normalized);
                const std::string resultingText =
                    parameter->getText(parameter->getValue(), 128).toStdString();
                PumpMessageLoop(50);
                effect.Reset();

                StereoAudio render;
                const auto stats = RenderPass(effect, *audio, render);
                if (!stats)
                {
                    ++failures;
                    continue;
                }

                const std::string wavFile = SanitizeForFilename(job.pluginId) + "_" + sampleStem + "_"
                    + (semitones < 0 ? "m" : "p") + std::to_string(std::abs(semitones)) + "st.wav";
                if (!WriteWav16(externalDir / wavFile, render))
                {
                    std::cerr << "ERROR: cannot write " << (externalDir / wavFile) << '\n';
                    ++failures;
                    continue;
                }

                nlohmann::json entry;
                entry["sample"] = sampleName;
                entry["semitones"] = semitones;
                entry["wav"] = "external/" + wavFile;
                entry["parameterValue"] = *normalized;
                entry["parameterText"] = resultingText;
                entry["reportedLatencySamples"] = stats->reportedLatencySamples;
                entry["processMs"] = stats->processMs;
                entry["audioMs"] = stats->audioMs;
                entry["realtimeFactor"] = stats->realtimeFactor;
                entry["avgBlockUs"] = stats->avgBlockUs;
                entry["maxBlockUs"] = stats->maxBlockUs;
                manifest["entries"].push_back(std::move(entry));
                ++rendered;

                std::cout << "param=\"" << resultingText << "\" (norm " << *normalized
                          << "), latency " << stats->reportedLatencySamples
                          << " rep samples, rtf " << stats->realtimeFactor << '\n';
            }
        }

        if (manifest["entries"].empty())
        {
            std::cerr << "WARN: no renders produced for " << job.pluginId << '\n';
            continue;
        }

        const fs::path manifestPath = snapshotDir / ("external-renders-" + SanitizeForFilename(job.pluginId) + ".json");
        std::ofstream manifestOut(manifestPath);
        if (!manifestOut)
        {
            std::cerr << "ERROR: cannot write " << manifestPath << '\n';
            ++failures;
            continue;
        }
        manifestOut << manifest.dump(2) << '\n';
        manifestsWritten.push_back(manifestPath);
        std::cout << "  wrote manifest " << manifestPath << '\n';
    }

    if (listParams)
        return 0;

    if (rendered == 0)
    {
        std::cerr << "\nERROR: no external renders were produced\n";
        return 1;
    }

    std::cout << "\nRendered " << rendered << " external pass(es).\nNext steps:\n";
    for (const auto& manifestPath : manifestsWritten)
    {
        std::cout << "  python tools/transpose-benchmark/build_external_passes.py "
                  << snapshotDir.generic_string() << ' ' << manifestPath.generic_string() << '\n';
    }
    std::cout << "  python tools/transpose-benchmark/generate_report.py "
              << snapshotDir.parent_path().generic_string() << '\n';

    return failures == 0 ? 0 : 1;
}

/**
 * PluginControllerEffectAnalysis.cpp - Offline questions about how an effect sounds.
 *
 * The response curve an effect's panel draws, exporting an effect as an IR, matching the
 * Simple Cabinet to an IR from the library, and lining up the IR Cabinet's two IRs (read
 * from their files, not from the playing cab) and drawing their sum. Every answer comes from a fresh instance
 * built from the parameters the UI sends (dsp/EffectAnalysis.h), never from a running
 * graph, so none of this takes the DSP lock or depends on the node being in the active
 * preset.
 */

#include "PluginController.h"

#include "controller/internal/ControllerUtils.h"
#include "dsp/EffectAnalysis.h"
#include "dsp/EffectGuids.h"
#include "dsp/EffectRegistry.h"
#include "dsp/FiniteCheck.h"
#include "dsp/IRWavLoader.h"
#include "dsp/ImpulseResampler.h"
#include "dsp/IrAlignment.h"
#include "dsp/IrPairResponse.h"
#include "dsp/IrSlotAlignment.h"
#include "dsp/effects/SimpleCabMatch.h"
#include "presets/PresetTypesJson.h"
#include "resources/ResourceLibrary.h"
#include "util/Base64.h"
#include "util/PathEncoding.h"
#include "util/PathSanitizer.h"
#include "util/Wav.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <system_error>

using namespace guitarfx::controller_detail;

namespace guitarfx
{
namespace
{
constexpr int kDefaultResponsePoints = 160;
constexpr int kMaxResponsePoints = 1024;
/// 170 ms at 48 kHz: a cabinet has long gone quiet, and a dry mix's impulse is at the start.
constexpr int kExportedImpulseLength = 8192;

/// The numeric entries of a JSON object; anything else is skipped.
std::map<std::string, double> ParseParams(const nlohmann::json& payload)
{
    std::map<std::string, double> params;
    const auto found = payload.find("params");

    if (found == payload.end() || !found->is_object())
    {
        return params;
    }

    for (const auto& [key, value] : found->items())
    {
        if (value.is_number())
        {
            params[key] = value.get<double>();
        }
    }

    return params;
}

std::string StringField(const nlohmann::json& payload, const char* key)
{
    const auto found = payload.find(key);
    return found != payload.end() && found->is_string() ? found->get<std::string>() : std::string{};
}

/// dB to 0.01, which is finer than any curve can show and keeps the reply small.
double RoundCentibel(double db)
{
    return std::round(db * 100.0) / 100.0;
}

/// The rate the Alignment section's analysis and waveforms are at. Offsets go back in ms, so the
/// host's rate does not matter.
constexpr double kAlignmentRate = 48000.0;
/// How much of each IR the section draws and works its combined response from: the response
/// window SmoothedMagnitudeDb uses, plus room for B moved by the largest offset.
constexpr double kAlignmentWaveformMs = 60.0;

/// One IR as the Alignment section sees it: mono at kAlignmentRate, and the gain the cab's
/// Normalize applies to it, worked out over its channels the way the cab works it out.
struct AlignmentImpulse
{
    std::vector<float> mono;
    double normalizeGain = 1.0;
};

std::optional<AlignmentImpulse> LoadAlignmentImpulse(const std::filesystem::path& path)
{
    IRWavData data;

    if (!irwav::LoadAudioFile(path, data) || data.samples.empty())
    {
        return std::nullopt;
    }

    std::vector<float> left;
    std::vector<float> right;

    if (data.channels >= 2)
    {
        irwav::SplitToStereo(data, left, right);
        right.resize(left.size());
    }
    else
    {
        left = data.samples;
    }

    const auto toAnalysisRate = [&data](std::vector<float>& channel) {
        if (!channel.empty() && std::abs(data.sampleRate - kAlignmentRate) > 1.0)
        {
            ResampleImpulseForConvolution(channel, data.sampleRate, kAlignmentRate);
        }
    };
    toAnalysisRate(left);
    toAnalysisRate(right);

    AlignmentImpulse impulse;
    double energy = 0.0;

    for (const float sample : left)
    {
        energy += static_cast<double>(sample) * sample;
    }

    if (right.empty())
    {
        impulse.mono = std::move(left);
    }
    else
    {
        for (const float sample : right)
        {
            energy += static_cast<double>(sample) * sample;
        }

        energy *= 0.5;
        impulse.mono.resize(std::min(left.size(), right.size()));

        for (std::size_t n = 0; n < impulse.mono.size(); ++n)
        {
            impulse.mono[n] = 0.5f * (left[n] + right[n]);
        }
    }

    impulse.normalizeGain = energy > 1e-12 ? 1.0 / std::sqrt(energy) : 1.0;
    return impulse;
}

/// The IRs the Alignment section's response was last drawn from, so a drag of the offset or a
/// knob re-reads no file: each IR's samples and spectra, and each pair's likeness for Auto Gain
/// Comp. Keyed by path, size and modification time, so an IR rewritten in place is read again.
/// Shared by every instance in the process; the lock keeps two editors' requests apart.
struct CachedIr
{
    std::string key;
    AlignmentImpulse impulse;
    std::map<int, ir_pair_response::BandSpectrum> spectraByPoints;
};

struct AlignmentResponseCache
{
    std::mutex mutex;
    std::list<std::shared_ptr<CachedIr>> irs; // the most recently used first
    std::list<std::pair<std::string, IRCabEffect::BlendCorrelation>> pairs;
};

constexpr std::size_t kCachedIrs = 8;
constexpr std::size_t kCachedPairs = 4;

AlignmentResponseCache& ResponseCache()
{
    static AlignmentResponseCache cache;
    return cache;
}

std::string FileKey(const std::filesystem::path& path)
{
    std::error_code error;
    const auto size = std::filesystem::file_size(path, error);
    const auto written = std::filesystem::last_write_time(path, error);
    return util::PathToUtf8(path) + "|" + std::to_string(size) + "|" +
           std::to_string(static_cast<long long>(written.time_since_epoch().count()));
}

/// The IR at `path`, read once while it stays in the cache. Call with the cache locked.
std::shared_ptr<CachedIr> CachedImpulse(AlignmentResponseCache& cache, const std::filesystem::path& path)
{
    const std::string key = FileKey(path);
    const auto found = std::find_if(cache.irs.begin(), cache.irs.end(),
                                    [&key](const std::shared_ptr<CachedIr>& ir) { return ir->key == key; });

    if (found != cache.irs.end())
    {
        cache.irs.splice(cache.irs.begin(), cache.irs, found);
        return cache.irs.front();
    }

    auto impulse = LoadAlignmentImpulse(path);

    if (!impulse)
    {
        return nullptr;
    }

    auto entry = std::make_shared<CachedIr>();
    entry->key = key;
    entry->impulse = std::move(*impulse);
    cache.irs.push_front(entry);

    if (cache.irs.size() > kCachedIrs)
    {
        cache.irs.pop_back();
    }

    return entry;
}

const ir_pair_response::BandSpectrum& SpectrumOf(CachedIr& ir, int points, const std::vector<double>& frequencies)
{
    auto found = ir.spectraByPoints.find(points);

    if (found == ir.spectraByPoints.end())
    {
        found =
            ir.spectraByPoints
                .emplace(points, ir_pair_response::MeasureBandSpectrum(ir.impulse.mono, kAlignmentRate, frequencies))
                .first;
    }

    return found->second;
}

/// How alike A and B are, as Auto Gain Comp measures it, before Normalize. Call with the cache
/// locked.
IRCabEffect::BlendCorrelation PairCorrelation(AlignmentResponseCache& cache, const CachedIr& a, const CachedIr& b)
{
    const std::string key = a.key + "::" + b.key;

    for (auto it = cache.pairs.begin(); it != cache.pairs.end(); ++it)
    {
        if (it->first == key)
        {
            cache.pairs.splice(cache.pairs.begin(), cache.pairs, it);
            return cache.pairs.front().second;
        }
    }

    cache.pairs.emplace_front(key,
                              IRCabEffect::MeasureBlendCorrelation(a.impulse.mono, b.impulse.mono, kAlignmentRate));

    if (cache.pairs.size() > kCachedPairs)
    {
        cache.pairs.pop_back();
    }

    return cache.pairs.front().second;
}

/// The start of an IR as little-endian float32, base64'd: a fraction of the size of a JSON array.
std::string EncodeWaveform(const std::vector<float>& samples)
{
    const std::size_t count =
        std::min(samples.size(), static_cast<std::size_t>(kAlignmentWaveformMs * kAlignmentRate / 1000.0));
    std::vector<std::uint8_t> bytes(count * sizeof(float));

    if (count > 0)
    {
        std::memcpy(bytes.data(), samples.data(), bytes.size());
    }

    return util::EncodeBase64(bytes);
}
} // namespace

void PluginController::HandleGetEffectResponseRequest(const nlohmann::json& payload)
{
    const std::string requestedType = StringField(payload, "effectType");
    const auto points = payload.find("points");
    const int pointCount = points != payload.end() && points->is_number_integer()
                               ? std::clamp(points->get<int>(), 2, kMaxResponsePoints)
                               : kDefaultResponsePoints;
    const std::vector<double> frequencies = effect_analysis::LogFrequencies(pointCount);
    const auto magnitudes = effect_analysis::MagnitudeResponseDb(EffectRegistry::Instance().Resolve(requestedType),
                                                                 ParseParams(payload), frequencies);

    nlohmann::json reply;
    reply["type"] = "effectResponse";
    reply["requestId"] = StringField(payload, "requestId");
    reply["effectType"] = requestedType;
    reply["supported"] = magnitudes.has_value();

    if (magnitudes)
    {
        nlohmann::json rounded = nlohmann::json::array();

        for (const double db : *magnitudes)
        {
            rounded.push_back(RoundCentibel(db));
        }

        reply["frequencies"] = frequencies;
        reply["magnitudesDb"] = std::move(rounded);

        // Each band's own curve, for a plot that shades them (the EQs).
        if (const auto bands = payload.find("bands");
            bands != payload.end() && bands->is_boolean() && bands->get<bool>())
        {
            nlohmann::json bandCurves = nlohmann::json::array();

            for (const auto& curve : effect_analysis::BandResponsesDb(EffectRegistry::Instance().Resolve(requestedType),
                                                                      ParseParams(payload), frequencies))
            {
                nlohmann::json values = nlohmann::json::array();

                for (const double db : curve)
                {
                    values.push_back(RoundCentibel(db));
                }

                bandCurves.push_back(std::move(values));
            }

            reply["bandsDb"] = std::move(bandCurves);
        }

        // What the curve leaves out, for an effect that says (EffectProcessor::DescribeSettings).
        if (const auto effect = effect_analysis::CreateConfigured(EffectRegistry::Instance().Resolve(requestedType),
                                                                  ParseParams(payload)))
        {
            nlohmann::json summary = nlohmann::json::object();

            for (const auto& [name, value] : effect->DescribeSettings())
            {
                if (IsFinite(value))
                {
                    summary[name] = value;
                }
            }

            if (!summary.empty())
            {
                reply["summary"] = std::move(summary);
            }
        }
    }

    SendMessageToUI(reply.dump());
}

void PluginController::HandleExportEffectAsIrRequest(const nlohmann::json& payload)
{
    const std::string requestId = StringField(payload, "requestId");
    const std::string effectType = EffectRegistry::Instance().Resolve(StringField(payload, "effectType"));
    auto impulse = effect_analysis::RenderImpulse(effectType, ParseParams(payload), kExportedImpulseLength);

    if (!impulse)
    {
        AnnounceLocalResourceSaveFailure("This effect has no fixed response to capture as an IR", requestId);
        return;
    }

    // 16-bit PCM clips above full scale. A cabinet's impulse peaks well below it, but a hot
    // Output setting could push it over, so only then is it scaled down to fit.
    float peak = 0.0f;

    for (const auto& channel : impulse->channels)
    {
        for (const float sample : channel)
        {
            peak = std::max(peak, std::abs(sample));
        }
    }

    constexpr float kMaxPeak = 0.999f;

    if (peak > kMaxPeak)
    {
        for (auto& channel : impulse->channels)
        {
            for (float& sample : channel)
            {
                sample *= kMaxPeak / peak;
            }
        }
    }

    std::vector<const std::vector<float>*> channels;

    for (const auto& channel : impulse->channels)
    {
        channels.push_back(&channel);
    }

    const auto bytes = util::Encode16BitWav(channels, static_cast<int>(std::lround(effect_analysis::kSampleRate)));
    const auto typeInfo = EffectRegistry::Instance().GetTypeInfo(effectType);
    std::string name = StringField(payload, "name");

    if (name.empty())
    {
        name = (typeInfo ? typeInfo->displayName : std::string("Effect")) + " IR";
    }

    // The library save reuses an entry, and overwrites its file, when the file name is taken.
    // Re-exporting under a used name would then change the sound of every preset using the
    // earlier export, so each export gets a name of its own.
    const auto existing = mResourceLibrary.GetAllResources();
    const auto nameTaken = [&existing](const std::string& candidate) {
        const std::string fileName = util::SanitizeFilename(candidate + ".wav");
        return std::any_of(existing.begin(), existing.end(), [&](const LibraryResource& resource) {
            return resource.type == "ir" &&
                   (resource.name == candidate || util::PathToUtf8(resource.filePath.filename()) == fileName);
        });
    };
    const std::string baseName = name;

    for (int suffix = 2; nameTaken(name); ++suffix)
    {
        name = baseName + " " + std::to_string(suffix);
    }

    nlohmann::json resource;
    resource["resourceType"] = "ir";
    resource["data"] = util::EncodeBase64(bytes);
    resource["fileName"] = name + ".wav";
    resource["name"] = name;
    resource["category"] = "cab";
    resource["description"] = "Exported from " + (typeInfo ? typeInfo->displayName : effectType);
    resource["tags"] = nlohmann::json::array({"exported"});

    std::string error;
    const auto saved = SaveLocalLibraryResource(resource, error, true);

    if (!saved)
    {
        AnnounceLocalResourceSaveFailure(error, requestId);
        return;
    }

    AppendSessionLog("Exported " + effectType + " as IR " + saved->id);
    AnnounceSavedLocalResource(*saved, requestId);
}

void PluginController::HandleMatchSimpleCabToIrRequest(const nlohmann::json& payload)
{
    nlohmann::json reply;
    reply["type"] = "simpleCabIrMatch";
    reply["requestId"] = StringField(payload, "requestId");

    const auto fail = [this, &reply](const std::string& message) {
        reply["error"] = message;
        SendMessageToUI(reply.dump());
    };

    ResourceRef ref;
    ref.resourceType = "ir";
    ref.resourceId = StringField(payload, "resourceId");

    if (ref.resourceId.empty())
    {
        fail("No IR chosen");
        return;
    }

    reply["resourceId"] = ref.resourceId;
    const auto path = ResolveResourceRef(ref);
    std::error_code existsError;

    // A library entry can outlive its file (a moved drive, a deleted folder): say so, rather
    // than blaming the file's contents.
    if (!path || path->empty() || !std::filesystem::exists(*path, existsError))
    {
        fail("The IR's file is missing");
        return;
    }

    IRWavData ir;

    if (!irwav::LoadAudioFile(*path, ir))
    {
        fail("The IR could not be read");
        return;
    }

    std::vector<float> mono;
    irwav::DownmixToMono(ir, mono);
    const simple_cab::MatchResult match = simple_cab::MatchImpulseResponse(mono, ir.sampleRate);

    if (match.rmsErrorDb < 0.0)
    {
        fail("The IR is empty");
        return;
    }

    nlohmann::json params = nlohmann::json::object();

    for (int i = 0; i < simple_cab::kParamCount; ++i)
    {
        if (simple_cab::IsSetByMatch(i))
        {
            params[simple_cab::kParams[i].id] = match.values[i];
        }
    }

    reply["params"] = std::move(params);
    reply["rmsErrorDb"] = RoundCentibel(match.rmsErrorDb);
    SendMessageToUI(reply.dump());
}

void PluginController::HandleAnalyzeIrAlignmentRequest(const nlohmann::json& payload)
{
    nlohmann::json reply;
    reply["type"] = "irAlignment";
    reply["requestId"] = StringField(payload, "requestId");

    const auto fail = [this, &reply](const std::string& message) {
        reply["error"] = message;
        SendMessageToUI(reply.dump());
    };

    // The refs come as the node holds them, so a browsed file works as well as a library entry.
    const auto load = [this](const nlohmann::json& refJson) -> std::optional<AlignmentImpulse> {
        if (!refJson.is_object())
        {
            return std::nullopt;
        }

        ResourceRef ref = DeserializeResourceRef(refJson);

        if (ref.resourceType.empty())
        {
            ref.resourceType = "ir";
        }

        const auto path = ResolveResourceRef(ref);
        std::error_code existsError;

        if (!path || path->empty() || !std::filesystem::exists(*path, existsError))
        {
            return std::nullopt;
        }

        return LoadAlignmentImpulse(*path);
    };

    // A cab with one IR sends only that slot: there is nothing to align, but the UI still draws
    // the one IR's response.
    const bool wantA = payload.contains("irA") && payload["irA"].is_object();
    const bool wantB = payload.contains("irB") && payload["irB"].is_object();

    if (!wantA && !wantB)
    {
        fail("No IR to analyse");
        return;
    }

    std::optional<AlignmentImpulse> irA;
    std::optional<AlignmentImpulse> irB;

    if (wantA)
    {
        irA = load(payload["irA"]);
    }

    if (wantB)
    {
        irB = load(payload["irB"]);
    }

    if ((wantA && !irA) || (wantB && !irB))
    {
        fail(wantA && !irA ? "IR A could not be read" : "IR B could not be read");
        return;
    }

    reply["sampleRate"] = kAlignmentRate;

    if (!irA || !irB)
    {
        const AlignmentImpulse& only = irA ? *irA : *irB;
        reply[irA ? "waveformA" : "waveformB"] = EncodeWaveform(only.mono);
        reply[irA ? "normalizeGainA" : "normalizeGainB"] = only.normalizeGain;
        SendMessageToUI(reply.dump());
        return;
    }

    const auto analysis = ir_alignment::Analyse(irA->mono, irB->mono, kAlignmentRate, IrSlotAlignment::kMaxOffsetMs);

    if (!analysis.valid)
    {
        fail("One of the IRs is silent");
        return;
    }

    nlohmann::json matchByOffset = nlohmann::json::array();

    for (const double match : analysis.matchByOffset)
    {
        matchByOffset.push_back(std::round(match * 1000.0) / 1000.0);
    }

    reply["waveformA"] = EncodeWaveform(irA->mono);
    reply["waveformB"] = EncodeWaveform(irB->mono);
    reply["normalizeGainA"] = irA->normalizeGain;
    reply["normalizeGainB"] = irB->normalizeGain;
    reply["alignedOffsetMs"] = analysis.alignedOffsetMs;
    reply["invertB"] = analysis.invertB;
    reply["match"] = analysis.match;
    reply["onsetAMs"] = analysis.onsetAMs;
    reply["onsetBMs"] = analysis.onsetBMs;
    reply["maxOffsetSamples"] = analysis.maxOffsetSamples;
    reply["matchByOffset"] = std::move(matchByOffset);
    SendMessageToUI(reply.dump());
}

void PluginController::HandleGetIrAlignmentResponseRequest(const nlohmann::json& payload)
{
    nlohmann::json reply;
    reply["type"] = "irAlignmentResponse";
    reply["requestId"] = StringField(payload, "requestId");

    const auto fail = [this, &reply](const std::string& message) {
        reply["error"] = message;
        SendMessageToUI(reply.dump());
    };

    // The refs come as the node holds them, so a browsed file works as well as a library entry.
    const auto resolve = [this](const nlohmann::json& refJson) -> std::optional<std::filesystem::path> {
        ResourceRef ref = DeserializeResourceRef(refJson);

        if (ref.resourceType.empty())
        {
            ref.resourceType = "ir";
        }

        const auto path = ResolveResourceRef(ref);
        std::error_code existsError;

        if (!path || path->empty() || !std::filesystem::exists(*path, existsError))
        {
            return std::nullopt;
        }

        return path;
    };

    const bool wantA = payload.contains("irA") && payload["irA"].is_object();
    const bool wantB = payload.contains("irB") && payload["irB"].is_object();

    if (!wantA && !wantB)
    {
        fail("No IR to analyse");
        return;
    }

    const auto pointsField = payload.find("points");
    const int points = pointsField != payload.end() && pointsField->is_number_integer()
                           ? std::clamp(pointsField->get<int>(), 2, kMaxResponsePoints)
                           : kDefaultResponsePoints;
    const std::vector<double> frequencies = effect_analysis::LogFrequencies(points);
    const auto params = ParseParams(payload);

    // The cab as the node has it set: its slot gains, Auto Gain Comp and filters (DescribeSlotSum).
    const auto effect = effect_analysis::CreateConfigured(EffectGuids::kCabIr, params);
    const auto* cab = dynamic_cast<const IRCabEffect*>(effect.get());

    if (!cab)
    {
        fail("The IR Cabinet is not available");
        return;
    }

    double offsetMs = 0.0;

    if (const auto offset = payload.find("offsetMs"); offset != payload.end() && offset->is_number())
    {
        offsetMs = offset->get<double>();
    }
    else if (const auto param = params.find("slotBOffset"); param != params.end())
    {
        offsetMs = param->second;
    }

    offsetMs =
        IsFinite(offsetMs) ? std::clamp(offsetMs, -IrSlotAlignment::kMaxOffsetMs, IrSlotAlignment::kMaxOffsetMs) : 0.0;

    auto& cache = ResponseCache();
    const std::lock_guard lock(cache.mutex);
    std::shared_ptr<CachedIr> irA;
    std::shared_ptr<CachedIr> irB;

    if (wantA)
    {
        const auto path = resolve(payload["irA"]);
        irA = path ? CachedImpulse(cache, *path) : nullptr;

        if (!irA)
        {
            fail("IR A could not be read");
            return;
        }
    }

    if (wantB)
    {
        const auto path = resolve(payload["irB"]);
        irB = path ? CachedImpulse(cache, *path) : nullptr;

        if (!irB)
        {
            fail("IR B could not be read");
            return;
        }
    }

    const bool both = irA && irB;
    IRCabEffect::BlendCorrelation correlation;

    if (both)
    {
        // Measured before Normalize; with it on, the energies and the cross term take its gains.
        correlation = PairCorrelation(cache, *irA, *irB);

        if (cab->GetParam("normalizeIR") >= 0.5)
        {
            const double gainA = irA->impulse.normalizeGain;
            const double gainB = irB->impulse.normalizeGain;
            correlation.energyA *= gainA * gainA;
            correlation.energyB *= gainB * gainB;

            for (double& value : correlation.cross)
            {
                value *= gainA * gainB;
            }
        }
    }

    const auto sum =
        cab->DescribeSlotSum(irA != nullptr, irB != nullptr, correlation, offsetMs * kAlignmentRate / 1000.0);
    const auto* spectrumA = irA ? &SpectrumOf(*irA, points, frequencies) : nullptr;
    const auto* spectrumB = irB ? &SpectrumOf(*irB, points, frequencies) : nullptr;
    const auto curves = ir_pair_response::Combine(spectrumA, spectrumB, sum, irA ? irA->impulse.normalizeGain : 1.0,
                                                  irB ? irB->impulse.normalizeGain : 1.0, offsetMs, kAlignmentRate);

    const auto rounded = [](const std::vector<double>& values) {
        nlohmann::json out = nlohmann::json::array();

        for (const double db : values)
        {
            out.push_back(RoundCentibel(db));
        }

        return out;
    };

    reply["slots"] = both ? "both" : (irA ? "a" : "b");
    reply["offsetMs"] = offsetMs;
    reply["frequencies"] = frequencies;
    reply["summedDb"] = rounded(curves.summedDb);

    // One IR alone cannot interfere with anything, so there is nothing to compare it with.
    if (both)
    {
        reply["unrelatedDb"] = rounded(curves.unrelatedDb);
    }

    SendMessageToUI(reply.dump());
}
} // namespace guitarfx

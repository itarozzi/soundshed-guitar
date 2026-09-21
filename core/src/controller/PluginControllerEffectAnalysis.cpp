/**
 * PluginControllerEffectAnalysis.cpp - Offline questions about how an effect sounds.
 *
 * The response curve an effect's panel draws, exporting an effect as an IR, and matching
 * the Simple Cabinet to an IR from the library. Every answer comes from a fresh instance
 * built from the parameters the UI sends (dsp/EffectAnalysis.h), never from a running
 * graph, so none of this takes the DSP lock or depends on the node being in the active
 * preset.
 */

#include "PluginController.h"

#include "controller/internal/ControllerUtils.h"
#include "dsp/EffectAnalysis.h"
#include "dsp/EffectGuids.h"
#include "dsp/EffectRegistry.h"
#include "dsp/IRWavLoader.h"
#include "dsp/effects/SimpleCabMatch.h"
#include "resources/ResourceLibrary.h"
#include "util/Base64.h"
#include "util/PathSanitizer.h"
#include "util/Wav.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <map>
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
                   (resource.name == candidate || resource.filePath.filename().string() == fileName);
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
} // namespace guitarfx

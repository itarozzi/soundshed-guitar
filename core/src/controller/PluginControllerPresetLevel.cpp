/**
 * PluginControllerPresetLevel.cpp - measurePresetLevel, the message-thread half: from a request
 * to a graph built, prepared and handed to the PresetLevelMeter, which renders and measures it
 * on its own thread (controller/PresetLevelMeter.h has the whole design).
 *
 * The preset is prepared for the engine exactly as a load prepares it (scene, NAM params,
 * blends, boundary nodes) and built by MultiPresetMixer::BuildDetachedExecutor as a slot would
 * be, so what is measured is what would play. Nothing here touches the active preset, the
 * running slots or the DSP lock.
 */

#include "PluginController.h"

#include "controller/PresetLevelMeter.h"
#include "controller/internal/HostedPluginSupport.h"
#include "controller/internal/NamResourceMetadata.h"
#include "controller/internal/SettingsKeys.h"
#include "dsp/SignalGraphExecutor.h"
#include "presets/PresetStorage.h"
#include "resources/ResourceLibrary.h"
#include "util/PathEncoding.h"

#include <algorithm>
#include <filesystem>
#include <system_error>

using namespace guitarfx::controller_detail;

namespace guitarfx
{
namespace
{
/// The demo DI the content pipeline balances presets on (DI_Guitar_L.mp3 in ui/demo/clips.json).
constexpr const char* kDefaultLevelClipId = "di-riff-03";
/// Offline blocks: large enough to keep per-block overhead down, small enough for any effect.
constexpr int kLevelRenderBlockSize = 512;
constexpr double kFallbackSampleRate = 48000.0;
constexpr double kMaxMeasureSeconds = 600.0;

std::string StringField(const nlohmann::json& payload, const char* key)
{
    const auto it = payload.find(key);
    return it != payload.end() && it->is_string() ? it->get<std::string>() : std::string{};
}

/// Every resource an enabled node names that resolves to no file on disk.
nlohmann::json FindMissingResources(const Preset& preset, const ResourceLibrary& library)
{
    nlohmann::json missing = nlohmann::json::array();

    for (const auto& node : preset.graph.nodes)
    {
        if (!node.enabled)
        {
            continue;
        }

        for (const auto& ref : node.resources)
        {
            if (!ref.IsValid())
            {
                continue;
            }

            auto path = library.ResolveResource(ref);

            if (!path && ref.IsFilePath())
            {
                path = ref.filePath;
            }

            std::error_code ec;

            if (path && std::filesystem::is_regular_file(*path, ec))
            {
                continue;
            }

            const auto entry = library.LookupResource(ref.resourceType, ref.resourceId);
            missing.push_back({{"nodeId", node.id},
                               {"nodeType", node.type},
                               {"resourceType", ref.resourceType},
                               {"resourceId", ref.resourceId},
                               {"name", entry ? entry->name : std::string{}},
                               {"filePath", util::PathToUtf8(ref.filePath)}});
        }
    }

    return missing;
}
} // namespace

std::optional<PresetLevelJob> PluginController::BuildPresetLevelJob(const nlohmann::json& payload,
                                                                    nlohmann::json& failure, bool forPlayer)
{
    const auto fail = [&failure](const std::string& message) -> std::optional<PresetLevelJob> {
        failure["message"] = message;
        return std::nullopt;
    };

    // The preset: sent whole, or by id as a load reads it (an unsaved active preset included).
    std::optional<Preset> preset;
    std::string presetId = StringField(payload, "presetId");

    if (const auto it = payload.find("preset"); it != payload.end() && it->is_object())
    {
        preset = PresetStorage::DeserializeFromJson(it->dump());

        if (!preset)
        {
            return fail("The preset could not be read");
        }

        if (presetId.empty())
        {
            presetId = preset->id;
        }
    }
    else if (!presetId.empty())
    {
        preset = LoadPresetById(presetId);

        if (!preset && mActivePreset && (mActivePreset->id == presetId || mActivePresetId == presetId))
        {
            preset = *mActivePreset;
        }

        if (!preset)
        {
            failure["presetId"] = presetId;
            return fail("Preset '" + presetId + "' not found");
        }
    }
    else
    {
        return fail("Send a preset or a presetId to measure");
    }

    failure["presetId"] = presetId;

    // A scene asked for by name must exist; without one, the scene a load would start on.
    const std::string sceneId = StringField(payload, "sceneId");
    NormalizePresetScenes(*preset);

    if (!sceneId.empty() && FindPresetScene(*preset, sceneId) == nullptr)
    {
        return fail("Scene '" + sceneId + "' not found in preset '" + presetId + "'");
    }

    const std::string resolvedSceneId = PreparePresetForEngine(*preset, sceneId, GlobalSignalChainConfig{});

    // A hosted plugin is loaded, run and destroyed through the message thread and its own editor;
    // a second copy rendered off it is not something to try on a user's plugins.
    for (const auto& node : preset->graph.nodes)
    {
        if (IsHostedPluginNode(node))
        {
            return fail("Presets with third-party plugins cannot be measured offline");
        }
    }

    const auto missing = FindMissingResources(*preset, mResourceLibrary);

    if (!missing.empty())
    {
        failure["missingResources"] = missing;
        return fail(std::to_string(missing.size()) + (missing.size() == 1 ? " resource is" : " resources are") +
                    " missing, so the preset would not sound as it should");
    }

    // The clip, by id, from the bundled demo folder.
    std::string clipId = StringField(payload, "clipId");

    if (clipId.empty())
    {
        clipId = kDefaultLevelClipId;
    }

    const auto clipFile = FindDemoClipFile(clipId, nullptr);
    std::error_code ec;

    // A re-tone plays the player's own DI, not a clip.
    if (!forPlayer && (clipFile.empty() || !std::filesystem::is_regular_file(clipFile, ec)))
    {
        return fail("Unknown demo clip: " + clipId);
    }

    PresetLevelJob job;
    job.presetId = presetId;
    job.sceneId = resolvedSceneId;
    job.clipId = clipId;
    job.clipFile = clipFile;
    const double hostRate = mHost.GetSampleRate();
    job.sampleRate = hostRate >= 8000.0 && hostRate <= 384000.0 ? hostRate : kFallbackSampleRate;
    job.blockSize = kLevelRenderBlockSize;
    // A lone rig at the default mix (1.0) and pan (centre), as a load installs it.
    MultiPresetMixer::ComputePanGains(0.0, job.slotGainLeft, job.slotGainRight);

    if (const auto it = payload.find("maxSeconds"); it != payload.end() && it->is_number())
    {
        job.maxSeconds = std::clamp(it->get<double>(), 0.0, kMaxMeasureSeconds);
    }

    // Built and prepared here, off the DSP lock, as a preset load builds a slot.
    job.executor = mPresetMixer.BuildDetachedExecutor(*preset, forPlayer ? ChannelLayout::Stereo : ChannelLayout::Mono);

    if (!job.executor->IsValid())
    {
        return fail("The preset's signal graph is not valid");
    }

    if (job.executor->AnyNodeRequiresMainThreadLoad())
    {
        return fail("Presets with third-party plugins cannot be measured offline");
    }

    job.executor->Prepare(job.sampleRate, job.blockSize);

    // The reference interface level, not this player's (see PresetLevelMeter.h).
    for (const auto& node : preset->graph.nodes)
    {
        if (IsNamCalibratableEffectType(node.type))
        {
            const bool playerOff = forPlayer && !mNamInterfaceCalibrationLevelDbu.has_value();
            job.executor->SetNodeParam(node.id, "calibrationInputLevel",
                                       forPlayer && mNamInterfaceCalibrationLevelDbu ? *mNamInterfaceCalibrationLevelDbu
                                                                                     : kNamInterfaceCalibrationLevelDbuDefault);
            job.executor->SetNodeParam(node.id, "calibrationInputLevelEnabled", playerOff ? 0.0 : 1.0);
        }
    }

    job.executor->SetTempo(GetEffectiveTempoBpm());

    AppendSessionLog("measurePresetLevel: " + presetId + " scene=" + resolvedSceneId + " clip=" + clipId + " at " +
                     std::to_string(static_cast<int>(job.sampleRate)) + " Hz");
    return job;
}
} // namespace guitarfx

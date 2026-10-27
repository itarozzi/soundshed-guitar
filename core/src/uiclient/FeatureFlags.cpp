#include "uiclient/FeatureFlags.h"

#include <string>

namespace guitarfx::uiclient
{
const char* FeatureSettingKey(Feature feature)
{
    switch (feature)
    {
        case Feature::FactoryPresetPacks: return "features.factoryPresetPacks.enabled";
        case Feature::Tone3000: return "features.tone3000.enabled";
        case Feature::ResourceLibrary: return "features.resourceLibrary.enabled";
        case Feature::RiffLibrary: return "features.riffLibrary.enabled";
        case Feature::ToneSharing: return "features.toneSharing.enabled";
        case Feature::Jam: return "features.jam.enabled";
        case Feature::PracticeTool: return "features.practiceTool.enabled";
        case Feature::MultiRig: return "features.multiRig.enabled";
        case Feature::ToneBuilder: return "features.toneBuilder.enabled";
        case Feature::ExperimentalEffects: return "features.experimentalEffects.enabled";
        case Feature::PickupMatch: return "features.pickupMatch.enabled";
        case Feature::CustomEffects: return "features.customEffects.enabled";
        case Feature::CompositeEffects: return "features.compositeEffects.enabled";
        case Feature::BlendTools: return "features.blendTools.enabled";
        case Feature::EffectLayouts: return "features.effectLayouts.enabled";
        case Feature::ResourceCleanup: return "features.resourceCleanup.enabled";
        case Feature::FactoryPresetArchives: return "features.factoryPresetArchives.enabled";
        case Feature::DebugStateCapture: return "features.debugStateCapture.enabled";
        case Feature::EffectsPerformancePads: return "features.effectsPerformancePads.enabled";
        case Feature::Looper: return "features.looper.enabled";
    }
    return "";
}

bool FeatureDefault(Feature feature)
{
    switch (feature)
    {
        case Feature::FactoryPresetPacks:
        case Feature::Tone3000:
        case Feature::ResourceLibrary:
        case Feature::RiffLibrary:
        case Feature::ToneSharing:
        case Feature::Jam:
        case Feature::PracticeTool:
        case Feature::MultiRig:
            return true;
        default:
            return false;
    }
}

bool IsFeatureEnabled(const ClientState& state, Feature feature)
{
    const auto it = state.appSettings.find(FeatureSettingKey(feature));
    if (it == state.appSettings.end() || !it->is_boolean())
        return FeatureDefault(feature);
    return it->get<bool>();
}
} // namespace guitarfx::uiclient

#include "uiclient/FeatureFlags.h"

#include <string>

namespace guitarfx::uiclient
{
const std::vector<Feature>& AllFeatures()
{
    static const std::vector<Feature> all = {Feature::FactoryPresetPacks,
                                             Feature::Tone3000,
                                             Feature::ResourceLibrary,
                                             Feature::RiffLibrary,
                                             Feature::ToneSharing,
                                             Feature::ToneBuilder,
                                             Feature::Jam,
                                             Feature::PracticeTool,
                                             Feature::ExperimentalEffects,
                                             Feature::PickupMatch,
                                             Feature::CustomEffects,
                                             Feature::MultiRig,
                                             Feature::CompositeEffects,
                                             Feature::BlendTools,
                                             Feature::EffectLayouts,
                                             Feature::ResourceCleanup,
                                             Feature::FactoryPresetArchives,
                                             Feature::DebugStateCapture,
                                             Feature::EffectsPerformancePads,
                                             Feature::Looper};
    return all;
}

const char* FeatureLabel(Feature feature)
{
    switch (feature)
    {
    case Feature::FactoryPresetPacks:
        return "Factory Preset Packs";
    case Feature::Tone3000:
        return "Tone3000 Integration";
    case Feature::ResourceLibrary:
        return "Resource Library";
    case Feature::RiffLibrary:
        return "Riff Library";
    case Feature::ToneSharing:
        return "Tone Sharing";
    case Feature::ToneBuilder:
        return "AI Tone Builder";
    case Feature::Jam:
        return "Jam Panel";
    case Feature::PracticeTool:
        return "Practice Tool";
    case Feature::ExperimentalEffects:
        return "Experimental Effects";
    case Feature::PickupMatch:
        return "Pickup Match";
    case Feature::CustomEffects:
        return "Custom Effects";
    case Feature::MultiRig:
        return "Multi-Rig Mixer";
    case Feature::CompositeEffects:
        return "Composite Effects";
    case Feature::BlendTools:
        return "Blend Tools";
    case Feature::EffectLayouts:
        return "Effect Layout Editor";
    case Feature::ResourceCleanup:
        return "Resource Cleanup Tools";
    case Feature::FactoryPresetArchives:
        return "Factory Preset Archive Tools";
    case Feature::DebugStateCapture:
        return "Capture Debug State";
    case Feature::EffectsPerformancePads:
        return "Effects Performance Pads";
    case Feature::Looper:
        return "Looper";
    }

    return "";
}

const char* FeatureSettingKey(Feature feature)
{
    switch (feature)
    {
    case Feature::FactoryPresetPacks:
        return "features.factoryPresetPacks.enabled";
    case Feature::Tone3000:
        return "features.tone3000.enabled";
    case Feature::ResourceLibrary:
        return "features.resourceLibrary.enabled";
    case Feature::RiffLibrary:
        return "features.riffLibrary.enabled";
    case Feature::ToneSharing:
        return "features.toneSharing.enabled";
    case Feature::Jam:
        return "features.jam.enabled";
    case Feature::PracticeTool:
        return "features.practiceTool.enabled";
    case Feature::MultiRig:
        return "features.multiRig.enabled";
    case Feature::ToneBuilder:
        return "features.toneBuilder.enabled";
    case Feature::ExperimentalEffects:
        return "features.experimentalEffects.enabled";
    case Feature::PickupMatch:
        return "features.pickupMatch.enabled";
    case Feature::CustomEffects:
        return "features.customEffects.enabled";
    case Feature::CompositeEffects:
        return "features.compositeEffects.enabled";
    case Feature::BlendTools:
        return "features.blendTools.enabled";
    case Feature::EffectLayouts:
        return "features.effectLayouts.enabled";
    case Feature::ResourceCleanup:
        return "features.resourceCleanup.enabled";
    case Feature::FactoryPresetArchives:
        return "features.factoryPresetArchives.enabled";
    case Feature::DebugStateCapture:
        return "features.debugStateCapture.enabled";
    case Feature::EffectsPerformancePads:
        return "features.effectsPerformancePads.enabled";
    case Feature::Looper:
        return "features.looper.enabled";
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
    {
        return FeatureDefault(feature);
    }

    return it->get<bool>();
}
} // namespace guitarfx::uiclient

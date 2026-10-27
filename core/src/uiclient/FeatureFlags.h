#pragma once

/**
 * FeatureFlags.h - The features a user can switch off (or on) in Settings > Features.
 *
 * The web UI's table (core/ui/ts/featureFlags.ts): each is the app setting
 * "features.<key>.enabled", shared by every UI, and reads as its default until set. A native
 * UI hides what the web UI hides, so a feature switched off in one is off in both.
 */

#include "uiclient/ClientState.h"

namespace guitarfx::uiclient
{
enum class Feature
{
    // Core: on unless switched off.
    FactoryPresetPacks,
    Tone3000,
    ResourceLibrary,
    RiffLibrary,
    ToneSharing,
    Jam,
    PracticeTool,
    MultiRig,
    // Power: off unless switched on.
    ToneBuilder,
    ExperimentalEffects,
    PickupMatch,
    CustomEffects,
    CompositeEffects,
    BlendTools,
    EffectLayouts,
    ResourceCleanup,
    FactoryPresetArchives,
    DebugStateCapture,
    EffectsPerformancePads,
    Looper
};

/// The app setting's key, "features.<key>.enabled".
[[nodiscard]] const char* FeatureSettingKey(Feature feature);
[[nodiscard]] bool FeatureDefault(Feature feature);
[[nodiscard]] bool IsFeatureEnabled(const ClientState& state, Feature feature);
} // namespace guitarfx::uiclient

#pragma once

/**
 * FactoryPresetSupport.h — Building an effect's factory presets from its parameter table.
 *
 * A factory preset is copied into a node key by key, so one that left a control out would
 * leave the previous preset's value behind. Each preset therefore starts from every default
 * the effect declares and changes what makes it itself. The controls that are the player's,
 * not the sound's (an output level, a pedal's position, a gate threshold set to their
 * pickups), are left out of every preset, and keep whatever the player set.
 *
 * Each list opens with its default: the parameter defaults themselves, which also seed a
 * newly added node, so a fresh node and one set to the default preset agree.
 * EffectFactoryPresetTests holds every list built here to all of that.
 */

#include "dsp/EffectRegistry.h"

#include <algorithm>
#include <initializer_list>
#include <string>
#include <utility>
#include <vector>

namespace guitarfx::factory_presets
{
/// What one effect's presets are built from: its parameter table, and the controls no preset
/// sets. A preset may still set a left-out control by naming it, as a tempo-synced delay names
/// its division.
class Builder
{
  public:
    Builder(const std::vector<ParameterDef>& params, std::initializer_list<const char*> leaveOut)
        : mParams(params), mLeaveOut(leaveOut.begin(), leaveOut.end())
    {
    }

    /// The defaults themselves, marked as the preset new nodes start from.
    [[nodiscard]] EffectPresetDefinition Defaults(const char* id, const char* name) const
    {
        auto preset = Make(id, name, {});
        preset.isDefault = true;
        return preset;
    }

    /// Every default but the left-out controls, then `overrides`.
    [[nodiscard]] EffectPresetDefinition Make(const char* id, const char* name,
                                              std::initializer_list<std::pair<const char*, double>> overrides) const
    {
        EffectPresetDefinition preset;
        preset.id = id;
        preset.displayName = name;
        preset.isFactory = true;

        for (const auto& param : mParams)
        {
            if (!IsLeftOut(param.id))
            {
                preset.parameters[param.id] = param.defaultValue;
            }
        }

        for (const auto& [key, value] : overrides)
        {
            preset.parameters[key] = value;
        }

        // In the order the effect declares them, which is the order any that bound others
        // need to land in.
        for (const auto& param : mParams)
        {
            if (preset.parameters.contains(param.id))
            {
                preset.parameterOrder.push_back(param.id);
            }
        }

        return preset;
    }

  private:
    [[nodiscard]] bool IsLeftOut(const std::string& id) const
    {
        return std::find(mLeaveOut.begin(), mLeaveOut.end(), id) != mLeaveOut.end();
    }

    const std::vector<ParameterDef>& mParams;
    std::vector<std::string> mLeaveOut;
};
} // namespace guitarfx::factory_presets
